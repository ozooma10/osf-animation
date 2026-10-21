# Shared scene preparation (H3)

Engine services **3.10** adds a versioned, vetoable preparation boundary before scene-owned
dependent playback. It uses the [H2 actor-state service](ACTOR_STATE.md). Animation remains
content-neutral: consumers decide which devices, resources and attachments a destination needs.

## Admission and observable completion

Every registered-scene start (native, Papyrus, matched or Browser), ad-hoc files/staged start,
same-cast handoff, explicit/edge/automatic node transition, and runtime-owned internal playback
stage change enters the same coordinator. Standalone low-level animation playback without a
scene owner has no scene preparation contract.

A nonzero start handle now means **reserved and queued**, not already playing. A true transition
result means **accepted for preparation**. Actors are exclusive from reservation through
termination. Definitions and the exact destination cast are retained for the attempt. Source
motion freezes on the verified frame drain while readiness is pending; speed and user pause
remain independent. Sampling hooks only queue stage requests and hold the current stage.

Successful `SCENE_BEGIN`, `SCENE_HANDOFF` and destination `NODE_ENTER` follow preparation
and playback admission. An internal playback segment does not invent a node-enter event.
A rejected queued start emits `SCENE_END` with interrupted/cancelled cause, without a preceding
begin/enter, so callers holding its reserved handle receive a terminal acknowledgement.
World replacement emits no old-world scene events.

Subscribe to lifecycle events before issuing a start. Native callers can also use
`ReadScenePreparation` through `Post`. Its copied JSON text contains
`{scene,status,reason,pending,playing,attempt?}`. Status is `pending`, `committed`,
`vetoed`, `failed`, `cancelled`, `timed-out` or `owner-withdrawn`.
Before the first drain, a pending attempt ID can be zero. Playing reports a published runtime
playback identity; it is not renderer evidence. Terminal results are retained with the bounded
ended-roster cache, and disappear on world replacement or handle retirement.

## Native registration and callback lifetime

Copy `OSFEngineAPI.h`, `OSFSceneAPI.h`, `OSFActorState.h` and `OSFPreparation.h`.
Request services 3.10 and check the negotiated table size. Register/unregister/readback calls
require the engine's verified `Post` drain. SFSE's general task queue alone does not qualify.

`ScenePreparer` version 1 has size/version, signed order, timeoutMs, prepare/finish callbacks
and caller context. Registration copies this structure and returns a nonzero token. Callbacks
and context storage remain owned by the caller until unregistration and any currently executing
prepare/finish callback have returned. Unregistering inside a callback does not make it safe to
unload its DLL or destroy its context before the callback and finalization unwind.

Providers are snapshotted when the queued attempt begins, ordered by ascending order, with
registration order breaking ties. A new registration participates in later attempts only.
Registrations survive world replacement; attempt IDs never recycle. Unregistration cancels
pending attempts that include that provider and conservatively stops live sessions that committed
with it, including a provider that contributed readiness without equipment.

`PreparationContext` is a borrowed view of an owned, copied transition description: attempt,
provider, scene handle, kind (`Start/Handoff/Node/Stage`), source/destination IDs and node IDs,
stage indices, and actor form IDs in exact destination-role order. Graph nodes without a linear
stage index use -1; internal segments use zero-based segment indices. Copy fields needed after
the callback. These IDs are world-local, not persistent references or authoritative matching
snapshots.

## Phases

| Phase | Contract |
|---|---|
| Plan | Read restrictions, reserve exact resource keys, and collect equipment requirements. Do not mutate actors or attachments. Approve, Pending or Veto. |
| Apply | All plans passed preflight and built-in equipment was applied. Apply the provider's required attachment/backend operations once. Approve or Veto; Pending is invalid here. |
| Ready | Confirm the required backend work is ready. Approve, Pending or Veto. Pending polls on a later frame. |
| Validate | Recheck permissions, resource/attachment identity and readiness immediately before playback admission. Approve or Veto; no new asynchronous work. |

After all Plan callbacks approve, the host previews the combined ownership plan without backend
writes. It then emits the old node's exit observations (where applicable), applies the H2 edit,
runs Apply/Ready/Validate, reconciles inventory and actor 3D once more, acquires commit guards,
checks the cast's H2 revisions, and commits playback.
Callbacks run outside runtime/manager locks; scene identity and cancellation are checked after
each callback. Work queued by a callback waits for a later finite frame batch.

A whole attempt has a monotonic deadline: the minimum provider timeout, from 1 through 5000 ms;
5000 ms without providers. Paused game updates do not run callbacks. Expiry is processed on the
next drain. Native callbacks must return promptly; the host cannot preempt a blocked callback.
Plan/Ready approval is retained while later providers wait, so Validate must recheck anything
that could have changed in the meantime.

Each entered provider receives one `finish`, in reverse order, before host finalization.
Outcomes are Committed, Vetoed, Failed, Cancelled, TimedOut, OwnerWithdrawn and WorldReset.
On failure, undo only this attempt's provider-owned work. On WorldReset discard old-world
references without engine mutation. A reset during an earlier finalizer changes later finalizers
to WorldReset. A successful finalizer that cancels/stops the scene prevents later begin/enter
publication. Committed finalization is an attempt notification; providers must separately observe
scene termination to retire their own attachment/backend state.

## Equipment and resource ownership

During its own Plan callback a provider may call:

- `RequirePreparedEquipment(attempt, provider, request)`: exact actor must belong to the cast.
  Uses H2 Equip/Preserve/Hide/HideOptional; only AllowAdd is accepted as a flag and request.group
  is ignored. Exact repeated requests are deduplicated.
- `ReservePreparationResource(attempt, provider, key)`: a caller-defined, case-sensitive,
  nonempty identity of at most 256 bytes. Use namespaced exact-instance/use identities agreed
  with the resource owner. The host cannot infer aliases between two different keys.
- `SetPreparationFailure(attempt, provider, reason)`: copies up to 1024 bytes from any prepare
  phase. Return Veto separately; setting text alone does not refuse admission.

Plan polling accumulates requirements; it cannot retract an earlier requirement in that attempt.
Every destination replaces that provider's prior session requirements. Re-submit everything that
must remain required, even on an internal stage change. Separate persistent H2 scopes remain
independent. Resource reservations are exclusive across scene/provider owners; committed leases
remain until the next successful destination plan drops them or the session ends. Failure drops
only tentative leases and retains the source's committed leases.

Built-in defaults, gear picks, role equipment and entry equipment actions join the same plan.
Authored exit equipment actions join the transaction too, rather than escaping rollback through
an exit callback. Optional hides yield to positive ownership. Required consumer claims precede
optional gear; conflicts with another owner refuse admission. Authored equipment can supersede
its own conflicting authored/gear lane, never a persistent owner's device. Existing standalone
pack defaults remain enabled. There is no converted-content suppression policy or implicit
device-removal permission in this implementation.

Only the current destination's requirements are prepared, not a union of reachable nodes.
Timed equipment actions retain ordinary H2 behavior; they cannot retroactively satisfy a required
entry prerequisite. Decorative `osf.prop.attach` remains best effort after entry. Mandatory
attachments must be implemented by a preparer that reserves their use and verifies readiness.

## Failure, save and reset

A manual preflight refusal restores no target state because none was applied, and resumes only
an exact, still-live source playback with no pending automatic boundary. Automatic refusal,
source disappearance, cancellation, or any failure after application/exit begins interrupts the
scene. Partial failure rolls back the selected H2 scopes against current live inventory, preserving
unrelated owners, then terminates conservatively: arbitrary provider effects cannot be proven
restored by the host. Playback loading/admission can still fail after preflight; this takes that
same partial-failure path.

Stop can cancel pending preparation. The End input remains usable on an existing scene when its
grant permits it. Provider withdrawal also ends sessions that depend on that provider. Save begin
cancels pending admission and suspends H2 session demands; coordinator pumping waits until save
end, then completes cancellation/cleanup. World replacement revokes the old H2 edits without
restoration and finalizes old preparation as WorldReset on the frame drain.

## Readiness and validation limits

The built-in backend verifies inventory reconciliation and the presence of each actor's 3D root.
**It does not prove that a particular equipped mesh has finished loading or that a required
attachment exists.** A consumer relying on those conditions must check its authoritative backend
in Ready and Validate, returning Pending or Veto until supported evidence exists. No automatic
item-mesh readiness backend, restraint/toy implementation, versioned restriction snapshot or
persistent pose layer is supplied by H3's initial implementation.

Native tests cover coordinator ordering, all four kinds, pending/readiness, veto/timeout,
callback reentry, owner withdrawal, world reset, failed playback commit, transaction rollback with
an unrelated persistent owner, and stage gating/pose/pause behavior. The production runtime and
both DLLs compile; the Browser boundary check verifies public-header separation. These are not
in-game acceptance. Fresh start/handoff/node/internal-stage scenarios, actual item/attachment
readiness, visible poses, save/load and consumer integration remain live gates. A7 stays open.

## Final commit guard (services 3.13)

Animation captures each participant's H2 world/ownership/equipment revision after Ready's
reconciliation, before Validate. After every provider has validated, it reconciles again,
acquires the optional consumer guards, then compares those revisions immediately before
calling playback. Any observed change, including an ownership change reversed before commit,
refuses with `actor-state-changed-after-validate`. Changes to unrelated actors do not veto.
H3's own application precedes this baseline, so ordinary preparation remains valid.

From its Validate callback, a provider may call `GuardPreparationCommit(attempt, provider,
&guard)` once. The host copies the descriptor; callback/context storage must survive through
release/finish. Acquire runs once, in Validate order, after final inventory reconciliation.
Return false to refuse; optionally call `SetPreparationFailure` for a copied diagnostic.
Acquire must be short and must not mutate gameplay. Consumers can atomically check their
validated revision and lease metadata publication through playback. Queue incoming metadata
until release; never hold a consumer mutex across engine calls or wait for a writer thread.

Every entered acquire gets one release, even after false, exceptions, cancellation, world reset,
provider withdrawal, or playback failure. Unentered guards receive no release. Releases run
in reverse order before provider finish, H2 rollback and scene event publication. Failed acquire
must make its release harmless. Nested registration, duplicate guards and registration outside
the provider's Validate callback are refused. All four transition kinds share this path.

This protects managed state and observed equipment. It is not an inventory hook or a global
freeze of third-party engine writes. Unobserved external mutations still require observation
and active-playback incompatibility handling. The coordinator tests cover late provider,
reconciliation and guard changes plus failure cleanup; live qualification is recorded by the
paired Intimacy guard fixture.
