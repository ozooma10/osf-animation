#pragma once

#include "ActorState/Service.h"
#include "Equipment/GearRegistry.h"
#include "Input/InputTypes.h"
#include "Matchmaking/Matchmaker.h"
#include "Preparation/Coordinator.h"
#include "Props/PropService.h"
#include "Registry/ContentRegistry.h"
#include "Scene/AlignmentState.h"
#include "Scene/SceneFoundation.h"
#include "Scene/TransitionCover.h"

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
		using AnchorOverride = WorldAnchor;

		struct StartOverrides
		{
			std::optional<bool> hideApparel;
			std::optional<bool> playerInputLock;
			std::optional<bool> fade;
			std::optional<bool> sceneControls;     // false revokes OSF scene commands
			std::optional<std::string> camera;  // "none" = vanilla camera; when set, node cameras are suppressed
			std::optional<Animation::WorldPlacementMode> worldPlacement;
			float               loopScale = 1.0f;  // multiplies loop-driven (loops>0) stages only, floored
			std::optional<float> speed;
			RE::TESObjectREFR* anchorRef = nullptr;  // borrowed until Start pins it
			WorldAnchor anchorBase;  // rendered ref transform before the definition offset
		};

		void               RegisterWithGraphManager();
		Preparation::Token RegisterPreparer(Preparation::Provider);
		bool               UnregisterPreparer(Preparation::Token);
		bool               PlanEquipment(Preparation::Token attempt, Preparation::Token provider, const ActorState::Request&);
		bool               ReserveResource(Preparation::Token attempt, Preparation::Token provider, std::string_view key);
		bool               PreparationFailure(Preparation::Token attempt, Preparation::Token provider, std::string_view reason);
		std::string        ReadPreparation(std::int32_t scene);
		bool               OnPlaybackStage(Animation::PlaybackId, std::int32_t stage, bool automatic);

		// Game thread. If no auto-edge matches, the scene completes. False if not a runtime scene.
		bool OnGraphAutoEnd(Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_participants,
			Animation::PlaybackEndReason a_reason);

		// Game thread. Same-tick lane order: action -> camera -> sound -> cue; a cue then takes the
		// first matching trigger:<id> edge.
		void OnTimedMarks(Animation::PlaybackId a_playbackId, const std::vector<RE::Actor*>& a_participants,
			const std::vector<Animation::FiredMark>& a_marks);

		bool Stop(std::int32_t a_scene);
		bool StopForActor(RE::Actor* a_actor);
		bool SetSpeed(std::int32_t a_scene, float a_speed);
		float GetSpeed(std::int32_t a_scene);
		bool TogglePause(std::int32_t a_scene);
		// Game thread. Affects only nodes authored with allowManualHold; other nodes complete normally.
		bool SetAutoAdvance(std::int32_t a_scene, bool a_enabled);
		bool GetAutoAdvance(std::int32_t a_scene);
		bool AllowsManualHold(std::int32_t a_scene);
		bool SetAlignment(std::int32_t a_scene, std::string_view a_role, const Alignment::Delta& a_delta);
		std::optional<Alignment::Delta> GetAlignment(std::int32_t a_scene, std::string_view a_role);
		bool SaveAlignment(std::int32_t a_scene);
		void AlignmentInput(std::int32_t a_scene, Input::Verb a_verb);
		bool AcceptInput(const Input::Grant& a_grant, Input::Verb a_verb);

		struct ReplaceOptions
		{
			RE::TESObjectREFR* anchor = nullptr;
			std::optional<float> headingRad;
			std::int32_t stage = 0;
			bool covered = false;
		};
		bool Replace(std::int32_t a_scene, std::string_view a_id, const std::vector<std::string>& a_roles,
			const ReplaceOptions& a_options);
		// Matchmade handoff for the same cast. a_roles[i] constrains current participant i (GetParticipants order).
		// A non-null a_anchor replaces the retained anchor reference for both the match and the handoff.
		bool ReplaceByQuery(std::int32_t a_scene, const Matchmaking::TagQuery& a_query,
			const Matchmaking::RoleConstraints& a_roles = {}, RE::TESObjectREFR* a_anchor = nullptr,
			Matchmaking::AnchorMode a_mode = Matchmaking::AnchorMode::kAllow);

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
		std::string  GetSceneId(std::int32_t a_scene);
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

		// Ledger-tracked, so every end path restores. a_slotMask = ARMO biped slot bits.
		bool HideEquipment(std::int32_t a_handle, RE::Actor* a_actor, std::uint32_t a_slotMask);
		bool RestoreHiddenEquipment(std::int32_t a_handle);

		// Game thread, SaveLoadEvent: serialize baseline equipment without consuming the scene ledger.
		void OnSaveBegin();
		void OnSaveEnd();

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

		struct PreparedTransition;
		struct Slot
		{
			std::uint16_t           generation = 0;  // 0 = empty
			// Ended but not reclaimed: roster stays readable for the async SCENE_END dispatch while
			// actors count as free. FIFO-bounded.
			bool                    ended = false;
			// One transition at a time: ApplyTransition runs outside _lock, and overlapping triggers
			// (Papyrus advance vs auto-end) orphaned a graph and froze the player. Claimed under _lock.
			bool                    transitioning = false;
			ActorState::Handle                               equipmentScope{};
			std::shared_ptr<PreparedTransition>              preparation;
			std::shared_ptr<PreparedTransition>              cover;
			std::string                                      preparationResult, preparationError;
			std::map<Preparation::Token, ActorState::Handle> preparedScopes;
			TerminationCause        terminationCause = TerminationCause::kCompleted;
			std::string             id;
			std::string             node;
			std::vector<RE::Actor*> participants;
			// Keeps retained ended rosters from dangling if a ref unloads.
			std::vector<RE::NiPointer<RE::Actor>> retiredParticipantRefs;
			Animation::PlaybackId   playbackId = 0;
			Registry::SceneRef      definition;
			AnchorOverride          anchor;  // unset = anchor at participant[0]
			AnchorOverride          frozenAnchor;
			RE::NiPointer<RE::TESObjectREFR> anchorRef;
			WorldAnchor             anchorBase;
			ActorBaseline           baseline;
			Alignment::WorkingCopy  align;
			std::size_t             alignSelected = 0;
			bool                    alignFine = false;
			float                   loopScale = 1.0f;
			SceneSpeed              speed;
			std::optional<Animation::WorldPlacementMode> worldPlacement;
			bool                    autoAdvance = true;
			bool                    singleStage = false;
			bool                    cameraOverridden = false;  // authored node cameras stand down
			std::vector<Mechanism>  ledger;  // at most one entry per Mechanism
			std::vector<RE::Actor*> sheathedWeapon;
			// Destroyed before SCENE_END dispatch.
			std::vector<ActiveProp> props;
			Input::Grant            grant;
			std::uint64_t           grantRevision = 0;
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

		struct PreparedTransition
		{
			Preparation::Context                                           context;
			SlotView                                                       old;
			Registry::SceneRef                                             target;
			std::vector<RE::Actor*>                                        cast;
			std::vector<RE::NiPointer<RE::Actor>>                          pins;
			StartOverrides                                                 overrides;
			WorldAnchor                                                    anchor, anchorBase;
			RE::NiPointer<RE::TESObjectREFR>                               anchorRef;
			std::optional<Animation::PlaybackPlan>                         plan;
			std::string                                                    failureReason;
			std::atomic<bool>                                              automatic{}, cancelled{}, reset{}, interrupted{};
			bool                                                           defaults{}, applied{}, exitFired{}, committed{};
			bool                                                           playerInputLock{ true }, hideApparel{ true }, clearHeldItems{ true }, fade{};
			Animation::PlaybackId                                          published{};
			bool covered{}, coverReleased{};
			std::uint64_t coverToken{};
			TransitionCover coverState;
			ActorState::Handle                                             edit{};
			std::vector<ActorState::Store::Change>                         changes;
			std::map<Preparation::Token, std::vector<ActorState::Request>> requests;
			std::map<Preparation::Token, ActorState::Handle>               scopes;
			std::vector<ActorState::Handle>                                newScopes;
			std::map<std::string, Preparation::Token>                      resources;
		};
		bool                                                              QueuePreparation(const std::shared_ptr<PreparedTransition>&);
		void                                                              StartPreparation(const std::shared_ptr<PreparedTransition>&);
		bool                                                              PreparationValid(const std::shared_ptr<PreparedTransition>&);
		bool                                                              BuildPreparationPlan(const std::shared_ptr<PreparedTransition>&);
		bool                                                              CommitPreparation(const std::shared_ptr<PreparedTransition>&);
		void                                                              FinishPreparation(const std::shared_ptr<PreparedTransition>&, Preparation::Outcome, bool applied);
		void                                                              PollPreparation();
		void                                                              PollCover(const std::shared_ptr<PreparedTransition>&);
		void                                                              ReleaseCover(const std::shared_ptr<PreparedTransition>&);
		void                                                              ResetPreparation();
		void                                                              ReleaseResources(std::int32_t scene);
		static bool                                                       PreparedEquipmentAction(Registry::ActionKind);
		static bool                                                       LifecycleValid(std::int32_t, std::string_view, bool enter);
		Preparation::Coordinator                                          _preparers;
		std::map<Preparation::Token, std::shared_ptr<PreparedTransition>> _preparations;
		struct ResourceLease
		{
			std::int32_t       scene;
			Preparation::Token provider, attempt;
		};
		std::map<std::string, ResourceLease> _resources;
		bool                                 _preparationPollQueued{};
		std::atomic<bool>                    _preparationSaving{};

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
		static void Fire(std::int32_t a_handle, std::int32_t a_event, std::string_view a_node, std::string_view a_trackPosition,
			std::string_view a_edge = {});

		// A non-empty a_role sets the event's role and its bound actor.
		static void DispatchCue(std::int32_t a_handle, std::string_view a_node, std::string_view a_cue,
			std::string_view a_role, std::string_view a_trackPosition, float a_time, std::uint64_t a_detectedNs = 0, bool a_nativeOnly = false);

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
		void SetEquipmentSaveWindow(bool a_saving);

		void AttachSceneProp(
			std::int32_t a_handle, const Registry::ActionEntry& a_action);
		void DestroySceneProp(std::int32_t a_handle, std::string_view a_prop);

		// Empty role = first participant. nullptr if the handle is dead or the role is unknown.
		RE::Actor* ResolveRoleActor(std::int32_t a_handle, std::string_view a_role);

		ActorState::Handle EquipmentScope(std::int32_t a_handle);
		bool ApplyEquippedItem(std::int32_t a_handle, RE::Actor* actor, RE::TESBoundObject* object, bool gear = false);
		std::size_t ApplyHiddenEquipment(std::int32_t a_handle, RE::Actor* actor, std::uint32_t slots, bool held,
			std::span<RE::TESBoundObject* const> keep = {});

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
		void EngageDefaultCamera(std::int32_t a_handle, std::string_view a_entryNode, bool a_playerInputLock, std::string_view a_cameraOverride, const std::vector<RE::Actor*>& a_participants);

		void EngageDefaultSceneControls(std::int32_t a_handle, std::string_view a_defId, std::optional<bool> a_controlOverride, const std::vector<RE::Actor*>& a_participants);

		// Self-releasing fade, not ledger-recorded. Async (UI queue), so it can't hide the frame-0 snap.
		void FadeSceneStart(std::int32_t a_handle, bool a_fade, const std::vector<RE::Actor*>& a_participants);

		// Post-play commit shared by registry and ad-hoc starts; playback and slot must already be live.
		void CompleteStart(std::int32_t a_handle, std::string_view a_defId, std::string_view a_entryNode,
			const std::vector<RE::Actor*>& a_participants, bool a_playerInputLock, bool a_hideApparel,
			bool a_clearHeldItems, bool a_fade, const StartOverrides& a_over);

		// NODE_EXIT, then end or play a_newNode + NODE_ENTER. Call outside _lock with a snapshot.
		void ApplyTransition(std::int32_t a_handle, std::string_view a_oldNode, std::string_view a_newNode, bool a_end, std::string_view a_sceneId, const std::vector<RE::Actor*>& a_participants, bool a_automatic = false);

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
		Animation::PlaybackSinkId _playbackSinkId = 0;
	};
}
