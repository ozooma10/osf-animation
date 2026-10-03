#pragma once

#include "Equipment/EquipmentService.h"
#include "Equipment/GearRegistry.h"
#include "Input/InputTypes.h"
#include "Props/PropService.h"
#include "Registry/SceneRegistry.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

namespace OSF::Camera
{
	enum class CameraMode : std::uint8_t;
}

namespace OSF::Scene
{
	class SceneRuntime
	{
	public:
		static SceneRuntime& GetSingleton();

		// When set, every node anchors here instead of at participant[0].
		struct AnchorOverride
		{
			bool         set = false;
			RE::NiPoint3 pos{};
			float        heading = 0.0f;  // radians
		};

		struct StartOverrides
		{
			std::optional<bool> hideApparel;
			std::optional<bool> playerInputLock;
			std::optional<bool> fade;
			std::optional<bool> sceneControls;     // false revokes OSF scene commands
			std::optional<std::string> camera;  // "none" = vanilla camera; when set, node cameras are suppressed
			std::optional<Animation::WorldPlacementMode> worldPlacement;
			float               loopScale = 1.0f;  // multiplies loop-driven (loops>0) stages only, floored
		};

		void RegisterWithGraphManager();

		// Game thread. If no auto-edge matches, the scene completes. False if not a runtime scene.
		bool OnGraphAutoEnd(Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_participants,
			Animation::PlaybackEndReason a_reason);

		// Game thread. Same-tick lane order: action -> camera -> sound -> cue; a cue then takes the
		// first matching trigger:<id> edge.
		void OnTimedMarks(Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_participants,
			const std::vector<Animation::FiredMark>& a_marks);

		bool Stop(std::int32_t a_scene);
		bool StopForActor(RE::Actor* a_actor);

		// Enters at the def's `entry`, or a_entryNode when non-empty. 0 = unknown scene or node.
		std::int32_t StartFromDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants, const StartOverrides& a_over = {}, std::string_view a_entryNode = {});

		// a_anchorHeading in radians.
		std::int32_t StartFromDefAt(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants,
			RE::NiPoint3 a_anchorPos, float a_anchorHeading, const StartOverrides& a_over = {}, std::string_view a_entryNode = {});

		// a_plan must already be actor-aligned.
		std::int32_t StartFromPlan(const std::vector<RE::Actor*>& a_participants, Animation::PlaybackPlan a_plan,
			std::int32_t a_startStage, const AnchorOverride& a_anchor = {}, const StartOverrides& a_over = {});

		// a_roles[i] is a_actors[i]'s role; actors are reordered into role-declaration order.
		std::int32_t StartFromDefRoles(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_actors,
			const std::vector<std::string>& a_roles, const AnchorOverride& a_anchor = {}, const StartOverrides& a_over = {},
			std::string_view a_entryNode = {});

		// Ad-hoc files scenes, or registry scenes declaring `linearStages`.
		bool SetStage(std::int32_t a_scene, std::int32_t a_stage);

		// Default advance edge only; a default is never inferred.
		bool Advance(std::int32_t a_scene);

		bool Navigate(std::int32_t a_scene, std::string_view a_edgeId);

		// Wheel emotes: any edge out of the current node ends the scene, so advance acts as cancel.
		void SetSingleStage(std::int32_t a_scene);
		bool IsSingleStage(std::int32_t a_scene);

		// Advance edges of the current node. Sentinels: 0 / "" when invalid.
		std::int32_t EdgeCount(std::int32_t a_scene);
		std::string  EdgeId(std::int32_t a_scene, std::int32_t a_index);
		std::string  EdgeLabel(std::int32_t a_scene, std::int32_t a_index);

		// Sentinels: "" / -1 / 0 when invalid.
		std::string  GetNode(std::int32_t a_scene);
		std::int32_t GetStage(std::int32_t a_scene);  // linear scenes only
		std::int32_t GetSceneForActor(RE::Actor* a_actor);

		// Role-declaration order. Recently ended scenes stay readable (bounded FIFO) for async callbacks.
		std::vector<RE::Actor*> GetParticipants(std::int32_t a_scene);

		struct ActiveScene
		{
			std::int32_t            handle = 0;
			std::string             id;    // empty for an ad-hoc files scene
			std::string             node;
			std::vector<RE::Actor*> participants;
			Animation::PlaybackId   playbackId = 0;
		};

		// Live scenes only. Actor pointers are game-thread data.
		std::vector<ActiveScene> ListScenes();

		// Fires on node enter and scene end, on whichever thread drove it; the observer does its own thread hop.
		void SetSceneObserver(std::function<void()> a_fn);

		// Ledger-tracked, so every end path restores. a_slotMask = ARMO biped slot bits.
		bool HideEquipment(std::int32_t a_handle, RE::Actor* a_actor, std::uint32_t a_slotMask);
		bool RestoreHiddenEquipment(std::int32_t a_handle);

		// Load teardown, via GraphManager::StopAll's clear handler.
		void Clear();

	private:
		// 0 = table full or an actor is already in a scene.
		std::int32_t Start(std::string_view a_id, std::string_view a_entryNode, const std::vector<RE::Actor*>& a_participants,
			const AnchorOverride& a_anchor = {}, const StartOverrides& a_over = {});

		bool SetNode(std::int32_t a_scene, std::string_view a_node);

		// Undo ledger entries; replayed in reverse, once, on any termination.
		enum class Mechanism : std::uint8_t
		{
			kPlayerInputLock, // at most one scene holds the player
			kFade,
			kEquipment,
			kHeldEquipment, // held items cleared at start; re-equipped only on scene cleanup
			kEquipItem,     // undo = unequip + remove-if-added
			kCamera,        // third-person camera lock
			kCameraState,   // free-fly / orbit state override
			kWeapon,
			kInputChannel,
			kProps
		};

		// Distinct from PlaybackEndReason, which is the Layer-A condition behind an auto-end.
		enum class TerminationCause : std::uint8_t
		{
			kCompleted,
			kCancelled,
			kInterrupted
		};

		static constexpr std::string_view TerminationCauseName(TerminationCause a_cause) noexcept
		{
			switch (a_cause) {
			case TerminationCause::kCompleted: return "completed";
			case TerminationCause::kCancelled: return "cancelled";
			case TerminationCause::kInterrupted: return "interrupted";
			}
			return "interrupted";
		}

		struct ActiveProp
		{
			std::string     id;
			Props::Instance instance;
		};

		struct Slot
		{
			std::uint16_t           generation = 0;  // 0 = empty
			// Ended but not reclaimed: roster stays readable for the async SCENE_END dispatch while
			// actors count as free. FIFO-bounded.
			bool                    ended = false;
			// One transition at a time: ApplyTransition runs outside _lock, and overlapping triggers
			// (Papyrus advance vs auto-end) orphaned a graph and froze the player. Claimed under _lock.
			bool                    transitioning = false;
			TerminationCause        terminationCause = TerminationCause::kCompleted;
			std::string             id;
			std::string             node;
			std::vector<RE::Actor*> participants;
			// Keeps retained ended rosters from dangling if a ref unloads.
			std::vector<RE::NiPointer<RE::Actor>> retiredParticipantRefs;
			Animation::PlaybackId   playbackId = 0;
			Registry::SceneRef      definition;
			AnchorOverride          anchor;  // unset = anchor at participant[0]
			float                   loopScale = 1.0f;
			std::optional<Animation::WorldPlacementMode> worldPlacement;
			bool                    singleStage = false;
			bool                    cameraOverridden = false;  // authored node cameras stand down
			std::vector<Mechanism>  ledger;  // at most one entry per Mechanism
			// Per-mechanism state lives here so the ledger stays a plain ordered list.
			std::vector<std::pair<RE::Actor*, Equipment::Snapshot>> hiddenEquip;
			// Separate so osf.equipment.restore stays apparel-only.
			std::vector<std::pair<RE::Actor*, Equipment::Snapshot>> hiddenHeldEquip;
			std::vector<std::pair<RE::Actor*, Equipment::EquippedItem>> equippedItems;
			std::vector<RE::Actor*> sheathedWeapon;
			// Destroyed before SCENE_END dispatch.
			std::vector<ActiveProp> props;
			Input::Grant            grant;
		};

		// Copy of slot fields for reads outside _lock.
		struct SlotView
		{
			std::string             id;
			std::string             node;
			std::vector<RE::Actor*> participants;
			Animation::PlaybackId   playbackId = 0;
			Registry::SceneRef      definition;
			bool                    cameraOverridden = false;
		};

		struct AdvanceEdgeInfo
		{
			std::string id;
			std::string label;  // labelKey if set, else the literal label
		};

		// token = (generation << 16) | slot; generation >= 1 keeps tokens nonzero.
		static std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		// Caller holds _lock. Ended slots resolve only with a_includeEnded (roster reads).
		Slot* Resolve(std::int32_t a_handle, bool a_includeEnded = false);

		// Exclusivity-checked; doesn't play or fire. 0 = table full or an actor is busy.
		std::int32_t MintSlot(std::string_view a_id, std::string_view a_node, const std::vector<RE::Actor*>& a_participants);

		// Marks the slot ended (no events), freeing its actors; also rollback for a failed start. Takes _lock.
		void ReleaseSlot(std::int32_t a_handle);

		// Caller holds _lock. Single source of the one-actor-one-scene invariant.
		Slot* FindSlotForActor(RE::Actor* a_actor, std::int32_t* a_token);
		// Caller holds _lock. Exact playback match, so a deferred callback can't bind to a replacement scene.
		Slot* FindSlotForPlayback(Animation::PlaybackId a_playbackId, std::int32_t* a_token);

		// Takes _lock. False if the handle is stale.
		bool SnapshotSlot(std::int32_t a_handle, SlotView& a_out);

		// The functions below that dispatch or enter the VM must be called outside _lock.
		static void Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_trackPosition);

		static void DispatchCue(std::int32_t a_handle, std::string_view a_node, std::string_view a_cue,
			std::string_view a_trackPosition, float a_time);

		// Enter/exit entries only; timed ones go through OnTimedMarks.
		static void DispatchLifecycleCues(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		static void DispatchLifecycleSounds(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// The role selects voice channel, gender substitution, subtitle speaker, and emitter.
		static void PlaySound(std::int32_t a_handle, std::string_view a_spec, std::string_view a_role,
			Registry::SoundEmitter a_emitter);

		static void DispatchLifecycleCamera(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// a_distance (> 0) seeds the zoom pull-back for thirdperson_hold only.
		static void RunCamera(std::int32_t a_handle, Registry::CameraState a_state, bool a_hasPlayer, float a_distance = 0.0f);

		static void DispatchLifecycleActions(std::int32_t a_handle, std::string_view a_node, bool a_enter);

		// Built-in osf.* actions run directly; others emit EVENT_ACTION. May take _lock.
		static void RunAction(std::int32_t a_handle, std::string_view a_node, const Registry::ActionEntry& a_action,
			std::string_view a_trackPosition, bool a_hasPlayer);

		static void DispatchAction(std::int32_t a_handle, std::string_view a_node, std::string_view a_type,
			std::string_view a_role, std::string_view a_trackPosition);

		// All idempotent. ReplayLedger runs from the single SCENE_END point, covering every end path.
		void RecordMechanism(std::int32_t a_handle, Mechanism a_mech);
		void UndoMechanism(std::int32_t a_handle, Mechanism a_mech);
		void ReplayLedger(std::int32_t a_handle);

		void AttachSceneProp(
			std::int32_t a_handle, const Registry::ActionEntry& a_action);
		void DestroySceneProp(std::int32_t a_handle, std::string_view a_prop);

		// Empty role = first participant. nullptr if the handle is dead or the role is unknown.
		RE::Actor* ResolveRoleActor(std::int32_t a_handle, std::string_view a_role);

		// Record* helpers take _lock themselves; call them outside it.
		void RecordHiddenEquip(std::int32_t a_handle, RE::Actor* a_actor, Equipment::Snapshot a_snapshot);

		// Cleanup-only, so authored apparel restore can't re-equip held props.
		void RecordHiddenHeldEquip(std::int32_t a_handle, RE::Actor* a_actor, Equipment::Snapshot a_snapshot);
		void RecordEquippedItem(std::int32_t a_handle, RE::Actor* a_actor, Equipment::EquippedItem a_item);

		void RecordSheathedWeapon(std::int32_t a_handle, RE::Actor* a_actor);

		bool RecordCameraState(std::int32_t a_handle, Camera::CameraMode a_mode);

		void RecordInputChannel(std::int32_t a_handle, const Input::Grant& a_grant);

		// Call outside _lock. 0 = no playable or play failed; ApplyTransition then ends the scene
		// cleanly rather than stranding it with the input lock held.
		static Animation::PlaybackId PlayNodeAnim(const std::vector<RE::Actor*>& a_participants,
			std::string_view a_sceneId, std::string_view a_nodeId,
			Animation::PlaybackId a_expectedPlayback = 0);
		static void StopGraph(const std::vector<RE::Actor*>& a_participants,
			Animation::PlaybackId a_expectedPlayback);

		void EngageDefaultPlayerInputLock(std::int32_t a_handle, bool a_playerInputLock,
			const std::vector<RE::Actor*>& a_participants);

		// Player participates and the scene specifies no camera: scene orbit.
		void EngageDefaultCamera(std::int32_t a_handle, std::string_view a_defId, std::string_view a_entryNode, bool a_playerInputLock, std::string_view a_cameraOverride, const std::vector<RE::Actor*>& a_participants);

		// Empties every participant's hands (slate, tool, weapon). Worn gear picks are exempt.
		void ClearHeldItems(std::int32_t a_handle, bool a_clearHeldItems,
			const std::vector<RE::Actor*>& a_participants,
			const std::vector<std::vector<Equipment::Gear::Pick>>& a_gearPicks);

		// Preserves base skin; worn gear picks are exempt.
		void HideDefaultApparel(std::int32_t a_handle, bool a_hideApparel, const std::vector<RE::Actor*>& a_participants,
			const std::vector<std::vector<Equipment::Gear::Pick>>& a_gearPicks);
		void StripDefaultActors(std::int32_t a_handle, bool a_stripActors, const std::vector<RE::Actor*>& a_participants,
			const std::vector<std::vector<Equipment::Gear::Pick>>& a_gearPicks)
		{
			HideDefaultApparel(a_handle, a_stripActors, a_participants, a_gearPicks);
		}

		// Gear registry ∩ inventory, minus slots claimed by authored role `equip`. Index-parallel to
		// a_participants. Game thread.
		std::vector<std::vector<Equipment::Gear::Pick>> BuildGearPicks(std::string_view a_defId, const std::vector<RE::Actor*>& a_participants);

		// Already-worn picks were strip-exempted instead; nothing to equip or undo.
		void EquipGearItems(std::int32_t a_handle, const std::vector<RE::Actor*>& a_participants,
			const std::vector<std::vector<Equipment::Gear::Pick>>& a_gearPicks);

		// a_participants[i] <-> def->roles[i]; gender picks the male/female ref, falling back to `any`.
		void EquipRoleItems(std::int32_t a_handle, std::string_view a_defId, const std::vector<RE::Actor*>& a_participants);

		void EngageDefaultSceneControls(std::int32_t a_handle, std::string_view a_defId, std::optional<bool> a_controlOverride, const std::vector<RE::Actor*>& a_participants);
		void EngageDefaultPlayerControl(std::int32_t a_handle, std::string_view a_defId, std::optional<bool> a_controlOverride, const std::vector<RE::Actor*>& a_participants)
		{
			EngageDefaultSceneControls(a_handle, a_defId, a_controlOverride, a_participants);
		}

		// Self-releasing fade, not ledger-recorded. Async (UI queue), so it can't hide the frame-0 snap.
		void FadeSceneStart(std::int32_t a_handle, bool a_fade, const std::vector<RE::Actor*>& a_participants);

		// Post-play commit shared by registry and ad-hoc starts; playback and slot must already be live.
		void CompleteStart(std::int32_t a_handle, std::string_view a_defId, std::string_view a_entryNode,
			const std::vector<RE::Actor*>& a_participants, bool a_playerInputLock, bool a_hideApparel,
			bool a_clearHeldItems, bool a_fade, const StartOverrides& a_over);

		// NODE_EXIT, then end or play a_newNode + NODE_ENTER. Call outside _lock with a snapshot.
		void ApplyTransition(std::int32_t a_handle, std::string_view a_oldNode, std::string_view a_newNode,
			bool a_end, std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants);

		// Releases Slot::transitioning after a non-terminal transition. Takes _lock.
		void EndTransition(std::int32_t a_handle);

		bool TakeEdge(std::int32_t a_scene, const std::function<const Registry::SceneEdge*(const Registry::SceneNode&)>& a_selectEdge);

		std::vector<AdvanceEdgeInfo> AdvanceEdges(std::int32_t a_scene);

		// Invoked outside _lock; the observer may re-enter via ListScenes.
		void NotifySceneObserver();

		std::mutex        _lock;
		std::vector<Slot> _slots;
		std::deque<std::int32_t> _retiredHandles;
		std::uint16_t     _nextGen = 1;
		std::function<void()> _sceneObserver;
		Animation::PlaybackSinkId _playbackSinkId = 0;
	};
}
