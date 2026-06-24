#pragma once

#include "Equipment/EquipmentService.h"  // Snapshot stored per-handle in the undo ledger
#include "Input/InputTypes.h"            // Grant stored per-handle for the input channel

#include <functional>

// Tracks live scenes in a generational int-handle table and fires lifecycle events
// (NODE_ENTER / NODE_EXIT / SCENE_END) through the SceneEventRelay. Each live scene is an id,
// its current node, a stage, and its participants, with graph navigation, the track scheduler,
// and the undo ledger built on top of that.

namespace OSF::Animation
{
	enum class SceneEndReason : std::uint8_t;  // Animation/Scene.h
	struct FiredMark;                          // Animation/Scene.h
}

namespace OSF::Registry
{
	struct ActionEntry;   // Registry/SceneRegistry.h
	struct SceneNode;     // Registry/SceneRegistry.h
	struct SceneEdge;     // Registry/SceneRegistry.h
}

namespace OSF::Camera
{
	enum class CameraMode : std::uint8_t;  // Camera/CameraService.h
}

namespace OSF::Scene
{
	class SceneRuntime
	{
	public:
		static SceneRuntime& GetSingleton();

		// Explicit world anchor for a scene (StartSceneAt). When set, every node's playback
		// anchors here instead of at participant[0], so a scene world-anchors to a thing
		// (furniture/bed/marker). Stored on the Slot so node transitions reuse it.
		struct AnchorOverride
		{
			bool         set = false;
			RE::NiPoint3 pos{};
			float        heading = 0.0f;  // radians
		};

		// Wire this runtime up to the GraphManager.
		void RegisterWithGraphManager();

		// Called on the game thread when a node's animation auto-ends. Resolves the live handle
		// owning the participants and which auto-edge fired (timer/loops/end); if no edge matches,
		// the scene completes and ends. Returns true if this runtime handled the scene, false
		// otherwise (e.g. a direct StartScene with no graph).
		bool OnGraphAutoEnd(const std::vector<RE::Actor*>& a_participants, Animation::SceneEndReason a_reason);

		// Timed-mark callback registered with the GraphManager. Runs on the game thread. Resolves
		// the handle owning a_participants, then decodes each fired mark by lane in the same-tick
		// order action -> camera -> sound -> cue: action-lane marks run the built-in/custom
		// mechanism; cue-lane marks dispatch EVENT_CUE and then take the first matching
		// trigger:<id> edge on the current node (transition, or end if "$end").
		void OnTimedMarks(const std::vector<RE::Actor*>& a_participants, const std::vector<Animation::FiredMark>& a_marks);

		// Mint a handle, record (id, entry node, participants), fire NODE_ENTER. An explicit
		// anchor (StartSceneAt) world-anchors the scene at a_anchor instead of at participant[0]
		// and is stored on the slot so node transitions reuse it. Returns the handle (0 = failed:
		// table full / an actor is already in a scene).
		std::int32_t Start(std::string_view a_id, std::string_view a_entryNode, const std::vector<RE::Actor*>& a_participants,
			const AnchorOverride& a_anchor = {});

		// Move to a different node: fire NODE_EXIT (old node) then NODE_ENTER (new node).
		// False if the handle is invalid.
		bool SetNode(std::int32_t a_scene, std::string_view a_node);

		// Fire NODE_EXIT then SCENE_END, then release the handle. False if invalid.
		bool Stop(std::int32_t a_scene);

		// Stop the live scene a_actor participates in (actor convenience). False if none.
		bool StopForActor(RE::Actor* a_actor);

		// --- Navigation (def-backed scenes) ---
		// Start a scene from its registered definition: enter at the def's `entry` node.
		// Returns the handle (0 = no such scene def). Navigation works on these.
		std::int32_t StartFromDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants);

		// Like StartFromDef, but world-anchored at (a_anchorPos, a_anchorHeading in RADIANS)
		// instead of at participant[0] (StartSceneAt). The anchor is stored on the slot so every
		// node transition reuses it.
		std::int32_t StartFromDefAt(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants,
			RE::NiPoint3 a_anchorPos, float a_anchorHeading);

		// Start an ad-hoc files scene (what StartSceneFiles is built on): a single synthetic "main"
		// node that holds, with the actors co-located and synced by the GraphManager. 0 = bad args,
		// play failure, or an actor is already in a scene.
		std::int32_t StartFromFiles(const std::vector<RE::Actor*>& a_participants,
			const std::vector<std::string>& a_files, float a_speed, float a_blendIn);

		// Start a def-backed scene with actors bound to NAMED roles (a_roles[i] = role for
		// a_actors[i]). Validates the binding (unknown/duplicate role, null/duplicate actor, role
		// count, every declared role filled) and reorders actors into role-declaration order
		// before entering. 0 = no such scene def, a validation failure, or an actor is busy.
		std::int32_t StartFromDefRoles(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_actors,
			const std::vector<std::string>& a_roles);

		// Jump a linear scene to stage a_stage: an ad-hoc files scene (delegates to the GraphManager's
		// stage jump) or a registry scene declaring `linearStages` (transitions to the indexed node).
		// False on a non-linear graph, an out-of-range stage, or an invalid handle.
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
		std::int32_t GetStage(std::int32_t a_scene);  // linear scenes only; else -1
		std::int32_t GetSceneForActor(RE::Actor* a_actor);

		// Drop all live scenes (load teardown). Invoked from GraphManager::StopAll via the
		// registered clear handler, so a stashed handle reads as dead after a load.
		void Clear();

	private:
		// A reversible side-effect a scene engaged, tracked in the per-handle undo ledger. On any
		// termination the ledger replays in reverse order, once, idempotently, so cleanup never
		// depends on an authored release. Mechanisms with cross-scene state (the control lock) keep
		// their own ref-count alongside the per-handle entry.
		enum class Mechanism : std::uint8_t
		{
			kControlLock,   // player control + camera lock (ref-counted across scenes)
			kFade,          // screen fade-to-black (undo = fade back in)
			kEquipment,     // hidden worn apparel (undo = re-equip; per-actor snapshots in the Slot)
			kCamera,        // held third-person camera lock (undo = release the standalone camera lock)
			kCameraState,   // alt camera posture: free-fly / vanity orbit (undo = release the state override)
			kWeapon,        // sheathed weapons (undo = re-draw; per-actor list in the Slot)
			kInputChannel   // player director-input channel (undo = release the InputService grant; Grant in the Slot)
		};

		struct Slot
		{
			std::uint16_t           generation = 0;  // 0 = empty
			std::string             id;
			std::string             node;
			std::vector<RE::Actor*> participants;
			AnchorOverride          anchor;  // StartSceneAt world anchor (unset = anchor at participant[0])
			// Ordered list of reversible mechanisms this scene engaged (at most one entry per
			// Mechanism — record is idempotent). Replayed in reverse on termination.
			std::vector<Mechanism>  ledger;
			// kEquipment's per-actor state: apparel this scene hid (restored on undo). Lives
			// here (not the ledger entry) so the ledger stays a plain ordered type list — same
			// pattern as control-lock keeping its count in _controlLockCount.
			std::vector<std::pair<RE::Actor*, Equipment::Snapshot>> hiddenEquip;
			// kWeapon's per-actor state: actors this scene sheathed (re-drawn on undo). Same
			// out-of-ledger pattern as hiddenEquip.
			std::vector<RE::Actor*> sheathedWeapon;
			// kInputChannel's data: the director-input grant this scene handed the InputService (engaged on record, released on undo).
			Input::Grant            grant;
		};

		// lock-free copy of slot fields for dispatch/lookup paths reading out of lock
		struct SlotView
		{
			std::string             id;
			std::string             node;
			std::vector<RE::Actor*> participants;
		};

		// The id + resolved label of one branchable (advance) edge, for the menu accessors.
		struct AdvanceEdgeInfo
		{
			std::string id;
			std::string label;  // labelKey if set, else the literal label
		};

		// token = (generation << 16) | slot ; token 0 = null (slot 0 gen >= 1 -> nonzero).
		static std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		// Caller holds _lock; null if the handle is stale/invalid.
		Slot* Resolve(std::int32_t a_handle);

		// Allocate a slot (exclusivity-checked), record it, return the handle (0 = table full or an actor is already in a live scene). 
		// Does NOT play or fire, the caller plays the animation + fires NODE_ENTER OUTSIDE _lock. Shared by every Start* entry point.
		std::int32_t MintSlot(std::string_view a_id, std::string_view a_node, const std::vector<RE::Actor*>& a_participants);

		// Free the slot a_handle names (no events). Rollback for a start whose playback failed
		// after the handle was minted. Takes _lock itself.
		void ReleaseSlot(std::int32_t a_handle);

		// Caller holds _lock. The live slot a_actor participates in (and its token via a_token if
		// non-null), or null. This is the single source for the one-actor-one-scene invariant and
		// the actor->handle lookups.
		Slot* FindSlotForActor(RE::Actor* a_actor, std::int32_t* a_token);

		// Snapshot the slot a_handle names into a_out under _lock; 
		// false (a_out left default) if the handle is stale. 
		// The single source of the "lock, Resolve, copy out, unlock" preamble the out-of-lock dispatch / lookup paths share.
		bool SnapshotSlot(std::int32_t a_handle, SlotView& a_out);

		// Log + dispatch a lifecycle event through the relay.
		// Call OUTSIDE _lock (it enters the VM); the caller snapshots node/handle first.
		// For NODE_ENTER / NODE_EXIT it also dispatches the node's enter / exit cue-track
		// entries as EVENT_CUE.
		static void Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_anchor);

		// Dispatch one EVENT_CUE through the relay. Call OUTSIDE _lock.
		static void DispatchCue(std::int32_t a_handle, std::string_view a_node, std::string_view a_cue,
			std::string_view a_anchor, float a_time);

		// Dispatch a node's enter (a_enter) or exit cue-track entries as EVENT_CUE. Call OUTSIDE
		// _lock. No-op for a non-def scene or a node with no matching cues.
		static void DispatchLifecycleCues(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// Play a node's enter (a_enter) or exit sound-track entries. Numeric/end-timed sounds
		// play via OnTimedMarks instead. Call OUTSIDE _lock. No-op for a non-def scene.
		static void DispatchLifecycleSounds(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// Play one content-neutral sound spec, positioned at a_role's actor (or the player when
		// the role resolves nothing). Shared by the sound lane + osf.voice.play.
		static void PlaySound(std::int32_t a_handle, std::string_view a_spec, std::string_view a_role, float a_volume);

		// Engage a node's enter (a_enter) or exit camera-track entries. Numeric/end-timed camera
		// entries engage via OnTimedMarks instead. Call OUTSIDE _lock. No-op for a non-def scene.
		static void DispatchLifecycleCamera(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// Engage one camera state (held, ledger-tracked, auto-restored).
		// "thirdperson_hold" -> the standalone camera lock (kCamera); "freefly" / "vanity_orbit" -> a PlayerCamera state override (kCameraState).
		static void RunCamera(std::int32_t a_handle, std::string_view a_state, bool a_hasPlayer);

		// Run a node's enter (a_enter) or exit action-track entries (the lifecycle anchors).
		// Numeric/end-timed actions run via OnTimedMarks instead. Call OUTSIDE _lock. No-op for
		// a non-def scene.
		static void DispatchLifecycleActions(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// Execute one action entry: built-in osf.* mechanisms (control.lock/release executed;
		// the rest recognized + logged), custom actions emitted as EVENT_ACTION. Shared by the
		// lifecycle + timed dispatch paths. Call OUTSIDE _lock (may Acquire/Release the lock).
		// a_hasPlayer = the player is a participant (gates player-only mechanisms).
		static void RunAction(std::int32_t a_handle, std::string_view a_node, const Registry::ActionEntry& a_action,
			std::string_view a_anchor, bool a_hasPlayer);

		// Dispatch one EVENT_ACTION (custom action notification) through the relay.
		static void DispatchAction(std::int32_t a_handle, std::string_view a_node, std::string_view a_type,
			std::string_view a_role, std::string_view a_anchor);

		// Undo ledger. RecordMechanism engages and records a reversible mechanism for a scene
		// (idempotent per scene+mechanism). UndoMechanism reverses one mechanism for a scene
		// (idempotent: applies its undo and drops the entry) — the authored osf.*.release /
		// osf.fade.in path. ReplayLedger reverses the whole ledger in reverse order, once,
		// idempotently — called from the single Fire(SCENE_END) point so cleanup runs on every
		// termination path. The control lock keeps a cross-scene ref-count (_controlLockCount).
		void RecordMechanism(std::int32_t a_handle, Mechanism a_mech);
		void UndoMechanism(std::int32_t a_handle, Mechanism a_mech);
		void ReplayLedger(std::int32_t a_handle);

		// The participant bound to a_role (by role-declaration order; an empty role -> the first
		// participant). nullptr if the handle is dead or the role is unknown.
		RE::Actor* ResolveRoleActor(std::int32_t a_handle, std::string_view a_role);

		// Record apparel this scene hid for a_actor (osf.equipment.hide), adding the kEquipment
		// ledger entry. Call OUTSIDE _lock (it locks). a_snapshot moved in.
		void RecordHiddenEquip(std::int32_t a_handle, RE::Actor* a_actor, Equipment::Snapshot a_snapshot);

		// Record a weapon this scene sheathed for a_actor (osf.weapon.sheathe), adding the
		// kWeapon ledger entry. Call OUTSIDE _lock (it locks).
		void RecordSheathedWeapon(std::int32_t a_handle, RE::Actor* a_actor);

		// Engage an alternate camera posture (free-fly / vanity orbit) for a_handle. Call OUTSIDE _lock (it locks, then drives the service outside it).
		void RecordCameraState(std::int32_t a_handle, Camera::CameraMode a_mode);

		// Store a_grant on the slot, add the kInputChannel ledger entry (idempotent), and engage the InputService director channel. 
		// Call OUTSIDE _lock (it locks, then engages outside it).
		void RecordInputChannel(std::int32_t a_handle, const Input::Grant& a_grant);

		// Hands playback off to the GraphManager. Call these OUTSIDE _lock, with the participants
		// already snapshotted. PlayNodeAnim resolves the node's playable (inline `stages` or a `use`
		// reference, via SceneRegistry::BuildNodePlan) and copies the node's loop policy and timerSec
		// onto the last stage of the plan it plays, so the GraphManager auto-ends it and reports the
		// timer/loops back through OnGraphAutoEnd. If the scene has no playable, or it won't play, it
		// does nothing.
		static void PlayNodeAnim(const std::vector<RE::Actor*>& a_participants, std::string_view a_sceneId, std::string_view a_nodeId);
		// StopGraph ends the participants' scene.
		static void StopGraph(const std::vector<RE::Actor*>& a_participants);

		// Default player lock on scene start: when the player is among a_participants and policy keeps it on. 
		// The caller resolves a_lockPlayer from the scene def (`lockPlayer`) or pack;
		void EngageDefaultPlayerLock(std::int32_t a_handle, bool a_lockPlayer, const std::vector<RE::Actor*>& a_participants);

		// Default actor strip on scene start: when a_stripActors (caller-resolved policy), hide EVERY participant's worn apparel (base skin kept). Resolved like a_lockPlayer above.
		void StripDefaultActors(std::int32_t a_handle, bool a_stripActors, const std::vector<RE::Actor*>& a_participants);

		// Player director-input on scene start: when the def declares a `playerControl` block and the player is a participant, hand the InputService a Grant (ledger-tracked via kInputChannel so it releases on any termination). 
		// No-op for non-def / no playerControl / player-absent scenes.
		void EngageDefaultPlayerControl(std::int32_t a_handle, std::string_view a_defId, const std::vector<RE::Actor*>& a_participants);

		// The node-transition lifecycle, shared by every transition path (SetNode / Advance /
		// Navigate / OnGraphAutoEnd / cue-trigger): fire NODE_EXIT for a_oldNode, then either end
		// the scene (StopGraph + SCENE_END + release the handle) when a_end, or play a_newNode and
		// fire NODE_ENTER. Call OUTSIDE _lock with the slot's id + participants already snapshotted.
		void ApplyTransition(std::int32_t a_handle, std::string_view a_oldNode, std::string_view a_newNode,
			bool a_end, std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants);

		// Shared body of Advance / Navigate. Under _lock: resolve the def node and let a_selectEdge
		// pick an outgoing edge; on a hit, snapshot the transition args (mutating the slot's node for
		// a non-"$end" target) and run ApplyTransition OUTSIDE _lock. False if the handle is invalid,
		// not def-backed, or a_selectEdge finds no edge. Centralizes the "$end vs next node" +
		// snapshot-under-lock contract that Advance and Navigate would otherwise each copy.
		bool TakeEdge(std::int32_t a_scene, const std::function<const Registry::SceneEdge*(const Registry::SceneNode&)>& a_selectEdge);

		// The current node's branchable (advance) edges, snapshotted under _lock for the menu accessors (EdgeCount / EdgeId / EdgeLabel). 
		// Empty if the handle is invalid or the node isn't def-backed.
		std::vector<AdvanceEdgeInfo> AdvanceEdges(std::int32_t a_scene);

		std::mutex        _lock;
		std::vector<Slot> _slots;
		std::uint16_t     _nextGen = 1;
		std::int32_t      _controlLockCount = 0;  // # of live scenes holding the player control lock
	};
}
