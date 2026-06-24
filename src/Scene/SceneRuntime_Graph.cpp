#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Equipment/EquipmentService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <charconv>
#include <optional>

// SceneRuntime — graph / playback slice (one class, split across translation units; see SceneRuntime.cpp). 
// Owns the node graph: starting scenes, node transitions (the single ApplyTransition path), the auto-edge / cue-trigger navigation, and decoding the timed marks a stage crosses. 
// The file-local helpers below are used only by this slice.

namespace OSF::Scene
{
	namespace
	{
		// Timed-mark lane ids. The playback layer treats these as opaque; this runtime assigns
		// the meaning and the same-tick ordering — sorting fired marks by lane ascending yields
		// the fixed order action -> camera -> sound -> cue.
		constexpr std::uint8_t kLaneAction = 0;
		constexpr std::uint8_t kLaneCamera = 1;
		constexpr std::uint8_t kLaneSound = 2;
		constexpr std::uint8_t kLaneCue = 3;

		// Schedule a node's timed-track entries (numeric `at` -> a fraction mark; `end` -> an end mark) onto a_marks as opaque lane+token marks.
		// a_token maps an entry + its list index to the mark token.
		// Lifecycle enter/exit entries carry other pos values and are skipped here (they're fired directly by the lifecycle dispatch, not via the timeline).
		template <class Entry, class Pos, class TokenFn>
		void ScheduleMarks(std::vector<Animation::TimedMark>& a_marks, const std::vector<Entry>& a_entries,
			Pos a_fractionPos, Pos a_endPos, std::uint8_t a_lane, TokenFn a_token)
		{
			for (std::size_t i = 0; i < a_entries.size(); i++) {
				const auto& e = a_entries[i];
				if (e.pos == a_fractionPos) {
					a_marks.push_back({ e.fraction, e.everyLoop, false, a_lane, a_token(e, i) });
				} else if (e.pos == a_endPos) {
					a_marks.push_back({ 0.0f, false, true, a_lane, a_token(e, i) });
				}
			}
		}

		// Parse a decimal index token (the one ApplyNodeMarks stamped) into a_out, bounded by a_count.
		// false if the token isn't a number or is >= a_count.
		bool ParseIndexToken(std::string_view a_token, std::size_t a_count, std::size_t& a_out)
		{
			std::size_t idx = 0;
			const auto* first = a_token.data();
			const auto [ptr, ec] = std::from_chars(first, first + a_token.size(), idx);
			if (ec != std::errc{} || idx >= a_count) {
				return false;
			}
			a_out = idx;
			return true;
		}

		// Map a node's loop policy + timerSec onto the played plan's TERMINAL stage so
		// GraphManager auto-ends it (and reports timer vs loops). For a single-stage anim
		// (the common node) that is "the stage"; for a multi-stage pack used as a node, the
		// intermediate stages keep their pack-authored timers and the node policy bounds the
		// last one. once -> loops 1 (play once -> 'end'); count N -> loops N (-> 'loops');
		// hold -> loops 0 (never auto-ends). timerSec arms the stage timer (-> 'timer') only
		// when the node actually carries a `timer` edge (a bare timerSec is just a warning).
		void ApplyNodePolicy(Animation::ScenePlan& a_plan, const Registry::SceneNode& a_node)
		{
			if (a_plan.stages.empty()) {
				return;
			}
			auto& stage = a_plan.stages.back();
			switch (a_node.loopMode) {
			case Registry::LoopMode::kOnce:
				stage.loops = 1;
				break;
			case Registry::LoopMode::kCount:
				stage.loops = a_node.loopCount;
				break;
			case Registry::LoopMode::kHold:
				stage.loops = 0;
				break;
			}
			bool hasTimerEdge = false;
			for (const auto& e : a_node.edges) {
				if (e.when == Registry::EdgeWhen::kTimer) {
					hasTimerEdge = true;
					break;
				}
			}
			stage.timer = (hasTimerEdge && a_node.timerSec > 0.0f) ? a_node.timerSec : 0.0f;
		}

		// Map a node's timed track entries (numeric `at` + `end`) onto the played plan's terminal
		// stage as opaque lane+token marks, so the Scene fires them by clip time and this runtime
		// decodes them in OnTimedMarks. Lifecycle enter/exit entries are fired by the lifecycle
		// dispatch (DispatchLifecycleCues / DispatchLifecycleActions), not here.
		//   - cue lane: token = the cue id (matches trigger:<id> edges + the EVENT_CUE payload).
		//   - action lane: token = the action's index in node.actions (stable; OnTimedMarks
		//     resolves the node and recovers the full ActionEntry by index).
		void ApplyNodeMarks(Animation::ScenePlan& a_plan, const Registry::SceneNode& a_node)
		{
			if (a_plan.stages.empty()) {
				return;
			}
			auto& marks = a_plan.stages.back().marks;
			// cue lane token = the cue id; action/sound/camera lanes token = the entry's list index (OnTimedMarks recovers the entry by index).
			// Per-lane declaration order is preserved.
			ScheduleMarks(marks, a_node.cues, Registry::CuePos::kFraction, Registry::CuePos::kEnd, kLaneCue, [](const auto& a_cue, std::size_t) { return a_cue.id; });
			ScheduleMarks(marks, a_node.actions, Registry::ActionPos::kFraction, Registry::ActionPos::kEnd, kLaneAction, [](const auto&, std::size_t a_i) { return std::to_string(a_i); });
			ScheduleMarks(marks, a_node.sounds, Registry::SoundPos::kFraction, Registry::SoundPos::kEnd, kLaneSound, [](const auto&, std::size_t a_i) { return std::to_string(a_i); });
			ScheduleMarks(marks, a_node.cameras, Registry::CameraPos::kFraction, Registry::CameraPos::kEnd, kLaneCamera, [](const auto&, std::size_t a_i) { return std::to_string(a_i); });
		}

		// The outgoing auto-edge of a_node whose `when` matches: highest priority wins, ties keep declaration order. nullptr if the node has no such edge.
		const Registry::SceneEdge* SelectAutoEdge(const Registry::SceneNode& a_node, Registry::EdgeWhen a_when)
		{
			const Registry::SceneEdge* best = nullptr;
			for (const auto& e : a_node.edges) {
				if (e.when == a_when && (!best || e.priority > best->priority)) {
					best = &e;
				}
			}
			return best;
		}
	}

	void SceneRuntime::OnTimedMarks(const std::vector<RE::Actor*>& a_participants, const std::vector<Animation::FiredMark>& a_marks)
	{
		if (a_marks.empty()) {
			return;
		}

		// The same-tick order across lanes is fixed (action -> camera -> sound -> cue), not JSON
		// order. The lane ids are numbered in that order, so a stable sort by lane yields it while
		// preserving the Scene's per-lane declaration order.
		std::vector<Animation::FiredMark> marks = a_marks;
		std::stable_sort(marks.begin(), marks.end(),
			[](const Animation::FiredMark& a_lhs, const Animation::FiredMark& a_rhs) { return a_lhs.lane < a_rhs.lane; });

		std::int32_t handle = 0;
		std::string oldNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		{
			std::lock_guard l{ _lock };
			Slot* s = nullptr;
			for (auto* a : a_participants) {
				if ((s = FindSlotForActor(a, &handle))) {
					break;
				}
			}
			if (!s) {
				return;  // not a SceneRuntime scene (e.g. PlaySequence)
			}
			oldNode = s->node;
			sceneId = s->id;
			participants = s->participants;
		}

		const auto* def = Registry::SceneRegistry::GetSingleton().Find(sceneId);
		const auto* node = def ? def->FindNode(oldNode) : nullptr;

		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(participants.begin(), participants.end(), static_cast<RE::Actor*>(player)) != participants.end();

		// Decode each mark by lane (already lane-ordered). Action-lane marks run their mechanism
		// now; cue-lane ids are collected for EVENT_CUE + the trigger pass after the tick's
		// entries have run. The camera and sound lanes run their marks here too.
		std::vector<std::string> cueIds;
		for (const auto& m : marks) {
			if (m.lane == kLaneCue) {
				cueIds.push_back(m.token);
			} else if (m.lane == kLaneAction && node) {
				// token = index into node->actions (set by ApplyNodeMarks).
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->actions.size(), idx)) {
					RunAction(handle, oldNode, node->actions[idx], "", hasPlayer);
				}
			} else if (m.lane == kLaneSound && node) {
				// token = index into node->sounds (set by ApplyNodeMarks).
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->sounds.size(), idx)) {
					const auto& snd = node->sounds[idx];
					PlaySound(handle, snd.spec, snd.role, snd.volume);
				}
			} else if (m.lane == kLaneCamera && node) {
				// token = index into node->cameras (set by ApplyNodeMarks).
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->cameras.size(), idx)) {
					RunCamera(handle, node->cameras[idx].state, hasPlayer);
				}
			}
		}

		// EVENT_CUE per fired cue on the node it fired on (notification). (Precise fraction/anchor
		// isn't threaded back from the Scene yet; the id is the key field anyway.)
		for (const auto& id : cueIds) {
			DispatchCue(handle, oldNode, id, "", -1.0f);
		}

		// trigger:<cueId> edge auto-take — evaluated after the tick's track entries. The
		// first fired cue with a matching trigger edge on the CURRENT node wins (a transition
		// changes the node, so later cues' triggers are moot). Re-resolve under the lock: an
		// action above could (defensively) have ended the scene or changed the node.
		std::string newNode;
		bool triggered = false;
		bool end = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(handle);
			if (s && s->node == oldNode) {
				const auto* d = Registry::SceneRegistry::GetSingleton().Find(s->id);
				const auto* n = d ? d->FindNode(s->node) : nullptr;
				if (n) {
					for (const auto& id : cueIds) {
						const auto want = Util::ToLower(id);
						const Registry::SceneEdge* edge = nullptr;
						for (const auto& e : n->edges) {
							if (e.when == Registry::EdgeWhen::kTrigger && Util::ToLower(e.trigger) == want) {
								edge = &e;
								break;
							}
						}
						if (edge) {
							triggered = true;
							if (edge->to == "$end") {
								end = true;  // freed after SCENE_END (handle valid through the events)
							} else {
								s->node = edge->to;
								newNode = edge->to;
							}
							break;
						}
					}
				}
			}
		}
		if (triggered) {
			REX::INFO("SceneRuntime: scene {:#010x} cue-trigger node '{}' -> {}", handle, oldNode, end ? "$end" : newNode);
			ApplyTransition(handle, oldNode, newNode, end, sceneId, participants);
		}
	}

	void SceneRuntime::PlayNodeAnim(const std::vector<RE::Actor*>& a_participants, std::string_view a_sceneId, std::string_view a_nodeId)
	{
		if (a_participants.empty()) {
			return;  // synthetic scene with no participants — nothing to play
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
		const auto* node = def ? def->FindNode(a_nodeId) : nullptr;
		if (!node || (node->use.empty() && node->stages.empty())) {
			return;  // not def-backed, or the node has no playable
		}
		// Resolve the node's playable: inline `stages`, or a `use` reference, in the scene registry.
		auto plan = Registry::SceneRegistry::GetSingleton().BuildNodePlan(*def, *node, a_participants.size());
		if (!plan) {
			REX::WARN("SceneRuntime: node '{}' not playable for {} participant(s)", a_nodeId, a_participants.size());
			return;
		}
		// StartSceneAt: if the owning scene carries an explicit world anchor, every node plays
		// anchored there instead of at participant[0]. Read it fresh from the slot so all the
		// transition paths (Advance/Navigate/auto-end/trigger) reuse it with no per-site plumbing.
		{
			std::lock_guard l{ GetSingleton()._lock };
			std::int32_t tok = 0;
			if (Slot* s = GetSingleton().FindSlotForActor(a_participants.front(), &tok); s && s->anchor.set) {
				plan->anchorExplicit = true;
				plan->anchorPos = s->anchor.pos;
				plan->anchorHeading = s->anchor.heading;
			}
		}
		// Stamp the node's loop policy + timerSec onto the plan so the GraphManager auto-ends
		// at the node's terminal condition and reports it back via OnGraphAutoEnd (which takes
		// the matching auto-edge). A `hold` node leaves the stage un-timed and holds for a
		// manual AdvanceScene.
		ApplyNodePolicy(*plan, *node);
		ApplyNodeMarks(*plan, *node);  // node's timed cues + actions -> the played stage (Scene fires them)
		// Re-playing on actors already in a scene tears the old node's scene first, so a
		// node transition is just a fresh PlaySceneStaged.
		Animation::GraphManager::GetSingleton().PlaySceneStaged(a_participants, *plan, 0);
	}

	void SceneRuntime::StopGraph(const std::vector<RE::Actor*>& a_participants)
	{
		if (!a_participants.empty() && a_participants.front()) {
			Animation::GraphManager::GetSingleton().StopScene(a_participants.front());
		}
	}

	void SceneRuntime::EngageDefaultPlayerLock(std::int32_t a_handle, bool a_lockPlayer, const std::vector<RE::Actor*>& a_participants)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(a_participants.begin(), a_participants.end(), static_cast<RE::Actor*>(player)) != a_participants.end();
		if (!hasPlayer) {
			return;  // NPC-only scene, the player isn't involved, so there's nothing to lock.
		}
		// A def scene / pack can decline the default (a scene the player only spectates)
		if (!a_lockPlayer) {
			REX::INFO("SceneRuntime: scene {:#010x} default player lock skipped — opted out (lockPlayer:false)", a_handle);
			return;
		}
		REX::INFO("SceneRuntime: scene {:#010x} default player lock engaged — player is a participant", a_handle);
		RecordMechanism(a_handle, Mechanism::kControlLock);
	}

	void SceneRuntime::StripDefaultActors(std::int32_t a_handle, bool a_stripActors, const std::vector<RE::Actor*>& a_participants)
	{
		if (!a_stripActors) {
			REX::INFO("SceneRuntime: scene {:#010x} default actor strip skipped — opted out (stripActors:false)", a_handle);
			return;
		}
		// Hide every participant's worn apparel
		for (auto* actor : a_participants) {
			if (!actor) {
				continue;
			}
			auto snap = Equipment::EquipmentService::GetSingleton().Hide(actor);
			if (!snap.Empty()) {
				RecordHiddenEquip(a_handle, actor, std::move(snap));
			}
		}
		REX::INFO("SceneRuntime: scene {:#010x} default actor strip applied to {} participant(s)", a_handle, a_participants.size());
	}

	void SceneRuntime::EngageDefaultPlayerControl(std::int32_t a_handle, std::string_view a_defId, const std::vector<RE::Actor*>& a_participants)
	{
		// Input control is ENABLED BY DEFAULT (like lockPlayer/stripActors). A def scene can opt out / narrow via its playerControl block; 
		// pack/files scenes (empty defId) always get the default.
		Registry::PlayerControl pc;  // enabled, all capabilities, not locked
		if (!a_defId.empty()) {
			if (const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_defId)) {
				pc = def->playerControl;
			}
		}
		if (!pc.enabled || pc.capabilities == 0) {
			REX::INFO("SceneRuntime: scene {:#010x} playerControl disabled by scene config", a_handle);
			return;  // scene opted out (or disabled every capability) — no input channel
		}
		// v1: the local human drives only when they're a participant (director mode — a non-participant
		// player driving an NPC-only scene — is a later addition; controlRole is parsed but unused here).
		auto*      player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(a_participants.begin(), a_participants.end(), static_cast<RE::Actor*>(player)) != a_participants.end();
		if (!hasPlayer) {
			return;  // NPC-only scene — no local input to drive it (nothing to engage)
		}
		Input::Grant grant;
		grant.handle = a_handle;
		grant.capabilities = pc.capabilities;
		grant.driver = player;
		grant.locked = pc.locked;
		RecordInputChannel(a_handle, grant);
		REX::INFO("SceneRuntime: scene {:#010x} playerControl engaged (capabilities {:#x}, locked {})", a_handle, grant.capabilities, grant.locked);
	}

	void SceneRuntime::ApplyTransition(std::int32_t a_handle, std::string_view a_oldNode, std::string_view a_newNode,
		bool a_end, std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants)
	{
		Fire(a_handle, Event::kNodeExit, a_oldNode, "exit");
		if (a_end) {
			StopGraph(a_participants);  // cleanup after NODE_EXIT, before SCENE_END
			Fire(a_handle, Event::kSceneEnd, a_oldNode, "");
			ReleaseSlot(a_handle);  // generation 0 → invalidates the handle
		} else {
			PlayNodeAnim(a_participants, a_sceneId, a_newNode);
			Fire(a_handle, Event::kNodeEnter, a_newNode, "enter");
		}
	}

	std::int32_t SceneRuntime::Start(std::string_view a_id, std::string_view a_entryNode,
		const std::vector<RE::Actor*>& a_participants, const AnchorOverride& a_anchor)
	{
		// Filter enforcement (every def start path funnels through here): a bound actor must satisfy
		// its role's filters. Binding is role-declaration order (participants[i] <-> roles[i]); validate
		// the bound pairs. StartSceneByTags pre-binds via matchmaking so this always passes for it;
		// the explicit paths (StartScene/StartSceneAt/StartSceneRoles) reject a bad bind here.
		if (const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id)) {
			const std::size_t n = std::min(a_participants.size(), def->roles.size());
			for (std::size_t i = 0; i < n; i++) {
				if (!Matchmaking::RoleAccepts(def->roles[i], a_participants[i])) {
					REX::WARN("SceneRuntime: start '{}' refused — actor {:X} fails role '{}' filters",
						a_id, a_participants[i] ? a_participants[i]->formID : 0, def->roles[i].name);
					return 0;
				}
			}
		}

		const std::int32_t handle = MintSlot(a_id, a_entryNode, a_participants);
		if (!handle) {
			return 0;
		}
		// Store the explicit anchor on the slot BEFORE the first play so PlayNodeAnim (which
		// reads it back from the slot) and every later node transition reuse it (StartSceneAt).
		if (a_anchor.set) {
			std::lock_guard l{ _lock };
			if (Slot* s = Resolve(handle)) {
				s->anchor = a_anchor;
			}
		}
		PlayNodeAnim(a_participants, a_id, a_entryNode);
		// Resolve the def's opt-outs (both default-on when the scene has no def / omits the key).
		bool lockPlayer = true;
		bool stripActors = true;
		if (const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id)) {
			lockPlayer = def->lockPlayer;
			stripActors = def->stripActors;
		}
		// Default-lock the player's input BEFORE the enter actions run, so an authored osf.control.lock is a no-op and the ledger records the control lock first (undone last).
		EngageDefaultPlayerLock(handle, lockPlayer, a_participants);
		StripDefaultActors(handle, stripActors, a_participants);  // hide every participant's apparel by default
		// Recorded AFTER lock + strip so the ledger undoes the input channel FIRST (release the wheel before unlocking/redressing).
		EngageDefaultPlayerControl(handle, a_id, a_participants);
		Fire(handle, Event::kNodeEnter, a_entryNode, "enter");
		return handle;
	}

	bool SceneRuntime::SetNode(std::int32_t a_scene, std::string_view a_node)
	{
		std::string oldNode;
		std::string newNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			oldNode = s->node;
			s->node = std::string(a_node);
			newNode = s->node;
			sceneId = s->id;
			participants = s->participants;
		}

		ApplyTransition(a_scene, oldNode, newNode, /*a_end*/ false, sceneId, participants);
		return true;
	}

	bool SceneRuntime::Stop(std::int32_t a_scene)
	{
		// Snapshot keeps the handle valid through NODE_EXIT + SCENE_END: the exit cues resolve it to
		// look up the node, and the handle stays read-only-valid during SCENE_END. Released right after.
		SlotView view;
		if (!SnapshotSlot(a_scene, view)) {
			return false;
		}

		// Stop via the normal "end" transition (which trigers all the right events)
		ApplyTransition(a_scene, view.node, /*a_newNode*/ "", /*a_end*/ true, view.id, view.participants);
		return true;
	}

	bool SceneRuntime::StopForActor(RE::Actor* a_actor)
	{
		std::int32_t handle = 0;
		{
			std::lock_guard l{ _lock };
			if (!FindSlotForActor(a_actor, &handle)) {
				return false;
			}
		}
		return Stop(handle);  // re-resolves under its own lock; fires NODE_EXIT + SCENE_END
	}

	std::int32_t SceneRuntime::StartFromDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants)
	{
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::WARN("SceneRuntime::StartFromDef: no scene def '{}'", a_sceneId);
			return 0;
		}
		// Start mints the handle, records the instance, and fires NODE_ENTER for the entry.
		return Start(def->id, def->entry, a_participants);
	}

	std::int32_t SceneRuntime::StartFromDefAt(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants,
		RE::NiPoint3 a_anchorPos, float a_anchorHeading)
	{
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::WARN("SceneRuntime::StartFromDefAt: no scene def '{}'", a_sceneId);
			return 0;
		}
		return Start(def->id, def->entry, a_participants, AnchorOverride{ true, a_anchorPos, a_anchorHeading });
	}

	std::int32_t SceneRuntime::StartFromFiles(const std::vector<RE::Actor*>& a_participants,
		const std::vector<std::string>& a_files, float a_speed, float a_blendIn)
	{
		if (a_participants.empty() || a_participants.size() != a_files.size()) {
			return 0;
		}
		const std::int32_t handle = MintSlot("", "main", a_participants);
		if (!handle) {
			return 0;  // actor already in a scene
		}
		Animation::ScenePlan plan;
		plan.stages.push_back({ a_files, {}, 0.0f });  // one stage, holds (loop mode hold)
		plan.speed = a_speed;
		plan.blendIn = a_blendIn;
		if (!Animation::GraphManager::GetSingleton().PlaySceneStaged(a_participants, plan, 0)) {
			ReleaseSlot(handle);
			return 0;
		}
		EngageDefaultPlayerLock(handle, true, a_participants);   // files scene: no field to opt out via
		StripDefaultActors(handle, true, a_participants);        // files scene: always strips participants
		EngageDefaultPlayerControl(handle, "", a_participants);  // files scene: default-on input (all capabilities)
		Fire(handle, Event::kNodeEnter, "main", "enter");
		return handle;
	}

	std::int32_t SceneRuntime::StartFromDefRoles(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_actors,
		const std::vector<std::string>& a_roles)
	{
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::WARN("StartSceneRoles: no scene def '{}'", a_sceneId);
			return 0;
		}
		if (a_actors.size() != a_roles.size()) {
			REX::WARN("StartSceneRoles '{}': {} actor(s) vs {} role name(s)", a_sceneId, a_actors.size(), a_roles.size());
			return 0;
		}
		if (a_actors.size() != def->roles.size()) {
			REX::WARN("StartSceneRoles '{}': scene declares {} role(s), got {}", a_sceneId, def->roles.size(), a_actors.size());
			return 0;
		}

		// Place each actor into its named role's declaration slot. Rejects unknown roles,
		// a role filled twice, and null actors; missing roles fall out as an unfilled slot.
		std::vector<RE::Actor*> ordered(def->roles.size(), nullptr);
		for (std::size_t i = 0; i < a_actors.size(); i++) {
			if (!a_actors[i]) {
				REX::WARN("StartSceneRoles '{}': null actor for role '{}'", a_sceneId, a_roles[i]);
				return 0;
			}
			const auto want = Util::ToLower(a_roles[i]);
			std::int32_t roleIdx = -1;
			for (std::size_t r = 0; r < def->roles.size(); r++) {
				if (Util::ToLower(def->roles[r].name) == want) {
					roleIdx = static_cast<std::int32_t>(r);
					break;
				}
			}
			if (roleIdx < 0) {
				REX::WARN("StartSceneRoles '{}': unknown role '{}'", a_sceneId, a_roles[i]);
				return 0;
			}
			if (ordered[roleIdx]) {
				REX::WARN("StartSceneRoles '{}': role '{}' assigned twice", a_sceneId, a_roles[i]);
				return 0;
			}
			ordered[roleIdx] = a_actors[i];
		}
		// Every role filled (size match + each filled at most once => all filled); a duplicate
		// actor across two roles is caught by MintSlot's same-actor-twice check.

		// Bind by role-declaration order, then enter at the def's entry (MintSlot enforces
		// actor exclusivity + the duplicate-actor reject).
		return Start(def->id, def->entry, ordered);
	}

	bool SceneRuntime::TakeEdge(std::int32_t a_scene,
		const std::function<const Registry::SceneEdge*(const Registry::SceneNode&)>& a_selectEdge)
	{
		std::string oldNode;
		std::string newNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		bool end = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
			const auto* node = def ? def->FindNode(s->node) : nullptr;
			if (!node) {
				return false;  // not def-backed, or current node not in the def
			}
			const Registry::SceneEdge* edge = a_selectEdge(*node);
			if (!edge) {
				return false;  // selector found no matching edge
			}
			oldNode = s->node;
			sceneId = s->id;
			participants = s->participants;
			if (edge->to == "$end") {
				end = true;  // freed after SCENE_END (handle valid through the events)
			} else {
				s->node = edge->to;
				newNode = edge->to;
			}
		}

		ApplyTransition(a_scene, oldNode, newNode, end, sceneId, participants);
		return true;
	}

	bool SceneRuntime::Advance(std::int32_t a_scene)
	{
		// Read the node before the transition so we can name where we came from in the log.
		// Empty => the handle is dead (scene ended / loaded) — the keypress hit nothing.
		const std::string fromNode = GetNode(a_scene);
		if (fromNode.empty()) {
			REX::INFO("SceneRuntime: Advance scene {:#010x} ignored — no active scene on this handle", a_scene);
			return false;
		}

		// The node's DEFAULT advance edge (never inferred).
		const bool took = TakeEdge(a_scene, [](const Registry::SceneNode& a_node) -> const Registry::SceneEdge* {
			for (const auto& e : a_node.edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && e.isDefault) {
					return &e;
				}
			}
			return nullptr;
		});

		if (took) {
			// After the transition the handle may be freed (edge targeted "$end") — empty node = scene ended.
			const std::string toNode = GetNode(a_scene);
			REX::INFO("SceneRuntime: Advance scene {:#010x} — node '{}' -> {}",
				a_scene, fromNode, toNode.empty() ? "$end (scene ended)" : ("'" + toNode + "'"));
		} else {
			REX::INFO("SceneRuntime: Advance scene {:#010x} — node '{}' has no default advance edge (nothing to do)",
				a_scene, fromNode);
		}
		return took;
	}

	bool SceneRuntime::Navigate(std::int32_t a_scene, std::string_view a_edgeId)
	{
		// The branchable advance edge whose id matches a_edgeId.
		const auto wantId = Util::ToLower(std::string(a_edgeId));
		return TakeEdge(a_scene, [&wantId](const Registry::SceneNode& a_node) -> const Registry::SceneEdge* {
			for (const auto& e : a_node.edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && Util::ToLower(e.id) == wantId) {
					return &e;
				}
			}
			return nullptr;
		});
	}

	bool SceneRuntime::OnGraphAutoEnd(const std::vector<RE::Actor*>& a_participants, Animation::SceneEndReason a_reason)
	{
		std::int32_t handle = 0;
		std::string oldNode;
		std::string newNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		bool end = false;
		bool tookEdge = false;  // an explicit edge matched (vs terminal completion)
		{
			std::lock_guard l{ _lock };

			// Resolve the live handle owning these participants. Actor exclusivity makes
			// this single-valued: the first participant that maps to a slot is THE scene.
			// Unowned => not a SceneRuntime scene (GraphManager stops it itself).
			Slot* s = nullptr;
			for (auto* a : a_participants) {
				if ((s = FindSlotForActor(a, &handle))) {
					break;
				}
			}
			if (!s) {
				return false;  // standalone scene (e.g. PlaySequence) — GraphManager stops it
			}

			oldNode = s->node;
			sceneId = s->id;
			participants = s->participants;

			// The handle is freed after the events for any `end` path (ReleaseSlot below) so exit
			// cues resolve it and SCENE_END dispatches with a valid handle.
			// A registry scene takes its matching auto-edge; an ad-hoc files scene (no registry def)
			// has no edges, so its terminal stage IS the whole scene ending — it falls through the
			// no-node path. We own the teardown so SCENE_END fires and the handle invalidates here
			// (vs returning false and letting GraphManager stop it silently, which would leak it).
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
			const auto* node = def ? def->FindNode(s->node) : nullptr;
			if (!node) {
				end = true;  // files scene, or the def/node vanished — end defensively.
			} else {
				// Map the fired condition to the node's edge semantics: a timer arms a
				// `timer` edge; a loop/clip-end arms `loops` (count) or `end` (once).
				const auto wantWhen = (a_reason == Animation::SceneEndReason::kTimer)
					? Registry::EdgeWhen::kTimer
					: (node->loopMode == Registry::LoopMode::kCount ? Registry::EdgeWhen::kLoops : Registry::EdgeWhen::kEnd);
				const Registry::SceneEdge* edge = SelectAutoEdge(*node, wantWhen);
				if (edge) {
					tookEdge = true;
					if (edge->to == "$end") {
						end = true;
					} else {
						s->node = edge->to;
						newNode = edge->to;
					}
				} else {
					// No matching edge. once/count with no outgoing edge = terminal completion
					// (ends when the clip / loop target finishes). A timer can't land here (we
					// only arm the stage timer when a `timer` edge exists).
					end = true;
				}
			}
		}

		REX::INFO("SceneRuntime: scene {:#010x} auto-end (reason={}) node '{}' -> {}{}",
			handle, a_reason == Animation::SceneEndReason::kTimer ? "timer" : "loops",
			oldNode, end ? "$end" : newNode, tookEdge ? "" : " (terminal, no edge)");

		ApplyTransition(handle, oldNode, newNode, end, sceneId, participants);
		return true;
	}

	std::vector<SceneRuntime::AdvanceEdgeInfo> SceneRuntime::AdvanceEdges(std::int32_t a_scene)
	{
		std::vector<AdvanceEdgeInfo> out;
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return out;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return out;
		}
		for (const auto& e : node->edges) {
			// Only id'd advance edges are navigable branch choices (Navigate matches by id). The
			// synthetic default advance edge on a linear stage carries no id — skip it here so it
			// doesn't show up as a blank menu entry.
			if (e.when == Registry::EdgeWhen::kAdvance && !e.id.empty()) {
				// labelKey (a localization token) if present, else the literal label.
				out.push_back({ e.id, e.labelKey.empty() ? e.label : e.labelKey });
			}
		}
		return out;
	}

	std::int32_t SceneRuntime::EdgeCount(std::int32_t a_scene)
	{
		return static_cast<std::int32_t>(AdvanceEdges(a_scene).size());
	}

	std::string SceneRuntime::EdgeId(std::int32_t a_scene, std::int32_t a_index)
	{
		const auto edges = AdvanceEdges(a_scene);
		return (a_index >= 0 && static_cast<std::size_t>(a_index) < edges.size()) ? edges[a_index].id : std::string{};
	}

	std::string SceneRuntime::EdgeLabel(std::int32_t a_scene, std::int32_t a_index)
	{
		const auto edges = AdvanceEdges(a_scene);
		return (a_index >= 0 && static_cast<std::size_t>(a_index) < edges.size()) ? edges[a_index].label : std::string{};
	}
}
