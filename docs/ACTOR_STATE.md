# Shared actor-state ownership (H2)

Engine services **3.9** adds owner-held equipment claims. Scene defaults, authored role
equipment, equipment actions, automatic gear, and held-item clearing all use this service.
Gameplay mods can use it outside a scene. No anatomy, device unlocking, or encounter policy
is built into Animation.

This is H2's equipment implementation and common scope contract. Services 3.10 builds
[H3 preparation](PREPARATION.md) on it, with transactional destination requirements and resource
reservations. Services 3.12 adds the revision feed below. Pose layers, bone holds and
attachment backends remain future work. Success here confirms inventory state, not mesh readiness or playback admission.

## Read-only equipment observations (services 3.11)

`ReadActorEquipment(actor, object, &state)` uses this service's existing inventory backend.
Call it only in the verified frame drain (including preparation callbacks). It writes the
size-tagged `ActorEquipmentState` only on `Ok`, returning base-form count, worn state, 64-bit
slots, and slots overlapping other worn equipment. The queried item and default skins are
excluded from conflicts using the same overlap rule as claims. A valid absent item returns
`Ok`, count zero and worn zero; unavailable inventory/description returns `Unavailable`.
This does not acquire claims, reconcile, mutate inventory, prove mesh readiness or provide
an exact-instance/versioned snapshot. Providers can combine it with their own item geometry
checks without copying engine offsets or walking inventory independently.

## Per-actor revisions and notifications (services 3.12)

`ReadActorStateRevision(actorFormID, &revision)` returns the current world generation plus
separate ownership and equipment counters. `ReadActorStateChanges(cursor, &changes)` copies
up to 256 retained notifications into caller-owned storage. Both require the verified frame
drain, read memory only, and write the size-tagged output only on `Ok`. No consumer callback
runs inside an ownership or inventory operation. Poll from `Post` or a preparation callback.

Start with cursor zero and retain each successful result's cursor. Equal cursors return an
empty batch. A cursor older than retained history or ahead of the store returns
`resnapshot=1`, an empty batch and the current cursor: discard cached snapshots and recapture.
World reset advances `world`, clears actor counters/observations, and emits a `Reset` event
with actor zero without touching inventory. The journal sequence never restarts during the
process; old actor IDs/counters cannot validate a new world. Never persist these values.

Ownership advances for claim admission, replacement, release, scope/session cleanup,
selective rollback and suspension/resumption of session demands during saving. Empty scopes,
exact duplicates, unchanged rollback and rejected handle operations do not advance it.
Failed application can advance ownership twice as provisional claims are installed and
removed. Preview edits never publish revisions or write inventory.

Equipment advances when H2 observes a different canonical inventory or availability state,
after establishing its first baseline, and before each attempted add/remove/wear operation. Failed
operations and lost readback therefore invalidate cached evidence too. Observation covers
all returned forms, ignores ordering and zero-count entries, and includes changes observed
through `ReadActorEquipment`. Revisions are invalidation counters, not operation counts or
proof that an operation succeeded. Unchanged reconciliation and reads are quiet.

This is an H2 observation feed, not a global game inventory hook. A third-party inventory
change becomes visible only when H2 reads that actor again; memory-only revision reads do not
scan the game. Mesh readiness, device capabilities and exact inventory-instance identity
remain separate evidence. A consumer rechecks compatibility after a notification rather
than treating every ownership change as an incompatibility.

## Lifetime and authority

Create a scope with a copied owner label (1–256 bytes), a lifetime, and a scene handle.
The returned nonzero 64-bit token authorizes operations on that scope. Owner labels are
diagnostic names, not authentication. Multiple scopes may have the same label.

| Lifetime | Scene argument | End |
|---|---|---|
| Persistent | Zero | Explicit close or world replacement; survives scene end and saving |
| Session | Live scene handle | Explicit close, scene termination, or world replacement; survives same-cast handoff |

Session API requests must target a participant in that scene. The runtime closes all
session scopes before delivering scene-end callbacks and again when releasing the slot
to cover failure paths. Closing one scope cannot close another owner's scope.

Scope and claim tokens share a monotonic, never-recycled space. They are process-local and
must never be serialized. World reset revokes both without touching old-world inventory.
An owning mod persists its device facts and re-registers them after the new world is ready;
the reset callback is only for invalidating local tokens. Close scopes before unloading a
provider.

All calls require serialized game context; use the engine's verified-frame `Post` queue
from other contexts. Queue calls from equipment/inventory event listeners rather than
reentering the service during its backend operations. The store does not hold a mutex
across engine calls.

## Equipment requests

Include `OSFEngineAPI.h` and its companion public headers, including `OSFActorState.h`.
Request services 3.9 before accessing the appended functions.

| Operation | Demand |
|---|---|
| Equip | Keep the base form equipped; add one copy only when absent and `ActorEquipmentAllowAdd` is explicit |
| Preserve | Keep an existing item equipped; never create a missing item |
| Hide | Require this item unequipped; refuse a conflicting Equip/Preserve claim |
| HideOptional | Hide unless another owner requests Equip/Preserve; used by legacy stripping |

Compatible positive claims share the item. Multiple hide claims keep it hidden until the
last demand ends. Two required positive demands occupying overlapping armor slots conflict.
Non-armor forms conservatively share one held-item channel; hand-specific fitting is not
inferred. The actor's skin cannot be hidden.

`group` is a caller-chosen release lane within a scope. `ActorEquipmentReplaceGroup`
allows an Equip request to retire only conflicting Equip claims in that same scope and
group. Other owners, groups, and Preserve claims still block it. Claims in non-overlapping
slots remain. Identical requests in the same scope/group return the same claim, without
incrementing a reference count.

A typical persistent device registration, executed in game context:

```cpp
using namespace OSF::API;
const auto* api = RequestEngineAPI(); // current header requests 3.10
if (!api) return;
const auto scope = api->CreateActorStateScope(
    "example.device", ActorStateLifetime::Persistent, 0);
if (!scope) return;

ActorEquipmentRequest request;
request.actor = actor;
request.object = device;
request.operation = ActorEquipmentOperation::Preserve;
request.group = 1;
std::uint64_t claim = 0;
const auto status = api->AcquireActorEquipment(scope, &request, &claim);
if (status != ActorStateStatus::Ok) {
    api->CloseActorStateScope(scope);
    // Retain/report the refusal; do not assume the device is protected.
    return;
}
// Retain scope/claim in the provider's runtime state.
// On owner withdrawal: ReleaseActorStateClaim(scope, claim), or CloseActorStateScope(scope).
```

`ReleaseActorStateClaim` requires the issuing scope. `ReleaseActorStateGroup` releases
only that scope's selected lane; `CloseActorStateScope` releases all its claims.
Repeated releases return Invalid and cause no mutation. A successful acquisition records
ownership before the first backend write. Failure retires the attempted claim and
reconciles its partial changes; failed replacement restores the prior claims.

| Status | Meaning |
|---|---|
| Ok | Requested inventory reconciliation completed |
| Invalid | Bad request, stale/wrong token, or actor outside the session cast |
| Missing | Required inventory item is absent and adding is not allowed |
| Conflict | Required claims or current worn slots are incompatible |
| Unavailable | Form/inventory cannot be read, token space exhausted, or session acquisition attempted during saving |
| Failed | Backend write/readback did not establish the requested state, or an API exception occurred |

Release/close retire the handle even when cleanup fails. Residual mutation records remain
in Animation; call `ReconcileActorState(actor)` when the actor/inventory becomes available.
An acquisition failure can also leave these retryable records with no returned claim.
There is no background retry scheduler in this slice.

## Reconciliation and provenance

The service records the first observed worn baseline, its own changes, displaced equipment,
and whether it added a missing copy. It re-reads inventory after each mutation. A pending
operation survives a lost readback so a later read can distinguish its result from an
external change. Cleanup keeps surviving owners' demands and restores only still-owned
deltas. Removed items are not resurrected; newly worn conflicting items are not displaced
by cleanup.

The backend identifies items by **actor form ID and base-form ID**, with total count and
worn state. It does not identify individual inventory stacks, modifications, or instances.
When an observed external count change makes ownership ambiguous, destructive removal is
relinquished. This can retain a service-added copy rather than risk deleting someone else's.
An external replacement that preserves the same count and worn state cannot be detected.
Mods needing exact-instance guarantees must not treat this API as that guarantee.

A newly acquired Equip/Preserve demand may displace unclaimed worn equipment and records
its baseline. Register device preservation before scene preparation/stripping so explicit
conflicts protect it. The service cannot infer an unregistered item's lock or owner.

## Existing scene behavior

The scene holds one session scope with separate apparel, held-item, authored-item, and
automatic-gear groups. Authored sequential equipment replacement remains allowed within
its own group and supersedes conflicting automatic-gear claims in that same scene scope.
Other owners still block it. `osf.equipment.unequip` releases authored claims only; automatic gear and
external preserve demands survive that action; superseded gear is not reacquired. Automatic gear uses existing inventory only,
and even already-worn gear is claimed so another scene operation cannot strip it. Default
apparel stripping and held-item clearing are optional hide demands, yielding to every
active positive claim.

The ledger now releases groups instead of replaying independent inventory snapshots.
Final scene cleanup closes the session; the same handle retains ownership across handoff.
Required default/entry equipment now participates in H3 preparation before dependent playback;
conflicts refuse admission. Timed/ordinary equipment requests still log refusal and retain
existing ownership without rewinding already-started playback. Preparation edits snapshot only
selected scopes' claims, retain provenance across temporary release, and reconcile rollback
against live inventory without replacing another owner's claims.

## Saving and world replacement

The existing save lifecycle suspends session demands before serialization while persistent
demands remain active. Temporary copies with no surviving persistent demand are removed,
and owned displaced apparel is restored. The claims remain alive.

After saving, reconciliation reapplies session demands. It may displace apparel restored
from its own captured baseline, but refuses to displace a newly worn conflicting device.
Failure is reported and records remain retryable. Closing a session during the save window
cannot resurrect its items when saving ends. Reset drops all records, claims, and scopes
without backend writes; the next registration starts from the new world's actual state.

SaveLoadEvent begins cover event-producing saves. The normal `Game.RequestSave()` path
has no begin event, so a byte-checked main-request-pump hook also opens this same window
around its synchronous dispatch. A return guard closes the window even when the engine
refuses the request. Persistent demands remain active in either path. See
[PERSISTENCE_API.md](PERSISTENCE_API.md) for the native compatibility boundary.
Native tests do not establish runtime thread affinity, serialization ordering, or mesh timing.

## Validation

`osf-actor-state-test` runs the production ownership core against an inventory backend with
failure injection. It covers concurrent persistent/session owners, shared claims, wrong
and stale tokens, pre-owned/pre-worn items, displaced apparel, strict/optional hiding,
group replacement/release, devices introduced mid-session or during saving, external count
changes, failed writes, lost readback on acquisition/release, deferred cleanup, save
suspension, and reset without old-world writes.

Run `xmake test` for all native suites and `tools/Test-BrowserBoundary.ps1` for the public
header boundary, plus the production DLL build. Fresh engine inventory, scene handoff,
save/load, device interoperability, and visual acceptance still require live fixtures.
