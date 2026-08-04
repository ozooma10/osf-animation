#pragma once

// Graph storage plus the per-frame hooks.
// The main hook surface is a vtable patch on AnimationManager::Update (vfunc 4)
// This graph/manager structure is adapted, in reduced form, from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh).

#include "Animation/Graph.h"
#include "Animation/Scene.h"

#include <functional>

namespace OSF::Animation
{
	// True when a_relPath (Data-relative) opens through BSResource — loose file OR archive-resident,
	// with the engine's loose-over-BA2 precedence. Cheap open/close probe, no read. Used by the
	// registry's clip-availability sweep; only meaningful once archives are mounted (kPostDataLoad+).
	bool ResourceExists(std::string_view a_relPath);

	class GraphManager
	{
	public:
		static GraphManager& GetSingleton();

		using SceneAutoEndHandler = std::function<bool(PlaybackId, const std::vector<RE::Actor*>&, SceneEndReason)>;
		using SceneClearHandler = std::function<void()>;
		using SceneTimedMarkHandler = std::function<void(PlaybackId, const std::vector<RE::Actor*>&, const std::vector<FiredMark>&)>;
		struct PlaybackSink
		{
			SceneAutoEndHandler autoEnd;
			SceneTimedMarkHandler timedMarks;
			SceneClearHandler clear;
		};

		// Register a callback owner once, then stamp its id onto each staged playback. Dispatch is
		// direct to that owner; registration order and PlaybackId guessing are not part of routing.
		PlaybackSinkId RegisterPlaybackSink(PlaybackSink a_sink);
		bool UnregisterPlaybackSink(PlaybackSinkId a_id);

		// Installs the AnimationManager::Update vtable hook.
		void InstallHooks();

		// Registers a MenuOpenCloseEvent sink that freezes playback while the console is open.
		// console pauses the world but does NOT set Main::isGameMenuPaused, Call once after the UI singleton exists (kPostPostDataLoad).
		void RegisterConsolePauseSink();

		// True when both vtable hooks verified and patched (the core playback path is live).
		bool HooksInstalled() const { return _origAnimGraphUpdate && _origModelNodeUpdate; }

		// Synchronously loads a_file (relative to the game's Data folder, or absolute) and starts playing it on a_actor.
		bool PlayAnimation(RE::Actor* a_actor, std::string_view a_file, std::string_view a_animId);

		// Decodes an engine-native human `.af` directly from caller-owned bytes and starts it as a
		// normal looping solo graph. Used by the transient Studio link; never enters the pack registry.
		bool PlayAnimationBytes(RE::Actor* a_actor, const std::vector<std::uint8_t>& a_bytes,
			std::string_view a_clipKey, std::string* a_error = nullptr);

		// Starts a fade-out (the graph keeps sampling while its stamp weight ramps to 0, landing on the engine's live pose, then removes itself).
		// Refuses for scene participants, use StopScene in that case.
		bool StopAnimation(RE::Actor* a_actor);

		// Erases actors's graph if its fade-out has fully elapsed (no-op if it was replayed meanwhile). Called from the deferred removal task.
		void RemoveFadedGraph(RE::TESObjectREFR* a_refr);

		// Starts a staged synced scene from a ScenePlan. every stages clips are loaded up front. (stage switches are pointer swaps, not file IO)
		// Refuses partial scenes. stages auto-advance on timers or loop count targets.
		// after last stage the scene stops itself (on game thread)
		bool PlaySceneStaged(const std::vector<RE::Actor*>& a_actors, const ScenePlan& a_plan, int32_t a_startStage,
			PlaybackId* a_outPlaybackId = nullptr, PlaybackId a_expectedPlayback = 0,
			PlaybackSinkId a_sinkId = 0, bool a_strictTimedMarks = false);

		// Jumps the scene containing a_actor to the given stage (0-based);
		// false if the actor is not in a scene or the stage is out of range.
		bool SetSceneStage(RE::Actor* a_actor, int32_t a_stage, PlaybackId a_expectedPlayback = 0);

		struct ScenePlayback
		{
			float time = 0.0f;
			float duration = 0.0f;
			float speed = 0.0f;
			int32_t stage = -1;
			PlaybackId playbackId = 0;
		};
		struct AnimationPlayback
		{
			float time = 0.0f;
			float duration = 0.0f;
			float speed = 0.0f;
		};

		// Browser/director inspection of the current scene clock. Seek never fires timed marks.
		bool SetSceneTime(RE::Actor* a_actor, float a_time, PlaybackId a_expectedPlayback = 0);
		std::optional<ScenePlayback> GetScenePlayback(RE::Actor* a_actor, PlaybackId a_expectedPlayback = 0);
		// Solo-clip transport used by the transient Studio preview. Scene actors are rejected.
		bool SetAnimationTime(RE::Actor* a_actor, float a_time);
		std::optional<AnimationPlayback> GetAnimationPlayback(RE::Actor* a_actor);
		bool SetAnimationHoldAtEnd(RE::Actor* a_actor, bool a_hold);

		// Stops the scene that a_actor participates in (all its participants).
		bool StopScene(RE::Actor* a_actor, PlaybackId a_expectedPlayback = 0);
		// Immediate no-fade removal for owner handoff (overlay suspension / zero-animation station).
		// The expected playback is mandatory so another owner can never be erased accidentally.
		bool StopSceneImmediate(RE::Actor* a_actor, PlaybackId a_expectedPlayback);

		// Current scene stage for a_actor, or -1 if not in a scene.
		int32_t GetSceneStage(RE::Actor* a_actor);

		// True while a_actor has a live graph (scene or solo, including fades).
		bool IsPlaying(RE::Actor* a_actor);

		// Current source file path playing on a_actor (as the caller gave it), or "" if the actor has no live graph.
		std::string GetCurrentAnimation(RE::Actor* a_actor);

		// Per-graph playback speed (1.0 = authored, 0 = freeze; clamped 0..100).
		// For a scene participant this sets/reads the shared scene clock speed.
		bool SetSpeed(RE::Actor* a_actor, float a_speed);
		float GetSpeed(RE::Actor* a_actor);

		// Pin a SOLO graph to a world point + heading (degrees), with rootMode 0=pin / 1=follow.
		// Legacy value 2 remains a deprecated follow alias for 1.x callers.
		// Also moves the capsule there. Refused for scene participants. ClearAnchor releases it.
		bool SetAnchor(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode);
		bool ClearAnchor(RE::Actor* a_actor);

		// Bring N playing solo graphs together. basically promotes them into a single clock-shared scene at actor[0]'s transform.
		// assumes the clips baked root offsets arange actors arouns same origin point. (if anchoring)
		bool Sync(const std::vector<RE::Actor*>& a_actors, bool a_anchor = true);

		// Solo multi-stage sequence (no anchor)
		bool PlaySequence(RE::Actor* a_actor, const std::vector<std::string>& a_files, const std::vector<std::string>& a_animIds, const std::vector<int32_t>& a_loops, const std::vector<float>& a_blends, bool a_loopWhole);

		// Stops a scene by pointer if it is still live (no-op otherwise). Used by the deferred auto-end task; 
		// the caller keeps the Scene alive via shared_ptr so the pointer can never be stale.
		void StopSceneByPtr(Scene* a_scene);

		// drops all inmemory scene + graph state immediately. (no fade or actor mutation)
		// call when game loads save to nuke our state and reset from the world which is authority.
		// World teardown intentionally dispatches no scene callbacks into the discarded VM/world.
		// Does not restore equipment/movement; the loaded world is authoritative.
		void StopAll(const char* a_reason);

		// We want to to a scrub of transient bits we set on actors that might persist in save.
		// Right now largely remove ai-driven flags on anchored NPCs (kAnimationDriven) and the player (kPlayerAnimationDriven) so they dont get stuck in a save mid-scene.
		void OnSaveBegin();
		void OnSaveEnd();

	private:
		bool PlayDecodedAnimation(RE::Actor* a_actor,
			std::shared_ptr<const OzzSkeleton> a_skeleton,
			std::shared_ptr<const OzzAnimation> a_anim,
			std::string a_source);

		// Detaches all participants of a_scene (revert movement mode, drop graphs) and removes it from `scenes`. Caller holds stateLock unique.
		// Game-thread follow-ups are appended to a_deferred; the caller MUST hand them to SFSE
		// (FlushDeferredTasks) after releasing stateLock — AddTask under stateLock inverts lock
		// order against SFSE's task drain.
		void StopSceneLocked(Scene* a_scene, std::vector<std::function<void()>>& a_deferred);

		// Every live anchored NPC participant (player excluded), NiPointer-pinned. Takes stateLock shared.
		// The save-window strip/re-assert pair (OnSaveBegin/OnSaveEnd) acts on this set.
		std::vector<RE::NiPointer<RE::Actor>> CollectAnchoredNpcParticipants();

		// Per-graph upkeep run from Hook_AnimGraphUpdate right after sampling, while the caller holds stateLock (shared) and the graph's own lock.
		// Each appends its game-thread-only follow-up to a_deferred; the hook flushes the batch to
		// the SFSE task queue AFTER releasing both locks (AddTask under stateLock inverts lock order
		// against SFSE's drain).
		void QueueAutoEndIfFinished(Graph& a_graph, std::vector<std::function<void()>>& a_deferred);                   // last stage ran out -> StopScene
		void QueueTimedMarksIfFired(Graph& a_graph, std::vector<std::function<void()>>& a_deferred);                   // stage fired timed marks -> mark handler
		void QueueFadeRemovalIfDone(Graph& a_graph, std::vector<std::function<void()>>& a_deferred);                  // fade-out elapsed -> RemoveFadedGraph
		void HoldAnchoredParticipant(Graph& a_graph, RE::TESObjectREFR* a_refr, std::vector<std::function<void()>>& a_deferred);  // keep an anchored scene NPC animation-driven (no AI walk-back)

		// Defer a scene's end to the game thread: the runtime auto-end handler takes over (advance/end +
		// ledger replay), else we stop it ourselves. Idempotent via Scene::endQueued; the shared_ptr keeps
		// the Scene alive across the deferral. Shared by QueueAutoEndIfFinished and the stall watchdog.
		// With a_deferred (locked contexts) the end task is appended there instead of handed to
		// SFSE directly; the caller flushes after releasing stateLock.
		void QueueSceneEndDeferred(std::shared_ptr<Scene> a_scene, std::vector<std::function<void()>>* a_deferred = nullptr);

		// Stall watchdog: runs from the update hook every call. Finds live scenes the engine stopped
		// ticking (lastAdvanceMs gone stale while the game runs) and ends them cleanly as kInterrupted, so
		// an unloaded/AI-disabled/interrupted scene can't strand its participants with the player lock held.
		// Pause/resume-filtered (the hook itself stalls when the game pauses) and throttled (fires ~7x/frame).
		void StallWatchTick();

		// AnimationManager::Update (vfunc 4). Runs per graph ~7x per render frame on job threads with subdivided dt (timeDelta @ +0x60). 
		// Used for clock advance + pose sampling ONLY - rig writes here dont work, the engine's snapshot applier (vfunc 7) rewrites rig locals right after every update.
		static void Hook_AnimGraphUpdate(void* a_this, RE::BSAnimationUpdateData* a_updateData);

		// BGSModelNode::Update (vfunc 2), called once per skeleton per frame from BSFadeNode::Update on scene-update threads. 
		// Stamping rig locals before the original runs is the right write point: that same call composes and commits them deterministically 
		// (and it keeps running for AI-frozen actors, whose AnimationManager updates stop).
		static uint64_t Hook_ModelNodeUpdate(
			RE::BGSModelNode* a_this, void* a_parentTransform, void* a_updateData, void* a_outputTransform);

		using AnimUpdateFn = void(void*, RE::BSAnimationUpdateData*);
		static inline AnimUpdateFn* _origAnimGraphUpdate = nullptr;

		using ModelNodeUpdateFn = uint64_t(RE::BGSModelNode*, void*, void*, void*);
		static inline ModelNodeUpdateFn* _origModelNodeUpdate = nullptr;

		std::shared_mutex stateLock;
		std::unordered_map<RE::TESObjectREFR*, std::shared_ptr<Graph>> graphs;
		std::vector<std::shared_ptr<Scene>> scenes;

		std::optional<PlaybackSink> GetPlaybackSink(PlaybackSinkId a_id) const;
		mutable std::shared_mutex _sinkLock;
		std::unordered_map<PlaybackSinkId, PlaybackSink> _playbackSinks;
		std::atomic<PlaybackSinkId> _nextPlaybackSinkId{ 1 };

		// Mirror of graphs.size(), refreshed after every mutation under unique stateLock.
		// Both hooks run for EVERY skeleton/manager in the game, game-wide, forever - this lets the idle case (no OSF playback) early out without touching stateLock at all.
		std::atomic<size_t> graphCount{ 0 };

		// Concrete playback identity and world generation. Deferred tasks carry both: playbackId
		// prevents actor-reuse ABA bugs, while worldEpoch invalidates every pre-load task at once.
		std::atomic<PlaybackId> _nextPlaybackId{ 1 };
		std::atomic<std::uint64_t> _worldEpoch{ 1 };

		// Stall watchdog bookkeeping (see StallWatchTick), all steady-clock ms, lock-free.
		std::atomic<std::int64_t> _stallLastHookMs{ 0 };  // last time the hook ran (a big jump = the game was paused/loading)
		std::atomic<std::int64_t> _stallArmedMs{ 0 };     // don't flag stalls before this time (post-resume grace)
		std::atomic<std::int64_t> _stallLastScanMs{ 0 };  // last scene scan (throttles the per-call work)

		// True between OnSaveBegin and OnSaveEnd: engine bout to serialize, so every kAnimationDriven set-point (initial placement + the per-frame re-assert) stand down.
		std::atomic<bool> _saveWindow{ false };
	};
}
