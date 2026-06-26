#include "Scene/SceneRuntime.h"

#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Equipment/EquipmentService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "UI/FadeService.h"
#include "Util/FormRef.h"
#include "Util/StringUtil.h"
#include "Weapon/WeaponService.h"

#include <algorithm>

// SceneRuntime: event / service dispatch slice
// Fires lifecycle events through the relay and executes a node's track entries: actions (built-in osf.* mechanisms + custom EVENT_ACTION), sounds, and camera holds. 
// The undo recording these engage lives in the ledger slice; the slot table lives in the core.

namespace OSF::Scene
{
	void SceneRuntime::Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_anchor)
	{
		// Undo-ledger replay before SCENE_END: reverse every reversible mechanism this scene
		// engaged (reverse order, once, idempotently) so a listener reacting to scene-end already
		// sees the actors/screen restored. Runs on every termination path (they all Fire
		// SCENE_END), so cleanup never depends on an authored release action.
		if (a_event == Event::kSceneEnd) {
			GetSingleton().ReplayLedger(a_handle);
		}

		// "exit" track entries run before the structural NODE_EXIT; within the tick they run in
		// the fixed cross-lane order action -> (camera) -> sound -> cue.
		if (a_event == Event::kNodeExit) {
			DispatchLifecycleActions(a_handle, a_node, false);
			DispatchLifecycleCamera(a_handle, a_node, false);
			DispatchLifecycleSounds(a_handle, a_node, false);
			DispatchLifecycleCues(a_handle, a_node, false);
		}

		// Logged so the lifecycle is visible even with no registered receiver; the relay
		// delivers the OSFTypes:SceneEvent struct to any that are registered.
		REX::INFO("SceneRuntime: scene {:#010x} event {:#x} node='{}' anchor='{}'", a_handle, a_event, a_node, a_anchor);
		SceneEvent e;
		e.scene = a_handle;
		e.event = a_event;
		e.node = std::string(a_node);
		e.anchor = std::string(a_anchor);
		// actor/role left default.
		SceneEventRelay::GetSingleton().Dispatch(e);

		// "enter" track entries run after the structural NODE_ENTER, in the same cross-lane
		// order action -> (camera) -> sound -> cue.
		if (a_event == Event::kNodeEnter) {
			DispatchLifecycleActions(a_handle, a_node, true);
			DispatchLifecycleCamera(a_handle, a_node, true);
			DispatchLifecycleSounds(a_handle, a_node, true);
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

	void SceneRuntime::PlaySound(std::int32_t a_handle, std::string_view a_spec, std::string_view a_role, float a_volume)
	{
		// Resolve the role actor up front — it positions the sound AND supplies the {gender} substitution.
		RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_role);

		// A '$'-prefixed spec is a sound-pool reference ("$seduce,{gender},moan,loud"): substitute the
		// {gender} placeholder from the actor (empty -> the tag drops out, i.e. any gender), then resolve
		// to a clip NOW (at fire time) so a repeated/per-loop cue re-rolls. A plain path/event spec plays verbatim.
		std::string spec(a_spec);
		if (!spec.empty() && spec.front() == '$') {
			const std::string gender = actor ? Matchmaking::ActorGenderTag(actor) : std::string{};
			for (std::size_t p = 0; (p = spec.find("{gender}", p)) != std::string::npos; p += gender.size()) {
				spec.replace(p, 8, gender);  // 8 == len("{gender}")
			}
			auto resolved = Registry::SoundRegistry::GetSingleton().Resolve(spec);
			if (!resolved) {
				REX::WARN("SceneRuntime: scene {:#010x} sound pool '{}' (role '{}') matched no clip — skipped",
					a_handle, spec, a_role);
				return;
			}
			spec = std::move(*resolved);
		}

		RE::NiPoint3 pos{};
		if (actor) {
			pos = actor->data.location;
		} else if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			pos = player->data.location;  // listener-centered fallback (full volume)
		}
		// Per-actor VOICE slot: a new sound on a channel replaces (cuts) the prior one so cues on the same
		// actor never overlap. Key on the role actor's formID; with no actor, fall back to the scene handle so
		// the scene still has a single channel. The high-bit tag keeps the actor and scene key spaces disjoint.
		const std::uint64_t slot = actor
			? ((1ull << 62) | static_cast<std::uint64_t>(actor->formID))
			: ((2ull << 62) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_handle)));
		REX::INFO("SceneRuntime: scene {:#010x} sound '{}' (role '{}') at ({:.0f},{:.0f},{:.0f}) vol {:.2f} slot {:#x}",
			a_handle, spec, a_role, pos.x, pos.y, pos.z, a_volume, slot);
		Audio::SoundService::GetSingleton().Play(slot, spec, pos, a_volume);
	}

	void SceneRuntime::DispatchLifecycleSounds(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		const std::string id = GetSingleton().GetId(a_handle);  // "" for pack/files -> no sounds
		if (id.empty()) {
			return;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(id);
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;
		}
		const auto wantPos = a_enter ? Registry::SoundPos::kEnter : Registry::SoundPos::kExit;
		for (const auto& snd : node->sounds) {
			if (snd.pos == wantPos) {
				PlaySound(a_handle, snd.spec, snd.role, snd.volume);
			}
		}
	}

	void SceneRuntime::RunCamera(std::int32_t a_handle, std::string_view a_state, bool a_hasPlayer)
	{
		// Camera affects the player's view, so an NPC-only scene must not seize it.
		if (!a_hasPlayer) {
			REX::INFO("SceneRuntime: scene {:#010x} camera '{}' — no player participant, no-op", a_handle, a_state);
			return;
		}
		// thirdperson_hold engages the ref-counted standalone lock (kCamera); freefly / vanity_orbit engage a PlayerCamera state override (kCameraState) that suppresses the hold's bounce while held.
		const std::string state = Util::ToLower(std::string{ a_state });
		if (state == "thirdperson_hold") {
			REX::INFO("SceneRuntime: scene {:#010x} camera '{}' — third-person hold engaged", a_handle, a_state);
			GetSingleton().RecordMechanism(a_handle, Mechanism::kCamera);
		} else if (state == "freefly") {
			REX::INFO("SceneRuntime: scene {:#010x} camera '{}' — free-fly engaged (native, ToggleFreeCameraMode)", a_handle, a_state);
			GetSingleton().RecordCameraState(a_handle, Camera::CameraMode::kFreeFly);
		} else if (state == "scene_orbit") {
			REX::INFO("SceneRuntime: scene {:#010x} camera '{}' — scene orbit engaged (mouse-steered)", a_handle, a_state);
			GetSingleton().RecordCameraState(a_handle, Camera::CameraMode::kSceneOrbit);
		} else if (state == "vanity_orbit") {
			REX::INFO("SceneRuntime: scene {:#010x} camera '{}' — vanity orbit engaged", a_handle, a_state);
			GetSingleton().RecordCameraState(a_handle, Camera::CameraMode::kVanityOrbit);
		} else {
			// SceneRegistry validation restricts authored states to the known set; an unknown one here is a no-op.
			REX::INFO("SceneRuntime: scene {:#010x} camera '{}' — unknown state, no-op", a_handle, a_state);
		}
	}

	void SceneRuntime::DispatchLifecycleCamera(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		SlotView view;
		if (!GetSingleton().SnapshotSlot(a_handle, view)) {
			return;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(view.id);
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;  // pack/files scene or unknown node — no camera entries
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(view.participants.begin(), view.participants.end(), static_cast<RE::Actor*>(player)) != view.participants.end();

		const auto wantPos = a_enter ? Registry::CameraPos::kEnter : Registry::CameraPos::kExit;
		for (const auto& cam : node->cameras) {
			if (cam.pos == wantPos) {
				RunCamera(a_handle, cam.state, hasPlayer);
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
		// Carry the bound actor when the action names a role, so the receiver gets it directly
		// (akEvent.actorRef) without a GetSceneForActor round-trip. Empty role -> None.
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
			return participants.front();  // default target = the first participant
		}
		// The binding is role-declaration order: roles[i] <-> participants[i].
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(view.id);
		if (!def) {
			return nullptr;
		}
		const auto want = Util::ToLower(std::string(a_role));
		for (std::size_t i = 0; i < def->roles.size(); i++) {
			if (Util::ToLower(def->roles[i].name) == want) {
				return i < participants.size() ? participants[i] : nullptr;
			}
		}
		return nullptr;  // unknown role
	}

	void SceneRuntime::DispatchLifecycleActions(std::int32_t a_handle, std::string_view a_node, bool a_enter)
	{
		SlotView view;
		if (!GetSingleton().SnapshotSlot(a_handle, view)) {
			return;
		}
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(view.id);
		const auto* node = def ? def->FindNode(a_node) : nullptr;
		if (!node) {
			return;  // pack/files scene or unknown node — no actions
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
		std::string_view a_anchor, bool a_hasPlayer)
	{
		const auto type = Util::ToLower(a_action.type);
		if (type == "osf.control.lock") {
			if (a_hasPlayer) {
				REX::INFO("SceneRuntime: scene {:#010x} action osf.control.lock (role '{}')", a_handle, a_action.role);
				GetSingleton().RecordMechanism(a_handle, Mechanism::kControlLock);
			} else {
				REX::INFO("SceneRuntime: scene {:#010x} osf.control.lock — no player participant, no-op", a_handle);
			}
		} else if (type == "osf.control.release") {
			REX::INFO("SceneRuntime: scene {:#010x} action osf.control.release", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kControlLock);
		} else if (type == "osf.fade.out") {
			// Screen fade only matters when the player is watching (NPC-only scenes must not
			// black out the player's screen).
			if (!a_hasPlayer) {
				REX::INFO("SceneRuntime: scene {:#010x} osf.fade.out — no player participant, no-op", a_handle);
			} else {
				const float dur = a_action.duration > 0.0f ? a_action.duration : 0.5f;
				const bool posted = UI::FadeService::GetSingleton().FadeToBlack(dur, /*holdMaxSecs*/ 10.0f);
				REX::INFO("SceneRuntime: scene {:#010x} osf.fade.out ({:.2f}s ramp, hold={}) — {}",
					a_handle, dur, a_action.hold, posted ? "posted" : "unavailable");
				// Reversible by default: record the fade so cleanup fades back in. hold:true opts
				// out (scene intends to end faded; the bounded Tick cap still un-fades for safety).
				if (posted && !a_action.hold) {
					GetSingleton().RecordMechanism(a_handle, Mechanism::kFade);
				}
			}
		} else if (type == "osf.fade.in") {
			// Fade back in + drop the fade debt so the cleanup ledger won't redo it. (Uses the
			// mechanism's default ramp; the authored `duration` is honoured on fade.out only.)
			REX::INFO("SceneRuntime: scene {:#010x} osf.fade.in", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kFade);
		} else if (type == "osf.equipment.hide") {
			// Strip the role's actor's worn apparel; record the snapshot in the ledger so cleanup
			// (or osf.equipment.restore) re-equips it.
			if (RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_action.role)) {
				auto snap = Equipment::EquipmentService::GetSingleton().Hide(actor);
				REX::INFO("SceneRuntime: scene {:#010x} osf.equipment.hide (role '{}') — hid {} item(s)",
					a_handle, a_action.role, snap.stripped.size());
				if (!snap.Empty()) {
					GetSingleton().RecordHiddenEquip(a_handle, actor, std::move(snap));
				}
			} else {
				REX::WARN("SceneRuntime: scene {:#010x} osf.equipment.hide — role '{}' resolved no actor, skipped",
					a_handle, a_action.role);
			}
		} else if (type == "osf.equipment.restore") {
			// Re-equip everything this scene hid + drop the equipment debt so cleanup won't redo
			// it. (Restores the whole scene's hidden apparel; per-role restore isn't done yet.)
			REX::INFO("SceneRuntime: scene {:#010x} osf.equipment.restore", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kEquipment);
		} else if (type == "osf.equipment.equip") {
			// Equip an arbitrary item on the role's actor for the scene; record it so cleanup (or
			// osf.equipment.unequip) takes it back off + removes any copy we added. The form ref is
			// "<plugin>|0xLOCAL" (resolved at fire time; the local id's load-order byte is ignored).
			if (RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_action.role)) {
				if (auto* object = Util::ResolveFormRef<RE::TESBoundObject>(a_action.item)) {
					auto record = Equipment::EquipmentService::GetSingleton().EquipItem(actor, object);
					if (record.object) {
						REX::INFO("SceneRuntime: scene {:#010x} osf.equipment.equip (role '{}', item '{}')",
							a_handle, a_action.role, a_action.item);
						GetSingleton().RecordEquippedItem(a_handle, actor, std::move(record));
					} else {
						REX::INFO("SceneRuntime: scene {:#010x} osf.equipment.equip — unavailable on this build, skipped", a_handle);
					}
				} else {
					REX::WARN("SceneRuntime: scene {:#010x} osf.equipment.equip — item '{}' did not resolve to a loaded form, skipped",
						a_handle, a_action.item);
				}
			} else {
				REX::WARN("SceneRuntime: scene {:#010x} osf.equipment.equip — role '{}' resolved no actor, skipped",
					a_handle, a_action.role);
			}
		} else if (type == "osf.equipment.unequip") {
			// Take back off everything this scene equipped + drop the debt so cleanup won't redo it.
			REX::INFO("SceneRuntime: scene {:#010x} osf.equipment.unequip", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kEquipItem);
		} else if (type == "osf.weapon.sheathe") {
			// Holster the role's actor's weapon; record it so cleanup (or osf.weapon.restore)
			// re-draws it. Symmetric pair (see WeaponService): re-draw on cleanup is unconditional,
			// so author this only on a role that's armed.
			if (RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_action.role)) {
				if (Weapon::WeaponService::GetSingleton().Sheathe(actor)) {
					REX::INFO("SceneRuntime: scene {:#010x} osf.weapon.sheathe (role '{}')", a_handle, a_action.role);
					GetSingleton().RecordSheathedWeapon(a_handle, actor);
				} else {
					REX::INFO("SceneRuntime: scene {:#010x} osf.weapon.sheathe — unavailable on this build, skipped", a_handle);
				}
			} else {
				REX::WARN("SceneRuntime: scene {:#010x} osf.weapon.sheathe — role '{}' resolved no actor, skipped",
					a_handle, a_action.role);
			}
		} else if (type == "osf.weapon.restore") {
			// Re-draw everything this scene sheathed + drop the weapon debt so cleanup won't redo it.
			REX::INFO("SceneRuntime: scene {:#010x} osf.weapon.restore", a_handle);
			GetSingleton().UndoMechanism(a_handle, Mechanism::kWeapon);
		} else if (type == "osf.voice.play") {
			// Fire-and-forget voice line: play the `set` spec at the role's actor. Not reversible
			// (a one-shot sound has nothing to undo), so no ledger entry.
			if (a_action.set.empty()) {
				REX::WARN("SceneRuntime: scene {:#010x} osf.voice.play — missing 'set' spec, skipped", a_handle);
			} else {
				REX::INFO("SceneRuntime: scene {:#010x} osf.voice.play (role '{}', set '{}')", a_handle, a_action.role, a_action.set);
				PlaySound(a_handle, a_action.set, a_action.role, 1.0f);
			}
		} else if (type.rfind("osf.", 0) == 0) {
			// recognised built-in mechanism we don't execute yet.
			REX::INFO("SceneRuntime: scene {:#010x} action '{}' (role '{}') — recognized, not yet executed",
				a_handle, a_action.type, a_action.role);
		} else {
			// custom namespaced action -> EVENT_ACTION (best-effort notification).
			DispatchAction(a_handle, a_node, a_action.type, a_action.role, a_anchor);
		}
	}
}
