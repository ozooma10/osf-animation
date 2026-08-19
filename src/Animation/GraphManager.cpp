#include "GraphManager.h"
#include "GraphManagerClipLoad.h"

#include "Animation/PlaybackAdmission.h"

#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Player/PlayerInputLockService.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "Util/ClipPath.h"
#include "Util/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

namespace OSF::Animation
{
	namespace
	{
		// Hand a batch of game-thread follow-ups to SFSE *after* stateLock is released. SFSE's drain
		// runs every queued task inside its own queue lock while our task bodies take stateLock, so an
		// AddTask made while holding stateLock inverts that order and can ABBA-deadlock against the drain.
		void FlushDeferredTasks(std::vector<std::function<void()>>& a_tasks)
		{
			for (auto& fn : a_tasks) {
				SFSE::GetTaskInterface()->AddTask(std::move(fn));
			}
			a_tasks.clear();
		}


		// The two engine hooks. InstallHooks verifies each vtable slot still points where we expect before patching it.
		// AnimationManager::Update - slot 4, the clock/sampling point.
		// TODO: move to commonlib
		constexpr REL::ID AnimManagerVTableID(467920);
		constexpr REL::ID AnimManagerUpdateFnID(122232);
		constexpr size_t UpdateVFuncIdx = 4;
		
		// BGSModelNode::Update - slot 2, sig (modelNode, &fadeNode->local, NiUpdateData*, outputTransform).
		// Stamping before the original runs is the rig-buffer write point (that same call composes + commits).
		// TODO: move to commonlib
		constexpr REL::ID ModelNodeVTableID(400534);
		constexpr REL::ID ModelNodeUpdateFnID(48634);
		constexpr size_t ModelNodeUpdateVFuncIdx = 2;

		// CAP (not floor) for a pinned actor's cull-sphere radius (meters). The engine fades an actor out as the
		// camera nears it — the SAME near-fade that hides the player in 3rd person when you zoom in — and that
		// fade distance SCALES WITH THIS RADIUS. Inflating the sphere (the old max(...,2.5)) pushed the fade out
		// to ~2.5 m, so zooming the orbit in vanished the partner. We now CAP the radius small so the camera can
		// get close before any fade. Frustum safety comes from the CENTER (kept on the torso, where scene_orbit
		// always points), not from a big radius. (Proven in-game: 8.0 was strictly worse than 2.5.)
		constexpr float kPinCullRadius = 1.0f;

		// Lift the cull-sphere center from the compose-root (= the actor's feet/origin = pinWorld) up to the
		// torso, where the visible posed mesh actually sits. The engine's own capsule-derived bound is centered
		// near the torso; clobbering the center down to the feet mis-aims the sphere low, so at certain orbit
		// angles it leaves the view frustum and the participant pops out. Meters along world +Z.
		constexpr float kPinCullCenterUp = 1.0f;

		// BSFadeNode near-camera fade flag (+0x1B4, a float: 1.0 = drawn, 0.0 = faded).
		// The engine fades an actor out when the third-person camera orbits close; we hold it at 1.0 each frame so pinned participants don't vanish.
		constexpr std::ptrdiff_t kFadeNodeVisFlagOff = 0x1B4;

		bool IsGameMenuPaused()
		{
			if (auto* main = RE::Main::GetSingleton()) {
				return main->isGameMenuPaused;
			}
			return false;
		}

		//flag for if console is open.
		std::atomic<bool> g_consoleOpen{ false };

		class ConsoleMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static ConsoleMenuSink* GetSingleton()
			{
				static ConsoleMenuSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.menuName == RE::Console::MENU_NAME) {
					g_consoleOpen.store(a_event.opening, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		float PlaybackDelta(const RE::BSAnimationUpdateData& a_updateData)
		{
			//Freeze playback if menu open (or console which doesnt set flag)
			if (IsGameMenuPaused() || g_consoleOpen.load(std::memory_order_relaxed)) {
				return 0.0f;
			}

			// Animation updates are normally subdivided into small slices. After a menu pause the engine can report accumulated wall time; 
			// that is a resume/catch-up spike for OSF playback, not animation time.
			constexpr float kMaxUpdateStep = 0.1f;
			const float dt = a_updateData.timeDelta;
			return (std::isfinite(dt) && dt >= 0.0f && dt <= kMaxUpdateStep) ? dt : 0.0f;
		}

		// Validates a synchronized-playback request's shape (actor count, per-segment file/placement counts, no null actors);
		// logs the specific failure. The hook-installed precondition is checked separately by the caller.
		bool ValidatePlaybackPlanArgs(const std::vector<RE::Actor*>& a_actors, const PlaybackPlan& a_plan, int32_t a_startSegment)
		{
			if (a_actors.empty()) {
				REX::ERROR("[Anim] synchronized playback: need >= 1 actor (got 0)");
				return false;
			}
			if (a_plan.stages.empty()) {
				REX::ERROR("[Anim] synchronized playback: plan has no segments");
				return false;
			}
			if (a_startSegment < 0 || static_cast<size_t>(a_startSegment) >= a_plan.stages.size()) {
				REX::ERROR("[Anim] synchronized playback: start segment {} out of range (plan has {} segments)", a_startSegment, a_plan.stages.size());
				return false;
			}
			if (a_plan.anchorExplicit &&
				(!std::isfinite(a_plan.anchorPos.x) || !std::isfinite(a_plan.anchorPos.y) ||
					!std::isfinite(a_plan.anchorPos.z) || !std::isfinite(a_plan.anchorHeading))) {
				REX::ERROR("[Anim] synchronized playback: explicit anchor contains a non-finite value");
				return false;
			}
			if (!HasValidRolePolicyShape(a_plan, a_actors.size())) {
				REX::ERROR("[Anim] synchronized playback: role policies do not match {} actor(s) or contain an invalid pose weight/mask "
					"({} preserve, {} modes, {} weights, {} masks, {} names)",
					a_actors.size(), a_plan.preserveBones.size(), a_plan.poseModes.size(),
					a_plan.poseWeights.size(), a_plan.masks.size(), a_plan.roleNames.size());
				return false;
			}
			for (size_t s = 0; s < a_plan.stages.size(); s++) {
				const auto& stage = a_plan.stages[s];
				if (stage.files.size() != a_actors.size() ||
					(!stage.animIds.empty() && stage.animIds.size() != stage.files.size()) ||
					(!stage.placements.empty() && stage.placements.size() != a_actors.size()) ||
					(!stage.masks.empty() && stage.masks.size() != a_actors.size()) ||
					(!stage.poseModes.empty() && stage.poseModes.size() != a_actors.size()) ||
					(!stage.poseWeights.empty() && stage.poseWeights.size() != a_actors.size())) {
					REX::ERROR("[Anim] synchronized playback: segment {} does not match the actor count ({} files, {} anim ids, {} placements, {} masks, {} modes, {} weights, {} actors)",
						s, stage.files.size(), stage.animIds.size(), stage.placements.size(), stage.masks.size(),
						stage.poseModes.size(), stage.poseWeights.size(), a_actors.size());
					return false;
				}
				if (!std::isfinite(stage.timer)) {
					REX::ERROR("[Anim] synchronized playback: segment {} timer is non-finite", s);
					return false;
				}
				for (const auto& placement : stage.placements) {
					if (!std::isfinite(placement.x) || !std::isfinite(placement.y) ||
						!std::isfinite(placement.z) || !std::isfinite(placement.heading)) {
						REX::ERROR("[Anim] synchronized playback: segment {} placement contains a non-finite value", s);
						return false;
					}
				}
			}
			for (size_t i = 0; i < a_actors.size(); i++) {
				if (!a_actors[i]) {
					REX::ERROR("[Anim] synchronized playback: null actor at index {}", i);
					return false;
				}
			}
			return true;
		}

		// Loads every clip of every segment up front and assembles the PlaybackSession. Returns nullptr
		// if any clip fails to load; the caller refuses partial playback.
		std::shared_ptr<PlaybackSession> BuildPlaybackFromPlan(const std::vector<RE::Actor*>& a_actors, const PlaybackPlan& a_plan,
			int32_t a_startSegment)
		{
			auto scene = std::make_shared<PlaybackSession>();
			scene->stages.reserve(a_plan.stages.size());
			for (const auto& planStage : a_plan.stages) {
				PlaybackSession::SegmentData stage;
				stage.timer = planStage.timer;
				stage.loops = planStage.loops;
				stage.hold = std::isfinite(planStage.hold) && planStage.hold >= 0.0f ?
				                 std::min(planStage.hold, 1.0f) : -1.0f;
				const float defaultBlend = std::isfinite(a_plan.blendIn) && a_plan.blendIn >= 0.0f ? a_plan.blendIn : 0.4f;
				stage.blendIn = std::isfinite(planStage.blendIn) && planStage.blendIn >= 0.0f ? planStage.blendIn : defaultBlend;
				stage.placements = planStage.placements.empty() ?
				                       std::vector<ParticipantPlacement>(a_actors.size()) :
				                       planStage.placements;
				stage.masks = planStage.masks.empty() ?
				                  (a_plan.masks.empty() ? std::vector<std::string>(a_actors.size()) : a_plan.masks) :
				                  planStage.masks;
				stage.poseModes = planStage.poseModes.empty() ?
				                      (a_plan.poseModes.empty() ? std::vector<PoseMode>(a_actors.size(), PoseMode::kOverride) : a_plan.poseModes) :
				                      planStage.poseModes;
				stage.poseWeights = planStage.poseWeights.empty() ?
				                        (a_plan.poseWeights.empty() ? std::vector<float>(a_actors.size(), 1.0f) : a_plan.poseWeights) :
				                        planStage.poseWeights;
				stage.contactPose = planStage.contactPose.empty() ?
				                       std::vector<ContactPose>(a_actors.size()) : planStage.contactPose;
				stage.marks = planStage.marks;
				for (std::size_t clipIdx = 0; clipIdx < planStage.files.size(); clipIdx++) {
					const AnimationClipSpec clipSpec{
						.resourcePath = planStage.files[clipIdx],
						.animationId = clipIdx < planStage.animIds.size() ? planStage.animIds[clipIdx] : std::string{}
					};
					auto file = Util::ResolveClipSpec(std::filesystem::path{ clipSpec.resourcePath });
					auto load = GraphManagerClipLoad::Load(file, clipSpec.animationId);
					if (!load.ok) {
						REX::ERROR("[Anim] synchronized playback: failed to load '{}' ({}) — session not started",
							load.source.empty() ? file.display : load.source, load.detail);
						return nullptr;
					}
					stage.participants.push_back({ std::move(load.skeleton), std::move(load.anim), clipSpec.resourcePath });
				}
				stage.duration = stage.participants[0].anim->data->duration();
				for (std::size_t i = 1; i < stage.participants.size(); i++) {
					const float participantDuration = stage.participants[i].anim->data->duration();
					if (std::abs(participantDuration - stage.duration) > 0.01f) {
						REX::WARN("[Anim] synchronized playback: segment participant {} duration {:.3f}s differs from reference {:.3f}s; sampling wraps independently",
							i, participantDuration, stage.duration);
					}
				}
				scene->stages.push_back(std::move(stage));
			}
			scene->animId = a_plan.animId;
			scene->anchored = a_plan.anchored;
			scene->restoreParticipantTransforms = a_plan.anchored && a_plan.anchorExplicit;
			scene->loopWhole = a_plan.loopWhole;
			if (scene->restoreParticipantTransforms) {
				if (a_plan.baselineTransforms.size() == a_actors.size()) {
					scene->originalTransforms = a_plan.baselineTransforms;  // caller-carried pre-playback baseline (see PlaybackPlan)
				} else {
					scene->originalTransforms.reserve(a_actors.size());
					for (const auto* actor : a_actors) {
						scene->originalTransforms.emplace_back(actor->data.location, actor->data.angle.z);
					}
				}
			}
			const float requestedSpeed = std::isfinite(a_plan.speed) ? a_plan.speed : 1.0f;
			scene->speed = std::clamp(requestedSpeed, 0.0f, 100.0f);  // same range as SetSpeed

			scene->SetSegment(a_startSegment);

			// Anchor at actor[0]'s current transform by default; participant world positions are anchor + each placement's offset rotated into the anchor heading frame (see PlacementToWorld).
			// An explicit anchor (including the StartSceneAt compatibility API) overrides actor[0], so the playback session world-anchors to a thing (furniture/marker).
			if (a_plan.anchorExplicit) {
				scene->anchorPos = a_plan.anchorPos;
				scene->anchorHeading = a_plan.anchorHeading;
			} else {
				scene->anchorPos = a_actors[0]->data.location;
				scene->anchorHeading = a_actors[0]->data.angle.z;
			}

			// Arm the stall watchdog from birth. MaintenanceTick skips lastAdvanceMs == 0, so a session the
			// engine never ticks even once (participant 3D dropped right after start) would otherwise be
			// invisible to it and hold the player input lock forever.
			scene->lastAdvanceMs.store(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch())
					.count(),
				std::memory_order_relaxed);
			return scene;
		}
	}

	GraphManager& GraphManager::GetSingleton()
	{
		static GraphManager singleton;
		return singleton;
	}

	PlaybackSinkId GraphManager::RegisterPlaybackSink(PlaybackSink a_sink)
	{
		PlaybackSinkId id = _nextPlaybackSinkId.fetch_add(1, std::memory_order_relaxed);
		if (id == 0) {
			id = _nextPlaybackSinkId.fetch_add(1, std::memory_order_relaxed);
		}
		std::unique_lock l{ _sinkLock };
		_playbackSinks.emplace(id, std::move(a_sink));
		return id;
	}

	bool GraphManager::UnregisterPlaybackSink(PlaybackSinkId a_id)
	{
		if (a_id == 0) return false;
		std::unique_lock l{ _sinkLock };
		return _playbackSinks.erase(a_id) != 0;
	}

	std::optional<GraphManager::PlaybackSink> GraphManager::GetPlaybackSink(PlaybackSinkId a_id) const
	{
		if (a_id == 0) return std::nullopt;
		std::shared_lock l{ _sinkLock };
		const auto it = _playbackSinks.find(a_id);
		return it == _playbackSinks.end() ? std::nullopt : std::optional<PlaybackSink>{ it->second };
	}

	void GraphManager::InstallHooks()
	{
		REL::Relocation<uintptr_t> vtbl{ AnimManagerVTableID };
		REL::Relocation<uintptr_t> expected{ AnimManagerUpdateFnID };

		const uintptr_t slotValue = *reinterpret_cast<uintptr_t*>(vtbl.address() + UpdateVFuncIdx * sizeof(uintptr_t));
		if (slotValue != expected.address()) {
			REX::ERROR("[Anim] AnimationManager vtable slot {} = {:X}, expected AnimationManager::Update at {:X} — "
				"AddressLib IDs stale for this game version, NOT patching (animations disabled)",
				UpdateVFuncIdx, slotValue, expected.address());
			return;
		}

		REL::Relocation<uintptr_t> mnVtbl{ ModelNodeVTableID };
		REL::Relocation<uintptr_t> mnExpected{ ModelNodeUpdateFnID };

		const uintptr_t mnSlotValue = *reinterpret_cast<uintptr_t*>(mnVtbl.address() + ModelNodeUpdateVFuncIdx * sizeof(uintptr_t));
		if (mnSlotValue != mnExpected.address()) {
			REX::ERROR("[Anim] BGSModelNode vtable slot {} = {:X}, expected BGSModelNode::Update at {:X} — "
				"AddressLib IDs stale for this game version, NOT patching (animations disabled)",
				ModelNodeUpdateVFuncIdx, mnSlotValue, mnExpected.address());
			return;
		}

		_origAnimGraphUpdate = reinterpret_cast<AnimUpdateFn*>(
			vtbl.write_vfunc(UpdateVFuncIdx, &Hook_AnimGraphUpdate));
		_origModelNodeUpdate = reinterpret_cast<ModelNodeUpdateFn*>(
			mnVtbl.write_vfunc(ModelNodeUpdateVFuncIdx, &Hook_ModelNodeUpdate));
		REX::TRACE("[Anim] installed AnimationManager::Update (vfunc {}) + BGSModelNode::Update (vfunc {}) hooks",
			UpdateVFuncIdx, ModelNodeUpdateVFuncIdx);
	}

	void GraphManager::RegisterConsolePauseSink()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::WARN("[Anim] console-pause sink not registered: RE::UI singleton null — the console will NOT freeze playback");
			return;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(ConsoleMenuSink::GetSingleton());
		REX::DEBUG("[Anim] registered console MenuOpenCloseEvent sink (freezes playback while the console is open)");
	}

	bool GraphManager::PlayAnimation(RE::Actor* a_actor, std::string_view a_file, std::string_view a_animId)
	{
		if (!a_actor) {
			return false;
		}

		if (!_origAnimGraphUpdate) {
			REX::ERROR("[Anim] Play refused: update hook is not installed");
			return false;
		}

		auto file = Util::ResolveClipSpec(std::filesystem::path{ a_file });

		auto loadResult = GraphManagerClipLoad::Load(file, a_animId);
		if (!loadResult.ok) {
			REX::ERROR("[Anim] Failed to load animation '{}' ({})", loadResult.source.empty() ? file.display : loadResult.source, loadResult.detail);
			return false;
		}

		const bool played = PlayDecodedAnimation(a_actor, std::move(loadResult.skeleton),
			std::move(loadResult.anim), std::string(a_file));
		if (played) {
			REX::DEBUG("[Anim] Playing '{}' on actor {:X}", loadResult.source.empty() ? file.display : loadResult.source, a_actor->formID);
		}
		return played;
	}

	bool GraphManager::PlayAnimationBytes(RE::Actor* a_actor, const std::vector<std::uint8_t>& a_bytes,
		std::string_view a_clipKey, std::string* a_error)
	{
		if (!a_actor) {
			if (a_error) *a_error = "The player is unavailable";
			return false;
		}
		if (!_origAnimGraphUpdate) {
			if (a_error) *a_error = "OSF Animation playback hooks are unavailable";
			return false;
		}

		auto loaded = GraphManagerClipLoad::LoadAfBytes(a_clipKey, a_bytes);
		if (!loaded.ok) {
			if (a_error) *a_error = loaded.detail;
			REX::ERROR("[Anim] Studio preview '{}' failed to decode: {}", a_clipKey, loaded.detail);
			return false;
		}

		if (!PlayDecodedAnimation(a_actor, std::move(loaded.skeleton), std::move(loaded.anim), std::string(a_clipKey))) {
			if (a_error) *a_error = "The player is already participating in an OSF scene";
			return false;
		}
		return true;
	}

	bool GraphManager::PlayDecodedAnimation(RE::Actor* a_actor,
		std::shared_ptr<const OzzSkeleton> a_skeleton,
		std::shared_ptr<const OzzAnimation> a_anim,
		std::string a_source)
	{
		std::shared_ptr<Graph> g;
		{
			std::unique_lock l{ stateLock };
			auto& slot = graphs[a_actor];
			if (slot) {
				// Refuse to clobber a synchronized-playback participant (it would keep g->playbackSession set and the next segment tick would overwrite this clip anyway).
				// Mirror of StopAnimation's guard — use the StopScene compatibility API to end the session first.
				std::scoped_lock gl{ slot->stateLock };
				if (slot->playbackSession) {
					REX::WARN("[Anim] Play refused: actor {:X} is in a synchronized playback session — use StopScene first", a_actor->formID);
					return false;
				}
			} else {
				slot = std::make_shared<Graph>();
				slot->target.reset(a_actor);
			}
			g = slot;
			graphCount.store(graphs.size(), std::memory_order_relaxed);
		}

		{
			std::unique_lock gl{ g->stateLock };
			static const std::vector<std::string> kNoPreservedBones;
			g->SetPosePolicy(PoseMode::kOverride, 1.0f);
			g->SetPreserveBones(kNoPreservedBones);
			g->SetBoneMask({});
			g->SetAnimation(std::move(a_skeleton), std::move(a_anim), std::move(a_source));
		}
		return true;
	}

	bool GraphManager::StopAnimation(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}

		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		if (iter->second->playbackSession) {
			REX::WARN("[Anim] Stop: actor {:X} is in a synchronized playback session — use StopScene", a_actor->formID);
			return false;
		}
		iter->second->BeginFadeOut();
		return true;
	}

	void GraphManager::RemoveFadedGraph(RE::TESObjectREFR* a_refr)
	{
		std::unique_lock l{ stateLock };
		auto iter = graphs.find(a_refr);
		if (iter == graphs.end()) {
			return;
		}
		auto g = iter->second;  // keep alive past the erase
		bool faded = false;
		bool sceneSet = false;
		bool fadedOut = false;
		{
			std::scoped_lock gl{ g->stateLock };
			sceneSet = (g->playbackSession != nullptr);
			const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			fadedOut = g->IsFadedOut() || g->IsStranded(now);  // stranded = the fade can never elapse (Sample stopped)
			faded = !sceneSet && fadedOut;  // replayed meanwhile? keep it
			if (!faded) {
				g->removalQueued = false;  // declined — let the hook (or the sweep) queue again if it re-qualifies
			}
		}
		if (faded) {
			graphs.erase(iter);
			graphCount.store(graphs.size(), std::memory_order_relaxed);
			REX::DEBUG("[Anim] fade-removal DONE — actor {:X} graph removed (graphs now {})",
				a_refr ? a_refr->formID : 0, graphs.size());
		} else {
			// DIAG: removal task ran but DECLINED to remove — this is the "never returns to vanilla" smoking gun.
			REX::DEBUG("[Anim] fade-removal KEPT — actor {:X} (session={} fadedOut={})",
				a_refr ? a_refr->formID : 0, sceneSet, fadedOut);
		}
	}

	bool GraphManager::PlaySynchronized(const std::vector<RE::Actor*>& a_actors, const PlaybackPlan& a_plan, int32_t a_startSegment,
		PlaybackId* a_outPlaybackId, PlaybackId a_expectedPlayback, PlaybackSinkId a_sinkId,
		bool a_strictTimedMarks)
	{
		if (a_outPlaybackId) {
			*a_outPlaybackId = 0;
		}
		if (!_origAnimGraphUpdate) {
			REX::ERROR("[Anim] synchronized playback refused: update hook is not installed");
			return false;
		}
		if (!ValidatePlaybackPlanArgs(a_actors, a_plan, a_startSegment)) {
			return false;
		}
		if (a_sinkId != 0 && !GetPlaybackSink(a_sinkId)) {
			REX::ERROR("[Anim] synchronized playback refused: playback sink {} is not registered", a_sinkId);
			return false;
		}

		// Load every clip of every segment up front; a null result means one failed, so refuse a partial session.
		// Preloading is what makes segment switches on the job threads IO-free.
		auto scene = BuildPlaybackFromPlan(a_actors, a_plan, a_startSegment);
		if (!scene) {
			return false;
		}
		if (a_strictTimedMarks) {
			for (std::size_t stageIndex = 0; stageIndex < scene->stages.size(); ++stageIndex) {
				const auto& stage = scene->stages[stageIndex];
				if (const auto* mark = FirstInvalidStrictTimedMark(stage.marks, stage.duration)) {
					REX::ERROR("[Anim] synchronized playback refused: strict timed mark '{}' at {:.3f}s is at/past segment {} duration {:.3f}s",
						mark->token, MarkTime(*mark, stage.duration), stageIndex, stage.duration);
					return false;
				}
			}
		}
		scene->playbackId = _nextPlaybackId.fetch_add(1, std::memory_order_relaxed);
		if (scene->playbackId == 0) {
			scene->playbackId = _nextPlaybackId.fetch_add(1, std::memory_order_relaxed);
		}
		scene->worldEpoch = _worldEpoch.load(std::memory_order_acquire);
		scene->playbackSinkId = a_sinkId;

		const auto startSegment = static_cast<uint32_t>(a_startSegment);
		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock l{ stateLock };

			// Admission is checked for the full cast before any graph mutation. A synchronized-playback owner may
			// replace only the exact playback it names; a new owner never silently clobbers a playback session.
			std::vector<PlaybackSession*> replacements;
			for (auto* actor : a_actors) {
				auto iter = graphs.find(actor);
				PlaybackClaim claim;
				PlaybackSession* currentScene = nullptr;
				if (iter != graphs.end()) {
					std::scoped_lock gl{ iter->second->stateLock };
					currentScene = iter->second->playbackSession;
					if (currentScene) {
						claim = { PlaybackOccupant::kStaged, currentScene->playbackId, currentScene->playbackSinkId };
					} else {
						claim.occupant = PlaybackOccupant::kStandalone;
					}
				}
				const auto admission = EvaluatePlaybackAdmission(claim, a_expectedPlayback, a_sinkId);
				if (!admission.accepted) {
					if (admission.reason == PlaybackAdmissionReason::kExpectedMissing) {
						REX::WARN("[Anim] synchronized playback refused: expected playback {} is not present on actor {:X}", a_expectedPlayback, actor->formID);
					} else if (admission.reason == PlaybackAdmissionReason::kStandaloneOccupied) {
						REX::WARN("[Anim] synchronized playback refused: actor {:X} has standalone playback", actor->formID);
					} else if (admission.reason == PlaybackAdmissionReason::kOwnerMismatch) {
						REX::WARN("[Anim] synchronized playback refused: playback {} belongs to sink {}, not requesting sink {}",
							claim.playbackId, claim.sinkId, a_sinkId);
					} else {
						REX::WARN("[Anim] synchronized playback refused: actor {:X} is owned by playback {} (expected {})",
							actor->formID, claim.playbackId, a_expectedPlayback);
					}
					return false;
				}
				if (admission.replace && currentScene &&
					std::find(replacements.begin(), replacements.end(), currentScene) == replacements.end()) {
					replacements.push_back(currentScene);
				}
			}
			// Replacement reuses these actors' Graph objects. Preserve the currently displayed
			// pose and its driven-joint set before StopPlaybackLocked invalidates the old binding.
			for (auto* actor : a_actors) {
				auto iter = graphs.find(actor);
				if (iter == graphs.end() || !iter->second) {
					continue;
				}
				std::scoped_lock gl{ iter->second->stateLock };
				if (iter->second->playbackSession &&
					std::find(replacements.begin(), replacements.end(), iter->second->playbackSession) != replacements.end()) {
					iter->second->PrepareBlendSource();
				}
			}
			for (auto* replacement : replacements) {
				REX::DEBUG("[Anim] replacing expected playback {}", replacement->playbackId);
				StopPlaybackLocked(replacement, deferred);
			}

			for (size_t i = 0; i < a_actors.size(); i++) {
				const auto& startSlot = scene->stages[startSegment].participants[i];
				auto& slot = graphs[a_actors[i]];
				if (!slot) {
					slot = std::make_shared<Graph>();
					slot->target.reset(a_actors[i]);
				}
				{
					std::scoped_lock gl{ slot->stateLock };
					static const std::vector<std::string> kNoPreservedBones;
					const PoseMode poseMode = scene->stages[startSegment].poseModes[i];
					const float poseWeight = scene->stages[startSegment].poseWeights[i];
					const std::string roleName = a_plan.roleNames.empty() ? std::string{} : a_plan.roleNames[i];
					slot->SetPosePolicy(poseMode, poseWeight, roleName);
					slot->SetPreserveBones(a_plan.preserveBones.empty() ? kNoPreservedBones : a_plan.preserveBones[i]);
					slot->SetBoneMask(scene->stages[startSegment].masks[i]);
					slot->SetContactPose(scene->stages[startSegment].contactPose[i]);
					slot->SetAnimation(startSlot.skeleton, startSlot.anim, startSlot.file);
					slot->blendDuration = scene->stages[startSegment].blendIn;
					slot->playbackSession = scene.get();
					slot->participantIndex = static_cast<int>(i);
					slot->scenePlacement = scene->stages[startSegment].placements[i];
					slot->appliedStage = startSegment;
					slot->sceneFrames = 0;    // restart the HoldAnchoredParticipant re-assert cadence for this session
					slot->hasAnchor = false;  // the session drives positioning; drop any solo SetAnchor
					slot->anchorRevision++;
					slot->syncGroup.reset();  // the session clock supersedes any Sync group

				}
				scene->participants.push_back(slot);
			}
			playbackSessions.push_back(scene);
			graphCount.store(graphs.size(), std::memory_order_relaxed);
		}
		FlushDeferredTasks(deferred);  // old-session teardown follow-ups, queued now that stateLock is released

		// Initial placement, dispatched to the GAME THREAD per participant (charController teleports + AI/flag writes
		// don't take from the Papyrus thread that calls PlaySynchronized). Per anchored participant: move the actor
		// FOR REAL to its placement (SetPosition updateCharController=true moves the havok CAPSULE — the position the
		// body-skin render cull reads), set heading, and set Actor::boolFlags2 kAnimationDriven so the AI keeps the
		// anim running but stops walking the actor back to its post (re-asserted each frame by HoldAnchoredParticipant,
		// cleared in StopPlaybackLocked). See OSF RE module gameplay.actor_animation_driven.
		for (size_t i = 0; scene->anchored && i < scene->participants.size(); i++) {
			const auto& pl = scene->stages[startSegment].placements[i];
			auto* refr = scene->participants[i]->target.get();
			if (!refr) {
				continue;
			}
			const RE::NiPoint3 worldPos = PlacementToWorld(scene->anchorPos, scene->anchorHeading, pl);
			const float heading = scene->anchorHeading + pl.heading;
			// The PLAYER is never made animation-driven. Can get them stuck in a bad state
			const bool isPlayer = refr == static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton());
			RE::NiPointer<RE::Actor> keepAlive{ static_cast<RE::Actor*>(refr) };
			const PlaybackId playbackId = scene->playbackId;
			const std::uint64_t worldEpoch = scene->worldEpoch;
			SFSE::GetTaskInterface()->AddTask([keepAlive, worldPos, heading, isPlayer, playbackId, worldEpoch]() {
				auto& gm = GetSingleton();
				if (gm._worldEpoch.load(std::memory_order_acquire) != worldEpoch) {
					return;
				}
				{
					std::shared_lock l{ gm.stateLock };
					const auto it = gm.graphs.find(keepAlive.get());
					if (it == gm.graphs.end()) {
						return;
					}
					std::scoped_lock gl{ it->second->stateLock };
					if (!it->second->playbackSession || it->second->playbackSession->playbackId != playbackId) {
						return;
					}
				}
				keepAlive->SetPosition(worldPos, true);  // move the havok capsule to the anchor (the cull's position input)
				// Save window: keep the bit clear while a save serializes actor state (the hold cadence sets it once the window closes).
				if (!isPlayer && !GetSingleton()._saveWindow.load(std::memory_order_relaxed)) {
					keepAlive->boolFlags2.set(RE::Actor::BOOL_FLAGS2::kAnimationDriven);  // anchored NPC: AI runs the anim but won't walk it back
				}
				if (auto* transforms = RE::TransformService::GetSingleton()) {
					transforms->SetHeadingZ(keepAlive.get(), heading);
				}
			});
			if (pl.x != 0.0f || pl.y != 0.0f || pl.z != 0.0f || pl.heading != 0.0f) {
				REX::TRACE("[Anim] placement[{}]: local ({:+.2f},{:+.2f},{:+.2f}) heading {:+.2f} -> world ({:.1f},{:.1f},{:.1f})",
					i, pl.x, pl.y, pl.z, pl.heading, worldPos.x, worldPos.y, worldPos.z);
			}
		}

		REX::DEBUG("[Anim] started synchronized playback session: {} actors, {} segment(s) starting at {} (duration {:.2f}s, timer {:.1f}s, loops {}), "
			"anchored at ({:.1f},{:.1f},{:.1f}) heading {:.2f}",
			a_actors.size(), scene->stages.size(), startSegment, scene->duration,
			scene->stages[startSegment].timer, scene->stages[startSegment].loops,
			scene->anchorPos.x, scene->anchorPos.y, scene->anchorPos.z, scene->anchorHeading);
		if (a_outPlaybackId) {
			*a_outPlaybackId = scene->playbackId;
		}
		return true;
	}

	bool GraphManager::SetSceneStage(RE::Actor* a_actor, int32_t a_stage, PlaybackId a_expectedPlayback)
	{
		if (!a_actor) {
			return false;
		}

		// shared stateLock keeps the playback session alive (the collection mutates only under unique);
		// the segment jump itself is guarded by the session's own lock
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		PlaybackSession* scene = nullptr;
		{
			std::scoped_lock gl{ iter->second->stateLock };
			scene = iter->second->playbackSession;
		}
		if (!scene) {
			REX::WARN("[Anim] SetSceneStage: actor {:X} is not in a synchronized playback session", a_actor->formID);
			return false;
		}
		if ((a_expectedPlayback && scene->playbackId != a_expectedPlayback) ||
			(!a_expectedPlayback && scene->playbackSinkId != 0)) {
			REX::WARN("[Anim] SetSceneStage refused: playback ownership does not match");
			return false;
		}
		if (!scene->SetSegment(a_stage)) {
			REX::WARN("[Anim] SetSceneStage: segment {} out of range ({} segments)", a_stage, scene->stages.size());
			return false;
		}
		REX::DEBUG("[Anim] playback session jumped to segment {}", a_stage);
		return true;
	}

	bool GraphManager::SetSceneTime(RE::Actor* a_actor, float a_time, PlaybackId a_expectedPlayback)
	{
		if (!a_actor || !std::isfinite(a_time)) {
			return false;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		PlaybackSession* scene = nullptr;
		{
			std::scoped_lock gl{ iter->second->stateLock };
			scene = iter->second->playbackSession;
		}
		return scene && ((a_expectedPlayback && scene->playbackId == a_expectedPlayback) ||
			(!a_expectedPlayback && scene->playbackSinkId == 0)) && scene->Seek(a_time);
	}

	std::optional<GraphManager::SynchronizedPlaybackState> GraphManager::GetScenePlayback(
		RE::Actor* a_actor, PlaybackId a_expectedPlayback)
	{
		if (!a_actor) {
			return std::nullopt;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return std::nullopt;
		}
		PlaybackSession* scene = nullptr;
		{
			std::scoped_lock gl{ iter->second->stateLock };
			scene = iter->second->playbackSession;
		}
		if (!scene || (a_expectedPlayback && scene->playbackId != a_expectedPlayback)) {
			return std::nullopt;
		}
		const auto snapshot = scene->GetPlaybackSnapshot();
		return SynchronizedPlaybackState{
			snapshot.time, snapshot.duration, snapshot.speed, static_cast<int32_t>(snapshot.stage), scene->playbackId
		};
	}

	bool GraphManager::StopScene(RE::Actor* a_actor, PlaybackId a_expectedPlayback)
	{
		if (!a_actor) {
			return false;
		}

		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock l{ stateLock };
			auto iter = graphs.find(a_actor);
			if (iter == graphs.end() || !iter->second->playbackSession ||
				(a_expectedPlayback && iter->second->playbackSession->playbackId != a_expectedPlayback) ||
				(!a_expectedPlayback && iter->second->playbackSession->playbackSinkId != 0)) {
				return false;
			}
			StopPlaybackLocked(iter->second->playbackSession, deferred);
		}
		FlushDeferredTasks(deferred);
		return true;
	}

	bool GraphManager::StopSceneImmediate(RE::Actor* a_actor, PlaybackId a_expectedPlayback)
	{
		if (!a_actor || a_expectedPlayback == 0) return false;
		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock l{ stateLock };
			auto iter = graphs.find(a_actor);
			if (iter == graphs.end() || !iter->second->playbackSession ||
				iter->second->playbackSession->playbackId != a_expectedPlayback) {
				return false;
			}
			PlaybackSession* scene = iter->second->playbackSession;
			std::vector<RE::TESObjectREFR*> participants;
			for (const auto& graph : scene->participants) {
				if (graph && graph->target) participants.push_back(graph->target.get());
			}
			StopPlaybackLocked(scene, deferred);
			for (auto* participant : participants) {
				const auto found = graphs.find(participant);
				if (found != graphs.end()) {
					auto graph = found->second;
					std::scoped_lock gl{ graph->stateLock };
					if (!graph->playbackSession) graphs.erase(found);
				}
			}
			graphCount.store(graphs.size(), std::memory_order_relaxed);
		}
		FlushDeferredTasks(deferred);
		return true;
	}

	void GraphManager::StopPlaybackByPtr(PlaybackSession* a_session)
	{
		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock l{ stateLock };
			StopPlaybackLocked(a_session, deferred);  // no-op if the playback was already stopped
		}
		FlushDeferredTasks(deferred);
	}

	void GraphManager::StopAll(const char* a_reason)
	{
		// Invalidate every queued task from the discarded world before any service/graph state is
		// cleared. Tasks already in the SFSE queue observe the new epoch and no-op.
		_worldEpoch.fetch_add(1, std::memory_order_acq_rel);
		// A pending save window must not outlive the world it was opened for (a load can begin mid-save-op).
		_saveWindow.store(false, std::memory_order_relaxed);

		// Release the player input lock + the persistent AI-driven flag (possibly left by older builds or free cam).
		// The AI-driven flag is serialized into saves, so it MUST be cleared on every load even when this process holds no in-memory lock;
		// the input-disable layer is non-persistent.
		Camera::CameraService::GetSingleton().OnStopAll();
		Player::PlayerInputLockService::GetSingleton().OnStopAll();
		// Drop any live director-input grant too (its scene is about to be cleared below).
		Input::InputService::GetSingleton().OnStopAll();

		// Release any held/pending screen fade before the load (the stay-faded latch crashes the load path).
		UI::FadeService::GetSingleton().OnStopAll();
		// Clear any subtitle still in the box so it can't bleed into the loaded world.
		UI::Subtitle::OnStopAll();
		// Cut every live cue sound, a loaded save shouldn't have last-world sounds ringing over it.
		Audio::SoundService::GetSingleton().StopAll();

		// Drop every playback owner's world-bound handles. Copy first so a clear callback may
		// unregister itself without re-entering the sink registry lock.
		std::vector<PlaybackClearHandler> clearHandlers;
		{
			std::shared_lock sinks{ _sinkLock };
			for (const auto& [id, sink] : _playbackSinks) {
				(void)id;
				if (sink.clear) clearHandlers.push_back(sink.clear);
			}
		}
		for (auto& clear : clearHandlers) {
			clear();
		}

		std::unique_lock l{ stateLock };
		if (playbackSessions.empty() && graphs.empty()) {
			return;
		}
		REX::INFO("[Anim] StopAll ({}): dropping {} playback session(s) + {} graph(s)",
			a_reason ? a_reason : "?", playbackSessions.size(), graphs.size());

		// We do NOT fade or revert movement here: on a save load the engine has already reset every actor to the saved state, and our graphs are anchored in the world that was just discarded.
		// Stale graphs/playback sessions just stop existing; the engine's rig refresh owns the pose.
		playbackSessions.clear();
		graphs.clear();
		graphCount.store(0, std::memory_order_relaxed);
	}

	std::vector<RE::NiPointer<RE::Actor>> GraphManager::CollectAnchoredNpcParticipants()
	{
		std::vector<RE::NiPointer<RE::Actor>> out;
		auto* player = static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton());
		std::shared_lock l{ stateLock };
		for (const auto& s : playbackSessions) {
			if (!s || !s->anchored) {
				continue;  // only anchored playback sessions engage kAnimationDriven
			}
			for (const auto& p : s->participants) {
				if (p && p->target && p->target.get() != player) {
					out.emplace_back(static_cast<RE::Actor*>(p->target.get()));
				}
			}
		}
		return out;
	}

	void GraphManager::OnSaveBegin()
	{
		// Runs on the game thread (SaveLoadEvent kBegin fires there), the same thread every other
		// boolFlags2 write lands on via the task queue — direct writes are ordered with them.
		if (_saveWindow.exchange(true, std::memory_order_relaxed)) {
			return;  // window already open (overlapping save ops) — the first opener did the strip
		}
		const auto participants = CollectAnchoredNpcParticipants();
		for (const auto& a : participants) {
			a->boolFlags2.reset(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
		}
		if (!participants.empty()) {
			REX::DEBUG("[Anim] save begin — animation-driven bit stripped from {} anchored participant(s) so the save can't bake it in",
				participants.size());
		}
	}

	void GraphManager::OnSaveEnd()
	{
		if (!_saveWindow.exchange(false, std::memory_order_relaxed)) {
			return;  // no window open (plain load events land here too) — nothing to restore
		}
		// Re-assert immediately rather than waiting for the hold cadence (~0.075s): the AI re-asserts
		// motion every update, so the sooner the bit is back the less the capsule can drift.
		const auto participants = CollectAnchoredNpcParticipants();
		for (const auto& a : participants) {
			a->boolFlags2.set(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
		}
		if (!participants.empty()) {
			REX::DEBUG("[Anim] save end — animation-driven bit re-asserted on {} anchored participant(s)", participants.size());
		}
	}

	int32_t GraphManager::GetSceneStage(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return -1;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return -1;
		}
		PlaybackSession* scene = nullptr;
		{
			std::scoped_lock gl{ iter->second->stateLock };
			scene = iter->second->playbackSession;
		}
		// Report the session's authoritative current segment (set immediately on SetSegment / auto-advance), not the graph's legacy appliedStage field (lags one sample).
		// stateLock (shared) keeps the playback session alive while we read it.
		return scene ? static_cast<int32_t>(scene->CurrentSegment()) : -1;
	}

	bool GraphManager::IsPlaying(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		return !iter->second->IsFadedOut();
	}

	std::string GraphManager::GetCurrentAnimation(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return {};
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return {};
		}
		std::scoped_lock gl{ iter->second->stateLock };
		if (iter->second->IsFadedOut()) {
			return {};
		}
		return iter->second->currentFile;
	}

	bool GraphManager::SetAnimationTime(RE::Actor* a_actor, float a_time)
	{
		if (!a_actor || !std::isfinite(a_time)) return false;
		std::shared_lock lock{ stateLock };
		const auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) return false;
		std::scoped_lock graphLock{ iter->second->stateLock };
		auto& graph = *iter->second;
		if (graph.playbackSession || !graph.syncGroup || !graph.anim || !graph.anim->data) return false;
		const float duration = graph.anim->data->duration();
		const float time = std::clamp(a_time, 0.0f, (std::max)(duration - 0.0001f, 0.0f));
		std::scoped_lock groupLock{ graph.syncGroup->lock };
		graph.syncGroup->clock.time = time;
		graph.localTime = time;
		return true;
	}

	std::optional<GraphManager::AnimationPlayback> GraphManager::GetAnimationPlayback(RE::Actor* a_actor)
	{
		if (!a_actor) return std::nullopt;
		std::shared_lock lock{ stateLock };
		const auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) return std::nullopt;
		std::scoped_lock graphLock{ iter->second->stateLock };
		const auto& graph = *iter->second;
		if (graph.playbackSession || !graph.syncGroup || !graph.anim || !graph.anim->data || graph.IsFadedOut()) {
			return std::nullopt;
		}
		return AnimationPlayback{
			.time = graph.localTime,
			.duration = graph.anim->data->duration(),
			.speed = graph.syncGroup->speed.load(std::memory_order_relaxed)
		};
	}

	bool GraphManager::SetAnimationHoldAtEnd(RE::Actor* a_actor, bool a_hold)
	{
		if (!a_actor) return false;
		std::shared_lock lock{ stateLock };
		const auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) return false;
		std::scoped_lock graphLock{ iter->second->stateLock };
		if (iter->second->playbackSession) return false;
		iter->second->holdClipAtEnd = a_hold;
		return true;
	}

	bool GraphManager::SetSpeed(RE::Actor* a_actor, float a_speed)
	{
		if (!a_actor) {
			return false;
		}
		if (!std::isfinite(a_speed)) {
			REX::WARN("[Anim] SetSpeed: rejected non-finite speed for actor {:X}", a_actor->formID);
			return false;
		}
		const float speed = std::clamp(a_speed, 0.0f, 100.0f);  // 1.0 = authored, 0 = freeze
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		// Synchronized-playback participants share the session clock (PlaybackSession::Advance reads speed);
		// a synced solo graph shares its group clock (which advances by the group speed, not the owner's); an unsynced solo graph advances by Graph::speed.
		if (iter->second->playbackSession) {
			iter->second->playbackSession->speed = speed;
		} else if (iter->second->syncGroup) {  // null only in the brief window before SetAnimation runs
			iter->second->syncGroup->speed.store(speed, std::memory_order_relaxed);
		}
		return true;
	}

	float GraphManager::GetSpeed(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0.0f;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return 0.0f;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		if (iter->second->playbackSession) {
			return iter->second->playbackSession->speed.load(std::memory_order_relaxed);
		}
		if (iter->second->syncGroup) {
			return iter->second->syncGroup->speed.load(std::memory_order_relaxed);
		}
		return 1.0f;  // graph created but not yet started (pre-SetAnimation), authored speed
	}

	bool GraphManager::SetAnchor(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode)
	{
		if (!a_actor) {
			return false;
		}
		if (!std::isfinite(a_x) || !std::isfinite(a_y) || !std::isfinite(a_z) || !std::isfinite(a_headingDeg)) {
			REX::WARN("[Anim] SetAnchor: rejected non-finite transform for actor {:X}", a_actor->formID);
			return false;
		}
		const float heading = a_headingDeg * Util::kDegToRadF;
		RootMode mode = RootMode::kPin;
		switch (a_rootMode) {
		case 0:
			break;
		case 1:
			mode = RootMode::kFollow;
			break;
		case 2:
			// Pre-1.1 callers used 2=follow. Keep the alias through the 1.x ABI instead of
			// casting it to an enum value that no longer exists.
			mode = RootMode::kFollow;
			REX::WARN("[Anim] SetAnchor: rootMode 2 is deprecated; use 1 for follow");
			break;
		default:
			REX::WARN("[Anim] SetAnchor: rejected unknown rootMode {}", a_rootMode);
			return false;
		}
		std::shared_ptr<Graph> anchoredGraph;
		std::uint64_t anchorRevision = 0;
		{
			std::shared_lock l{ stateLock };
			auto iter = graphs.find(a_actor);
			if (iter == graphs.end()) {
				REX::WARN("[Anim] SetAnchor: actor {:X} has no live graph", a_actor->formID);
				return false;
			}
			std::scoped_lock gl{ iter->second->stateLock };
			if (iter->second->playbackSession) {
				REX::WARN("[Anim] SetAnchor: actor {:X} is a synchronized-playback participant — anchoring is session-driven (use the session's placement offsets)",
					a_actor->formID);
				return false;
			}
			iter->second->anchorPos = { a_x, a_y, a_z };
			iter->second->rootMode = mode;
			iter->second->hasAnchor = true;
			anchorRevision = ++iter->second->anchorRevision;
			anchoredGraph = iter->second;
		}
		//Move capsule to anchor too via actor->SetPosition
		{
			RE::NiPointer<RE::Actor> keepAlive{ a_actor };
			const RE::NiPoint3 anchor{ a_x, a_y, a_z };
			const auto worldEpoch = _worldEpoch.load(std::memory_order_acquire);
			SFSE::GetTaskInterface()->AddTask([keepAlive, anchoredGraph, anchor, heading, anchorRevision, worldEpoch]() {
				auto& gm = GetSingleton();
				if (gm._worldEpoch.load(std::memory_order_acquire) != worldEpoch) {
					return;
				}
				{
					std::shared_lock l{ gm.stateLock };
					const auto it = gm.graphs.find(keepAlive.get());
					if (it == gm.graphs.end() || it->second != anchoredGraph) {
						return;
					}
					std::scoped_lock gl{ anchoredGraph->stateLock };
					if (anchoredGraph->playbackSession || !anchoredGraph->hasAnchor ||
						anchoredGraph->anchorRevision != anchorRevision) {
						return;
					}
				}
				keepAlive->SetPosition(anchor, true);
				if (auto* transforms = RE::TransformService::GetSingleton()) {
					transforms->SetHeadingZ(keepAlive.get(), heading);
				}
			});
		}
		REX::TRACE("[Anim] SetAnchor: actor {:X} -> ({:.1f},{:.1f},{:.1f}) heading {:.2f} rootMode {}",
			a_actor->formID, a_x, a_y, a_z, heading, a_rootMode);
		return true;
	}

	bool GraphManager::ClearAnchor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		iter->second->hasAnchor = false;
		iter->second->anchorRevision++;
		return true;
	}

	bool GraphManager::Sync(const std::vector<RE::Actor*>& a_actors, bool a_anchor)
	{
		if (a_actors.size() < 2) {
			REX::WARN("[Anim] Sync: need >= 2 actors (got {})", a_actors.size());
			return false;
		}

		// Auto-anchor (the default): promote the independently played solo graphs into ONE anchored, clock-shared playback session at actor[0]'s current transform.
		// Placements are left empty (same-spot overlap), so each paired clip's baked root offset arranges the actors about one shared origin+heading
		// Without this, Play+Sync leaves every actor animating about its own (far-apart) world position, which is the "actors stand apart" symptom.
		if (a_anchor) {
			std::vector<RE::Actor*> sceneActors;
			std::vector<std::string> sceneFiles;
			float groupSpeed = 1.0f;
			bool gotSpeed = false;
			{
				// Collect the qualifying solo graphs' clips in actor order WITHOUT mutating them, then drop stateLock before PlaySynchronized (which takes it unique).
				// Skip actors with no graph, no clip, or already in a synchronized playback session.
				std::shared_lock l{ stateLock };
				for (auto* actor : a_actors) {
					if (!actor) {
						continue;
					}
					auto iter = graphs.find(actor);
					if (iter == graphs.end()) {
						REX::DEBUG("[Anim] Sync: actor {:X} has no live graph — skipping", actor->formID);
						continue;
					}
					std::scoped_lock gl{ iter->second->stateLock };
					if (iter->second->playbackSession) {
						REX::DEBUG("[Anim] Sync: actor {:X} is already a synchronized-playback participant — skipping", actor->formID);
						continue;
					}
					if (iter->second->currentFile.empty()) {
						REX::DEBUG("[Anim] Sync: actor {:X} has no clip to anchor — skipping", actor->formID);
						continue;
					}
					if (!gotSpeed) {  // carry a pre-Sync SetSpeed over to the playback-session clock
						groupSpeed = iter->second->syncGroup->speed;
						gotSpeed = true;
					}
					sceneActors.push_back(actor);
					sceneFiles.push_back(iter->second->currentFile);
				}
			}
			if (sceneActors.size() < 2) {
				REX::WARN("[Anim] Sync: fewer than 2 anchorable solo graphs ({}) — nothing synced "
						  "(pass abAnchor=false for in-place clock sync)", sceneActors.size());
				return false;
			}
			// One ad-hoc anchored segment; empty placements = same-spot overlap at actor[0].
			PlaybackPlan plan;
			plan.SetWorldPlacementMode(WorldPlacementMode::kAnchorAndPin);
			plan.speed = groupSpeed;
			PlaybackPlan::Segment stage;
			stage.files = std::move(sceneFiles);
			plan.stages.push_back(std::move(stage));
			if (!PlaySynchronized(sceneActors, plan, 0)) {
				REX::WARN("[Anim] Sync: anchored promotion failed; actors left unsynced");
				return false;
			}
			REX::DEBUG("[Anim] Sync: {} graphs promoted to one anchored playback session (same-spot overlap at actor {:X})",
				sceneActors.size(), sceneActors.front()->formID);
			return true;
		}

		// Opt-out (abAnchor=false): legacy clock-merge only, frame-lock the solo graphs on one shared clock, leaving each actor at its own world position.
		std::shared_lock l{ stateLock };

		// First pass: collect the qualifying solo graphs WITHOUT mutating them, so a group that can't reach 2 members leaves nothing half-synced 
		// (a stray 1-member assignment strands that graph on a private clock and snaps it to frame 0 even though we report failure). 
		// Holding stateLock shared across both passes blocks playback-session (un)assignment, which takes stateLock unique, so the session check from pass 1 stays valid through pass 2.
		std::vector<std::shared_ptr<Graph>> targets;
		for (auto* actor : a_actors) {
			if (!actor) {
				continue;
			}
			auto iter = graphs.find(actor);
			if (iter == graphs.end()) {
				REX::DEBUG("[Anim] Sync: actor {:X} has no live graph — skipping", actor->formID);
				continue;
			}
			std::scoped_lock gl{ iter->second->stateLock };
			if (iter->second->playbackSession) {
				REX::DEBUG("[Anim] Sync: actor {:X} is a synchronized-playback participant (already clock-synced) — skipping", actor->formID);
				continue;
			}
			targets.push_back(iter->second);
		}
		if (targets.size() < 2) {
			REX::WARN("[Anim] Sync: fewer than 2 playable solo graphs to sync ({})", targets.size());
			return false;
		}
		// Second pass: one shared clock for the whole group; each graph reads it under its own lock (the group lock is a leaf). 
		// Graphs jump to the clock's t=0 on their next sample, so the group snaps into phase together. 
		// Seed the group speed from the first member so a pre-Sync SetSpeed carries over.
		auto group = std::make_shared<SyncGroup>();
		{
			std::scoped_lock gl{ targets.front()->stateLock };
			if (const auto& sg = targets.front()->syncGroup) {  // null only pre-SetAnimation
				group->speed.store(sg->speed.load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
		}
		for (auto& g : targets) {
			std::scoped_lock gl{ g->stateLock };
			g->syncGroup = group;
		}
		REX::TRACE("[Anim] Sync: {} graphs frame-locked on one shared clock", targets.size());
		return true;
	}

	bool GraphManager::PlaySequence(RE::Actor* a_actor, const std::vector<std::string>& a_files, const std::vector<std::string>& a_animIds, const std::vector<int32_t>& a_loops, const std::vector<float>& a_blends, bool a_loopWhole)
	{
		if (!a_actor) {
			return false;
		}
		if (a_files.empty() || a_files.size() != a_animIds.size() || a_files.size() != a_loops.size() || a_files.size() != a_blends.size()) {
			REX::WARN("[Anim] PlaySequence: files/animIds/loops/blends must be non-empty and equal length ({}/{}/{}/{})",
				a_files.size(), a_animIds.size(), a_loops.size(), a_blends.size());
			return false;
		}
		PlaybackPlan plan;
		plan.loopWhole = a_loopWhole;
		plan.SetWorldPlacementMode(WorldPlacementMode::kFollowActor);
		for (size_t i = 0; i < a_files.size(); i++) {
			PlaybackPlan::Segment stage;
			stage.files = { a_files[i] };
			stage.animIds = { a_animIds[i] };
			stage.loops = a_loops[i];    // loop-count advance; <= 0 = hold this phase
			stage.timer = 0.0f;
			stage.blendIn = a_blends[i];
			plan.stages.push_back(std::move(stage));
		}
		const std::vector<RE::Actor*> actors{ a_actor };
		return PlaySynchronized(actors, plan, 0);
	}

	void GraphManager::StopPlaybackLocked(PlaybackSession* a_session, std::vector<std::function<void()>>& a_deferred)
	{
		// Detach every participant of this playback and drop their graphs; the engine's own rig refresh restores the game pose next frame.
		auto sceneIter = std::find_if(playbackSessions.begin(), playbackSessions.end(),
			[&](const std::shared_ptr<PlaybackSession>& s) { return s.get() == a_session; });
		if (sceneIter == playbackSessions.end()) {
			return;
		}

		const bool anchored = (*sceneIter)->anchored;
		const bool restoreTransforms = (*sceneIter)->restoreParticipantTransforms;
		const auto originalTransforms = (*sceneIter)->originalTransforms;

		auto& participants = (*sceneIter)->participants;
		for (std::size_t participantIndex = 0; participantIndex < participants.size(); participantIndex++) {
			auto& p = participants[participantIndex];
			std::uint64_t stoppedRevision = 0;
			{
				// graph stays in the map and fades to the engine pose; the update hook queues its removal once the ramp elapses
				std::scoped_lock gl{ p->stateLock };
				p->DetachAndFadeOut();
				stoppedRevision = p->playbackRevision;
			}
			// DIAG: confirm the per-participant detach ran and the playback session was cleared, so we can tell a "never removed"
			// graph apart from a "never detached" one.
			REX::DEBUG("[Anim] session-end detach: actor {:X} — playback session cleared, fade-out begun (blendDur {:.2f}s)",
				p->target ? p->target->formID : 0, p->blendDuration);
			// Revert the animation-driven movement switch from PlaySynchronized (anchored playback only). Game-thread only.
			// The direct bit reset is the PROVEN lever; MovementControllerNPC::SetMotionDriven (REL 135315) is
			// mis-bound on this build (no-op at best, unknown code at worst — RE module
			// gameplay.actor_animation_driven) and is deliberately NOT called.
			if (anchored && p->target) {
				// Participants are always actors (PlaySynchronized takes Actor*).
				RE::NiPointer<RE::Actor> keepAlive{ static_cast<RE::Actor*>(p->target.get()) };
				const auto stoppedGraph = p;
				const PlaybackId stoppedPlayback = (*sceneIter)->playbackId;
				const std::uint64_t worldEpoch = (*sceneIter)->worldEpoch;
				const bool restoreTransform = restoreTransforms && participantIndex < originalTransforms.size();
				const auto originalTransform = restoreTransform ?
				                                   originalTransforms[participantIndex] :
				                                   std::pair<RE::NiPoint3, float>{};
				a_deferred.emplace_back([keepAlive, stoppedGraph, stoppedPlayback, stoppedRevision, worldEpoch, restoreTransform, originalTransform]() {
					auto& gm = GetSingleton();
					if (gm._worldEpoch.load(std::memory_order_acquire) != worldEpoch) {
						return;
					}
					bool applyTransformRestore = restoreTransform;
					{
						std::shared_lock l{ gm.stateLock };
						if (const auto it = gm.graphs.find(keepAlive.get()); it != gm.graphs.end()) {
							std::scoped_lock gl{ it->second->stateLock };
							if (it->second->playbackSession && it->second->playbackSession->playbackId != stoppedPlayback) {
								return;  // a replacement playback session now owns the actor's movement mode
							}
							if (applyTransformRestore &&
								(it->second != stoppedGraph || it->second->playbackRevision != stoppedRevision)) {
								// A replacement solo playback owns the actor. It does not need the
								// old session's movement flag, but must not receive its stale teleport.
								applyTransformRestore = false;
							}
						}
					}
					keepAlive->boolFlags2.reset(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
					if (applyTransformRestore) {
						keepAlive->SetPosition(originalTransform.first, true);
						if (auto* transforms = RE::TransformService::GetSingleton()) {
							transforms->SetHeadingZ(keepAlive.get(), originalTransform.second);
						}
						REX::TRACE("[Anim] session-end restore: actor {:X} -> ({:.1f},{:.1f},{:.1f}) heading {:.2f}",
							keepAlive->formID, originalTransform.first.x, originalTransform.first.y,
							originalTransform.first.z, originalTransform.second);
					}
				});
			}
		}
		playbackSessions.erase(sceneIter);

		REX::DEBUG("[Anim] stopped synchronized playback session");
	}

	void GraphManager::QueueAutoEndIfFinished(Graph& a_graph, std::vector<std::function<void()>>& a_deferred)
	{
		// The last timed/loop-counted segment ran out. Defer the stop to the game thread (the StopScene compatibility API needs
		// stateLock unique; the hook holds it shared). The endQueued load here is a cheap early-out; the
		// authoritative once-only gate is the exchange inside QueuePlaybackEndDeferred. Capturing the
		// shared_ptr keeps the PlaybackSession alive + ABA-safe.
		if (!a_graph.playbackSession || !a_graph.playbackSession->ended.load(std::memory_order_relaxed) ||
			a_graph.playbackSession->endQueued.load(std::memory_order_relaxed)) {
			return;
		}
		std::shared_ptr<PlaybackSession> keepAlive;
		for (const auto& s : playbackSessions) {
			if (s.get() == a_graph.playbackSession) {
				keepAlive = s;
				break;
			}
		}
		if (!keepAlive) {
			REX::ERROR("[Anim] playback-session end: session not found in the live list — cannot stop (this should be impossible)");
			return;
		}
		REX::DEBUG("[Anim] playback session finished its final segment — queueing stop task (from job thread)");
		QueuePlaybackEndDeferred(std::move(keepAlive), &a_deferred);
	}

	void GraphManager::HandlePlaybackEndOnGameThread(std::shared_ptr<PlaybackSession> a_session)
	{
		if (!a_session) {
			return;
		}
		// SetSegment between the claim and now revives the playback session (clears `ended`);
		// a revived session must not be killed by this stale completion.
		if (!a_session->ended.load(std::memory_order_relaxed)) {
			REX::DEBUG("[Anim] playback-session end: session was revived meanwhile — not stopping");
			return;
		}
		if (_worldEpoch.load(std::memory_order_acquire) != a_session->worldEpoch) {
			return;
		}

		// The registered playback owner gets first refusal to advance its domain lifecycle and own teardown.
		// An unclaimed playback session stops here. No manager lock is held, so the handler is free to
		// call PlaySynchronized or the StopScene compatibility API.
		bool handled = false;
		if (const auto sink = GetPlaybackSink(a_session->playbackSinkId); sink && sink->autoEnd) {
			std::vector<RE::Actor*> actors;
			for (const auto& p : a_session->participants) {
				if (p && p->target) {
					// Synchronized-playback participants are always actors (PlaySynchronized takes Actor*).
					actors.push_back(static_cast<RE::Actor*>(p->target.get()));
				}
			}
			handled = sink->autoEnd(a_session->playbackId, actors,
				a_session->endReason.load(std::memory_order_relaxed));
		}
		if (!handled) {
			StopPlaybackByPtr(a_session.get());
		}
	}

	void GraphManager::QueuePlaybackEndDeferred(std::shared_ptr<PlaybackSession> a_session, std::vector<std::function<void()>>* a_deferred)
	{
		// Fire once: a concurrent caller that loses the exchange no-ops.
		if (!a_session || a_session->endQueued.exchange(true, std::memory_order_relaxed)) {
			return;
		}
		auto task = [keepAlive = std::move(a_session)]() {
			REX::DEBUG("[Anim] playback-session end task executing on game thread");
			GetSingleton().HandlePlaybackEndOnGameThread(std::move(keepAlive));
		};
		if (a_deferred) {
			a_deferred->emplace_back(std::move(task));  // caller holds stateLock; it flushes after release
		} else {
			SFSE::GetTaskInterface()->AddTask(std::move(task));
		}
	}

	void GraphManager::MaintenanceTick()
	{
		// Main-thread-only. FrameMaintenance reaches this through the verified BSService drain;
		// the game-wide AnimationManager report hook never enters global maintenance.
		if (graphCount.load(std::memory_order_relaxed) == 0) {
			return;
		}
		// Permanent tasks may continue while a game menu/console freezes animation reports. Do not
		// interpret that intentional pause as participant death; force a fresh grace window on resume.
		if (IsGameMenuPaused() || g_consoleOpen.load(std::memory_order_relaxed)) {
			_stallWatchdog.Pause();
			return;
		}

		// Threshold (steady-clock ms). Set well above a frame so a playable-FPS hitch can't trip it; the
		// playback session must look dead for kSceneStallMs of CONTINUOUS engine-running time after any resume grace.
		constexpr std::int64_t kSceneStallMs = 1500;  // a live session unticked this long (game running) = interrupted

		using namespace std::chrono;
		const std::int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

		// Focus-loss/loading can stop even the maintenance beat. The schedule detects that gap,
		// re-arms a grace window, then allows only a few scans per second.
		if (!_stallWatchdog.ShouldScan(now)) {
			return;
		}

		// Collect stalled playback sessions under the shared lock; act on them OUTSIDE it.
		std::vector<std::shared_ptr<PlaybackSession>> stalled;
		{
			std::shared_lock l{ stateLock };
			for (const auto& s : playbackSessions) {
				if (!s || s->ended.load(std::memory_order_relaxed) || s->endQueued.load(std::memory_order_relaxed)) {
					continue;  // already terminal / queued — leave it to the normal path
				}
				std::int64_t lastDiag = s->lastDiagnosticMs.load(std::memory_order_relaxed);
				if (now - lastDiag >= 1000 && s->lastDiagnosticMs.compare_exchange_strong(lastDiag, now, std::memory_order_relaxed)) {
					const auto diag = s->GetDiagnosticSnapshot(now);
					const std::int64_t stampMs = s->lastStampMs.load(std::memory_order_relaxed);
					const std::int64_t sampleAge = now - s->lastAdvanceMs.load(std::memory_order_relaxed);
					const std::int64_t stampAge = stampMs > 0 ? now - stampMs : -1;
					REX::TRACE("[Anim] playback-session heartbeat playback={} id='{}' segment={} time={:.3f} speed={:.2f} sampleAge={}ms stampAge={}ms owner={:X} ownerAge={}ms cast={}",
						s->playbackId, s->animId, diag.stage, diag.time, s->speed.load(std::memory_order_relaxed),
						sampleAge, stampAge, reinterpret_cast<std::uintptr_t>(diag.owner), diag.ownerAgeMs, s->participants.size());
				}
				const std::int64_t lastAdv = s->lastAdvanceMs.load(std::memory_order_relaxed);
				if (lastAdv != 0 && now - lastAdv > kSceneStallMs) {
					stalled.push_back(s);
				}
			}
		}
		for (auto& s : stalled) {
			REX::WARN("[Anim] playback session stalled {}ms (engine stopped ticking it — actor unloaded / AI-disabled / interrupted) — ending it as interrupted",
				now - s->lastAdvanceMs.load(std::memory_order_relaxed));
			s->endReason.store(PlaybackEndReason::kInterrupted, std::memory_order_relaxed);
			s->ended.store(true, std::memory_order_relaxed);
			// Claim against a concurrent normal auto-end, then complete inline: this beat is already
			// on the game thread and no manager lock is held.
			if (!s->endQueued.exchange(true, std::memory_order_relaxed)) {
				HandlePlaybackEndOnGameThread(s);
			}
		}

		// Stranded-graph sweep: a DETACHED graph whose actor the engine stopped updating can never
		// finish its fade (blendClock only advances in Sample) and so is never reclaimed by the
		// normal QueueFadeRemovalIfDone path — the map entry pins the REFR, the model hook keeps
		// stamping the frozen pose, and graphCount never returns to 0. Judge by wall clock (the
		// pause/resume grace above already filtered global pauses), collect under the shared lock,
		// remove them OUTSIDE it on this game-thread beat.
		std::vector<RE::NiPointer<RE::TESObjectREFR>> strandedGraphs;
		{
			std::shared_lock l{ stateLock };
			for (auto& [refr, g] : graphs) {
				std::scoped_lock gl{ g->stateLock };
				if (g->playbackSession || g->removalQueued) {
					continue;  // playback-session graphs are the stall watchdog's job; queued ones are already on their way out
				}
				if (g->IsStranded(now)) {
					g->removalQueued = true;
					strandedGraphs.emplace_back(g->target);
				}
			}
		}
		for (auto& refr : strandedGraphs) {
			REX::DEBUG("[Anim] stranded-graph sweep: actor {:X} stopped updating — removing",
				refr ? refr->formID : 0);
			RemoveFadedGraph(refr.get());
		}
	}

	void GraphManager::QueueTimedMarksIfFired(Graph& a_graph, std::vector<std::function<void()>>& a_deferred)
	{
		if (!a_graph.playbackSession) {
			return;
		}
		const PlaybackSinkId sinkId = a_graph.playbackSession->playbackSinkId;
		const auto sink = GetPlaybackSink(sinkId);
		if (!sink || !sink->timedMarks) {
			return;
		}
		// Drain the marks the playback session fired this frame (token-gated, so only the advancing graph populates them; any participant drains them once).
		std::vector<FiredMark> marks;
		a_graph.playbackSession->DrainFiredMarks(marks);
		if (marks.empty()) {
			return;
		}
		// Snapshot the participant actors (NiPointer keeps them alive across the deferred task); 
		// dispatch on the game thread, the handler enters the VM (and may transition).
		std::vector<RE::NiPointer<RE::Actor>> keep;
		for (auto& p : a_graph.playbackSession->participants) {
			if (p && p->target) {
				keep.emplace_back(static_cast<RE::Actor*>(p->target.get()));
			}
		}
		const PlaybackId playbackId = a_graph.playbackSession->playbackId;
		const std::uint64_t worldEpoch = a_graph.playbackSession->worldEpoch;
		a_deferred.emplace_back([keep, marks, playbackId, worldEpoch, sinkId]() {
			auto& gm = GetSingleton();
			if (gm._worldEpoch.load(std::memory_order_acquire) != worldEpoch) {
				return;
			}
			std::vector<RE::Actor*> actors;
			actors.reserve(keep.size());
			for (auto& a : keep) {
				actors.push_back(a.get());
			}
			if (const auto currentSink = gm.GetPlaybackSink(sinkId); currentSink && currentSink->timedMarks) {
				currentSink->timedMarks(playbackId, actors, marks);
			}
		});
	}

	void GraphManager::QueueFadeRemovalIfDone(Graph& a_graph, std::vector<std::function<void()>>& a_deferred)
	{
		// Fade-out finished: queue removal on the game thread (the hook holds the state lock shared here). 
		// A replay before the task runs resets the blend state, and RemoveFadedGraph re-checks under the lock.
		if (a_graph.playbackSession || !a_graph.IsFadedOut() || a_graph.removalQueued) {
			return;
		}
		a_graph.removalQueued = true;
		REX::DEBUG("[Anim] fade-removal QUEUED for actor {:X}", a_graph.target ? a_graph.target->formID : 0);
		RE::NiPointer<RE::TESObjectREFR> keepAlive{ a_graph.target };
		a_deferred.emplace_back([keepAlive]() {
			GetSingleton().RemoveFadedGraph(keepAlive.get());
		});
	}

	// Per-update-call tick for a synchronized-playback participant: keep an anchored NPC ANIMATION-DRIVEN so its AI keeps the anim
	// running but stops pathing the body off the session anchor. The body-skin render cull reads the havok CAPSULE; a
	// motion-driven NPC walks its capsule back to its package post (~4 u/s) and the body clips at orbit angles that
	// no longer line up with the drifting capsule. The working lever is Actor::boolFlags2 kAnimationDriven (bit 19)
	// set DIRECTLY — MovementControllerNPC::SetAnimationDriven is a no-op on 1.16.244. Re-asserted ~every 18
	// update-calls on the game thread (the AI package re-asserts motion each update); cleared in StopPlaybackLocked so
	// the NPC resumes normal movement from where the session left it. Player excluded (movement-locked, doesn't drift).
	// OSF RE module: gameplay.actor_animation_driven.
	void GraphManager::HoldAnchoredParticipant(Graph& a_graph, RE::TESObjectREFR* a_refr, std::vector<std::function<void()>>& a_deferred)
	{
		if (!a_graph.playbackSession || !a_graph.playbackSession->anchored || a_graph.participantIndex < 0 || !a_refr ||
			a_refr == static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton())) {
			return;
		}
		// Save window: OnSaveBegin stripped the bit so the save can't serialize it; stand down until
		// OnSaveEnd re-asserts. Checked again inside the task — a task queued just before the window
		// opened must not race the bit back in while the save is being written.
		if (_saveWindow.load(std::memory_order_relaxed)) {
			return;
		}
		if (++a_graph.sceneFrames % 18 != 0) {  // ~every 0.075s of update time
			return;
		}
		RE::NiPointer<RE::Actor> keepAlive{ static_cast<RE::Actor*>(a_refr) };
		const PlaybackId playbackId = a_graph.playbackSession->playbackId;
		const std::uint64_t worldEpoch = a_graph.playbackSession->worldEpoch;
		a_deferred.emplace_back([keepAlive, playbackId, worldEpoch]() {
			auto& gm = GetSingleton();
			if (gm._worldEpoch.load(std::memory_order_acquire) != worldEpoch ||
				gm._saveWindow.load(std::memory_order_relaxed)) {
				return;
			}
			{
				std::shared_lock l{ gm.stateLock };
				const auto it = gm.graphs.find(keepAlive.get());
				if (it == gm.graphs.end()) {
					return;
				}
				std::scoped_lock gl{ it->second->stateLock };
				if (!it->second->playbackSession || it->second->playbackSession->playbackId != playbackId) {
					return;
				}
			}
			keepAlive->boolFlags2.set(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
		});
	}

	void GraphManager::Hook_AnimGraphUpdate(void* a_this, RE::BSAnimationUpdateData* a_updateData)
	{
		// Evaluate the game's graph first; our rig writes land after the engine's pose refresh + eval and before world composition.
		_origAnimGraphUpdate(a_this, a_updateData);

		if (!a_updateData) {
			return;
		}
		auto& gm = GetSingleton();
		const auto activeGraphCount = gm.graphCount.load(std::memory_order_relaxed);

		// Idle early-out: this hook fires ~7x per render frame for every AnimationManager in the game; 
		// with no managed graphs there is nothing else to do.
		if (activeGraphCount == 0) {
			return;
		}

		// Identity match: resolve the managed actor directly from the AnimationManager pointer (no position guessing, no collisions).
		auto* animMgr = static_cast<RE::AnimationManager*>(a_this);
		RE::TESObjectREFR* refr = animMgr ? animMgr->GetTargetReference() : nullptr;
		if (!refr) {
			return;
		}

		std::shared_lock l{ gm.stateLock };
		auto iter = gm.graphs.find(refr);
		if (iter == gm.graphs.end()) {
			return;
		}
		// stateLock is held shared through the rest of this function, so the map entry can't be erased from under us 
		// bind to the slot by reference instead of copying the shared_ptr (drops a refcount RMW per call, and this fires ~7x/frame per managed actor).
		auto& g = iter->second;

		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock gl{ g->stateLock };
			g->Sample(PlaybackDelta(*a_updateData), a_this);

			// Per-graph follow-ups, run under both locks; game-thread-only work is collected in
			// `deferred` and handed to SFSE only after both locks release (AddTask under stateLock
			// inverts lock order against SFSE's drain).
			// Marks BEFORE auto-end: the task queue drains FIFO, so on a play-once segment the final
			// at:"end" marks must be queued ahead of the stop task or they resolve a dead handle.
			gm.QueueTimedMarksIfFired(*g, deferred);
			gm.QueueAutoEndIfFinished(*g, deferred);
			gm.QueueFadeRemovalIfDone(*g, deferred);
			gm.HoldAnchoredParticipant(*g, refr, deferred);
		}
		l.unlock();
		FlushDeferredTasks(deferred);
	}

	uint64_t GraphManager::Hook_ModelNodeUpdate(
		RE::BGSModelNode* a_this, void* a_parentTransform, void* a_updateData, void* a_outputTransform)
	{
		{
			// Stamp the latest sampled pose for the graph driving this skeleton before the engine's compose+commit runs (the verified write point).
			// Unmanaged skeletons fall through with one map scan; managed graph counts are small (synchronized-playback participants).
			auto& gm = GetSingleton();
			// This runs once per skeleton per frame game-wide; with no OSF playback the atomic check keeps it lock-free.
			// A racing insert is benign (the new graph stamps next frame at the latest).
			if (gm.graphCount.load(std::memory_order_relaxed) > 0) {
				std::shared_lock l{ gm.stateLock };
				if (!gm.graphs.empty()) {
					for (auto& [refr, g] : gm.graphs) {
						// Normal case: one atomic pointer comparison, then verify under the graph lock.
						// Recovery only pays the actor-root lookup while no stamp target is published,
						// and ResolveAndBind accepts the candidate only if it is this graph's current
						// actor node. That lets the first compose after a 3D rebuild stamp immediately.
						const auto* publishedTarget = g->StampTarget();
						if (publishedTarget == a_this || publishedTarget == nullptr) {
						std::unique_lock gl{ g->stateLock };
						bool recovered = false;
						if (g->StampTarget() != a_this) {
							if (!g->ResolveAndBind(a_this)) {
								continue;
							}
							recovered = true;
						}
						if (recovered) {
							REX::TRACE("[Anim] compose recovered rig binding — actor {:08X}, modelNode {}",
								refr ? refr->formID : 0, static_cast<const void*>(a_this));
						}
						g->StampPose(a_this);
						if (g->playbackSession) {
							const auto stampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now().time_since_epoch()).count();
							g->playbackSession->lastStampMs.store(stampMs, std::memory_order_relaxed);
						}
						// Pin the compose root TRANSLATION to the placement world position. a_parentTransform = &fadeNode->local (this compose's root input;
						// NiTransform, translate at +0x30). Overriding the compose input pins the RENDERED skeleton without fighting physics 
						// (capsules sit ~0.3 m off and win any refr-teleport). Synchronized-playback participant -> its placement (anchored); solo graph -> its SetAnchor anchor when rootMode != kFollow.
						RE::NiPoint3 pinWorld{};
						bool doPin = false;
						// World heading to re-pin per frame (radians). Only known on the synchronized-playback path; solo anchors retain no heading. Gated to the PLAYER below.
						float pinHeading = 0.0f;
						bool hasPinHeading = false;
						if (g->playbackSession && g->playbackSession->anchored && g->participantIndex >= 0) {
							pinWorld = PlacementToWorld(g->playbackSession->anchorPos, g->playbackSession->anchorHeading,
								g->scenePlacement);
							pinHeading = g->playbackSession->anchorHeading +
								g->scenePlacement.heading;
							hasPinHeading = true;
							doPin = true;
						} else if (!g->playbackSession && g->hasAnchor && g->rootMode != RootMode::kFollow) {
							pinWorld = g->anchorPos;
							doPin = true;
						}
						if (doPin && a_parentTransform) {
							float* root = reinterpret_cast<float*>(a_parentTransform);
							root[12] = pinWorld.x;  // NiTransform translation (+0x30)
							root[13] = pinWorld.y;
							root[14] = pinWorld.z;

							//Set actor position to skeleton position
							refr->data.location = pinWorld;

							//Pin root rotation for participants. Overrides player rotate to camera heading and npc lookat targets
							// Rotation = NiMatrix3 at +0x00: three ROWS of 4 floats (4th pad), stride 4 -> root[r*4 + c].
							if (hasPinHeading) {
								const float c = std::cos(pinHeading);
								const float s = std::sin(pinHeading);
								root[0] = c;    root[1] = -s;   root[2] = 0.0f;   // row0
								root[4] = s;    root[5] = c;    root[6] = 0.0f;   // row1
								root[8] = 0.0f; root[9] = 0.0f; root[10] = 1.0f;  // row2

								// Also pin the LOGICAL heading. (root is just rendering)
								refr->data.angle.z = pinHeading;
							}

							// Keep the CULL SPHERE on the pinned render position. The engine derives worldBound from the physics capsule (~0.3 m off), 
							// so left alone NiCullingProcess pops the actor in/out as the camera orbits.
							// Recover the node from a_parentTransform (= local @ +0x40) and rewrite worldBound (center +0x100, radius +0x10C). 
							// Written pre-orig but after the bound pass, so culling sees it this frame.
							auto* fadeNode = reinterpret_cast<RE::NiAVObject*>(
								reinterpret_cast<std::byte*>(a_parentTransform) - offsetof(RE::NiAVObject, local));

							// Center on the visible mesh (torso), not the feet/origin: a feet-centered sphere
							// sits below the posed body and frustum-culls at certain orbit angles (see kPinCullCenterUp).
							// CAP the radius small (min, not max) so the near-fade threshold stays close in.
							fadeNode->worldBound.center = { pinWorld.x, pinWorld.y, pinWorld.z + kPinCullCenterUp };
							fadeNode->worldBound.radius = std::min(fadeNode->worldBound.radius, kPinCullRadius);

							// Hold the near-camera fade (BSFadeNode+0x1B4) at 1.0 so pinned participants don't fade
							// out when the camera orbits close (works together with the small bound radius above).
							*reinterpret_cast<float*>(
								reinterpret_cast<std::byte*>(fadeNode) + kFadeNodeVisFlagOff) = 1.0f;
						}

						break;
						}
					}
				}
			}
		}

		return _origModelNodeUpdate(a_this, a_parentTransform, a_updateData, a_outputTransform);
	}
}
