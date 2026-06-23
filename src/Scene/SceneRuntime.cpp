#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Equipment/EquipmentService.h"
#include "Matchmaking/Matchmaker.h"
#include "Player/PlayerControlService.h"
#include "Registry/PackRegistry.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "UI/FadeService.h"
#include "Util/StringUtil.h"
#include "Weapon/WeaponService.h"

#include <algorithm>
#include <charconv>

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
		// delivers the OSFEvent:SceneEvent struct to any that are registered.
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
		RE::NiPoint3 pos{};
		if (RE::Actor* actor = GetSingleton().ResolveRoleActor(a_handle, a_role)) {
			pos = actor->data.location;
		} else if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			pos = player->data.location;  // listener-centered fallback (full volume)
		}
		REX::INFO("SceneRuntime: scene {:#010x} sound '{}' (role '{}') at ({:.0f},{:.0f},{:.0f}) vol {:.2f}",
			a_handle, a_spec, a_role, pos.x, pos.y, pos.z, a_volume);
		Audio::SoundService::GetSingleton().Play(std::string(a_spec), pos, a_volume);
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
		// The one supported state. The hold is ledger-tracked (kCamera) and auto-restored on
		// cleanup; RecordMechanism engages the ref-counted standalone lock.
		REX::INFO("SceneRuntime: scene {:#010x} camera '{}' — third-person hold engaged", a_handle, a_state);
		GetSingleton().RecordMechanism(a_handle, Mechanism::kCamera);
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

	void SceneRuntime::RecordMechanism(std::int32_t a_handle, Mechanism a_mech)
	{
		bool engageLock = false;
		bool engageCamera = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			if (std::find(s->ledger.begin(), s->ledger.end(), a_mech) != s->ledger.end()) {
				return;  // already recorded — idempotent per scene+mechanism
			}
			s->ledger.push_back(a_mech);
			if (a_mech == Mechanism::kControlLock) {
				engageLock = (++_controlLockCount == 1);  // first global holder engages the lock
			} else if (a_mech == Mechanism::kCamera) {
				engageCamera = true;  // the camera lock is ref-counted internally (composes w/ control lock)
			}
			// kFade: the visible fade-out was posted by RunAction; recording just notes the debt.
		}
		if (engageLock) {
			Player::PlayerControlService::GetSingleton().SetStandaloneLock(true);
			Camera::CameraService::GetSingleton().SetStandaloneLock(true);
		}
		if (engageCamera) {
			Camera::CameraService::GetSingleton().SetStandaloneLock(true);
		}
	}

	void SceneRuntime::UndoMechanism(std::int32_t a_handle, Mechanism a_mech)
	{
		bool disengageLock = false;
		std::int32_t remaining = 0;
		std::vector<std::pair<RE::Actor*, Equipment::Snapshot>> equip;  // moved out for kEquipment
		std::vector<RE::Actor*> weapon;                                 // moved out for kWeapon
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			const auto it = std::find(s->ledger.begin(), s->ledger.end(), a_mech);
			if (it == s->ledger.end()) {
				return;  // not held — idempotent (already reversed, or never engaged)
			}
			s->ledger.erase(it);
			if (a_mech == Mechanism::kControlLock) {
				disengageLock = (--_controlLockCount <= 0);  // last holder releases the actual lock
				if (_controlLockCount < 0) {
					_controlLockCount = 0;
				}
				remaining = _controlLockCount;
			} else if (a_mech == Mechanism::kEquipment) {
				equip.swap(s->hiddenEquip);  // take this scene's hidden apparel out for restore
			} else if (a_mech == Mechanism::kWeapon) {
				weapon.swap(s->sheathedWeapon);  // take this scene's sheathed actors out for re-draw
			}
		}
		// Apply the reversal OUTSIDE the lock (services enter the VM / post UI messages / touch
		// the inventory lock).
		switch (a_mech) {
		case Mechanism::kControlLock:
			if (disengageLock) {
				REX::INFO("SceneRuntime: scene {:#010x} control lock released — player unlocked", a_handle);
				Player::PlayerControlService::GetSingleton().SetStandaloneLock(false);
				Camera::CameraService::GetSingleton().SetStandaloneLock(false);
			} else {
				REX::INFO("SceneRuntime: scene {:#010x} control lock released — {} scene(s) still hold it", a_handle, remaining);
			}
			break;
		case Mechanism::kFade:
			REX::INFO("SceneRuntime: scene {:#010x} fade undo — fading back in", a_handle);
			UI::FadeService::GetSingleton().FadeFromBlack(0.5f);
			break;
		case Mechanism::kEquipment:
			REX::INFO("SceneRuntime: scene {:#010x} equipment undo — restoring {} actor(s)", a_handle, equip.size());
			for (auto& [actor, snap] : equip) {
				Equipment::EquipmentService::GetSingleton().Restore(actor, snap);
			}
			break;
		case Mechanism::kCamera:
			REX::INFO("SceneRuntime: scene {:#010x} camera undo — releasing the camera hold", a_handle);
			Camera::CameraService::GetSingleton().SetStandaloneLock(false);
			break;
		case Mechanism::kWeapon:
			REX::INFO("SceneRuntime: scene {:#010x} weapon undo — re-drawing {} actor(s)", a_handle, weapon.size());
			for (auto* actor : weapon) {
				Weapon::WeaponService::GetSingleton().Draw(actor);
			}
			break;
		}
	}

	void SceneRuntime::ReplayLedger(std::int32_t a_handle)
	{
		// Reverse order, once, idempotently — the cleanup guarantee. Snapshot the ledger reversed
		// (UndoMechanism erases each entry as it reverses it; a later call finds it empty and
		// no-ops). The single Fire(SCENE_END) point calls this on every termination.
		std::vector<Mechanism> reversed;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s || s->ledger.empty()) {
				return;
			}
			reversed.assign(s->ledger.rbegin(), s->ledger.rend());
		}
		for (auto m : reversed) {
			UndoMechanism(a_handle, m);
		}
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

	void SceneRuntime::RecordHiddenEquip(std::int32_t a_handle, RE::Actor* a_actor, Equipment::Snapshot a_snapshot)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_handle);
		if (!s) {
			return;
		}
		s->hiddenEquip.emplace_back(a_actor, std::move(a_snapshot));
		if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kEquipment) == s->ledger.end()) {
			s->ledger.push_back(Mechanism::kEquipment);  // one ledger entry; the snapshots accumulate
		}
	}

	void SceneRuntime::RecordSheathedWeapon(std::int32_t a_handle, RE::Actor* a_actor)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_handle);
		if (!s) {
			return;
		}
		if (std::find(s->sheathedWeapon.begin(), s->sheathedWeapon.end(), a_actor) != s->sheathedWeapon.end()) {
			return;  // already recorded for this scene
		}
		s->sheathedWeapon.push_back(a_actor);
		if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kWeapon) == s->ledger.end()) {
			s->ledger.push_back(Mechanism::kWeapon);  // one ledger entry; the actors accumulate
		}
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
		if (!node || node->anim.empty()) {
			return;  // not def-backed, or the node has no anim
		}
		auto plan = Registry::PackRegistry::GetSingleton().BuildScenePlan(node->anim, a_participants.size());
		if (!plan) {
			REX::WARN("SceneRuntime: node '{}' anim '{}' not playable for {} participant(s)",
				a_nodeId, node->anim, a_participants.size());
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

	std::int32_t SceneRuntime::MintSlot(Kind a_kind, std::string_view a_id, std::string_view a_node, std::int32_t a_stage, const std::vector<RE::Actor*>& a_participants)
	{
		std::lock_guard l{ _lock };

		// Reject a null actor or the same actor passed twice in one call — both would break the
		// one-actor-one-scene model the rest of the runtime relies on.
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

		// Actor exclusivity: an actor is in at most one live scene, so a start on a busy actor
		// fails (0) — the caller must Stop it first. Without this, two handles could alias one
		// actor, making GetSceneForActor multi-valued and the auto-end resolver ambiguous.
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

		const std::int32_t handle = MintSlot(Kind::kDef, a_id, a_entryNode, 0, a_participants);
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

		StopGraph(view.participants);
		Fire(a_scene, Event::kNodeExit, view.node, "exit");
		Fire(a_scene, Event::kSceneEnd, view.node, "");
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

	std::int32_t SceneRuntime::StartFromPack(std::string_view a_packId, const std::vector<RE::Actor*>& a_participants, std::int32_t a_startStage,
		const AnchorOverride& a_anchor)
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
		// StartSceneAt: world-anchor the pack scene at the explicit anchor (a pack scene is
		// single-path, so there are no node transitions — stamping the plan once is enough).
		if (a_anchor.set) {
			plan->anchorExplicit = true;
			plan->anchorPos = a_anchor.pos;
			plan->anchorHeading = a_anchor.heading;
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
		plan.stages.push_back({ a_files, {}, 0.0f });  // one stage, holds (loop mode hold)
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
		// The node's DEFAULT advance edge (never inferred).
		return TakeEdge(a_scene, [](const Registry::SceneNode& a_node) -> const Registry::SceneEdge* {
			for (const auto& e : a_node.edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && e.isDefault) {
					return &e;
				}
			}
			return nullptr;
		});
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
						// completion (ends when the clip / loop target finishes). A timer
						// can't land here (we only arm the stage timer when a `timer` edge exists).
						end = true;
					}
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
			if (e.when == Registry::EdgeWhen::kAdvance) {
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
