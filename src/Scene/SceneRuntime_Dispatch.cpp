#include "Scene/SceneRuntime.h"
#include "Audio/SoundPlayback.h"

#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/ContentRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "Util/FormRef.h"
#include "Util/StringUtil.h"
#include "Weapon/WeaponService.h"

#include <algorithm>

namespace OSF::Scene
{
	void SceneRuntime::Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_trackPosition,
		std::string_view a_edge)
	{
		// Replay before dispatch so SCENE_END listeners see restored actors. Load teardown bypasses this.
		if (a_event == Event::kSceneEnd) {
			GetSingleton().ReplayLedger(a_handle);
		}

		// Exit entries run before NODE_EXIT, enter entries after NODE_ENTER; lane order action -> camera -> sound -> cue.
		if (a_event == Event::kNodeExit) {
			DispatchLifecycleActions(a_handle, a_node, false);
			DispatchLifecycleCamera(a_handle, a_node, false);
			DispatchLifecycleSounds(a_handle, a_node, false);
			DispatchLifecycleCues(a_handle, a_node, false);
		}

		REX::DEBUG("[Scene] scene {:#010x} event {:#x} node='{}' trackPosition='{}'", a_handle, a_event, a_node, a_trackPosition);
		SceneEvent e;
		e.scene = a_handle;
		e.event = a_event;
		{
			auto& rt = GetSingleton();
			std::lock_guard l{ rt._lock };
			if (const auto* s = rt.Resolve(a_handle)) {
				e.sceneId = s->id;
				if (a_event == Event::kSceneEnd) e.cause = static_cast<std::int32_t>(s->terminationCause);
			}
		}
		e.node = std::string(a_node);
		e.edge = std::string(a_edge);
		e.trackPosition = std::string(a_trackPosition);
		SceneEventRelay::GetSingleton().Dispatch(e);

		if (a_event == Event::kNodeEnter) {
			DispatchLifecycleActions(a_handle, a_node, true);
			DispatchLifecycleCamera(a_handle, a_node, true);
			DispatchLifecycleSounds(a_handle, a_node, true);
			DispatchLifecycleCues(a_handle, a_node, true);
		}

		// The observer defers to the task queue, so it sees the slot ReleaseSlot retires next.
		if (a_event == Event::kNodeEnter || a_event == Event::kSceneEnd) {
			GetSingleton().NotifySceneObserver();
		}
	}

	void SceneRuntime::DispatchCue(std::int32_t a_handle, std::string_view a_node, std::string_view a_cue,
		std::string_view a_role, std::string_view a_trackPosition, float a_time, std::uint64_t a_detectedNs, bool a_nativeOnly)
	{
		REX::DEBUG("[Scene] scene {:#010x} CUE '{}' node='{}' role='{}' trackPosition='{}' time={:.3f}",
			a_handle, a_cue, a_node, a_role, a_trackPosition, a_time);
		SceneEvent e;
		e.scene = a_handle;
		e.event = Event::kCue;
		e.sceneId = GetSingleton().GetSceneId(a_handle);
		e.node = std::string(a_node);
		e.cue = std::string(a_cue);
		e.trackPosition = std::string(a_trackPosition);
		e.time = a_time;
		e.detectedNs = a_detectedNs;
		e.nativeOnly = a_nativeOnly;
		if (!a_role.empty()) {
			e.role = std::string(a_role);
			e.actor = GetSingleton().ResolveRoleActor(a_handle, a_role);
		}
		SceneEventRelay::GetSingleton().Dispatch(e);
	}

	void SceneRuntime::DispatchLifecycleCues(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		SlotView view;
		if (!GetSingleton().SnapshotSlot(a_handle, view)) {
			return;
		}
		const auto def = view.definition;
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;
		}
		const auto wantPos = a_enter ? Registry::CuePos::kEnter : Registry::CuePos::kExit;
		for (const auto& cue : node->cues) {
			if (cue.pos == wantPos) {
				DispatchCue(a_handle, a_node, cue.id, cue.role, a_enter ? "enter" : "exit", -1.0f, 0, cue.nativeOnly);
			}
		}
	}

	void SceneRuntime::PlaySound(std::int32_t a_handle, std::string_view a_spec, std::string_view a_role,
		Registry::SoundEmitter a_emitter)
	{
		RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_role);

		Audio::SoundPlayback::Play(actor, static_cast<std::uint32_t>(a_handle), a_spec, a_emitter,
			std::format("scene {:#010x}", a_handle));
	}

	void SceneRuntime::DispatchLifecycleSounds(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		SlotView view;
		if (!GetSingleton().SnapshotSlot(a_handle, view)) {
			return;
		}
		const auto def = view.definition;
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;
		}
		const auto wantPos = a_enter ? Registry::SoundPos::kEnter : Registry::SoundPos::kExit;
		for (const auto& snd : node->sounds) {
			if (snd.pos == wantPos) {
				PlaySound(a_handle, snd.spec, snd.role, snd.emitter);
			}
		}
	}

	void SceneRuntime::RunCamera(std::int32_t a_handle, Registry::CameraState a_state, bool a_hasPlayer, float a_distance)
	{
		const auto stateName = Registry::CameraStateName(a_state);
		// NPC-only scenes must not seize the player's camera.
		if (!a_hasPlayer) {
			REX::DEBUG("[Scene] scene {:#010x} camera '{}' — no player participant, no-op", a_handle, stateName);
			return;
		}
		if (a_state == Registry::CameraState::kThirdPersonHold) {
			REX::DEBUG("[Scene] scene {:#010x} camera '{}' — third-person hold engaged", a_handle, stateName);
			GetSingleton().RecordMechanism(a_handle, Mechanism::kCamera);
			// After the lock, so the seed lands in the live third-person state. 0 = max zoom-out,
			// < 0 = don't seed. Not ledger-tracked; scene end restores the POV.
			Camera::CameraService::GetSingleton().SeedThirdPersonZoom(a_distance);
		} else if (a_state == Registry::CameraState::kFreeFly) {
			REX::DEBUG("[Scene] scene {:#010x} camera '{}' — free-fly engaged (native, ToggleFreeCameraMode)", a_handle, stateName);
			GetSingleton().RecordCameraState(a_handle, Camera::CameraMode::kFreeFly);
		} else if (a_state == Registry::CameraState::kSceneOrbit) {
			if (GetSingleton().RecordCameraState(a_handle, Camera::CameraMode::kSceneOrbit)) {
				REX::DEBUG("[Scene] scene {:#010x} camera '{}' — scene orbit engaged (mouse-steered)", a_handle, stateName);
			} else {
				REX::DEBUG("[Scene] scene {:#010x} camera '{}' — scene orbit skipped aboard a ship in space", a_handle, stateName);
			}
		} else if (a_state == Registry::CameraState::kVanityOrbit) {
			REX::DEBUG("[Scene] scene {:#010x} camera '{}' — vanity orbit engaged", a_handle, stateName);
			GetSingleton().RecordCameraState(a_handle, Camera::CameraMode::kVanityOrbit);
		} else {
			REX::DEBUG("[Scene] scene {:#010x} camera 'none' — vanilla camera untouched", a_handle);
		}
	}

	void SceneRuntime::DispatchLifecycleCamera(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		SlotView view;
		if (!GetSingleton().SnapshotSlot(a_handle, view)) {
			return;
		}
		// A per-start camera override owns the camera; node cameras would stomp it on NODE_ENTER.
		if (view.cameraOverridden) {
			REX::DEBUG("[Scene] scene {:#010x} node '{}' authored cameras suppressed — per-start camera override active", a_handle, a_node);
			return;
		}
		const auto def = view.definition;
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(view.participants.begin(), view.participants.end(), static_cast<RE::Actor*>(player)) != view.participants.end();

		const auto wantPos = a_enter ? Registry::CameraPos::kEnter : Registry::CameraPos::kExit;
		for (const auto& cam : node->cameras) {
			if (cam.pos == wantPos) {
				RunCamera(a_handle, cam.state, hasPlayer, a_enter ? cam.distance : -1.0f);
			}
		}
	}

	void SceneRuntime::DispatchAction(std::int32_t a_handle, std::string_view a_node, std::string_view a_type,
		std::string_view a_role, std::string_view a_trackPosition)
	{
		REX::DEBUG("[Scene] scene {:#010x} ACTION '{}' node='{}' role='{}' trackPosition='{}'",
			a_handle, a_type, a_node, a_role, a_trackPosition);
		SceneEvent e;
		e.scene = a_handle;
		e.event = Event::kAction;
		e.sceneId = GetSingleton().GetSceneId(a_handle);
		e.node = std::string(a_node);
		e.actionType = std::string(a_type);
		e.role = std::string(a_role);
		e.trackPosition = std::string(a_trackPosition);
		if (!a_role.empty()) {
			e.actor = GetSingleton().ResolveRoleActor(a_handle, a_role);
		}
		SceneEventRelay::GetSingleton().Dispatch(e);
	}

	RE::Actor* SceneRuntime::ResolveRoleActor(std::int32_t a_handle, std::string_view a_role)
	{
		SlotView view;
		if (!SnapshotSlot(a_handle, view)) {
			return nullptr;
		}
		const auto& participants = view.participants;
		if (participants.empty()) {
			return nullptr;
		}
		if (a_role.empty()) {
			return participants.front();
		}
		// roles[i] <-> participants[i].
		const auto def = view.definition;
		if (!def) {
			return nullptr;
		}
		const auto want = Util::ToLower(std::string(a_role));
		for (std::size_t i = 0; i < def->roles.size(); i++) {
			if (Util::ToLower(def->roles[i].name) == want) {
				return i < participants.size() ? participants[i] : nullptr;
			}
		}
		return nullptr;
	}

	void SceneRuntime::DispatchLifecycleActions(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		SlotView view;
		if (!GetSingleton().SnapshotSlot(a_handle, view)) {
			return;
		}
		const auto def = view.definition;
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(view.participants.begin(), view.participants.end(), static_cast<RE::Actor*>(player)) != view.participants.end();

		const auto wantPos = a_enter ? Registry::ActionPos::kEnter : Registry::ActionPos::kExit;
		for (const auto& act : node->actions) {
			if (act.pos == wantPos) {
				RunAction(a_handle, a_node, act, a_enter ? "enter" : "exit", hasPlayer);
			}
		}
	}

	void SceneRuntime::RunAction(std::int32_t a_handle, std::string_view a_node, const Registry::ActionEntry& a_action,
		std::string_view a_trackPosition, bool a_hasPlayer)
	{
		if (a_action.kind == Registry::ActionKind::kControlLock) {
			if (a_hasPlayer) {
				REX::DEBUG("[Scene] scene {:#010x} action osf.control.lock engaged player input lock (role '{}')", a_handle, a_action.role);
				GetSingleton().RecordMechanism(a_handle, Mechanism::kPlayerInputLock);
			} else {
				REX::DEBUG("[Scene] scene {:#010x} osf.control.lock — no player participant, no-op", a_handle);
			}
		} else if (a_action.kind == Registry::ActionKind::kControlRelease) {
			REX::DEBUG("[Scene] scene {:#010x} action osf.control.release", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kPlayerInputLock);
		} else if (a_action.kind == Registry::ActionKind::kFadeOut) {
			if (!a_hasPlayer) {
				REX::DEBUG("[Scene] scene {:#010x} osf.fade.out — no player participant, no-op", a_handle);
			} else {
				const float dur = a_action.duration > 0.0f ? a_action.duration : 0.5f;
				const bool posted = UI::FadeService::GetSingleton().FadeToBlack(dur, /*holdMaxSecs*/ 10.0f);
				REX::DEBUG("[Scene] scene {:#010x} osf.fade.out ({:.2f}s ramp, hold={}) — {}",
					a_handle, dur, a_action.hold, posted ? "posted" : "unavailable");
				// hold:true opts out of the cleanup fade-in; the bounded Tick cap still un-fades.
				if (posted && !a_action.hold) {
					GetSingleton().RecordMechanism(a_handle, Mechanism::kFade);
				}
			}
		} else if (a_action.kind == Registry::ActionKind::kFadeIn) {
			// Default ramp; `duration` is honoured on fade.out only.
			REX::DEBUG("[Scene] scene {:#010x} osf.fade.in", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kFade);
		} else if (a_action.kind == Registry::ActionKind::kEquipmentHide) {
			if (RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_action.role)) {
				GetSingleton().ApplyHiddenEquipment(a_handle, actor, 0xFFFFFFFFu, false);
			} else {
				REX::DEBUG("[Scene] scene {:#010x} osf.equipment.hide — role '{}' resolved no actor, skipped",
					a_handle, a_action.role);
			}
		} else if (a_action.kind == Registry::ActionKind::kEquipmentRestore) {
			// TODO: per-role restore; this restores the whole scene's hidden apparel.
			REX::DEBUG("[Scene] scene {:#010x} osf.equipment.restore", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kEquipment);
		} else if (a_action.kind == Registry::ActionKind::kEquipmentEquip) {
			// Item ref is "<plugin>|0xLOCAL"; the load-order byte is ignored.
			REX::DEBUG("[Scene] scene {:#010x} osf.equipment.equip — attempting (role '{}', item '{}')",
				a_handle, a_action.role, a_action.item);
			if (RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_action.role)) {
				if (auto* object = Util::ResolveBoundObject(a_action.item)) {
					if (!GetSingleton().ApplyEquippedItem(a_handle, actor, object))
						REX::WARN("[Scene] scene {:#010x} authored equip '{}' could not acquire ownership", a_handle, a_action.item);
				} else {
					REX::WARN("[Scene] scene {:#010x} osf.equipment.equip — item '{}' did not resolve to a loaded form, skipped",
						a_handle, a_action.item);
				}
			} else {
				REX::DEBUG("[Scene] scene {:#010x} osf.equipment.equip — role '{}' resolved no actor, skipped",
					a_handle, a_action.role);
			}
		} else if (a_action.kind == Registry::ActionKind::kEquipmentUnequip) {
			REX::DEBUG("[Scene] scene {:#010x} osf.equipment.unequip", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kEquipItem);
		} else if (a_action.kind == Registry::ActionKind::kPropAttach) {
			GetSingleton().AttachSceneProp(a_handle, a_action);
		} else if (a_action.kind == Registry::ActionKind::kPropDestroy) {
			GetSingleton().DestroySceneProp(a_handle, a_action.prop);
		} else if (a_action.kind == Registry::ActionKind::kWeaponSheathe) {
			// Re-draw on cleanup is unconditional, so author this only on an armed role.
			if (RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_action.role)) {
				if (Weapon::WeaponService::GetSingleton().Sheathe(actor)) {
					REX::DEBUG("[Scene] scene {:#010x} osf.weapon.sheathe (role '{}')", a_handle, a_action.role);
					GetSingleton().RecordSheathedWeapon(a_handle, actor);
				} else {
					REX::DEBUG("[Scene] scene {:#010x} osf.weapon.sheathe — unavailable on this build, skipped", a_handle);
				}
			} else {
				REX::DEBUG("[Scene] scene {:#010x} osf.weapon.sheathe — role '{}' resolved no actor, skipped",
					a_handle, a_action.role);
			}
		} else if (a_action.kind == Registry::ActionKind::kWeaponRestore) {
			REX::DEBUG("[Scene] scene {:#010x} osf.weapon.restore", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kWeapon);
		} else if (a_action.kind == Registry::ActionKind::kVoicePlay) {
			if (a_action.set.empty()) {
				REX::WARN("[Scene] scene {:#010x} osf.voice.play — missing 'set' spec, skipped", a_handle);
			} else {
				REX::DEBUG("[Scene] scene {:#010x} osf.voice.play (role '{}', set '{}')", a_handle, a_action.role, a_action.set);
				PlaySound(a_handle, a_action.set, a_action.role, a_action.emitter);
			}
		} else {
			DispatchAction(a_handle, a_node, a_action.type, a_action.role, a_trackPosition);
		}
	}
}
