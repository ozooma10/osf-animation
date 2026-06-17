#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Player/PlayerControlService.h"
#include "Registry/PackRegistry.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Util/StringUtil.h"

#include <algorithm>

namespace OSF::Scene
{
	namespace
	{
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

		// Map a node's TIMED cue-track entries (numeric `at` + `end`) onto the played plan's
		// terminal stage, so the Scene fires them by clip time. Enter/exit cues are fired by
		// Layer-B lifecycle (DispatchLifecycleCues), not here.
		void ApplyNodeCues(Animation::ScenePlan& a_plan, const Registry::SceneNode& a_node)
		{
			if (a_plan.stages.empty()) {
				return;
			}
			auto& stage = a_plan.stages.back();
			for (const auto& cue : a_node.cues) {
				if (cue.pos == Registry::CuePos::kFraction) {
					stage.cues.push_back({ cue.fraction, cue.everyLoop, false, cue.id });
				} else if (cue.pos == Registry::CuePos::kEnd) {
					stage.cues.push_back({ 0.0f, false, true, cue.id });
				}
			}
		}

		// The outgoing auto-edge of a_node whose `when` matches, by §1.3 arbitration: highest
		// priority wins, ties keep declaration order. nullptr if the node has no such edge.
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

	SceneRuntime& SceneRuntime::GetSingleton()
	{
		static SceneRuntime singleton;
		return singleton;
	}

	void SceneRuntime::RegisterWithGraphManager()
	{
		auto& gm = Animation::GraphManager::GetSingleton();
		gm.SetSceneAutoEndHandler(
			[](const std::vector<RE::Actor*>& a_actors, Animation::SceneEndReason a_reason) {
				return SceneRuntime::GetSingleton().OnGraphAutoEnd(a_actors, a_reason);
			});
		// Drop the handle table on any load teardown (handles hold raw Actor* participants).
		gm.SetSceneClearHandler([]() { SceneRuntime::GetSingleton().Clear(); });
		// Fire EVENT_CUE for the timed cues a scene's stage crosses.
		gm.SetSceneCueHandler([](const std::vector<RE::Actor*>& a_actors, const std::vector<std::string>& a_ids) {
			SceneRuntime::GetSingleton().OnTimedCues(a_actors, a_ids);
		});
	}

	SceneRuntime::Slot* SceneRuntime::Resolve(std::int32_t a_handle)
	{
		if (a_handle == 0) {
			return nullptr;
		}
		const auto slot = static_cast<std::uint16_t>(a_handle & 0xFFFF);
		const auto gen = static_cast<std::uint16_t>((a_handle >> 16) & 0xFFFF);
		if (slot >= _slots.size() || _slots[slot].generation != gen) {
			return nullptr;
		}
		return &_slots[slot];
	}

	SceneRuntime::Slot* SceneRuntime::FindSlotForActor(RE::Actor* a_actor, std::int32_t* a_token)
	{
		if (!a_actor) {
			return nullptr;
		}
		for (std::uint16_t slot = 0; slot < _slots.size(); slot++) {
			Slot& s = _slots[slot];
			if (s.generation == 0) {
				continue;
			}
			if (std::find(s.participants.begin(), s.participants.end(), a_actor) != s.participants.end()) {
				if (a_token) {
					*a_token = MakeToken(s.generation, slot);
				}
				return &s;
			}
		}
		return nullptr;
	}

	void SceneRuntime::Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_anchor)
	{
		// Undo-ledger replay before SCENE_END (§1.5): release this scene's control lock so a
		// listener reacting to scene-end already sees the player restored. Runs on every
		// termination path (they all Fire SCENE_END), so cleanup never depends on an authored
		// release action.
		if (a_event == Event::kSceneEnd) {
			GetSingleton().ReleaseSceneControlLock(a_handle);
		}

		// "exit" track entries run BEFORE the structural NODE_EXIT (§1.5); within the tick,
		// action runs before cue (§1.3 same-tick order).
		if (a_event == Event::kNodeExit) {
			DispatchLifecycleActions(a_handle, a_node, false);
			DispatchLifecycleCues(a_handle, a_node, false);
		}

		// Logged so the lifecycle is visible even with no registered receiver; the relay
		// delivers the OSFEvent:SceneEvent struct to any that are registered.
		REX::INFO("SceneRuntime: scene {:#010x} event {:#x} node='{}' anchor='{}'", a_handle, a_event, a_node, a_anchor);
		SceneEvent e;
		e.scene = a_handle;
		e.event = a_event;
		e.node = std::string(a_node);
		e.anchor = std::string(a_anchor);
		// actor/role left default (Phase A).
		SceneEventRelay::GetSingleton().Dispatch(e);

		// "enter" track entries run AFTER the structural NODE_ENTER; action before cue (§1.3).
		if (a_event == Event::kNodeEnter) {
			DispatchLifecycleActions(a_handle, a_node, true);
			DispatchLifecycleCues(a_handle, a_node, true);
		}
	}

	void SceneRuntime::DispatchCue(std::int32_t a_handle, std::string_view a_node, std::string_view a_cue,
		std::string_view a_anchor, float a_time)
	{
		REX::INFO("SceneRuntime: scene {:#010x} CUE '{}' node='{}' anchor='{}' time={:.3f}",
			a_handle, a_cue, a_node, a_anchor, a_time);
		SceneEvent e;
		e.scene = a_handle;
		e.event = Event::kCue;
		e.node = std::string(a_node);
		e.cue = std::string(a_cue);
		e.anchor = std::string(a_anchor);
		e.time = a_time;
		SceneEventRelay::GetSingleton().Dispatch(e);
	}

	void SceneRuntime::DispatchLifecycleCues(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		const std::string id = GetSingleton().GetId(a_handle);  // "" for pack/files -> no cues
		if (id.empty()) {
			return;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(id);
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;
		}
		const auto wantPos = a_enter ? Registry::CuePos::kEnter : Registry::CuePos::kExit;
		for (const auto& cue : node->cues) {
			if (cue.pos == wantPos) {
				DispatchCue(a_handle, a_node, cue.id, a_enter ? "enter" : "exit", -1.0f);
			}
		}
	}

	void SceneRuntime::DispatchAction(std::int32_t a_handle, std::string_view a_node, std::string_view a_type,
		std::string_view a_role, std::string_view a_anchor)
	{
		REX::INFO("SceneRuntime: scene {:#010x} ACTION '{}' node='{}' role='{}' anchor='{}'",
			a_handle, a_type, a_node, a_role, a_anchor);
		SceneEvent e;
		e.scene = a_handle;
		e.event = Event::kAction;
		e.node = std::string(a_node);
		e.actionType = std::string(a_type);
		e.role = std::string(a_role);
		e.anchor = std::string(a_anchor);
		SceneEventRelay::GetSingleton().Dispatch(e);
	}

	void SceneRuntime::AcquireSceneControlLock(std::int32_t a_handle)
	{
		bool engage = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s || s->controlLocked) {
				return;  // invalid, or this scene already holds the lock (idempotent)
			}
			s->controlLocked = true;
			engage = (++_controlLockCount == 1);  // first holder engages the actual lock
		}
		if (engage) {
			Player::PlayerControlService::GetSingleton().SetStandaloneLock(true);
			Camera::CameraService::GetSingleton().SetStandaloneLock(true);
		}
	}

	void SceneRuntime::ReleaseSceneControlLock(std::int32_t a_handle)
	{
		bool disengage = false;
		std::int32_t remaining = 0;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s || !s->controlLocked) {
				return;
			}
			s->controlLocked = false;
			disengage = (--_controlLockCount <= 0);  // last holder releases the actual lock
			if (_controlLockCount < 0) {
				_controlLockCount = 0;
			}
			remaining = _controlLockCount;
		}
		if (disengage) {
			REX::INFO("SceneRuntime: scene {:#010x} control lock released — player unlocked", a_handle);
			Player::PlayerControlService::GetSingleton().SetStandaloneLock(false);
			Camera::CameraService::GetSingleton().SetStandaloneLock(false);
		} else {
			REX::INFO("SceneRuntime: scene {:#010x} control lock released — {} scene(s) still hold it", a_handle, remaining);
		}
	}

	void SceneRuntime::DispatchLifecycleActions(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		std::string id;
		std::vector<RE::Actor*> participants;
		{
			std::lock_guard l{ GetSingleton()._lock };
			Slot* s = GetSingleton().Resolve(a_handle);
			if (!s) {
				return;
			}
			id = s->id;
			participants = s->participants;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(id);
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;  // pack/files scene or unknown node — no actions
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(participants.begin(), participants.end(), static_cast<RE::Actor*>(player)) != participants.end();

		const auto wantPos = a_enter ? Registry::ActionPos::kEnter : Registry::ActionPos::kExit;
		for (const auto& act : node->actions) {
			if (act.pos != wantPos) {
				continue;
			}
			const auto type = Util::ToLower(act.type);
			if (type == "osf.control.lock") {
				if (hasPlayer) {
					REX::INFO("SceneRuntime: scene {:#010x} action osf.control.lock (role '{}')", a_handle, act.role);
					GetSingleton().AcquireSceneControlLock(a_handle);
				} else {
					REX::INFO("SceneRuntime: scene {:#010x} osf.control.lock — no player participant, no-op", a_handle);
				}
			} else if (type == "osf.control.release") {
				REX::INFO("SceneRuntime: scene {:#010x} action osf.control.release", a_handle);
				GetSingleton().ReleaseSceneControlLock(a_handle);
			} else if (type.rfind("osf.", 0) == 0) {
				// recognized built-in mechanism, not yet executed (equipment/fade/voice — Layer C).
				REX::INFO("SceneRuntime: scene {:#010x} action '{}' (role '{}') — recognized, not yet executed",
					a_handle, act.type, act.role);
			} else {
				// custom namespaced action -> EVENT_ACTION (best-effort notification, §1.3).
				DispatchAction(a_handle, a_node, act.type, act.role, a_enter ? "enter" : "exit");
			}
		}
	}

	void SceneRuntime::OnTimedCues(const std::vector<RE::Actor*>& a_participants, const std::vector<std::string>& a_cueIds)
	{
		std::int32_t handle = 0;
		std::string oldNode;
		std::string newNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		bool triggered = false;
		bool end = false;
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

			// First fired cue with a matching trigger:<id> edge on the current node wins (a
			// transition changes the node, so later cues' triggers are moot). The slot's node
			// is set now for the transition; an $end stays valid through SCENE_END (freed below).
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
			const auto* node = def ? def->FindNode(s->node) : nullptr;
			if (node) {
				for (const auto& id : a_cueIds) {
					const auto want = Util::ToLower(id);
					const Registry::SceneEdge* edge = nullptr;
					for (const auto& e : node->edges) {
						if (e.when == Registry::EdgeWhen::kTrigger && Util::ToLower(e.trigger) == want) {
							edge = &e;
							break;
						}
					}
					if (edge) {
						triggered = true;
						if (edge->to == "$end") {
							end = true;
						} else {
							s->node = edge->to;
							newNode = edge->to;
						}
						break;
					}
				}
			}
		}

		// Every fired cue dispatches EVENT_CUE on the node it fired on (notification); the
		// trigger edge is evaluated after (§1.3). (Precise fraction/anchor isn't threaded back
		// from the Scene in v1; the id is the contract's key field.)
		for (const auto& id : a_cueIds) {
			DispatchCue(handle, oldNode, id, "", -1.0f);
		}
		if (triggered) {
			REX::INFO("SceneRuntime: scene {:#010x} cue-trigger node '{}' -> {}", handle, oldNode, end ? "$end" : newNode);
			Fire(handle, Event::kNodeExit, oldNode, "exit");
			if (end) {
				StopGraph(participants);  // cleanup after NODE_EXIT, before SCENE_END (§1.5)
				Fire(handle, Event::kSceneEnd, oldNode, "");
				ReleaseSlot(handle);
			} else {
				PlayNodeAnim(participants, sceneId, newNode);
				Fire(handle, Event::kNodeEnter, newNode, "enter");
			}
		}
	}

	void SceneRuntime::PlayNodeAnim(const std::vector<RE::Actor*>& a_participants, std::string_view a_sceneId, std::string_view a_nodeId)
	{
		if (a_participants.empty()) {
			return;  // synthetic scene with no participants — nothing to play
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
		const auto* node = def ? def->FindNode(a_nodeId) : nullptr;
		if (!node || node->anim.empty()) {
			return;  // not def-backed, or the node has no anim
		}
		auto plan = Registry::PackRegistry::GetSingleton().BuildScenePlan(node->anim, a_participants.size());
		if (!plan) {
			REX::WARN("SceneRuntime: node '{}' anim '{}' not playable for {} participant(s)",
				a_nodeId, node->anim, a_participants.size());
			return;
		}
		// P3: stamp the node's loop policy + timerSec onto the plan so GraphManager
		// auto-ends at the node's terminal condition and reports it back via OnGraphAutoEnd
		// (which takes the matching auto-edge). A `hold` node leaves the stage un-timed and
		// holds for a manual AdvanceScene, as before.
		ApplyNodePolicy(*plan, *node);
		ApplyNodeCues(*plan, *node);  // node's timed cues -> the played stage (Scene fires them)
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

	std::int32_t SceneRuntime::MintSlot(Kind a_kind, std::string_view a_id, std::string_view a_node,
		std::int32_t a_stage, const std::vector<RE::Actor*>& a_participants)
	{
		std::lock_guard l{ _lock };

		// Reject a null actor or the same actor passed twice in one call (§1.3) — both would
		// break the one-actor-one-scene model the rest of the runtime relies on.
		for (std::size_t i = 0; i < a_participants.size(); i++) {
			if (!a_participants[i]) {
				REX::WARN("SceneRuntime: null actor in start '{}'", a_id);
				return 0;
			}
			for (std::size_t j = i + 1; j < a_participants.size(); j++) {
				if (a_participants[i] == a_participants[j]) {
					REX::WARN("SceneRuntime: actor {:X} passed twice in start '{}'", a_participants[i]->formID, a_id);
					return 0;
				}
			}
		}

		// Actor exclusivity (v1, SCENE_DESIGN §1.3): an actor is in at most one live scene, so
		// a start on a busy actor fails (0) — the caller must Stop it first. Without this, two
		// handles could alias one actor, making GetSceneForActor multi-valued and the auto-end
		// resolver ambiguous.
		for (auto* a : a_participants) {
			std::int32_t busy = 0;
			if (FindSlotForActor(a, &busy)) {
				REX::WARN("SceneRuntime: actor {:X} already in live scene {:#010x} — refusing start '{}' (Stop it first)",
					a->formID, busy, a_id);
				return 0;
			}
		}

		std::uint16_t slot = 0;
		bool reused = false;
		for (; slot < _slots.size(); slot++) {
			if (_slots[slot].generation == 0) {
				reused = true;
				break;
			}
		}
		if (!reused) {
			if (_slots.size() >= 0xFFFF) {
				REX::ERROR("SceneRuntime: handle table full");
				return 0;
			}
			_slots.emplace_back();
		}

		const std::uint16_t gen = _nextGen++;
		if (_nextGen == 0) {
			_nextGen = 1;  // never hand out generation 0 (the empty-slot marker)
		}

		Slot& s = _slots[slot];
		s.generation = gen;
		s.kind = a_kind;
		s.id = std::string(a_id);
		s.node = std::string(a_node);
		s.stage = a_stage;
		s.participants = a_participants;
		return MakeToken(gen, slot);
	}

	void SceneRuntime::ReleaseSlot(std::int32_t a_handle)
	{
		std::lock_guard l{ _lock };
		if (Slot* s = Resolve(a_handle)) {
			*s = Slot{};
		}
	}

	std::int32_t SceneRuntime::Start(std::string_view a_id, std::string_view a_entryNode,
		const std::vector<RE::Actor*>& a_participants)
	{
		const std::int32_t handle = MintSlot(Kind::kDef, a_id, a_entryNode, 0, a_participants);
		if (!handle) {
			return 0;
		}
		PlayNodeAnim(a_participants, a_id, a_entryNode);
		Fire(handle, Event::kNodeEnter, a_entryNode, "enter");
		return handle;
	}

	bool SceneRuntime::SetNode(std::int32_t a_scene, std::string_view a_node, std::int32_t a_stage)
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
			s->stage = a_stage;
			newNode = s->node;
			sceneId = s->id;
			participants = s->participants;
		}

		Fire(a_scene, Event::kNodeExit, oldNode, "exit");
		PlayNodeAnim(participants, sceneId, newNode);
		Fire(a_scene, Event::kNodeEnter, newNode, "enter");
		return true;
	}

	bool SceneRuntime::Stop(std::int32_t a_scene)
	{
		std::string node;
		std::vector<RE::Actor*> participants;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			node = s->node;
			participants = s->participants;
			// Keep the handle valid through NODE_EXIT + SCENE_END: the exit cues resolve it to
			// look up the node, and §1.5 says the handle is read-only-valid during SCENE_END.
			// Released right after.
		}

		StopGraph(participants);
		Fire(a_scene, Event::kNodeExit, node, "exit");
		Fire(a_scene, Event::kSceneEnd, node, "");
		ReleaseSlot(a_scene);  // generation 0 → invalidates the handle
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

	std::int32_t SceneRuntime::StartFromPack(std::string_view a_packId, const std::vector<RE::Actor*>& a_participants, std::int32_t a_startStage)
	{
		if (a_participants.empty()) {
			return 0;
		}
		// Build the pack plan FIRST (cheap validate) so we don't mint a handle for an unknown
		// or wrong-actor-count pack (PackRegistry logs the reason).
		auto plan = Registry::PackRegistry::GetSingleton().BuildScenePlan(a_packId, a_participants.size());
		if (!plan) {
			return 0;
		}
		const std::int32_t handle = MintSlot(Kind::kPack, a_packId, "main", a_startStage, a_participants);
		if (!handle) {
			return 0;  // actor already in a scene
		}
		if (!Animation::GraphManager::GetSingleton().PlaySceneStaged(a_participants, *plan, a_startStage)) {
			ReleaseSlot(handle);  // play failed after mint — no events fired yet, just free it
			return 0;
		}
		Fire(handle, Event::kNodeEnter, "main", "enter");
		return handle;
	}

	std::int32_t SceneRuntime::StartFromFiles(const std::vector<RE::Actor*>& a_participants,
		const std::vector<std::string>& a_files, float a_speed, float a_blendIn)
	{
		if (a_participants.empty() || a_participants.size() != a_files.size()) {
			return 0;
		}
		const std::int32_t handle = MintSlot(Kind::kFiles, "", "main", 0, a_participants);
		if (!handle) {
			return 0;  // actor already in a scene
		}
		Animation::ScenePlan plan;
		plan.stages.push_back({ a_files, {}, 0.0f });  // one stage, holds (loop mode hold, §1.2)
		plan.speed = a_speed;
		plan.blendIn = a_blendIn;
		if (!Animation::GraphManager::GetSingleton().PlaySceneStaged(a_participants, plan, 0)) {
			ReleaseSlot(handle);
			return 0;
		}
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

	bool SceneRuntime::Advance(std::int32_t a_scene)
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
			const Registry::SceneEdge* edge = nullptr;
			for (const auto& e : node->edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && e.isDefault) {
					edge = &e;
					break;
				}
			}
			if (!edge) {
				return false;  // no default advance edge — never inferred
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

		Fire(a_scene, Event::kNodeExit, oldNode, "exit");
		if (end) {
			StopGraph(participants);  // cleanup after NODE_EXIT, before SCENE_END (§1.5)
			Fire(a_scene, Event::kSceneEnd, oldNode, "");
			ReleaseSlot(a_scene);
		} else {
			PlayNodeAnim(participants, sceneId, newNode);
			Fire(a_scene, Event::kNodeEnter, newNode, "enter");
		}
		return true;
	}

	bool SceneRuntime::Navigate(std::int32_t a_scene, std::string_view a_edgeId)
	{
		const auto wantId = Util::ToLower(std::string(a_edgeId));
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
				return false;
			}
			const Registry::SceneEdge* edge = nullptr;
			for (const auto& e : node->edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && Util::ToLower(e.id) == wantId) {
					edge = &e;
					break;
				}
			}
			if (!edge) {
				return false;  // no such branchable edge on the current node
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

		Fire(a_scene, Event::kNodeExit, oldNode, "exit");
		if (end) {
			StopGraph(participants);
			Fire(a_scene, Event::kSceneEnd, oldNode, "");
			ReleaseSlot(a_scene);
		} else {
			PlayNodeAnim(participants, sceneId, newNode);
			Fire(a_scene, Event::kNodeEnter, newNode, "enter");
		}
		return true;
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

			// The handle is freed AFTER the events for any `end` path (ReleaseSlot below) so
			// exit cues resolve it and SCENE_END dispatches with a valid handle (§1.5).
			if (s->kind != Kind::kDef) {
				// Pack / files single-path scene: it has no edges, so its terminal stage IS
				// the whole scene ending. (We still own the teardown so SCENE_END fires and
				// the handle invalidates, vs returning false and letting GraphManager stop it
				// silently — which would leak the handle.)
				end = true;
			} else {
				const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
				const auto* node = def ? def->FindNode(s->node) : nullptr;
				if (!node) {
					// def-backed but the node/def vanished — end defensively (don't leak).
					end = true;
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
						// No matching edge. once/count with no outgoing edge = terminal
						// completion (ends when the clip / loop target finishes, §1.3). A timer
						// can't land here (we only arm the stage timer when a `timer` edge exists).
						end = true;
					}
				}
			}
		}

		REX::INFO("SceneRuntime: scene {:#010x} auto-end (reason={}) node '{}' -> {}{}",
			handle, a_reason == Animation::SceneEndReason::kTimer ? "timer" : "loops",
			oldNode, end ? "$end" : newNode, tookEdge ? "" : " (terminal, no edge)");

		Fire(handle, Event::kNodeExit, oldNode, "exit");
		if (end) {
			StopGraph(participants);  // cleanup after NODE_EXIT, before SCENE_END (§1.5)
			Fire(handle, Event::kSceneEnd, oldNode, "");
			ReleaseSlot(handle);
		} else {
			PlayNodeAnim(participants, sceneId, newNode);
			Fire(handle, Event::kNodeEnter, newNode, "enter");
		}
		return true;
	}

	std::int32_t SceneRuntime::EdgeCount(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return 0;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return 0;
		}
		std::int32_t count = 0;
		for (const auto& e : node->edges) {
			if (e.when == Registry::EdgeWhen::kAdvance) {
				count++;
			}
		}
		return count;
	}

	std::string SceneRuntime::EdgeId(std::int32_t a_scene, std::int32_t a_index)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return {};
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return {};
		}
		std::int32_t i = 0;
		for (const auto& e : node->edges) {
			if (e.when == Registry::EdgeWhen::kAdvance) {
				if (i == a_index) {
					return e.id;
				}
				i++;
			}
		}
		return {};
	}

	std::string SceneRuntime::EdgeLabel(std::int32_t a_scene, std::int32_t a_index)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return {};
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(s->id);
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return {};
		}
		std::int32_t i = 0;
		for (const auto& e : node->edges) {
			if (e.when == Registry::EdgeWhen::kAdvance) {
				if (i == a_index) {
					// labelKey (a localization token) if present, else the literal label.
					return e.labelKey.empty() ? e.label : e.labelKey;
				}
				i++;
			}
		}
		return {};
	}

	std::string SceneRuntime::GetId(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return {};
		}
		// A files scene has no registry id — synthesize the documented one (§1.2).
		if (s->kind == Kind::kFiles) {
			return "runtime.files:" + std::to_string(a_scene);
		}
		return s->id;
	}

	std::string SceneRuntime::GetNode(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		return s ? s->node : std::string{};
	}

	std::int32_t SceneRuntime::GetStage(std::int32_t a_scene)
	{
		Kind kind;
		std::string id;
		std::string node;
		RE::Actor* first = nullptr;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return -1;
			}
			kind = s->kind;
			id = s->id;
			node = s->node;
			first = s->participants.empty() ? nullptr : s->participants.front();
		}
		if (kind != Kind::kDef) {
			// pack/files: the single GraphManager scene's live stage (files -> always 0).
			return Animation::GraphManager::GetSingleton().GetSceneStage(first);
		}
		// def graph: a stage number exists only if linearStages is declared.
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(id);
		return def ? def->LinearStageOf(node) : -1;
	}

	bool SceneRuntime::SetStage(std::int32_t a_scene, std::int32_t a_stage)
	{
		Kind kind;
		std::string id;
		RE::Actor* first = nullptr;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s) {
				return false;
			}
			kind = s->kind;
			id = s->id;
			first = s->participants.empty() ? nullptr : s->participants.front();
		}
		if (kind != Kind::kDef) {
			// pack/files: jump the live GraphManager scene (it range-checks the stage).
			return Animation::GraphManager::GetSingleton().SetSceneStage(first, a_stage);
		}
		// def graph: linear only via linearStages; jumping a stage = transitioning to its node.
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(id);
		if (!def || a_stage < 0 || static_cast<std::size_t>(a_stage) >= def->linearStages.size()) {
			return false;
		}
		// SetNode fires NODE_EXIT/ENTER and plays the target node (re-locks internally).
		return SetNode(a_scene, def->linearStages[a_stage], a_stage);
	}

	std::int32_t SceneRuntime::GetSceneForActor(RE::Actor* a_actor)
	{
		std::lock_guard l{ _lock };
		std::int32_t token = 0;
		return FindSlotForActor(a_actor, &token) ? token : 0;
	}

	void SceneRuntime::Clear()
	{
		std::lock_guard l{ _lock };
		_slots.clear();
		_nextGen = 1;
		// The actual player lock is released by GraphManager::StopAll (PlayerControlService /
		// CameraService OnStopAll) before this runs; just drop the ref-count.
		_controlLockCount = 0;
	}
}
