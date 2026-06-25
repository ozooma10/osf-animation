#include "Registry/SceneRegistry.h"

#include "Input/InputTypes.h"
#include "Util/FormRef.h"
#include "Util/Math.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <charconv>
#include <fstream>
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

		LoopMode ParseLoop(const json& a_node, std::int32_t& a_count)
		{
			a_count = 0;
			const auto it = a_node.find("loop");
			if (it == a_node.end() || !it->is_object()) {
				throw std::runtime_error("missing object 'loop' { mode, ... }");
			}
			const auto mode = ToLower(it->value("mode", std::string{}));
			if (mode == "once") {
				return LoopMode::kOnce;
			}
			if (mode == "hold") {
				return LoopMode::kHold;
			}
			if (mode == "count") {
				a_count = it->value("count", 0);
				if (a_count <= 0) {
					throw std::runtime_error("loop mode 'count' requires integer count > 0");
				}
				return LoopMode::kCount;
			}
			throw std::runtime_error("unknown loop.mode '" + mode + "'");
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
				const auto typeLower = ToLower(ae.type);
				if (typeLower.rfind("osf.", 0) == 0) {
					if (!IsKnownBuiltinAction(typeLower)) {
						throw std::runtime_error("node '" + a_node_out.id + "': unknown built-in action '" + ae.type + "'");
					}
					// Per-action required fields. Role-targeted mechanisms need a role; voice
					// also needs its sound set. Fade takes no required field.
					const bool needsRole = typeLower == "osf.control.lock" || typeLower == "osf.control.release" ||
						typeLower == "osf.equipment.hide" || typeLower == "osf.equipment.restore" ||
						typeLower == "osf.weapon.sheathe" || typeLower == "osf.weapon.restore" ||
						typeLower == "osf.voice.play";
					if (needsRole && ae.role.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': action '" + ae.type + "' requires 'role'");
					}
					if (typeLower == "osf.voice.play" && ae.set.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': action 'osf.voice.play' requires 'set'");
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

		void ParseSoundTrack(const json& a_entries, SceneNode& a_node_out)
		{
			if (!a_entries.is_array()) {
				throw std::runtime_error("node '" + a_node_out.id + "': 'sound' track must be an array");
			}
			for (const auto& s : a_entries) {
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
					r.keywords.push_back(ResolveFormRef<RE::BGSKeyword>(a_ref, a_sceneId, r.name, a_field, "Keyword (KYWD)"));
				});
				parseRefs("race", "filters.race", [&](const std::string& a_ref, const char* a_field) {
					r.races.push_back(ResolveFormRef<RE::TESRace>(a_ref, a_sceneId, r.name, a_field, "Race (RACE)"));
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
			return r;
		}

		// Top-level metadata (name/priority/weight/lockPlayer/stripActors/playerControl). id, tags,
		// roles, and the playable (clip/stages/nodes) are parsed by the caller. a_lockDefault/
		// a_stripDefault seed the policy opt-outs (the file-level defaults).
		void ParseSceneMeta(const json& a_json, SceneDef& def, bool a_lockDefault, bool a_stripDefault)
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
								REX::WARN("scene '{}': unknown playerControl capability '{}' — ignored (typo, or a newer OSF Animation?)", def.id, name);
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
		// Scene parser — the one "scene" concept (RFC-unified-animation-schema.md).
		// ============================================================================

		// Reject authored ids that collide with the synthetic desugar namespace.
		void RejectReservedId(const std::string& a_id, const char* a_what)
		{
			if (a_id.find('#') != std::string::npos) {
				throw std::runtime_error(std::string(a_what) + " id '" + a_id +
					"' may not contain '#' (reserved for synthetic stage nodes)");
			}
		}

		// Parse a stage list (timer/loops/clips, with the play-once default and the bare-array
		// shorthand) — the unified equivalent of the pack stages[] parse. a_ioActorCount is the
		// participant count: when a_fixed it is authoritative (every stage must match it); else the
		// first stage's clip count sets it.
		std::vector<StageDef> ParseOsfStageList(const json& a_stages, const std::string& a_subject,
			size_t& a_ioActorCount, bool a_fixed)
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
					// Optional per-stage `sound` lane, so a linear stage can carry audio without authoring the full nodes[] graph form.
					if (auto soundIt = jStage.find("sound"); soundIt != jStage.end()) {
						SceneNode scratch;
						scratch.id = a_subject;  // diagnostics only
						ParseSoundTrack(*soundIt, scratch);
						info.sounds = std::move(scratch.sounds);
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
					StageClip clip;
					if (jClip.is_string()) {
						clip.file = jClip.get<std::string>();
					} else if (jClip.is_object()) {
						clip.file = jClip.at("file").get<std::string>();
						if (auto oit = jClip.find("offset"); oit != jClip.end()) {
							clip.offset = ParseOffsetField(*oit);
						}
					} else {
						throw std::runtime_error(a_subject + ": a clip must be a file string or a { file, offset } object");
					}
					if (clip.file.empty()) {
						throw std::runtime_error(a_subject + ": empty clip file");
					}
					info.clips.push_back(std::move(clip));
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

		// Cue lane (the one track the legacy ParseCueTracks parses inline). Cues need an explicit `at`.
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
		SceneNode ParseOsfNode(const json& a_node, std::vector<std::string>& a_warnings, const std::string& a_sceneId)
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
				n.stages = ParseOsfStageList(a_node.at("stages"), "node '" + n.id + "'", ac, /*a_fixed*/ false);
			}

			// loop is optional; absent -> hold (the SceneNode default).
			if (a_node.contains("loop")) {
				n.loopMode = ParseLoop(a_node, n.loopCount);
			}
			n.timerSec = a_node.value("timerSec", 0.0f);
			n.loopForever = a_node.value("loopForever", false);

			std::unordered_set<std::string> edgeIds;
			int defaults = 0;
			bool hasTimerEdge = false;
			if (const auto it = a_node.find("edges"); it != a_node.end()) {
				for (const auto& jEdge : *it) {
					auto e = ParseEdge(jEdge, n.id);
					if (e.when == EdgeWhen::kEnd && n.loopMode == LoopMode::kHold) {
						throw std::runtime_error("node '" + n.id + "': an 'end' edge on a hold node can never fire");
					}
					if (e.when == EdgeWhen::kLoops && n.loopMode != LoopMode::kCount) {
						throw std::runtime_error("node '" + n.id + "': a 'loops' edge needs loop mode 'count'");
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
				throw std::runtime_error("node '" + n.id + "': has a 'timer' edge but timerSec <= 0");
			}
			if (!hasTimerEdge && n.timerSec > 0.0f) {
				a_warnings.push_back("scene '" + a_sceneId + "' node '" + n.id + "': timerSec set but no 'timer' edge");
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
				node.sounds = st.sounds;  // forward the stage's `sound` lane onto the node, where dispatch reads it
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
					node.loopMode = LoopMode::kHold;
					node.loopForever = true;  // explicit hold (timer:0, loops:0): hold here until the player advances
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
		// references. Mirrors the legacy ParseScene checks (anonymous roles are unreferenceable).
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

		// Parse one unified scene. a_lockDefault/a_stripDefault are the file-level policy defaults.
		SceneDef ParseOsfScene(const json& a_json, std::vector<std::string>& a_warnings, bool a_lockDefault, bool a_stripDefault,
			std::string_view a_cameraDefault)
		{
			SceneDef def;
			def.id = a_json.value("id", std::string{});
			if (def.id.empty()) {
				throw std::runtime_error("scene missing 'id'");
			}
			RejectReservedId(def.id, "scene");
			ParseSceneMeta(a_json, def, a_lockDefault, a_stripDefault);
			if (const auto it = a_json.find("tags"); it != a_json.end()) {
				for (const auto& t : *it) {
					def.tags.push_back(t.get<std::string>());
				}
			}
			// roles[]: unified participant list; `name` optional (anonymous positional slot).
			bool rolesGiven = false;
			if (const auto it = a_json.find("roles"); it != a_json.end()) {
				if (!it->is_array()) {
					throw std::runtime_error("scene '" + def.id + "': 'roles' must be an array");
				}
				rolesGiven = true;
				for (const auto& jRole : *it) {
					def.roles.push_back(ParseRole(jRole, def.id));
				}
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
					auto nd = ParseOsfNode(jNode, a_warnings, def.id);
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
				// A hold node with no advance/timer/trigger edge never ends (unless loopForever is set).
				for (const auto& nd : def.nodes) {
					if (nd.loopMode != LoopMode::kHold || nd.loopForever) {
						continue;
					}
					bool hasExit = false;
					for (const auto& e : nd.edges) {
						if (e.when == EdgeWhen::kAdvance || e.when == EdgeWhen::kTimer || e.when == EdgeWhen::kTrigger) {
							hasExit = true;
							break;
						}
					}
					if (!hasExit) {
						a_warnings.push_back("scene '" + def.id + "' node '" + nd.id +
							"': hold node with no advance/timer/trigger edge never ends (set loopForever:true if intended)");
					}
				}
				ValidateGraph(def, nodeIds);
			} else {
				// Linear scene: top-level clip/stages -> a synthetic node chain (desugar).
				size_t actorCount = rolesGiven ? def.roles.size() : 0;
				std::vector<StageDef> stages;
				if (hasClip) {
					const auto& clip = a_json.at("clip");
					if (!clip.is_string() || clip.get<std::string>().empty()) {
						throw std::runtime_error("scene '" + def.id + "': 'clip' must be a non-empty file string");
					}
					if (actorCount != 0 && actorCount != 1) {
						throw std::runtime_error("scene '" + def.id + "': 'clip' is single-actor but " +
							std::to_string(actorCount) + " role(s) declared (use 'stages' for multi-actor)");
					}
					StageDef st;
					st.loops = 1;  // play once, then end
					StageClip c;
					c.file = clip.get<std::string>();
					st.clips.push_back(std::move(c));
					actorCount = 1;
					stages.push_back(std::move(st));
				} else {
					stages = ParseOsfStageList(a_json.at("stages"), "scene '" + def.id + "'", actorCount, rolesGiven);
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
			return def;
		}

		void LoadOsfFile(const json& a_json, const std::filesystem::path& a_file,
			std::unordered_map<std::string, SceneDef>& a_out, std::vector<std::string>& a_errors)
		{
			const std::string fileName = a_file.filename().string();

			const auto it = a_json.find("schema");
			if (it == a_json.end() || !it->is_number_integer()) {
				a_errors.push_back("[error] '" + fileName + "': missing/non-integer 'schema'");
				REX::ERROR("SceneRegistry: '{}' missing/non-integer 'schema' — skipped", fileName);
				return;
			}
			const auto schema = it->get<std::int64_t>();
			if (schema != kSchemaVersion) {
				a_errors.push_back("[error] '" + fileName + "': *.osf.json schema " + std::to_string(schema) +
					" unsupported (expected " + std::to_string(kSchemaVersion) + ")");
				REX::ERROR("SceneRegistry: '{}' declares osf schema {} but this build expects {} — skipped",
					fileName, schema, kSchemaVersion);
				return;
			}

			const bool lockDefault = a_json.value("lockPlayer", true);
			const bool stripDefault = a_json.value("stripActors", true);

			// Pack-level default camera: "camera": "<state>" attaches that posture to each scene's
			// entry node (unless that node already declares its own camera track). When omitted, the
			// pack defaults to "scene_orbit".
			std::string cameraDefault = "scene_orbit";
			if (const auto cit = a_json.find("camera"); cit != a_json.end()) {
				if (!cit->is_string()) {
					a_errors.push_back("[error] '" + fileName + "': 'camera' must be a string");
					REX::ERROR("SceneRegistry: '{}' 'camera' must be a string — skipped", fileName);
					return;
				}
				cameraDefault = ToLower(cit->get<std::string>());
				// "none" opts a pack out of the default camera override entirely.
				if (cameraDefault == "none") {
					cameraDefault.clear();
				} else if (!IsKnownCameraState(cameraDefault)) {
					a_errors.push_back("[error] '" + fileName + "': unknown camera state '" + cit->get<std::string>() +
						"' (supported: 'thirdperson_hold', 'freefly', 'vanity_orbit', 'scene_orbit', 'none')");
					REX::ERROR("SceneRegistry: '{}' unknown camera state '{}' — skipped", fileName, cit->get<std::string>());
					return;
				}
			}

			// A file holds a single bare scene, or { schema, scenes: [...] }.
			std::vector<const json*> sceneJsons;
			if (auto sit = a_json.find("scenes"); sit != a_json.end()) {
				if (!sit->is_array()) {
					a_errors.push_back("[error] '" + fileName + "': 'scenes' must be an array");
					REX::ERROR("SceneRegistry: '{}' 'scenes' must be an array — skipped", fileName);
					return;
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
					auto def = ParseOsfScene(*sj, warnings, lockDefault, stripDefault, cameraDefault);
					def.sourceFile = a_file;
					auto key = ToLower(def.id);
					if (const auto f = a_out.find(key); f != a_out.end()) {
						a_errors.push_back("[error] duplicate scene id '" + def.id + "' in '" + fileName +
							"' (already from '" + f->second.sourceFile.filename().string() + "') — keeping the first");
						REX::ERROR("SceneRegistry: duplicate scene id '{}' in '{}' — keeping first from '{}'",
							def.id, fileName, f->second.sourceFile.filename().string());
						continue;
					}
					for (const auto& w : warnings) {
						a_errors.push_back("[warn] " + w);
						REX::WARN("SceneRegistry: {}", w);
					}
					REX::INFO("SceneRegistry: loaded scene '{}' ({} node(s)) from '{}'", def.id, def.nodes.size(), fileName);
					a_out[std::move(key)] = std::move(def);
				} catch (const std::exception& e) {
					a_errors.push_back("[error] '" + fileName + "': " + e.what());
					REX::ERROR("SceneRegistry: skipping scene in '{}': {}", fileName, e.what());
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
						REX::ERROR("SceneRegistry: scene '{}' node '{}' use references unknown scene '{}'", def.id, nd.id, nd.use);
						continue;
					}
					const auto& target = tit->second;
					if (target.nodes.size() != 1 || target.nodes[0].stages.empty()) {
						a_errors.push_back("[error] scene '" + def.id + "' node '" + nd.id + "': use target '" + nd.use +
							"' is not a single inline-stage scene (use splices one node's stages)");
						REX::ERROR("SceneRegistry: scene '{}' node '{}' use target '{}' is not single inline-stage", def.id, nd.id, nd.use);
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
				REX::WARN("SceneRegistry: '{}' needs {} role(s), got {}", a_id, a_roles.size(), a_actorCount);
				return std::nullopt;
			}
			Animation::ScenePlan plan;
			plan.animId = a_id;
			plan.stages.reserve(a_stages.size());
			for (const auto& sd : a_stages) {
				if (sd.clips.size() != a_actorCount) {
					REX::WARN("SceneRegistry: '{}' stage has {} clip(s) but {} role(s)", a_id, sd.clips.size(), a_actorCount);
					return std::nullopt;
				}
				Animation::ScenePlan::Stage stage;
				stage.timer = sd.timer;
				stage.loops = sd.loops;
				for (size_t a = 0; a < a_actorCount; a++) {
					const auto& clip = sd.clips[a];
					stage.files.push_back(clip.file);
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
					REX::ERROR("SceneRegistry: failed to parse '{}': {}", file.filename().string(), e.what());
				}
			}

			// Resolve every node `use` now that the whole set is loaded (catches dangling refs at load).
			ValidateUseRefs(loaded, errors);
		}

		const auto sceneCount = loaded.size();
		const auto problemCount = errors.size();
		{
			std::unique_lock l{ lock };
			scenes = std::move(loaded);
			loadErrors = std::move(errors);
		}
		REX::INFO("SceneRegistry: {} scene(s) loaded, {} problem(s)", sceneCount, problemCount);
	}

	const SceneDef* SceneRegistry::Find(std::string_view a_id) const
	{
		std::shared_lock l{ lock };
		const auto it = scenes.find(ToLower(std::string(a_id)));
		return it != scenes.end() ? &it->second : nullptr;
	}

	std::optional<Animation::ScenePlan> SceneRegistry::BuildNodePlan(const SceneDef& a_def, const SceneNode& a_node, size_t a_actorCount) const
	{
		if (!a_node.use.empty()) {
			// A `use` node splices the target scene's single inline-stage node (validated at load,
			// re-checked here). The target's own roles supply the default placements.
			std::shared_lock l{ lock };
			const auto it = scenes.find(ToLower(a_node.use));
			if (it == scenes.end()) {
				REX::WARN("SceneRegistry: node '{}' use '{}' references unknown scene", a_node.id, a_node.use);
				return std::nullopt;
			}
			const auto& target = it->second;
			if (target.nodes.size() != 1 || !target.nodes[0].use.empty() || target.nodes[0].stages.empty()) {
				REX::WARN("SceneRegistry: node '{}' use target '{}' is not a single inline-stage scene", a_node.id, a_node.use);
				return std::nullopt;
			}
			return BuildPlanFromStages(target.id, target.roles, target.nodes[0].stages, a_actorCount);
		}
		// Inline node: this scene's roles supply the default placements.
		return BuildPlanFromStages(a_def.id, a_def.roles, a_node.stages, a_actorCount);
	}

	void SceneRegistry::ForEachDef(const std::function<void(const SceneDef&)>& a_fn) const
	{
		std::shared_lock l{ lock };
		for (const auto& [key, def] : scenes) {
			a_fn(def);
		}
	}

	size_t SceneRegistry::Size() const
	{
		std::shared_lock l{ lock };
		return scenes.size();
	}

	std::vector<std::string> SceneRegistry::LoadErrors() const
	{
		std::shared_lock l{ lock };
		return loadErrors;
	}
}
