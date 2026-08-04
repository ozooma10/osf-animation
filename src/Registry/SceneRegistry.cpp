#include "Registry/SceneRegistry.h"
#include "Registry/SceneRegistryClips.h"

#include "Input/InputTypes.h"
#include "Util/FormRef.h"
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
		using SceneRegistryClips::ClipInstalledCache;
		using SceneRegistryClips::ClipLibraryRegistration;
		using SceneRegistryClips::PendingImportProblem;
		using SceneRegistryClips::ProblemSink;
		using SceneRegistryClips::AccumulateFileStats;
		using SceneRegistryClips::AddSceneClipEntries;
		using SceneRegistryClips::ClipSpecInstalled;
		using SceneRegistryClips::DesugarLinear;
		using SceneRegistryClips::SweepClipAvailability;

		constexpr std::size_t kMaxScenesPerFile = 4096;
		constexpr std::size_t kMaxRoutesPerFile = 4096;
		constexpr std::size_t kMaxRolesPerFile = 512;
		constexpr std::size_t kMaxPropsPerFile = 512;
		constexpr std::size_t kMaxClipLibraryEntriesPerFile = 65536;
		constexpr std::size_t kMaxNodesPerScene = 4096;
		constexpr std::size_t kMaxStagesPerScene = 16384;
		constexpr std::size_t kMaxClipsPerScene = 65536;
		constexpr std::size_t kMaxScenesTotal = 32768;
		constexpr std::size_t kMaxRoutesTotal = 32768;
		constexpr std::size_t kMaxNodesTotal = 131072;
		constexpr std::size_t kMaxStagesTotal = 262144;
		constexpr std::size_t kMaxClipsTotal = 1048576;
		constexpr std::size_t kMaxClipLibraryEntriesTotal = 262144;

		struct SceneLoadBudget
		{
			std::size_t scenes = 0;
			std::size_t routes = 0;
			std::size_t nodes = 0;
			std::size_t stages = 0;
			std::size_t clips = 0;
			std::size_t clipLibraryEntries = 0;
		};

		// --- Form-ref resolution ("Plugin.esm|0xLOCAL") ------------------------------------------
		// Resolve a form ref to T* (BGSKeyword / TESRace). Throws (rejecting the scene) with a precise
		// role+field message on any failure. The RE-sensitive FormID composition lives in
		// Util::ComposeFormID. LookupByID<T> returns null for not-found OR wrong-type.
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

		// File-local reusable role TEMPLATES: the OBJECT form of a multi-scene file's top-level
		// `roles`. Exact, case-sensitive id -> a validated template. Scene-level `roles` arrays may
		// mix inline role objects with references to these by id; a reference expands to an ordinary
		// SceneRole at load. Never leaves the file — no aliases or cross-file references.
		struct RoleTemplate
		{
			json      raw;     // the definition's original JSON (`name` defaulted to the id) — the merge base for object overrides
			SceneRole parsed;  // the validated parse of `raw` — copied directly by plain-string references
		};
		using RoleRegistry = std::map<std::string, RoleTemplate>;

		// File-local reusable prop TEMPLATES: a file's top-level `props`. Exact, case-sensitive id ->
		// the definition's RAW json, deliberately left unparsed: a template may be PARTIAL (source
		// only, node only), so only the merged action is a complete, validatable attachment. An
		// `osf.prop.attach` looks its template up by `use` when present, else by its own `prop` id.
		// Never leaves the file — no aliases or cross-file references.
		using PropRegistry = std::map<std::string, json>;

		// How a scene-level role slot got its runtime name (drives AssignRoleNames).
		enum class RoleNameKind : std::uint8_t
		{
			kAutomatic,  // plain string ref, or { "id": ... } with no `name`: the template's effective name, numbered on collision
			kExplicit,   // { "id": ..., "name": "x" } or a named inline role: kept exactly; duplicates reject the scene
			kAnonymous   // explicit/inline "name": "" or an unnamed inline role: never numbered
		};

		// One scene-level `roles` entry after reference expansion, before runtime-name assignment.
		struct PendingRole
		{
			SceneRole     role;
			RoleNameKind  nameKind = RoleNameKind::kAnonymous;
			std::string   autoBase;  // kAutomatic: the template's effective name ("" = stays anonymous)
		};

		// Parse an { x, y, z, heading } placement. Authors write heading in DEGREES; the
		// runtime uses radians.
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
			// Any other value is a Data-relative path prefix joined ahead of each relative clip file (the generated vanilla packs use "meshes/actors/human/animations").
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
				clip.file = a_clip.at("file").get<std::string>();
				SplitAnimSuffix(clip);
				if (auto ait = a_clip.find("anim"); ait != a_clip.end()) {
					if (!ait->is_string()) {
						throw std::runtime_error(a_subject + ": clip 'anim' must be a string");
					}
					clip.animId = ait->get<std::string>();
				}
				if (auto oit = a_clip.find("offset"); oit != a_clip.end()) {
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
				// Pre-measured duration (the generated vanilla packs carry these) — spares the
				// duration scan a per-clip walk of the game archive.
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
					if (const auto nit = entry.find("name"); nit != entry.end()) {
						if (!nit->is_string() || nit->get_ref<const std::string&>().empty()) {
							throw std::runtime_error(subject + ": 'name' must be a non-empty string");
						}
						reg.name = nit->get<std::string>();
					}
					if (const auto fit = entry.find("folder"); fit != entry.end()) {
						reg.folder = ParseCatalogFolder(*fit, subject);
					}
					if (const auto tit = entry.find("tags"); tit != entry.end()) {
						if (!tit->is_array()) {
							throw std::runtime_error(subject + ": 'tags' must be an array of strings");
						}
						for (const auto& tag : *tit) {
							if (!tag.is_string() || tag.get_ref<const std::string&>().empty()) {
								throw std::runtime_error(subject + ": every tag must be a non-empty string");
							}
							reg.tags.push_back(tag.get<std::string>());
						}
					}
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
			// branchable (advance) edges feed GetSceneEdge* menus -> need id + label.
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

		// Parse the timing fields shared by every track lane (cue/action/sound/camera): the
		// `repeat` flag and the position — either `at` (enter/exit/end anchor or a numeric
		// clip-fraction in [0,1)) or `atFrame` (a zero-based clip frame at kFrameRate). The two are
		// mutually exclusive. Writes pos/fraction/frame/everyLoop onto a_out, whose `pos` enum
		// supplies the kEnter/kExit/kEnd/kFraction values (all four lane enums share those names);
		// an `atFrame` entry is a kFraction entry that carries `frame` instead of `fraction`.
		// a_subject is the entry descriptor for diagnostics, e.g. "action 'osf.fade.out'". When
		// a_atRequired, a missing position is rejected (cues); otherwise it defaults to the enter anchor.
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
				// A frame is an ABSOLUTE clip-local position (frame / kFrameRate seconds), so unlike a
				// fraction it needs no clip duration here — and a frame past the clip end simply never
				// fires (nothing to validate it against at load time).
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

		// The prop parsers below take a_subject — the full diagnostic prefix of whatever owns the
		// keys, e.g. "node 'x': action 'osf.prop.attach'" for an action or "props template 'helmet'"
		// for a file-level definition — so one set of validators serves both.
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
			const auto& keyword = equipped.at("keyword");
			const auto append = [&](const json& a_value) {
				if (!a_value.is_string() || a_value.get<std::string>().empty()) {
					throw std::runtime_error(a_subject +
						" equippedArmor.keyword entries must be non-empty strings");
				}
				result.keywords.push_back(a_value.get<std::string>());
			};
			if (keyword.is_string()) {
				append(keyword);
			} else if (keyword.is_array()) {
				for (const auto& entry : keyword) {
					append(entry);
				}
			} else {
				throw std::runtime_error(a_subject +
					" source.equippedArmor.keyword must be a string or array");
			}
			if (result.keywords.empty()) {
				throw std::runtime_error(a_subject + " source.equippedArmor.keyword cannot be empty");
			}
			result.kind = Props::SourceKind::kEquippedArmor;
			return result;
		}

		// Split out of ParsePropAttachment so a file-level `props` template can validate its own
		// `scale` without carrying the `node` that only a complete attachment needs.
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
			const auto node = a_owner.find("node");
			if (node == a_owner.end() || !node->is_string() ||
				node->get<std::string>().empty()) {
				throw std::runtime_error(a_subject + " requires a non-empty 'node'");
			}
			result.node = node->get<std::string>();
			result.position = ParsePropVector(a_owner, "position", a_subject);
			result.rotation = ParsePropVector(a_owner, "rotation", a_subject);
			result.scale = ParsePropScale(a_owner, a_subject);
			return result;
		}

		// The prop keys a `props` template may supply, and that an action may override. Deliberately a
		// STRICT SUBSET of an action's keys: timing (`at`/`atFrame`/`repeat`), dispatch (`type`) and
		// identity (`prop`, `role`) are never inheritable, so a template can neither move an action in
		// time nor retarget it.
		constexpr std::array<const char*, 5> kPropTemplateKeys{ "source", "node", "position", "rotation", "scale" };

		// Validate ONE file-level `props` definition. A template is deliberately allowed to be
		// PARTIAL — source-only, node-only — so only the keys it actually carries are checked here;
		// completeness is enforced on the MERGED action, the only place it can be known.
		//
		// Unknown keys ARE rejected: because the template namespace is that strict subset, a stray
		// "at" or "role" would otherwise sit in the file doing exactly nothing.
		void ValidatePropTemplate(const json& a_templ, const std::string& a_subject)
		{
			for (const auto& [key, value] : a_templ.items()) {
				if (std::find(kPropTemplateKeys.begin(), kPropTemplateKeys.end(), key) ==
					kPropTemplateKeys.end()) {
					throw std::runtime_error(a_subject + " has unknown key '" + key +
						"' (a prop template holds only 'source', 'node', 'position', 'rotation', 'scale')");
				}
			}
			if (const auto it = a_templ.find("source"); it != a_templ.end()) {
				(void)ParsePropSource(*it, a_subject);
			}
			if (const auto it = a_templ.find("node"); it != a_templ.end()) {
				if (!it->is_string() || it->get_ref<const std::string&>().empty()) {
					throw std::runtime_error(a_subject + " 'node' must be a non-empty string");
				}
			}
			(void)ParsePropVector(a_templ, "position", a_subject);  // absent = no-op
			(void)ParsePropVector(a_templ, "rotation", a_subject);
			(void)ParsePropScale(a_templ, a_subject);
		}

		// Every id a `props` registry defines, for a "you probably meant one of these" diagnostic.
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

		// Resolve an `osf.prop.attach` entry against the file's `props` registry. The template is
		// named by `use` when present, else by the action's own `prop` id — so the common case
		// ({ "prop": "helmet" }) needs no reference syntax at all.
		//
		// Returns a REFERENCE to whichever object the attach keys should be read from: the action
		// itself when no template applies, so a file without `props` copies nothing and behaves
		// exactly as it always has. Otherwise the merge lands in a_merged_out and that is returned.
		//
		// Merging is SHALLOW and key-level — a key the action authors wins outright, otherwise the
		// template supplies it. Notably `source` is inherited or overridden WHOLE, never deep-merged:
		// it is exactly-one-of `form`/`equippedArmor`, so a deep merge of a `form` override onto an
		// `equippedArmor` template would yield an object carrying BOTH and trip ParsePropSource with
		// an error blaming a `source` the author never wrote.
		//
		// Nothing is validated here. A partial template that merges to an incomplete attachment still
		// lands on the ordinary ParsePropSource/ParsePropAttachment errors, naming the same node.
		const json& ResolvePropAttach(const json& a_action, const PropRegistry& a_props,
			const std::string& a_prop, const std::string& a_subject, json& a_merged_out)
		{
			const auto useIt = a_action.find("use");
			const bool hasUse = useIt != a_action.end();
			// Deliberately NOT as lenient as a node's `use` (which treats a non-string as absent):
			// a silently-dropped template reference is the failure mode this feature exists to kill.
			if (hasUse && (!useIt->is_string() || useIt->get_ref<const std::string&>().empty())) {
				throw std::runtime_error(a_subject +
					" 'use' must be a non-empty string naming a top-level 'props' definition");
			}
			const std::string& templateId = hasUse ? useIt->get_ref<const std::string&>() : a_prop;

			const auto templ = a_props.find(templateId);  // exact, case-sensitive — as the roles registry
			if (templ == a_props.end()) {
				if (hasUse) {
					throw std::runtime_error(a_subject + " prop template '" + templateId +
						"' is not defined in this file's top-level 'props' registry (" +
						DescribePropRegistry(a_props) + ")");
				}
				// A bare `prop` matching no template is an ordinary fully-inline attach, judged by
				// the caller's existing "requires 'source'".
				return a_action;
			}

			a_merged_out = a_action;
			for (const char* const key : kPropTemplateKeys) {
				if (a_merged_out.contains(key)) {
					continue;  // authored inline: the action wins
				}
				if (const auto it = templ->second.find(key); it != templ->second.end()) {
					a_merged_out[key] = *it;
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
				ae.hold = a.value("hold", false);          // osf.fade.out: stay faded on cleanup
				ae.duration = a.value("duration", 0.0f);   // osf.fade.*: ramp secs (0 = default)
				ae.set = a.value("set", std::string{});    // osf.voice.play: sound spec
				ae.item = a.value("item", std::string{});  // osf.equipment.equip: item form ref
				ae.prop = a.value("prop", std::string{});  // osf.prop.*: scene-local prop id
				const auto typeLower = ToLower(ae.type);
				if (typeLower.rfind("osf.", 0) == 0) {
					const auto kind = ParseBuiltinAction(typeLower);
					if (!kind) {
						throw std::runtime_error("node '" + a_node_out.id + "': unknown built-in action '" + ae.type + "'");
					}
					ae.kind = *kind;
					// Per-action required fields: voice needs its sound set, equip its item. `role` is
					// OPTIONAL on every action — an omitted/empty role targets the scene's first
					// participant (ResolveRoleActor), matching the sound lane's default. A NAMED role
					// must still exist (ValidateGraph rejects undeclared references).
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
						// Fill anything the action left out from the file's `props` template of the same
						// id (or the one `use` names), then validate the MERGED entry exactly as before —
						// so every pre-existing diagnostic still fires, word for word.
						const std::string subject = "node '" + a_node_out.id + "': action 'osf.prop.attach'";
						json merged;  // populated only when a template actually applies
						const json& attach = ResolvePropAttach(a, a_props, ae.prop, subject, merged);
						const auto source = attach.find("source");
						if (source == attach.end()) {
							// Three distinct author mistakes, three messages. A matched template that
							// still yields no `source` (identified by ResolvePropAttach returning the
							// merge, not the action) is a partial template — do NOT claim "no matching
							// entry" while DescribePropRegistry lists that very id as defined.
							if (&attach != &a) {
								const std::string templateId = a.contains("use") ? a["use"].get<std::string>() : ae.prop;
								throw std::runtime_error(subject + " prop template '" + templateId +
									"' supplies no 'source' and the action authors none");
							}
							// With no registry in play this is the plain old "you forgot source". With one,
							// the likeliest cause is a typo'd `prop` id, so name that possibility.
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
						// `use` is only a prop-template reference. Silently ignoring it elsewhere would
						// hide a mistyped `type` — the very failure this feature exists to remove.
						throw std::runtime_error("node '" + a_node_out.id + "': action '" + ae.type +
							"': 'use' is only meaningful on 'osf.prop.attach'");
					}
				} else if (a.value("required", false)) {
					// Custom actions are best-effort notifications; `required` is reserved.
					throw std::runtime_error("node '" + a_node_out.id + "': custom action '" + ae.type + "' cannot be 'required'");
				}
				// `at` mirrors the cue time model: enter/exit/end named anchors, or a numeric
				// clip-local fraction in [0,1). repeat:"loop" only applies to numeric positions.
				ParseTrackTiming(a, ae, a_node_out.id, "action '" + ae.type + "'", /*a_atRequired*/ false);
				a_node_out.actions.push_back(std::move(ae));
			}
		}

		// Expand the `sound` ladder sugar: an object { role?, spec, repeat?, at } whose shared
		// fields apply to every hit, appending each hit's tag(s) to the base `spec`. `{gender}` in the base
		// stays for fire-time substitution (SceneRuntime::PlaySound). The lane's `at` holds many positions
		// (vs. a flat entry's single scalar `at`), in one of two shapes:
		//
		//   GROUPED (terse for repeated tiers) — keyed by the tag(s) to append, value = the positions:
		//     "at": { "low": [0.1, 0.3], "loud": [0.8] }
		//
		//   ARRAY (ordered, heterogeneous, per-hit overrides) — each entry is:
		//     0.5                  -> at 0.5, the base spec, lane defaults
		//     [0.5, "loud"]        -> at 0.5, spec = base + ",loud"   (extra elements append more tags)
		//     { at, tags?, spec?, role?, repeat? }  -> per-hit overrides (spec replaces the base)
		void ExpandSoundLadder(const json& a_lane, SceneNode& a_node_out)
		{
			const std::string baseSpec = a_lane.value("spec", a_lane.value("sound", a_lane.value("pool", std::string{})));
			const std::string laneRole = a_lane.value("role", std::string{});
			const std::string laneRepeat = ToLower(a_lane.value("repeat", "none"));  // ladders opt in with "loop"
			const SoundEmitter laneEmitter = ParseSoundEmitter(a_lane, SoundEmitter::kListener,
				a_node_out.id, "sound ladder");

			// A ladder can carry its positions under `atFrame` instead of `at` (both shapes), in which
			// case every bare position is a clip frame; a per-hit object still picks its own key.
			const bool laneFrames = a_lane.contains("atFrame");
			const char* const laneKey = laneFrames ? "atFrame" : "at";

			// Emit one entry; timing (at/atFrame/repeat) reuses the shared track-timing parse + validation.
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
						spec += "," + it.key();  // the group key is the tag(s) appended to the base
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
					emit(baseSpec, m, laneRepeat, laneRole, laneEmitter, laneKey);  // bare position -> base spec
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
						spec = it->get<std::string>();  // per-hit spec replaces the base
					}
					// `tags` accepts a string or an array of strings (the schema doc's own example
					// uses the array form — rejecting it threw the whole file away).
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
					// A per-hit object picks its own timing key; falling back to the lane's keeps a bare
					// `{ tags: [...] }` hit on the lane's units (and still errors as a missing position).
					// `at` and `atFrame` are mutually exclusive everywhere else (ParseTrackTiming), but the
					// emit lambda rebuilds a single-key timing object so that guard never sees the pair —
					// reject it here or the losing key is silently discarded.
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
			// Ladder sugar: a single object { spec, at:[...] } expands to many entries (see above).
			if (a_entries.is_object()) {
				ExpandSoundLadder(a_entries, a_node_out);
				return;
			}
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node_out.id + "': 'sound' track must be an array of entries or a { spec, at } ladder object");
			}
			for (const auto& s : a_entries) {
				// An element whose `at` is an array (or tag-keyed object) is a ladder — many tagged hits on
				// one lane (e.g. a per-role vocal ladder). A scalar `at` (a fraction or "enter"/"exit"/"end")
				// is a single flat cue. This lets `sound` mix flat cues and role ladders in one list.
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
				// `spec` is the canonical key (unified *.osf.json); `sound`/`pool` are accepted
				// aliases. A '$'-prefixed value is a SoundRegistry pool query resolved at fire time
				// (SceneRuntime::PlaySound); a plain value is a literal file/event spec.
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

		// The camera postures the runtime understands — shared by node `camera` tracks and the
		// pack-level `camera` default. (Tethered orbit / photo mode / cinematic between-actor shots
		// aren't wired yet.)

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
				ce.distance = c.value("distance", 0.0f);  // thirdperson_hold opening zoom; 0 = engine default
				if (ce.distance != 0.0f && ce.state != CameraState::kThirdPersonHold) {
					REX::DEBUG("[Registry] node '{}': camera 'distance' is only honored for 'thirdperson_hold' — state '{}' ignores it",
						a_node_out.id, stateName);
				}
				a_node_out.cameras.push_back(std::move(ce));
			}
		}

		// Parse one role: name, gender (the `gender` shorthand and `filters.gender` are the same
		// constraint — reject if both present and differ), and the resolved keyword/race filters.
		SceneRole ParseRole(const json& a_role, const std::string& a_sceneId)
		{
			SceneRole r;
			r.name = a_role.value("name", std::string{});  // "" = anonymous positional slot

			std::optional<SlotGender> shorthand;
			if (auto git = a_role.find("gender"); git != a_role.end()) {
				if (!git->is_string()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': 'gender' must be a string");
				}
				shorthand = ParseSlotGender(git->get<std::string>());
			}
			std::optional<SlotGender> fromFilter;

			if (auto fit = a_role.find("filters"); fit != a_role.end()) {
				if (!fit->is_object()) {
					throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': 'filters' must be an object");
				}
				const json& f = *fit;
				if (auto git = f.find("gender"); git != f.end()) {
					if (!git->is_string()) {
						throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': filters.gender must be a string");
					}
					fromFilter = ParseSlotGender(git->get<std::string>());
				}
				// keyword / race: a single string or an array of strings; resolved to forms now
				// (any-of within each list). Unresolvable / wrong-type => the scene is rejected.
				auto parseRefs = [&](const char* a_key, const char* a_field, auto a_push) {
					auto kit = f.find(a_key);
					if (kit == f.end()) {
						return;
					}
					if (kit->is_string()) {
						a_push(kit->get<std::string>(), a_field);
					} else if (kit->is_array()) {
						for (const auto& e : *kit) {
							if (!e.is_string()) {
								throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': " + a_field + " entries must be strings");
							}
							a_push(e.get<std::string>(), a_field);
						}
					} else {
						throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': " + a_field + " must be a string or array of strings");
					}
				};
				parseRefs("keyword", "filters.keyword", [&](const std::string& a_ref, const char* a_field) {
					r.keywords.push_back(ResolveFormRef<RE::BGSKeyword>(a_ref, a_sceneId, r.name, a_field, "Keyword (KYWD)")->GetFormID());
				});
				parseRefs("race", "filters.race", [&](const std::string& a_ref, const char* a_field) {
					r.races.push_back(ResolveFormRef<RE::TESRace>(a_ref, a_sceneId, r.name, a_field, "Race (RACE)")->GetFormID());
				});
			}

			if (shorthand && fromFilter && *shorthand != *fromFilter) {
				throw std::runtime_error("scene '" + a_sceneId + "': role '" + r.name + "': 'gender' and filters.gender disagree");
			}
			r.gender = shorthand ? *shorthand : (fromFilter ? *fromFilter : SlotGender::kAny);
			// Role-local pose composition. Omission preserves the historical absolute/override path.
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
			// Optional named driven-bone mask: the role stamps ONLY the mask's bones (per-bone
			// weighted), leaving the rest of the rig engine-driven — the partial-body gesture path
			// (an equip/wave plays over the engine's own locomotion instead of replacing it).
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
				r.mask = named->id;  // canonical casing regardless of authored casing
			}
			// Optional exact-name bone mask. Preserved bones stay under the engine's live pose for
			// this role while every other matched animation joint continues to stamp normally.
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
			// Optional default placement for this slot (unified *.osf.json roles).
			if (auto oit = a_role.find("offset"); oit != a_role.end()) {
				r.offset = ParseOffsetField(*oit);
			}
			// Optional per-gender item to equip for the scene (resolved at fire time). A bare string
			// applies to any gender; an object keys it by male/female (with an optional `any` fallback).
			// equip is fully SOFT: it never blocks scene selection. The form is resolved on the game
			// thread at scene start, so an uninstalled plugin is a skipped equip (warned), not a load
			// error. A malformed ref shape ("Plugin|0xLocal" expected) is likewise dropped with a
			// warning rather than rejecting the scene — so a typo'd or missing body mod still leaves
			// the scene playable, just without the equip.
			if (auto qit = a_role.find("equip"); qit != a_role.end()) {
				auto checkRef = [&](const std::string& a_ref, const char* a_key) -> std::string {
					if (!a_ref.empty() && a_ref.find('|') == std::string::npos) {
						REX::WARN("[Registry] scene '{}': role '{}': equip.{} '{}' is not a "
							"\"Plugin.esm|0xLocalID\" form ref — equip dropped (scene still loads)",
							a_sceneId, r.name, a_key, a_ref);
						return std::string{};  // drop the bad ref; runtime treats empty as "no equip"
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

		// Expand one scene-level `roles` entry against the file-local registry:
		//   "m"                        -> a copy of the referenced template (automatic name)
		//   { "id": "m", ... }         -> the template, with the remaining keys merged over it (see below)
		//   { ... }  (no `id`)         -> an ordinary inline role (unchanged behavior)
		// `id` must be a non-empty string naming a registry definition; it is removed before the merged
		// JSON goes through the normal role parse. Overrides merge JSON-merge-patch style (RFC 7386):
		// scalars (including poseMode/poseWeight/mask) replace, `filters`/`offset`/`equip` merge by key, arrays (preserveBones) replace
		// wholesale, and null removes an inherited optional field. Unknown or malformed references
		// throw — rejecting only this scene.
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
			// `gender` and `filters.gender` are aliases for the SAME constraint. When the override
			// supplies exactly one of the two paths, drop the inherited value on the other path so
			// the merged role can't trip ParseRole's disagreement check. (Both paths overridden:
			// left in place for ParseRole to validate, exactly like an inline role.)
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
				// An explicit name ("" = an anonymous slot): kept exactly, never numbered.
				out.nameKind = (nit->is_string() && !nit->get_ref<const std::string&>().empty()) ?
					RoleNameKind::kExplicit : RoleNameKind::kAnonymous;
			} else {
				out.nameKind = RoleNameKind::kAutomatic;
				out.autoBase = ref->second.parsed.name;
			}
			return out;
		}

		// Assign runtime role names for one scene, deterministically. EXPLICIT names (an override's
		// `name`, a named inline role) are reserved first — duplicates reject the scene — then
		// AUTOMATIC slots take their template's effective name, suffixed 2, 3, ... past any reserved
		// or already-used name (["m","m","f"] -> m, m2, f; ["m", {"id":"m","name":"m"}] -> m2, m).
		// Anonymous slots are never numbered.
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

		// Apply one `playerControl` value onto a grant that already holds the inherited default (the
		// built-in all-capabilities grant at file level, the file-level grant at scene level). Boolean
		// form toggles `enabled`; object form narrows what it inherited (`disable` removes bits from the
		// set already in a_out, so a scene's disable list composes with the pack's). a_ctx labels errors
		// ("scene 'x'" / "'pack.json'").
		void ApplyPlayerControl(const json& a_value, PlayerControl& a_out, const std::string& a_ctx)
		{
			if (a_value.is_boolean()) {
				a_out.enabled = a_value.get<bool>();
				return;
			}
			if (!a_value.is_object()) {
				throw std::runtime_error(a_ctx + ": 'playerControl' must be a boolean or an object");
			}
			if (auto en = a_value.find("enabled"); en != a_value.end()) {
				if (!en->is_boolean()) {
					throw std::runtime_error(a_ctx + ": 'playerControl.enabled' must be a boolean");
				}
				a_out.enabled = en->get<bool>();
			}
			if (auto d = a_value.find("disable"); d != a_value.end()) {
				if (!d->is_array()) {
					throw std::runtime_error(a_ctx + ": 'playerControl.disable' must be an array of strings");
				}
				for (const auto& v : *d) {
					if (!v.is_string()) {
						throw std::runtime_error(a_ctx + ": 'playerControl.disable' entries must be strings");
					}
					const auto name = v.get<std::string>();
					const auto bit = Input::CapabilityBit(name);
					if (bit == 0) {
						REX::WARN("[Registry] {}: unknown playerControl capability '{}' — ignored (typo, or a newer OSF Animation?)", a_ctx, name);
					}
					a_out.capabilities &= ~bit;  // remove from the inherited set
				}
			}
			if (auto lk = a_value.find("locked"); lk != a_value.end()) {
				if (!lk->is_boolean()) {
					throw std::runtime_error(a_ctx + ": 'playerControl.locked' must be a boolean");
				}
				a_out.locked = lk->get<bool>();
			}
		}

		// Every file-level key a scene inherits when it omits its own. One struct so hoisting another
		// field to the pack level stays a one-line change here plus one line in LoadOsfFile. (`roles`,
		// `anchor`, `camera` and `clipRoot` are threaded separately — each carries its own
		// registry/optional/string shape.)
		struct PackDefaults
		{
			bool                     lockPlayer = true;
			bool                     stripActors = true;
			bool                     clearHeldItems = true;
			bool                     fade = false;
			bool                     unlisted = false;
			bool                     inPlace = false;
			PlayerControl            playerControl{};
			std::int32_t             priority = 0;
			std::int32_t             weight = 1;
			std::vector<std::string> tags;  // UNION-ed with a scene's own tags, not replaced by them
		};

		// Top-level metadata (name/priority/weight/unlisted/lockPlayer/stripActors/clearHeldItems/fade/
		// playerControl). id, tags, roles, and the playable (clip/stages/nodes) are parsed by the caller.
		// a_defaults seeds every inherited field with the file-level value.
		void ParseSceneMeta(const json& a_json, SceneDef& def, const PackDefaults& a_defaults)
		{
			def.name = a_json.value("name", def.id);
			def.priority = a_json.value("priority", a_defaults.priority);
			def.weight = a_defaults.weight;
			if (auto it = a_json.find("weight"); it != a_json.end()) {
				if (!it->is_number_integer()) {
					throw std::runtime_error("scene '" + def.id + "': 'weight' must be an integer");
				}
				const auto w = it->get<std::int64_t>();
				if (w < 1 || w > 1000000) {
					throw std::runtime_error("scene '" + def.id + "': 'weight' must be in [1, 1000000]");
				}
				def.weight = static_cast<std::int32_t>(w);
			}
			def.unlisted = a_defaults.unlisted;
			if (auto it = a_json.find("unlisted"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'unlisted' must be a boolean");
				}
				def.unlisted = it->get<bool>();
			}
			def.lockPlayer = a_defaults.lockPlayer;
			if (auto it = a_json.find("lockPlayer"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'lockPlayer' must be a boolean");
				}
				def.lockPlayer = it->get<bool>();
			}
			def.stripActors = a_defaults.stripActors;
			if (auto it = a_json.find("stripActors"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'stripActors' must be a boolean");
				}
				def.stripActors = it->get<bool>();
			}
			def.clearHeldItems = a_defaults.clearHeldItems;
			if (auto it = a_json.find("clearHeldItems"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'clearHeldItems' must be a boolean");
				}
				def.clearHeldItems = it->get<bool>();
			}
			def.fade = a_defaults.fade;
			if (auto it = a_json.find("fade"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'fade' must be a boolean");
				}
				def.fade = it->get<bool>();
			}
			def.inPlace = a_defaults.inPlace;
			if (auto it = a_json.find("inPlace"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'inPlace' must be a boolean");
				}
				def.inPlace = it->get<bool>();
			}
			// Input control is enabled-by-default (a_playerControlDefault is the built-in all-capabilities
			// grant unless the pack narrowed it). `"playerControl": false` turns it off; an object narrows
			// what was inherited via `disable`/`locked`.
			def.playerControl = a_defaults.playerControl;
			if (auto it = a_json.find("playerControl"); it != a_json.end()) {
				ApplyPlayerControl(*it, def.playerControl, "scene '" + def.id + "'");
			}
		}

		// ============================================================================
		// Scene parser — the one "scene" concept.
		// ============================================================================

		// Reject authored ids that collide with the synthetic desugar namespace.
		void RejectReservedId(const std::string& a_id, const char* a_what)
		{
			if (a_id.find('#') != std::string::npos) {
				throw std::runtime_error(std::string(a_what) + " id '" + a_id +
					"' may not contain '#' (reserved for synthetic stage nodes)");
			}
		}

		// Forward decl: the cue-lane parser is defined below (with the other node-lane parsers), but
		// ParseOsfStageList needs it so a linear stage can carry a `cue` lane like a node does.
		void ParseOsfCueLane(const json& a_entries, SceneNode& a_node);

		// Parse a stage list (timer/loops/clips, with the play-once default and the bare-array
		// shorthand) — the unified equivalent of the pack stages[] parse. a_ioActorCount is the
		// participant count: when a_fixed it is authoritative (every stage must match it); else the
		// first stage's clip count sets it.
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
					// `hold`: freeze on one frame instead of playing the clip. A frozen clip never
					// wraps, so a loop count could never expire — reject the pair rather than let it
					// read as "hold for N loops". A timer (or a manual advance) still leaves the stage.
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
					// Optional stage identity (label + tags) for the browsable-animation catalog.
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
					// Optional per-stage track lanes (cue/action/sound/camera): the lane parsers target a
					// SceneNode, so parse into a scratch node and move them onto the stage. DesugarLinear
					// forwards them to the stage's synthetic node, letting a linear stage carry cues,
					// actions, audio, and camera postures without authoring the full nodes[] graph form.
					// Inside a graph node's stages[] the runtime never reads per-stage lanes — reject
					// them there instead of silently discarding what the author wrote.
					if (!a_allowStageLanes &&
						(jStage.contains("cue") || jStage.contains("action") ||
							jStage.contains("sound") || jStage.contains("camera"))) {
						throw std::runtime_error(a_subject + ": track lanes on a stage inside a graph node are not "
							"supported — put cue/action/sound/camera on the node itself");
					}
					{
						SceneNode scratch;
						scratch.id = a_subject;  // diagnostics only
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
					info.loops = 1;  // untimed -> play once, then advance / end
				}
				// An untimed frozen stage holds its frame until something advances it (loops stay 0),
				// which is exactly the hold policy DesugarLinear reads.
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

		// Cue lane. Cues need an explicit `at`.
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

		// A unified graph node: `use` XOR inline `stages`, optional loop policy, edges, and the four
		// node-level track lanes (cue/action/sound/camera, flat — not nested under a `tracks` block).
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

			// Node loop policy — one `loops` int, identical to a linear stage:
			//   omitted -> once (play through, then take a `when:"end"` edge)
			//   0       -> hold (loop until advanced; the explicit terminal hold)
			//   N >= 1  -> loop N times, then take a `when:"loops"` edge
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
				n.loopMode = LoopMode::kOnce;  // default: play through once
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

			// Node-level track lanes (flat keys, not a `tracks` block).
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

			// A trigger:<cueId> edge must reference a cue emitted on this same node.
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


		// Cross-node validation of a graph scene: edge targets, entry-is-a-node, and action/sound role
		// references. Anonymous roles are intentionally unreferenceable.
		// Role-name references in the action/sound lanes, checked for BOTH scene forms — linear
		// scenes desugar to nodes too, and a typo'd lane role otherwise loads clean and misfires
		// silently at runtime (DEBUG-only logs in a shipped build).
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
				// Inline stage clip counts must match the declared cast: a mismatch otherwise
				// surfaces only mid-scene (a live scene aborts, or the node can never start).
				for (const auto& st : nd.stages) {
					if (!st.clips.empty() && st.clips.size() != def.roles.size()) {
						throw std::runtime_error("scene '" + def.id + "': node '" + nd.id + "' stage has " +
							std::to_string(st.clips.size()) + " clip(s) but the scene has " +
							std::to_string(def.roles.size()) + " role(s)");
					}
				}
			}
		}

		// The parsed `anchor` block (a scene's own, or a file-level default). given=false when the key is absent.
		// keyword/base are validated against loaded forms (any-of within each) but kept as FormIDs; offset corrects the ref transform.
		struct AnchorReq
		{
			std::vector<RE::TESFormID>      keywords;
			std::vector<RE::TESFormID>      baseForms;
			Animation::ParticipantPlacement offset{};
			bool                            given = false;
		};

		// Parse an `anchor` block: { keyword?: <ref|[refs]>, base?: <ref|[refs]>, offset?: { x, y, z, heading } }.
		// keyword/base parse exactly like a role's filters.keyword/filters.race; resolved now (any-of within each), and an unresolvable ref REJECTS the scene. 
		// At least one of keyword/base is required (else nothing could satisfy the requirement). a_subject labels diagnostics (a scene id, or a file label).
		AnchorReq ParseAnchorBlock(const json& a_json, const std::string& a_subject)
		{
			AnchorReq req;
			const auto it = a_json.find("anchor");
			if (it == a_json.end()) {
				return req;  // given = false
			}
			if (!it->is_object()) {
				throw std::runtime_error(a_subject + ": 'anchor' must be an object { keyword?, base?, offset? }");
			}
			const json& anchor = *it;
			req.given = true;

			// keyword/base: a single string or an array of strings; resolved to forms now.
			auto parseRefs = [&](const char* a_key, auto a_push) {
				auto kit = anchor.find(a_key);
				if (kit == anchor.end()) {
					return;
				}
				if (kit->is_string()) {
					a_push(kit->get<std::string>());
				} else if (kit->is_array()) {
					for (const auto& e : *kit) {
						if (!e.is_string()) {
							throw std::runtime_error(a_subject + ": anchor." + a_key + " entries must be strings");
						}
						a_push(e.get<std::string>());
					}
				} else {
					throw std::runtime_error(a_subject + ": anchor." + a_key + " must be a string or array of strings");
				}
			};
			parseRefs("keyword", [&](const std::string& a_ref) {
				auto* kw = Util::ResolveFormRef<RE::BGSKeyword>(a_ref);
				if (!kw) {
					throw std::runtime_error(a_subject + ": anchor.keyword '" + a_ref +
						"' is malformed, names an unloaded plugin, or isn't a Keyword (KYWD) (use \"Plugin.esm|0xLocalID\")");
				}
				req.keywords.push_back(kw->GetFormID());
			});
			parseRefs("base", [&](const std::string& a_ref) {
				const auto id = Util::ComposeFormID(a_ref);
				if (!id) {
					throw std::runtime_error(a_subject + ": anchor.base '" + a_ref +
						"' is malformed or names an unloaded plugin (use \"Plugin.esm|0xLocalID\")");
				}
				if (!RE::TESForm::LookupByID(*id)) {
					throw std::runtime_error(a_subject + ": anchor.base '" + a_ref + "' did not resolve to a form");
				}
				req.baseForms.push_back(*id);
			});
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

		// Parse one unified scene. a_defaults holds the file-level policy/catalog defaults;
		// a_packRoles are the ARRAY form of the file-level `roles` (inherited by a scene that omits its own);
		// a_roleRegistry is the OBJECT form (id -> reusable template a scene's `roles` references by id string
		// or { "id", ...overrides } object); a_propRegistry is the file-level `props` (id -> reusable
		// prop template an `osf.prop.attach` inherits from); a_anchorDefault is the file-level `anchor`
		// (likewise inherited).
		SceneDef ParseOsfScene(const json& a_json, std::vector<std::string>& a_warnings,
			const PackDefaults& a_defaults, std::optional<CameraState> a_cameraDefault,
			const std::vector<SceneRole>& a_packRoles, const RoleRegistry& a_roleRegistry,
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
			// File-level `tags` are UNION-ed with the scene's own (pack tags first, in author order) —
			// unlike `roles`, a scene declaring tags narrows nothing, it adds. Matchmaking is a set
			// membership test, so a pack-wide tag every scene must carry belongs at the file level.
			// De-duplicated case-insensitively via tagSet, which matchmaking queries anyway.
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
		// roles[]: unified participant list; `name` optional (anonymous positional slot). Entries are
		// inline role objects, or references to the file-level roles REGISTRY — a plain id string, or
		// an object { "id", ...overrides } that merge-patches the template (both expanded to ordinary
		// slots here; the three forms mix freely). A scene's own `roles` overrides the pack-level
		// `roles`; a scene that omits the key inherits them. A registry is NOT a default cast —
		// omitting `roles` under a registry falls through to clip-count inference.
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
		} else if (!a_packRoles.empty()) {
			// Inherit the pack-level roles (names, filters, offsets, equip): named slots are explicit,
			// so duplicates still reject the scene exactly like inline roles.
			rolesGiven = true;
			pendingRoles.reserve(a_packRoles.size());
			for (const auto& r : a_packRoles) {
				pendingRoles.push_back(PendingRole{ r, r.name.empty() ? RoleNameKind::kAnonymous : RoleNameKind::kExplicit, {} });
			}
		}
		// Runtime role names bind actors (StartSceneRoles) and target track entries, so they must be
		// unambiguous within a scene: explicit names are reserved (duplicates reject the scene),
		// automatic (template-derived) names are numbered past any collision, and anonymous slots
		// stay anonymous (intentionally unreferenceable).
		AssignRoleNames(def, pendingRoles);

			// Anchor requirement: the scene's own `anchor` block overrides the file-level default entirely (mirrors roles).
			{
				AnchorReq anchorReq = ParseAnchorBlock(a_json, "scene '" + def.id + "'");
				if (!anchorReq.given && a_anchorDefault.given) {
					anchorReq = a_anchorDefault;
				}
				def.anchorKeywords = anchorReq.keywords;
				def.anchorBaseForms = anchorReq.baseForms;
				def.anchorOffset = anchorReq.offset;
			}
			// An anchor-bound scene positions the cast AT the anchor — the exact thing inPlace turns off.
			if (def.inPlace && def.RequiresAnchor()) {
				throw std::runtime_error("scene '" + def.id + "': 'inPlace' cannot be combined with an 'anchor' requirement");
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
				// A hold node (loops:0) with no advance/timer/trigger edge holds until the consumer calls
				// StopScene — that is the explicit, intentional terminal-hold pattern, so it is not flagged.
				ValidateGraph(def, nodeIds);
				// Role inference, graph form: mirror the linear rule — a scene with no `roles` gets one
				// anonymous slot per clip in the entry node's first inline stage. Without this a
				// roles-less graph scene loads with ZERO roles and can never matchmake (the pool
				// filters on roles.size() == actor count). A `use` entry resolves post-load, so it
				// can't seed a count — those scenes must declare roles explicitly.
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
				// Linear scene: top-level clip/stages -> a synthetic node chain (desugar).
				size_t actorCount = rolesGiven ? def.roles.size() : 0;
				std::vector<StageDef> stages;
				if (hasClip) {
					const auto& clip = a_json.at("clip");
					if (actorCount != 0 && actorCount != 1) {
						throw std::runtime_error("scene '" + def.id + "': 'clip' is single-actor but " +
							std::to_string(actorCount) + " role(s) declared (use 'stages' for multi-actor)");
					}
					StageDef st;
					st.loops = 1;  // play once, then end
					st.clips.push_back(ParseStageClip(clip, clipRoot, "scene '" + def.id + "'"));
					actorCount = 1;
					stages.push_back(std::move(st));
				} else {
					stages = ParseOsfStageList(a_json.at("stages"), "scene '" + def.id + "'", actorCount, rolesGiven, clipRoot, a_propRegistry);
				}
				if (!rolesGiven) {
					def.roles.assign(actorCount, SceneRole{});  // synthesize anonymous slots
				}
				DesugarLinear(def, stages);
				ValidateRoleRefs(def);  // linear lanes reference roles too (graph scenes get this via ValidateGraph)
			}

			// Pack-level default camera (file-level "camera": "<state>"): attach that posture to the
			// entry node's enter so a scene picks it up without authoring a per-node camera track. The
			// state override is held by the ledger until scene-stop, so engaging it on the entry node
			// holds it across every stage. An explicit node-level camera track on the entry node wins.
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
			// Species (skeleton family) = the first clip's actor folder. Derived, not authored, so
			// every scene — vanilla creature pack or a hand-written mod scene — is classified the same
			// way playback picks the rig. Empty / loose / NAF clips leave it "human" (the default lane).
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
			PackDefaults               scene;
			std::string                clipRoot;
			std::optional<CameraState> camera = CameraState::kSceneOrbit;
		};

		struct OsfFileContents
		{
			std::vector<const json*> sceneJsons;
			std::vector<const json*> routeJsons;
			std::vector<SceneRole>   packRoles;
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

			for (const char* key : { "lockPlayer", "stripActors", "clearHeldItems", "fade", "unlisted", "inPlace" }) {
				if (const auto value = a_json.find(key); value != a_json.end() && !value->is_boolean()) {
					a_reject("'" + a_fileName + "': '" + std::string(key) + "' must be true or false");
					return false;
				}
			}
			a_out.scene.lockPlayer = a_json.value("lockPlayer", true);
			a_out.scene.stripActors = a_json.value("stripActors", !a_out.library);
			a_out.scene.clearHeldItems = a_json.value("clearHeldItems", true);
			a_out.scene.fade = a_json.value("fade", false);
			a_out.scene.unlisted = a_json.value("unlisted", false);
			a_out.scene.inPlace = a_json.value("inPlace", false);
			if (const auto controlIt = a_json.find("playerControl"); controlIt != a_json.end()) {
				try {
					ApplyPlayerControl(*controlIt, a_out.scene.playerControl, "'" + a_fileName + "'");
				} catch (const std::exception& e) {
					a_reject(e.what());
					return false;
				}
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
								a_out.packRoles.push_back(ParseRole(roleJson, "<pack:" + a_fileName + ">"));
							}
						} catch (const std::exception& e) {
							a_reject("'" + a_fileName + "': pack-level roles: " + std::string(e.what()));
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
								role.parsed = ParseRole(roleJson, "<pack:" + a_fileName + ">");
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
			} else if (!a_json.contains("clipLibrary") && !a_json.contains("routes")) {
				a_reject("'" + a_fileName + "': expected a bare scene 'id', 'scenes', 'routes', or 'clipLibrary'");
				return false;
			}
			if (const auto routesIt = a_json.find("routes"); routesIt != a_json.end()) {
				if (!routesIt->is_array()) {
					a_reject("'" + a_fileName + "': 'routes' must be an array");
					return false;
				}
				if (routesIt->size() > kMaxRoutesPerFile) {
					a_reject("'" + a_fileName + "': contains more than " +
						std::to_string(kMaxRoutesPerFile) + " routes");
					return false;
				}
				for (const auto& route : *routesIt) {
					a_out.routeJsons.push_back(&route);
				}
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

		std::string RequiredRouteString(const json& a_json, const char* a_key, const std::string& a_subject)
		{
			const auto it = a_json.find(a_key);
			if (it == a_json.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
				throw std::runtime_error(a_subject + ": '" + a_key + "' must be a non-empty string");
			}
			return it->get<std::string>();
		}

		float RouteFrame(const json& a_json, const char* a_key, const std::string& a_subject)
		{
			const auto it = a_json.find(a_key);
			if (it == a_json.end() || !it->is_number()) {
				throw std::runtime_error(a_subject + ": '" + a_key + "' must be a non-negative number");
			}
			const float frame = it->get<float>();
			if (!std::isfinite(frame) || frame < 0.0f || std::floor(frame) != frame) {
				throw std::runtime_error(a_subject + ": '" + a_key + "' must be a whole frame number >= 0");
			}
			return frame;
		}

		RouteLifetime ParseRouteLifetime(const json& a_json, const std::string& a_subject)
		{
			const auto value = RequiredRouteString(a_json, "lifetime", a_subject);
			const auto lower = ToLower(value);
			if (lower == "transition") return RouteLifetime::kTransition;
			if (lower == "station") return RouteLifetime::kStation;
			if (lower == "controller") return RouteLifetime::kController;
			if (lower == "external") return RouteLifetime::kExternal;
			throw std::runtime_error(a_subject + ": unknown lifetime '" + value +
				"' (expected transition, station, controller, or external)");
		}

		RouteLayer ParseRouteLayer(const json& a_json, std::string_view a_clipRoot,
			const std::string& a_subject, bool a_station)
		{
			if (!a_json.is_object()) {
				throw std::runtime_error(a_subject + ": 'layer' must be an object");
			}
			for (const auto& [key, value] : a_json.items()) {
				(void)value;
				if (key != "clip" && key != "mask" && key != "mode" && key != "weight" &&
					key != "holdAt") {
					throw std::runtime_error(a_subject + ": layer has unknown key '" + key + "'");
				}
			}
			const auto clipIt = a_json.find("clip");
			if (clipIt == a_json.end()) {
				throw std::runtime_error(a_subject + ": layer requires 'clip'");
			}
			RouteLayer out;
			out.clip = ParseStageClip(*clipIt, a_clipRoot, a_subject + " layer");
			const auto mask = RequiredRouteString(a_json, "mask", a_subject + " layer");
			const auto* named = Animation::BoneMask::Find(mask);
			if (!named) {
				throw std::runtime_error(a_subject + ": layer has unknown mask '" + mask +
					"' (known: " + Animation::BoneMask::KnownList() + ")");
			}
			out.mask = named->id;
			if (const auto modeIt = a_json.find("mode"); modeIt != a_json.end()) {
				if (!modeIt->is_string()) {
					throw std::runtime_error(a_subject + ": layer 'mode' must be 'override' or 'additive'");
				}
				const auto mode = ToLower(modeIt->get<std::string>());
				if (mode == "override") out.mode = Animation::PoseMode::kOverride;
				else if (mode == "additive") out.mode = Animation::PoseMode::kAdditive;
				else throw std::runtime_error(a_subject + ": layer has unknown mode '" + mode + "'");
			}
			if (const auto weightIt = a_json.find("weight"); weightIt != a_json.end()) {
				if (!weightIt->is_number()) {
					throw std::runtime_error(a_subject + ": layer 'weight' must be a finite number");
				}
				const auto normalized = Animation::NormalizePoseWeight(weightIt->get<double>());
				if (!normalized) {
					throw std::runtime_error(a_subject + ": layer 'weight' must be finite");
				}
				out.weight = *normalized;
			}
			if (const auto holdIt = a_json.find("holdAt"); holdIt != a_json.end()) {
				if (!a_station || !holdIt->is_number()) {
					throw std::runtime_error(a_subject + ": 'holdAt' is numeric and valid only on station layers");
				}
				out.holdAt = holdIt->get<float>();
				if (!std::isfinite(out.holdAt) || out.holdAt < 0.0f || out.holdAt > 1.0f) {
					throw std::runtime_error(a_subject + ": 'holdAt' must be in [0,1]");
				}
			}
			return out;
		}

		RouteDef ParseRoute(const json& a_json, const PropRegistry& a_props,
			std::string_view a_clipRoot)
		{
			if (!a_json.is_object()) {
				throw std::runtime_error("route definition must be an object");
			}
			RouteDef out;
			out.id = RequiredRouteString(a_json, "id", "route");
			RejectReservedId(out.id, "route");
			const std::string subject = "route '" + out.id + "'";
			for (const char* key : { "stripActors", "lockPlayer", "camera", "inPlace", "fade",
				"playerControl", "clearHeldItems", "roles", "claims", "conditions", "when" }) {
				if (a_json.contains(key)) {
					throw std::runtime_error(subject + ": policy/arbitration key '" + key + "' is not valid in route schema v1");
				}
			}
			const auto stationsIt = a_json.find("stations");
			if (stationsIt == a_json.end() || !stationsIt->is_array() || stationsIt->empty()) {
				throw std::runtime_error(subject + ": 'stations' must be a non-empty array");
			}
			std::unordered_set<std::string> stationIds;
			for (const auto& stationJson : *stationsIt) {
				if (!stationJson.is_object()) {
					throw std::runtime_error(subject + ": station must be an object");
				}
				RouteStation station;
				station.id = RequiredRouteString(stationJson, "id", subject + " station");
				if (!stationIds.insert(ToLower(station.id)).second) {
					throw std::runtime_error(subject + ": duplicate station id '" + station.id + "'");
				}
				if (const auto layer = stationJson.find("layer"); layer != stationJson.end()) {
					station.layer = ParseRouteLayer(*layer, a_clipRoot, subject + " station '" + station.id + "'", true);
				}
				out.stations.push_back(std::move(station));
			}

			const auto transitionsIt = a_json.find("transitions");
			if (transitionsIt == a_json.end() || !transitionsIt->is_array()) {
				throw std::runtime_error(subject + ": 'transitions' must be an array");
			}
			std::unordered_set<std::string> transitionIds;
			for (const auto& transitionJson : *transitionsIt) {
				if (!transitionJson.is_object()) {
					throw std::runtime_error(subject + ": transition must be an object");
				}
				RouteTransition transition;
				transition.id = RequiredRouteString(transitionJson, "id", subject + " transition");
				const std::string edgeSubject = subject + " transition '" + transition.id + "'";
				if (!transitionIds.insert(ToLower(transition.id)).second) {
					throw std::runtime_error(subject + ": duplicate transition id '" + transition.id + "'");
				}
				transition.from = RequiredRouteString(transitionJson, "from", edgeSubject);
				transition.to = RequiredRouteString(transitionJson, "to", edgeSubject);
				if (!stationIds.contains(ToLower(transition.from)) || !stationIds.contains(ToLower(transition.to))) {
					throw std::runtime_error(edgeSubject + ": from/to must name declared stations");
				}
				const auto layer = transitionJson.find("layer");
				if (layer == transitionJson.end()) {
					throw std::runtime_error(edgeSubject + ": transition requires an animated 'layer'");
				}
				transition.layer = ParseRouteLayer(*layer, a_clipRoot, edgeSubject, false);
				if (const auto interrupt = transitionJson.find("interrupt"); interrupt != transitionJson.end()) {
					if (!interrupt->is_string()) throw std::runtime_error(edgeSubject + ": 'interrupt' must be a string");
					const auto value = ToLower(interrupt->get<std::string>());
					if (value == "finish") transition.interruption = RouteInterruption::kFinish;
					else if (value == "crossfade-before-commit") transition.interruption = RouteInterruption::kCrossfadeBeforeCommit;
					else throw std::runtime_error(edgeSubject + ": unknown interrupt mode '" + value + "'");
				}
				if (const auto commit = transitionJson.find("commit"); commit != transitionJson.end()) {
					if (!commit->is_object()) throw std::runtime_error(edgeSubject + ": 'commit' must be an object");
					RouteMarker marker;
					marker.frame = RouteFrame(*commit, "atFrame", edgeSubject + " commit");
					marker.id = RequiredRouteString(*commit, "marker", edgeSubject + " commit");
					transition.commit = std::move(marker);
				}
				if (const auto markers = transitionJson.find("markers"); markers != transitionJson.end()) {
					if (!markers->is_array()) throw std::runtime_error(edgeSubject + ": 'markers' must be an array");
					for (const auto& markerJson : *markers) {
						if (!markerJson.is_object()) throw std::runtime_error(edgeSubject + ": marker must be an object");
						RouteMarker marker;
						marker.frame = RouteFrame(markerJson, "atFrame", edgeSubject + " marker");
						marker.id = RequiredRouteString(markerJson, "id", edgeSubject + " marker");
						if (markerJson.contains("lifetime")) {
							throw std::runtime_error(edgeSubject + " marker '" + marker.id +
								"': markers are instantaneous and do not accept 'lifetime'");
						}
						transition.markers.push_back(std::move(marker));
					}
				}
				if (const auto props = transitionJson.find("props"); props != transitionJson.end()) {
					if (!props->is_array()) throw std::runtime_error(edgeSubject + ": 'props' must be an array");
					for (const auto& propJson : *props) {
						if (!propJson.is_object()) throw std::runtime_error(edgeSubject + ": prop must be an object");
						RouteProp prop;
						prop.id = RequiredRouteString(propJson, "prop", edgeSubject + " prop");
						const bool attach = propJson.contains("attachAtFrame");
						const bool destroy = propJson.contains("destroyAtFrame");
						if (attach == destroy) throw std::runtime_error(edgeSubject + " prop '" + prop.id +
							"': specify exactly one of attachAtFrame or destroyAtFrame");
						prop.attach = attach;
						prop.frame = RouteFrame(propJson, attach ? "attachAtFrame" : "destroyAtFrame", edgeSubject + " prop '" + prop.id + "'");
						prop.lifetime = ParseRouteLifetime(propJson, edgeSubject + " prop '" + prop.id + "'");
						if (attach && prop.lifetime != RouteLifetime::kExternal) {
							json merged;
							const json& resolved = ResolvePropAttach(propJson, a_props, prop.id, edgeSubject + " prop '" + prop.id + "'", merged);
							const auto source = resolved.find("source");
							if (source == resolved.end()) throw std::runtime_error(edgeSubject + " prop '" + prop.id + "': attach requires source or a matching prop template");
							prop.source = ParsePropSource(*source, edgeSubject + " prop '" + prop.id + "'");
							prop.attachment = ParsePropAttachment(resolved, edgeSubject + " prop '" + prop.id + "'");
						}
						transition.props.push_back(std::move(prop));
					}
				}
				if (const auto sounds = transitionJson.find("sound"); sounds != transitionJson.end()) {
					if (!sounds->is_array()) throw std::runtime_error(edgeSubject + ": 'sound' must be an array");
					for (const auto& soundJson : *sounds) {
						if (!soundJson.is_object()) throw std::runtime_error(edgeSubject + ": sound must be an object");
						RouteSound sound;
						sound.frame = RouteFrame(soundJson, "atFrame", edgeSubject + " sound");
						sound.spec = RequiredRouteString(soundJson, "spec", edgeSubject + " sound");
						if (soundJson.contains("lifetime")) {
							throw std::runtime_error(edgeSubject + " sound: sounds are one-shot and do not accept 'lifetime'");
						}
						transition.sounds.push_back(std::move(sound));
					}
				}
				out.transitions.push_back(std::move(transition));
			}
			return out;
		}

		void LoadOsfRoutes(const OsfFileContents& a_contents, const OsfFileDefaults& a_defaults,
			const std::filesystem::path& a_file, std::unordered_map<std::string, RouteDef>& a_out,
			SceneLoadBudget& a_budget, ProblemSink& a_problems)
		{
			for (const auto* routeJson : a_contents.routeJsons) {
				const std::string authoredId = routeJson->is_object() && routeJson->contains("id") &&
					(*routeJson)["id"].is_string() ? (*routeJson)["id"].get<std::string>() : std::string{};
				try {
					if (a_budget.routes >= kMaxRoutesTotal) {
						throw std::runtime_error("route registry aggregate limit reached");
					}
					auto definition = ParseRoute(*routeJson, a_contents.props, a_defaults.clipRoot);
					definition.sourceFile = a_file;
					auto key = ToLower(definition.id);
					if (const auto found = a_out.find(key); found != a_out.end()) {
						throw std::runtime_error("duplicate route id '" + definition.id + "' (already from '" +
							found->second.sourceFile.filename().string() + "')");
					}
					a_out.emplace(std::move(key), std::move(definition));
					++a_budget.routes;
				} catch (const std::exception& e) {
					a_problems.Push("[error] '" + a_file.filename().string() + "': " + e.what(), a_file,
						"route-invalid", "Fix the named route field; only this route was skipped.", authoredId);
					REX::ERROR("[Registry] skipping route in '{}': {}", a_file.filename().string(), e.what());
				}
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
						a_contents.packRoles, a_contents.roles, a_contents.props,
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
			std::unordered_map<std::string, SceneDef>& a_out, std::unordered_map<std::string, RouteDef>& a_routes,
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
			LoadOsfRoutes(contents, defaults, a_file, a_routes, a_budget, a_problems);
		}


		// Post-load: resolve every node `use` against the loaded set. A use only splices the target's
		// single inline-stage node (RFC §9), so the target must exist and be a single-node inline scene.
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

		// Build an Animation::ScenePlan from a resolved (roles, stages) pair: a per-clip offset overrides
		// the role's default placement; every stage's clip count must equal a_actorCount. Shared by the
		// inline-stages and `use`-target node paths.
		std::optional<Animation::ScenePlan> BuildPlanFromStages(const std::string& a_id,
			const std::vector<SceneRole>& a_roles, const std::vector<StageDef>& a_stages, size_t a_actorCount)
		{
			if (a_stages.empty()) {
				return std::nullopt;
			}
			if (a_roles.size() != a_actorCount) {
				REX::WARN("[Registry] '{}' needs {} role(s), got {}", a_id, a_roles.size(), a_actorCount);
				return std::nullopt;
			}
			Animation::ScenePlan plan;
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
				Animation::ScenePlan::Stage stage;
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

	SlotGender ParseSlotGender(std::string_view a_str)
	{
		const auto s = ToLower(a_str);
		if (s == "male" || s == "m") {
			return SlotGender::kMale;
		}
		if (s == "female" || s == "f") {
			return SlotGender::kFemale;
		}
		return SlotGender::kAny;  // "any"/"" and anything else
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

	SceneRegistry& SceneRegistry::GetSingleton()
	{
		static SceneRegistry singleton;
		return singleton;
	}


	void SceneRegistry::LoadAll()
	{
		std::unordered_map<std::string, SceneDef> loaded;
		std::unordered_map<std::string, RouteDef> loadedRoutes;
		std::vector<std::string> errors;
		std::vector<PendingImportProblem> pendingProblems;
		ProblemSink problems{ errors, pendingProblems };
		std::vector<ClipLibraryRegistration> clipLibrary;
		std::vector<SceneFileStats> fileStats;
		std::unordered_map<std::string, std::size_t> fileIndex;  // full source path -> index into fileStats
		ClipInstalledCache clipCache;
		SceneLoadBudget loadBudget;

		auto ensureFileStats = [&](const Util::RegistryJsonSource& a_source) -> SceneFileStats& {
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
				if (const auto routes = a_json.find("routes"); routes != a_json.end() && routes->is_array()) {
					stats.declaredRoutes = static_cast<std::uint32_t>(routes->size());
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
				LoadOsfFile(a_json, a_source.file, loaded, loadedRoutes, clipLibrary, loadBudget, problems);
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

		// Resolve every node `use` now that the whole set is loaded (catches dangling refs at load).
		ValidateUseRefs(loaded, problems);

		// Hide scenes whose clips aren't installed (compat pack without its source mod).
		SweepClipAvailability(loaded, problems, clipCache);
		const auto sceneCount = loaded.size();
		AccumulateFileStats(loaded, clipCache, fileStats, fileIndex);
		for (const auto& [key, route] : loadedRoutes) {
			(void)key;
			if (const auto it = fileIndex.find(route.sourceFile.string()); it != fileIndex.end()) {
				++fileStats[it->second].routes;
			}
		}
		std::map<std::string, std::uint32_t> clipEntriesByFile;
		const auto clipEntryCount = AddSceneClipEntries(loaded, clipLibrary, problems, clipEntriesByFile);
		for (const auto& [path, count] : clipEntriesByFile) {
			if (const auto it = fileIndex.find(path); it != fileIndex.end()) {
				fileStats[it->second].clipEntries = count;
			}
		}
		for (auto& stats : fileStats) {
			stats.rejectedScenes = stats.declaredScenes > stats.scenes ? stats.declaredScenes - stats.scenes : 0;
			stats.rejectedRoutes = stats.declaredRoutes > stats.routes ? stats.declaredRoutes - stats.routes : 0;
		}

		const auto problemCount = errors.size();

		// Hand each structured record to its file. Anything unowned lands in one trailing bucket.
		SceneFileStats crossFile;
		for (std::size_t i = 0; i < errors.size() && i < pendingProblems.size(); ++i) {
			const auto& pending = pendingProblems[i];
			const auto it = fileIndex.find(pending.owner.string());
			auto& target = it != fileIndex.end() ? fileStats[it->second] : crossFile;
			(pending.warning ? target.warnings : target.errors) += 1;
			target.problems.push_back(SceneImportProblem{
				pending.warning, pending.code, errors[i], pending.hint, pending.scene, pending.node,
				pending.role, pending.clip
			});
		}
		std::sort(fileStats.begin(), fileStats.end(), [](const SceneFileStats& a_lhs, const SceneFileStats& a_rhs) {
			return a_lhs.path < a_rhs.path;
		});
		const auto fileCount = fileStats.size();  // files SCANNED — before the bucket, which is not one
		if (!crossFile.problems.empty()) {
			fileStats.push_back(std::move(crossFile));  // empty path — kept last on purpose
		}

		auto next = std::make_shared<SceneRegistrySnapshot>();
		next->scenes = std::move(loaded);
		next->routes = std::move(loadedRoutes);
		next->loadErrors = std::move(errors);
		next->authoredSceneCount = sceneCount;
		next->files = std::move(fileStats);
		const auto routeCount = next->routes.size();
		snapshot.store(std::move(next), std::memory_order_release);
		REX::INFO("[Registry] {} scene(s), {} route(s) loaded from {} file(s), {} scene clip entr{}, {} problem(s)",
			sceneCount, routeCount, fileCount, clipEntryCount, clipEntryCount == 1 ? "y" : "ies", problemCount);
	}

	SceneRef SceneRegistry::Find(std::string_view a_id) const
	{
		SceneRef out;
		out.owner = snapshot.load(std::memory_order_acquire);
		const auto it = out.owner->scenes.find(ToLower(std::string(a_id)));
		if (it != out.owner->scenes.end()) {
			out.value = &it->second;
		}
		return out;
	}

	RouteRef SceneRegistry::FindRoute(std::string_view a_id) const
	{
		RouteRef out;
		out.owner = snapshot.load(std::memory_order_acquire);
		const auto it = out.owner->routes.find(ToLower(std::string(a_id)));
		if (it != out.owner->routes.end()) {
			out.value = &it->second;
		}
		return out;
	}

	std::optional<Animation::ScenePlan> SceneRegistry::BuildNodePlan(const SceneRef& a_def, const SceneNode& a_node, size_t a_actorCount) const
	{
		if (!a_def) {
			return std::nullopt;
		}
		if (!a_node.use.empty()) {
			// A `use` node splices the target scene's single inline-stage node (validated at load,
			// re-checked here). The target's own roles supply the default placements.
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
				plan->anchored = !a_def->inPlace;  // the OWNING scene's posture governs, like its other policies
			}
			return plan;
		}
		// Inline node: this scene's roles supply the default placements.
		auto plan = BuildPlanFromStages(a_def->id, a_def->roles, a_node.stages, a_actorCount);
		if (plan) {
			// inPlace scene: no teleport/pin — the rig follows each actor's live transform, so the
			// player's heading/position (and with them the vanilla third-person camera) stay untouched.
			plan->anchored = !a_def->inPlace;
		}
		return plan;
	}

	void SceneRegistry::ForEachDef(const std::function<void(const SceneDef&)>& a_fn) const
	{
		const auto current = snapshot.load(std::memory_order_acquire);
		for (const auto& [key, def] : current->scenes) {
			a_fn(def);
		}
	}

	size_t SceneRegistry::Size() const
	{
		return snapshot.load(std::memory_order_acquire)->authoredSceneCount;
	}

	std::vector<std::string> SceneRegistry::LoadErrors() const
	{
		return snapshot.load(std::memory_order_acquire)->loadErrors;
	}

	std::vector<SceneFileStats> SceneRegistry::FileStats() const
	{
		return snapshot.load(std::memory_order_acquire)->files;
	}

	std::vector<std::string> SceneRegistry::MissingClipRefs() const
	{
		// Same probe as the load-time availability sweep (BSResource-aware, so archive-resident
		// vanilla .af clips are NOT false positives), re-run live for the diagnostic.
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
