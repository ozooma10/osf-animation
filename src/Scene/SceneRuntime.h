#pragma once

// Phase-A scene-instance spine (docs/SCENE_DESIGN.md §1.2 / §2.5). A generational
// int-handle table of live scenes; lifecycle (NODE_ENTER / NODE_EXIT / SCENE_END) is fired
// through the SceneEventRelay. The real Layer-B runtime (graph navigation, track scheduler,
// the undo ledger) grows on top of this; today a "scene" is just id + current node + stage +
// participants, driven by the debug natives (the StartScene*/GraphManager handle-mint
// integration, and the handle-based StopScene/GetSceneStage that collide with the existing
// actor-keyed natives, are a later — signature-changing — slice).

namespace OSF::Scene
{
	class SceneRuntime
	{
	public:
		static SceneRuntime& GetSingleton();

		// Mint a handle, record (id, entry node, participants), fire NODE_ENTER. Returns the
		// handle (0 = failed: table full).
		std::int32_t Start(std::string_view a_id, std::string_view a_entryNode,
			const std::vector<RE::Actor*>& a_participants);

		// Move to a different node: fire NODE_EXIT (old node) then NODE_ENTER (new node).
		// False if the handle is invalid.
		bool SetNode(std::int32_t a_scene, std::string_view a_node, std::int32_t a_stage);

		// Fire NODE_EXIT then SCENE_END, then release the handle. False if invalid.
		bool Stop(std::int32_t a_scene);

		// --- Navigation (def-backed scenes) ---
		// Start a scene from its *.scene.json definition: enter at the def's `entry` node.
		// Returns the handle (0 = no such scene def). Navigation works on these.
		std::int32_t StartFromDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants);

		// Take the current node's DEFAULT advance edge (transition, or end the scene if the
		// edge targets "$end"). False if the handle is invalid or the node has no default
		// advance edge (a default is never inferred).
		bool Advance(std::int32_t a_scene);

		// Take the current node's branchable advance edge whose id == a_edgeId. False if the
		// handle is invalid or no such edge on the current node.
		bool Navigate(std::int32_t a_scene, std::string_view a_edgeId);

		// Branchable (advance) edges of the current node — for menus. Count, then id/label by
		// index (0..count). Sentinels: count 0 / id "" / label "" when invalid.
		std::int32_t EdgeCount(std::int32_t a_scene);
		std::string  EdgeId(std::int32_t a_scene, std::int32_t a_index);
		std::string  EdgeLabel(std::int32_t a_scene, std::int32_t a_index);

		// Lookups (contract sentinels): id/node "" if invalid; stage -1; actor→handle 0.
		std::string  GetId(std::int32_t a_scene);
		std::string  GetNode(std::int32_t a_scene);
		std::int32_t GetStage(std::int32_t a_scene);
		std::int32_t GetSceneForActor(RE::Actor* a_actor);

		// Drop all live scenes (load teardown — wired into StopAll in a later slice).
		void Clear();

	private:
		struct Slot
		{
			std::uint16_t           generation = 0;  // 0 = empty
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

		// Log + dispatch a lifecycle event through the relay. Call OUTSIDE _lock (it enters
		// the VM); the caller snapshots node/handle first.
		static void Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_anchor);

		// Playback bridge to Layer A (GraphManager). Call OUTSIDE _lock with snapshotted
		// participants. PlayNodeAnim resolves the node's `anim` id and plays it (no-op for a
		// synthetic/no-anim scene or an unplayable id); StopGraph ends the participants' scene.
		static void PlayNodeAnim(const std::vector<RE::Actor*>& a_participants, std::string_view a_sceneId, std::string_view a_nodeId);
		static void StopGraph(const std::vector<RE::Actor*>& a_participants);

		std::mutex        _lock;
		std::vector<Slot> _slots;
		std::uint16_t     _nextGen = 1;
	};
}
