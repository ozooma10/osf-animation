#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Registry/SceneRegistry.h"

#include <algorithm>
#include <chrono>

// SceneRuntime is one class implemented across several translation units, grouped by concern:
//   - SceneRuntime.cpp           — this file: the handle table, lookups, snapshots, GraphManager registration, and the linear-stage accessors.
//   - SceneRuntime_Graph.cpp     — starting scenes, node transitions (ApplyTransition), navigation, auto-end / cue-trigger, and timed-mark decoding.
//   - SceneRuntime_Dispatch.cpp  — lifecycle events through the relay + action/sound/camera execution.
//   - SceneRuntime_Ledger.cpp    — the per-handle undo ledger (record / undo / replay).

namespace OSF::Scene
{
	namespace
	{
		// Execute one director verb against the active grant. Runs on the game thread (the InputService posts it via the SFSE task queue), so it may safely touch scene/graph state.
		// Bails if the scene ended between the keypress and now (a load ran Clear); that keeps us off a stale driver Actor*, since a live handle implies live participants.
		void DispatchInputVerb(Input::Verb a_verb, const Input::Grant& a_grant)
		{
			auto& rt = SceneRuntime::GetSingleton();
			if (rt.GetNode(a_grant.handle).empty()) {
				REX::DEBUG("[Scene] input verb {} dropped — scene {:#010x} already ended", static_cast<int>(a_verb), a_grant.handle);
				return;  // handle dead (scene ended / load) — do nothing
			}
			auto&              gm = Animation::GraphManager::GetSingleton();
			static float       s_prePauseSpeed = 1.0f;  // game-thread only; remembers speed across a pause toggle
			constexpr float    kStep = 0.25f;
			constexpr float    kMax = 5.0f;
			constexpr float    kMin = 0.1f;
			switch (a_verb) {
			case Input::Verb::kAdvance: {
				// Debounce manual advance. A press starts a node teardown+rebuild; without a
				// meaningful floor, mashing space can stutter-skip through several stages before
				// the player has actually seen the newly entered one.
				using clock = std::chrono::steady_clock;
				constexpr auto      kAdvanceCooldown = std::chrono::seconds(3);
				static std::int32_t s_lastAdvanceHandle = 0;  // game-thread only
				static clock::time_point s_lastAdvance{};
				const auto               now = clock::now();
				if (a_grant.handle == s_lastAdvanceHandle && now - s_lastAdvance < kAdvanceCooldown) {
					REX::DEBUG("[Scene] Advance scene {:#010x} debounced (within {}s cooldown)",
						a_grant.handle,
						std::chrono::duration_cast<std::chrono::seconds>(kAdvanceCooldown).count());
					break;
				}
				// Arm the floor only on a real transition; a no-op press leaves the next genuine advance free to fire immediately.
				if (rt.Advance(a_grant.handle)) {
					s_lastAdvanceHandle = a_grant.handle;
					s_lastAdvance = now;
				}
				break;
			}
			case Input::Verb::kFreecam:
				Camera::CameraService::GetSingleton().ToggleFreeCam();  // MMB toggle; cap kFreecam already enforced in the InputService
				break;
			case Input::Verb::kEnd:
				rt.Stop(a_grant.handle);  // Grant.locked was already enforced in the InputService
				break;
			case Input::Verb::kSpeedUp:
				gm.SetSpeed(a_grant.driver, std::min(gm.GetSpeed(a_grant.driver) + kStep, kMax));
				break;
			case Input::Verb::kSpeedDown:
				gm.SetSpeed(a_grant.driver, std::max(gm.GetSpeed(a_grant.driver) - kStep, kMin));
				break;
			case Input::Verb::kSpeedReset:
				gm.SetSpeed(a_grant.driver, 1.0f);
				break;
			case Input::Verb::kPause: {
				const float cur = gm.GetSpeed(a_grant.driver);
				if (cur > 0.0f) {
					s_prePauseSpeed = cur;
					gm.SetSpeed(a_grant.driver, 0.0f);
				} else {
					gm.SetSpeed(a_grant.driver, s_prePauseSpeed > 0.0f ? s_prePauseSpeed : 1.0f);
				}
				break;
			}
			default:
				break;
			}
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
		// Decode the timed marks a scene's stage crosses (cue -> EVENT_CUE/trigger, action -> run).
		gm.SetSceneTimedMarkHandler([](const std::vector<RE::Actor*>& a_actors, const std::vector<Animation::FiredMark>& a_marks) {
			SceneRuntime::GetSingleton().OnTimedMarks(a_actors, a_marks);
		});
		// How a player director verb (from the InputService input hook) drives the active scene.
		Input::InputService::GetSingleton().SetVerbHandler(
			[](Input::Verb a_verb, const Input::Grant& a_grant) { DispatchInputVerb(a_verb, a_grant); });
	}

	SceneRuntime::Slot* SceneRuntime::Resolve(std::int32_t a_handle, bool a_includeEnded)
	{
		if (a_handle == 0) {
			return nullptr;
		}
		const auto slot = static_cast<std::uint16_t>(a_handle & 0xFFFF);
		const auto gen = static_cast<std::uint16_t>((a_handle >> 16) & 0xFFFF);
		if (slot >= _slots.size() || _slots[slot].generation != gen) {
			return nullptr;
		}
		// An ended slot still matches by generation (so its roster stays readable through the
		// async SCENE_END dispatch) but is dead to everything except the explicit read path.
		if (_slots[slot].ended && !a_includeEnded) {
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
			if (s.generation == 0 || s.ended) {
				continue;  // empty, or retired (its actors are already free to re-scene)
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
		a_out.id = s->id;
		a_out.node = s->node;
		a_out.participants = s->participants;
		return true;
	}

	std::int32_t SceneRuntime::MintSlot(std::string_view a_id, std::string_view a_node, const std::vector<RE::Actor*>& a_participants)
	{
		std::lock_guard l{ _lock };

		// Reject a null actor or the same actor passed twice in one call - both would break the one-actor-one-scene model the rest of the runtime relies on.
		for (std::size_t i = 0; i < a_participants.size(); i++) {
			if (!a_participants[i]) {
				REX::ERROR("[Scene] null actor in start '{}'", a_id);
				return 0;
			}
			for (std::size_t j = i + 1; j < a_participants.size(); j++) {
				if (a_participants[i] == a_participants[j]) {
					REX::ERROR("[Scene] actor {:X} passed twice in start '{}'", a_participants[i]->formID, a_id);
					return 0;
				}
			}
		}

		// Actor exclusivity: an actor is in at most one live scene, so a start on a busy actor fails (0), the caller must Stop it first. 
		// Without this, two handles could alias one actor, making GetSceneForActor multi-valued and the auto-end resolver ambiguous.
		for (auto* a : a_participants) {
			std::int32_t busy = 0;
			if (FindSlotForActor(a, &busy)) {
				REX::WARN("[Scene] actor {:X} already in live scene {:#010x} — refusing start '{}' (Stop it first)",
					a->formID, busy, a_id);
				return 0;
			}
		}

		std::uint16_t slot = 0;
		bool reused = false;
		for (; slot < _slots.size(); slot++) {
			// Reclaim empty slots and retired (ended) ones — the latter still hold a roster from
			// their finished scene, which the reset below discards (and the new generation kills
			// any lingering handle that was still reading it).
			if (_slots[slot].generation == 0 || _slots[slot].ended) {
				reused = true;
				break;
			}
		}
		if (!reused) {
			if (_slots.size() >= 0xFFFF) {
				REX::ERROR("[Scene] handle table full");
				return 0;
			}
			_slots.emplace_back();
		}

		const std::uint16_t gen = _nextGen++;
		if (_nextGen == 0) {
			_nextGen = 1;  // never hand out generation 0 (the empty-slot marker)
		}

		Slot& s = _slots[slot];
		s = Slot{};  // clear any retained state from a reclaimed ended slot (roster/ledger/equip/grant)
		s.generation = gen;
		s.id = std::string(a_id);
		s.node = std::string(a_node);
		s.participants = a_participants;
		return MakeToken(gen, slot);
	}

	void SceneRuntime::ReleaseSlot(std::int32_t a_handle)
	{
		std::lock_guard l{ _lock };
		if (Slot* s = Resolve(a_handle)) {
			// Retire, don't erase: keep the generation (handle still resolves) and the participant
			// roster (the async SCENE_END dispatch reads it via GetParticipants), but drop every
			// other field — the undo ledger already replayed before SCENE_END, so equipment/weapon/
			// grant state is spent. The actors are freed for a new scene (FindSlotForActor skips
			// ended). MintSlot reclaims this slot later, resetting it and bumping the generation.
			const std::uint16_t gen = s->generation;
			std::vector<RE::Actor*> roster = std::move(s->participants);
			*s = Slot{};
			s->generation = gen;
			s->ended = true;
			s->participants = std::move(roster);
		}
	}

	std::string SceneRuntime::GetId(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		if (!s) {
			return {};
		}
		// An ad-hoc files scene has no registry id (empty) — synthesize one.
		if (s->id.empty()) {
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

	std::vector<RE::Actor*> SceneRuntime::GetParticipants(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		// includeEnded: the roster outlives the scene into its SCENE_END callback (see ReleaseSlot).
		Slot* s = Resolve(a_scene, true);
		return s ? s->participants : std::vector<RE::Actor*>{};
	}

	std::int32_t SceneRuntime::GetStage(std::int32_t a_scene)
	{
		SlotView view;
		if (!SnapshotSlot(a_scene, view)) {
			return -1;
		}
		RE::Actor* first = view.participants.empty() ? nullptr : view.participants.front();
		// A registry scene has a stage number only if it declares linearStages; an ad-hoc files
		// scene (no registry def) reports the single GraphManager scene's live stage.
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(view.id);
		if (!def) {
			return Animation::GraphManager::GetSingleton().GetSceneStage(first);
		}
		return def->LinearStageOf(view.node);
	}

	bool SceneRuntime::SetStage(std::int32_t a_scene, std::int32_t a_stage)
	{
		SlotView view;
		if (!SnapshotSlot(a_scene, view)) {
			return false;
		}
		RE::Actor* first = view.participants.empty() ? nullptr : view.participants.front();
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(view.id);
		if (!def) {
			// ad-hoc files scene: jump the live GraphManager scene (it range-checks the stage).
			return Animation::GraphManager::GetSingleton().SetSceneStage(first, a_stage);
		}
		// registry scene: linear only via linearStages; jumping a stage = transitioning to its node.
		if (a_stage < 0 || static_cast<std::size_t>(a_stage) >= def->linearStages.size()) {
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
