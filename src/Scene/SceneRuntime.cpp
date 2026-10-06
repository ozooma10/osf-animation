#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Registry/SceneRegistry.h"

#include <algorithm>
#include <chrono>

// Handle table here; see also SceneRuntime_Graph / _Dispatch / _Ledger / _Props.

namespace OSF::Scene
{
	namespace
	{
		constexpr std::size_t kMaxRetiredRosters = 256;
		// Bails if the scene ended since the keypress, keeping us off a stale driver Actor*.
		void DispatchInputVerb(Input::Verb a_verb, const Input::Grant& a_grant)
		{
			auto& rt = SceneRuntime::GetSingleton();
			if (rt.GetNode(a_grant.handle).empty()) {
				REX::DEBUG("[Scene] input verb {} dropped — scene {:#010x} already ended", static_cast<int>(a_verb), a_grant.handle);
				return;
			}
			auto&              gm = Animation::GraphManager::GetSingleton();
			static float       s_prePauseSpeed = 1.0f;  // game-thread only
			constexpr float    kStep = 0.25f;
			constexpr float    kMax = 5.0f;
			constexpr float    kMin = 0.1f;
			switch (a_verb) {
			case Input::Verb::kAdvance: {
				// Single-stage: advance cancels, undebounced; same locked gate as kEnd.
				if (rt.IsSingleStage(a_grant.handle)) {
					if (!a_grant.locked) {
						rt.Stop(a_grant.handle);
					}
					break;
				}
				// Mashing advance would otherwise skip several stages before the player sees one.
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
				// Arm only on a real transition.
				if (rt.Advance(a_grant.handle)) {
					s_lastAdvanceHandle = a_grant.handle;
					s_lastAdvance = now;
				}
				break;
			}
			case Input::Verb::kFreecam: {
				using clock = std::chrono::steady_clock;
				constexpr auto           kFreecamCooldown = std::chrono::milliseconds(500);
				static clock::time_point s_lastFreecam{};  // game-thread only
				const auto               now = clock::now();
				if (now - s_lastFreecam < kFreecamCooldown) {
					REX::DEBUG("[Scene] freecam toggle debounced (within {}ms cooldown)",
						std::chrono::duration_cast<std::chrono::milliseconds>(kFreecamCooldown).count());
					break;
				}
				s_lastFreecam = now;
				Camera::CameraService::GetSingleton().ToggleFreeCam();  // kFreecam cap enforced in InputService
				break;
			}
			case Input::Verb::kEnd:
				rt.Stop(a_grant.handle);  // Grant.locked enforced in InputService
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
		if (_playbackSinkId != 0) {
			return;
		}
		Animation::GraphManager::PlaybackSink sink;
		sink.autoEnd = [](Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_actors, Animation::PlaybackEndReason a_reason) {
				return SceneRuntime::GetSingleton().OnGraphAutoEnd(a_playbackId, a_actors, a_reason);
			};
		// Handles hold raw Actor* participants; drop them on load teardown.
		sink.clear = []() { SceneRuntime::GetSingleton().Clear(); };
		sink.timedMarks = [](Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_actors,
			const std::vector<Animation::FiredMark>& a_marks) {
			SceneRuntime::GetSingleton().OnTimedMarks(a_playbackId, a_actors, a_marks);
		};
		_playbackSinkId = gm.RegisterPlaybackSink(std::move(sink));
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

	SceneRuntime::Slot* SceneRuntime::FindSlotForPlayback(Animation::PlaybackId a_playbackId, std::int32_t* a_token)
	{
		if (a_playbackId == 0) {
			return nullptr;
		}
		for (std::uint16_t slot = 0; slot < _slots.size(); slot++) {
			Slot& s = _slots[slot];
			if (s.generation == 0 || s.ended || s.playbackId != a_playbackId) {
				continue;
			}
			if (a_token) {
				*a_token = MakeToken(s.generation, slot);
			}
			return &s;
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
		a_out.playbackId = s->playbackId;
		a_out.definition = s->definition;
		a_out.cameraOverridden = s->cameraOverridden;
		return true;
	}

	std::int32_t SceneRuntime::MintSlot(std::string_view a_id, std::string_view a_node, const std::vector<RE::Actor*>& a_participants)
	{
		std::lock_guard l{ _lock };

		// Null or duplicate actors would break the one-actor-one-scene invariant.
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

		// A busy actor fails the start; aliasing would make actor lookup and auto-end resolution ambiguous.
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
			// Retired slots aren't reclaimed here: Papyrus dispatch is async. ReleaseSlot expires them FIFO.
			if (_slots[slot].generation == 0) {
				reused = true;
				break;
			}
		}
		if (!reused) {
			if (_slots.size() >= 0xFFFF) {
				// 65k-slot pressure valve; live scenes are never displaced.
				for (slot = 0; slot < _slots.size(); slot++) {
					if (_slots[slot].ended) {
						reused = true;
						REX::WARN("[Scene] handle table exhausted — reclaiming an ended roster tombstone");
						break;
					}
				}
				if (!reused) {
					REX::ERROR("[Scene] handle table full of live scenes");
					return 0;
				}
			} else {
				_slots.emplace_back();
			}
		}

		const std::uint16_t gen = _nextGen++;
		if (_nextGen == 0) {
			_nextGen = 1;  // 0 = empty-slot marker
		}

		Slot& s = _slots[slot];
		s = Slot{};
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
			// Keep only the roster, for async SCENE_END consumers; bounded FIFO.
			const std::uint16_t gen = s->generation;
			std::vector<RE::Actor*> roster = std::move(s->participants);
			std::vector<RE::NiPointer<RE::Actor>> retained;
			retained.reserve(roster.size());
			for (auto* actor : roster) {
				retained.emplace_back(actor);
			}
			*s = Slot{};
			s->generation = gen;
			s->ended = true;
			s->participants = std::move(roster);
			s->retiredParticipantRefs = std::move(retained);
			_retiredHandles.push_back(a_handle);

			while (_retiredHandles.size() > kMaxRetiredRosters) {
				const auto expiredHandle = _retiredHandles.front();
				_retiredHandles.pop_front();
				if (Slot* expired = Resolve(expiredHandle, true); expired && expired->ended) {
					*expired = Slot{};
				}
			}
		}
	}
	std::string SceneRuntime::GetNode(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		return s ? s->node : std::string{};
	}

	void SceneRuntime::SetSingleStage(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		if (Slot* s = Resolve(a_scene)) {
			s->singleStage = true;
			REX::DEBUG("[Scene] scene {:#010x} pinned single-stage — any edge out of node '{}' ends it", a_scene, s->node);
		}
	}

	bool SceneRuntime::IsSingleStage(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_scene);
		return s && s->singleStage;
	}

	std::vector<RE::Actor*> SceneRuntime::GetParticipants(std::int32_t a_scene)
	{
		std::lock_guard l{ _lock };
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
		const auto def = view.definition;  // null for plan scenes
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
		const auto def = view.definition;
		if (!def) {
			return Animation::GraphManager::GetSingleton().SetSceneStage(first, a_stage, view.playbackId);
		}
		if (a_stage < 0 || static_cast<std::size_t>(a_stage) >= def->linearStages.size()) {
			return false;
		}
		return SetNode(a_scene, def->linearStages[a_stage]);
	}

	std::int32_t SceneRuntime::GetSceneForActor(RE::Actor* a_actor)
	{
		std::lock_guard l{ _lock };
		std::int32_t token = 0;
		return FindSlotForActor(a_actor, &token) ? token : 0;
	}

	std::vector<SceneRuntime::ActiveScene> SceneRuntime::ListScenes()
	{
		std::lock_guard l{ _lock };
		std::vector<ActiveScene> out;
		for (std::uint16_t slot = 0; slot < _slots.size(); slot++) {
			const Slot& s = _slots[slot];
			if (s.generation == 0 || s.ended) {
				continue;
			}
			out.push_back(ActiveScene{ MakeToken(s.generation, slot), s.id, s.node, s.participants, s.playbackId });
		}
		return out;
	}

	void SceneRuntime::SetSceneObserver(std::function<void()> a_fn)
	{
		std::lock_guard l{ _lock };
		_sceneObserver = std::move(a_fn);
	}

	void SceneRuntime::NotifySceneObserver()
	{
		std::function<void()> fn;
		{
			std::lock_guard l{ _lock };
			fn = _sceneObserver;
		}
		if (fn) {
			fn();
		}
	}

	void SceneRuntime::Clear()
	{
		std::vector<ActiveProp> props;
		{
			std::lock_guard l{ _lock };
			for (auto& slot : _slots) {
				for (auto& prop : slot.props) {
					props.push_back(std::move(prop));
				}
			}
			_slots.clear();
			_retiredHandles.clear();
		}
		// Load teardown suppresses SCENE_END, so destroy props here while actor roots are valid.
		for (auto& prop : props) {
			std::string error;
			if (!Props::PropService::GetSingleton().Destroy(
					prop.instance, &error)) {
				REX::ERROR("[Scene] load prop cleanup '{}' failed: {}", prop.id, error);
			}
		}
		// _nextGen stays monotonic so pre-load handles read as dead. GraphManager::StopAll already
		// released the player input lock.
	}
}
