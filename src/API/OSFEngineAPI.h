// Optional engine services. Copy with OSFSceneAPI.h, OSFActorState.h and OSFPreparation.h; link no Animation library.
#pragma once

#include "OSFActorState.h"
#include "OSFPreparation.h"
#include "OSFSceneAPI.h"

namespace OSF::API
{
	// Services 3.17 appends an optional compact clip-source provider registration.
	inline constexpr std::uint32_t kOSFEngineAPIVersion = 0x00030011;

	struct AudioChannelOptions {
		std::uint32_t size{sizeof(AudioChannelOptions)};
		std::uint32_t latestPending{}; // 0/1; replace one waiting job for this channel before capacity checks
		std::uint32_t maxAgeMs{}; // 0 disables expiry; otherwise measured from detection to submission
	};

	inline constexpr bool EngineRequestSupported(std::uint32_t requested, std::uint32_t provided) noexcept
	{
		// Two private 3.13 branches used the same slot for incompatible functions.
		// Reject that ambiguous request; consumers must rebuild against 3.14.
		return requested != 0x0003000D && (requested >> 16) == (provided >> 16) &&
			(requested & 0xFFFF) <= (provided & 0xFFFF);
	}
	inline constexpr bool EngineTableSupported(std::uint32_t requested, std::uint32_t provided,
		std::uint32_t size, std::size_t requiredSize) noexcept
	{
		return EngineRequestSupported(requested, provided) && size >= requiredSize;
	}

	enum class ClipProbeStatus : std::uint32_t { Missing, Unchanged, Available, Unprobeable };
	struct ClipProbeResult
	{
		std::uint32_t size{sizeof(ClipProbeResult)};
		ClipProbeStatus status{ClipProbeStatus::Missing};
		float seconds{-1};
		std::uint64_t bytes{};
		std::int64_t modified{};
	};
	// Exclusive external input channel. Zero restores normal scene input routing.
	enum InputRouting : std::uint32_t {
		SuppressRawMouse = 1, ConsumeThumbsticks = 2, ConsumeGamepadAfterDispatch = 4,
		AllowNativeFreeCamera = 8
	};
	enum class EngineEvent : std::uint32_t { WorldReset = 1, CatalogChanged = 2, ScenesChanged = 4, ContentReloaded = 8 };
	using EngineCallback = void (*)(EngineEvent, void*);
	using TextCallback = void (*)(const char*, void*);
	// Receives immutable base content (schema 3 or compact schema 1, per registration), excluding provider scenes.
	// Emit exactly one JSON array of OSFClipScene.h records, synchronously. No registry/API reentry.
	using ClipSceneProvider = void (*)(const char*, TextCallback emit, void* emitContext, void* context);
	// Invoked exactly once for an accepted post at a later verified frame drain. execute=false means
	// world replacement canceled the command: release context only, without touching game objects.
	using FrameCallback = void (*)(void* context, bool execute);

	// Singleton table owned by Animation. All callbacks must catch exceptions and remain loaded
	// until unregistered / all accepted posts have completed (SFSE DLLs normally live to process exit).
	// No STL objects or allocator ownership cross this boundary. Text is borrowed until callback return.
	struct OSFEngineAPI
	{
		std::uint32_t size;
		std::uint32_t version;
		// Any thread. Always deferred; FIFO with engine commands. false retains ownership in caller.
		bool (*Post)(FrameCallback, void*);
		// Any thread. WorldReset runs synchronously at teardown; invalidate local generations only.
		// Other events may also originate off-thread. Dispatch game work with Post.
		// Unsubscribe waits for callbacks on other threads; self-unsubscribe is allowed.
		std::uint64_t (*Subscribe)(EngineCallback, void*);
		bool (*Unsubscribe)(std::uint64_t);
		// Unless marked any-thread, the remaining functions require the game thread (normally via Post).
		// Any thread as of 3.16. Pins an immutable registry snapshot; reads only metadata and
		// mutex-protected decoded durations. JSON schema 3, defined by OSFCatalog.h.
		// Catalog includes diagnostics/load errors. Callback runs once, inline, on success.
		bool (*ReadCatalog)(TextCallback, void*);
		// Any thread. Memory-only decoded duration snapshot (OSFCatalog.h); caller copies inline.
		bool (*ReadClipDurations)(TextCallback, void*);
		// Any thread; synchronous, potentially slow IO. Call from a consumer-owned worker.
		// Optional previous source identity avoids decoding an unchanged file. No cache writes or workers.
		bool (*ProbeClipDuration)(const char*, const char*, const ClipProbeResult*, ClipProbeResult*);
		std::int32_t (*ReloadContent)();
		// Game thread. JSON array of accepting scene IDs; cast is positional role order.
		bool (*MatchCast)(RE::Actor* const*, std::uint32_t, TextCallback, void*);
		bool (*MatchAnchor)(RE::TESObjectREFR*, TextCallback, void*);
		// Game thread. JSON array: handle, id, stage, time, duration, speed, participant form IDs.
		bool (*ReadActiveScenes)(TextCallback, void*);
		bool (*SetSingleStage)(std::int32_t);
		bool (*SetSceneSpeed)(std::int32_t, float); // scene-owned across nodes; 0 pauses, clamps to 100
		float (*GetSceneSpeed)(std::int32_t); // -1 for an invalid/ended handle
		// Game thread, borrowed UTF-8 species/gender copied through callback.
		bool (*ReadActorIdentity)(RE::Actor*, TextCallback, void*);
		// Game thread. World transform and optional navmesh position, each three floats in meters.
		bool (*ReadWorldAnchor)(RE::TESObjectREFR*, float* position3, float* heading);
		bool (*SnapWorldPosition)(RE::TESObjectREFR*, const float* position3, float* snapped3);
		// Shared camera/input mechanisms. The consumer owns each successful orbit acquisition and
		// must release it once; WorldReset revokes all holds. No browser lifetime is kept in the engine.
		bool (*AcquireOrbit)(const std::uint32_t*, std::uint32_t);
		void (*ReleaseOrbit)();
		bool (*ReadOrbitPose)(float* position3, float* forward3);
		void (*KickToThirdPerson)();
		void (*SetInputRouting)(std::uint32_t);
		// Raw mouse-equivalent units and wheel notches. Consumers convert their own pointer units.
		void (*InjectOrbitDelta)(float, float, float);
		// Game thread. A unique owner is rebuilt immediately and on each content reload. Providers
		// also run on the reload caller's thread; remain loaded until unregistered. Existing scene refs
		// pin their publication across replacement/removal. A malformed batch is rejected as a whole.
		bool (*RegisterClipSceneProvider)(const char* owner, ClipSceneProvider, void*);
		bool (*UnregisterClipSceneProvider)(const char* owner);
		// 3.2. Exact role name; delta4 = anchor-local x/y/z and heading in degrees.
		bool (*SetSceneAlignment)(std::int32_t, const char* role, const float* delta4);
		bool (*GetSceneAlignment)(std::int32_t, const char* role, float* delta4);
		bool (*SaveSceneAlignment)(std::int32_t);
		bool (*ResetSceneAlignment)(std::int32_t, const char* role);
		// 3.3. Keep handle/cast/ledger/speed. Only startStage and anchorRef are read from options.
		// Other options are ignored. Null options use the target's entry and retained anchor.
		bool (*ReplaceScene)(std::int32_t, const char* sceneId, const OSFStartOptions*);
		// roleNames[i] names the target role for participant i in the current scene's roster.
		bool (*ReplaceSceneRoles)(std::int32_t, const char* sceneId, const char* const* roleNames, std::uint32_t count);
		// 3.4. Game thread. Matchmade handoff for the same cast, excluding the current scene and keeping its world
		// placement mode. query->roles[i] constrains current participant i (GetSceneParticipants order); a non-null
		// query->anchor replaces the retained anchor reference. False leaves the current scene playing.
		bool (*ReplaceSceneByQuery)(std::int32_t, const OSFSceneQuery* query);
		// 3.5. Game thread. Retained across nodes/handoffs, defaults true. False suppresses timer/loop
		// completion only on nodes with allowManualHold; sampling, marks and explicit navigation continue.
		// Re-enabling starts a fresh completion budget without restarting clips. A held playback that has already completed refuses.
		bool (*SetSceneAutoAdvance)(std::int32_t, bool);
		// Live JSON snapshot: sceneId, node, automatic, allowManualHold, speed, edges [{id,label}].
		bool (*ReadSceneControls)(std::int32_t, TextCallback, void*);
		// 3.6. Any thread. Opaque, process-local channel; 0 on exhaustion (4096 live maximum).
		// Caller owns the handle and closes it at its chosen scope boundary. No scene/actor
		// policy is inferred. World replacement revokes every channel; IDs are never recycled.
		std::uint64_t (*CreateAudioChannel)();
		// Literal Data-relative audio file or event: spec; copied before return. No pool queries.
		// Files queue without IO on the caller. true means accepted, not audible/decoded.
		// Event specs submit inline. Listener-relative until actor emitters are supported.
		// Latest published request replaces this channel's voice; older completions cannot post.
		bool (*PlayAudioChannel)(std::uint64_t, const char* spec);
		// Permanently retires the handle, cancels queued work and stops its submitted voice.
		// Serialized with final Wwise submission: after return no old job can post. A job that
		// committed first is stopped before return. Other channels and legacy slots survive.
		// false for invalid/already-closed handles; repeated close is harmless. No disk IO.
		bool (*CloseAudioChannel)(std::uint64_t);
		// 3.7. Game context. Route this actor's authored sound-lane/action playback into
		// an owned channel. Bind before authored entry sounds. Refuses another live binding.
		// Closing the channel removes its binding; world reset clears all bindings.
		// Actorless sounds and other actors are unaffected. Does not transfer ownership.
		bool (*BindActorSoundChannel)(RE::Actor*, std::uint64_t);
		// 3.8. Any thread. Options are immutable for the channel lifetime. Null/invalid options fail.
		// Latest-pending also suppresses executing older jobs before preparation/submission.
		// Once a newer file is admitted, superseded work is never revived if its preparation fails.
		// Invalid/rejected admissions do not supersede; failed replacements never stop a playing voice.
		// CreateAudioChannel retains the original non-coalescing, non-expiring behavior.
		std::uint64_t (*CreateAudioChannelWithOptions)(const AudioChannelOptions*);
		// Process-local monotonic nanoseconds; not wall time or animation time. Do not persist.
		std::uint64_t (*AudioClockNs)();
		// detectedNs must be nonzero, from AudioClockNs or scene 1.6 OSFSceneEvent.detectedNs,
		// and not in the future. Expiry includes queue/provider delay. Checks before preparation
		// and immediately before Wwise submission; no completion/audibility deadline is implied.
		// PlayAudioChannel starts age at call entry. Bindings inherit their channel's options.
		bool (*PlayAudioChannelAt)(std::uint64_t, const char* spec, std::uint64_t detectedNs);
		// 3.9. All actor-state calls require game context (use Post from other threads).
		// Do not reenter during backend inventory events; defer those calls with Post.
		// Persistent requires scene=0. Session requires a live scene and accepts only its cast.
		// Session scopes survive handoff and close before scene-end observers; persistent scopes
		// survive scene end. Owner is copied. Close every owned scope before unloading a provider.
		// Runtime-only tokens are never recycled or saved; world reset revokes them without undoing
		// old-world inventory. Re-register device state after load, not in the reset notification.
		std::uint64_t (*CreateActorStateScope)(const char* owner, ActorStateLifetime, std::int32_t scene);
		// Exact duplicate requests share a claim. Equip/Preserve share positive ownership; Preserve
		// never adds an item. Hide is required and conflicts with positive claims; HideOptional
		// yields to them. Conflicting worn slots refuse, except explicit same-scope/group replacement.
		// Success verifies inventory state, not 3D readiness. Item identity is a base form, not an
		// inventory instance. See ACTOR_STATE.md for provenance and external-change limitations.
		ActorStateStatus (*AcquireActorEquipment)(std::uint64_t scope, const ActorEquipmentRequest*, std::uint64_t* claim);
		ActorStateStatus (*ReleaseActorStateClaim)(std::uint64_t scope, std::uint64_t claim);
		ActorStateStatus (*ReleaseActorStateGroup)(std::uint64_t scope, std::uint32_t group);
		// Release retires claims even if cleanup fails. Failed cleanup records stay in the service;
		// ReconcileActorState retries when the actor/inventory is available. Invalid handles are harmless.
		ActorStateStatus (*CloseActorStateScope)(std::uint64_t scope);
		ActorStateStatus (*ReconcileActorState)(RE::Actor*);
		// 3.10. Verified-frame context only: call through Post. Structures copied, callback/context
		// storage remains caller-owned until unregister and any current callback/finish have returned.
		// Registrations survive world reset. Attempts are ephemeral; all callbacks run in the frame drain.
		std::uint64_t (*RegisterScenePreparer)(const ScenePreparer*);
		bool (*UnregisterScenePreparer)(std::uint64_t provider);
		// Only from this provider's Plan callback. group is ignored; AllowAdd is the only accepted flag.
		// Collects requirements without mutations. Each destination replaces this provider's prior
		// requirements in this session; persistent actor-state scopes remain independent.
		bool (*RequirePreparedEquipment)(std::uint64_t attempt, std::uint64_t provider, const ActorEquipmentRequest*);
		// Exact, caller-defined resource identity (1..256 bytes); exclusive across scene/provider owners.
		// Reservations survive commit until the next destination plan releases them, or scene termination.
		bool (*ReservePreparationResource)(std::uint64_t attempt, std::uint64_t provider, const char* key);
		// Optional copied diagnostic (up to 1024 bytes) from any prepare phase; return Veto to refuse.
		bool (*SetPreparationFailure)(std::uint64_t attempt, std::uint64_t provider, const char* reason);
		// JSON {scene,status,reason,pending,playing,attempt?}; retains terminal results with ended roster.
		bool (*ReadScenePreparation)(std::int32_t scene, TextCallback, void*);
		// 3.11. Verified-frame context only. Reads the same backend as H2, without claims,
		// reconciliation or writes. Ok with count=0/worn=0 means absent, not unavailable.
		// Conflicts exclude this base form and default skins, using H2's slot policy.
		// Output is written only on Ok. No mesh, exact-instance, or snapshot-version guarantee.
		ActorStateStatus (*ReadActorEquipment)(RE::Actor*, RE::TESBoundObject*, ActorEquipmentState*);
		// 3.12. Verified-frame context only. Memory-only reads; no callbacks or inventory scans.
		// actor is a nonzero world-local form ID; an unseen actor has zero counters in this world.
		ActorStateStatus (*ReadActorStateRevision)(std::uint32_t actor, ActorStateRevision*);
		// Start at cursor=0. Each successful read returns the current cursor; unchanged reads are empty.
		// resnapshot=1 means history was lost or the cursor is ahead: discard cached snapshots.
		// Reset changes world and emits actor=0. Do not persist cursors/revisions across processes.
		ActorStateStatus (*ReadActorStateChanges)(std::uint64_t cursor, ActorStateChanges*);
        // 3.14. Verified-frame context; one guard per provider/attempt, only during Validate.
        // Callback/context lifetime extends through release and finish. See OSFPreparation.h.
        bool (*GuardPreparationCommit)(std::uint64_t attempt, std::uint64_t provider, const PreparationCommitGuard*);
		// 3.14. Verified-frame context only. Opt-in covered handoff with the same role/anchor rules.
		// false: no handoff accepted; true: asynchronous preparation, not a commit guarantee.
		// Exclusive fade ownership; refuses an existing OSF fade. Cover preparation and actual stamped
		// pose blending, independent of clip speed. Five-second deadline cancels; stop/reset unwinds.
		// ReadScenePreparation.pending includes cover/reveal; optional cover reports phase/token/startedMs.
		bool (*ReplaceSceneRolesCovered)(std::int32_t, const char* sceneId, const char* const* roles, std::uint32_t count);
        // 3.15. Verified frame. Positional cast filter; JSON [{"scene":id,"node":id}].
        // Every available node is considered independently, including unlisted nodes.
        // Join to ReadCatalog.nodes for tags/ranking; anchor and caller policy remain caller filters.
        bool (*MatchNodeCast)(RE::Actor* const*, std::uint32_t, TextCallback, void*);
        // 3.15. Anchor compatibility without the scene-wide clip-availability restriction.
        bool (*MatchNodeAnchor)(RE::TESObjectREFR*, TextCallback, void*);
        // 3.15. Verified frame. Exact node handoff; same roles/placement/admission rules as ReplaceSceneRoles.
        // Strings are copied synchronously. Unknown/empty node refuses. Covered=true uses bounded cover.
        // Authored graph navigation still uses Navigate/Advance, preserving seamless edges.
        bool (*ReplaceNodeRoles)(std::int32_t, const char* sceneId, const char* nodeId,
            const char* const* roles, std::uint32_t count, bool covered);
		// 3.17. Same ownership, threading, response and atomic rejection rules as the original
		// RegisterClipSceneProvider, but input is OSFCatalog.h ClipSourceSnapshot (schema 1).
		// It includes every base scene ID and clip address; no provider output or presentation data.
		// UnregisterClipSceneProvider removes either registration kind. Owners share one namespace.
		bool (*RegisterClipSourceProvider)(const char* owner, ClipSceneProvider, void*);
	};

	// Released 3.0 clients check exact equality. Give that request its original prefix/version.
	inline OSFEngineAPI LegacyEngineAPI(const OSFEngineAPI& current)
	{
		auto legacy = current;
		legacy.version = 0x00030000;
		legacy.size = static_cast<std::uint32_t>(offsetof(OSFEngineAPI, UnregisterClipSceneProvider) + sizeof(current.UnregisterClipSceneProvider));
		return legacy;
	}
	inline const OSFEngineAPI* RequestEngineAPI(std::uint32_t version = kOSFEngineAPIVersion) noexcept
	{
		const auto mod = REX::W32::GetModuleHandleW(kOSFModuleName);
		using Request = const OSFEngineAPI* (*)(std::uint32_t);
		const auto fn = mod ? reinterpret_cast<Request>(REX::W32::GetProcAddress(mod, "OSF_RequestEngineAPI")) : nullptr;
		const auto* api = fn ? fn(version) : nullptr;
		return api && EngineTableSupported(version, api->version, api->size, sizeof(OSFEngineAPI)) ? api : nullptr;
	}
}
