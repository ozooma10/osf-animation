#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Equipment/EquipmentService.h"
#include "Input/InputService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/ContentRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/AlignmentStore.h"
#include "UI/FadeService.h"
#include "UI/HudMessage.h"
#include "Util/FormRef.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>

namespace OSF::Scene
{
	namespace
	{
		// Opaque to the playback layer; ascending lane = same-tick order action -> camera -> sound -> cue.
		constexpr std::uint8_t kLaneAction = 0;
		constexpr std::uint8_t kLaneCamera = 1;
		constexpr std::uint8_t kLaneSound = 2;
		constexpr std::uint8_t kLaneCue = 3;

		// `atFrame` entries use absolute `seconds` so they land on the authored frame at any clip length.
		// Enter/exit entries are fired by lifecycle dispatch, not here.
		template <class Entry, class Pos, class TokenFn>
		void ScheduleMarks(std::vector<Animation::TimedMark>& a_marks, const std::vector<Entry>& a_entries,
			Pos a_fractionPos, Pos a_endPos, std::uint8_t a_lane, TokenFn a_token)
		{
			for (std::size_t i = 0; i < a_entries.size(); i++) {
				const auto& e = a_entries[i];
				if (e.pos == a_fractionPos) {
					a_marks.push_back({ .fraction = e.fraction, .seconds = Registry::TrackSeconds(e),
						.everyLoop = e.everyLoop, .atEnd = false, .lane = a_lane, .token = a_token(e, i) });
				} else if (e.pos == a_endPos) {
					a_marks.push_back({ .atEnd = true, .lane = a_lane, .token = a_token(e, i) });
				}
			}
		}

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

		// Node policy bounds only the terminal stage; earlier stages keep their authored timers.
		// timerSec arms only when the node has a `timer` edge.
		void ApplyNodePolicy(Animation::PlaybackPlan& a_plan, const Registry::SceneNode& a_node)
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

		void ApplyNodeMarks(Animation::PlaybackPlan& a_plan, const Registry::SceneNode& a_node)
		{
			if (a_plan.stages.empty()) {
				return;
			}
			auto& marks = a_plan.stages.back().marks;
			// Cue token = cue id; other lanes' token = entry index.
			ScheduleMarks(marks, a_node.cues, Registry::CuePos::kFraction, Registry::CuePos::kEnd, kLaneCue, [](const auto& a_cue, std::size_t) { return a_cue.id; });
			ScheduleMarks(marks, a_node.actions, Registry::ActionPos::kFraction, Registry::ActionPos::kEnd, kLaneAction, [](const auto&, std::size_t a_i) { return std::to_string(a_i); });
			ScheduleMarks(marks, a_node.sounds, Registry::SoundPos::kFraction, Registry::SoundPos::kEnd, kLaneSound, [](const auto&, std::size_t a_i) { return std::to_string(a_i); });
			ScheduleMarks(marks, a_node.cameras, Registry::CameraPos::kFraction, Registry::CameraPos::kEnd, kLaneCamera, [](const auto&, std::size_t a_i) { return std::to_string(a_i); });
		}

		// Highest priority wins; ties keep declaration order.
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

	void SceneRuntime::OnTimedMarks(Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_participants,
		const std::vector<Animation::FiredMark>& a_marks)
	{
		(void)a_participants;
		if (a_marks.empty()) {
			return;
		}

		std::vector<Animation::FiredMark> marks = a_marks;
		std::stable_sort(marks.begin(), marks.end(),
			[](const Animation::FiredMark& a_lhs, const Animation::FiredMark& a_rhs) { return a_lhs.lane < a_rhs.lane; });

		std::int32_t handle = 0;
		std::string oldNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		Registry::SceneRef definition;
		bool cameraOverridden = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = FindSlotForPlayback(a_playbackId, &handle);
			if (!s) {
				return;  // standalone playback, or stale task from a replaced node
			}
			oldNode = s->node;
			sceneId = s->id;
			participants = s->participants;
			definition = s->definition;
			cameraOverridden = s->cameraOverridden;
		}

		const auto def = definition;
		const auto* node = def ? def->FindNode(oldNode) : nullptr;

		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(participants.begin(), participants.end(), static_cast<RE::Actor*>(player)) != participants.end();

		// Cues are deferred until the tick's other entries have run.
		std::vector<std::string> cueIds;
		for (const auto& m : marks) {
			if (m.lane == kLaneCue) {
				cueIds.push_back(m.token);
			} else if (m.lane == kLaneAction && node) {
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->actions.size(), idx)) {
					RunAction(handle, oldNode, node->actions[idx], "", hasPlayer);
				}
			} else if (m.lane == kLaneSound && node) {
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->sounds.size(), idx)) {
					const auto& snd = node->sounds[idx];
					PlaySound(handle, snd.spec, snd.role, snd.emitter);
				}
			} else if (m.lane == kLaneCamera && node && !cameraOverridden) {
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->cameras.size(), idx)) {
					RunCamera(handle, node->cameras[idx].state, hasPlayer, node->cameras[idx].distance);
				}
			}
		}

		// TODO: thread the fired fraction back from the Scene; time is sent as -1.
		for (const auto& id : cueIds) {
			DispatchCue(handle, oldNode, id, "", -1.0f);
		}

		// First fired cue with a matching trigger edge wins. Re-resolve: an action above may have
		// ended the scene or changed the node.
		std::string newNode;
		bool triggered = false;
		bool end = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(handle);
			if (s && s->playbackId == a_playbackId && s->node == oldNode && !s->transitioning) {
				const auto d = s->definition;
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
							if (edge->to == "$end" || s->singleStage) {
								end = true;
							} else {
								s->node = edge->to;
								newNode = edge->to;
							}
							s->transitioning = true;
							break;
						}
					}
				}
			}
		}
		if (triggered) {
			REX::DEBUG("[Scene] scene {:#010x} cue-trigger node '{}' -> {}", handle, oldNode, end ? "$end" : newNode);
			ApplyTransition(handle, oldNode, newNode, end, sceneId, participants);
		}
	}

	Animation::PlaybackId SceneRuntime::PlayNodeAnim(const std::vector<RE::Actor*>& a_participants,
		std::string_view a_sceneId, std::string_view a_nodeId, Animation::PlaybackId a_expectedPlayback)
	{
		if (a_participants.empty()) {
			return 0;
		}
		// Per-start anchor/placement/loopScale are read fresh from the slot for every node entry.
		float loopScale = 1.0f;
		std::optional<Animation::WorldPlacementMode> placementOverride;
		auto def = Registry::ContentRegistry::GetSingleton().Find(a_sceneId);
		AnchorOverride anchor;
		AnchorOverride frozenAnchor;
		ActorBaseline baseline;
		SceneSpeed speed;
		std::vector<Alignment::Delta> alignment;
		{
			std::lock_guard l{ GetSingleton()._lock };
			std::int32_t tok = 0;
			if (Slot* s = GetSingleton().FindSlotForActor(a_participants.front(), &tok); s) {
				if (s->definition) {
					def = s->definition;  // the version pinned at Start
				}
				anchor = s->anchor;
				frozenAnchor = s->frozenAnchor;
				baseline = s->baseline;
				speed = s->speed;
				loopScale = s->loopScale;
				placementOverride = s->worldPlacement;
				if (def && def->roles.size() == a_participants.size()) {
					std::vector<std::string> roles;
					for (const auto& role : def->roles) roles.push_back(role.name);
					alignment = s->align.Enter(std::string(a_nodeId), Alignment::Read(s->id, a_nodeId, roles));
				}
			}
		}
		const auto* node = def ? def->FindNode(a_nodeId) : nullptr;
		if (!node || (node->use.empty() && node->stages.empty())) {
			return 0;
		}
		auto plan = Registry::ContentRegistry::GetSingleton().BuildNodePlan(def, *node, a_participants.size());
		if (!plan) {
			REX::WARN("[Scene] node '{}' not playable for {} participant(s)", a_nodeId, a_participants.size());
			return 0;
		}
		ApplyScenePlacement(*plan, anchor, frozenAnchor, baseline, a_participants);
		speed.Apply(*plan);
		if (placementOverride.has_value()) {
			plan->SetWorldPlacementMode(*placementOverride);
		}
		ApplyNodePolicy(*plan, *node);
		// Every loop-driven stage, floored at 1; loops<=0 (hold/timer-only) must stay infinite.
		if (loopScale != 1.0f) {
			for (auto& st : plan->stages) {
				if (st.loops > 0) {
					st.loops = std::max<std::int32_t>(1, static_cast<std::int32_t>(st.loops * loopScale + 0.5f));
				}
			}
		}
		ApplyNodeMarks(*plan, *node);
		Animation::PlaybackId playbackId = 0;
		if (!Animation::GraphManager::GetSingleton().PlaySynchronized(a_participants, *plan, 0, &playbackId,
			a_expectedPlayback, GetSingleton()._playbackSinkId)) {
			return 0;
		}
		if (const auto playback = Animation::GraphManager::GetSingleton().GetScenePlayback(a_participants.front(), playbackId)) {
			std::lock_guard l{ GetSingleton()._lock };
			if (Slot* s = GetSingleton().FindSlotForActor(a_participants.front(), nullptr); s && !s->frozenAnchor.set) {
				s->frozenAnchor = { true, playback->anchorPos, playback->anchorHeading };
			}
		}
		for (std::size_t i = 0; i < alignment.size(); ++i)
			Animation::GraphManager::GetSingleton().SetAlignmentDelta(a_participants[i], alignment[i], playbackId);
		return playbackId;
	}

	void SceneRuntime::StopGraph(const std::vector<RE::Actor*>& a_participants,
		Animation::PlaybackId a_expectedPlayback)
	{
		if (!a_participants.empty() && a_participants.front()) {
			Animation::GraphManager::GetSingleton().StopScene(a_participants.front(), a_expectedPlayback);
		}
	}

	void SceneRuntime::EngageDefaultPlayerInputLock(std::int32_t a_handle, bool a_playerInputLock,
		const std::vector<RE::Actor*>& a_participants)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(a_participants.begin(), a_participants.end(), static_cast<RE::Actor*>(player)) != a_participants.end();
		if (!hasPlayer) {
			return;
		}
		if (!a_playerInputLock) {
			REX::DEBUG("[Scene] scene {:#010x} default player input lock skipped — opted out (playerInputLock:false)", a_handle);
			return;
		}
		REX::DEBUG("[Scene] scene {:#010x} default player input lock engaged — player is a participant", a_handle);
		RecordMechanism(a_handle, Mechanism::kPlayerInputLock);
	}

	void SceneRuntime::EngageDefaultCamera(std::int32_t a_handle, std::string_view a_entryNode,
		bool a_playerInputLock, std::string_view a_cameraOverride, const std::vector<RE::Actor*>& a_participants)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(a_participants.begin(), a_participants.end(), static_cast<RE::Actor*>(player)) != a_participants.end();
		if (!hasPlayer) {
			return;
		}
		// Same gate as the input lock: opting out keeps the free camera too.
		if (!a_playerInputLock) {
			REX::DEBUG("[Scene] scene {:#010x} default camera skipped — player input lock disabled", a_handle);
			return;
		}

		if (!a_cameraOverride.empty()) {
			REX::DEBUG("[Scene] scene {:#010x} camera overridden at start -> '{}'", a_handle, a_cameraOverride);
			const auto state = Registry::ParseCameraState(a_cameraOverride);
			if (!state) {
				REX::DEBUG("[Scene] scene {:#010x} camera override '{}' is unknown — no-op", a_handle, a_cameraOverride);
				return;
			}
			RunCamera(a_handle, *state, hasPlayer, 0.0f);
			return;
		}
		// An authored entry-node enter camera wins; DispatchLifecycleCamera engages it.
		SlotView view;
		if (SnapshotSlot(a_handle, view); const auto def = view.definition) {
			if (const auto* node = def->FindNode(a_entryNode)) {
				for (const auto& cam : node->cameras) {
					if (cam.pos == Registry::CameraPos::kEnter) {
						return;
					}
				}
			}
		}
		REX::DEBUG("[Scene] scene {:#010x} default camera engaged — no authored camera, framing cast with native-assisted orbit", a_handle);
		RunCamera(a_handle, Registry::CameraState::kSceneOrbit, hasPlayer, 0.0f);
	}

	void SceneRuntime::ClearHeldItems(std::int32_t a_handle, bool a_clearHeldItems,
		const std::vector<RE::Actor*>& a_participants,
		const std::vector<std::vector<Equipment::Gear::Pick>>& a_gearPicks)
	{
		if (!a_clearHeldItems) {
			REX::DEBUG("[Scene] scene {:#010x} held-item clear skipped — opted out (clearHeldItems:false)", a_handle);
			return;
		}
		// Held props stay welded to the hand through an animation; independent of hideApparel.
		std::size_t cleared = 0;
		for (std::size_t i = 0; i < a_participants.size(); i++) {
			RE::Actor* actor = a_participants[i];
			if (!actor) {
				continue;
			}
			auto keep = OwnedEquipment(a_handle, actor);
			if (i < a_gearPicks.size()) {
				for (const auto& pick : a_gearPicks[i]) {
					if (pick.worn && pick.object) {
						keep.push_back(pick.object);
					}
				}
			}
			auto snap = Equipment::EquipmentService::GetSingleton().HideHeld(actor, keep);
			if (!snap.Empty()) {
				cleared += snap.hiddenItems.size();
				RecordHiddenHeldEquip(a_handle, actor, std::move(snap));
			}
		}
		REX::DEBUG("[Scene] scene {:#010x} held-item clear: {} item(s) across {} participant(s)",
			a_handle, cleared, a_participants.size());
	}

	void SceneRuntime::HideDefaultApparel(std::int32_t a_handle, bool a_hideApparel, const std::vector<RE::Actor*>& a_participants,
		const std::vector<std::vector<Equipment::Gear::Pick>>& a_gearPicks)
	{
		if (!a_hideApparel) {
			REX::DEBUG("[Scene] scene {:#010x} default apparel hide skipped — opted out (legacy stripActors:false)", a_handle);
			return;
		}
		for (std::size_t i = 0; i < a_participants.size(); i++) {
			RE::Actor* actor = a_participants[i];
			if (!actor) {
				continue;
			}
			auto keep = OwnedEquipment(a_handle, actor);
			if (i < a_gearPicks.size()) {
				for (const auto& pick : a_gearPicks[i]) {
					if (pick.worn && pick.object) {
						keep.push_back(pick.object);
					}
				}
			}
			auto snap = Equipment::EquipmentService::GetSingleton().Hide(actor, 0xFFFFFFFFu, keep);
			if (!snap.Empty()) {
				RecordHiddenEquip(a_handle, actor, std::move(snap));
			}
		}
		REX::DEBUG("[Scene] scene {:#010x} default apparel hide applied to {} participant(s)", a_handle, a_participants.size());
	}

	std::vector<std::vector<Equipment::Gear::Pick>> SceneRuntime::BuildGearPicks(std::int32_t a_handle, const std::vector<RE::Actor*>& a_participants)
	{
		std::vector<std::vector<Equipment::Gear::Pick>> picks(a_participants.size());
		if (!Equipment::Gear::AutoEquip()) {
			return picks;
		}
		SlotView view;
		SnapshotSlot(a_handle, view);
		const auto def = view.definition;
		for (std::size_t i = 0; i < a_participants.size(); i++) {
			RE::Actor* actor = a_participants[i];
			if (!actor) {
				continue;
			}
			// Authored role `equip` claims its gear slot; unregistered items still win since EquipRoleItems runs after.
			std::vector<std::string> taken;
			if (def && i < def->roles.size() && !def->roles[i].equip.Empty()) {
				const std::string  gender = Matchmaking::ActorGenderTag(actor);
				const std::string& ref = def->roles[i].equip.ForGender(gender);
				if (!ref.empty()) {
					if (const auto id = Util::ComposeFormID(ref)) {
						if (auto slot = Equipment::Gear::SlotOfForm(*id); !slot.empty()) {
							taken.push_back(std::move(slot));
						}
					}
				}
			}
			picks[i] = Equipment::Gear::SelectForActor(actor, taken);
		}
		return picks;
	}

	void SceneRuntime::EquipGearItems(std::int32_t a_handle, const std::vector<RE::Actor*>& a_participants,
		const std::vector<std::vector<Equipment::Gear::Pick>>& a_gearPicks)
	{
		auto& svc = Equipment::EquipmentService::GetSingleton();
		for (std::size_t i = 0; i < a_participants.size() && i < a_gearPicks.size(); i++) {
			RE::Actor* actor = a_participants[i];
			if (!actor) {
				continue;
			}
			for (const auto& pick : a_gearPicks[i]) {
				if (!pick.object) {
					continue;
				}
				if (pick.worn) {
					REX::DEBUG("[Scene] scene {:#010x} actor {:08X} gear '{}' (slot '{}') kept on — worn before the scene",
						a_handle, actor->formID, pick.ref, pick.slot);
					continue;
				}
				if (OwnsEquipment(a_handle, actor, pick.object)) continue;
				auto record = svc.EquipItem(actor, pick.object);
				if (record.object) {
					REX::DEBUG("[Scene] scene {:#010x} actor {:08X} gear '{}' (slot '{}') equipped (added={})",
						a_handle, actor->formID, pick.ref, pick.slot, record.addedToInventory);
					RecordEquippedItem(a_handle, actor, std::move(record));
				} else {
					REX::WARN("[Scene] scene {:#010x} actor {:08X} gear '{}' — EquipItem did nothing (feature unavailable on this build?)",
						a_handle, actor->formID, pick.ref);
				}
			}
		}
	}

	void SceneRuntime::EquipRoleItems(std::int32_t a_handle, std::string_view a_defId, const std::vector<RE::Actor*>& a_participants)
	{
		if (a_defId.empty()) {
			return;
		}
		SlotView view;
		SnapshotSlot(a_handle, view);
		const auto def = view.definition;
		if (!def) {
			return;
		}
		auto& svc = Equipment::EquipmentService::GetSingleton();
		for (std::size_t i = 0; i < def->roles.size() && i < a_participants.size(); i++) {
			const auto& role = def->roles[i];
			if (role.equip.Empty()) {
				continue;
			}
			RE::Actor* actor = a_participants[i];
			if (!actor) {
				REX::WARN("[Scene] scene {:#010x} role '{}' has equip but bound no actor — skipped", a_handle, role.name);
				continue;
			}
			const std::string gender = Matchmaking::ActorGenderTag(actor);
			const std::string& ref = role.equip.ForGender(gender);
			if (ref.empty()) {
				REX::DEBUG("[Scene] scene {:#010x} role '{}' (actor {:08X}, gender '{}') has equip but none for that gender — skipped",
					a_handle, role.name, actor->formID, gender.empty() ? "any" : gender);
				continue;
			}
			REX::DEBUG("[Scene] scene {:#010x} role '{}' (actor {:08X}, gender '{}') equipping '{}'",
				a_handle, role.name, actor->formID, gender.empty() ? "any" : gender, ref);
			const auto composed = Util::ComposeFormID(ref);
			if (!composed) {
				REX::ERROR("[Scene] scene {:#010x} role '{}' equip '{}' — plugin not loaded or ref malformed (no FormID composed); is the .esm active in the load order?",
					a_handle, role.name, ref);
				continue;
			}
			auto* anyForm = RE::TESForm::LookupByID(*composed);
			auto* object = (anyForm && Util::IsBoundObjectType(anyForm->GetFormType())) ? static_cast<RE::TESBoundObject*>(anyForm) : nullptr;
			if (!object) {
				if (anyForm) {
					REX::ERROR("[Scene] scene {:#010x} role '{}' equip '{}' resolved to FormID {:#010x} but it is a {} (formType {}), not an equippable bound object — skipped",
						a_handle, role.name, ref, *composed,
						anyForm->GetFormEditorID() ? anyForm->GetFormEditorID() : "?",
						static_cast<std::uint32_t>(anyForm->GetFormType()));
				} else {
					REX::ERROR("[Scene] scene {:#010x} role '{}' equip '{}' composed FormID {:#010x} but no such form is loaded — wrong local id? — skipped",
						a_handle, role.name, ref, *composed);
				}
				continue;
			}
			if (OwnsEquipment(a_handle, actor, object)) continue;
			auto record = svc.EquipItem(actor, object);
			if (record.object) {
				REX::DEBUG("[Scene] scene {:#010x} role '{}' equipped '{}' (added={}, equipped={})",
					a_handle, role.name, ref, record.addedToInventory, record.equipped);
				RecordEquippedItem(a_handle, actor, std::move(record));
			} else {
				REX::WARN("[Scene] scene {:#010x} role '{}' equip '{}' — EquipItem did nothing (feature unavailable on this build?)",
					a_handle, role.name, ref);
			}
		}
	}

	void SceneRuntime::FadeSceneStart(std::int32_t a_handle, bool a_fade, const std::vector<RE::Actor*>& a_participants)
	{
		auto*      player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(a_participants.begin(), a_participants.end(), static_cast<RE::Actor*>(player)) != a_participants.end();
		if (!hasPlayer) {
			return;
		}
		if (!a_fade) {
			REX::DEBUG("[Scene] scene {:#010x} default start fade skipped — opted out (fade:false)", a_handle);
			return;
		}
		const bool posted = UI::FadeService::GetSingleton().FadeToBlack(/*fadeSecs*/ 0.5f, /*holdMaxSecs*/ 1.0f);
		REX::DEBUG("[Scene] scene {:#010x} default start fade — {}", a_handle, posted ? "posted" : "unavailable");
	}

	void SceneRuntime::CompleteStart(std::int32_t a_handle, std::string_view a_defId, std::string_view a_entryNode,
		const std::vector<RE::Actor*>& a_participants, bool a_playerInputLock, bool a_hideApparel,
		bool a_clearHeldItems, bool a_fade, const StartOverrides& a_over)
	{
		const std::string_view cameraOverride = a_over.camera ? std::string_view(*a_over.camera) : std::string_view{};
		EngageDefaultPlayerInputLock(a_handle, a_playerInputLock, a_participants);
		EngageDefaultCamera(a_handle, a_entryNode, a_playerInputLock, cameraOverride, a_participants);
		const auto gearPicks = BuildGearPicks(a_handle, a_participants);
		ClearHeldItems(a_handle, a_clearHeldItems, a_participants, gearPicks);
		HideDefaultApparel(a_handle, a_hideApparel, a_participants, gearPicks);
		EquipGearItems(a_handle, a_participants, gearPicks);
		EquipRoleItems(a_handle, a_defId, a_participants);
		FadeSceneStart(a_handle, a_fade, a_participants);
		EngageDefaultSceneControls(a_handle, a_defId, a_over.sceneControls, a_participants);
		REX::INFO("[Scene] start id='{}' handle={:#010x}", a_defId.empty() ? "<ad-hoc>" : a_defId, a_handle);
		Fire(a_handle, Event::kSceneBegin, a_entryNode, "");
		Fire(a_handle, Event::kNodeEnter, a_entryNode, "enter");
	}

	void SceneRuntime::EngageDefaultSceneControls(std::int32_t a_handle, std::string_view a_defId, std::optional<bool> a_controlOverride, const std::vector<RE::Actor*>& a_participants)
	{
		Registry::SceneControls controls;  // default: enabled, all capabilities, unlocked
		if (!a_defId.empty()) {
			SlotView view;
			if (SnapshotSlot(a_handle, view); const auto def = view.definition) {
				controls = def->sceneControls;
			}
		}
		if (a_controlOverride.has_value()) {
			controls.enabled = *a_controlOverride;
		}
		if (!controls.enabled || controls.capabilities == 0) {
			UndoMechanism(a_handle, Mechanism::kInputChannel);
			Input::InputService::GetSingleton().Release(a_handle);
			REX::DEBUG("[Scene] scene {:#010x} scene controls disabled by config (legacy playerControl)", a_handle);
			return;
		}
		auto*      player = RE::PlayerCharacter::GetSingleton();
		const bool hasPlayer = player &&
			std::find(a_participants.begin(), a_participants.end(), static_cast<RE::Actor*>(player)) != a_participants.end();
		if (!hasPlayer) {
			return;
		}
		Input::Grant grant;
		grant.handle = a_handle;
		grant.capabilities = controls.capabilities;
		grant.driver = player;
		grant.locked = controls.locked;
		RecordInputChannel(a_handle, grant);
		REX::DEBUG("[Scene] scene {:#010x} scene controls engaged (capabilities {:#x}, locked {})", a_handle, grant.capabilities, grant.locked);
	}

	void SceneRuntime::ApplyTransition(std::int32_t a_handle, std::string_view a_oldNode, std::string_view a_newNode,
		bool a_end, std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants)
	{
		Fire(a_handle, Event::kNodeExit, a_oldNode, "exit");

		// A target that can't play must end the scene cleanly, not strand it with the input lock held.
		bool end = a_end;
		Animation::PlaybackId oldPlaybackId = 0;
		{
			std::lock_guard l{ _lock };
			if (const Slot* s = Resolve(a_handle)) oldPlaybackId = s->playbackId;
		}
		Animation::PlaybackId newPlaybackId = 0;
		if (!end && !(newPlaybackId = PlayNodeAnim(a_participants, a_sceneId, a_newNode, oldPlaybackId))) {
			REX::WARN("[Scene] scene {:#010x} transition '{}' -> '{}' could not play the target node — ending scene",
				a_handle, a_oldNode, a_newNode);
			UI::HudMessage::Error(std::format("could not play '{}' — scene ended", a_newNode));
			end = true;
			std::lock_guard l{ _lock };
			if (Slot* s = Resolve(a_handle)) {
				s->terminationCause = TerminationCause::kInterrupted;
			}
		}

		if (end) {
			TerminationCause terminationCause = TerminationCause::kCompleted;
			{
				std::lock_guard l{ _lock };
				if (const Slot* s = Resolve(a_handle)) {
					terminationCause = s->terminationCause;
				}
			}
			StopGraph(a_participants, oldPlaybackId);  // also tears down the old graph after a failed play
			Fire(a_handle, Event::kSceneEnd, a_oldNode, "");  // async Papyrus dispatch
			REX::INFO("[Scene] end id='{}' handle={:#010x} cause={}", a_sceneId, a_handle,
				TerminationCauseName(terminationCause));
			ReleaseSlot(a_handle);  // also clears the transitioning guard
			UI::HudMessage::Debug("OSF: scene ended");
		} else {
			{
				std::lock_guard l{ _lock };
				if (Slot* s = Resolve(a_handle)) {
					s->playbackId = newPlaybackId;
				}
			}
			Fire(a_handle, Event::kNodeEnter, a_newNode, "enter");
			if (UI::HudMessage::DebugEnabled()) {
				const auto         def = Registry::ContentRegistry::GetSingleton().Find(a_sceneId);
				const std::int32_t stage = def ? def->LinearStageOf(a_newNode) : -1;
				UI::HudMessage::Show(stage >= 0
						? std::format("OSF: stage {}/{} - {}", stage + 1, def->linearStages.size(), a_newNode)
						: std::format("OSF: -> {}", a_newNode));
			}
			EndTransition(a_handle);
		}
	}

	void SceneRuntime::EndTransition(std::int32_t a_handle)
	{
		std::lock_guard l{ _lock };
		if (Slot* s = Resolve(a_handle)) {
			s->transitioning = false;
		}
	}

	std::int32_t SceneRuntime::Start(std::string_view a_id, std::string_view a_entryNode,
		const std::vector<RE::Actor*>& a_participants, const AnchorOverride& a_anchor, const StartOverrides& a_over)
	{
		const auto definition = Registry::ContentRegistry::GetSingleton().Find(a_id);
		// Every def start funnels here: participants[i] must satisfy roles[i]'s filters.
		if (definition) {
			const std::size_t n = std::min(a_participants.size(), definition->roles.size());
			for (std::size_t i = 0; i < n; i++) {
				if (!Matchmaking::RoleAccepts(definition->roles[i], a_participants[i])) {
					REX::WARN("[Scene] start '{}' refused — actor {:X} fails role '{}' filters",
						a_id, a_participants[i] ? a_participants[i]->formID : 0, definition->roles[i].name);
					return 0;
				}
			}
		}

		const std::int32_t handle = MintSlot(a_id, a_entryNode, a_participants);
		if (!handle) {
			return 0;
		}
		// Store before the first play; plans are rebuilt per node, so these re-apply rather than compound.
		{
			std::lock_guard l{ _lock };
			if (Slot* s = Resolve(handle)) {
				s->definition = definition;
				s->anchorRef.reset(a_over.anchorRef);
				s->anchorBase = a_over.anchorBase;
				if (a_anchor.set) {
					s->anchor = a_anchor;
				}
				s->loopScale = a_over.loopScale;
				s->speed.Set(a_over.speed.value_or(1.0f));
				s->worldPlacement = a_over.worldPlacement;
				s->cameraOverridden = a_over.camera.has_value();
			}
		}
		// An unplayable entry would hold the input lock with no graph for the watchdog to recover.
		const Animation::PlaybackId playbackId = PlayNodeAnim(a_participants, a_id, a_entryNode);
		if (!playbackId) {
			REX::WARN("[Scene] start '{}' entry node '{}' could not play — aborting start", a_id, a_entryNode);
			ReleaseSlot(handle);
			return 0;
		}
		{
			std::lock_guard l{ _lock };
			if (Slot* s = Resolve(handle)) {
				s->playbackId = playbackId;
			}
		}
		std::string castDiag;
		for (RE::Actor* actor : a_participants) {
			castDiag += std::format("{}{:08X}", castDiag.empty() ? "" : ",", actor->formID);
		}
		REX::DEBUG("[Scene] start correlation handle={:#010x} playback={} id='{}' cast=[{}]",
			handle, playbackId, a_id, castDiag);
		bool playerInputLock = true;
		bool hideApparel = true;
		bool clearHeldItems = true;
		bool fade = false;
		if (definition) {
			playerInputLock = definition->playerInputLock;
			hideApparel = definition->hideApparel;
			clearHeldItems = definition->clearHeldItems;
			fade = definition->fade;
		}
		playerInputLock = a_over.playerInputLock.value_or(playerInputLock);
		hideApparel = a_over.hideApparel.value_or(hideApparel);
		fade = a_over.fade.value_or(fade);
		CompleteStart(handle, a_id, a_entryNode, a_participants, playerInputLock, hideApparel,
			clearHeldItems, fade, a_over);
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
			if (s->transitioning) {
				return false;
			}
			oldNode = s->node;
			s->node = std::string(a_node);
			newNode = s->node;
			sceneId = s->id;
			participants = s->participants;
			s->transitioning = true;
		}

		ApplyTransition(a_scene, oldNode, newNode, /*a_end*/ false, sceneId, participants);
		return true;
	}

	bool SceneRuntime::Stop(std::int32_t a_scene)
	{
		// Bails if a transition is in flight; that transition owns the teardown.
		SlotView view;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_scene);
			if (!s || s->transitioning) {
				return false;
			}
			view.id = s->id;
			view.node = s->node;
			view.participants = s->participants;
			s->terminationCause = TerminationCause::kCancelled;
			s->transitioning = true;
		}

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
		return Stop(handle);
	}

	static std::string_view ResolveEntryNode(const Registry::SceneDef& a_def, std::string_view a_entryNode)
	{
		if (a_entryNode.empty()) {
			return a_def.entry;
		}
		if (a_def.FindNode(a_entryNode)) {
			return a_entryNode;
		}
		REX::WARN("[Scene] start '{}': entry node '{}' not found — using the scene's entry", a_def.id, a_entryNode);
		return a_def.entry;
	}

	std::int32_t SceneRuntime::StartFromDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants,
		const StartOverrides& a_over, std::string_view a_entryNode)
	{
		const auto def = Registry::ContentRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::ERROR("[Scene] StartFromDef: no scene def '{}'", a_sceneId);
			return 0;
		}
		return Start(def->id, ResolveEntryNode(*def, a_entryNode), a_participants, AnchorOverride{}, a_over);
	}

	std::int32_t SceneRuntime::StartFromDefAt(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants,
		RE::NiPoint3 a_anchorPos, float a_anchorHeading, const StartOverrides& a_over, std::string_view a_entryNode)
	{
		const auto def = Registry::ContentRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::ERROR("[Scene] StartFromDefAt: no scene def '{}'", a_sceneId);
			return 0;
		}
		return Start(def->id, ResolveEntryNode(*def, a_entryNode), a_participants,
			AnchorOverride{ true, a_anchorPos, a_anchorHeading }, a_over);
	}

	std::int32_t SceneRuntime::StartFromPlan(const std::vector<RE::Actor*>& a_participants, Animation::PlaybackPlan a_plan,
		std::int32_t a_startStage, const AnchorOverride& a_anchor, const StartOverrides& a_over)
	{
		if (a_participants.empty() || a_plan.stages.empty()) {
			return 0;
		}
		if (a_startStage < 0 || static_cast<std::size_t>(a_startStage) >= a_plan.stages.size()) {
			return 0;
		}
		if (a_anchor.set) {
			a_plan.anchorResolved = true;
			a_plan.restoreTransformsOnEnd = true;
			a_plan.anchorPos = a_anchor.pos;
			a_plan.anchorHeading = a_anchor.heading;
		}
		if (a_over.worldPlacement.has_value()) {
			a_plan.SetWorldPlacementMode(*a_over.worldPlacement);
		}
		const std::int32_t handle = MintSlot("", "main", a_participants);
		if (!handle) {
			return 0;
		}
		{
			std::lock_guard l{ _lock };
			auto* s = Resolve(handle);
			s->anchor = a_anchor;
			s->anchorRef.reset(a_over.anchorRef);
			s->anchorBase = a_over.anchorBase;
			s->speed.Set(a_over.speed.value_or(a_plan.speed));
			s->speed.Apply(a_plan);
			s->loopScale = a_over.loopScale;
			s->cameraOverridden = a_over.camera.has_value();
			s->worldPlacement = a_over.worldPlacement.value_or(a_plan.anchored ?
				Animation::WorldPlacementMode::kAnchorAndPin : Animation::WorldPlacementMode::kFollowActor);
			ApplyScenePlacement(a_plan, a_anchor, {}, s->baseline, a_participants);
		}
		Animation::PlaybackId playbackId = 0;
		if (!Animation::GraphManager::GetSingleton().PlaySynchronized(a_participants, a_plan, a_startStage,
			&playbackId, 0, _playbackSinkId)) {
			ReleaseSlot(handle);
			return 0;
		}
		{
			std::lock_guard l{ _lock };
			if (Slot* s = Resolve(handle)) {
				s->playbackId = playbackId;
			}
		}
		const bool playerInputLock = a_over.playerInputLock.value_or(true);
		if (const auto playback = Animation::GraphManager::GetSingleton().GetScenePlayback(a_participants.front())) {
			std::lock_guard l{ _lock };
			if (auto* s = Resolve(handle)) s->frozenAnchor = { true, playback->anchorPos, playback->anchorHeading };
		}
		const bool hideApparel = a_over.hideApparel.value_or(true);
		const bool fade = a_over.fade.value_or(false);
		CompleteStart(handle, "", "main", a_participants, playerInputLock, hideApparel, true, fade, a_over);
		return handle;
	}

	std::int32_t SceneRuntime::StartFromDefRoles(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_actors,
		const std::vector<std::string>& a_roles, const AnchorOverride& a_anchor, const StartOverrides& a_over,
		std::string_view a_entryNode)
	{
		const auto def = Registry::ContentRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::ERROR("[Scene] StartSceneRoles: no scene def '{}'", a_sceneId);
			return 0;
		}
		if (a_actors.size() != a_roles.size()) {
			REX::ERROR("[Scene] StartSceneRoles '{}': {} actor(s) vs {} role name(s)",
				a_sceneId, a_actors.size(), a_roles.size());
			return 0;
		}
		std::vector<RE::Actor*> ordered;
		std::string bindingError;
		if (!Matchmaking::BindSceneRoles(*def, a_actors, a_roles, ordered, bindingError)) {
			REX::ERROR("[Scene] StartSceneRoles '{}': {}", a_sceneId, bindingError);
			return 0;
		}

		return Start(def->id, ResolveEntryNode(*def, a_entryNode), ordered, a_anchor, a_over);
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
			if (s->transitioning) {
				REX::DEBUG("[Scene] edge on scene {:#010x} ignored — a transition is already in flight", a_scene);
				return false;
			}
			const auto def = s->definition;
			const auto* node = def ? def->FindNode(s->node) : nullptr;
			if (!node) {
				return false;
			}
			const Registry::SceneEdge* edge = a_selectEdge(*node);
			if (!edge) {
				return false;
			}
			oldNode = s->node;
			sceneId = s->id;
			participants = s->participants;
			if (edge->to == "$end") {
				end = true;
			} else if (s->singleStage) {
				// Single-stage: leaving the node is a cancel; an authored $end stays a completion.
				end = true;
				s->terminationCause = TerminationCause::kCancelled;
			} else {
				s->node = edge->to;
				newNode = edge->to;
			}
			s->transitioning = true;
		}

		ApplyTransition(a_scene, oldNode, newNode, end, sceneId, participants);
		return true;
	}

	bool SceneRuntime::Advance(std::int32_t a_scene)
	{
		const std::string fromNode = GetNode(a_scene);
		if (fromNode.empty()) {
			REX::DEBUG("[Scene] Advance scene {:#010x} ignored — no active scene on this handle", a_scene);
			return false;
		}

		const bool took = TakeEdge(a_scene, [](const Registry::SceneNode& a_node) -> const Registry::SceneEdge* {
			for (const auto& e : a_node.edges) {
				if (e.when == Registry::EdgeWhen::kAdvance && e.isDefault) {
					return &e;
				}
			}
			return nullptr;
		});

		if (took) {
			const std::string toNode = GetNode(a_scene);
			REX::DEBUG("[Scene] Advance scene {:#010x} — node '{}' -> {}",
				a_scene, fromNode, toNode.empty() ? "$end (scene ended)" : ("'" + toNode + "'"));
		} else {
			REX::DEBUG("[Scene] Advance scene {:#010x} — node '{}' has no default advance edge (nothing to do)",
				a_scene, fromNode);
		}
		return took;
	}

	bool SceneRuntime::Navigate(std::int32_t a_scene, std::string_view a_edgeId)
	{
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

	bool SceneRuntime::OnGraphAutoEnd(Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_participants,
		Animation::PlaybackEndReason a_reason)
	{
		(void)a_participants;  // the slot is selected by playback id, not actors
		std::int32_t handle = 0;
		std::string oldNode;
		std::string newNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		bool end = false;
		bool tookEdge = false;
		{
			std::lock_guard l{ _lock };

			// Actors may already belong to a replacement scene by the time this deferred task runs.
			Slot* s = FindSlotForPlayback(a_playbackId, &handle);
			if (!s) {
				return false;  // standalone scene, or stale task
			}
			if (s->transitioning) {
				// The in-flight transition owns the teardown; report handled so GraphManager doesn't also stop it.
				REX::DEBUG("[Scene] auto-end for scene {:#010x} skipped — a transition is already in flight", handle);
				return true;
			}

			oldNode = s->node;
			sceneId = s->id;
			participants = s->participants;

			// Files scenes have no edges and end here; returning false instead would leak the handle.
			const auto def = s->definition;
			const auto* node = def ? def->FindNode(s->node) : nullptr;
			if (a_reason == Animation::PlaybackEndReason::kInterrupted) {
				s->terminationCause = TerminationCause::kInterrupted;
				end = true;  // unloaded / AI-disabled; never advance an edge
			} else if (!node) {
				end = true;
			} else {
				const auto wantWhen = (a_reason == Animation::PlaybackEndReason::kTimerComplete)
					? Registry::EdgeWhen::kTimer
					: (node->loopMode == Registry::LoopMode::kCount ? Registry::EdgeWhen::kLoops : Registry::EdgeWhen::kEnd);
				const Registry::SceneEdge* edge = SelectAutoEdge(*node, wantWhen);
				if (edge) {
					tookEdge = true;
					if (edge->to == "$end" || s->singleStage) {
						end = true;
					} else {
						s->node = edge->to;
						newNode = edge->to;
					}
				} else {
					end = true;  // terminal completion
				}
			}
			s->transitioning = true;
		}

		const char* reasonStr = a_reason == Animation::PlaybackEndReason::kTimerComplete ? "timer"
			: (a_reason == Animation::PlaybackEndReason::kInterrupted ? "interrupted" : "loops");
		REX::DEBUG("[Scene] scene {:#010x} auto-end (reason={}) node '{}' -> {}{}",
			handle, reasonStr, oldNode, end ? "$end" : newNode, tookEdge ? "" : " (terminal, no edge)");

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
		const auto def = s->definition;
		const auto* node = def ? def->FindNode(s->node) : nullptr;
		if (!node) {
			return out;
		}
		for (const auto& e : node->edges) {
			// Linear stages' synthetic default edge has no id and isn't navigable.
			if (e.when == Registry::EdgeWhen::kAdvance && !e.id.empty()) {
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
