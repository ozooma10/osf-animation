# Animation browser architecture

The browser is a single Preact application embedded in OSF UI. Native OSF Animation owns the
catalog and scene runtime; this application owns selection, pre-flight configuration, presentation,
and input translation.

```text
OSF Animation DLL
      | JSON text
      v
bridge/client.ts ---- dev/mock-bridge.ts
      |
      v
app/controller.ts (effects and retries)
      |
      v
app/reducer.ts -> app/selectors.ts -> feature components
      ^                                  |
      +----------- actions/commands -----+
```

## State versus effects

Reducer state contains facts needed to render or make a later decision: received catalogs, cast,
anchor, selection, filters, active scenes, launch state, and transient wheel state. Values that can
be calculated from those facts—playability, grouping, busy tokens, duration labels, wheel candidates,
and filtered lists—are selectors.

Timers, bridge sends, focus movement, document listeners, animation frames, fixture loading, and
body classes are effects. They do not belong in reducers or model functions.

## Bridge policy

`bridge/contract.ts` is the protocol source of truth. Incoming JSON begins as `unknown` and becomes a
discriminated event only after decoding. The production client and standalone client implement the
same interface, so feature code does not branch on bridge availability.

## Development

The existing Vite `/frame` route is the visual harness because it reproduces the 1600x900 game
composition. Committed snapshots under `fixtures/live/` feed the standalone bridge. Storybook is
intentionally unnecessary: a second fixture/runtime system would drift from the actual bridge.

