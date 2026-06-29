#pragma once

// Graph storage plus the per-frame hooks.
// The main hook surface is a vtable patch on AnimationManager::Update (vfunc 4)
// This graph/manager structure is adapted, in reduced form, from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh).

#include "Animation/Graph.h"
#include "Animation/Scene.h"

#include <functional>

namespace OSF::Animation
{
	class GraphManager
	{
	public:
		static GraphManager& GetSingleton();

		// This is hook scene runtime sets for autoadvance. called on game thread when anchored scene auto ends at final stage.
		// This manager doesnt know scene graph, just hands runtime the actors and reasons.
		// returns true if runtime took over what happens (move to next node or end scene). false means manager stops the scene itself, set at load before scene runs.
		using SceneAutoEndHandler = std::function<bool(const std::vector<RE::Actor*>&, SceneEndReason)>;
		void SetSceneAutoEndHandler(SceneAutoEndHandler a_handler) { _autoEndHandler = std::move(a_handler); }

		// Hook called from StopAll (every teardown path) so scene runtime can drop handle table.
		// its scene handles hold raw Actor* participants that would be invalidated. 
		using SceneClearHandler = std::function<void()>;
		void SetSceneClearHandler(SceneClearHandler a_handler) { _clearHandler = std::move(a_handler); }

		// Hook called on Game Thread when scene stage fires marks (numeric or end) on any lane.
		// hands runtime opaque (lane, token) marks and actors, and runtime decodes them (cue -> EVENT_CUE + trigger edges. action -> notify, sound/camera -> relevant service)
		using SceneTimedMarkHandler = std::function<void(const std::vector<RE::Actor*>&, const std::vector<FiredMark>&)>;
		void SetSceneTimedMarkHandler(SceneTimedMarkHandler a_handler) { _timedMarkHandler = std::move(a_handler); }

		// Installs the AnimationManager::Update vtable hook.
		void InstallHooks();

		// Registers a MenuOpenCloseEvent sink that freezes playback while the console is open.
		// console pauses the world but does NOT set Main::isGameMenuPaused, Call once after the UI singleton exists (kPostPostDataLoad).
		void RegisterConsolePauseSink();

		// True when both vtable hooks verified and patched (the core playback path is live).
		bool HooksInstalled() const { return _origAnimGraphUpdate && _origModelNodeUpdate; }

		// Synchronously loads a_file (relative to the game's Data folder, or absolute) and starts playing it on a_actor.
		bool PlayAnimation(RE::Actor* a_actor, std::string_view a_file, std::string_view a_animId);

		// Starts a fade-out (the graph keeps sampling while its stamp weight ramps to 0, landing on the engine's live pose, then removes itself).
		// Refuses for scene participants, use StopScene in that case.
		bool StopAnimation(RE::Actor* a_actor);

		// Erases actors's graph if its fade-out has fully elapsed (no-op if it was replayed meanwhile). Called from the deferred removal task.
		void RemoveFadedGraph(RE::TESObjectREFR* a_refr);

		// Starts a staged synced scene from a ScenePlan. every stages clips are loaded up front. (stage switches are pointer swaps, not file IO)
		// Refuses partial scenes. stages auto-advance on timers or loop count targets.
		// after last stage the scene stops itself (on game thread)
		bool PlaySceneStaged(const std::vector<RE::Actor*>& a_actors, const ScenePlan& a_plan, int32_t a_startStage);

		// Jumps the scene containing a_actor to the given stage (0-based);
		// false if the actor is not in a scene or the stage is out of range.
		bool SetSceneStage(RE::Actor* a_actor, int32_t a_stage);

		// Stops the scene that a_actor participates in (all its participants).
		bool StopScene(RE::Actor* a_actor);

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

		// Pin a SOLO graph to a world point + heading (degrees), with a rootMode (0 pin / 1 additive / 2 follow). 
		// Also moves the capsule there. Refused for scene participants. ClearAnchor releases it.
		bool SetAnchor(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode);
		bool ClearAnchor(RE::Actor* a_actor);

		// Bring N playing solo graphs together. basically promotes them into a single clock-shared scene at actor[0]'s transform.
		// assumes the clips baked root offsets arange actors arouns same origin point. (if anchoring)
		bool Sync(const std::vector<RE::Actor*>& a_actors, bool a_anchor = true);

		// First OTHER participant in a_actor's live scene. null if a_actor has no live graph, is not in a scene, or is the only participant.
		RE::Actor* GetScenePartner(RE::Actor* a_actor);


		// Solo multi-stage sequence (no anchor)
		bool PlaySequence(RE::Actor* a_actor, const std::vector<std::string>& a_files, const std::vector<std::string>& a_animIds, const std::vector<int32_t>& a_loops, const std::vector<float>& a_blends, bool a_loopWhole);

		// Stops a scene by pointer if it is still live (no-op otherwise). Used by the deferred auto-end task; 
		// the caller keeps the Scene alive via shared_ptr so the pointer can never be stale.
		void StopSceneByPtr(Scene* a_scene);

		// drops all inmemory scene + graph state immediately. (no fade or actor mutation)
		// call when game loads save to nuke our state and reset from the world which is authority.
		// dispatches end per torn-down scene (deferred, lands after load). Does not restore equipment/movement
		void StopAll(const char* a_reason);

	private:
		// Detaches all participants of a_scene (revert movement mode, drop graphs) and removes it from `scenes`. Caller holds stateLock unique.
		void StopSceneLocked(Scene* a_scene);

		// Per-graph upkeep run from Hook_AnimGraphUpdate right after sampling, while the caller holds stateLock (shared) and the graph's own lock.
		// Each defers any game-thread-only follow-up via the SFSE task queue.
		void QueueAutoEndIfFinished(Graph& a_graph);                   // last stage ran out -> StopScene
		void QueueTimedMarksIfFired(Graph& a_graph);                   // stage fired timed marks -> mark handler
		void QueueFadeRemovalIfDone(Graph& a_graph);                  // fade-out elapsed -> RemoveFadedGraph
		void HoldAnchoredParticipant(Graph& a_graph, RE::TESObjectREFR* a_refr);  // keep an anchored scene NPC animation-driven (no AI walk-back)

		// Defer a scene's end to the game thread: the runtime auto-end handler takes over (advance/end +
		// ledger replay), else we stop it ourselves. Idempotent via Scene::endQueued; the shared_ptr keeps
		// the Scene alive across the deferral. Shared by QueueAutoEndIfFinished and the stall watchdog.
		void QueueSceneEndDeferred(std::shared_ptr<Scene> a_scene);

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
		static uint64_t Hook_ModelNodeUpdate(RE::BGSModelNode* a_this, void* a_parentTransform, void* a_updateData);

		using AnimUpdateFn = void(void*, RE::BSAnimationUpdateData*);
		static inline AnimUpdateFn* _origAnimGraphUpdate = nullptr;

		using ModelNodeUpdateFn = uint64_t(RE::BGSModelNode*, void*, void*);
		static inline ModelNodeUpdateFn* _origModelNodeUpdate = nullptr;

		std::shared_mutex stateLock;
		std::unordered_map<RE::TESObjectREFR*, std::shared_ptr<Graph>> graphs;
		std::vector<std::shared_ptr<Scene>> scenes;

		// Auto-advance hook (see SetSceneAutoEndHandler). Empty = standalone behaviour, where auto-end just stops the scene.
		SceneAutoEndHandler _autoEndHandler;

		// Handle-table drop hook (see SetSceneClearHandler). Empty = no scene runtime registered.
		SceneClearHandler _clearHandler;

		// Timed-mark hook (see SetSceneTimedMarkHandler). Empty = marks are dropped.
		SceneTimedMarkHandler _timedMarkHandler;

		// Mirror of graphs.size(), refreshed after every mutation under unique stateLock.
		// Both hooks run for EVERY skeleton/manager in the game, game-wide, forever - this lets the idle case (no OSF playback) early out without touching stateLock at all.
		std::atomic<size_t> graphCount{ 0 };

		// Stall watchdog bookkeeping (see StallWatchTick), all steady-clock ms, lock-free.
		std::atomic<std::int64_t> _stallLastHookMs{ 0 };  // last time the hook ran (a big jump = the game was paused/loading)
		std::atomic<std::int64_t> _stallArmedMs{ 0 };     // don't flag stalls before this time (post-resume grace)
		std::atomic<std::int64_t> _stallLastScanMs{ 0 };  // last scene scan (throttles the per-call work)
	};
}
