#pragma once

// Graph storage plus the per-frame hooks.
// The main hook surface is a vtable patch on AnimationManager::Update (vfunc 4), the
// per-graph per-frame evaluation point. The graph/manager structure is adapted, in
// reduced form, from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh).

#include "Animation/Graph.h"
#include "Animation/Scene.h"

#include <functional>

namespace OSF::Animation
{
	class GraphManager
	{
	public:
		static GraphManager& GetSingleton();

		// Hook the scene runtime sets for auto-advance. Called on the game thread when an
		// anchored scene auto-ends at its final stage (timer or loop target). This manager
		// knows nothing about scene graphs — it just hands the runtime the participant actors
		// and the reason. Returns true if the runtime took over what happens next (move to the
		// next node, or end its own scene); false (or no handler) -> the manager stops the scene
		// itself. Set once at load, before any scene runs.
		using SceneAutoEndHandler = std::function<bool(const std::vector<RE::Actor*>&, SceneEndReason)>;
		void SetSceneAutoEndHandler(SceneAutoEndHandler a_handler) { _autoEndHandler = std::move(a_handler); }

		// Hook called from StopAll (every load-teardown path) so the scene runtime can drop its
		// handle table — its scene handles hold raw Actor* participants that a world-replacing
		// load invalidates. Like the auto-end hook, the dependency is inverted: this manager
		// never names the runtime directly.
		using SceneClearHandler = std::function<void()>;
		void SetSceneClearHandler(SceneClearHandler a_handler) { _clearHandler = std::move(a_handler); }

		// Hook called on the game thread when a scene's stage fires timed marks (numeric or end)
		// on any lane. This manager stays content-neutral: it hands the runtime the opaque
		// (lane, token) marks and the participant actors, and the runtime decodes them (cue ->
		// EVENT_CUE + trigger edges, action -> built-in/notify, sound/camera -> services). Same
		// inverted dependency as the auto-end hook.
		using SceneTimedMarkHandler = std::function<void(const std::vector<RE::Actor*>&, const std::vector<FiredMark>&)>;
		void SetSceneTimedMarkHandler(SceneTimedMarkHandler a_handler) { _timedMarkHandler = std::move(a_handler); }

		// Installs the AnimationManager::Update vtable hook. Verifies the
		// resolved slot actually points at the expected function before
		// patching (guards against silently re-bound AddressLib IDs).
		void InstallHooks();

		// True when both vtable hooks verified and patched (the core playback
		// path is live). False = the binding gates refused this game version.
		bool HooksInstalled() const { return _origAnimGraphUpdate && _origModelNodeUpdate; }

		// Synchronously loads a_file (relative to the game's Data folder, or
		// absolute) and starts playing it on a_actor.
		bool PlayAnimation(RE::Actor* a_actor, std::string_view a_file, std::string_view a_animId);

		// Starts a fade-out (the graph keeps sampling while its stamp weight
		// ramps to 0, landing on the engine's live pose, then removes itself).
		// Refuses for scene participants — use StopScene.
		bool StopAnimation(RE::Actor* a_actor);

		// Erases a_actor's graph if its fade-out has fully elapsed (no-op if
		// it was replayed meanwhile). Called from the deferred removal task.
		void RemoveFadedGraph(RE::TESObjectREFR* a_refr);

		// Starts a staged synced scene from a plan. EVERY stage's clips are loaded
		// up front — stage switches on the job threads are pure pointer swaps, no
		// file IO. Refuses partial scenes. Stages auto-advance on their timers or
		// loop-count targets; after the last timed/loop-counted stage the scene
		// stops itself (deferred to the game thread). a_startStage picks the
		// initial stage. Anchoring/placement is driven by a_plan.anchored.
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

		// Current source file path playing on a_actor (as the caller gave it),
		// or "" if the actor has no live graph.
		std::string GetCurrentAnimation(RE::Actor* a_actor);

		// Per-graph playback speed (1.0 = authored, 0 = freeze; clamped 0..100).
		// For a scene participant this sets/reads the shared scene clock speed.
		bool SetSpeed(RE::Actor* a_actor, float a_speed);
		float GetSpeed(RE::Actor* a_actor);

		// Pin a SOLO graph to a world point + heading (degrees), with a rootMode
		// (0 pin / 1 additive / 2 follow). Also moves the
		// capsule there. Refused for scene participants. ClearAnchor releases it.
		bool SetAnchor(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode);
		bool ClearAnchor(RE::Actor* a_actor);

		// Put N already-playing SOLO graphs on one shared clock so they stay
		// frame-locked. Scene participants are skipped; needs >= 2 playable graphs.
		bool Sync(const std::vector<RE::Actor*>& a_actors);

		// Solo multi-phase sequence (primitive: no anchor/policy, event-free). Phase
		// i plays a_files[i] looping a_loops[i] times then advances; a_blends[i] =
		// blend-in secs. a_loopWhole restarts at phase 0 after the last. Parallel
		// arrays, equal non-zero length.
		bool PlaySequence(RE::Actor* a_actor, const std::vector<std::string>& a_files,
			const std::vector<int32_t>& a_loops, const std::vector<float>& a_blends, bool a_loopWhole);

		// Stops a scene by pointer if it is still live (no-op otherwise). Used
		// by the deferred auto-end task; the caller keeps the Scene alive via
		// shared_ptr so the pointer can never be stale.
		void StopSceneByPtr(Scene* a_scene);

		// Save-safety: drops ALL in-memory scene + graph state immediately (no
		// fade, no actor mutation). Call when the game loads a save — the
		// loaded world is authoritative for actor state, and our graphs are
		// anchored in the world that was just discarded, so leaving them live
		// renders stale poses pinned to a dead anchor. Dispatches "end" per
		// torn-down scene (deferred, lands after the load). Does NOT restore
		// equipment/movement: the snapshot may belong to a different timeline
		// than the loaded save, and re-equipping then would be wrong — that's
		// a known limitation of not serializing this state. Triggers:
		// SaveLoadEvent begin for world-replacing loads (safe, pre-teardown),
		// TESLoadGameEvent as a late backstop, and OSF.NotifyGameLoaded() as a
		// manual fallback.
		void StopAll(const char* a_reason);

	private:
		// Detaches all participants of a_scene (revert movement mode, drop
		// graphs) and removes it from `scenes`. Caller holds stateLock unique.
		void StopSceneLocked(Scene* a_scene);

		// Per-graph upkeep run from Hook_AnimGraphUpdate right after sampling,
		// while the caller holds stateLock (shared) and the graph's own lock.
		// Each defers any game-thread-only follow-up via the SFSE task queue.
		void QueueAutoEndIfFinished(Graph& a_graph);                   // last stage ran out -> StopScene
		void QueueTimedMarksIfFired(Graph& a_graph);                   // stage fired timed marks -> mark handler
		void QueueFadeRemovalIfDone(Graph& a_graph);                  // fade-out elapsed -> RemoveFadedGraph
		void LogSceneDiag(Graph& a_graph, RE::TESObjectREFR* a_refr);  // throttled scene diagnostics

		// AnimationManager::Update (vfunc 4). Runs per graph ~7x per render
		// frame on job threads with subdivided dt (timeDelta @ +0x60). Used for
		// clock advance + pose sampling ONLY — rig writes here are dead by
		// design: the engine's snapshot applier (vfunc 7) rewrites rig locals
		// right after every update.
		static void Hook_AnimGraphUpdate(void* a_this, RE::BSAnimationUpdateData* a_updateData);

		// BGSModelNode::Update (vfunc 2), called once per skeleton per frame
		// from BSFadeNode::Update on scene-update threads. Stamping rig locals
		// before the original runs is the right write point: that same call composes and
		// commits them deterministically (and it keeps running for AI-frozen
		// actors, whose AnimationManager updates stop).
		static uint64_t Hook_ModelNodeUpdate(RE::BGSModelNode* a_this, void* a_parentTransform, void* a_updateData);

		using AnimUpdateFn = void(void*, RE::BSAnimationUpdateData*);
		static inline AnimUpdateFn* _origAnimGraphUpdate = nullptr;

		using ModelNodeUpdateFn = uint64_t(RE::BGSModelNode*, void*, void*);
		static inline ModelNodeUpdateFn* _origModelNodeUpdate = nullptr;

		std::shared_mutex stateLock;
		std::unordered_map<RE::TESObjectREFR*, std::shared_ptr<Graph>> graphs;
		std::vector<std::shared_ptr<Scene>> scenes;

		// Auto-advance hook (see SetSceneAutoEndHandler). Empty = standalone behaviour,
		// where auto-end just stops the scene.
		SceneAutoEndHandler _autoEndHandler;

		// Handle-table drop hook (see SetSceneClearHandler). Empty = no scene runtime registered.
		SceneClearHandler _clearHandler;

		// Timed-mark hook (see SetSceneTimedMarkHandler). Empty = marks are dropped.
		SceneTimedMarkHandler _timedMarkHandler;

		// Mirror of graphs.size(), refreshed after every mutation under unique
		// stateLock. Both hooks run for EVERY skeleton/manager in the game,
		// game-wide, forever — this lets the idle case (no OSF playback) early
		// out without touching stateLock at all.
		std::atomic<size_t> graphCount{ 0 };
	};
}
