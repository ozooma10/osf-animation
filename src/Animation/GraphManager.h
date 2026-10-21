#pragma once

// Graph storage plus the per-frame hooks (vtable patch on AnimationManager::Update, vfunc 4).
// This graph/manager structure is adapted, in reduced form, from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh).

#include "Animation/Graph.h"
#include "Animation/Scene.h"
#include "Animation/StallWatchdogSchedule.h"

#include <functional>

namespace OSF::Animation
{
	class PreparedPlayback;
	// Data-relative path opens via BSResource (loose or BA2, engine precedence); no read.
	// Only meaningful once archives are mounted (kPostDataLoad+).
	bool ResourceExists(std::string_view a_relPath);

	class GraphManager
	{
	public:
		static GraphManager& GetSingleton();

		using PlaybackAutoEndHandler = std::function<bool(PlaybackId, const std::vector<RE::Actor*>&, PlaybackEndReason)>;
		using PlaybackClearHandler = std::function<void()>;
		using PlaybackTimedMarkHandler = std::function<void(PlaybackId, const std::vector<RE::Actor*>&, const std::vector<FiredMark>&)>;
		struct PlaybackSink
		{
			PlaybackAutoEndHandler autoEnd;
			PlaybackTimedMarkHandler timedMarks;
			PlaybackClearHandler clear;
			std::function<bool(PlaybackId, float)> setSpeed;
            std::function<bool(PlaybackId, int32_t, bool)> prepareStage; // true = automatic completion
		};

		PlaybackSinkId RegisterPlaybackSink(PlaybackSink a_sink);

		void InstallHooks();

		// Freezes playback while the console is open (it pauses the world without setting isGameMenuPaused).
		// Call once after the UI singleton exists (kPostPostDataLoad).
		void RegisterConsolePauseSink();

		bool HooksInstalled() const;

#ifdef OSF_ANIMATION_SMOKE
		struct SmokePose
		{
			std::size_t boundBones{};
			bool stamped{};
			bool finite{};
			PoseMode mode{};
			float poseWeight{};
			std::string maskId;
			// Values copied from the actual live stamp binding, not reconstructed from the mask.
			std::vector<std::pair<std::string, float>> boneWeights;
		};
		std::optional<SmokePose> ObservePoseForSmoke(RE::Actor* a_actor);
#endif

		// Synchronous load; a_file is Data-relative or absolute.
		bool PlayAnimation(RE::Actor* a_actor, std::string_view a_file, std::string_view a_animId);

		// Fades to the live pose, then removes the graph. Refuses scene participants (use StopScene).
		bool StopAnimation(RE::Actor* a_actor);

		// No-op if replayed meanwhile. Called by deferred follow-ups and the watchdog sweep.
		void RemoveFadedGraph(RE::TESObjectREFR* a_refr);

		// Preloads every segment's clips so segment switches are pointer swaps. Refuses partial playback.
		bool PlaySynchronized(const std::vector<RE::Actor*>& a_actors, const PlaybackPlan& a_plan, int32_t a_startSegment,
			PlaybackId* a_outPlaybackId = nullptr, PlaybackId a_expectedPlayback = 0,
			PlaybackSinkId a_sinkId = 0);

		// Same resource/engine context as PlaySynchronized. Preparation retains decoded clips
		// and exact actor identities without publishing; publication rechecks world/ownership.
		std::shared_ptr<PreparedPlayback> PrepareSynchronized(const std::vector<RE::Actor*>& a_actors,
			const PlaybackPlan& a_plan, int32_t a_startSegment, PlaybackSinkId a_sinkId);
		bool PlayPrepared(const std::shared_ptr<PreparedPlayback>& a_prepared, float a_speed,
			PlaybackId* a_outPlaybackId, PlaybackId a_expectedPlayback);

		bool SetSceneStage(RE::Actor* a_actor, int32_t a_stage, PlaybackId a_expectedPlayback = 0);
        // SceneRuntime preparation commit only. No public SDK surface bypasses the gate.
        bool CommitSceneStage(RE::Actor*, int32_t, PlaybackId);
        bool HoldForPreparation(RE::Actor*, PlaybackId, bool);

		bool SetSceneAutoAdvance(RE::Actor* a_actor, bool a_enabled, PlaybackId a_expectedPlayback);

		struct SynchronizedPlaybackState
		{
			float time = 0.0f;
			float duration = 0.0f;
			float speed = 0.0f;
			int32_t stage = -1;
			PlaybackId playbackId = 0;
			RE::NiPoint3 anchorPos{};
			float anchorHeading = 0.0f;
			int32_t      stageCount = 0;
		};

		std::optional<SynchronizedPlaybackState> GetScenePlayback(RE::Actor* a_actor, PlaybackId a_expectedPlayback = 0);
#ifdef OSF_ANIMATION_SMOKE
		struct AnimationPlayback
		{
			float time = 0.0f;
			float duration = 0.0f;
			float speed = 0.0f;
		};
		// Observe solo playback for the smoke suite; scene actors are excluded.
		std::optional<AnimationPlayback> GetAnimationPlayback(RE::Actor* a_actor);
#endif

		bool StopScene(RE::Actor* a_actor, PlaybackId a_expectedPlayback = 0);

		// -1 if not in a scene.
		int32_t GetSceneStage(RE::Actor* a_actor);

		bool IsPlaying(RE::Actor* a_actor);

		// Source path as the caller gave it; "" if no live graph.
		std::string GetCurrentAnimation(RE::Actor* a_actor);

		// 1.0 = authored, 0 = freeze; clamped 0..100. Scene participants share the scene clock speed.
		bool SetSpeed(RE::Actor* a_actor, float a_speed);
		// Playback owner only; bypasses routing back to the sink and verifies session identity.
		bool SetPlaybackSpeed(RE::Actor* a_actor, PlaybackId a_playbackId, float a_speed);
		bool SetAlignmentDelta(RE::Actor* a_actor, const ParticipantPlacement& a_delta, PlaybackId a_expectedPlayback);
		float GetSpeed(RE::Actor* a_actor);

		// SOLO graphs only; heading in degrees, rootMode 0=pin / 1=follow (2 = deprecated follow alias).
		// Also moves the capsule.
		bool SetAnchor(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode);
		bool ClearAnchor(RE::Actor* a_actor);

		// Promotes playing solo graphs into one clock-shared scene at actor[0]'s transform; anchoring
		// assumes the clips' baked root offsets share an origin.
		bool Sync(const std::vector<RE::Actor*>& a_actors, bool a_anchor = true);

		bool PlaySequence(RE::Actor* a_actor, const std::vector<std::string>& a_files, const std::vector<std::string>& a_animIds, const std::vector<int32_t>& a_loops, const std::vector<float>& a_blends, bool a_loopWhole);

		// No-op if no longer live; the caller keeps the session alive via shared_ptr.
		void StopPlaybackByPtr(PlaybackSession* a_session);

		// Drops all in-memory scene/graph state with no fade, actor mutation, or callbacks (save load: the
		// loaded world is authoritative).
		void StopAll(const char* a_reason);

		// Strip kAnimationDriven (NPCs) / kPlayerAnimationDriven so a mid-scene save doesn't persist them.
		void OnSaveBegin();
		void OnSaveEnd();

		// Main thread, after the frame command batch.
		void MaintenanceTick();

	private:
		bool PlayDecodedAnimation(RE::Actor* a_actor,
			std::shared_ptr<const OzzSkeleton> a_skeleton,
			std::shared_ptr<const OzzAnimation> a_anim,
			std::string a_source);

		// Caller holds stateLock unique. Game-thread follow-ups go to a_deferred; flush after unlocking.
		void StopPlaybackLocked(PlaybackSession* a_session, std::vector<std::function<void()>>& a_deferred);

		// Game thread; returns the number of anchored NPCs updated.
		std::size_t SetAnchoredNpcAnimationDriven(bool a_enabled);

		// Called from Hook_AnimGraphUpdate under shared stateLock + the graph lock; follow-ups go to
		// a_deferred and are flushed after both locks are released.
		void QueueAutoEndIfFinished(Graph& a_graph, std::vector<std::function<void()>>& a_deferred);
		void QueueTimedMarksIfFired(Graph& a_graph, std::vector<std::function<void()>>& a_deferred);
		void QueueFadeRemovalIfDone(Graph& a_graph, std::vector<std::function<void()>>& a_deferred);

		// Game thread, no manager lock held: the runtime auto-end handler takes over, else we stop it.
		void HandlePlaybackEndOnGameThread(std::shared_ptr<PlaybackSession> a_session);

		void QueuePlaybackEndDeferred(std::shared_ptr<PlaybackSession> a_session, std::vector<std::function<void()>>* a_deferred = nullptr);

		// AnimationManager::Update (vfunc 4): ~7x/frame per graph on job threads, subdivided dt (timeDelta @ +0x60).
		// Clock + sampling only: the snapshot applier (vfunc 7) overwrites rig locals after every update.
		static void Hook_AnimGraphUpdate(void* a_this, RE::BSAnimationUpdateData* a_updateData);

		// BGSModelNode::Update (vfunc 2), once per skeleton per frame from BSFadeNode::Update. Stamping before
		// the original lands in the same compose, and it keeps running for AI-frozen actors.
		static uint64_t Hook_ModelNodeUpdate(
			RE::BGSModelNode* a_this, void* a_parentTransform, void* a_updateData, void* a_outputTransform);

		std::shared_mutex stateLock;
		std::unordered_map<RE::TESObjectREFR*, std::shared_ptr<Graph>> graphs;
		std::vector<std::shared_ptr<PlaybackSession>> playbackSessions;

		std::optional<PlaybackSink> GetPlaybackSink(PlaybackSinkId a_id) const;
		mutable std::shared_mutex _sinkLock;
		std::unordered_map<PlaybackSinkId, PlaybackSink> _playbackSinks;
		std::atomic<PlaybackSinkId> _nextPlaybackSinkId{ 1 };

		// Mirror of graphs.size() (updated under unique stateLock) so the hooks early-out when idle
		// without touching stateLock.
		std::atomic<size_t> graphCount{ 0 };

		// playbackId prevents actor-reuse ABA; worldEpoch invalidates every pre-load task at once.
		std::atomic<PlaybackId> _nextPlaybackId{ 1 };
		std::atomic<std::uint64_t> _worldEpoch{ 1 };

		// Main thread only.
		StallWatchdogSchedule _stallWatchdog;

		// Between OnSaveBegin/OnSaveEnd: all kAnimationDriven set-points stand down.
		std::atomic<bool> _saveWindow{ false };
	};
}
