#include "Scene/SceneRuntime.h"
#include "Profiling/Profile.h"

#include "Animation/GraphManager.h"
#include "Input/InputService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/ContentRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneTiming.h"
#include "UI/FadeService.h"
#include "UI/HudMessage.h"
#include "Util/FormRef.h"
#include "Util/NativeMainThreadQueue.h"
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

		void ApplyNodeMarks(Animation::PlaybackPlan& a_plan, const Registry::SceneNode& a_node)
		{
			if (a_plan.stages.empty()) {
				return;
			}
			auto& marks = a_plan.stages.back().marks;
			// Every lane's token is the entry index into the node's lane.
			ScheduleMarks(marks, a_node.cues, Registry::CuePos::kFraction, Registry::CuePos::kEnd, kLaneCue, [](const auto&, std::size_t a_i) { return std::to_string(a_i); });
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
		OSF_PROFILE_SCOPE("Scene.OnTimedMarks");
		OSF_PROFILE_GAUGE("Scene.OnTimedMarks.Batch", a_marks.size());
		(void)a_participants;
		if (a_marks.empty()) {
			return;
		}

		// Lane order action -> camera -> sound -> cue. Batches usually arrive ordered (one mark, or one lane), so copy
		// and stable-sort only when they don't.
		const auto byLane = [](const Animation::FiredMark& a_lhs, const Animation::FiredMark& a_rhs) { return a_lhs.lane < a_rhs.lane; };
		std::vector<Animation::FiredMark> sortedMarks;
		const std::vector<Animation::FiredMark>* ordered = &a_marks;
		if (!std::is_sorted(a_marks.begin(), a_marks.end(), byLane)) {
			sortedMarks = a_marks;
			std::stable_sort(sortedMarks.begin(), sortedMarks.end(), byLane);
			ordered = &sortedMarks;
		}

		std::int32_t handle = 0;
		std::string oldNode;
		std::string sceneId;
		std::vector<RE::Actor*> participants;
		Registry::SceneRef definition;
		bool cameraOverridden = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = FindSlotForPlayback(a_playbackId, &handle);
			if (!s || s->transitioning) {
				return;  // standalone playback, preparing, or stale task from a replaced node
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

		// Cues are deferred until the tick's other entries have run. `def` pins the definition, so node entries stay valid.
		std::vector<const Registry::CueEntry*> cues;
		std::vector<const Animation::FiredMark*> cueMarks;
		for (const auto& m : *ordered) {
			if (!LifecycleValid(handle, oldNode, true))
				return;
			if (m.lane == kLaneCue) {
				std::size_t idx = 0;
				if (node && ParseIndexToken(m.token, node->cues.size(), idx)) {
					cues.push_back(&node->cues[idx]);
					cueMarks.push_back(&m);
				}
			} else if (m.lane == kLaneAction && node) {
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->actions.size(), idx)) {
					RunAction(handle, oldNode, node->actions[idx], "", hasPlayer);
				}
			} else if (m.lane == kLaneSound && node) {
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->sounds.size(), idx)) {
					const auto& snd = node->sounds[idx];
#ifdef OSF_ANIMATION_SMOKE
					Testing::D3AudioProbe::BeginMark(handle, a_playbackId, oldNode, m.lane, m.token, m.detectedNs, m.d3MotionLateNs, m.d3InputDelta);
#endif
					PlaySound(handle, snd.spec, snd.role, snd.emitter);
#ifdef OSF_ANIMATION_SMOKE
					Testing::D3AudioProbe::EndMark();
#endif
				}
			} else if (m.lane == kLaneCamera && node && !cameraOverridden) {
				std::size_t idx = 0;
				if (ParseIndexToken(m.token, node->cameras.size(), idx)) {
					RunCamera(handle, node->cameras[idx].state, hasPlayer, node->cameras[idx].distance);
				}
			}
		}

		// TODO: thread the fired fraction back from the Scene; time is sent as -1.
		for (std::size_t cueIndex = 0; cueIndex < cues.size(); ++cueIndex) {
			if (!LifecycleValid(handle, oldNode, true))
				return;
			const auto& cue = *cues[cueIndex];
			const auto& mark = *cueMarks[cueIndex];
#ifdef OSF_ANIMATION_SMOKE
			Testing::D3AudioProbe::BeginMark(handle, a_playbackId, oldNode, mark.lane, mark.token, mark.detectedNs, mark.d3MotionLateNs, mark.d3InputDelta);
#endif
			DispatchCue(handle, oldNode, cue.id, cue.role, "", -1.0f, mark.detectedNs, cue.nativeOnly);
#ifdef OSF_ANIMATION_SMOKE
			Testing::D3AudioProbe::EndMark();
#endif
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
					for (const auto* cue : cues) {
						const Registry::SceneEdge* edge = nullptr;
						for (const auto& e : n->edges) {
							if (e.when == Registry::EdgeWhen::kTrigger && Util::IEquals(e.trigger, cue->id)) {
								edge = &e;
								break;
							}
						}
						if (edge) {
							triggered = true;
							if (edge->to == "$end" || s->singleStage) {
								end = true;
							} else {
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
			ApplyTransition(handle, oldNode, newNode, end, sceneId, participants, TransitionOrigin::TimedCue);
		}
	}

	bool SceneRuntime::PreparePlayback(const std::shared_ptr<PreparedTransition>& p)
	{
		OSF_PROFILE_SCOPE("Scene.PreparePlayback");
		if (p->context.kind == Preparation::Kind::Stage) return true; // this session already owns every segment
		if (p->cast.empty() || !PreparationValid(p)) return false;
		// Definitions and destination cast are pinned by the attempt. No slot identity or
		// alignment state changes until the prepared resources pass final admission.
		float loopScale = 1.0f;
		bool autoAdvance = true;
		std::optional<Animation::WorldPlacementMode> placementOverride;
		AnchorOverride anchor;
		AnchorOverride frozenAnchor;
		ActorBaseline baseline;
		{
			std::lock_guard l{ _lock };
			const auto* s = Resolve(p->context.scene);
			if (!s || s->preparation != p) return false;
			anchor = p->context.kind == Preparation::Kind::Handoff ? p->anchor : s->anchor;
			frozenAnchor = s->frozenAnchor;
			baseline = s->baseline;
			loopScale = s->loopScale;
			autoAdvance = s->autoAdvance;
			placementOverride = s->worldPlacement;
		}
		auto plan = p->plan;
		if (!plan) {
			const auto* node = p->target ? p->target->FindNode(p->context.toNode) : nullptr;
			if (!node || (node->use.empty() && node->stages.empty())) return false;
			plan = Registry::ContentRegistry::GetSingleton().BuildNodePlan(p->target, *node, p->cast.size());
			if (!plan) {
				REX::WARN("[Scene] node '{}' not playable for {} participant(s)", p->context.toNode, p->cast.size());
				return false;
			}
			ApplyNodeTiming(*plan, *node, loopScale);
			plan->autoAdvance = !node->allowManualHold || autoAdvance;
			ApplyNodeMarks(*plan, *node);
		}
		ApplyScenePlacement(*plan, anchor, frozenAnchor, baseline, p->cast);
		if (placementOverride) plan->SetWorldPlacementMode(*placementOverride);
		p->playback = Animation::GraphManager::GetSingleton().PrepareSynchronized(p->cast, *plan,
			p->plan ? p->context.toStage : 0, _playbackSinkId);
		return p->playback && PreparationValid(p);
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
		(void)a_hideApparel;
		(void)a_clearHeldItems;  // applied by preparation before playback
		FadeSceneStart(a_handle, a_fade, a_participants);
		EngageDefaultSceneControls(a_handle, a_defId, a_over.sceneControls, a_participants);
		REX::INFO("[Scene] start id='{}' handle={:#010x}", a_defId.empty() ? "<ad-hoc>" : a_defId, a_handle);
		Fire(a_handle, Event::kSceneBegin, a_entryNode, "");
		{
			std::lock_guard l{ _lock };
			const auto*     slot = Resolve(a_handle);
			if (!slot || slot->transitioning || slot->node != a_entryNode)
				return;
		}
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

	void SceneRuntime::ApplyTransition(std::int32_t handle, std::string_view oldNode, std::string_view newNode, bool end, std::string_view sceneId, const std::vector<RE::Actor*>& cast, TransitionOrigin origin)
	{
		OSF_PROFILE_SCOPE("Scene.ApplyTransition");
		SlotView old;
		if (!SnapshotSlot(handle, old))
			return;
		if (end) {
			auto pins = std::make_shared<std::vector<RE::NiPointer<RE::Actor>>>();
			for (auto* actor : cast) pins->emplace_back(actor);
			Util::NativeMainThreadQueue::Post([this, handle, old, pins] {
				SlotView live;
				if (!SnapshotSlot(handle, live) || live.playbackId != old.playbackId)
					return;
				Fire(handle, Event::kNodeExit, old.node, "exit");
				if (!SnapshotSlot(handle, live) || live.playbackId != old.playbackId)
					return;
				StopGraph(old.participants, old.playbackId);
				Fire(handle, Event::kSceneEnd, old.node, "");
				ReleaseSlot(handle);
			});
			return;
		}
		auto p = std::make_shared<PreparedTransition>();
		p->context.scene = handle;
		p->context.kind = Preparation::Kind::Node;
		p->context.fromScene = std::string(sceneId);
		p->context.toScene = std::string(sceneId);
		p->context.fromNode = std::string(oldNode);
		p->context.toNode = std::string(newNode);
		p->old = old;
		p->cast = cast;
		p->target = old.definition;
		p->automatic = IsAutomaticTransition(origin);
		if (old.definition) {
			p->context.fromStage = old.definition->LinearStageOf(oldNode);
			p->context.toStage = old.definition->LinearStageOf(newNode);
		}
		if (!QueuePreparation(p))
			EndTransition(handle);
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
		OSF_PROFILE_SCOPE("Scene.Start");
		auto& registry = Registry::ContentRegistry::GetSingleton();
		const auto definition = registry.ResolveLoopGlobals(registry.Find(a_id));
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
		auto p = std::make_shared<PreparedTransition>();
		p->context.scene = handle;
		p->context.kind = Preparation::Kind::Start;
		p->context.toScene = std::string(a_id);
		p->context.toNode = std::string(a_entryNode);
		p->context.toStage = definition ? definition->LinearStageOf(a_entryNode) : 0;
		p->old = { std::string(a_id), std::string(a_entryNode), a_participants, 0, definition, a_over.camera.has_value() };
		p->cast = a_participants;
		p->target = definition;
		p->overrides = a_over;
		p->defaults = true;
		p->playerInputLock = a_over.playerInputLock.value_or(definition ? definition->playerInputLock : true);
		p->hideApparel = a_over.hideApparel.value_or(definition ? definition->hideApparel : true);
		p->clearHeldItems = definition ? definition->clearHeldItems : true;
		p->fade = a_over.fade.value_or(definition ? definition->fade : false);
		if (!QueuePreparation(p)) {
			ReleaseSlot(handle);
			return 0;
		}
		return handle;  // reserved; SCENE_BEGIN is emitted only after preparation/playback commit
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
			if (!s->definition || !s->definition->FindNode(a_node))
				return false;
			newNode = std::string(a_node);
			sceneId = s->id;
			participants = s->participants;
			s->transitioning = true;
		}

		ApplyTransition(a_scene, oldNode, newNode, /*a_end*/ false, sceneId, participants, TransitionOrigin::Manual);
		return true;
	}

	bool SceneRuntime::Stop(std::int32_t a_scene)
	{
		SlotView view;
		{
			std::lock_guard l{ _lock };
			Slot*           s = Resolve(a_scene);
			if (!s)
				return false;
			if (s->cover) {
				s->cover->cancelled = true;
				if (!s->preparation) s->transitioning = false;
			}
			if (s->preparation) {
				const auto p = s->preparation;
				p->cancelled = true;
				Util::NativeMainThreadQueue::Post([this, p] {
					if (p->context.attempt)
						_preparers.Cancel(p->context.attempt, p->reset ? Preparation::Outcome::WorldReset : Preparation::Outcome::Cancelled);
				});
				return true;
			}
			if (s->transitioning)
				return false;
			view.id = s->id;
			view.node = s->node;
			view.participants = s->participants;
			s->terminationCause = TerminationCause::kCancelled;
			s->transitioning = true;
		}

		ApplyTransition(a_scene, view.node, /*a_newNode*/ "", /*a_end*/ true, view.id, view.participants, TransitionOrigin::Manual);
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
		auto p = std::make_shared<PreparedTransition>();
		p->context.scene = handle;
		p->context.kind = Preparation::Kind::Start;
		p->context.toNode = "main";
		p->context.toStage = a_startStage;
		p->old = { "", "main", a_participants, 0, {}, a_over.camera.has_value() };
		p->cast = a_participants;
		p->plan = std::move(a_plan);
		p->overrides = a_over;
		p->defaults = true;
		p->playerInputLock = a_over.playerInputLock.value_or(true);
		p->hideApparel = a_over.hideApparel.value_or(true);
		p->fade = a_over.fade.value_or(false);
		if (!QueuePreparation(p)) {
			ReleaseSlot(handle);
			return 0;
		}
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
				newNode = edge->to;
			}
			s->transitioning = true;
		}

		ApplyTransition(a_scene, oldNode, newNode, end, sceneId, participants, TransitionOrigin::Manual);
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
		OSF_PROFILE_SCOPE("Scene.Navigate");
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
				// Completion makes resuming the source unsafe. Interruption also cancels admission.
				if (s->preparation) {
					s->preparation->automatic = true;
					if (a_reason == Animation::PlaybackEndReason::kInterrupted) {
						s->preparation->interrupted = true;
						s->preparation->cancelled = true;
					}
				}
				// The in-flight transition owns teardown; don't leave an ended source on veto.
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

		ApplyTransition(handle, oldNode, newNode, end, sceneId, participants, TransitionOrigin::PlaybackCompletion);
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
