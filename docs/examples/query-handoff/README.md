# Query, alignment and handoff example

Requires OSF Animation 1.5.0. This example launches two available human actors by
tag and role constraints, applies a temporary alignment nudge, swaps their roles
through a query-based handoff, and explicitly stops the retained handle.

1. Install `example.osf.json` in your content mod under `Data/OSF/Examples/`.
   Both clips reference an installed vanilla asset; no game asset is redistributed.
2. Compile `OSFQueryHandoffExample.psc` with the shipped OSF Papyrus sources on
   your import path, and attach it to a quest in your own test plugin.
3. Invoke `RunExample(leadActor, partnerActor, worldAnchor)` from that quest with
   two distinct actors and a reference in clear space. It returns false if launch
   or handoff is refused. `StopExample()` is the caller's cancellation path.

The scene opts out of apparel stripping, input locking and camera changes. It
uses an explicit anchor, so actors return to their original positions when the
scene ends, including after the role swap. Run with idle actors in a disposable
save; this is a placement example, not a finished animation pack.

Expected behavior: the start scene plays at half speed for two seconds, the target
scene uses the same handle and half speed for another two seconds, then stops.
Role placement changes snap; pose replacement blends. Alignment belongs to the
specific scene/node/role, so the start scene's temporary nudge does not carry into
the target definition. Save it only after an explicit user choice by calling
`OSF.SaveSceneAlignment(handle)` before the handoff.

For your own content, replace the clips and tags, retain a handle per operation,
and cancel that operation when its quest shuts down. The example guards concurrent
starts and rechecks its handle after waiting. World replacement invalidates runtime
handles; a production quest should also clear its stored handle when loading.

Loop counts may instead use `{"global":"MyMod.esm|0x1234","default":3}`. Values
are captured at start/handoff. Manual progression is a separate native services
3.5 capability (`SetSceneAutoAdvance`) for nodes declaring `allowManualHold`;
it keeps animation running rather than pausing its clock. See [API.md](../../API.md).

The release fixture stage compiles this exact script. The registry suite parses
this exact JSON; the live suite validates its imported scene identities alongside
the separate private interaction fixtures. The example is not installed by the
engine or Browser archives.
