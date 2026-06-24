#include "Registry/SceneRegistry.h"

#include "Util/FormRef.h"
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
				// `sound` is the literal spec; `pool` is accepted as an alias (name->clip
				// resolution isn't wired up yet, so for now either is treated literally).
				se.spec = s.value("sound", s.value("pool", std::string{}));
				if (se.spec.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': a sound track entry is missing 'sound'/'pool'");
				}
				se.role = s.value("role", std::string{});
				se.volume = s.value("volume", 1.0f);
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
				ce.state = c.value("state", std::string{});
				if (ce.state.empty()) {
					throw std::runtime_error("node '" + a_node_out.id + "': a camera track entry is missing 'state'");
				}
				// Only one camera state is supported for now; free-fly/orbit/matrix aren't yet.
				if (ToLower(ce.state) != "thirdperson_hold") {
					throw std::runtime_error("node '" + a_node_out.id + "': unknown camera state '" + ce.state +
						"' (only 'thirdperson_hold' is supported)");
				}
				ParseTrackTiming(c, ce, a_node_out.id, "camera '" + ce.state + "'", /*a_atRequired*/ false);
				a_node_out.cameras.push_back(std::move(ce));
			}
		}

		// Parse the node's `tracks` block. The lanes are `cue`, `action`, `sound`, and `camera`,
		// all parsed and run; any other lane name is rejected.
		void ParseCueTracks(const json& a_node, SceneNode& a_node_out)
		{
			const auto it = a_node.find("tracks");
			if (it == a_node.end()) {
				return;
			}
			if (!it->is_object()) {
				throw std::runtime_error("node '" + a_node_out.id + "': 'tracks' must be an object");
			}
			for (const auto& [lane, entries] : it->items()) {
				const auto laneLower = ToLower(lane);
				if (laneLower == "action") {
					ParseActionTrack(entries, a_node_out);
					continue;
				}
				if (laneLower == "sound") {
					ParseSoundTrack(entries, a_node_out);
					continue;
				}
				if (laneLower == "camera") {
					ParseCameraTrack(entries, a_node_out);
					continue;
				}
				if (laneLower != "cue") {
					throw std::runtime_error("node '" + a_node_out.id + "': unknown track lane '" + lane + "'");
				}
				if (!entries.is_array()) {
					throw std::runtime_error("node '" + a_node_out.id + "': 'cue' track must be an array");
				}
				for (const auto& c : entries) {
					CueEntry ce;
					ce.id = c.value("id", std::string{});
					if (ce.id.empty()) {
						throw std::runtime_error("node '" + a_node_out.id + "': a cue track entry is missing 'id'");
					}
					// Cues require an explicit `at` (no enter default); the rest of the time model is shared.
					ParseTrackTiming(c, ce, a_node_out.id, "cue '" + ce.id + "'", /*a_atRequired*/ true);
					a_node_out.cues.push_back(std::move(ce));
				}
			}
		}

		SceneNode ParseNode(const json& a_node, std::vector<std::string>& a_warnings, const std::string& a_sceneId)
		{
			SceneNode n;
			n.id = a_node.value("id", std::string{});
			if (n.id.empty()) {
				throw std::runtime_error("a node is missing 'id'");
			}
			n.anim = a_node.value("anim", std::string{});
			if (n.anim.empty()) {
				throw std::runtime_error("node '" + n.id + "': missing 'anim'");
			}
			n.loopMode = ParseLoop(a_node, n.loopCount);
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

			ParseCueTracks(a_node, n);

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

		// Parse one role: name, gender (the `gender` shorthand and `filters.gender` are the same
		// constraint — reject if both present and differ), and the resolved keyword/race filters.
		SceneRole ParseRole(const json& a_role, const std::string& a_sceneId)
		{
			SceneRole r;
			r.name = a_role.value("name", std::string{});
			if (r.name.empty()) {
				throw std::runtime_error("scene '" + a_sceneId + "': a role is missing 'name'");
			}

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
			return r;
		}

		// Throws std::runtime_error on any reject; pushes non-fatal notes to a_warnings.
		SceneDef ParseScene(const json& a_json, std::vector<std::string>& a_warnings)
		{
			SceneDef def;
			def.id = a_json.value("id", std::string{});
			if (def.id.empty()) {
				throw std::runtime_error("scene missing 'id'");
			}
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
			if (auto it = a_json.find("lockPlayer"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'lockPlayer' must be a boolean");
				}
				def.lockPlayer = it->get<bool>();
			}
			if (auto it = a_json.find("stripActors"); it != a_json.end()) {
				if (!it->is_boolean()) {
					throw std::runtime_error("scene '" + def.id + "': 'stripActors' must be a boolean");
				}
				def.stripActors = it->get<bool>();
			}
			def.entry = a_json.value("entry", std::string{});
			if (def.entry.empty()) {
				throw std::runtime_error("scene '" + def.id + "': missing 'entry'");
			}
			if (const auto it = a_json.find("tags"); it != a_json.end()) {
				for (const auto& t : *it) {
					def.tags.push_back(t.get<std::string>());
				}
			}
			if (const auto it = a_json.find("roles"); it != a_json.end()) {
				for (const auto& jRole : *it) {
					def.roles.push_back(ParseRole(jRole, def.id));
				}
			}

			const auto jNodes = a_json.find("nodes");
			if (jNodes == a_json.end() || !jNodes->is_array() || jNodes->empty()) {
				throw std::runtime_error("scene '" + def.id + "': 'nodes' must be a non-empty array");
			}
			std::unordered_set<std::string> nodeIds;
			for (const auto& jNode : *jNodes) {
				auto n = ParseNode(jNode, a_warnings, def.id);
				if (!nodeIds.insert(ToLower(n.id)).second) {
					throw std::runtime_error("scene '" + def.id + "': duplicate node id '" + n.id + "'");
				}
				def.nodes.push_back(std::move(n));
			}

			// Optional linear-stage map (stage i -> node id). Each must name a real node.
			if (const auto it = a_json.find("linearStages"); it != a_json.end()) {
				for (const auto& s : *it) {
					auto nid = s.get<std::string>();
					if (!nodeIds.count(ToLower(nid))) {
						throw std::runtime_error("scene '" + def.id + "': linearStages references missing node '" + nid + "'");
					}
					def.linearStages.push_back(std::move(nid));
				}
			}

			// Reference validation (needs the full node-id set).
			if (!nodeIds.count(ToLower(def.entry))) {
				throw std::runtime_error("scene '" + def.id + "': entry '" + def.entry + "' is not a node");
			}
			// A non-empty action/sound role must name a role the scene declares.
			std::unordered_set<std::string> roleNames;
			for (const auto& r : def.roles) {
				roleNames.insert(ToLower(r.name));
			}
			for (const auto& n : def.nodes) {
				for (const auto& a : n.actions) {
					if (!a.role.empty() && !roleNames.count(ToLower(a.role))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + n.id + "' action '" + a.type +
							"' references undeclared role '" + a.role + "'");
					}
				}
				for (const auto& s : n.sounds) {
					if (!s.role.empty() && !roleNames.count(ToLower(s.role))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + n.id + "' sound '" + s.spec +
							"' references undeclared role '" + s.role + "'");
					}
				}
			}
			for (const auto& n : def.nodes) {
				for (const auto& e : n.edges) {
					if (e.to != "$end" && !nodeIds.count(ToLower(e.to))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + n.id + "' edge targets missing node '" + e.to + "'");
					}
				}
				if (n.loopMode == LoopMode::kHold && !n.loopForever) {
					bool hasExit = false;
					for (const auto& e : n.edges) {
						if (e.when == EdgeWhen::kAdvance || e.when == EdgeWhen::kTimer || e.when == EdgeWhen::kTrigger) {
							hasExit = true;
							break;
						}
					}
					if (!hasExit) {
						a_warnings.push_back("scene '" + def.id + "' node '" + n.id +
							"': hold node with no advance/timer/trigger edge never ends (set loopForever:true if intended)");
					}
				}
			}
			return def;
		}

		void LoadSceneFile(const json& a_json, const std::filesystem::path& a_file,
			std::unordered_map<std::string, SceneDef>& a_out, std::vector<std::string>& a_errors)
		{
			const std::string fileName = a_file.filename().string();

			const auto it = a_json.find("schema");
			if (it == a_json.end()) {
				a_errors.push_back("[error] '" + fileName + "': missing required 'schema'");
				REX::ERROR("SceneRegistry: '{}' missing required 'schema' — skipped", fileName);
				return;
			}
			if (!it->is_number_integer()) {
				a_errors.push_back("[error] '" + fileName + "': non-integer 'schema'");
				REX::ERROR("SceneRegistry: '{}' has a non-integer 'schema' — skipped", fileName);
				return;
			}
			const auto schema = it->get<std::int64_t>();
			if (schema < 1 || schema > kSceneSchemaVersion) {
				a_errors.push_back("[error] '" + fileName + "': scene schema " + std::to_string(schema) +
					" unsupported (max " + std::to_string(kSceneSchemaVersion) + ")");
				REX::ERROR("SceneRegistry: '{}' declares scene schema {} but this build understands up to {} — skipped",
					fileName, schema, kSceneSchemaVersion);
				return;
			}

			std::vector<std::string> warnings;
			try {
				auto def = ParseScene(a_json, warnings);
				def.sourceFile = a_file;
				auto key = ToLower(def.id);
				if (const auto f = a_out.find(key); f != a_out.end()) {
					a_errors.push_back("[error] duplicate scene id '" + def.id + "' in '" + fileName +
						"' (already from '" + f->second.sourceFile.filename().string() + "') — keeping the first");
					REX::ERROR("SceneRegistry: duplicate scene id '{}' in '{}' — keeping first from '{}'",
						def.id, fileName, f->second.sourceFile.filename().string());
					return;
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
				if (entry.is_regular_file(ec) && ToLower(entry.path().filename().string()).ends_with(".scene.json")) {
					files.push_back(entry.path());
				}
			}
			std::sort(files.begin(), files.end(), [](const fs::path& a_lhs, const fs::path& a_rhs) {
				return ToLower(a_lhs.filename().string()) < ToLower(a_rhs.filename().string());
			});
			for (const auto& file : files) {
				try {
					std::ifstream in(file, std::ios::binary);
					const auto j = nlohmann::json::parse(in, nullptr, true, true);  // tolerate // comments
					LoadSceneFile(j, file, loaded, errors);
				} catch (const std::exception& e) {
					errors.push_back("[error] '" + file.filename().string() + "': parse failed: " + e.what());
					REX::ERROR("SceneRegistry: failed to parse '{}': {}", file.filename().string(), e.what());
				}
			}
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
