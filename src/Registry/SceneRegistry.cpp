#include "Registry/SceneRegistry.h"

#include "Animation/GraphManager.h"  // ResourceExists (clip-availability probe)
#include "Input/InputTypes.h"
#include "Util/ClipPath.h"
#include "Util/FormRef.h"
#include "Util/Math.h"
#include "Util/Species.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <charconv>
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
				// Pre-measured duration (the generated vanilla packs carry these) — spares the
				// duration scan a per-clip walk of the game archive.
				if (auto sit = a_clip.find("sec"); sit != a_clip.end()) {
					if (!sit->is_number() || sit->get<float>() <= 0.0f) {
						throw std::runtime_error(a_subject + ": clip 'sec' must be a positive number");
					}
					clip.sec = sit->get<float>();
				}
			} else {
				throw std::runtime_error(a_subject + ": a clip must be a file string or a { file, anim?, offset?, sec? } object");
			}
			if (clip.file.empty()) {
				throw std::runtime_error(a_subject + ": empty clip file");
			}
			ApplyClipRoot(clip, a_clipRoot);
			return clip;
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

		// The built-in osf.* action mechanisms this build recognizes (a subset are executed;
		// the rest are validated + accepted but logged-not-executed by the runtime).
		bool IsKnownBuiltinAction(const std::string& a_typeLower)
		{
			static const std::unordered_set<std::string> kKnown{
				"osf.control.lock", "osf.control.release",
				"osf.equipment.hide", "osf.equipment.restore",
				"osf.equipment.equip", "osf.equipment.unequip",
				"osf.weapon.sheathe", "osf.weapon.restore",
				"osf.fade.out", "osf.fade.in",
				"osf.voice.play"
			};
			return kKnown.count(a_typeLower) != 0;
		}

		// Parse the timing fields shared by every track lane (cue/action/sound/camera): the
		// `repeat` flag and the `at` position (enter/exit/end anchor or a numeric clip-fraction
		// in [0,1)). Writes pos/fraction/everyLoop onto a_out, whose `pos` enum supplies the
		// kEnter/kExit/kEnd/kFraction values (all four lane enums share those names). a_subject is
		// the entry descriptor for diagnostics, e.g. "action 'osf.fade.out'". When a_atRequired,
		// a missing `at` is rejected (cues); otherwise a missing `at` defaults to the enter anchor.
		template <class Entry>
		void ParseTrackTiming(const json& a_entry, Entry& a_out, const std::string& a_nodeId,
			const std::string& a_subject, bool a_atRequired)
		{
			using Pos = decltype(a_out.pos);
			const auto atIt = a_entry.find("at");
			if (a_atRequired && atIt == a_entry.end()) {
				throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " is missing 'at'");
			}
			const auto repeat = ToLower(a_entry.value("repeat", "none"));
			if (repeat != "none" && repeat != "loop") {
				throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " has unknown repeat '" + repeat + "'");
			}
			a_out.everyLoop = (repeat == "loop");
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
				throw std::runtime_error("node '" + a_nodeId + "': " + a_subject + " 'at' must be a number or enter/exit/end");
			}
		}

		void ParseActionTrack(const json& a_entries, SceneNode& a_node_out)
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
				const auto typeLower = ToLower(ae.type);
				if (typeLower.rfind("osf.", 0) == 0) {
					if (!IsKnownBuiltinAction(typeLower)) {
						throw std::runtime_error("node '" + a_node_out.id + "': unknown built-in action '" + ae.type + "'");
					}
					// Per-action required fields: voice needs its sound set, equip its item. `role` is
					// OPTIONAL on every action — an omitted/empty role targets the scene's first
					// participant (ResolveRoleActor), matching the sound lane's default. A NAMED role
					// must still exist (ValidateGraph rejects undeclared references).
					if (typeLower == "osf.voice.play" && ae.set.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': action 'osf.voice.play' requires 'set'");
					}
					if (typeLower == "osf.equipment.equip" && ae.item.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': action 'osf.equipment.equip' requires 'item'");
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

		// Expand the `sound` ladder sugar: an object { role?, spec, repeat?, volume?, at } whose shared
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
		//     { at, tags?, spec?, role?, repeat?, volume? }  -> per-hit overrides (spec replaces the base)
		void ExpandSoundLadder(const json& a_lane, SceneNode& a_node_out)
		{
			const std::string baseSpec = a_lane.value("spec", a_lane.value("sound", a_lane.value("pool", std::string{})));
			const std::string laneRole = a_lane.value("role", std::string{});
			const float laneVolume = a_lane.value("volume", 1.0f);
			const std::string laneRepeat = ToLower(a_lane.value("repeat", "none"));  // ladders opt in with "loop"

			// Emit one entry; timing (at/repeat) reuses the shared track-timing parse + validation.
			const auto emit = [&](const std::string& a_spec, const json& a_at, const std::string& a_repeat,
				const std::string& a_role, float a_volume) {
				if (a_spec.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': a sound mark has no spec (set the lane 'spec' or a per-mark 'spec')");
				}
				SoundEntry se;
				se.spec = a_spec;
				se.role = a_role;
				se.volume = a_volume;
				json timing = json::object();
				timing["at"] = a_at;
				timing["repeat"] = a_repeat;
				ParseTrackTiming(timing, se, a_node_out.id, "sound '" + se.spec + "'", /*a_atRequired*/ true);
				a_node_out.sounds.push_back(std::move(se));
			};

			const auto positionsIt = a_lane.find("at");
			if (positionsIt == a_lane.end()) {
				throw std::runtime_error("node '" + a_node_out.id + "': a sound ladder needs 'at' (an array or tag-keyed object of positions)");
			}
			const json& positions = *positionsIt;

			if (positions.is_object()) {
				if (positions.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': sound ladder 'at' object is empty");
				}
				for (auto it = positions.begin(); it != positions.end(); ++it) {
					if (!it.value().is_array()) {
						throw std::runtime_error("node '" + a_node_out.id + "': sound ladder 'at' group '" + it.key() + "' must be an array of positions");
					}
					std::string spec = baseSpec;
					if (!it.key().empty()) {
						spec += "," + it.key();  // the group key is the tag(s) appended to the base
					}
					for (const auto& at : it.value()) {
						emit(spec, at, laneRepeat, laneRole, laneVolume);
					}
				}
				return;
			}
			if (!positions.is_array() || positions.empty()) {
				throw std::runtime_error("node '" + a_node_out.id + "': sound ladder 'at' must be a non-empty array or an object keyed by tag");
			}
			for (const auto& m : positions) {
				if (m.is_number() || m.is_string()) {
					emit(baseSpec, m, laneRepeat, laneRole, laneVolume);  // bare position -> base spec
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
					emit(spec, m[0], laneRepeat, laneRole, laneVolume);
				} else if (m.is_object()) {
					std::string spec = baseSpec;
					if (auto it = m.find("spec"); it != m.end()) {
						spec = it->get<std::string>();  // per-hit spec replaces the base
					}
					if (auto it = m.find("tags"); it != m.end()) {
						spec += "," + it->get<std::string>();
					}
					const json at = m.contains("at") ? m.at("at") : json();
					emit(spec, at, ToLower(m.value("repeat", laneRepeat)), m.value("role", laneRole), m.value("volume", laneVolume));
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
					if (auto it = s.find("at"); it != s.end() && (it->is_array() || it->is_object())) {
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
				se.volume = s.value("volume", 1.0f);
				ParseTrackTiming(s, se, a_node_out.id, "sound '" + se.spec + "'", /*a_atRequired*/ false);
				a_node_out.sounds.push_back(std::move(se));
			}
		}

		// The camera postures the runtime understands — shared by node `camera` tracks and the
		// pack-level `camera` default. (Tethered orbit / photo mode / cinematic between-actor shots
		// aren't wired yet.)
		bool IsKnownCameraState(std::string_view a_stateLower)
		{
			return a_stateLower == "thirdperson_hold" || a_stateLower == "freefly" ||
			       a_stateLower == "vanity_orbit" || a_stateLower == "scene_orbit";
		}

		void ParseCameraTrack(const json& a_entries, SceneNode& a_node_out)
		{
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node_out.id + "': 'camera' track must be an array");
			}
			for (const auto& c : a_entries) {
				CameraEntry ce;
				ce.state = c.value("state", std::string{});
				if (ce.state.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': a camera track entry is missing 'state'");
				}
				if (!IsKnownCameraState(ToLower(ce.state))) {
					throw std::runtime_error("node '" + a_node_out.id + "': unknown camera state '" + ce.state +
						"' (supported: 'thirdperson_hold', 'freefly', 'vanity_orbit', 'scene_orbit')");
				}
				ParseTrackTiming(c, ce, a_node_out.id, "camera '" + ce.state + "'", /*a_atRequired*/ false);
				ce.distance = c.value("distance", 0.0f);  // thirdperson_hold opening zoom; 0 = engine default
				if (ce.distance != 0.0f && ToLower(ce.state) != "thirdperson_hold") {
					REX::DEBUG("[Registry] node '{}': camera 'distance' is only honored for 'thirdperson_hold' — state '{}' ignores it",
						a_node_out.id, ce.state);
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

		// Top-level metadata (name/priority/weight/unlisted/lockPlayer/stripActors/fade/playerControl). id, tags,
		// roles, and the playable (clip/stages/nodes) are parsed by the caller. a_lockDefault/
		// a_stripDefault/a_fadeDefault/a_unlistedDefault seed the policy opt-outs (the file-level defaults).
		void ParseSceneMeta(const json& a_json, SceneDef& def, bool a_lockDefault, bool a_stripDefault, bool a_fadeDefault,
			bool a_unlistedDefault, bool a_inPlaceDefault)
		{
			def.name = a_json.value("name", def.id);
			def.priority = a_json.value("priority", 0);
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
			def.unlisted = a_unlistedDefault;
			if (auto it = a_json.find("unlisted"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'unlisted' must be a boolean");
				}
				def.unlisted = it->get<bool>();
			}
			def.lockPlayer = a_lockDefault;
			if (auto it = a_json.find("lockPlayer"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'lockPlayer' must be a boolean");
				}
				def.lockPlayer = it->get<bool>();
			}
			def.stripActors = a_stripDefault;
			if (auto it = a_json.find("stripActors"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'stripActors' must be a boolean");
				}
				def.stripActors = it->get<bool>();
			}
			def.fade = a_fadeDefault;
			if (auto it = a_json.find("fade"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'fade' must be a boolean");
				}
				def.fade = it->get<bool>();
			}
			def.inPlace = a_inPlaceDefault;
			if (auto it = a_json.find("inPlace"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'inPlace' must be a boolean");
				}
				def.inPlace = it->get<bool>();
			}
			// Input control is enabled-by-default (def.playerControl starts enabled with all capabilities).
			// `"playerControl": false` turns it off; an object narrows it via `disable`/`locked`.
			if (auto it = a_json.find("playerControl"); it != a_json.end()) {
				if (it->is_boolean()) {
					def.playerControl.enabled = it->get<bool>();
				} else if (it->is_object()) {
					if (auto en = it->find("enabled"); en != it->end()) {
						if (!en->is_boolean()) {
							throw std::runtime_error("scene '" + def.id + "': 'playerControl.enabled' must be a boolean");
						}
						def.playerControl.enabled = en->get<bool>();
					}
					if (auto d = it->find("disable"); d != it->end()) {
						if (!d->is_array()) {
							throw std::runtime_error("scene '" + def.id + "': 'playerControl.disable' must be an array of strings");
						}
						for (const auto& v : *d) {
							if (!v.is_string()) {
								throw std::runtime_error("scene '" + def.id + "': 'playerControl.disable' entries must be strings");
							}
							const auto name = v.get<std::string>();
							const auto bit = Input::CapabilityBit(name);
							if (bit == 0) {
								REX::WARN("[Registry] scene '{}': unknown playerControl capability '{}' — ignored (typo, or a newer OSF Animation?)", def.id, name);
							}
							def.playerControl.capabilities &= ~bit;  // remove from the default-all set
						}
					}
					if (auto r = it->find("controlRole"); r != it->end()) {
						if (!r->is_string()) {
							throw std::runtime_error("scene '" + def.id + "': 'playerControl.controlRole' must be a string");
						}
						def.playerControl.controlRole = r->get<std::string>();
					}
					if (auto lk = it->find("locked"); lk != it->end()) {
						if (!lk->is_boolean()) {
							throw std::runtime_error("scene '" + def.id + "': 'playerControl.locked' must be a boolean");
						}
						def.playerControl.locked = lk->get<bool>();
					}
				} else {
					throw std::runtime_error("scene '" + def.id + "': 'playerControl' must be a boolean or an object");
				}
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
					info.timer = jStage.value("timer", 0.0f);
					info.loops = jStage.value("loops", 0);
					timingGiven = jStage.contains("timer") || jStage.contains("loops");
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
					{
						SceneNode scratch;
						scratch.id = a_subject;  // diagnostics only
						if (auto it = jStage.find("cue"); it != jStage.end()) {
							ParseOsfCueLane(*it, scratch);
						}
						if (auto it = jStage.find("action"); it != jStage.end()) {
							ParseActionTrack(*it, scratch);
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
				if (!timingGiven) {
					info.loops = 1;  // untimed -> play once, then advance / end
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
		SceneNode ParseOsfNode(const json& a_node, std::vector<std::string>& a_warnings, const std::string& a_sceneId, std::string_view a_clipRoot)
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
				n.stages = ParseOsfStageList(a_node.at("stages"), "node '" + n.id + "'", ac, /*a_fixed*/ false, a_clipRoot);
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
				ParseActionTrack(*it, n);
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

		// Rewrite a linear scene (top-level clip/stages, no nodes[]) into a synthetic node chain so the
		// runtime only ever sees graph-shaped data. Fills def.nodes/entry/linearStages. Per stage:
		// timer>0 -> hold + timer edge; loops>0 -> count + loops edge; both -> count + both edges;
		// neither (explicit hold) -> hold-forever, no auto edge. See RFC §4.
		void DesugarLinear(SceneDef& def, const std::vector<StageDef>& a_stages)
		{
			const size_t n = a_stages.size();
			for (size_t i = 0; i < n; i++) {
				const auto& st = a_stages[i];
				SceneNode node;
				node.id = "#s" + std::to_string(i);
				node.stages = { st };
				// Forward the stage's track lanes onto the node, where the runtime's dispatch reads them.
				node.cues = st.cues;
				node.actions = st.actions;
				node.sounds = st.sounds;
				node.cameras = st.cameras;
				const std::string to = (i + 1 == n) ? std::string("$end") : ("#s" + std::to_string(i + 1));
				auto autoEdge = [&](EdgeWhen a_when) {
					SceneEdge e;
					e.to = to;
					e.when = a_when;
					return e;
				};
				if (st.timer > 0.0f && st.loops > 0) {
					node.loopMode = LoopMode::kCount;
					node.loopCount = st.loops;
					node.timerSec = st.timer;
					node.edges.push_back(autoEdge(EdgeWhen::kTimer));
					node.edges.push_back(autoEdge(EdgeWhen::kLoops));
				} else if (st.timer > 0.0f) {
					node.loopMode = LoopMode::kHold;
					node.timerSec = st.timer;
					node.edges.push_back(autoEdge(EdgeWhen::kTimer));
				} else if (st.loops > 0) {
					node.loopMode = LoopMode::kCount;
					node.loopCount = st.loops;
					node.edges.push_back(autoEdge(EdgeWhen::kLoops));
				} else {
					node.loopMode = LoopMode::kHold;  // explicit hold (timer:0, loops:0): hold until the player advances
				}
				// Every linear stage also gets a DEFAULT advance edge so the player can step to the next
				// stage manually (space / AdvanceScene), independent of any timer/loops auto-end above.
				// It carries no id/label (it isn't a branch choice — AdvanceEdges skips id-less edges).
				{
					SceneEdge adv = autoEdge(EdgeWhen::kAdvance);
					adv.isDefault = true;
					node.edges.push_back(std::move(adv));
				}
				def.linearStages.push_back(node.id);
				def.nodes.push_back(std::move(node));
			}
			def.entry = "#s0";
		}

		// Cross-node validation of a graph scene: edge targets, entry-is-a-node, and action/sound role
		// references. Anonymous roles are intentionally unreferenceable.
		void ValidateGraph(const SceneDef& def, const std::unordered_set<std::string>& a_nodeIds)
		{
			if (!a_nodeIds.count(ToLower(def.entry))) {
				throw std::runtime_error("scene '" + def.id + "': entry '" + def.entry + "' is not a node");
			}
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
				for (const auto& e : nd.edges) {
					if (e.to != "$end" && !a_nodeIds.count(ToLower(e.to))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + nd.id + "' edge targets missing node '" + e.to + "'");
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

		// Parse one unified scene. a_lockDefault/a_stripDefault/a_fadeDefault/a_unlistedDefault are the file-level policy defaults;
		// a_packRoles are the file-level `roles` (inherited by a scene that omits its own);
		// a_anchorDefault is the file-level `anchor` (likewise inherited).
		SceneDef ParseOsfScene(const json& a_json, std::vector<std::string>& a_warnings, bool a_lockDefault, bool a_stripDefault,
			bool a_fadeDefault, bool a_unlistedDefault, bool a_inPlaceDefault, std::string_view a_cameraDefault, const std::vector<SceneRole>& a_packRoles,
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
			ParseSceneMeta(a_json, def, a_lockDefault, a_stripDefault, a_fadeDefault, a_unlistedDefault, a_inPlaceDefault);
			if (const auto it = a_json.find("tags"); it != a_json.end()) {
				for (const auto& t : *it) {
					def.tags.push_back(t.get<std::string>());
				}
			}
			for (const auto& t : def.tags) {
				def.tagSet.insert(ToLower(t));
			}
			// roles[]: unified participant list; `name` optional (anonymous positional slot). A scene's
			// own `roles` overrides the pack-level `roles`; a scene that omits the key inherits them.
			bool rolesGiven = false;
			if (const auto it = a_json.find("roles"); it != a_json.end()) {
				if (!it->is_array()) {
					throw std::runtime_error("scene '" + def.id + "': 'roles' must be an array");
				}
				rolesGiven = true;
				for (const auto& jRole : *it) {
					def.roles.push_back(ParseRole(jRole, def.id));
				}
			} else if (!a_packRoles.empty()) {
				def.roles = a_packRoles;  // inherit the pack-level roles (names, filters, offsets, equip)
				rolesGiven = true;
			}

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
					auto nd = ParseOsfNode(jNode, a_warnings, def.id, clipRoot);
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
					stages = ParseOsfStageList(a_json.at("stages"), "scene '" + def.id + "'", actorCount, rolesGiven, clipRoot);
				}
				if (!rolesGiven) {
					def.roles.assign(actorCount, SceneRole{});  // synthesize anonymous slots
				}
				DesugarLinear(def, stages);
			}

			// Pack-level default camera (file-level "camera": "<state>"): attach that posture to the
			// entry node's enter so a scene picks it up without authoring a per-node camera track. The
			// state override is held by the ledger until scene-stop, so engaging it on the entry node
			// holds it across every stage. An explicit node-level camera track on the entry node wins.
			if (!a_cameraDefault.empty()) {
				const std::string entryLower = ToLower(def.entry);
				for (auto& nd : def.nodes) {
					if (ToLower(nd.id) == entryLower) {
						if (nd.cameras.empty()) {
							CameraEntry ce;
							ce.state = std::string(a_cameraDefault);  // already validated + lowercased by the caller
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

		void LoadOsfFile(const json& a_json, const std::filesystem::path& a_file,
			std::unordered_map<std::string, SceneDef>& a_out, std::vector<std::string>& a_errors)
		{
			const std::string fileName = a_file.filename().string();

			const auto it = a_json.find("schema");
			if (it == a_json.end() || !it->is_number_integer()) {
				a_errors.push_back("[error] '" + fileName + "': missing/non-integer 'schema'");
				REX::ERROR("[Registry] '{}' missing/non-integer 'schema' — skipped", fileName);
				return;
			}
			const auto schema = it->get<std::int64_t>();
			if (schema != kSchemaVersion) {
				a_errors.push_back("[error] '" + fileName + "': *.osf.json schema " + std::to_string(schema) +
					" unsupported (expected " + std::to_string(kSchemaVersion) + ")");
				REX::ERROR("[Registry] '{}' declares osf schema {} but this build expects {} — skipped",
					fileName, schema, kSchemaVersion);
				return;
			}

			// File-level catalog lane. "library" = reference content (the generated vanilla packs):
			bool library = false;
			if (const auto secIt = a_json.find("section"); secIt != a_json.end()) {
				const std::string section = secIt->is_string() ? ToLower(secIt->get<std::string>()) : std::string{};
				if (section == "library") {
					library = true;
				} else {
					a_errors.push_back("[error] '" + fileName + "': unknown 'section' value (supported: 'library')");
					REX::ERROR("[Registry] '{}' unknown 'section' value — skipped", fileName);
					return;
				}
			}

			// Optional content-pack label: the browser groups scenes under it, so a pack spanning
			// many files (one per furniture, say) still reads as ONE collapsible entry. File-level
			// only — every scene in the file inherits it; absent = the browser falls back to grouping
			// by the file itself.
			std::string packName;
			if (const auto pit = a_json.find("pack"); pit != a_json.end()) {
				if (!pit->is_string()) {
					a_errors.push_back("[error] '" + fileName + "': 'pack' must be a string");
					REX::ERROR("[Registry] '{}' 'pack' must be a string — skipped", fileName);
					return;
				}
				packName = pit->get<std::string>();
			}

			const bool lockDefault = a_json.value("lockPlayer", true);
			// Library packs default to NO strip
			const bool stripDefault = a_json.value("stripActors", !library);
			const bool fadeDefault = a_json.value("fade", false);
			const bool unlistedDefault = a_json.value("unlisted", false);
			// Pack-level `inPlace:true` = every scene plays on the actors where they stand (no teleport,
			// no per-frame root/heading pin) — the emote-pack posture; scenes may override per-scene.
			const bool inPlaceDefault = a_json.value("inPlace", false);
			std::string packClipRoot;
			if (auto crit = a_json.find("clipRoot"); crit != a_json.end()) {
				if (!crit->is_string()) {
					a_errors.push_back("[error] '" + fileName + "': 'clipRoot' must be a string");
					REX::ERROR("[Registry] '{}' 'clipRoot' must be a string — skipped", fileName);
					return;
				}
				try {
					packClipRoot = NormalizeClipRoot(crit->get<std::string>(), "'" + fileName + "'");
				} catch (const std::exception& e) {
					a_errors.push_back(std::string("[error] ") + e.what());
					REX::ERROR("[Registry] {} — skipped", e.what());
					return;
				}
			}

			// Pack-level default camera: "camera": "<state>" attaches that posture to each scene's
			// entry node (unless that node already declares its own camera track). The default orbit
			// bootstraps native TFC's close-actor renderer policy, then hands transform control to OSF
			// for automatic cast framing and orbit input. Pure native freefly remains author-selectable.
			std::string cameraDefault = "scene_orbit";
			if (const auto cit = a_json.find("camera"); cit != a_json.end()) {
				if (!cit->is_string()) {
					a_errors.push_back("[error] '" + fileName + "': 'camera' must be a string");
					REX::ERROR("[Registry] '{}' 'camera' must be a string — skipped", fileName);
					return;
				}
				cameraDefault = ToLower(cit->get<std::string>());
				// "none" opts a pack out of the default camera override entirely.
				if (cameraDefault == "none") {
					cameraDefault.clear();
				} else if (!IsKnownCameraState(cameraDefault)) {
					a_errors.push_back("[error] '" + fileName + "': unknown camera state '" + cit->get<std::string>() +
						"' (supported: 'thirdperson_hold', 'freefly', 'vanity_orbit', 'scene_orbit', 'none')");
					REX::ERROR("[Registry] '{}' unknown camera state '{}' — skipped", fileName, cit->get<std::string>());
					return;
				}
			}

			// A file holds a single bare scene, or { schema, scenes: [...] }. In the multi-scene form a
			// file-level `roles` is a PACK default inherited by every scene that omits its own `roles`
			// (a bare file's top-level `roles` is just that one scene's roles, parsed by ParseOsfScene).
			std::vector<const json*> sceneJsons;
			std::vector<SceneRole>   packRoles;
			AnchorReq                packAnchor;  // file-level `anchor` default (multi-scene envelope only)
			if (auto sit = a_json.find("scenes"); sit != a_json.end()) {
				if (!sit->is_array()) {
					a_errors.push_back("[error] '" + fileName + "': 'scenes' must be an array");
					REX::ERROR("[Registry] '{}' 'scenes' must be an array — skipped", fileName);
					return;
				}
				try {
					packAnchor = ParseAnchorBlock(a_json, "'" + fileName + "' file-level anchor");
				} catch (const std::exception& e) {
					a_errors.push_back(std::string("[error] ") + e.what());
					REX::ERROR("[Registry] {} — skipped", e.what());
					return;
				}
				if (auto rit = a_json.find("roles"); rit != a_json.end()) {
					if (!rit->is_array()) {
						a_errors.push_back("[error] '" + fileName + "': pack-level 'roles' must be an array");
						REX::ERROR("[Registry] '{}' pack-level 'roles' must be an array — skipped", fileName);
						return;
					}
					try {
						for (const auto& jRole : *rit) {
							packRoles.push_back(ParseRole(jRole, "<pack:" + fileName + ">"));
						}
					} catch (const std::exception& e) {
						a_errors.push_back("[error] '" + fileName + "': pack-level roles: " + e.what());
						REX::ERROR("[Registry] '{}' pack-level roles rejected: {} — skipped", fileName, e.what());
						return;
					}
				}
				for (const auto& s : *sit) {
					sceneJsons.push_back(&s);
				}
			} else {
				sceneJsons.push_back(&a_json);  // bare single-scene file
			}

			for (const auto* sj : sceneJsons) {
				std::vector<std::string> warnings;
				try {
					auto def = ParseOsfScene(*sj, warnings, lockDefault, stripDefault, fadeDefault, unlistedDefault, inPlaceDefault, cameraDefault, packRoles, packClipRoot, packAnchor);
					def.sourceFile = a_file;
					def.pack = packName;
					def.library = library;
					auto key = ToLower(def.id);
					if (const auto f = a_out.find(key); f != a_out.end()) {
						a_errors.push_back("[error] duplicate scene id '" + def.id + "' in '" + fileName +
							"' (already from '" + f->second.sourceFile.filename().string() + "') — keeping the first");
						REX::ERROR("[Registry] duplicate scene id '{}' in '{}' — keeping first from '{}'",
							def.id, fileName, f->second.sourceFile.filename().string());
						continue;
					}
					for (const auto& w : warnings) {
						a_errors.push_back("[warn] " + w);
						REX::WARN("[Registry] {}", w);
					}
					REX::DEBUG("[Registry] loaded scene '{}' ({} node(s)) from '{}'", def.id, def.nodes.size(), fileName);
					a_out[std::move(key)] = std::move(def);
				} catch (const std::exception& e) {
					a_errors.push_back("[error] '" + fileName + "': " + e.what());
					REX::ERROR("[Registry] skipping scene in '{}': {}", fileName, e.what());
				}
			}
		}

		// Post-load: resolve every node `use` against the loaded set. A use only splices the target's
		// single inline-stage node (RFC §9), so the target must exist and be a single-node inline scene.
		void ValidateUseRefs(const std::unordered_map<std::string, SceneDef>& a_scenes, std::vector<std::string>& a_errors)
		{
			for (const auto& [key, def] : a_scenes) {
				for (const auto& nd : def.nodes) {
					if (nd.use.empty()) {
						continue;
					}
					const auto tit = a_scenes.find(ToLower(nd.use));
					if (tit == a_scenes.end()) {
						a_errors.push_back("[error] scene '" + def.id + "' node '" + nd.id +
							"': use references unknown scene '" + nd.use + "'");
						REX::ERROR("[Registry] scene '{}' node '{}' use references unknown scene '{}'", def.id, nd.id, nd.use);
						continue;
					}
					const auto& target = tit->second;
					if (target.nodes.size() != 1 || target.nodes[0].stages.empty()) {
						a_errors.push_back("[error] scene '" + def.id + "' node '" + nd.id + "': use target '" + nd.use +
							"' is not a single inline-stage scene (use splices one node's stages)");
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
			plan.stages.reserve(a_stages.size());
			for (const auto& sd : a_stages) {
				if (sd.clips.size() != a_actorCount) {
					REX::WARN("[Registry] '{}' stage has {} clip(s) but {} role(s)", a_id, sd.clips.size(), a_actorCount);
					return std::nullopt;
				}
				Animation::ScenePlan::Stage stage;
				stage.timer = sd.timer;
				stage.loops = sd.loops;
				for (size_t a = 0; a < a_actorCount; a++) {
					const auto& clip = sd.clips[a];
					stage.files.push_back(clip.file);
					stage.animIds.push_back(clip.animId);
					stage.placements.push_back(clip.offset.value_or(a_roles[a].offset));
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

	namespace
	{
		// True when an authored clip spec resolves to SOMETHING the runtime could open: any
		// ResolveClipSpec candidate that either opens through BSResource (loose OR archive-resident —
		// the vanilla packs' .af clips live inside the game BA2s) or, for an absolute spec outside
		// Data, exists on disk. Mirrors GraphManager::LoadClip's candidate walk, minus the decode.
		bool ClipSpecInstalled(const std::string& a_spec)
		{
			const auto spec = Util::ResolveClipSpec(std::filesystem::path{ a_spec });
			for (const auto& cand : spec.candidates) {
				if (cand.resource) {
					if (Animation::ResourceExists(cand.resourcePath)) {
						return true;
					}
				} else {
					std::error_code ec;
					if (std::filesystem::exists(cand.filePath, ec)) {
						return true;
					}
				}
			}
			return false;
		}

		// Availability sweep over a freshly loaded scene set: a scene referencing at least one clip
		// with no installed file is marked !clipsAvailable (hidden from the catalog + matchmaking,
		// still registered for direct-id starts and diagnostics). Compat packs ship scene JSON that
		// points at another mod's files, so "pack installed without its source mod" must degrade to
		// hidden scenes, not a browser full of unplayable cards. One warning per source file.
		void SweepClipAvailability(std::unordered_map<std::string, SceneDef>& a_scenes, std::vector<std::string>& a_errors)
		{
			std::unordered_map<std::string, bool> cache;  // per unique spec; packs repeat clips across stages
			const auto installed = [&cache](const std::string& a_file) {
				auto [it, fresh] = cache.try_emplace(a_file, false);
				if (fresh) {
					it->second = ClipSpecInstalled(a_file);
				}
				return it->second;
			};

			struct FileTally
			{
				int         hidden = 0;
				std::string firstMissing;
			};
			std::map<std::string, FileTally> byFile;  // ordered so the warning list is deterministic
			for (auto& [key, def] : a_scenes) {
				std::string missing;
				for (const auto& node : def.nodes) {
					for (const auto& stage : node.stages) {
						for (const auto& clip : stage.clips) {
							if (!installed(clip.file)) {
								missing = clip.file;
								break;
							}
						}
						if (!missing.empty()) {
							break;
						}
					}
					if (!missing.empty()) {
						break;
					}
				}
				if (!missing.empty()) {
					def.clipsAvailable = false;
					auto& tally = byFile[def.sourceFile.filename().string()];
					++tally.hidden;
					if (tally.firstMissing.empty()) {
						tally.firstMissing = missing;
					}
				}
			}
			for (const auto& [file, tally] : byFile) {
				a_errors.push_back("[warn] '" + file + "': " + std::to_string(tally.hidden) +
					" scene(s) hidden — clips not installed (e.g. '" + tally.firstMissing +
					"'); install the animation pack this file references");
				REX::WARN("[Registry] '{}': {} scene(s) hidden — clips not installed (e.g. '{}')",
					file, tally.hidden, tally.firstMissing);
			}
		}

		// Publish every distinct clip referenced by a non-library scene as a generated one-actor,
		// one-stage definition in the library lane. This is the browser's clip-level debug surface:
		// a multi-actor scene can still be inspected one raw clip at a time without authors having to
		// mint parallel solo scenes. Generated vanilla/reference-library scenes and emotes are
		// deliberately excluded because they already populate Animations.
		std::size_t AddSceneClipEntries(std::unordered_map<std::string, SceneDef>& a_scenes)
		{
			struct ClipEntry
			{
				std::string           display;
				StageClip             clip;
				std::filesystem::path sourceFile;
				std::string           pack;
			};

			// The source map is unordered. Sort first so both de-dup winners and generated IDs stay
			// stable across launches and ReloadPacks calls.
			std::vector<const SceneDef*> sources;
			sources.reserve(a_scenes.size());
			for (const auto& [key, def] : a_scenes) {
				const bool alreadyAnEmote = std::ranges::any_of(def.tagSet,
					[](const std::string& a_tag) { return a_tag.starts_with("player.emote."); });
				if (!def.library && !alreadyAnEmote) {
					sources.push_back(&def);
				}
			}
			std::sort(sources.begin(), sources.end(), [](const SceneDef* a_lhs, const SceneDef* a_rhs) {
				const auto lf = ToLower(a_lhs->sourceFile.filename().string());
				const auto rf = ToLower(a_rhs->sourceFile.filename().string());
				return lf != rf ? lf < rf : ToLower(a_lhs->id) < ToLower(a_rhs->id);
			});

			std::map<std::string, ClipEntry> unique;
			std::unordered_map<std::string, bool> installed;
			for (const SceneDef* def : sources) {
				// `pack` is the browser's preferred group key. Without one, the browser groups by
				// source filename, so use that same identity for clip de-duplication.
				const std::string group = !def->pack.empty()
				                              ? "pack:" + ToLower(def->pack)
				                              : "file:" + ToLower(def->sourceFile.filename().string());
				for (const auto& node : def->nodes) {
					for (const auto& stage : node.stages) {
						for (const auto& clip : stage.clips) {
							const std::string display = Util::ClipSpecDisplay(std::filesystem::path{ clip.file });
							const std::string installKey = ToLower(display);
							auto [iit, fresh] = installed.try_emplace(installKey, false);
							if (fresh) {
								iit->second = ClipSpecInstalled(clip.file);
							}
							if (!iit->second) {
								continue;  // an Animations entry must be runnable even if its parent scene is not
							}

							const std::string key = group + '\n' + ToLower(display) + '\n' + clip.animId;
							unique.try_emplace(key, ClipEntry{ display, clip, def->sourceFile, def->pack });
						}
					}
				}
			}

			const auto stableHash = [](std::string_view a_text) {
				std::uint64_t hash = 14695981039346656037ull;  // FNV-1a 64
				for (const unsigned char ch : a_text) {
					hash ^= ch;
					hash *= 1099511628211ull;
				}
				return hash;
			};

			std::size_t added = 0;
			for (auto& [key, entry] : unique) {
				std::string id = std::format("osf.scene-clip/{:016x}", stableHash(key));
				for (std::uint32_t collision = 1; a_scenes.contains(ToLower(id)); ++collision) {
					id = std::format("osf.scene-clip/{:016x}-{}", stableHash(key), collision);
				}

				SceneDef def;
				def.id = std::move(id);
				const std::filesystem::path displayPath{ entry.display };
				def.name = displayPath.filename().string();
				if (def.name.empty()) {
					def.name = entry.display;
				}
				if (!entry.clip.animId.empty()) {
					def.name += " · " + entry.clip.animId;
				}
				def.species = Util::SpeciesFromAnimPath(entry.clip.file);
				if (def.species.empty()) {
					def.species = "human";
				}
				def.unlisted = true;  // direct/browser only; never enter tag matchmaking
				def.library = true;   // Animations lane, beside (but not duplicated from) vanilla
				def.lockPlayer = false;
				def.stripActors = false;
				def.fade = false;
				def.inPlace = true;   // raw-clip debugging must not teleport or root-pin the actor
				def.tags = { "scene.clip" };
				def.tagSet = { "scene.clip" };
				def.roles.emplace_back();
				def.sourceFile = std::move(entry.sourceFile);
				def.pack = std::move(entry.pack);

				StageDef stage;
				stage.name = entry.display;
				if (!entry.clip.animId.empty()) {
					stage.name += ":" + entry.clip.animId;
				}
				stage.tags = { "scene.clip" };
				entry.clip.offset.reset();  // isolate the animation, not its role placement
				stage.clips.push_back(std::move(entry.clip));
				DesugarLinear(def, { stage });  // one holding node; Advance/Space ends it

				const std::string generatedKey = ToLower(def.id);
				a_scenes.emplace(generatedKey, std::move(def));
				++added;
			}
			return added;
		}
	}

	void SceneRegistry::LoadAll()
	{
		namespace fs = std::filesystem;
		std::unordered_map<std::string, SceneDef> loaded;
		std::vector<std::string> errors;

		const fs::path dir = fs::current_path() / "Data" / "OSF";
		std::error_code ec;
		if (fs::is_directory(dir, ec)) {
			std::vector<fs::path> files;
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file(ec) && ToLower(entry.path().filename().string()).ends_with(".osf.json")) {
					files.push_back(entry.path());
				}
			}
			// Sorted by filename so "first-loaded wins" on an id collision is deterministic.
			std::sort(files.begin(), files.end(), [](const fs::path& a_lhs, const fs::path& a_rhs) {
				return ToLower(a_lhs.filename().string()) < ToLower(a_rhs.filename().string());
			});
			for (const auto& file : files) {
				try {
					std::ifstream in(file, std::ios::binary);
					const auto j = nlohmann::json::parse(in, nullptr, true, true);  // tolerate // comments
					LoadOsfFile(j, file, loaded, errors);
				} catch (const std::exception& e) {
					errors.push_back("[error] '" + file.filename().string() + "': parse failed: " + e.what());
					REX::ERROR("[Registry] failed to parse '{}': {}", file.filename().string(), e.what());
				}
			}

			// Resolve every node `use` now that the whole set is loaded (catches dangling refs at load).
			ValidateUseRefs(loaded, errors);

			// Hide scenes whose clips aren't installed (compat pack without its source mod).
			SweepClipAvailability(loaded, errors);
		}

		const auto sceneCount = loaded.size();
		const auto clipEntryCount = AddSceneClipEntries(loaded);
		const auto problemCount = errors.size();
		auto next = std::make_shared<SceneRegistrySnapshot>();
		next->scenes = std::move(loaded);
		next->loadErrors = std::move(errors);
		next->authoredSceneCount = sceneCount;
		snapshot.store(std::move(next), std::memory_order_release);
		REX::INFO("[Registry] {} scene(s) loaded, {} scene clip entr{}, {} problem(s)",
			sceneCount, clipEntryCount, clipEntryCount == 1 ? "y" : "ies", problemCount);
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
