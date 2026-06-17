#pragma once

// Tracks live scenes in a generational int-handle table 
// fires lifecycle events (NODE_ENTER / NODE_EXIT / SCENE_END) through the SceneEventRelay. 
// For now a scene is just an id, its current node, a stage, and the participants; 
/// the heavier runtime (graph navigation, track scheduler, undo ledger) gets built on top of this later.

namespace OSF::Animation
{
	enum class SceneEndReason : std::uint8_t;  // Animation/Scene.h
}

namespace OSF::Scene
{
	class SceneRuntime
	{
	public:
		static SceneRuntime& GetSingleton();

		// Register the SceneRuntime with the GraphManager.
		void RegisterWithGraphManager();

		// callback for when a node's animation auto-ends.  (game thread)
		// Resolves the live handle owning the participants. and what triggered auto-edge (timer, loops, end) edge.
		// no match edge -> terminal completion (ends the scene).
		// Returns true if the scene was handled by the SceneRuntime, false otherwise (ex. direct StartScene without a graph).
		bool OnGraphAutoEnd(const std::vector<RE::Actor*>& a_participants, Animation::SceneEndReason a_reason);

		// Layer-A timed-cue callback (registered with GraphManager). Game thread. Resolves the
		// handle owning a_participants, dispatches each fired cue id as EVENT_CUE, then takes the
		// first matching trigger:<id> edge on the current node (transition, or end if "$end").
		void OnTimedCues(const std::vector<RE::Actor*>& a_participants, const std::vector<std::string>& a_cueIds);

		// Mint a handle, record (id, entry node, participants), fire NODE_ENTER. 
		// Returns the handle (0 = failed: table full).
		std::int32_t Start(std::string_view a_id, std::string_view a_entryNode, const std::vector<RE::Actor*>& a_participants);

		// Move to a different node: fire NODE_EXIT (old node) then NODE_ENTER (new node).
		// False if the handle is invalid.
		bool SetNode(std::int32_t a_scene, std::string_view a_node, std::int32_t a_stage);

		// Fire NODE_EXIT then SCENE_END, then release the handle. False if invalid.
		bool Stop(std::int32_t a_scene);

		// Stop the live scene a_actor participates in (actor convenience). False if none.
		bool StopForActor(RE::Actor* a_actor);

		// --- Navigation (def-backed scenes) ---
		// Start a scene from its *.scene.json definition: enter at the def's `entry` node.
		// Returns the handle (0 = no such scene def). Navigation works on these.
		std::int32_t StartFromDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants);

		// Start a registry PACK as a single-path scene: the pack's stages play as ONE GraphManager scene (the engine handles stage auto-advance); 
		// the handle wraps it and its terminal stage ends the scene (SCENE_END). a_startStage = initial pack stage.
		// 0 = unknown/unbuildable pack, play failure, or an actor is already in a scene.
		std::int32_t StartFromPack(std::string_view a_packId, const std::vector<RE::Actor*>& a_participants, std::int32_t a_startStage);

		// Start an ad-hoc files scene (the StartSceneFiles backing): one synthetic "main" node athat holds (co-located + synced by GraphManager). 
		// 0 = bad args, play failure, or an
		// actor is already in a scene.
		std::int32_t StartFromFiles(const std::vector<RE::Actor*>& a_participants,
			const std::vector<std::string>& a_files, float a_speed, float a_blendIn);

		// Start a def-backed scene with actors bound to NAMED roles (a_roles[i] = role for
		// a_actors[i]). Validates per §1.3 (unknown/duplicate role, null/duplicate actor, role
		// count, every declared role filled) and reorders actors into role-declaration order
		// before entering. 0 = no such scene def, a validation failure, or an actor is busy.
		std::int32_t StartFromDefRoles(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_actors,
			const std::vector<std::string>& a_roles);

		// Jump a LINEAR scene to stage a_stage (SCENE_DESIGN §1.4): a pack/files scene (delegates
		// to GraphManager's stage jump) or a def scene declaring `linearStages` (transitions to
		// the indexed node). False on a non-linear graph, out-of-range stage, or invalid handle.
		bool SetStage(std::int32_t a_scene, std::int32_t a_stage);

		// Take the current node's DEFAULT advance edge (transition, or end the scene if the edge targets "$end").
		// False if the handle is invalid or the node has no default advance edge (a default is never inferred).
		bool Advance(std::int32_t a_scene);

		// Take the current node's branchable advance edge whose id == a_edgeId. 
		// False if the handle is invalid or no such edge on the current node.
		bool Navigate(std::int32_t a_scene, std::string_view a_edgeId);

		// Branchable (advance) edges of the current node, for menus. Count, then id/label by
		// index (0..count). Sentinels: count 0 / id "" / label "" when invalid.
		std::int32_t EdgeCount(std::int32_t a_scene);
		std::string  EdgeId(std::int32_t a_scene, std::int32_t a_index);
		std::string  EdgeLabel(std::int32_t a_scene, std::int32_t a_index);

		// Lookups (contract sentinels): id/node "" if invalid; stage -1; actor→handle 0.
		std::string  GetId(std::int32_t a_scene);
		std::string  GetNode(std::int32_t a_scene);
		std::int32_t GetStage(std::int32_t a_scene);  // linear scenes only (§1.4); else -1
		std::int32_t GetSceneForActor(RE::Actor* a_actor);

		// Drop all live scenes (load teardown — invoked from GraphManager::StopAll via the
		// registered clear handler, so a stashed handle reads as dead after a load).
		void Clear();

	private:
		// What backs a live scene instance. kDef = a *.scene.json graph (navigable edges).
		// kPack/kFiles = single-path "main" scenes with no edges; their GraphManager scene's
		// terminal stage IS the scene ending (OnGraphAutoEnd ends them rather than taking an
		// edge). kFiles also synthesizes its id ("runtime.files:<handle>", §1.2).
		enum class Kind : std::uint8_t
		{
			kDef,
			kPack,
			kFiles
		};

		struct Slot
		{
			std::uint16_t           generation = 0;  // 0 = empty
			Kind                    kind = Kind::kDef;
			std::string             id;
			std::string             node;
			std::int32_t            stage = 0;
			std::vector<RE::Actor*> participants;
		};

		// token = (generation << 16) | slot ; token 0 = null (slot 0 gen>=1 → nonzero).
		static std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		// Caller holds _lock; null if the handle is stale/invalid.
		Slot* Resolve(std::int32_t a_handle);

		// Allocate a slot (exclusivity-checked), record it, return the handle (0 = table full
		// or an actor is already in a live scene). Does NOT play or fire — the caller plays
		// the animation + fires NODE_ENTER OUTSIDE _lock. Shared by every Start* entry point.
		std::int32_t MintSlot(Kind a_kind, std::string_view a_id, std::string_view a_node,
			std::int32_t a_stage, const std::vector<RE::Actor*>& a_participants);

		// Free the slot a_handle names (no events). Rollback for a start whose playback failed
		// after the handle was minted. Takes _lock itself.
		void ReleaseSlot(std::int32_t a_handle);

		// Caller holds _lock. The live slot a_actor participates in (and its token via a_token if non-null), or null. 
		// Single source for the actor-exclusivity invariant (one actor -> at most one live scene) and the actor->handle lookups.
		Slot* FindSlotForActor(RE::Actor* a_actor, std::int32_t* a_token);

		// Log + dispatch a lifecycle event through the relay.
		// Call OUTSIDE _lock (it enters the VM); the caller snapshots node/handle first.
		// For NODE_ENTER / NODE_EXIT it also dispatches the node's enter / exit cue-track
		// entries as EVENT_CUE (notification; trigger:<id> auto-take is a later increment).
		static void Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_anchor);

		// Dispatch one EVENT_CUE through the relay. Call OUTSIDE _lock.
		static void DispatchCue(std::int32_t a_handle, std::string_view a_node, std::string_view a_cue,
			std::string_view a_anchor, float a_time);

		// Dispatch a node's enter (a_enter) or exit cue-track entries as EVENT_CUE. Call OUTSIDE
		// _lock. No-op for a non-def scene or a node with no matching cues.
		static void DispatchLifecycleCues(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// Hands playback off to the GraphManager. Call these OUTSIDE _lock, with the participants already snapshotted. 
		// PlayNodeAnim looks up the node's `anim` id and copies the node's loop policy and timerSec onto the last stage of the plan it plays,
		// so the GraphManager auto-ends it and reports the timer/loops back through OnGraphAutoEnd. 
		// If the scene has no animation or the id won't play, it does nothing. 
		static void PlayNodeAnim(const std::vector<RE::Actor*>& a_participants, std::string_view a_sceneId, std::string_view a_nodeId);
		// StopGraph ends the participants' scene.
		static void StopGraph(const std::vector<RE::Actor*>& a_participants);

		std::mutex        _lock;
		std::vector<Slot> _slots;
		std::uint16_t     _nextGen = 1;
	};
}
