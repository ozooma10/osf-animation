#include "API/EngineServices.h"
#include "Util/NativeMainThreadQueue.h"
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
#include "REL/THook.h"
#ifdef OSF_ANIMATION_SMOKE
#include "Testing/AnimationSmoke.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <spdlog/spdlog.h>

namespace OSF::Animation
{
#ifdef OSF_ANIMATION_SMOKE
	std::optional<GraphManager::SmokePose> GraphManager::ObservePoseForSmoke(RE::Actor* a_actor)
	{
		std::shared_lock mapLock(stateLock);
		const auto found = graphs.find(a_actor);
		if (found == graphs.end()) return std::nullopt;
		auto& graph = *found->second;
		std::lock_guard graphLock(graph.stateLock);
		SmokePose result{ graph.binding.size(), graph.stampProbeValid,
			graph.stampProbeValid && std::ranges::all_of(graph.stampProbe, [](float v) { return std::isfinite(v); }),
			graph.poseMode, graph.poseWeight, graph.boneMask ? graph.boneMask->id : "" };
		if (graph.skeleton && graph.skeleton->data) {
			const auto names = graph.skeleton->data->joint_names();
			result.boneWeights.reserve(graph.binding.size());
			for (const auto& bound : graph.binding) {
				if (bound.jointIndex < names.size() && bound.rigIndex < graph.cachedRigBoneCount) {
					result.boneWeights.emplace_back(names[bound.jointIndex], bound.weight);
				}
			}
		}
		return result;
	}
#endif
	namespace
	{
		// Final marks and their stop command must land in the same frame batch, in order.
		void FlushDeferredTasks(std::vector<std::function<void()>>& a_tasks)
		{
			Util::NativeMainThreadQueue::PostBatch(std::move(a_tasks));
		}

		using AnimUpdateHook = REL::THookVFT<void(void*, RE::BSAnimationUpdateData*)>;
		using ModelNodeUpdateHook = REL::THookVFT<uint64_t(RE::BGSModelNode*, void*, void*, void*)>;
		std::optional<AnimUpdateHook> g_animUpdateHook;
		std::optional<ModelNodeUpdateHook> g_modelNodeUpdateHook;

		constexpr size_t UpdateVFuncIdx = 4;
		
		// sig (modelNode, &fadeNode->local, NiUpdateData*, outputTransform).
		constexpr size_t ModelNodeUpdateVFuncIdx = 2;

		// Cap (not floor), meters: the engine's near-camera fade distance scales with this radius, so a large
		// sphere hides partners when the orbit zooms in. Frustum safety comes from the torso-lifted center.
		constexpr float kPinCullRadius = 1.0f;

		// Lifts the cull-sphere center from the feet to the torso (meters, +Z); a feet-centered sphere leaves
		// the frustum at some orbit angles and the participant pops out.
		constexpr float kPinCullCenterUp = 1.0f;

		// BSFadeNode near-camera fade (+0x1B4, float: 1.0 = drawn, 0.0 = faded); held at 1.0 for pinned actors.
		constexpr std::ptrdiff_t kFadeNodeVisFlagOff = 0x1B4;

		bool IsGameMenuPaused()
		{
			if (auto* main = RE::Main::GetSingleton()) {
				return main->isGameMenuPaused;
			}
			return false;
		}

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
			// Console pauses the world without setting isGameMenuPaused.
			if (IsGameMenuPaused() || g_consoleOpen.load(std::memory_order_relaxed)) {
				return 0.0f;
			}

			// After a menu pause the engine may report accumulated wall time; drop that catch-up spike.
			constexpr float kMaxUpdateStep = 0.1f;
			const float dt = a_updateData.timeDelta;
			return (std::isfinite(dt) && dt >= 0.0f && dt <= kMaxUpdateStep) ? dt : 0.0f;
		}

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
			if (a_plan.anchorResolved &&
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
			scene->restoreParticipantTransforms = a_plan.anchored && a_plan.restoreTransformsOnEnd;
			scene->loopWhole = a_plan.loopWhole;
			if (scene->restoreParticipantTransforms) {
				if (a_plan.baselineTransforms.size() == a_actors.size()) {
					scene->originalTransforms = a_plan.baselineTransforms;  // caller-carried pre-playback baseline
				} else {
					scene->originalTransforms.reserve(a_actors.size());
					for (const auto* actor : a_actors) {
						scene->originalTransforms.emplace_back(actor->data.location, actor->data.angle.z);
					}
				}
			}
			const float requestedSpeed = std::isfinite(a_plan.speed) ? a_plan.speed : 1.0f;
			scene->speed = std::clamp(requestedSpeed, 0.0f, 100.0f);

			scene->SetSegment(a_startSegment);

			if (a_plan.anchorResolved) {
				scene->anchorPos = a_plan.anchorPos;
				scene->anchorHeading = a_plan.anchorHeading;
			} else {
				scene->anchorPos = a_actors[0]->data.location;
				scene->anchorHeading = a_actors[0]->data.angle.z;
			}

			// Arm the stall watchdog from birth: it skips lastAdvanceMs == 0, so a never-ticked session would
			// otherwise hold the player input lock forever.
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

	std::optional<GraphManager::PlaybackSink> GraphManager::GetPlaybackSink(PlaybackSinkId a_id) const
	{
		if (a_id == 0) return std::nullopt;
		std::shared_lock l{ _sinkLock };
		const auto it = _playbackSinks.find(a_id);
		return it == _playbackSinks.end() ? std::nullopt : std::optional<PlaybackSink>{ it->second };
	}

	void GraphManager::InstallHooks()
	{
		if (HooksInstalled()) {
			return;
		}

		// Capture the current targets so earlier plugins remain in the hook chain.
		g_animUpdateHook.emplace("[Anim] AnimationManager::Update", RE::VTABLE::AnimationManager[0], UpdateVFuncIdx, &Hook_AnimGraphUpdate);
		g_modelNodeUpdateHook.emplace("[Anim] BGSModelNode::Update", RE::VTABLE::BGSModelNode[0], ModelNodeUpdateVFuncIdx, &Hook_ModelNodeUpdate);
		g_animUpdateHook->Enable();
		g_modelNodeUpdateHook->Enable();
		REX::TRACE("[Anim] installed AnimationManager::Update (vfunc {}) + BGSModelNode::Update (vfunc {}) hooks",
			UpdateVFuncIdx, ModelNodeUpdateVFuncIdx);
	}

	bool GraphManager::HooksInstalled() const
	{
		return g_animUpdateHook && g_animUpdateHook->GetEnabled() &&
		       g_modelNodeUpdateHook && g_modelNodeUpdateHook->GetEnabled();
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

		if (!HooksInstalled()) {
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
				// Don't clobber a scene participant: its next segment tick would overwrite this clip anyway.
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
			fadedOut = g->IsFadedOut() || g->IsStranded(now);  // stranded = Sample stopped, so the fade can never elapse
			faded = !sceneSet && fadedOut;  // replayed meanwhile? keep it
			if (!faded) {
				g->removalQueued = false;
			}
		}
		if (faded) {
			graphs.erase(iter);
			graphCount.store(graphs.size(), std::memory_order_relaxed);
			REX::DEBUG("[Anim] fade-removal DONE — actor {:X} graph removed (graphs now {})",
				a_refr ? a_refr->formID : 0, graphs.size());
		} else {
			REX::DEBUG("[Anim] fade-removal KEPT — actor {:X} (session={} fadedOut={})",
				a_refr ? a_refr->formID : 0, sceneSet, fadedOut);
		}
	}

	bool GraphManager::PlaySynchronized(const std::vector<RE::Actor*>& a_actors, const PlaybackPlan& a_plan, int32_t a_startSegment,
		PlaybackId* a_outPlaybackId, PlaybackId a_expectedPlayback, PlaybackSinkId a_sinkId)
	{
		if (a_outPlaybackId) {
			*a_outPlaybackId = 0;
		}
		if (!HooksInstalled()) {
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

		auto scene = BuildPlaybackFromPlan(a_actors, a_plan, a_startSegment);
		if (!scene) {
			return false;
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

			// Admit the full cast before mutating any graph; an owner may replace only the playback it names.
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
			// Replacement reuses these Graphs: capture the displayed pose before StopPlaybackLocked drops the binding.
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
					slot->SetAnimation(startSlot.skeleton, startSlot.anim, startSlot.file);
					slot->blendDuration = scene->stages[startSegment].blendIn;
					slot->playbackSession = scene.get();
					slot->participantIndex = static_cast<int>(i);
					slot->scenePlacement = scene->stages[startSegment].placements[i];
					slot->appliedStage = startSegment;
					slot->hasAnchor = false;
					slot->anchorRevision++;
					slot->syncGroup.reset();

				}
				scene->participants.push_back(slot);
			}
			playbackSessions.push_back(scene);
			graphCount.store(graphs.size(), std::memory_order_relaxed);
		}
		FlushDeferredTasks(deferred);

		// Game thread: teleports/AI-flag writes don't take from Papyrus. SetPosition(.., true) moves the havok
		// capsule (the cull input); NPCs get kAnimationDriven so AI won't walk them back.
		for (size_t i = 0; scene->anchored && i < scene->participants.size(); i++) {
			const auto& pl = scene->stages[startSegment].placements[i];
			auto* refr = scene->participants[i]->target.get();
			if (!refr) {
				continue;
			}
			const RE::NiPoint3 worldPos = PlacementToWorld(scene->anchorPos, scene->anchorHeading, pl);
			const float heading = scene->anchorHeading + pl.heading;
			// Never make the player animation-driven; it can get stuck.
			const bool isPlayer = refr == static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton());
			RE::NiPointer<RE::Actor> keepAlive{ static_cast<RE::Actor*>(refr) };
			const PlaybackId playbackId = scene->playbackId;
			const std::uint64_t worldEpoch = scene->worldEpoch;
			Util::NativeMainThreadQueue::Post([keepAlive, worldPos, heading, isPlayer, playbackId, worldEpoch]() {
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
				keepAlive->SetPosition(worldPos, true);
				// Keep the bit clear during a save window; the maintenance hold sets it afterwards.
				if (!isPlayer && !GetSingleton()._saveWindow.load(std::memory_order_relaxed)) {
					keepAlive->boolFlags2.set(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
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
			snapshot.time, snapshot.duration, snapshot.speed, static_cast<int32_t>(snapshot.stage), scene->playbackId, scene->anchorPos, scene->anchorHeading
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

	void GraphManager::StopPlaybackByPtr(PlaybackSession* a_session)
	{
		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock l{ stateLock };
			StopPlaybackLocked(a_session, deferred);
		}
		FlushDeferredTasks(deferred);
	}

	void GraphManager::StopAll(const char* a_reason)
	{
#ifdef OSF_ANIMATION_SMOKE
		Testing::AnimationSmoke::OnWorldReset();
#endif
		// Bump the epoch first so already-queued tasks from the discarded world no-op.
		_worldEpoch.fetch_add(1, std::memory_order_acq_rel);
		API::EngineServices::WorldReset();
		// A load can begin mid-save-op; don't let the save window outlive its world.
		_saveWindow.store(false, std::memory_order_relaxed);

		// The AI-driven flag is serialized into saves, so clear it on every load even with no live lock.
		Camera::CameraService::GetSingleton().OnStopAll();
		Player::PlayerInputLockService::GetSingleton().OnStopAll();
		Input::InputService::GetSingleton().OnStopAll();

		// Release held fades before the load (the stay-faded latch crashes the load path).
		UI::FadeService::GetSingleton().OnStopAll();
		UI::Subtitle::OnStopAll();
		Audio::SoundService::GetSingleton().StopAll();

		// Copy first so a clear callback may unregister itself without re-entering _sinkLock.
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

		// No fade/revert: the loaded save already reset every actor, and our graphs belong to the discarded world.
		playbackSessions.clear();
		graphs.clear();
		graphCount.store(0, std::memory_order_relaxed);
	}

	std::size_t GraphManager::SetAnchoredNpcAnimationDriven(bool a_enabled)
	{
		std::size_t count = 0;
		const auto* player = RE::PlayerCharacter::GetSingleton();
		std::shared_lock l{ stateLock };
		for (const auto& session : playbackSessions) {
			if (!session->anchored) continue;
			for (const auto& participant : session->participants) {
				if (!participant || !participant->target || participant->target.get() == player) continue;
				auto* actor = static_cast<RE::Actor*>(participant->target.get());
				if (a_enabled) actor->boolFlags2.set(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
				else actor->boolFlags2.reset(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
				++count;
			}
		}
		return count;
	}

	void GraphManager::OnSaveBegin()
	{
		// Game thread (SaveLoadEvent kBegin), so these writes are ordered with the queued boolFlags2 writes.
		if (_saveWindow.exchange(true, std::memory_order_relaxed)) {
			return;  // overlapping save ops; the first opener stripped
		}
		const auto count = SetAnchoredNpcAnimationDriven(false);
		if (count != 0) {
			REX::DEBUG("[Anim] save begin — animation-driven bit stripped from {} anchored participant(s) so the save can't bake it in",
				count);
		}
	}

	void GraphManager::OnSaveEnd()
	{
		if (!_saveWindow.exchange(false, std::memory_order_relaxed)) {
			return;  // no window open (plain load events land here too)
		}
		const auto count = SetAnchoredNpcAnimationDriven(true);
		if (count != 0) {
			REX::DEBUG("[Anim] save end — animation-driven bit re-asserted on {} anchored participant(s)", count);
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
		// Session's authoritative segment, not the lagging appliedStage; shared stateLock keeps it alive.
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

#ifdef OSF_ANIMATION_SMOKE
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

#endif

	bool GraphManager::SetSpeed(RE::Actor* a_actor, float a_speed)
	{
		if (!a_actor) {
			return false;
		}
		if (!std::isfinite(a_speed)) {
			REX::WARN("[Anim] SetSpeed: rejected non-finite speed for actor {:X}", a_actor->formID);
			return false;
		}
		const float speed = std::clamp(a_speed, 0.0f, 100.0f);
		PlaybackId playbackId = 0;
		PlaybackSinkId sinkId = 0;
		{
			std::shared_lock l{ stateLock };
			auto iter = graphs.find(a_actor);
			if (iter == graphs.end()) return false;
			std::scoped_lock gl{ iter->second->stateLock };
			const auto* session = iter->second->playbackSession;
			if (!session) {
				if (iter->second->syncGroup) iter->second->syncGroup->speed.store(speed, std::memory_order_relaxed);
				return true;
			}
			playbackId = session->playbackId;
			sinkId = session->playbackSinkId;
		}
		// Never call a sink while holding manager/graph locks; it owns persistent scene state.
		if (const auto sink = GetPlaybackSink(sinkId); sink && sink->setSpeed)
			return sink->setSpeed(playbackId, speed);
		return SetPlaybackSpeed(a_actor, playbackId, speed);
	}

	bool GraphManager::SetPlaybackSpeed(RE::Actor* a_actor, PlaybackId a_playbackId, float a_speed)
	{
		if (!a_actor || !a_playbackId || !std::isfinite(a_speed)) return false;
		std::shared_lock l{ stateLock };
		const auto it = graphs.find(a_actor);
		if (it == graphs.end()) return false;
		std::scoped_lock gl{ it->second->stateLock };
		const auto& session = it->second->playbackSession;
		if (!session || session->playbackId != a_playbackId) return false;
		session->speed = std::clamp(a_speed, 0.0f, 100.0f);
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
		return 1.0f;  // pre-SetAnimation: authored speed
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
			// Pre-1.1 callers used 2=follow; kept through the 1.x ABI.
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
		// Move the capsule too.
		{
			RE::NiPointer<RE::Actor> keepAlive{ a_actor };
			const RE::NiPoint3 anchor{ a_x, a_y, a_z };
			const auto worldEpoch = _worldEpoch.load(std::memory_order_acquire);
			Util::NativeMainThreadQueue::Post([keepAlive, anchoredGraph, anchor, heading, anchorRevision, worldEpoch]() {
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

		// Auto-anchor: one anchored session at actor[0] with empty placements, so the clips' baked root
		// offsets arrange the actors about one origin (otherwise they animate far apart).
		if (a_anchor) {
			std::vector<RE::Actor*> sceneActors;
			std::vector<std::string> sceneFiles;
			float groupSpeed = 1.0f;
			bool gotSpeed = false;
			{
				// Collect without mutating, then drop stateLock before PlaySynchronized (takes it unique).
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
					if (!gotSpeed) {  // carry a pre-Sync SetSpeed over
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

		// abAnchor=false: clock-merge only; each actor stays at its own world position.
		std::shared_lock l{ stateLock };

		// Collect first so a group short of 2 isn't half-synced. Shared stateLock across both passes blocks
		// session (un)assignment, keeping the pass-1 session check valid.
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
		// The group lock is a leaf. Graphs jump to t=0 on their next sample; speed seeds from the first member.
		auto group = std::make_shared<SyncGroup>();
		{
			std::scoped_lock gl{ targets.front()->stateLock };
			if (const auto& sg = targets.front()->syncGroup) {
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
			stage.loops = a_loops[i];
			stage.timer = 0.0f;
			stage.blendIn = a_blends[i];
			plan.stages.push_back(std::move(stage));
		}
		const std::vector<RE::Actor*> actors{ a_actor };
		return PlaySynchronized(actors, plan, 0);
	}

	void GraphManager::StopPlaybackLocked(PlaybackSession* a_session, std::vector<std::function<void()>>& a_deferred)
	{
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
				// Graph stays mapped and fades; the update hook queues removal once the ramp elapses.
				std::scoped_lock gl{ p->stateLock };
				p->DetachAndFadeOut();
				stoppedRevision = p->playbackRevision;
			}
			REX::DEBUG("[Anim] session-end detach: actor {:X} — playback session cleared, fade-out begun (blendDur {:.2f}s)",
				p->target ? p->target->formID : 0, p->blendDuration);
			// Revert kAnimationDriven via the direct bit reset (game thread). MovementControllerNPC::SetMotionDriven
			// (REL 135315) is mis-bound on this build; don't call it.
			if (anchored && p->target) {
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
								return;  // a replacement session owns the movement mode
							}
							if (applyTransformRestore &&
								(it->second != stoppedGraph || it->second->playbackRevision != stoppedRevision)) {
								// A replacement solo playback owns the actor: clear the flag but skip the stale teleport.
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
		// Deferred: StopScene needs stateLock unique (the hook holds it shared). The once-only gate is the
		// exchange in QueuePlaybackEndDeferred; the shared_ptr keeps the session alive.
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
		// A SetSegment since the claim revived the session; don't kill it with this stale completion.
		if (!a_session->ended.load(std::memory_order_relaxed)) {
			REX::DEBUG("[Anim] playback-session end: session was revived meanwhile — not stopping");
			return;
		}
		if (_worldEpoch.load(std::memory_order_acquire) != a_session->worldEpoch) {
			return;
		}

		// The sink gets first refusal; no manager lock is held, so it may call PlaySynchronized/StopScene.
		bool handled = false;
		if (const auto sink = GetPlaybackSink(a_session->playbackSinkId); sink && sink->autoEnd) {
			std::vector<RE::Actor*> actors;
			for (const auto& p : a_session->participants) {
				if (p && p->target) {
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
			a_deferred->emplace_back(std::move(task));
		} else {
			Util::NativeMainThreadQueue::Post(std::move(task));
		}
	}

	void GraphManager::MaintenanceTick()
	{
		if (graphCount.load(std::memory_order_relaxed) == 0) {
			return;
		}
		// Menus/console freeze animation reports but not this beat; don't read that as a stall.
		if (IsGameMenuPaused() || g_consoleOpen.load(std::memory_order_relaxed)) {
			_stallWatchdog.Pause();
			return;
		}

		// Re-assert kAnimationDriven after AI updates; save begin/end release it around serialization.
		if (!_saveWindow.load(std::memory_order_relaxed)) {
			SetAnchoredNpcAnimationDriven(true);
		}

		constexpr std::int64_t kSceneStallMs = 1500;  // unticked this long while running = interrupted; well above any hitch

		using namespace std::chrono;
		const std::int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

		// Focus loss/loading can stop this beat too; the schedule re-arms a grace window after a gap.
		if (!_stallWatchdog.ShouldScan(now)) {
			return;
		}

		std::vector<std::shared_ptr<PlaybackSession>> stalled;
		{
			std::shared_lock l{ stateLock };
			for (const auto& s : playbackSessions) {
				if (!s || s->ended.load(std::memory_order_relaxed) || s->endQueued.load(std::memory_order_relaxed)) {
					continue;
				}
				if (spdlog::should_log(spdlog::level::trace) && now - s->lastDiagnosticMs >= 1000) {
					s->lastDiagnosticMs = now;
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
			// Claim against a concurrent auto-end, then complete inline (game thread, no manager lock).
			if (!s->endQueued.exchange(true, std::memory_order_relaxed)) {
				HandlePlaybackEndOnGameThread(s);
			}
		}

		// Stranded-graph sweep: a detached graph whose actor stopped updating never finishes its fade, so it
		// pins the REFR and keeps graphCount > 0. Collect under the shared lock, remove outside it.
		std::vector<RE::NiPointer<RE::TESObjectREFR>> strandedGraphs;
		{
			std::shared_lock l{ stateLock };
			for (auto& [refr, g] : graphs) {
				std::scoped_lock gl{ g->stateLock };
				if (g->playbackSession || g->removalQueued) {
					continue;  // session graphs belong to the stall watchdog; queued ones are already leaving
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
		// Token-gated: only the advancing graph fills the buffer; any participant drains it once.
		std::vector<FiredMark> marks;
		a_graph.playbackSession->DrainFiredMarks(marks);
		if (marks.empty()) {
			return;
		}
		// Game thread: the handler enters the VM (and may transition). NiPointers keep the actors alive.
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
		// Game thread (the hook holds stateLock shared); RemoveFadedGraph re-checks in case of a replay.
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

	void GraphManager::Hook_AnimGraphUpdate(void* a_this, RE::BSAnimationUpdateData* a_updateData)
	{
		// Run the engine graph first; our sampling follows its evaluation.
		(*g_animUpdateHook)(a_this, a_updateData);

		if (!a_updateData) {
			return;
		}
		auto& gm = GetSingleton();
		const auto activeGraphCount = gm.graphCount.load(std::memory_order_relaxed);

		if (activeGraphCount == 0) {
			return;
		}

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
		// Shared stateLock pins the map entry; bind by reference to skip a refcount RMW on this hot path.
		auto& g = iter->second;

		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock gl{ g->stateLock };
			g->Sample(PlaybackDelta(*a_updateData), a_this);

			// Marks before auto-end: the queue is FIFO and final at:"end" marks must precede the stop task.
			gm.QueueTimedMarksIfFired(*g, deferred);
			gm.QueueAutoEndIfFinished(*g, deferred);
			gm.QueueFadeRemovalIfDone(*g, deferred);
		}
		l.unlock();
		FlushDeferredTasks(deferred);
	}

	uint64_t GraphManager::Hook_ModelNodeUpdate(
		RE::BGSModelNode* a_this, void* a_parentTransform, void* a_updateData, void* a_outputTransform)
	{
		{
			// Stamp right before the engine's compose+commit; managed graph counts are small, so scan linearly.
			auto& gm = GetSingleton();
			// Runs per skeleton per frame game-wide; lock-free when idle. A racing insert stamps next frame.
			if (gm.graphCount.load(std::memory_order_relaxed) > 0) {
				std::shared_lock l{ gm.stateLock };
				if (!gm.graphs.empty()) {
					for (auto& [refr, g] : gm.graphs) {
						// Atomic compare, then verify under the graph lock. With no published target, ResolveAndBind accepts
						// only this graph's current node, so the first compose after a 3D rebuild stamps immediately.
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
						if (g->playbackSession && spdlog::should_log(spdlog::level::trace)) {
							const auto stampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now().time_since_epoch()).count();
							g->playbackSession->lastStampMs.store(stampMs, std::memory_order_relaxed);
						}
						// Pin the compose root (a_parentTransform = &fadeNode->local, NiTransform). Overriding the compose
						// input pins the rendered skeleton without fighting physics (capsules sit ~0.3 m off).
						RE::NiPoint3 pinWorld{};
						bool doPin = false;
						// Radians; only known for session participants (solo anchors keep no heading).
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

							refr->data.location = pinWorld;

							// Pin root rotation (overrides player camera heading / NPC look-at). NiMatrix3 rows at +0x00: root[r*4 + c].
							if (hasPinHeading) {
								const float c = std::cos(pinHeading);
								const float s = std::sin(pinHeading);
								root[0] = c;    root[1] = -s;   root[2] = 0.0f;
								root[4] = s;    root[5] = c;    root[6] = 0.0f;
								root[8] = 0.0f; root[9] = 0.0f; root[10] = 1.0f;

								// Logical heading too (root only affects rendering).
								refr->data.angle.z = pinHeading;
							}

							// Keep the cull sphere on the pin; worldBound follows the capsule (~0.3 m off) and pops on orbit.
							// fadeNode = local - 0x40; worldBound center +0x100, radius +0x10C; written after the bound pass.
							auto* fadeNode = reinterpret_cast<RE::NiAVObject*>(
								reinterpret_cast<std::byte*>(a_parentTransform) - offsetof(RE::NiAVObject, local));

							fadeNode->worldBound.center = { pinWorld.x, pinWorld.y, pinWorld.z + kPinCullCenterUp };
							fadeNode->worldBound.radius = std::min(fadeNode->worldBound.radius, kPinCullRadius);

							*reinterpret_cast<float*>(
								reinterpret_cast<std::byte*>(fadeNode) + kFadeNodeVisFlagOff) = 1.0f;
						}

						break;
						}
					}
				}
			}
		}

		return (*g_modelNodeUpdateHook)(a_this, a_parentTransform, a_updateData, a_outputTransform);
	}
}
