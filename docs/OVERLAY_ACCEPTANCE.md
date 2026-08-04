# Overlay route acceptance and downstream handoff

Use this checklist before treating native overlay ABI 1.0 as frozen. The engine-free suite proves
the contract mechanics; the following items require a running Starfield session or the downstream
Studio/Suit Protocol repositories.

## Native consumer smoke path

The route test target compiles `OSFOverlayAPI.h` as a consumer would and covers structured begin
results, callback exception isolation, owner uniqueness, quiescent release, playback admission, and
strict pre-mutation mark bounds. Run the repository gates with `xmake test` after a releasedbg build.

In a development consumer, request ABI 1.0, acquire one owner, and log every begin result and event.
Confirm these cases:

- invalid owner -> `rejected/ownerInvalid`;
- unknown route -> `rejected/routeUnknown`;
- unknown initial station -> `rejected/unknownStation`;
- occupied overlay or unrelated playback -> `rejected/busy`;
- missing actor 3D or an active scene -> a nonzero handle with `pending/actorUnavailable` or
  `pending/sceneBlocked`;
- begin from inside a callback -> a nonzero handle with `pending/dispatchDeferred`, followed by
  normal realization or a `kFailed` event;
- external prop marks -> `kPropAttach`/`kPropDestroy` with `event.prop`, never `kMarker` inference.

## In-game visual and lifecycle matrix

- Walk, run, turn, and idle indefinitely at an animated Held station; inspect the upper-body seam.
- Traverse a multi-edge route in both authored directions and look for playback-boundary pops.
- Countermand once before and once after commit; verify the callback is exactly-once and checkpoint
  recovery never repeats an acknowledged ownership handoff.
- Start a cinematic scene during both a station and a transition. The overlay must stop immediately
  before scene playback, then reconcile from its checkpoint after every normal, failed-start, abort,
  and load-teardown path. No crossfade is expected in Phase 1.
- Unload/reload actor 3D during a transition and at a station; requests remain pending and stale
  generations never fire after reconciliation.
- Exercise OSF-owned transition/station/controller props and external attach/destroy props. Verify
  cleanup on end, scene suspension, owner release, and world replacement.
- Save while Held, replace the world, then have the consumer reassert its semantic state. OSF must
  retain no actor/route state while the process-lifetime owner remains usable.

## Downstream Studio and Suit Protocol gate

Studio should add a `routes` output target that emits the schema in `ROUTE_SCHEMA.md`, including
mandatory masks, directed edges, explicit commit markers, prop-only lifetimes, and one-shot
marker/sound entries. Keep its existing compiled-scene output during parity testing.

Suit Protocol should feature-detect overlay ABI 1.0 and dual-ship: use `BeginRoute`/`RequestStation`
and typed prop/commit callbacks when available, while retaining the current scene/cue fallback.
Its atmosphere policy, transactions, persistence, and load reconciliation remain consumer-owned.
Remove the fallback only after the visual/lifecycle matrix passes on the same authored route in both
paths.

Declarative DAR/OAR-style conditions are not part of this gate. They remain a future owner that can
drive the same controller after Phase-2 arbitration/claims are designed.
