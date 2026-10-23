#include "Registry/ContentRegistry.h"
#include "Profiling/Profile.h"
#include "Registry/ContentRegistryClips.h"

#include "Input/InputTypes.h"
#include "Util/FormRef.h"
#include "Util/JsonField.h"
#include "Util/Math.h"
#include "Util/RegistryFiles.h"
#include "Util/Species.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace OSF::Registry
{
	using OSF::Util::ToLower;

	namespace
	{
		using json = nlohmann::json;
		using ContentRegistryClips::ClipInstalledCache;
		using ContentRegistryClips::ClipLibraryRegistration;
		using ContentRegistryClips::PendingImportProblem;
		using ContentRegistryClips::ProblemSink;
		using ContentRegistryClips::AccumulateFileStats;
		using ContentRegistryClips::AddRegisteredClipEntries;
		using ContentRegistryClips::ClipSpecInstalled;
		using ContentRegistryClips::SweepClipAvailability;

		constexpr std::size_t kMaxNodesPerFile = 4096;
		constexpr std::size_t kMaxRolesPerFile = 512;
		constexpr std::size_t kMaxPropsPerFile = 512;
		constexpr std::size_t kMaxClipLibraryEntriesPerFile = 65536;
		constexpr std::size_t kMaxStagesPerNode = 16384;
		constexpr std::size_t kMaxClipsPerNode = 65536;
		constexpr std::size_t kMaxNodesTotal = 131072;
		constexpr std::size_t kMaxStagesTotal = 262144;
		constexpr std::size_t kMaxClipsTotal = 1048576;
		constexpr std::size_t kMaxClipLibraryEntriesTotal = 262144;

		struct NodeLoadBudget
		{
			std::size_t nodes = 0;
			std::size_t stages = 0;
			std::size_t clips = 0;
			std::size_t clipLibraryEntries = 0;
		};

		// "Plugin.esm|0xLOCAL" -> T*; throws (rejecting the scene). LookupByID<T> is null for not-found OR wrong type.
		template <class T>
		T* ResolveFormRef(const std::string& a_ref, const std::string& a_sceneId, const std::string& a_role,
			const char* a_field, const char* a_expected)
		{
			const auto id = Util::ComposeFormID(a_ref);
			if (!id) {
				throw std::runtime_error("scene '" + a_sceneId + "': role '" + a_role + "': " + a_field +
					" '" + a_ref + "' is malformed or names an unloaded plugin (use \"Plugin.esm|0xLocalID\")");
			}
			T* form = RE::TESForm::LookupByID<T>(*id);
			if (!form) {
				throw std::runtime_error("scene '" + a_sceneId + "': role '" + a_role + "': " + a_field +
					" '" + a_ref + "' did not resolve to a " + a_expected);
			}
			return form;
		}

		// Object form of a file's top-level `roles`: exact, case-sensitive id -> template. File-local only.
		struct RoleTemplate
		{
			json      raw;     // `name` defaulted to the id; merge base for object overrides
			SceneRole parsed;  // copied directly by plain-string references
		};
		using RoleRegistry = std::map<std::string, RoleTemplate>;

		// A file's top-level `props`, kept as raw JSON: templates may be partial, so only the merged
		// action is validatable. File-local only.
		using PropRegistry = std::map<std::string, json>;

		enum class RoleNameKind : std::uint8_t
		{
			kAutomatic,  // template name, numbered on collision
			kExplicit,   // kept exactly; duplicates reject the scene
			kAnonymous   // never numbered
		};

		struct PendingRole
		{
			SceneRole     role;
			RoleNameKind  nameKind = RoleNameKind::kAnonymous;
			std::string   autoBase;  // kAutomatic: template name; "" = stays anonymous
		};

		// Authored heading is degrees; runtime uses radians.
		Animation::ParticipantPlacement ParseOffsetField(const json& a_json)
		{
			Animation::ParticipantPlacement p{};
			p.x = a_json.value("x", 0.0f);
			p.y = a_json.value("y", 0.0f);
			p.z = a_json.value("z", 0.0f);
			p.heading = static_cast<float>(a_json.value("heading", 0.0) * Util::kDegToRad);
			return p;
		}

		std::string NormalizeClipRoot(std::string_view a_root, const std::string& a_subject)
		{
			std::string root = ToLower(std::string(a_root));
			if (root.empty()) {
				return {};
			}
			if (root == "naf") {
				return root;
			}
			// Otherwise a Data-relative prefix for relative clip files.
			std::replace(root.begin(), root.end(), '\\', '/');
			while (!root.empty() && root.back() == '/') {
				root.pop_back();
			}
			if (root.find(':') != std::string::npos) {
				throw std::runtime_error(a_subject + ": clipRoot '" + std::string(a_root) + "' may not contain ':' (use 'NAF' or a Data-relative folder)");
			}
			return root;
		}

		bool IsLikelyGltfPath(std::string_view a_path)
		{
			const auto ext = ToLower(std::filesystem::path{ std::string(a_path) }.extension().string());
			return ext == ".glb" || ext == ".gltf";
		}

		void SplitAnimSuffix(StageClip& a_clip)
		{
			const auto pos = a_clip.file.rfind(':');
			if (pos == std::string::npos || pos + 1 >= a_clip.file.size()) {
				return;
			}
			const std::string pathPart = a_clip.file.substr(0, pos);
			if (!IsLikelyGltfPath(pathPart)) {
				return;
			}
			a_clip.animId = a_clip.file.substr(pos + 1);
			a_clip.file = pathPart;
		}

		void ApplyClipRoot(StageClip& a_clip, std::string_view a_clipRoot)
		{
			if (a_clipRoot.empty() || a_clip.file.empty()) {
				return;
			}
			const auto lower = ToLower(a_clip.file);
			if (lower.starts_with("naf:") || lower.starts_with("naf/") || lower.starts_with("naf\\") ||
				std::filesystem::path{ a_clip.file }.is_absolute()) {
				return;
			}
			if (a_clipRoot == "naf") {
				a_clip.file = "naf:" + a_clip.file;
			} else {
				a_clip.file = std::string(a_clipRoot) + "/" + a_clip.file;
			}
		}

		StageClip ParseStageClip(const json& a_clip, std::string_view a_clipRoot, const std::string& a_subject)
		{
			StageClip clip;
			if (a_clip.is_string()) {
				clip.file = a_clip.get<std::string>();
				SplitAnimSuffix(clip);
			} else if (a_clip.is_object()) {
				const auto fit = a_clip.find("file");
				if (fit == a_clip.end() || !fit->is_string()) {
					throw std::runtime_error(a_subject + ": clip 'file' must be a string");
				}
				clip.file = fit->get<std::string>();
				SplitAnimSuffix(clip);
				if (auto ait = a_clip.find("anim"); ait != a_clip.end()) {
					if (!ait->is_string()) {
						throw std::runtime_error(a_subject + ": clip 'anim' must be a string");
					}
					clip.animId = ait->get<std::string>();
				}
				if (auto oit = a_clip.find("offset"); oit != a_clip.end()) {
					if (!oit->is_object()) {
						throw std::runtime_error(a_subject + ": clip 'offset' must be an { x, y, z, heading } object");
					}
					clip.offset = ParseOffsetField(*oit);
				}
				if (auto mit = a_clip.find("mask"); mit != a_clip.end()) {
					if (!mit->is_string()) {
						throw std::runtime_error(a_subject + ": clip 'mask' must be a string");
					}
					const auto* mask = Animation::BoneMask::Find(mit->get<std::string>());
					if (!mask) {
						throw std::runtime_error(a_subject + ": clip has unknown 'mask' value '" +
							mit->get<std::string>() + "' (known: " + Animation::BoneMask::KnownList() + ")");
					}
					clip.mask = mask->id;
				}
				// Pre-measured duration spares the duration scan an archive walk.
				if (auto sit = a_clip.find("sec"); sit != a_clip.end()) {
					if (!sit->is_number() || sit->get<float>() <= 0.0f) {
						throw std::runtime_error(a_subject + ": clip 'sec' must be a positive number");
					}
					clip.sec = sit->get<float>();
				}
			} else {
				throw std::runtime_error(a_subject + ": a clip must be a file string or a { file, anim?, offset?, mask?, sec? } object");
			}
			if (clip.file.empty()) {
				throw std::runtime_error(a_subject + ": empty clip file");
			}
			ApplyClipRoot(clip, a_clipRoot);
			return clip;
		}

		std::string ParseCatalogFolder(const json& a_value, std::string_view a_subject)
		{
			if (!a_value.is_string()) {
				throw std::runtime_error(std::string(a_subject) + ": 'folder' must be a string");
			}
			const auto raw = a_value.get<std::string>();
			if (raw.empty() || raw.front() == '/' || raw.back() == '/' || raw.find('\\') != std::string::npos) {
				throw std::runtime_error(std::string(a_subject) +
					": 'folder' must be a non-empty relative path using '/' separators");
			}
			std::string normalized;
			std::size_t begin = 0;
			while (begin <= raw.size()) {
				const auto end = raw.find('/', begin);
				auto segment = raw.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
				const auto first = segment.find_first_not_of(" \t\r\n");
				const auto last = segment.find_last_not_of(" \t\r\n");
				if (first == std::string::npos) {
					throw std::runtime_error(std::string(a_subject) + ": 'folder' contains an empty segment");
				}
				segment = segment.substr(first, last - first + 1);
				if (segment == "." || segment == "..") {
					throw std::runtime_error(std::string(a_subject) + ": 'folder' may not contain '.' or '..' segments");
				}
				if (!normalized.empty()) {
					normalized += '/';
				}
				normalized += segment;
				if (end == std::string::npos) {
					break;
				}
				begin = end + 1;
			}
			return normalized;
		}

		std::vector<ClipLibraryRegistration> ParseClipLibrary(const json& a_json, const std::filesystem::path& a_file,
			std::string_view a_pack, std::string_view a_clipRoot, std::string_view a_folderDefault)
		{
			std::vector<ClipLibraryRegistration> out;
			const auto it = a_json.find("clipLibrary");
			if (it == a_json.end()) {
				return out;
			}
			const std::string fileName = a_file.filename().string();
			if (!it->is_array()) {
				throw std::runtime_error("'" + fileName + "': 'clipLibrary' must be an array");
			}
			if (it->size() > kMaxClipLibraryEntriesPerFile) {
				throw std::runtime_error("'" + fileName + "': 'clipLibrary' exceeds the " +
					std::to_string(kMaxClipLibraryEntriesPerFile) + "-entry limit");
			}
			out.reserve(it->size());
			for (std::size_t i = 0; i < it->size(); ++i) {
				const auto& entry = (*it)[i];
				const std::string subject = "'" + fileName + "': clipLibrary[" + std::to_string(i) + "]";

				ClipLibraryRegistration reg;
				reg.clip = ParseStageClip(entry, a_clipRoot, subject);
				reg.sourceFile = a_file;
				reg.pack = a_pack;
				reg.folder = a_folderDefault;

				if (entry.is_object()) {
					const Util::JsonView view{ entry, subject };
					view.ReadNonEmpty("name", reg.name);
					if (const auto* folder = view.Find("folder")) {
						reg.folder = ParseCatalogFolder(*folder, subject);
					}
					reg.tags = view.ReadList("tags",
						{ .expected = "an array of strings" },
						{ .whole = "every tag must be a non-empty string" }, /*nonEmptyItems*/ true);
				}
				out.push_back(std::move(reg));
			}
			return out;
		}

		EdgeWhen ParseWhen(const std::string& a_when, std::string& a_trigger)
		{
			if (a_when == "end") {
				return EdgeWhen::kEnd;
			}
			if (a_when == "loops") {
				return EdgeWhen::kLoops;
			}
			if (a_when == "timer") {
				return EdgeWhen::kTimer;
			}
			if (a_when == "advance") {
				return EdgeWhen::kAdvance;
			}
			if (a_when.rfind("trigger:", 0) == 0) {
				a_trigger = a_when.substr(8);
				return EdgeWhen::kTrigger;
			}
			throw std::runtime_error("unknown edge 'when' value '" + a_when + "' (cond:<expr> is deferred)");
		}

		SceneEdge ParseEdge(const json& a_edge, const std::string& a_nodeId)
		{
			SceneEdge e;
			e.to = a_edge.value("to", std::string{});
			if (e.to.empty()) {
				throw std::runtime_error("node '" + a_nodeId + "': an edge is missing 'to'");
			}
			e.when = ParseWhen(ToLower(a_edge.value("when", "advance")), e.trigger);
			e.id = a_edge.value("id", std::string{});
			e.label = a_edge.value("label", std::string{});
			e.labelKey = a_edge.value("labelKey", std::string{});
			e.isDefault = a_edge.value("default", false);
			Util::JsonView{ a_edge, "node '" + a_nodeId + "' edge" }.Read("covered", e.covered);
			e.priority = a_edge.value("priority", 0);
			// Advance edges feed GetSceneEdge* menus.
			if (e.when == EdgeWhen::kAdvance && (e.id.empty() || e.label.empty())) {
				throw std::runtime_error("node '" + a_nodeId + "': a branchable (advance) edge requires 'id' and 'label'");
			}
			return e;
		}

		std::optional<ActionKind> ParseBuiltinAction(std::string_view a_type)
		{
			static constexpr std::pair<std::string_view, ActionKind> kBuiltins[]{
				{ "osf.control.lock", ActionKind::kControlLock },
				{ "osf.control.release", ActionKind::kControlRelease },
				{ "osf.equipment.hide", ActionKind::kEquipmentHide },
				{ "osf.equipment.restore", ActionKind::kEquipmentRestore },
				{ "osf.equipment.equip", ActionKind::kEquipmentEquip },
				{ "osf.equipment.unequip", ActionKind::kEquipmentUnequip },
				{ "osf.weapon.sheathe", ActionKind::kWeaponSheathe },
				{ "osf.weapon.restore", ActionKind::kWeaponRestore },
				{ "osf.fade.out", ActionKind::kFadeOut },
				{ "osf.fade.in", ActionKind::kFadeIn },
				{ "osf.voice.play", ActionKind::kVoicePlay },
				{ "osf.prop.attach", ActionKind::kPropAttach },
				{ "osf.prop.destroy", ActionKind::kPropDestroy },
			};
			for (const auto& [name, kind] : kBuiltins) {
				if (name == a_type) {
					return kind;
				}
			}
			return std::nullopt;
		}

		SoundEmitter ParseSoundEmitter(const json& a_entry, SoundEmitter a_default,
			const std::string& a_nodeId, std::string_view a_label)
		{
			const auto it = a_entry.find("emitter");
			if (it == a_entry.end()) {
				return a_default;
			}
			if (!it->is_string()) {
				throw std::runtime_error("node '" + a_nodeId + "': " + std::string(a_label) +
					" 'emitter' must be a string");
			}
			const auto value = ToLower(it->get<std::string>());
			if (value == "listener") {
				return SoundEmitter::kListener;
			}
			if (value == "role") {
				return SoundEmitter::kRole;
			}
			throw std::runtime_error("node '" + a_nodeId + "': " + std::string(a_label) +
				" has unknown 'emitter' value '" + value + "' (expected 'listener' or 'role')");
		}

		// `at` (anchor or fraction in [0,1)) XOR `atFrame`; an `atFrame` entry is kFraction carrying `frame`.
		// Without a_atRequired a missing position defaults to enter.
		template <class Entry>
		void ParseTrackTiming(const json& a_entry, Entry& a_out, const std::string& a_nodeId,
			const std::string& a_subject, bool a_atRequired)
		{
			using Pos = decltype(a_out.pos);
			const auto atIt = a_entry.find("at");
			const auto frameIt = a_entry.find("atFrame");
			if (atIt != a_entry.end() && frameIt != a_entry.end()) {
				throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " sets both 'at' and 'atFrame' (pick one)");
			}
			if (a_atRequired && atIt == a_entry.end() && frameIt == a_entry.end()) {
				throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " is missing 'at'");
			}
			const auto repeat = ToLower(a_entry.value("repeat", "none"));
			if (repeat != "none" && repeat != "loop") {
				throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " has unknown repeat '" + repeat + "'");
			}
			a_out.everyLoop = (repeat == "loop");
			if (frameIt != a_entry.end()) {
				// Absolute position; a frame past the clip end can't be checked here and just never fires.
				if (!frameIt->is_number()) {
					throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " 'atFrame' must be a whole frame number");
				}
				const double frame = frameIt->get<double>();
				if (!std::isfinite(frame) || frame < 0.0 || frame != std::floor(frame)) {
					throw std::runtime_error("node '" + a_nodeId + "': " + a_subject +
						" 'atFrame' must be a whole frame number >= 0 (frame 0 is the clip start)");
				}
				a_out.pos = Pos::kFraction;
				a_out.frame = static_cast<float>(frame);
				return;
			}
			if (atIt == a_entry.end() || atIt->is_string()) {
				const std::string at = (atIt != a_entry.end()) ? ToLower(atIt->get<std::string>()) : "enter";
				if (at == "enter") {
					a_out.pos = Pos::kEnter;
				} else if (at == "exit") {
					a_out.pos = Pos::kExit;
				} else if (at == "end") {
					a_out.pos = Pos::kEnd;
				} else {
					throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " has unknown anchor 'at':'" + at + "'");
				}
				if (a_out.everyLoop) {
					throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " named anchor cannot use repeat:loop");
				}
			} else if (atIt->is_number()) {
				a_out.pos = Pos::kFraction;
				a_out.fraction = atIt->get<float>();
				if (a_out.fraction < 0.0f || a_out.fraction >= 1.0f) {
					throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " numeric 'at' must be in [0,1) (use 'end' for 1.0)");
				}
			} else {
				throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " 'at' must be a number or enter/exit/end (use 'atFrame' for a frame index)");
			}
		}

		std::array<float, 3> ParsePropVector(
			const json& a_owner, const char* a_field, const std::string& a_subject)
		{
			std::array<float, 3> result{};
			const auto it = a_owner.find(a_field);
			if (it == a_owner.end()) {
				return result;
			}
			if (!it->is_array() || it->size() != result.size()) {
				throw std::runtime_error(a_subject + " field '" +
					a_field + "' must be an array of exactly three finite numbers");
			}
			for (std::size_t i = 0; i < result.size(); ++i) {
				if (!(*it)[i].is_number()) {
					throw std::runtime_error(a_subject + " field '" +
						a_field + "' must be an array of exactly three finite numbers");
				}
				result[i] = (*it)[i].get<float>();
				if (!std::isfinite(result[i])) {
					throw std::runtime_error(a_subject + " field '" +
						a_field + "' must contain only finite numbers");
				}
			}
			return result;
		}

		Props::Source ParsePropSource(const json& a_source, const std::string& a_subject)
		{
			if (!a_source.is_object()) {
				throw std::runtime_error(a_subject + " field 'source' must be an object");
			}
			const bool hasForm = a_source.contains("form");
			const bool hasEquipped = a_source.contains("equippedArmor");
			if (hasForm == hasEquipped) {
				throw std::runtime_error(a_subject +
					" source must contain exactly one of 'form' or 'equippedArmor'");
			}

			Props::Source result;
			if (hasForm) {
				const auto& form = a_source.at("form");
				if (!form.is_string() || form.get<std::string>().empty()) {
					throw std::runtime_error(a_subject + " source.form must be a non-empty form ref");
				}
				result.kind = Props::SourceKind::kForm;
				result.form = form.get<std::string>();
				return result;
			}

			const auto& equipped = a_source.at("equippedArmor");
			if (!equipped.is_object() || !equipped.contains("keyword")) {
				throw std::runtime_error(a_subject + " source.equippedArmor requires 'keyword'");
			}
			const Util::JsonView armor{ equipped, a_subject, Util::JsonJoin::kSpace };
			result.keywords = armor.StringOrList("keyword",
				{ .field = "source.equippedArmor.keyword", .expected = "a string or array" },
				{ .field = "equippedArmor.keyword entries" }, /*nonEmptyItems*/ true);
			if (result.keywords.empty()) {
				throw std::runtime_error(a_subject + " source.equippedArmor.keyword cannot be empty");
			}
			result.kind = Props::SourceKind::kEquippedArmor;
			return result;
		}

		// Separate so a partial `props` template can validate `scale` without an attachment node.
		float ParsePropScale(const json& a_owner, const std::string& a_subject)
		{
			const float scale = a_owner.value("scale", 1.0f);
			if (!std::isfinite(scale) || scale <= 0.0f || scale > 10.0f) {
				throw std::runtime_error(a_subject + " scale must be finite and in (0,10]");
			}
			return scale;
		}

		Props::Attachment ParsePropAttachment(
			const json& a_owner, const std::string& a_subject)
		{
			Props::Attachment result;
			if (a_owner.contains("node")) {
				throw std::runtime_error(a_subject + " retired key 'node'; use 'attachmentNode'");
			}
			const auto node = a_owner.find("attachmentNode");
			if (node == a_owner.end() || !node->is_string() || node->get<std::string>().empty()) {
				throw std::runtime_error(a_subject + " requires a non-empty 'attachmentNode'");
			}
			result.targetNode = node->get<std::string>();
			result.position = ParsePropVector(a_owner, "position", a_subject);
			result.rotation = ParsePropVector(a_owner, "rotation", a_subject);
			result.scale = ParsePropScale(a_owner, a_subject);
			return result;
		}

		// Strict subset of action keys: timing, `type`, `prop` and `role` are never inheritable.
		constexpr std::array<const char*, 5> kPropTemplateKeys{
			"source", "attachmentNode", "position", "rotation", "scale"
		};
		constexpr auto kPropMergeKeys = kPropTemplateKeys;

		// Templates may be partial; completeness is checked on the merged action. Unknown keys are
		// rejected since a stray "at"/"role" would silently do nothing.
		void ValidatePropTemplate(const json& a_templ, const std::string& a_subject)
		{
			Util::JsonView{ a_templ, a_subject, Util::JsonJoin::kSpace }.RejectUnknownKeys(
				kPropTemplateKeys, "has unknown key",
				" (a prop template holds only 'source', 'attachmentNode', 'position', 'rotation', 'scale')");
			if (const auto it = a_templ.find("source"); it != a_templ.end()) {
				(void)ParsePropSource(*it, a_subject);
			}
			const auto node = a_templ.find("attachmentNode");
			if (node != a_templ.end() && (!node->is_string() || node->get_ref<const std::string&>().empty())) {
				throw std::runtime_error(a_subject + " attachment node must be a non-empty string");
			}
			(void)ParsePropVector(a_templ, "position", a_subject);
			(void)ParsePropVector(a_templ, "rotation", a_subject);
			(void)ParsePropScale(a_templ, a_subject);
		}

		std::string DescribePropRegistry(const PropRegistry& a_props)
		{
			if (a_props.empty()) {
				return "this file's top-level 'props' registry is empty";
			}
			constexpr std::size_t kMaxListed = 8;
			std::string listed = "defined: ";
			std::size_t shown = 0;
			for (const auto& [id, templ] : a_props) {
				if (shown == kMaxListed) {
					listed += ", ...";
					break;
				}
				if (shown++ != 0) {
					listed += ", ";
				}
				listed += "'" + id + "'";
			}
			return listed;
		}

		// Template = `use`, else the action's `prop` id. Returns a_action when no template applies, else
		// a_merged_out. Shallow key-level merge (action wins); `source` is never deep-merged since it is
		// exactly-one-of `form`/`equippedArmor`. No validation here.
		const json& ResolvePropAttach(const json& a_action, const PropRegistry& a_props,
			const std::string& a_prop, const std::string& a_subject, json& a_merged_out)
		{
			const auto useIt = a_action.find("use");
			const bool hasUse = useIt != a_action.end();
			// Stricter than a node's `use`: a silently dropped template reference must not happen.
			if (hasUse && (!useIt->is_string() || useIt->get_ref<const std::string&>().empty())) {
				throw std::runtime_error(a_subject +
					" 'use' must be a non-empty string naming a top-level 'props' definition");
			}
			const std::string& templateId = hasUse ? useIt->get_ref<const std::string&>() : a_prop;

			const auto templ = a_props.find(templateId);  // case-sensitive
			if (templ == a_props.end()) {
				if (hasUse) {
					throw std::runtime_error(a_subject + " prop template '" + templateId +
						"' is not defined in this file's top-level 'props' registry (" +
						DescribePropRegistry(a_props) + ")");
				}
				// Bare `prop` with no template: fully inline attach.
				return a_action;
			}

			a_merged_out = a_action;
			for (const char* const key : kPropMergeKeys) {
				if (a_merged_out.contains(key)) {
					continue;
				}
				if (const auto it = templ->second.find(key); it != templ->second.end()) {
					a_merged_out[key] = *it;
				}
			}
			return a_merged_out;
		}

		void ParseActionTrack(const json& a_entries, NodeDef& a_node_out, const PropRegistry& a_props)
		{
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node_out.id + "': 'action' track must be an array");
			}
			for (const auto& a : a_entries) {
				ActionEntry ae;
				ae.type = a.value("type", std::string{});
				if (ae.type.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': an action track entry is missing 'type'");
				}
				ae.role = a.value("role", std::string{});
				ae.hold = a.value("hold", false);
				ae.duration = a.value("duration", 0.0f);
				ae.set = a.value("set", std::string{});
				ae.item = a.value("item", std::string{});
				ae.prop = a.value("prop", std::string{});
				const auto typeLower = ToLower(ae.type);
				if (typeLower.rfind("osf.", 0) == 0) {
					const auto kind = ParseBuiltinAction(typeLower);
					if (!kind) {
						throw std::runtime_error("node '" + a_node_out.id + "': unknown built-in action '" + ae.type + "'");
					}
					ae.kind = *kind;
					// `role` is optional (empty = first participant); named roles are checked in ValidateGraph.
					if (ae.kind == ActionKind::kVoicePlay) {
						ae.emitter = ParseSoundEmitter(a, SoundEmitter::kListener, a_node_out.id,
							"action '" + ae.type + "'");
						if (ae.set.empty()) {
							throw std::runtime_error("node '" + a_node_out.id + "': action 'osf.voice.play' requires 'set'");
						}
					}
					if (ae.kind == ActionKind::kEquipmentEquip && ae.item.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': action 'osf.equipment.equip' requires 'item'");
					}
					if ((ae.kind == ActionKind::kPropAttach ||
						 ae.kind == ActionKind::kPropDestroy) && ae.prop.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': action '" +
							ae.type + "' requires a non-empty 'prop'");
					}
					if (ae.kind == ActionKind::kPropAttach) {
						const std::string subject = "node '" + a_node_out.id + "': action 'osf.prop.attach'";
						json merged;
						const json& attach = ResolvePropAttach(a, a_props, ae.prop, subject, merged);
						const auto source = attach.find("source");
						if (source == attach.end()) {
							// Merged result = a template matched but is partial; don't report "no matching entry".
							if (&attach != &a) {
								const std::string templateId = a.contains("use") ? a["use"].get<std::string>() : ae.prop;
								throw std::runtime_error(subject + " prop template '" + templateId +
									"' supplies no 'source' and the action authors none");
							}
							// With a registry, the likeliest cause is a typo'd `prop` id.
							if (a_props.empty()) {
								throw std::runtime_error(subject + " requires 'source'");
							}
							throw std::runtime_error(subject + " prop '" + ae.prop +
								"' has no inline 'source' and no matching entry in this file's top-level "
								"'props' registry (" + DescribePropRegistry(a_props) + ")");
						}
						ae.propSource = ParsePropSource(*source, subject);
						ae.propAttachment = ParsePropAttachment(attach, subject);
					} else if (a.contains("use")) {
						// Rejected elsewhere so a mistyped `type` doesn't silently drop it.
						throw std::runtime_error("node '" + a_node_out.id + "': action '" + ae.type +
							"': 'use' is only meaningful on 'osf.prop.attach'");
					}
				} else if (a.value("required", false)) {
					// `required` is reserved; custom actions are best-effort.
					throw std::runtime_error("node '" + a_node_out.id + "': custom action '" + ae.type + "' cannot be 'required'");
				}
				ParseTrackTiming(a, ae, a_node_out.id, "action '" + ae.type + "'", /*a_atRequired*/ false);
				a_node_out.actions.push_back(std::move(ae));
			}
		}

		// Sound ladder { role?, spec, repeat?, at }: each hit appends its tag(s) to the base spec.
		//   grouped: "at": { "low": [0.1, 0.3], "loud": [0.8] }
		//   array:   0.5 | [0.5, "loud", ...] | { at, tags?, spec?, role?, repeat? } (spec replaces base)
		void ExpandSoundLadder(const json& a_lane, NodeDef& a_node_out)
		{
			const std::string baseSpec = a_lane.value("spec", a_lane.value("sound", a_lane.value("pool", std::string{})));
			const std::string laneRole = a_lane.value("role", std::string{});
			const std::string laneRepeat = ToLower(a_lane.value("repeat", "none"));
			const SoundEmitter laneEmitter = ParseSoundEmitter(a_lane, SoundEmitter::kListener,
				a_node_out.id, "sound ladder");

			// With `atFrame`, every bare position is a clip frame; per-hit objects pick their own key.
			const bool laneFrames = a_lane.contains("atFrame");
			const char* const laneKey = laneFrames ? "atFrame" : "at";

			const auto emit = [&](const std::string& a_spec, const json& a_at, const std::string& a_repeat,
				const std::string& a_role, SoundEmitter a_emitter, const char* a_key) {
				if (a_spec.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': a sound mark has no spec (set the lane 'spec' or a per-mark 'spec')");
				}
				SoundEntry se;
				se.spec = a_spec;
				se.role = a_role;
				se.emitter = a_emitter;
				json timing = json::object();
				timing[a_key] = a_at;
				timing["repeat"] = a_repeat;
				ParseTrackTiming(timing, se, a_node_out.id, "sound '" + se.spec + "'", /*a_atRequired*/ true);
				a_node_out.sounds.push_back(std::move(se));
			};

			if (laneFrames && a_lane.contains("at")) {
				throw std::runtime_error("node '" + a_node_out.id + "': a sound ladder sets both 'at' and 'atFrame' (pick one)");
			}
			const auto positionsIt = a_lane.find(laneKey);
			if (positionsIt == a_lane.end()) {
				throw std::runtime_error("node '" + a_node_out.id + "': a sound ladder needs 'at' (an array or tag-keyed object of positions)");
			}
			const json& positions = *positionsIt;

			if (positions.is_object()) {
				if (positions.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': sound ladder '" + laneKey + "' object is empty");
				}
				for (auto it = positions.begin(); it != positions.end(); ++it) {
					if (!it.value().is_array()) {
						throw std::runtime_error("node '" + a_node_out.id + "': sound ladder '" + laneKey + "' group '" + it.key() + "' must be an array of positions");
					}
					std::string spec = baseSpec;
					if (!it.key().empty()) {
						spec += "," + it.key();
					}
					for (const auto& at : it.value()) {
						emit(spec, at, laneRepeat, laneRole, laneEmitter, laneKey);
					}
				}
				return;
			}
			if (!positions.is_array() || positions.empty()) {
				throw std::runtime_error("node '" + a_node_out.id + "': sound ladder '" + laneKey + "' must be a non-empty array or an object keyed by tag");
			}
			for (const auto& m : positions) {
				if (m.is_number() || m.is_string()) {
					emit(baseSpec, m, laneRepeat, laneRole, laneEmitter, laneKey);
				} else if (m.is_array()) {
					if (m.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': an empty sound ladder position");
					}
					std::string spec = baseSpec;
					for (std::size_t i = 1; i < m.size(); i++) {
						if (!m[i].is_string()) {
							throw std::runtime_error("node '" + a_node_out.id + "': sound ladder tags must be strings");
						}
						spec += "," + m[i].get<std::string>();
					}
					emit(spec, m[0], laneRepeat, laneRole, laneEmitter, laneKey);
				} else if (m.is_object()) {
					std::string spec = baseSpec;
					if (auto it = m.find("spec"); it != m.end()) {
						if (!it->is_string()) {
							throw std::runtime_error("node '" + a_node_out.id + "': sound ladder 'spec' must be a string");
						}
						spec = it->get<std::string>();
					}
					if (auto it = m.find("tags"); it != m.end()) {
						if (it->is_string()) {
							spec += "," + it->get<std::string>();
						} else if (it->is_array()) {
							for (const auto& tag : *it) {
								if (!tag.is_string()) {
									throw std::runtime_error("node '" + a_node_out.id + "': sound ladder 'tags' entries must be strings");
								}
								spec += "," + tag.get<std::string>();
							}
						} else {
							throw std::runtime_error("node '" + a_node_out.id + "': sound ladder 'tags' must be a string or an array of strings");
						}
					}
					// emit() rebuilds a single-key timing object, so ParseTrackTiming can't catch both keys here.
					if (m.contains("at") && m.contains("atFrame")) {
						throw std::runtime_error("node '" + a_node_out.id +
							"': a sound ladder hit sets both 'at' and 'atFrame' (pick one)");
					}
					const char* const hitKey = m.contains("atFrame") ? "atFrame" : m.contains("at") ? "at" : laneKey;
					const json at = m.contains(hitKey) ? m.at(hitKey) : json();
					emit(spec, at, ToLower(m.value("repeat", laneRepeat)), m.value("role", laneRole),
						ParseSoundEmitter(m, laneEmitter, a_node_out.id, "sound ladder mark"), hitKey);
				} else {
					throw std::runtime_error("node '" + a_node_out.id + "': a sound ladder hit must be a number, [at, tags...] array, or { at, ... } object");
				}
			}
		}

		void ParseSoundTrack(const json& a_entries, NodeDef& a_node_out)
		{
			if (a_entries.is_object()) {
				ExpandSoundLadder(a_entries, a_node_out);
				return;
			}
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node_out.id + "': 'sound' track must be an array of entries or a { spec, at } ladder object");
			}
			for (const auto& s : a_entries) {
				// An array/object `at` makes the element a ladder; a scalar `at` is a flat entry.
				if (s.is_object()) {
					auto it = s.find("at");
					if (it == s.end()) {
						it = s.find("atFrame");
					}
					if (it != s.end() && (it->is_array() || it->is_object())) {
						ExpandSoundLadder(s, a_node_out);
						continue;
					}
				}
				SoundEntry se;
				// `sound`/`pool` are aliases of `spec`.
				se.spec = s.value("spec", s.value("sound", s.value("pool", std::string{})));
				if (se.spec.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': a sound track entry is missing 'spec'/'sound'/'pool'");
				}
				se.role = s.value("role", std::string{});
				se.emitter = ParseSoundEmitter(s, SoundEmitter::kListener, a_node_out.id,
					"sound '" + se.spec + "'");
				ParseTrackTiming(s, se, a_node_out.id, "sound '" + se.spec + "'", /*a_atRequired*/ false);
				a_node_out.sounds.push_back(std::move(se));
			}
		}

		void ParseCameraTrack(const json& a_entries, NodeDef& a_node_out)
		{
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node_out.id + "': 'camera' track must be an array");
			}
			for (const auto& c : a_entries) {
				CameraEntry ce;
				const std::string stateName = c.value("state", std::string{});
				if (stateName.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': a camera track entry is missing 'state'");
				}
				const auto state = ParseCameraState(stateName);
				if (!state || *state == CameraState::kNone) {
					throw std::runtime_error("node '" + a_node_out.id + "': unknown camera state '" + stateName +
						"' (supported: 'thirdperson_hold', 'freefly', 'vanity_orbit', 'scene_orbit')");
				}
				ce.state = *state;
				ParseTrackTiming(c, ce, a_node_out.id, "camera '" + stateName + "'", /*a_atRequired*/ false);
				ce.distance = c.value("distance", 0.0f);
				if (ce.distance != 0.0f && ce.state != CameraState::kThirdPersonHold) {
					REX::DEBUG("[Registry] node '{}': camera 'distance' is only honored for 'thirdperson_hold' — state '{}' ignores it",
						a_node_out.id, stateName);
				}
				a_node_out.cameras.push_back(std::move(ce));
			}
		}

		// `gender` and `filters.gender` are one constraint; both present and differing rejects.
		SceneRole ParseRole(const json& a_role, const std::string& a_sceneId)
		{
			SceneRole r;
			r.name = a_role.value("name", std::string{});

			std::optional<RoleGender> shorthand;
			if (auto git = a_role.find("gender"); git != a_role.end()) {
				if (!git->is_string()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': 'gender' must be a string");
				}
				shorthand = ParseRoleGender(git->get<std::string>());
			}
			std::optional<RoleGender> fromFilter;

			if (auto fit = a_role.find("filters"); fit != a_role.end()) {
				if (!fit->is_object()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': 'filters' must be an object");
				}
				const Util::JsonView filters{ *fit, "scene '" + a_sceneId + "': role '" + r.name + "'" };
				if (std::string gender; filters.Read("gender", gender, { .field = "filters.gender" })) {
					fromFilter = ParseRoleGender(gender);
				}
				for (const auto& ref : filters.StringOrList("keyword",
					{ .field = "filters.keyword" }, { .field = "filters.keyword entries" })) {
					r.keywords.push_back(ResolveFormRef<RE::BGSKeyword>(ref, a_sceneId, r.name,
						"filters.keyword", "Keyword (KYWD)")->GetFormID());
				}
				for (const auto& ref : filters.StringOrList("race",
					{ .field = "filters.race" }, { .field = "filters.race entries" })) {
					r.races.push_back(ResolveFormRef<RE::TESRace>(ref, a_sceneId, r.name,
						"filters.race", "Race (RACE)")->GetFormID());
				}
			}

			if (shorthand && fromFilter && *shorthand != *fromFilter) {
				throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': 'gender' and filters.gender disagree");
			}
			r.gender = shorthand ? *shorthand : (fromFilter ? *fromFilter : RoleGender::kAny);
			if (auto pit = a_role.find("poseMode"); pit != a_role.end()) {
				if (!pit->is_string()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': 'poseMode' must be a string ('override' or 'additive')");
				}
				const auto mode = ToLower(pit->get<std::string>());
				if (mode == "override") {
					r.poseMode = Animation::PoseMode::kOverride;
				} else if (mode == "additive") {
					r.poseMode = Animation::PoseMode::kAdditive;
				} else {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': unknown 'poseMode' value '" + pit->get<std::string>() +
						"' (expected 'override' or 'additive')");
				}
			}
			if (auto wit = a_role.find("poseWeight"); wit != a_role.end()) {
				if (!wit->is_number()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': 'poseWeight' must be a finite number");
				}
				const auto normalized = Animation::NormalizePoseWeight(wit->get<double>());
				if (!normalized) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': 'poseWeight' must be finite");
				}
				r.poseWeight = *normalized;
			}
			// Only the mask's bones are stamped; the rest stay engine-driven.
			if (auto mit = a_role.find("mask"); mit != a_role.end()) {
				if (!mit->is_string()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': 'mask' must be a string naming a bone mask (" + Animation::BoneMask::KnownList() + ")");
				}
				const auto* named = Animation::BoneMask::Find(mit->get<std::string>());
				if (!named) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': unknown 'mask' value '" + mit->get<std::string>() +
						"' (expected one of: " + Animation::BoneMask::KnownList() + ")");
				}
				r.mask = named->id;  // canonical casing
			}
			if (auto bit = a_role.find("preserveBones"); bit != a_role.end()) {
				if (!bit->is_array()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': 'preserveBones' must be an array of strings");
				}
				std::unordered_set<std::string> seen;
				for (const auto& entry : *bit) {
					if (!entry.is_string() || entry.get_ref<const std::string&>().empty()) {
						throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
							"': 'preserveBones' entries must be non-empty strings");
					}
					auto bone = entry.get<std::string>();
					if (seen.emplace(ToLower(bone)).second) {
						r.preserveBones.push_back(std::move(bone));
					}
				}
			}
			if (auto oit = a_role.find("offset"); oit != a_role.end()) {
				if (!oit->is_object()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': 'offset' must be an { x, y, z, heading } object");
				}
				r.offset = ParseOffsetField(*oit);
			}
			// Soft: a malformed ref is dropped with a warning, never rejecting the scene.
			if (auto qit = a_role.find("equip"); qit != a_role.end()) {
				auto checkRef = [&](const std::string& a_ref, const char* a_key) -> std::string {
					if (!a_ref.empty() && a_ref.find('|') == std::string::npos) {
						REX::WARN("[Registry] scene '{}': role '{}': equip.{} '{}' is not a "
							"\"Plugin.esm|0xLocalID\" form ref — equip dropped (scene still loads)",
							a_sceneId, r.name, a_key, a_ref);
						return std::string{};
					}
					return a_ref;
				};
				if (qit->is_string()) {
					r.equip.any = checkRef(qit->get<std::string>(), "any");
				} else if (qit->is_object()) {
					r.equip.male = checkRef(qit->value("male", std::string{}), "male");
					r.equip.female = checkRef(qit->value("female", std::string{}), "female");
					r.equip.any = checkRef(qit->value("any", std::string{}), "any");
				} else {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name +
						"': 'equip' must be a form-ref string or an object { male?, female?, any? }");
				}
			}
			return r;
		}

		// "m" -> template copy; { "id": "m", ... } -> template merge-patched (RFC 7386) by the other keys;
		// no `id` -> inline role. Bad references reject only this scene.
		PendingRole ExpandRoleEntry(const json& a_entry, const std::string& a_sceneId, const RoleRegistry& a_registry)
		{
			if (a_entry.is_string()) {
				const auto& refId = a_entry.get_ref<const std::string&>();
				const auto ref = a_registry.find(refId);
				if (ref == a_registry.end()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role reference '" + refId +
						"' is not defined in this file's top-level 'roles' registry");
				}
				return PendingRole{ ref->second.parsed, RoleNameKind::kAutomatic, ref->second.parsed.name };
			}
			if (!a_entry.is_object()) {
				throw std::runtime_error("scene '" + a_sceneId +
					"': 'roles' entries must be role objects or registry id strings");
			}
			const auto idIt = a_entry.find("id");
			if (idIt == a_entry.end()) {
				PendingRole out;
				out.role = ParseRole(a_entry, a_sceneId);
				out.nameKind = out.role.name.empty() ? RoleNameKind::kAnonymous : RoleNameKind::kExplicit;
				return out;
			}
			if (!idIt->is_string() || idIt->get_ref<const std::string&>().empty()) {
				throw std::runtime_error("scene '" + a_sceneId +
					"': a role object's 'id' must be a non-empty string naming a roles-registry definition");
			}
			const auto& refId = idIt->get_ref<const std::string&>();
			const auto ref = a_registry.find(refId);
			if (ref == a_registry.end()) {
				throw std::runtime_error("scene '" + a_sceneId + "': role reference '" + refId +
					"' is not defined in this file's top-level 'roles' registry");
			}
			json merged = ref->second.raw;
			json overrides = a_entry;
			overrides.erase("id");
			merged.merge_patch(overrides);
			// `gender`/`filters.gender` are one constraint: an override on one path drops the inherited
			// other, so ParseRole's disagreement check doesn't trip.
			const bool topGender = overrides.contains("gender");
			bool nestedGender = false;
			if (const auto fit = overrides.find("filters"); fit != overrides.end() && fit->is_object()) {
				nestedGender = fit->contains("gender");
			}
			if (topGender && !nestedGender) {
				if (auto fit = merged.find("filters"); fit != merged.end() && fit->is_object()) {
					fit->erase("gender");
				}
			} else if (nestedGender && !topGender) {
				merged.erase("gender");
			}
			PendingRole out;
			out.role = ParseRole(merged, a_sceneId);
			if (const auto nit = overrides.find("name"); nit != overrides.end()) {
				out.nameKind = (nit->is_string() && !nit->get_ref<const std::string&>().empty()) ?
					RoleNameKind::kExplicit : RoleNameKind::kAnonymous;
			} else {
				out.nameKind = RoleNameKind::kAutomatic;
				out.autoBase = ref->second.parsed.name;
			}
			return out;
		}

		// Explicit names reserved first; automatic ones numbered past collisions (["m","m","f"] -> m, m2, f).
		void AssignRoleNames(NodeDef& a_def, std::vector<PendingRole>& a_pending)
		{
			std::unordered_set<std::string> used;
			for (const auto& p : a_pending) {
				if (p.nameKind == RoleNameKind::kExplicit && !used.insert(ToLower(p.role.name)).second) {
					throw std::runtime_error("scene '" + a_def.id + "': duplicate role name '" + p.role.name +
						"' (role names must be unique within a scene)");
				}
			}
			a_def.roles.reserve(a_pending.size());
			for (auto& p : a_pending) {
				if (p.nameKind == RoleNameKind::kAutomatic && !p.autoBase.empty()) {
					std::string name = p.autoBase;
					for (std::int32_t n = 2; !used.insert(ToLower(name)).second; ++n) {
						name = p.autoBase + std::to_string(n);
					}
					p.role.name = std::move(name);
				}
				a_def.roles.push_back(std::move(p.role));
			}
		}

		// a_out holds the inherited grant; `disable` removes from it, so scene and file lists compose.
		void ApplySceneControls(const json& a_value, SceneControls& a_out, const std::string& a_ctx,
			std::string_view a_key = "sceneControls")
		{
			if (a_value.is_boolean()) {
				a_out.enabled = a_value.get<bool>();
				return;
			}
			if (!a_value.is_object()) {
				throw std::runtime_error(a_ctx + ": '" + std::string(a_key) + "' must be a boolean or an object");
			}
			const std::string prefix = "'" + std::string(a_key) + ".";
			const std::string enabledField = prefix + "enabled'";
			const std::string disableField = prefix + "disable'";
			const std::string disableEntries = disableField + " entries";
			const std::string lockedField = prefix + "locked'";

			const Util::JsonView controls{ a_value, a_ctx };
			controls.Read("enabled", a_out.enabled, { .field = enabledField });
			for (const auto& name : controls.ReadList("disable",
				{ .field = disableField }, { .field = disableEntries })) {
				const auto bit = Input::CapabilityBit(name);
				if (bit == 0) {
					REX::WARN("[Registry] {}: unknown {} capability '{}' — ignored (typo, or a newer OSF Animation?)", a_ctx, a_key, name);
				}
				a_out.capabilities &= ~bit;
			}
			controls.Read("locked", a_out.locked, { .field = lockedField });
		}

		const json* ReadContentKey(const json& a_object, std::string_view a_key,
			std::string_view a_retired, const std::string& a_ctx)
		{
			if (a_object.contains(std::string{ a_retired })) {
				throw std::runtime_error(a_ctx + ": retired key '" + std::string(a_retired) +
					"'; use '" + std::string(a_key) + "'");
			}
			const auto value = a_object.find(std::string{ a_key });
			return value != a_object.end() ? &*value : nullptr;
		}

		bool ReadContentBool(const json& a_object, std::string_view a_key,
			std::string_view a_retired, bool a_default, const std::string& a_ctx)
		{
			const json* value = ReadContentKey(a_object, a_key, a_retired, a_ctx);
			if (!value) return a_default;
			if (!value->is_boolean()) {
				throw std::runtime_error(a_ctx + ": '" + std::string(a_key) + "' must be true or false");
			}
			return value->get<bool>();
		}

		Animation::WorldPlacementMode ReadWorldPlacement(const json& a_object,
			Animation::WorldPlacementMode a_default, const std::string& a_ctx)
		{
			const json* value = ReadContentKey(a_object, "placement", "inPlace", a_ctx);
			if (!value) {
				return a_default;
			}
			if (!value->is_string()) {
				throw std::runtime_error(a_ctx + ": 'placement' must be 'anchorAndPin' or 'followActor'");
			}
			const auto placement = value->get<std::string>();
			if (placement == "anchorAndPin") return Animation::WorldPlacementMode::kAnchorAndPin;
			if (placement == "followActor") return Animation::WorldPlacementMode::kFollowActor;
			throw std::runtime_error(a_ctx + ": unknown 'placement' value '" + placement +
				"' (expected 'anchorAndPin' or 'followActor')");
		}

		// File-level keys a scene inherits when it omits its own (`roles`, `anchor`, `camera`, `clipRoot` are threaded separately).
		struct ContentFileDefaults
		{
			std::string              name;
			bool                     playerInputLock = true;
			bool                     hideApparel = true;
			bool                     clearHeldItems = true;
			bool                     fade = false;
			bool                     unlisted = false;
			Animation::WorldPlacementMode worldPlacement = Animation::WorldPlacementMode::kAnchorAndPin;
			SceneControls            sceneControls{};
			std::int32_t             priority = 0;
			std::int32_t             weight = 1;
			std::vector<std::string> tags;  // unioned with a scene's own tags
		};

		void ParseNodeMeta(const json& a_json, NodeDef& def, const ContentFileDefaults& a_defaults)
		{
			const std::string sceneContext = "scene '" + def.id + "'";
			if (a_json.contains("allowManualHold")) {
				throw std::runtime_error(sceneContext + ": retired key 'allowManualHold'; holding is caller navigation policy");
			}
			const Util::JsonView scene{ a_json, sceneContext };

			// priority stays on value(): it truncates floats (3.7 -> 3) where ReadInt would reject.
			def.name = a_json.value("name", a_defaults.name.empty() ? def.id : a_defaults.name);
			def.priority = a_json.value("priority", a_defaults.priority);
			def.weight = a_defaults.weight;
			scene.ReadInt("weight", def.weight, 1, 1000000);
			def.unlisted = a_defaults.unlisted;
			scene.Read("unlisted", def.unlisted);
			def.playerInputLock = ReadContentBool(a_json, "playerInputLock", "lockPlayer",
				a_defaults.playerInputLock, sceneContext);
			def.hideApparel = ReadContentBool(a_json, "hideApparel", "stripActors",
				a_defaults.hideApparel, sceneContext);
			def.clearHeldItems = a_defaults.clearHeldItems;
			scene.Read("clearHeldItems", def.clearHeldItems);
			def.fade = a_defaults.fade;
			scene.Read("fade", def.fade);
			def.worldPlacement = ReadWorldPlacement(a_json, a_defaults.worldPlacement, sceneContext);
			def.sceneControls = a_defaults.sceneControls;
			if (const json* controls = ReadContentKey(a_json, "sceneControls", "playerControl", sceneContext)) {
				ApplySceneControls(*controls, def.sceneControls, sceneContext,
					"sceneControls");
			}
		}

		void ValidateNodeId(const std::string& a_id)
		{
			if (a_id.empty() || a_id.size() > 512 || a_id.front() == '$' || a_id.find('\0') != std::string::npos) {
				throw std::runtime_error("node id must be non-empty, at most 512 bytes, and not start with '$' or contain NUL");
			}
		}

		void ParseOsfCueLane(const json& a_entries, NodeDef& a_node);

		void ParseLoopCount(const json& a_value, const std::string& a_subject,
			std::int32_t& a_default, std::string& a_global)
		{
			const auto readCount = [&](const json& value, std::string_view field) {
				if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<std::int32_t>::max()) {
					throw std::runtime_error(a_subject + ": '" + std::string(field) + "' must be an integer in [0, 2147483647]");
				}
				return value.get<std::int32_t>();
			};
			if (!a_value.is_object()) {
				a_default = readCount(a_value, "loops");
				return;
			}
			const Util::JsonView binding{ a_value, a_subject + " loops" };
			constexpr const char* keys[]{ "global", "default" };
			binding.RejectUnknownKeys(keys);
			if (!binding.ReadNonEmpty("global", a_global) || a_global.find('\0') != std::string::npos ||
				!Util::ParseFormRef(a_global)) {
				throw std::runtime_error(a_subject + ": loops 'global' must be a form reference (Plugin.esm|0xLocalID)");
			}
			const auto* fallback = binding.Find("default");
			if (!fallback) throw std::runtime_error(a_subject + ": loops global requires a 'default' count");
			a_default = readCount(*fallback, "loops.default");
		}

		// a_ioActorCount is authoritative when a_fixed; otherwise the first stage's clip count sets it.
		std::vector<StageDef> ParseOsfStageList(const json& a_stages, const std::string& a_subject,
			size_t& a_ioActorCount, bool a_fixed, std::string_view a_clipRoot)
		{
			if (!a_stages.is_array() || a_stages.empty()) {
				throw std::runtime_error(a_subject + ": 'stages' must be a non-empty array");
			}
			std::vector<StageDef> out;
			size_t actorCount = a_ioActorCount;
			for (const auto& jStage : a_stages) {
				StageDef info;
				const json* clipsNode = nullptr;
				bool timingGiven = false;
				if (jStage.is_array()) {
					clipsNode = &jStage;
				} else if (jStage.is_object()) {
					if (jStage.contains("allowManualHold")) {
						throw std::runtime_error(a_subject + ": retired key 'allowManualHold'; holding is caller navigation policy");
					}
					info.timer = jStage.value("timer", 0.0f);
					if (const auto it = jStage.find("loops"); it != jStage.end()) {
						ParseLoopCount(*it, a_subject + " stage " + std::to_string(out.size()), info.loops, info.loopGlobal);
					}
					timingGiven = jStage.contains("timer") || jStage.contains("loops");
					// A frozen clip never wraps, so hold + loops could never expire.
					if (auto it = jStage.find("hold"); it != jStage.end()) {
						if (it->is_boolean()) {
							info.hold = it->get<bool>() ? 1.0f : -1.0f;
						} else if (it->is_number()) {
							const float at = it->get<float>();
							if (!std::isfinite(at) || at < 0.0f || at > 1.0f) {
								throw std::runtime_error(a_subject + ": stage 'hold' must be true or a clip position in [0, 1]");
							}
							info.hold = at;
						} else {
							throw std::runtime_error(a_subject + ": stage 'hold' must be true or a clip position in [0, 1]");
						}
						if (info.hold >= 0.0f && jStage.contains("loops")) {
							throw std::runtime_error(a_subject + ": stage 'hold' cannot combine with 'loops' — a frozen "
								"clip never loops; use 'timer' or a manual advance to leave the stage");
						}
					}
						if (auto it = jStage.find("name"); it != jStage.end()) {
						if (!it->is_string()) {
							throw std::runtime_error(a_subject + ": stage 'name' must be a string");
						}
						info.name = it->get<std::string>();
					}
					if (auto it = jStage.find("tags"); it != jStage.end()) {
						if (!it->is_array()) {
							throw std::runtime_error(a_subject + ": stage 'tags' must be an array of strings");
						}
						for (const auto& t : *it) {
							if (!t.is_string()) {
								throw std::runtime_error(a_subject + ": stage 'tags' must be an array of strings");
							}
							info.tags.push_back(t.get<std::string>());
						}
					}
					if (jStage.contains("cue") || jStage.contains("action") ||
						jStage.contains("sound") || jStage.contains("camera")) {
						throw std::runtime_error(a_subject + ": put cue/action/sound/camera on the node itself; "
							"use explicit nodes for stage-scoped tracks");
					}
					const auto clipsIt = jStage.find("clips");
					if (clipsIt == jStage.end() || !clipsIt->is_array()) {
						throw std::runtime_error(a_subject + ": every stage needs a 'clips' array (one clip per role)");
					}
					clipsNode = &(*clipsIt);
				} else {
					throw std::runtime_error(a_subject + ": a stage must be a clips array (shorthand) or a { timer, loops, clips } object");
				}
				if (!timingGiven && info.hold < 0.0f) {
					info.loops = 1;
				}
				for (const auto& jClip : *clipsNode) {
					info.clips.push_back(ParseStageClip(jClip, a_clipRoot, a_subject));
				}
				if (info.clips.empty()) {
					throw std::runtime_error(a_subject + ": every stage needs at least one clip");
				}
				if (!a_fixed && actorCount == 0) {
					actorCount = info.clips.size();
				}
				if (info.clips.size() != actorCount) {
					throw std::runtime_error(a_subject + ": stage has " + std::to_string(info.clips.size()) +
						" clip(s) but the scene has " + std::to_string(actorCount) + " role(s)");
				}
				out.push_back(std::move(info));
			}
			a_ioActorCount = actorCount;
			return out;
		}

		void ParseOsfCueLane(const json& a_entries, NodeDef& a_node)
		{
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node.id + "': 'cue' track must be an array");
			}
			for (const auto& c : a_entries) {
				CueEntry ce;
				if (const auto delivery = c.find("delivery"); delivery != c.end()) {
					if (!delivery->is_string() || (*delivery != "both" && *delivery != "native")) {
						throw std::runtime_error("node '" + a_node.id + "': cue 'delivery' must be 'both' or 'native'");
					}
					ce.nativeOnly = *delivery == "native";
				}
				ce.id = c.value("id", std::string{});
				if (ce.id.empty()) {
					throw std::runtime_error("node '" + a_node.id + "': a cue track entry is missing 'id'");
				}
				if (const auto role = c.find("role"); role != c.end()) {
					if (!role->is_string()) {
						throw std::runtime_error("node '" + a_node.id + "': cue '" + ce.id + "' role must be a string");
					}
					ce.role = role->get<std::string>();
				}
				ParseTrackTiming(c, ce, a_node.id, "cue '" + ce.id + "'", /*a_atRequired*/ true);
				a_node.cues.push_back(std::move(ce));
			}
		}

		// Timing and lanes belong to the same globally addressed definition as its metadata.
		void ParseNodeTimeline(const json& a_node, NodeDef& n, const PropRegistry& a_props)
		{
			// Omitted loops preserve the terminal stage; 0 holds, N sets a loop budget.
			if (n.stages.back().hold >= 0 && a_node.contains("loops")) {
				throw std::runtime_error("node '" + n.id + "': loops cannot bound a frozen terminal stage; use timer or manual advance");
			}
			if (const auto it = a_node.find("loops"); it != a_node.end()) {
				ParseLoopCount(*it, "node '" + n.id + "'", n.loopCount, n.loopGlobal);
				if (n.loopCount == 0) {
					n.loopMode = LoopMode::kHold;
				} else {
					n.loopMode = LoopMode::kCount;
				}
			} else {
				n.loopMode = LoopMode::kTimeline;
			}
			n.timerSec = a_node.value("timer", -1.0f);
			if (a_node.contains("timer") && (!std::isfinite(n.timerSec) || n.timerSec < 0)) {
				throw std::runtime_error("node '" + n.id + "': timer must be finite and non-negative");
			}
			if (a_node.contains("allowManualHold")) {
				throw std::runtime_error("node '" + n.id + "': retired key 'allowManualHold'; holding is caller navigation policy");
			}

			std::unordered_set<std::string> edgeIds;
			int defaults = 0;
			bool hasTimerEdge = false;
			if (const auto it = a_node.find("edges"); it != a_node.end()) {
				for (const auto& jEdge : *it) {
					auto e = ParseEdge(jEdge, n.id);
					if (e.when == EdgeWhen::kEnd && n.loopMode == LoopMode::kHold) {
						throw std::runtime_error("node '" + n.id + "': an 'end' edge on a hold node (loops:0) can never fire");
					}
					if (e.when == EdgeWhen::kLoops && n.loopMode != LoopMode::kCount && n.loopGlobal.empty()) {
						throw std::runtime_error("node '" + n.id + "': a 'loops' edge needs a counted loop (loops:N, N >= 1)");
					}
					if (e.when == EdgeWhen::kTimer) {
						hasTimerEdge = true;
					}
					if (e.isDefault) {
						defaults++;
					}
					if (!e.id.empty() && !edgeIds.insert(ToLower(e.id)).second) {
						throw std::runtime_error("node '" + n.id + "': duplicate edge id '" + e.id + "'");
					}
					n.edges.push_back(std::move(e));
				}
			}
			if (defaults > 1) {
				throw std::runtime_error("node '" + n.id + "': more than one default advance edge");
			}
			if (hasTimerEdge && (n.timerSec >= 0 ? n.timerSec : n.stages.back().timer) <= 0.0f) {
				throw std::runtime_error("node '" + n.id + "': has a 'timer' edge but 'timer' <= 0");
			}

			if (auto it = a_node.find("cue"); it != a_node.end()) {
				ParseOsfCueLane(*it, n);
			}
			if (auto it = a_node.find("action"); it != a_node.end()) {
				ParseActionTrack(*it, n, a_props);
			}
			if (auto it = a_node.find("sound"); it != a_node.end()) {
				ParseSoundTrack(*it, n);
			}
			if (auto it = a_node.find("camera"); it != a_node.end() && !it->is_string()) {
				ParseCameraTrack(*it, n);
			}

			for (const auto& e : n.edges) {
				if (e.when != EdgeWhen::kTrigger) {
					continue;
				}
				const auto want = ToLower(e.trigger);
				bool found = false;
				for (const auto& c : n.cues) {
					if (ToLower(c.id) == want) {
						found = true;
						break;
					}
				}
				if (!found) {
					throw std::runtime_error("node '" + n.id + "': trigger edge references cue '" + e.trigger +
						"' with no matching cue track entry on this node");
				}
			}
		}

		// Lane roles must name this node's resolved cast.
		void ValidateRoleRefs(const NodeDef& def)
		{
			std::unordered_set<std::string> roleNames;
			for (const auto& role : def.roles) if (!role.name.empty()) roleNames.insert(ToLower(role.name));
			const auto validate = [&](const auto& entries, const char* lane, const auto& name) {
				for (const auto& entry : entries) {
					if (!entry.role.empty() && !roleNames.contains(ToLower(entry.role))) {
						throw std::runtime_error("node '" + def.id + "': " + lane + " '" + name(entry) +
							"' references undeclared role '" + entry.role + "'");
					}
				}
			};
			validate(def.actions, "action", [](const auto& entry) { return entry.type; });
			validate(def.sounds, "sound", [](const auto& entry) { return entry.spec; });
			validate(def.cues, "cue", [](const auto& entry) { return entry.id; });
		}

		struct AnchorReq
		{
			std::vector<RE::TESFormID>      keywords;
			std::vector<RE::TESFormID>      baseForms;
			Animation::ParticipantPlacement offset{};
			bool                            given = false;  // key present
		};

		// { keyword?, base?, offset? }; an unresolvable ref rejects the scene.
		AnchorReq ParseAnchorBlock(const json& a_json, const std::string& a_subject)
		{
			AnchorReq req;
			const auto it = a_json.find("anchor");
			if (it == a_json.end()) {
				return req;
			}
			if (!it->is_object()) {
				throw std::runtime_error(a_subject + ": 'anchor' must be an object { keyword?, base?, offset? }");
			}
			const json& anchor = *it;
			req.given = true;

			const Util::JsonView view{ anchor, a_subject };
			for (const auto& ref : view.StringOrList("keyword",
				{ .field = "anchor.keyword" }, { .field = "anchor.keyword entries" })) {
				auto* kw = Util::ResolveFormRef<RE::BGSKeyword>(ref);
				if (!kw) {
					throw std::runtime_error(a_subject + ": anchor.keyword '" + ref +
						"' is malformed, names an unloaded plugin, or isn't a Keyword (KYWD) (use \"Plugin.esm|0xLocalID\")");
				}
				req.keywords.push_back(kw->GetFormID());
			}
			for (const auto& ref : view.StringOrList("base",
				{ .field = "anchor.base" }, { .field = "anchor.base entries" })) {
				const auto id = Util::ComposeFormID(ref);
				if (!id) {
					throw std::runtime_error(a_subject + ": anchor.base '" + ref +
						"' is malformed or names an unloaded plugin (use \"Plugin.esm|0xLocalID\")");
				}
				if (!RE::TESForm::LookupByID(*id)) {
					throw std::runtime_error(a_subject + ": anchor.base '" + ref + "' did not resolve to a form");
				}
				req.baseForms.push_back(*id);
			}
			if (req.keywords.empty() && req.baseForms.empty()) {
				throw std::runtime_error(a_subject + ": 'anchor' needs at least one 'keyword' or 'base' (else nothing can satisfy it)");
			}
			if (auto oit = anchor.find("offset"); oit != anchor.end()) {
				if (!oit->is_object()) {
					throw std::runtime_error(a_subject + ": anchor.offset must be an { x, y, z, heading } object");
				}
				req.offset = ParseOffsetField(*oit);
			}
			return req;
		}

		// a_fileRoles: array form of file-level `roles` (inherited); a_roleRegistry: object form (templates).
		NodeDef ParseNodeDefinition(const json& a_json,
			const ContentFileDefaults& a_defaults, std::optional<CameraState> a_cameraDefault,
			const std::vector<SceneRole>& a_fileRoles, const RoleRegistry& a_roleRegistry,
			const PropRegistry& a_propRegistry,
			std::string_view a_packClipRoot, const AnchorReq& a_anchorDefault)
		{
			NodeDef def;
			def.id = a_json.value("id", std::string{});
			if (def.id.empty()) {
				throw std::runtime_error("scene missing 'id'");
			}
			ValidateNodeId(def.id);
			for (const char* key : { "nodes", "scenes", "entry", "linearStages", "use" }) {
				if (a_json.contains(key)) throw std::runtime_error("node '" + def.id + "': retired key '" + key + "'; author global nodes and edges");
			}
			const std::string clipRoot = a_json.contains("clipRoot") ?
				NormalizeClipRoot(a_json.value("clipRoot", std::string{}), "scene '" + def.id + "'") :
				std::string(a_packClipRoot);
			ParseNodeMeta(a_json, def, a_defaults);
			// File-level tags first, unioned case-insensitively via tagSet.
			const auto addTag = [&def](const std::string& a_tag) {
				if (def.tagSet.insert(ToLower(a_tag)).second) {
					def.tags.push_back(a_tag);
				}
			};
			for (const auto& t : a_defaults.tags) {
				addTag(t);
			}
			if (const auto it = a_json.find("tags"); it != a_json.end()) {
				for (const auto& t : *it) {
					addTag(t.get<std::string>());
				}
			}
		// A scene's `roles` replaces the file-level array. A registry is not a default cast: omitting
		// `roles` falls through to clip-count inference.
		bool rolesGiven = false;
		std::vector<PendingRole> pendingRoles;
		if (const auto it = a_json.find("roles"); it != a_json.end()) {
			if (!it->is_array()) {
				throw std::runtime_error("scene '" + def.id + "': 'roles' must be an array");
			}
			rolesGiven = true;
			pendingRoles.reserve(it->size());
			for (const auto& jRole : *it) {
				pendingRoles.push_back(ExpandRoleEntry(jRole, def.id, a_roleRegistry));
			}
		} else if (!a_fileRoles.empty()) {
			rolesGiven = true;
			pendingRoles.reserve(a_fileRoles.size());
			for (const auto& r : a_fileRoles) {
				pendingRoles.push_back(PendingRole{ r, r.name.empty() ? RoleNameKind::kAnonymous : RoleNameKind::kExplicit, {} });
			}
		}
		AssignRoleNames(def, pendingRoles);

			// A scene's `anchor` replaces the file-level default entirely.
			{
				AnchorReq anchorReq = ParseAnchorBlock(a_json, "scene '" + def.id + "'");
				if (!anchorReq.given && a_anchorDefault.given) {
					anchorReq = a_anchorDefault;
				}
				def.anchorKeywords = anchorReq.keywords;
				def.anchorBaseForms = anchorReq.baseForms;
				def.anchorOffset = anchorReq.offset;
			}
				if (def.worldPlacement == Animation::WorldPlacementMode::kFollowActor && def.RequiresAnchor()) {
				throw std::runtime_error("scene '" + def.id + "': placement 'followActor' cannot be combined with an 'anchor' requirement");
			}

			const bool hasStages = a_json.contains("stages");
			const bool hasClip = a_json.contains("clip");
			if (hasStages == hasClip) {
				throw std::runtime_error("node '" + def.id + "': specify exactly one of 'clip' or 'stages'");
			}
			size_t actorCount = rolesGiven ? def.roles.size() : 0;
			if (hasClip) {
				if (rolesGiven && actorCount != 1) {
					throw std::runtime_error("node '" + def.id + "': 'clip' requires exactly one role; use 'stages' for a cast");
				}
				StageDef stage;
				stage.loops = 1;
				stage.clips.push_back(ParseStageClip(a_json.at("clip"), clipRoot, "node '" + def.id + "'"));
				def.stages.push_back(std::move(stage));
				actorCount = 1;
			} else {
				def.stages = ParseOsfStageList(a_json.at("stages"), "node '" + def.id + "'", actorCount, rolesGiven, clipRoot);
			}
			if (!rolesGiven) def.roles.assign(actorCount, SceneRole{});
			ParseNodeTimeline(a_json, def, a_propRegistry);
			ValidateRoleRefs(def);
			// A node camera track overrides the file default. A string is shorthand for enter.
			if (const auto camera = a_json.find("camera"); camera != a_json.end() && camera->is_string()) {
				const auto state = ParseCameraState(camera->get<std::string>());
				if (!state) throw std::runtime_error("node '" + def.id + "': unknown camera state");
				a_cameraDefault = *state == CameraState::kNone ? std::optional<CameraState>{} : state;
			}
			if (def.cameras.empty() && a_cameraDefault &&
				(!a_json.contains("camera") || a_json.at("camera").is_string())) {
				CameraEntry camera;
				camera.state = *a_cameraDefault;
				def.cameras.push_back(std::move(camera));
			}
			def.species = Util::SpeciesFromAnimPath(def.stages.front().clips.front().file);
			if (def.species.empty()) def.species = "human";
			return def;
		}

		struct OsfFileDefaults
		{
			bool                       library = false;
			std::string                packName;
			std::string                folder;
			ContentFileDefaults        scene;
			std::string                clipRoot;
			std::optional<CameraState> camera = CameraState::kSceneOrbit;
		};

		struct OsfFileContents
		{
			std::vector<const json*> nodeJsons;
			std::vector<SceneRole>   fileRoles;
			RoleRegistry             roles;
			PropRegistry             props;
			AnchorReq                anchor;
		};

		struct FileRejector
		{
			const std::filesystem::path& file;
			ProblemSink&                 problems;

			void operator()(const std::string& a_what, std::string a_code = "file-invalid",
				std::string a_hint = "Fix the named file field, then reload packs.") const
			{
				problems.Push("[error] " + a_what, file, std::move(a_code), std::move(a_hint));
				REX::ERROR("[Registry] {} — skipped", a_what);
			}
		};

		bool ParseOsfFileDefaults(const json& a_json, const std::string& a_fileName,
			const FileRejector& a_reject, OsfFileDefaults& a_out)
		{
			if (a_json.contains("allowManualHold")) {
				a_reject("'" + a_fileName + "': retired key 'allowManualHold'; holding is caller navigation policy");
				return false;
			}
			const auto schemaIt = a_json.find("schema");
			if (schemaIt == a_json.end() || !schemaIt->is_number_integer()) {
				a_reject("'" + a_fileName + "': missing/non-integer 'schema'");
				return false;
			}
			const auto schema = schemaIt->get<std::int64_t>();
			if (schema != kSchemaVersion) {
				a_reject("'" + a_fileName + "': *.osf.json schema " + std::to_string(schema) +
					" unsupported (expected " + std::to_string(kSchemaVersion) + ")");
				return false;
			}

			if (const auto sectionIt = a_json.find("section"); sectionIt != a_json.end()) {
				const std::string section = sectionIt->is_string()
				                              ? ToLower(sectionIt->get<std::string>())
				                              : std::string{};
				if (section == "library") {
					a_out.library = true;
				} else {
					a_reject("'" + a_fileName + "': unknown 'section' value (supported: 'library')");
					return false;
				}
			}

			if (const auto packIt = a_json.find("pack"); packIt != a_json.end()) {
				if (!packIt->is_string()) {
					a_reject("'" + a_fileName + "': 'pack' must be a string");
					return false;
				}
				a_out.packName = packIt->get<std::string>();
			}
			if (const auto folderIt = a_json.find("folder"); folderIt != a_json.end()) {
				try {
					a_out.folder = ParseCatalogFolder(*folderIt, "'" + a_fileName + "'");
				} catch (const std::exception& e) {
					a_reject(e.what());
					return false;
				}
			}

			for (const char* key : { "clearHeldItems", "fade", "unlisted" }) {
				if (const auto value = a_json.find(key); value != a_json.end() && !value->is_boolean()) {
					a_reject("'" + a_fileName + "': '" + std::string(key) + "' must be true or false");
					return false;
				}
			}
			const std::string fileContext = "'" + a_fileName + "'";
			try {
				a_out.scene.playerInputLock = ReadContentBool(a_json, "playerInputLock", "lockPlayer", true, fileContext);
				a_out.scene.hideApparel = ReadContentBool(a_json, "hideApparel", "stripActors", !a_out.library, fileContext);
				a_out.scene.worldPlacement = ReadWorldPlacement(a_json,
					Animation::WorldPlacementMode::kAnchorAndPin, fileContext);
			} catch (const std::exception& e) {
				a_reject(e.what());
				return false;
			}
			a_out.scene.name = a_json.value("name", std::string{});
			a_out.scene.clearHeldItems = a_json.value("clearHeldItems", true);
			a_out.scene.fade = a_json.value("fade", false);
			a_out.scene.unlisted = a_json.value("unlisted", false);
			try {
				if (const json* controls = ReadContentKey(a_json, "sceneControls", "playerControl", fileContext)) {
					ApplySceneControls(*controls, a_out.scene.sceneControls, fileContext,
						"sceneControls");
				}
			} catch (const std::exception& e) {
				a_reject(e.what());
				return false;
			}

			if (const auto priorityIt = a_json.find("priority"); priorityIt != a_json.end()) {
				if (!priorityIt->is_number_integer()) {
					a_reject("'" + a_fileName + "': 'priority' must be an integer");
					return false;
				}
				a_out.scene.priority = priorityIt->get<std::int32_t>();
			}
			if (const auto weightIt = a_json.find("weight"); weightIt != a_json.end()) {
				const auto weight = weightIt->is_number_integer() ? weightIt->get<std::int64_t>() : 0;
				if (weight < 1 || weight > 1000000) {
					a_reject("'" + a_fileName + "': 'weight' must be an integer in [1, 1000000]");
					return false;
				}
				a_out.scene.weight = static_cast<std::int32_t>(weight);
			}
			if (const auto tagsIt = a_json.find("tags"); tagsIt != a_json.end()) {
				if (!tagsIt->is_array()) {
					a_reject("'" + a_fileName + "': 'tags' must be an array of strings");
					return false;
				}
				for (const auto& tag : *tagsIt) {
					if (!tag.is_string()) {
						a_reject("'" + a_fileName + "': 'tags' entries must be strings");
						return false;
					}
					a_out.scene.tags.push_back(tag.get<std::string>());
				}
			}
			if (const auto rootIt = a_json.find("clipRoot"); rootIt != a_json.end()) {
				if (!rootIt->is_string()) {
					a_reject("'" + a_fileName + "': 'clipRoot' must be a string");
					return false;
				}
				try {
					a_out.clipRoot = NormalizeClipRoot(rootIt->get<std::string>(), "'" + a_fileName + "'");
				} catch (const std::exception& e) {
					a_reject(e.what());
					return false;
				}
			}

			if (const auto cameraIt = a_json.find("camera"); cameraIt != a_json.end() &&
				!(a_json.contains("id") && cameraIt->is_array())) {
				if (!cameraIt->is_string()) {
					a_reject("'" + a_fileName + "': 'camera' must be a string");
					return false;
				}
				const auto parsed = ParseCameraState(cameraIt->get<std::string>());
				if (!parsed) {
					a_reject("'" + a_fileName + "': unknown camera state '" + cameraIt->get<std::string>() +
						"' (supported: 'thirdperson_hold', 'freefly', 'vanity_orbit', 'scene_orbit', 'none')");
					return false;
				}
				a_out.camera = *parsed == CameraState::kNone ? std::optional<CameraState>{} : parsed;
			}
			return true;
		}

		bool ParseOsfFileContents(const json& a_json, const std::string& a_fileName,
			const FileRejector& a_reject, OsfFileContents& a_out)
		{
			if (const auto propsIt = a_json.find("props"); propsIt != a_json.end()) {
				if (!propsIt->is_object()) {
					a_reject("'" + a_fileName + "': 'props' must be an object of prop templates keyed by id");
					return false;
				}
				if (propsIt->size() > kMaxPropsPerFile) {
					a_reject("'" + a_fileName + "': file-level props exceed the " +
						std::to_string(kMaxPropsPerFile) + "-entry limit");
					return false;
				}
				for (const auto& [propId, propJson] : propsIt->items()) {
					if (propId.empty() || !propJson.is_object()) {
						a_reject("'" + a_fileName + "': props registry entry '" + propId +
							"': definitions must be prop objects keyed by a non-empty id");
						return false;
					}
					try {
						ValidatePropTemplate(propJson, "props template '" + propId + "'");
					} catch (const std::exception& e) {
						a_reject("'" + a_fileName + "': " + std::string(e.what()));
						return false;
					}
					a_out.props.emplace(propId, propJson);
				}
			}

			for (const char* key : { "scenes", "entry", "linearStages", "use" }) {
				if (a_json.contains(key)) {
					a_reject("'" + a_fileName + "': retired key '" + key + "'; use schema 2 global nodes");
					return false;
				}
			}
			if (a_json.contains("nodes") && (a_json.contains("id") || a_json.contains("clip") || a_json.contains("stages"))) {
				a_reject("'" + a_fileName + "': use a nodes array or a bare node, not both");
				return false;
			}
			if (const auto scenesIt = a_json.find("nodes"); scenesIt != a_json.end()) {
				if (!scenesIt->is_array()) {
					a_reject("'" + a_fileName + "': 'nodes' must be an array");
					return false;
				}
				if (scenesIt->size() > kMaxNodesPerFile) {
					a_reject("'" + a_fileName + "': contains more than " +
						std::to_string(kMaxNodesPerFile) + " scenes");
					return false;
				}
				try {
					a_out.anchor = ParseAnchorBlock(a_json, "'" + a_fileName + "' file-level anchor");
				} catch (const std::exception& e) {
					a_reject(e.what());
					return false;
				}
				if (const auto rolesIt = a_json.find("roles"); rolesIt != a_json.end()) {
					if ((rolesIt->is_array() || rolesIt->is_object()) && rolesIt->size() > kMaxRolesPerFile) {
						a_reject("'" + a_fileName + "': file-level roles exceed the " +
							std::to_string(kMaxRolesPerFile) + "-entry limit");
						return false;
					}
					if (rolesIt->is_array()) {
						try {
							for (const auto& roleJson : *rolesIt) {
								a_out.fileRoles.push_back(ParseRole(roleJson, "<file:" + a_fileName + ">"));
							}
						} catch (const std::exception& e) {
							a_reject("'" + a_fileName + "': file-level roles: " + std::string(e.what()));
							return false;
						}
					} else if (rolesIt->is_object()) {
						for (const auto& [roleId, roleJson] : rolesIt->items()) {
							if (roleId.empty() || !roleJson.is_object()) {
								a_reject("'" + a_fileName + "': roles registry entry '" + roleId +
									"': definitions must be role objects keyed by a non-empty id");
								return false;
							}
							try {
								RoleTemplate role;
								role.raw = roleJson;
								role.parsed = ParseRole(roleJson, "<file:" + a_fileName + ">");
								if (!roleJson.contains("name")) {
									role.parsed.name = roleId;
									role.raw["name"] = roleId;
								}
								a_out.roles.emplace(roleId, std::move(role));
							} catch (const std::exception& e) {
								a_reject("'" + a_fileName + "': roles registry entry '" + roleId +
									"': " + std::string(e.what()));
								return false;
							}
						}
					} else {
						a_reject("'" + a_fileName +
							"': file-level 'roles' must be an array (default cast) or an object (roles registry)");
						return false;
					}
				}
				for (const auto& scene : *scenesIt) {
					if (scene.is_object() && scene.contains("props")) {
						const auto idIt = scene.find("id");
						const std::string id = idIt != scene.end() && idIt->is_string()
						                         ? idIt->get<std::string>()
						                         : std::string{};
						a_reject("'" + a_fileName + "': scene '" + id +
							"': 'props' is a file-level registry — move it beside 'nodes'");
						return false;
					}
					a_out.nodeJsons.push_back(&scene);
				}
			} else if (a_json.contains("id")) {
				a_out.nodeJsons.push_back(&a_json);
			} else if (!a_json.contains("clipLibrary")) {
				a_reject("'" + a_fileName + "': expected a bare node 'id', 'nodes', or 'clipLibrary'");
				return false;
			}
			return true;
		}

		bool AppendClipLibrary(const json& a_json, const std::filesystem::path& a_file,
			const OsfFileDefaults& a_defaults, NodeLoadBudget& a_budget,
			std::vector<ClipLibraryRegistration>& a_out, const FileRejector& a_reject)
		{
			try {
				auto registrations = ParseClipLibrary(a_json, a_file, a_defaults.packName,
					a_defaults.clipRoot, a_defaults.folder);
				if (registrations.size() > kMaxClipLibraryEntriesTotal - a_budget.clipLibraryEntries) {
					a_reject("'" + a_file.filename().string() +
						"': aggregate clipLibrary entry limit would be exceeded");
					return false;
				}
				const auto count = registrations.size();
				a_out.insert(a_out.end(), std::make_move_iterator(registrations.begin()),
					std::make_move_iterator(registrations.end()));
				a_budget.clipLibraryEntries += count;
				return true;
			} catch (const std::exception& e) {
				a_reject(e.what());
				return false;
			}
		}

		void LoadOsfNodes(const OsfFileContents& a_contents, const OsfFileDefaults& a_defaults,
			const std::filesystem::path& a_file, std::unordered_map<std::string, NodeDef>& a_out,
			NodeLoadBudget& a_budget, ProblemSink& a_problems)
		{
			const std::string fileName = a_file.filename().string();
			for (const auto* nodeJson : a_contents.nodeJsons) {
				const std::string authoredId = nodeJson->is_object() && nodeJson->contains("id") &&
					(*nodeJson)["id"].is_string() ? (*nodeJson)["id"].get<std::string>() : std::string{};
				try {
					auto definition = ParseNodeDefinition(*nodeJson, a_defaults.scene, a_defaults.camera,
						a_contents.fileRoles, a_contents.roles, a_contents.props,
						a_defaults.clipRoot, a_contents.anchor);
					const std::size_t stageCount = definition.stages.size();
					std::size_t clipCount = 0;
					for (const auto& stage : definition.stages) clipCount += stage.clips.size();
					if (stageCount > kMaxStagesPerNode || clipCount > kMaxClipsPerNode) {
						throw std::runtime_error("scene '" + definition.id + "': stage/clip limit exceeded");
					}
					definition.sourceFile = a_file;
					definition.pack = nodeJson->value("pack", a_defaults.packName);
					definition.folder = nodeJson->contains("folder") ?
						ParseCatalogFolder(nodeJson->at("folder"), "node '" + definition.id + "'") : a_defaults.folder;
					definition.library = a_defaults.library;
					definition.sourceKind = a_defaults.library ? CatalogSourceKind::kReferenceAnimation :
						CatalogSourceKind::kAuthoredScene;
					auto key = ToLower(definition.id);
					if (const auto found = a_out.find(key); found != a_out.end()) {
						a_problems.Push("[error] duplicate scene id '" + definition.id + "' in '" + fileName +
							"' (already from '" + found->second.sourceFile.filename().string() + "') — keeping the first",
							a_file, "duplicate-scene-id",
							"Rename this scene id or remove the duplicate; OSF keeps the first definition.", definition.id);
						REX::ERROR("[Registry] duplicate scene id '{}' in '{}' — keeping first from '{}'",
							definition.id, fileName, found->second.sourceFile.filename().string());
						continue;
					}
					if (a_budget.nodes >= kMaxNodesTotal ||
						stageCount > kMaxStagesTotal - a_budget.stages ||
						clipCount > kMaxClipsTotal - a_budget.clips) {
						a_problems.Push("[error] scene registry aggregate scene/node/stage/clip limit reached — remaining scenes skipped",
							a_file, "registry-limit",
							"Reduce the installed scene content below the registry limits, then reload packs.", definition.id);
						REX::ERROR("[Registry] aggregate scene/node/stage/clip limit reached — remaining scenes skipped");
						break;
					}
					REX::DEBUG("[Registry] loaded node '{}' from '{}'", definition.id, fileName);
					a_out.emplace(std::move(key), std::move(definition));
					++a_budget.nodes;
					a_budget.stages += stageCount;
					a_budget.clips += clipCount;
				} catch (const std::exception& e) {
					a_problems.Push("[error] '" + fileName + "': " + e.what(), a_file, "scene-invalid",
						"Fix the named scene field; only this scene was skipped.", authoredId);
					REX::ERROR("[Registry] skipping scene in '{}': {}", fileName, e.what());
				}
			}
		}

		void LoadOsfFile(const json& a_json, const std::filesystem::path& a_file,
			std::unordered_map<std::string, NodeDef>& a_out,
			std::vector<ClipLibraryRegistration>& a_clipLibrary,
			NodeLoadBudget& a_budget, ProblemSink& a_problems)
		{
			const FileRejector reject{ a_file, a_problems };
			OsfFileDefaults defaults;
			if (!ParseOsfFileDefaults(a_json, a_file.filename().string(), reject, defaults)) {
				return;
			}

			OsfFileContents contents;
			if (!ParseOsfFileContents(a_json, a_file.filename().string(), reject, contents) ||
				!AppendClipLibrary(a_json, a_file, defaults, a_budget, a_clipLibrary, reject)) {
				return;
			}
			LoadOsfNodes(contents, defaults, a_file, a_out, a_budget, a_problems);
		}

		// Validate the complete publication, including cross-file routes. A missing destination
		// rejects its source and every source that depends on it; cycles of valid nodes survive.
		void ValidateEdgeTargets(std::unordered_map<std::string, NodeDef>& a_nodes, ProblemSink& a_problems)
		{
			std::unordered_map<std::string, std::vector<std::string>> incoming;
			std::map<std::string, std::string> invalid;
			std::vector<std::string> pending;
			for (const auto& [id, node] : a_nodes) {
				for (const auto& edge : node.edges) {
					if (edge.to == "$end") continue;
					const auto target = ToLower(edge.to);
					incoming[target].push_back(id);
					if (!a_nodes.contains(target) && invalid.try_emplace(id, edge.to).second) pending.push_back(id);
				}
			}
			std::sort(pending.begin(), pending.end());
			for (auto& [id, sources] : incoming) std::sort(sources.begin(), sources.end());
			for (size_t i = 0; i < pending.size(); ++i) {
				const auto failed = pending[i];
				for (const auto& source : incoming[failed]) {
					if (invalid.try_emplace(source, a_nodes.at(failed).id).second) pending.push_back(source);
				}
			}
			for (const auto& [id, target] : invalid) {
				const auto& node = a_nodes.at(id);
				a_problems.Push("[error] node '" + node.id + "': edge targets missing or rejected node '" + target + "'",
					node.sourceFile, "unknown-node-reference", "Fix the edge destination or install its node definition.", node.id);
				REX::ERROR("[Registry] node '{}' rejected: unavailable edge destination '{}'", node.id, target);
				a_nodes.erase(id);
			}
		}

		// A per-clip offset overrides the role's placement; every stage's clip count must equal a_actorCount.
		std::optional<Animation::PlaybackPlan> BuildPlanFromStages(const std::string& a_id,
			const std::vector<SceneRole>& a_roles, const std::vector<StageDef>& a_stages, size_t a_actorCount)
		{
			if (a_stages.empty()) {
				return std::nullopt;
			}
			if (a_roles.size() != a_actorCount) {
				REX::WARN("[Registry] '{}' needs {} role(s), got {}", a_id, a_roles.size(), a_actorCount);
				return std::nullopt;
			}
			Animation::PlaybackPlan plan;
			plan.animId = a_id;
			plan.preserveBones.reserve(a_roles.size());
			plan.poseModes.reserve(a_roles.size());
			plan.poseWeights.reserve(a_roles.size());
			plan.masks.reserve(a_roles.size());
			plan.roleNames.reserve(a_roles.size());
			for (const auto& role : a_roles) {
				plan.preserveBones.push_back(role.preserveBones);
				plan.poseModes.push_back(role.poseMode);
				plan.poseWeights.push_back(role.poseWeight);
				plan.masks.push_back(role.mask);
				plan.roleNames.push_back(role.name);
			}
			plan.stages.reserve(a_stages.size());
			for (const auto& sd : a_stages) {
				if (sd.clips.size() != a_actorCount) {
					REX::WARN("[Registry] '{}' stage has {} clip(s) but {} role(s)", a_id, sd.clips.size(), a_actorCount);
					return std::nullopt;
				}
				Animation::PlaybackPlan::Segment stage;
				stage.timer = sd.timer;
				stage.loops = sd.loops;
				stage.hold = sd.hold;
				for (size_t a = 0; a < a_actorCount; a++) {
					const auto& clip = sd.clips[a];
					stage.files.push_back(clip.file);
					stage.animIds.push_back(clip.animId);
					stage.placements.push_back(clip.offset.value_or(a_roles[a].offset));
					stage.masks.push_back(clip.mask.value_or(a_roles[a].mask));
				}
				plan.stages.push_back(std::move(stage));
			}
			return plan;
		}
	}

	RoleGender ParseRoleGender(std::string_view a_str)
	{
		const auto s = ToLower(a_str);
		if (s == "male" || s == "m") {
			return RoleGender::kMale;
		}
		if (s == "female" || s == "f") {
			return RoleGender::kFemale;
		}
		return RoleGender::kAny;
	}

	std::optional<CameraState> ParseCameraState(std::string_view a_state)
	{
		const auto state = ToLower(a_state);
		if (state == "none") {
			return CameraState::kNone;
		}
		if (state == "thirdperson_hold") {
			return CameraState::kThirdPersonHold;
		}
		if (state == "freefly") {
			return CameraState::kFreeFly;
		}
		if (state == "vanity_orbit") {
			return CameraState::kVanityOrbit;
		}
		if (state == "scene_orbit") {
			return CameraState::kSceneOrbit;
		}
		return std::nullopt;
	}

	std::string_view CameraStateName(CameraState a_state)
	{
		switch (a_state) {
		case CameraState::kNone: return "none";
		case CameraState::kThirdPersonHold: return "thirdperson_hold";
		case CameraState::kFreeFly: return "freefly";
		case CameraState::kVanityOrbit: return "vanity_orbit";
		case CameraState::kSceneOrbit: return "scene_orbit";
		}
		return "none";
	}

	ContentRegistry& ContentRegistry::GetSingleton()
	{
		static ContentRegistry singleton;
		return singleton;
	}

	void ContentRegistry::LoadAll()
	{
		OSF_PROFILE_SCOPE("Registry.LoadAll");
		std::unordered_map<std::string, NodeDef> loaded;
		std::vector<std::string> errors;
		std::vector<PendingImportProblem> pendingProblems;
		ProblemSink problems{ errors, pendingProblems };
		std::vector<ClipLibraryRegistration> clipLibrary;
		std::vector<ContentFileStats> fileStats;
		std::unordered_map<std::string, std::size_t> fileIndex;  // full source path -> fileStats index
		ClipInstalledCache clipCache;
		NodeLoadBudget loadBudget;

		auto ensureFileStats = [&](const Util::RegistryJsonSource& a_source) -> ContentFileStats& {
			if (const auto found = fileIndex.find(a_source.file.string()); found != fileIndex.end()) {
				return fileStats[found->second];
			}
			auto& stats = fileStats.emplace_back();
			fileIndex.emplace(a_source.file.string(), fileStats.size() - 1);
			stats.file = a_source.file.filename().string();
			const auto relative = a_source.file.lexically_relative(a_source.root);
			stats.path = relative.empty() ? stats.file : relative.generic_string();
			stats.bytes = a_source.bytes;
			return stats;
		};

		Util::ForEachRegistryJson(".osf.json",
			[](std::ifstream& a_input) {
				OSF_PROFILE_SCOPE("Registry.LoadAll.ParseJson");
				return nlohmann::json::parse(a_input, nullptr, true, true);
			},
			[&](const Util::RegistryJsonSource& a_source, const nlohmann::json& a_json) {
				OSF_PROFILE_SCOPE("Registry.LoadAll.File");
				auto& stats = ensureFileStats(a_source);
				if (const auto scenes = a_json.find("nodes"); scenes != a_json.end() && scenes->is_array()) {
					stats.declaredNodes = static_cast<std::uint32_t>(scenes->size());
				} else if (a_json.contains("id")) {
					stats.declaredNodes = 1;
				}
				// Keep lenient header metadata even when every declared scene is later rejected.
				if (const auto schema = a_json.find("schema"); schema != a_json.end() && schema->is_number_integer()) {
					stats.schema = schema->get<std::int64_t>();
				}
				if (const auto pack = a_json.find("pack"); pack != a_json.end() && pack->is_string()) {
					stats.pack = pack->get<std::string>();
				}
				if (const auto section = a_json.find("section"); section != a_json.end() && section->is_string()) {
					stats.library = ToLower(section->get<std::string>()) == "library";
				}
				LoadOsfFile(a_json, a_source.file, loaded, clipLibrary, loadBudget, problems);
				stats.parseMs = std::chrono::duration<float, std::milli>(
					std::chrono::steady_clock::now() - a_source.begun).count();
			},
			[&](Util::RegistryJsonProblemKind a_kind, const Util::RegistryJsonSource* a_source,
				const std::string& a_message) {
				if (a_kind != Util::RegistryJsonProblemKind::kFile || !a_source) {
					problems.Push("[error] scene discovery: " + a_message, {}, "discovery-failed",
						"Restore access to the named Data/OSF path, then reload packs.");
					if (a_kind == Util::RegistryJsonProblemKind::kGameDirectory) {
						REX::ERROR("[Registry] {}", a_message);
					} else {
						REX::ERROR("[Registry] discovery: {}", a_message);
					}
					return;
				}
				auto& stats = ensureFileStats(*a_source);
				const bool unknown = a_message == "unknown exception";
				const std::string detail = unknown
					? "parse failed with an unknown exception"
					: "parse failed: " + a_message;
				problems.Push("[error] '" + stats.file + "': " + detail, a_source->file,
					"parse-failed", unknown ? "Validate this file as JSON, then reload packs."
					                              : "Fix the JSON syntax near the reported byte or field, then reload packs.");
				if (unknown) {
					REX::ERROR("[Registry] failed to parse '{}' with an unknown exception", stats.file);
				} else {
					REX::ERROR("[Registry] failed to parse '{}': {}", stats.file, a_message);
				}
				stats.parseMs = std::chrono::duration<float, std::milli>(
					std::chrono::steady_clock::now() - a_source->begun).count();
			});

		{
			// One installed probe (BSResource opens) per distinct clip spec.
			OSF_PROFILE_SCOPE("Registry.LoadAll.SweepClips");
			SweepClipAvailability(loaded, problems, clipCache);
		}
		const auto sceneCount = loaded.size();
		{
			OSF_PROFILE_SCOPE("Registry.LoadAll.FileStats");
			AccumulateFileStats(loaded, clipCache, fileStats, fileIndex);
		}
		std::map<std::string, std::uint32_t> clipEntriesByFile;
		const auto clipEntryCount = OSF_PROFILE_CALL("Registry.LoadAll.ClipEntries",
			AddRegisteredClipEntries(loaded, clipLibrary, problems, clipEntriesByFile, clipCache));
		for (const auto& [path, count] : clipEntriesByFile) {
			if (const auto it = fileIndex.find(path); it != fileIndex.end()) {
				fileStats[it->second].clipEntries = count;
			}
		}
		for (auto& stats : fileStats) {
			stats.rejectedNodes = stats.declaredNodes > stats.nodes ? stats.declaredNodes - stats.nodes : 0;
		}


		// Unowned problems land in one trailing bucket.
		ContentFileStats crossFile;
		for (std::size_t i = 0; i < errors.size() && i < pendingProblems.size(); ++i) {
			const auto& pending = pendingProblems[i];
			const auto it = fileIndex.find(pending.owner.string());
			auto& target = it != fileIndex.end() ? fileStats[it->second] : crossFile;
			(pending.warning ? target.warnings : target.errors) += 1;
			target.problems.push_back(ContentImportProblem{
				pending.warning, pending.code, errors[i], pending.hint, pending.node,
				pending.role, pending.clip
			});
		}
		const auto fileCount = fileStats.size();  // before the bucket, which is not a file
		if (!crossFile.problems.empty()) {
			fileStats.push_back(std::move(crossFile));  // empty path; must stay last
		}

		auto next = std::make_shared<ContentRegistrySnapshot>();
		next->nodes = std::move(loaded);
		next->loadErrors = std::move(errors);
		next->authoredNodeCount = sceneCount;
		next->files = std::move(fileStats);
		// Retain per-clip availability as source metadata, including installed clips in a
		// partially unavailable scene. Catalog reads and provider callbacks perform no IO.
		for (const auto& [id, def] : next->nodes)
			for (const auto& stage : def.stages) for (const auto& clip : stage.clips) {
				if (!clipCache.contains(clip.file)) clipCache.emplace(clip.file, ClipSpecInstalled(clip.file));
			}
		next->clipAvailability = std::move(clipCache);
		{
			OSF_PROFILE_SCOPE("Registry.LoadAll.Publish");
			std::lock_guard lock(providerMutex);
			baseFileIndex = std::move(fileIndex);
			baseSnapshot = std::move(next);
			PublishProviders();
		}
		const auto published = ReadSnapshot();
		REX::INFO("[Registry] {} node(s) loaded from {} file(s), {} clip entr{}, {} problem(s)",
			published->authoredNodeCount, fileCount, clipEntryCount, clipEntryCount == 1 ? "y" : "ies", published->loadErrors.size());
	}


	void ContentRegistry::FinalizePublication(ContentRegistrySnapshot& a_next)
	{
		// Providers see immutable base candidates. Validate only once their output is present,
		// so registration/removal can restore/reject routes without reparsing or clip IO.
		std::vector<std::string> errors;
		std::vector<PendingImportProblem> pending;
		ProblemSink problems{errors, pending};
		ValidateEdgeTargets(a_next.nodes, problems);
		for (auto& stats : a_next.files) {
			stats.hidden = stats.unlisted = stats.anchored = stats.nodes = stats.stages = 0;
			stats.roles = stats.clips = stats.cues = stats.actions = stats.sounds = stats.cameras = 0;
		}
		AccumulateFileStats(a_next.nodes, a_next.clipAvailability, a_next.files, baseFileIndex);
		a_next.authoredNodeCount = 0;
		for (auto& stats : a_next.files) {
			stats.rejectedNodes = stats.declaredNodes > stats.nodes ? stats.declaredNodes - stats.nodes : 0;
			a_next.authoredNodeCount += stats.nodes;
		}
		for (std::size_t i = 0; i < pending.size(); ++i) {
			const auto& p = pending[i];
			const auto it = baseFileIndex.find(p.owner.string());
			if (it == baseFileIndex.end() && (a_next.files.empty() || !a_next.files.back().path.empty()))
				a_next.files.emplace_back();
			auto& stats = it != baseFileIndex.end() ? a_next.files[it->second] : a_next.files.back();
			(p.warning ? stats.warnings : stats.errors) += 1;
			stats.problems.push_back({p.warning, p.code, errors[i], p.hint, p.node, p.role, p.clip});
		}
		a_next.loadErrors.insert(a_next.loadErrors.end(), errors.begin(), errors.end());
		std::sort(a_next.files.begin(), a_next.files.end(), [](const auto& lhs, const auto& rhs) {
			if (lhs.path.empty() != rhs.path.empty()) return !lhs.path.empty();
			return lhs.path < rhs.path;
		});
	}

	NodeRef ContentRegistry::Find(std::string_view a_id, const NodeRef& a_publication) const
	{
		NodeRef out;
		out.owner = a_publication.owner ? a_publication.owner : snapshot.load(std::memory_order_acquire);
		const auto it = out.owner->nodes.find(ToLower(std::string(a_id)));
		if (it != out.owner->nodes.end()) {
			out.value = &it->second;
		}
		return out;
	}

	NodeRef ContentRegistry::ResolveLoopGlobals(const NodeRef& a_def, const GlobalReader& a_readGlobal) const
	{
		if (!a_def) return {};
		const bool hasGlobals = !a_def->loopGlobal.empty() || std::ranges::any_of(a_def->stages,
			[](const auto& stage) { return !stage.loopGlobal.empty(); });
		if (!hasGlobals) return a_def;

		std::unordered_map<std::string, std::optional<std::int32_t>> values;
		const auto resolve = [&](std::int32_t fallback, const std::string& ref) {
			if (ref.empty()) return fallback;
			auto [it, fresh] = values.try_emplace(ToLower(ref));
			if (fresh) {
				std::optional<float> value;
				if (a_readGlobal) value = a_readGlobal(ref);
				else if (const auto* global = Util::ResolveFormRef<RE::TESGlobal>(ref)) value = global->GetValue();
				if (value && std::isfinite(*value) && *value >= 0 &&
					static_cast<double>(*value) <= std::numeric_limits<std::int32_t>::max() && std::trunc(*value) == *value) {
					it->second = static_cast<std::int32_t>(*value);
				} else {
					REX::DEBUG("[Scene] '{}' loop global '{}' unavailable or not a valid count; using authored defaults", a_def->id, ref);
				}
			}
			return it->second.value_or(fallback);
		};
		// Timing values belong to this admission. Edges still resolve in the same immutable publication.
		auto resolved = std::make_shared<NodeDef>(*a_def);
		if (!resolved->loopGlobal.empty()) {
			resolved->loopCount = resolve(resolved->loopCount, resolved->loopGlobal);
			resolved->loopMode = resolved->loopCount == 0 ? LoopMode::kHold : LoopMode::kCount;
			resolved->loopGlobal.clear();
		}
		for (auto& stage : resolved->stages) {
			stage.loops = resolve(stage.loops, stage.loopGlobal);
			stage.loopGlobal.clear();
		}
		NodeRef out;
		out.owner = a_def.owner;
		out.resolved = std::move(resolved);
		out.value = out.resolved.get();
		return out;
	}

	std::optional<Animation::PlaybackPlan> ContentRegistry::BuildNodePlan(const NodeRef& a_def, size_t a_actorCount) const
	{
		if (!a_def) return std::nullopt;
		auto plan = BuildPlanFromStages(a_def->id, a_def->roles, a_def->stages, a_actorCount);
		if (plan) plan->SetWorldPlacementMode(a_def->worldPlacement);
		return plan;
	}

	void ContentRegistry::ForEachDef(const std::function<void(const NodeDef&)>& a_fn) const
	{
		const auto current = snapshot.load(std::memory_order_acquire);
		for (const auto& [key, def] : current->nodes) {
			a_fn(def);
		}
	}

	size_t ContentRegistry::Size() const
	{
		return snapshot.load(std::memory_order_acquire)->authoredNodeCount;
	}

	std::vector<std::string> ContentRegistry::LoadErrors() const
	{
		return snapshot.load(std::memory_order_acquire)->loadErrors;
	}

	std::vector<ContentFileStats> ContentRegistry::FileStats() const
	{
		return snapshot.load(std::memory_order_acquire)->files;
	}

	std::vector<std::string> ContentRegistry::MissingClipRefs() const
	{
		// The snapshot already holds the load-time BSResource-aware probe for every clip (the same view the
		// catalog and matchmaking use); probe only clips it doesn't know.
		const auto current = snapshot.load(std::memory_order_acquire);
		std::unordered_map<std::string, bool> cache;
		const auto installed = [&cache, &current](const std::string& a_file) {
			if (const auto known = current->clipAvailability.find(a_file); known != current->clipAvailability.end()) {
				return known->second;
			}
			auto [it, fresh] = cache.try_emplace(a_file, false);
			if (fresh) {
				it->second = ClipSpecInstalled(a_file);
			}
			return it->second;
		};

		std::vector<std::string> out;
		for (const auto& [key, def] : current->nodes) {
			for (const auto& stage : def.stages) {
				for (const auto& clip : stage.clips) {
					if (!installed(clip.file)) {
						out.push_back("node '" + def.id + "': missing clip '" + clip.file +
							(clip.animId.empty() ? "" : (":" + clip.animId)) + "'");
					}
				}
			}
		}
		return out;
	}
}
