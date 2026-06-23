#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Registry/SceneRegistry.h"

#include <algorithm>

// SceneRuntime is one class implemented across several translation units, grouped by concern:
//   - SceneRuntime.cpp           — this file: the handle table, lookups, snapshots, GraphManager registration, and the linear-stage accessors.
//   - SceneRuntime_Graph.cpp     — starting scenes, node transitions (ApplyTransition), navigation, auto-end / cue-trigger, and timed-mark decoding.
//   - SceneRuntime_Dispatch.cpp  — lifecycle events through the relay + action/sound/camera execution.
//   - SceneRuntime_Ledger.cpp    — the per-handle undo ledger (record / undo / replay).

namespace OSF::Scene
{
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
		// Decode the timed marks a scene's stage crosses (cue -> EVENT_CUE/trigger, action -> run).
		gm.SetSceneTimedMarkHandler([](const std::vector<RE::Actor*>& a_actors, const std::vector<Animation::FiredMark>& a_marks) {
			SceneRuntime::GetSingleton().OnTimedMarks(a_actors, a_marks);
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

	bool SceneRuntime::SnapshotSlot(std::int32_t a_handle, SlotView& a_out)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_handle);
		if (!s) {
			return false;
		}
		a_out.kind = s->kind;
		a_out.id = s->id;
		a_out.node = s->node;
		a_out.participants = s->participants;
		return true;
	}

	std::int32_t SceneRuntime::MintSlot(Kind a_kind, std::string_view a_id, std::string_view a_node, const std::vector<RE::Actor*>& a_participants)
	{
		std::lock_guard l{ _lock };

		// Reject a null actor or the same actor passed twice in one call - both would break the one-actor-one-scene model the rest of the runtime relies on.
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

		// Actor exclusivity: an actor is in at most one live scene, so a start on a busy actor fails (0), the caller must Stop it first. 
		// Without this, two handles could alias one actor, making GetSceneForActor multi-valued and the auto-end resolver ambiguous.
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

	std::string SceneRuntime::GetId(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return {};
		}
		// A files scene has no registry id — synthesize one.
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
		SlotView view;
		if (!SnapshotSlot(a_scene, view)) {
			return -1;
		}
		RE::Actor* first = view.participants.empty() ? nullptr : view.participants.front();
		if (view.kind != Kind::kDef) {
			// pack/files: the single GraphManager scene's live stage (files -> always 0).
			return Animation::GraphManager::GetSingleton().GetSceneStage(first);
		}
		// def graph: a stage number exists only if linearStages is declared.
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(view.id);
		return def ? def->LinearStageOf(view.node) : -1;
	}

	bool SceneRuntime::SetStage(std::int32_t a_scene, std::int32_t a_stage)
	{
		SlotView view;
		if (!SnapshotSlot(a_scene, view)) {
			return false;
		}
		RE::Actor* first = view.participants.empty() ? nullptr : view.participants.front();
		if (view.kind != Kind::kDef) {
			// pack/files: jump the live GraphManager scene (it range-checks the stage).
			return Animation::GraphManager::GetSingleton().SetSceneStage(first, a_stage);
		}
		// def graph: linear only via linearStages; jumping a stage = transitioning to its node.
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(view.id);
		if (!def || a_stage < 0 || static_cast<std::size_t>(a_stage) >= def->linearStages.size()) {
			return false;
		}
		// SetNode fires NODE_EXIT/ENTER and plays the target node (re-locks internally).
		return SetNode(a_scene, def->linearStages[a_stage]);
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
		// The actual player lock is released by GraphManager::StopAll (PlayerControlService / CameraService OnStopAll) before this runs; just drop the ref-count.
		_controlLockCount = 0;
	}
}
