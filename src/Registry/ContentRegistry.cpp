#include "Registry/ContentRegistry.h"
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
		using ContentRegistryClips::AddSceneClipEntries;
		using ContentRegistryClips::ClipSpecInstalled;
		using ContentRegistryClips::DesugarLinear;
		using ContentRegistryClips::SweepClipAvailability;

		constexpr std::size_t kMaxScenesPerFile = 4096;
		constexpr std::size_t kMaxRolesPerFile = 512;
		constexpr std::size_t kMaxPropsPerFile = 512;
		constexpr std::size_t kMaxClipLibraryEntriesPerFile = 65536;
		constexpr std::size_t kMaxNodesPerScene = 4096;
		constexpr std::size_t kMaxStagesPerScene = 16384;
		constexpr std::size_t kMaxClipsPerScene = 65536;
		constexpr std::size_t kMaxScenesTotal = 32768;
		constexpr std::size_t kMaxNodesTotal = 131072;
		constexpr std::size_t kMaxStagesTotal = 262144;
		constexpr std::size_t kMaxClipsTotal = 1048576;
		constexpr std::size_t kMaxClipLibraryEntriesTotal = 262144;

		struct SceneLoadBudget
		{
			std::size_t scenes = 0;
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
			const auto canonical = a_owner.find("attachmentNode");
			const auto legacy = a_owner.find("node");
			if (canonical != a_owner.end() && legacy != a_owner.end()) {
				throw std::runtime_error(a_subject + " uses both 'attachmentNode' and legacy 'node'");
			}
			const auto node = canonical != a_owner.end() ? canonical : legacy;
			if (node == a_owner.end() || !node->is_string() || node->get<std::string>().empty()) {
				throw std::runtime_error(a_subject + " requires a non-empty 'attachmentNode' (legacy: 'node')");
			}
			result.targetNode = node->get<std::string>();
			result.position = ParsePropVector(a_owner, "position", a_subject);
			result.rotation = ParsePropVector(a_owner, "rotation", a_subject);
			result.scale = ParsePropScale(a_owner, a_subject);
			return result;
		}

		// Strict subset of action keys: timing, `type`, `prop` and `role` are never inheritable.
		constexpr std::array<const char*, 6> kPropTemplateKeys{
			"source", "attachmentNode", "node", "position", "rotation", "scale"
		};
		constexpr std::array<const char*, 4> kPropMergeKeys{ "source", "position", "rotation", "scale" };

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
			if (a_templ.contains("attachmentNode") && a_templ.contains("node")) {
				throw std::runtime_error(a_subject + " uses both 'attachmentNode' and legacy 'node'");
			}
			const auto node = a_templ.contains("attachmentNode") ? a_templ.find("attachmentNode") : a_templ.find("node");
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
			// `attachmentNode`/`node` are one key: an inline value under either spelling wins.
			if (!a_merged_out.contains("attachmentNode") && !a_merged_out.contains("node")) {
				if (const auto canonical = templ->second.find("attachmentNode"); canonical != templ->second.end()) {
					a_merged_out["attachmentNode"] = *canonical;
				} else if (const auto legacy = templ->second.find("node"); legacy != templ->second.end()) {
					a_merged_out["node"] = *legacy;
				}
			}
			return a_merged_out;
		}

		void ParseActionTrack(const json& a_entries, SceneNode& a_node_out, const PropRegistry& a_props)
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
		void ExpandSoundLadder(const json& a_lane, SceneNode& a_node_out)
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

		void ParseSoundTrack(const json& a_entries, SceneNode& a_node_out)
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

		void ParseCameraTrack(const json& a_entries, SceneNode& a_node_out)
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
		void AssignRoleNames(SceneDef& a_def, std::vector<PendingRole>& a_pending)
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
			std::string_view a_key = "playerControl")
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

		const json* FindVocabularyKey(const json& a_object, std::string_view a_canonical,
			std::string_view a_legacy, const std::string& a_ctx)
		{
			const std::string canonicalKey{ a_canonical };
			const std::string legacyKey{ a_legacy };
			const auto canonical = a_object.find(canonicalKey);
			const auto legacy = a_object.find(legacyKey);
			if (canonical != a_object.end() && legacy != a_object.end()) {
				throw std::runtime_error(a_ctx + ": use either '" + std::string(a_canonical) +
					"' or legacy '" + std::string(a_legacy) + "', not both");
			}
			if (canonical != a_object.end()) {
				return &*canonical;
			}
			return legacy != a_object.end() ? &*legacy : nullptr;
		}

		bool ReadVocabularyBool(const json& a_object, std::string_view a_canonical,
			std::string_view a_legacy, bool a_default, const std::string& a_ctx)
		{
			const json* value = FindVocabularyKey(a_object, a_canonical, a_legacy, a_ctx);
			if (!value) {
				return a_default;
			}
			if (!value->is_boolean()) {
				throw std::runtime_error(a_ctx + ": '" + std::string(
					a_object.contains(std::string{ a_canonical }) ? a_canonical : a_legacy) + "' must be true or false");
			}
			return value->get<bool>();
		}

		Animation::WorldPlacementMode ReadWorldPlacement(const json& a_object,
			Animation::WorldPlacementMode a_default, const std::string& a_ctx)
		{
			const json* value = FindVocabularyKey(a_object, "placement", "inPlace", a_ctx);
			if (!value) {
				return a_default;
			}
			if (a_object.contains("inPlace")) {
				if (!value->is_boolean()) {
					throw std::runtime_error(a_ctx + ": legacy 'inPlace' must be true or false");
				}
				return Animation::WorldPlacementFromLegacyInPlace(value->get<bool>());
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

		void ParseSceneMeta(const json& a_json, SceneDef& def, const ContentFileDefaults& a_defaults)
		{
			const std::string sceneContext = "scene '" + def.id + "'";
			const Util::JsonView scene{ a_json, sceneContext };

			// priority stays on value(): it truncates floats (3.7 -> 3) where ReadInt would reject.
			def.name = a_json.value("name", def.id);
			def.priority = a_json.value("priority", a_defaults.priority);
			def.weight = a_defaults.weight;
			scene.ReadInt("weight", def.weight, 1, 1000000);
			def.unlisted = a_defaults.unlisted;
			scene.Read("unlisted", def.unlisted);
			def.playerInputLock = ReadVocabularyBool(a_json, "playerInputLock", "lockPlayer",
				a_defaults.playerInputLock, sceneContext);
			def.hideApparel = ReadVocabularyBool(a_json, "hideApparel", "stripActors",
				a_defaults.hideApparel, sceneContext);
			def.clearHeldItems = a_defaults.clearHeldItems;
			scene.Read("clearHeldItems", def.clearHeldItems);
			def.fade = a_defaults.fade;
			scene.Read("fade", def.fade);
			def.worldPlacement = ReadWorldPlacement(a_json, a_defaults.worldPlacement, sceneContext);
			def.sceneControls = a_defaults.sceneControls;
			if (const json* controls = FindVocabularyKey(a_json, "sceneControls", "playerControl", sceneContext)) {
				ApplySceneControls(*controls, def.sceneControls, sceneContext,
					a_json.contains("sceneControls") ? "sceneControls" : "playerControl");
			}
		}

		// '#' is reserved for synthetic desugar nodes.
		void RejectReservedId(const std::string& a_id, const char* a_what)
		{
			if (a_id.find('#') != std::string::npos) {
				throw std::runtime_error(std::string(a_what) + " id '" + a_id +
					"' may not contain '#' (reserved for synthetic stage nodes)");
			}
		}

		void ParseOsfCueLane(const json& a_entries, SceneNode& a_node);

		// a_ioActorCount is authoritative when a_fixed; otherwise the first stage's clip count sets it.
		std::vector<StageDef> ParseOsfStageList(const json& a_stages, const std::string& a_subject,
			size_t& a_ioActorCount, bool a_fixed, std::string_view a_clipRoot, const PropRegistry& a_props,
			bool a_allowStageLanes = true)
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
					info.timer = jStage.value("timer", 0.0f);
					info.loops = jStage.value("loops", 0);
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
					// The runtime never reads per-stage lanes inside a graph node, so reject rather than drop them.
					if (!a_allowStageLanes &&
						(jStage.contains("cue") || jStage.contains("action") ||
							jStage.contains("sound") || jStage.contains("camera"))) {
						throw std::runtime_error(a_subject + ": track lanes on a stage inside a graph node are not "
							"supported — put cue/action/sound/camera on the node itself");
					}
					{
						SceneNode scratch;
						scratch.id = a_subject;
						if (auto it = jStage.find("cue"); it != jStage.end()) {
							ParseOsfCueLane(*it, scratch);
						}
						if (auto it = jStage.find("action"); it != jStage.end()) {
							ParseActionTrack(*it, scratch, a_props);
						}
						if (auto it = jStage.find("sound"); it != jStage.end()) {
							ParseSoundTrack(*it, scratch);
						}
						if (auto it = jStage.find("camera"); it != jStage.end()) {
							ParseCameraTrack(*it, scratch);
						}
						info.cues = std::move(scratch.cues);
						info.actions = std::move(scratch.actions);
						info.sounds = std::move(scratch.sounds);
						info.cameras = std::move(scratch.cameras);
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

		void ParseOsfCueLane(const json& a_entries, SceneNode& a_node)
		{
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node.id + "': 'cue' track must be an array");
			}
			for (const auto& c : a_entries) {
				CueEntry ce;
				ce.id = c.value("id", std::string{});
				if (ce.id.empty()) {
					throw std::runtime_error("node '" + a_node.id + "': a cue track entry is missing 'id'");
				}
				ParseTrackTiming(c, ce, a_node.id, "cue '" + ce.id + "'", /*a_atRequired*/ true);
				a_node.cues.push_back(std::move(ce));
			}
		}

		// `use` XOR inline `stages`; track lanes are flat node keys.
		SceneNode ParseOsfNode(const json& a_node, std::vector<std::string>& a_warnings, const std::string& a_sceneId, std::string_view a_clipRoot, const PropRegistry& a_props)
		{
			SceneNode n;
			n.id = a_node.value("id", std::string{});
			if (n.id.empty()) {
				throw std::runtime_error("scene '" + a_sceneId + "': a node is missing 'id'");
			}
			RejectReservedId(n.id, "node");

			const auto useIt = a_node.find("use");
			const bool hasUse = useIt != a_node.end() && useIt->is_string() && !useIt->get<std::string>().empty();
			const bool hasStages = a_node.contains("stages");
			if (hasUse && hasStages) {
				throw std::runtime_error("node '" + n.id + "': a node has both 'use' and 'stages' (exactly one is allowed)");
			}
			if (!hasUse && !hasStages) {
				throw std::runtime_error("node '" + n.id + "': a node needs 'use' (a scene id) or 'stages' (an inline clip timeline)");
			}
			if (hasUse) {
				n.use = useIt->get<std::string>();
			} else {
				size_t ac = 0;
				n.stages = ParseOsfStageList(a_node.at("stages"), "node '" + n.id + "'", ac, /*a_fixed*/ false, a_clipRoot,
					a_props, /*a_allowStageLanes*/ false);
			}

			// `loops`: omitted = once, 0 = hold, N = loop N times.
			if (const auto it = a_node.find("loops"); it != a_node.end()) {
				if (!it->is_number_integer()) {
					throw std::runtime_error("node '" + n.id + "': 'loops' must be an integer (omit = once, 0 = hold, N = loop N)");
				}
				const int loops = it->get<int>();
				if (loops < 0) {
					throw std::runtime_error("node '" + n.id + "': 'loops' must be >= 0 (0 = hold forever)");
				}
				if (loops == 0) {
					n.loopMode = LoopMode::kHold;
				} else {
					n.loopMode = LoopMode::kCount;
					n.loopCount = loops;
				}
			} else {
				n.loopMode = LoopMode::kOnce;
			}
			n.timerSec = a_node.value("timer", 0.0f);

			std::unordered_set<std::string> edgeIds;
			int defaults = 0;
			bool hasTimerEdge = false;
			if (const auto it = a_node.find("edges"); it != a_node.end()) {
				for (const auto& jEdge : *it) {
					auto e = ParseEdge(jEdge, n.id);
					if (e.when == EdgeWhen::kEnd && n.loopMode == LoopMode::kHold) {
						throw std::runtime_error("node '" + n.id + "': an 'end' edge on a hold node (loops:0) can never fire");
					}
					if (e.when == EdgeWhen::kLoops && n.loopMode != LoopMode::kCount) {
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
			if (hasTimerEdge && n.timerSec <= 0.0f) {
				throw std::runtime_error("node '" + n.id + "': has a 'timer' edge but 'timer' <= 0");
			}
			if (!hasTimerEdge && n.timerSec > 0.0f) {
				a_warnings.push_back("scene '" + a_sceneId + "' node '" + n.id + "': 'timer' set but no 'timer' edge");
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
			if (auto it = a_node.find("camera"); it != a_node.end()) {
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
			return n;
		}

		// Checked for both scene forms: a typo'd lane role would otherwise misfire silently at runtime.
		// Anonymous roles are unreferenceable.
		void ValidateRoleRefs(const SceneDef& def)
		{
			std::unordered_set<std::string> roleNames;
			for (const auto& r : def.roles) {
				if (!r.name.empty()) {
					roleNames.insert(ToLower(r.name));
				}
			}
			for (const auto& nd : def.nodes) {
				for (const auto& a : nd.actions) {
					if (!a.role.empty() && !roleNames.count(ToLower(a.role))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + nd.id + "' action '" + a.type +
							"' references undeclared role '" + a.role + "'");
					}
				}
				for (const auto& s : nd.sounds) {
					if (!s.role.empty() && !roleNames.count(ToLower(s.role))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + nd.id + "' sound '" + s.spec +
							"' references undeclared role '" + s.role + "'");
					}
				}
			}
		}

		void ValidateGraph(const SceneDef& def, const std::unordered_set<std::string>& a_nodeIds)
		{
			if (!a_nodeIds.count(ToLower(def.entry))) {
				throw std::runtime_error("scene '" + def.id + "': entry '" + def.entry + "' is not a node");
			}
			ValidateRoleRefs(def);
			for (const auto& nd : def.nodes) {
				for (const auto& e : nd.edges) {
					if (e.to != "$end" && !a_nodeIds.count(ToLower(e.to))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + nd.id + "' edge targets missing node '" + e.to + "'");
					}
				}
				// Otherwise the mismatch only surfaces mid-scene.
				for (const auto& st : nd.stages) {
					if (!st.clips.empty() && st.clips.size() != def.roles.size()) {
						throw std::runtime_error("scene '" + def.id + "': node '" + nd.id + "' stage has " +
							std::to_string(st.clips.size()) + " clip(s) but the scene has " +
							std::to_string(def.roles.size()) + " role(s)");
					}
				}
			}
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
		SceneDef ParseOsfScene(const json& a_json, std::vector<std::string>& a_warnings,
			const ContentFileDefaults& a_defaults, std::optional<CameraState> a_cameraDefault,
			const std::vector<SceneRole>& a_fileRoles, const RoleRegistry& a_roleRegistry,
			const PropRegistry& a_propRegistry,
			std::string_view a_packClipRoot, const AnchorReq& a_anchorDefault)
		{
			SceneDef def;
			def.id = a_json.value("id", std::string{});
			if (def.id.empty()) {
				throw std::runtime_error("scene missing 'id'");
			}
			RejectReservedId(def.id, "scene");
			const std::string clipRoot = a_json.contains("clipRoot") ?
				NormalizeClipRoot(a_json.value("clipRoot", std::string{}), "scene '" + def.id + "'") :
				std::string(a_packClipRoot);
			ParseSceneMeta(a_json, def, a_defaults);
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
				throw std::runtime_error("scene '" + def.id + "': placement 'followActor' (legacy 'inPlace:true') cannot be combined with an 'anchor' requirement");
			}

			const bool hasNodes = a_json.contains("nodes");
			const bool hasStages = a_json.contains("stages");
			const bool hasClip = a_json.contains("clip");
			if (!hasNodes && !hasStages && !hasClip) {
				throw std::runtime_error("scene '" + def.id + "': needs a playable — top-level 'clip', 'stages', or 'nodes'");
			}
			if (hasNodes && (hasStages || hasClip)) {
				throw std::runtime_error("scene '" + def.id + "': a scene has both 'nodes' and top-level 'clip'/'stages' (use one)");
			}
			if (hasStages && hasClip) {
				throw std::runtime_error("scene '" + def.id + "': a scene has both 'clip' and 'stages' (use one)");
			}

			if (hasNodes) {
				const auto& jNodes = a_json.at("nodes");
				if (!jNodes.is_array() || jNodes.empty()) {
					throw std::runtime_error("scene '" + def.id + "': 'nodes' must be a non-empty array");
				}
				def.entry = a_json.value("entry", std::string{});
				if (def.entry.empty()) {
					throw std::runtime_error("scene '" + def.id + "': a graph scene needs 'entry'");
				}
				std::unordered_set<std::string> nodeIds;
				for (const auto& jNode : jNodes) {
					auto nd = ParseOsfNode(jNode, a_warnings, def.id, clipRoot, a_propRegistry);
					if (!nodeIds.insert(ToLower(nd.id)).second) {
						throw std::runtime_error("scene '" + def.id + "': duplicate node id '" + nd.id + "'");
					}
					def.nodes.push_back(std::move(nd));
				}
				if (const auto it = a_json.find("linearStages"); it != a_json.end()) {
					for (const auto& s : *it) {
						auto nid = s.get<std::string>();
						if (!nodeIds.count(ToLower(nid))) {
							throw std::runtime_error("scene '" + def.id + "': linearStages references missing node '" + nid + "'");
						}
						def.linearStages.push_back(std::move(nid));
					}
				}
				// A hold node with no exit edge is the intentional terminal hold (ends via StopScene); not flagged.
				ValidateGraph(def, nodeIds);
				// Infer one anonymous role per entry-stage clip, else the scene could never matchmake.
				// A `use` entry resolves post-load, so it can't seed a count.
				if (!rolesGiven) {
					const std::string entryLower = ToLower(def.entry);
					const auto entryIt = std::find_if(def.nodes.begin(), def.nodes.end(),
						[&](const SceneNode& a_nd) { return ToLower(a_nd.id) == entryLower; });
					if (entryIt == def.nodes.end() || entryIt->stages.empty()) {
						throw std::runtime_error("scene '" + def.id + "': a graph scene whose entry node is a 'use' needs explicit 'roles'");
					}
					def.roles.assign(entryIt->stages.front().clips.size(), SceneRole{});
				}
			} else {
					size_t actorCount = rolesGiven ? def.roles.size() : 0;
				std::vector<StageDef> stages;
				if (hasClip) {
					const auto& clip = a_json.at("clip");
					if (actorCount != 0 && actorCount != 1) {
						throw std::runtime_error("scene '" + def.id + "': 'clip' is single-actor but " +
							std::to_string(actorCount) + " role(s) declared (use 'stages' for multi-actor)");
					}
					StageDef st;
					st.loops = 1;
					st.clips.push_back(ParseStageClip(clip, clipRoot, "scene '" + def.id + "'"));
					actorCount = 1;
					stages.push_back(std::move(st));
				} else {
					stages = ParseOsfStageList(a_json.at("stages"), "scene '" + def.id + "'", actorCount, rolesGiven, clipRoot, a_propRegistry);
				}
				if (!rolesGiven) {
					def.roles.assign(actorCount, SceneRole{});
				}
				DesugarLinear(def, stages);
				ValidateRoleRefs(def);
			}

			// File-level `camera` goes on the entry node (the ledger holds it until scene-stop); an
			// explicit entry-node camera track wins.
			if (a_cameraDefault) {
				const std::string entryLower = ToLower(def.entry);
				for (auto& nd : def.nodes) {
					if (ToLower(nd.id) == entryLower) {
						if (nd.cameras.empty()) {
							CameraEntry ce;
							ce.state = *a_cameraDefault;
							nd.cameras.push_back(std::move(ce));
						}
						break;
					}
				}
			}
			// Derived from the first clip path, matching how playback picks the rig.
			for (const auto& nd : def.nodes) {
				for (const auto& st : nd.stages) {
					for (const auto& clip : st.clips) {
						if (std::string sp = Util::SpeciesFromAnimPath(clip.file); !sp.empty()) {
							def.species = std::move(sp);
							break;
						}
					}
					if (!def.species.empty()) {
						break;
					}
				}
				if (!def.species.empty()) {
					break;
				}
			}
			if (def.species.empty()) {
				def.species = "human";
			}
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
			std::vector<const json*> sceneJsons;
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
				a_out.scene.playerInputLock = ReadVocabularyBool(a_json, "playerInputLock", "lockPlayer", true, fileContext);
				a_out.scene.hideApparel = ReadVocabularyBool(a_json, "hideApparel", "stripActors", !a_out.library, fileContext);
				a_out.scene.worldPlacement = ReadWorldPlacement(a_json,
					Animation::WorldPlacementMode::kAnchorAndPin, fileContext);
			} catch (const std::exception& e) {
				a_reject(e.what());
				return false;
			}
			a_out.scene.clearHeldItems = a_json.value("clearHeldItems", true);
			a_out.scene.fade = a_json.value("fade", false);
			a_out.scene.unlisted = a_json.value("unlisted", false);
			try {
				if (const json* controls = FindVocabularyKey(a_json, "sceneControls", "playerControl", fileContext)) {
					ApplySceneControls(*controls, a_out.scene.sceneControls, fileContext,
						a_json.contains("sceneControls") ? "sceneControls" : "playerControl");
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

			if (const auto cameraIt = a_json.find("camera"); cameraIt != a_json.end()) {
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

			if (const auto scenesIt = a_json.find("scenes"); scenesIt != a_json.end()) {
				if (!scenesIt->is_array()) {
					a_reject("'" + a_fileName + "': 'scenes' must be an array");
					return false;
				}
				if (scenesIt->size() > kMaxScenesPerFile) {
					a_reject("'" + a_fileName + "': contains more than " +
						std::to_string(kMaxScenesPerFile) + " scenes");
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
							"': 'props' is a file-level registry — move it beside 'scenes'");
						return false;
					}
					a_out.sceneJsons.push_back(&scene);
				}
			} else if (a_json.contains("id")) {
				a_out.sceneJsons.push_back(&a_json);
			} else if (!a_json.contains("clipLibrary")) {
				a_reject("'" + a_fileName + "': expected a bare scene 'id', 'scenes', or 'clipLibrary'");
				return false;
			}
			return true;
		}

		bool AppendClipLibrary(const json& a_json, const std::filesystem::path& a_file,
			const OsfFileDefaults& a_defaults, SceneLoadBudget& a_budget,
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

		void LoadOsfScenes(const OsfFileContents& a_contents, const OsfFileDefaults& a_defaults,
			const std::filesystem::path& a_file, std::unordered_map<std::string, SceneDef>& a_out,
			SceneLoadBudget& a_budget, ProblemSink& a_problems)
		{
			const std::string fileName = a_file.filename().string();
			for (const auto* sceneJson : a_contents.sceneJsons) {
				std::vector<std::string> warnings;
				const std::string authoredId = sceneJson->is_object() && sceneJson->contains("id") &&
					(*sceneJson)["id"].is_string() ? (*sceneJson)["id"].get<std::string>() : std::string{};
				try {
					auto definition = ParseOsfScene(*sceneJson, warnings, a_defaults.scene, a_defaults.camera,
						a_contents.fileRoles, a_contents.roles, a_contents.props,
						a_defaults.clipRoot, a_contents.anchor);
					if (definition.nodes.size() > kMaxNodesPerScene) {
						throw std::runtime_error("scene '" + definition.id + "': too many nodes");
					}
					std::size_t stageCount = 0;
					std::size_t clipCount = 0;
					for (const auto& node : definition.nodes) {
						stageCount += node.stages.size();
						for (const auto& stage : node.stages) {
							clipCount += stage.clips.size();
						}
					}
					if (stageCount > kMaxStagesPerScene || clipCount > kMaxClipsPerScene) {
						throw std::runtime_error("scene '" + definition.id + "': stage/clip limit exceeded");
					}
					definition.sourceFile = a_file;
					definition.pack = a_defaults.packName;
					definition.folder = a_defaults.folder;
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
					if (a_budget.scenes >= kMaxScenesTotal ||
						definition.nodes.size() > kMaxNodesTotal - a_budget.nodes ||
						stageCount > kMaxStagesTotal - a_budget.stages ||
						clipCount > kMaxClipsTotal - a_budget.clips) {
						a_problems.Push("[error] scene registry aggregate scene/node/stage/clip limit reached — remaining scenes skipped",
							a_file, "registry-limit",
							"Reduce the installed scene content below the registry limits, then reload packs.", definition.id);
						REX::ERROR("[Registry] aggregate scene/node/stage/clip limit reached — remaining scenes skipped");
						break;
					}
					for (const auto& warning : warnings) {
						a_problems.Push("[warn] " + warning, a_file, "scene-warning",
							"Review the authored value; the scene loaded with a fallback.", definition.id);
						REX::WARN("[Registry] {}", warning);
					}
					const auto nodeCount = definition.nodes.size();
					REX::DEBUG("[Registry] loaded scene '{}' ({} node(s)) from '{}'",
						definition.id, nodeCount, fileName);
					a_out.emplace(std::move(key), std::move(definition));
					++a_budget.scenes;
					a_budget.nodes += nodeCount;
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
			std::unordered_map<std::string, SceneDef>& a_out,
			std::vector<ClipLibraryRegistration>& a_clipLibrary,
			SceneLoadBudget& a_budget, ProblemSink& a_problems)
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
			LoadOsfScenes(contents, defaults, a_file, a_out, a_budget, a_problems);
		}

		// `use` splices the target's single inline-stage node, so the target must be exactly that.
		void ValidateUseRefs(const std::unordered_map<std::string, SceneDef>& a_scenes, ProblemSink& a_problems)
		{
			for (const auto& [key, def] : a_scenes) {
				for (const auto& nd : def.nodes) {
					if (nd.use.empty()) {
						continue;
					}
					const auto tit = a_scenes.find(ToLower(nd.use));
					if (tit == a_scenes.end()) {
						a_problems.Push("[error] scene '" + def.id + "' node '" + nd.id +
							"': use references unknown scene '" + nd.use + "'", def.sourceFile,
							"unknown-scene-reference", "Fix the node's use target or install the pack that defines it.",
							def.id, nd.id);
						REX::ERROR("[Registry] scene '{}' node '{}' use references unknown scene '{}'", def.id, nd.id, nd.use);
						continue;
					}
					const auto& target = tit->second;
					if (target.nodes.size() != 1 || target.nodes[0].stages.empty()) {
						a_problems.Push("[error] scene '" + def.id + "' node '" + nd.id + "': use target '" + nd.use +
							"' is not a single inline-stage scene (use splices one node's stages)", def.sourceFile,
							"invalid-use-target", "Reference a scene with one inline-stage node, then reload packs.",
							def.id, nd.id);
						REX::ERROR("[Registry] scene '{}' node '{}' use target '{}' is not single inline-stage", def.id, nd.id, nd.use);
					}
				}
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

	const SceneNode* SceneDef::FindNode(std::string_view a_id) const
	{
		const auto want = ToLower(std::string(a_id));
		for (const auto& n : nodes) {
			if (ToLower(n.id) == want) {
				return &n;
			}
		}
		return nullptr;
	}

	std::int32_t SceneDef::LinearStageOf(std::string_view a_nodeId) const
	{
		const auto want = ToLower(std::string(a_nodeId));
		for (std::size_t i = 0; i < linearStages.size(); i++) {
			if (ToLower(linearStages[i]) == want) {
				return static_cast<std::int32_t>(i);
			}
		}
		return -1;
	}

	ContentRegistry& ContentRegistry::GetSingleton()
	{
		static ContentRegistry singleton;
		return singleton;
	}

	void ContentRegistry::LoadAll()
	{
		std::unordered_map<std::string, SceneDef> loaded;
		std::vector<std::string> errors;
		std::vector<PendingImportProblem> pendingProblems;
		ProblemSink problems{ errors, pendingProblems };
		std::vector<ClipLibraryRegistration> clipLibrary;
		std::vector<ContentFileStats> fileStats;
		std::unordered_map<std::string, std::size_t> fileIndex;  // full source path -> fileStats index
		ClipInstalledCache clipCache;
		SceneLoadBudget loadBudget;

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
				return nlohmann::json::parse(a_input, nullptr, true, true);
			},
			[&](const Util::RegistryJsonSource& a_source, const nlohmann::json& a_json) {
				auto& stats = ensureFileStats(a_source);
				if (const auto scenes = a_json.find("scenes"); scenes != a_json.end() && scenes->is_array()) {
					stats.declaredScenes = static_cast<std::uint32_t>(scenes->size());
				} else if (a_json.contains("id")) {
					stats.declaredScenes = 1;
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

		ValidateUseRefs(loaded, problems);

		SweepClipAvailability(loaded, problems, clipCache);
		const auto sceneCount = loaded.size();
		AccumulateFileStats(loaded, clipCache, fileStats, fileIndex);
		std::map<std::string, std::uint32_t> clipEntriesByFile;
		const auto clipEntryCount = AddSceneClipEntries(loaded, clipLibrary, problems, clipEntriesByFile);
		for (const auto& [path, count] : clipEntriesByFile) {
			if (const auto it = fileIndex.find(path); it != fileIndex.end()) {
				fileStats[it->second].clipEntries = count;
			}
		}
		for (auto& stats : fileStats) {
			stats.rejectedScenes = stats.declaredScenes > stats.scenes ? stats.declaredScenes - stats.scenes : 0;
		}

		const auto problemCount = errors.size();

		// Unowned problems land in one trailing bucket.
		ContentFileStats crossFile;
		for (std::size_t i = 0; i < errors.size() && i < pendingProblems.size(); ++i) {
			const auto& pending = pendingProblems[i];
			const auto it = fileIndex.find(pending.owner.string());
			auto& target = it != fileIndex.end() ? fileStats[it->second] : crossFile;
			(pending.warning ? target.warnings : target.errors) += 1;
			target.problems.push_back(ContentImportProblem{
				pending.warning, pending.code, errors[i], pending.hint, pending.scene, pending.node,
				pending.role, pending.clip
			});
		}
		std::sort(fileStats.begin(), fileStats.end(), [](const ContentFileStats& a_lhs, const ContentFileStats& a_rhs) {
			return a_lhs.path < a_rhs.path;
		});
		const auto fileCount = fileStats.size();  // before the bucket, which is not a file
		if (!crossFile.problems.empty()) {
			fileStats.push_back(std::move(crossFile));  // empty path; must stay last
		}

		auto next = std::make_shared<ContentRegistrySnapshot>();
		next->scenes = std::move(loaded);
		next->loadErrors = std::move(errors);
		next->authoredSceneCount = sceneCount;
		next->files = std::move(fileStats);
		snapshot.store(std::move(next), std::memory_order_release);
		REX::INFO("[Registry] {} scene(s) loaded from {} file(s), {} scene clip entr{}, {} problem(s)",
			sceneCount, fileCount, clipEntryCount, clipEntryCount == 1 ? "y" : "ies", problemCount);
	}

	SceneRef ContentRegistry::Find(std::string_view a_id) const
	{
		SceneRef out;
		out.owner = snapshot.load(std::memory_order_acquire);
		const auto it = out.owner->scenes.find(ToLower(std::string(a_id)));
		if (it != out.owner->scenes.end()) {
			out.value = &it->second;
		}
		return out;
	}

	std::optional<Animation::PlaybackPlan> ContentRegistry::BuildNodePlan(const SceneRef& a_def, const SceneNode& a_node, size_t a_actorCount) const
	{
		if (!a_def) {
			return std::nullopt;
		}
		if (!a_node.use.empty()) {
			// Re-checked here; the target's own roles supply the default placements.
			const auto it = a_def.owner->scenes.find(ToLower(a_node.use));
			if (it == a_def.owner->scenes.end()) {
				REX::WARN("[Registry] node '{}' use '{}' references unknown scene", a_node.id, a_node.use);
				return std::nullopt;
			}
			const auto& target = it->second;
			if (target.nodes.size() != 1 || !target.nodes[0].use.empty() || target.nodes[0].stages.empty()) {
				REX::WARN("[Registry] node '{}' use target '{}' is not a single inline-stage scene", a_node.id, a_node.use);
				return std::nullopt;
			}
			auto plan = BuildPlanFromStages(target.id, target.roles, target.nodes[0].stages, a_actorCount);
			if (plan) {
				plan->SetWorldPlacementMode(a_def->worldPlacement);
			}
			return plan;
		}
		auto plan = BuildPlanFromStages(a_def->id, a_def->roles, a_node.stages, a_actorCount);
		if (plan) {
			plan->SetWorldPlacementMode(a_def->worldPlacement);
		}
		return plan;
	}

	void ContentRegistry::ForEachDef(const std::function<void(const SceneDef&)>& a_fn) const
	{
		const auto current = snapshot.load(std::memory_order_acquire);
		for (const auto& [key, def] : current->scenes) {
			a_fn(def);
		}
	}

	size_t ContentRegistry::Size() const
	{
		return snapshot.load(std::memory_order_acquire)->authoredSceneCount;
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
		// Same BSResource-aware probe as the load-time sweep, so archive-resident clips aren't false positives.
		std::unordered_map<std::string, bool> cache;
		const auto installed = [&cache](const std::string& a_file) {
			auto [it, fresh] = cache.try_emplace(a_file, false);
			if (fresh) {
				it->second = ClipSpecInstalled(a_file);
			}
			return it->second;
		};

		std::vector<std::string> out;
		const auto current = snapshot.load(std::memory_order_acquire);
		for (const auto& [key, def] : current->scenes) {
			for (const auto& node : def.nodes) {
				for (const auto& stage : node.stages) {
					for (const auto& clip : stage.clips) {
						if (!installed(clip.file)) {
							out.push_back(def.id + " node '" + node.id + "': missing clip '" + clip.file +
								(clip.animId.empty() ? "" : (":" + clip.animId)) + "'");
						}
					}
				}
			}
		}
		return out;
	}
}
