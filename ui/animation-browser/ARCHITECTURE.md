# Animation browser architecture

The browser is a single Octane application embedded in OSF UI. Native OSF Animation owns the
catalog and scene runtime; this application owns selection, pre-flight configuration, presentation,
and input translation.

```text
OSF Animation DLL
      | JSON text
      v
bridge/client.ts ---- dev/mock-bridge.ts
      ^                     ^
      |              Vite standalone preview
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

`npm run dev` serves the standalone browser at `http://localhost:8791/` with Vite HMR.
In development, a page without native bridge capabilities uses the stateful standalone bridge
for animation launches, scans, wheel edits, and active-scene transitions. Production always
uses OSF UI. Committed snapshots under `fixtures/live/` provide standalone catalog data.

## Settings

OSF Settings owns the schema, persistence, hotkeys, and diagnostics. The DLL publishes a flat
map of browser preferences through OSF UI's retained `settings` state. Both native and
standalone adapters replay it on subscription and publish replacement snapshots after writes.
The controller applies opening preferences only on the initial replay; subsequent updates
preserve the current browsing session. Writes use `osf.animation.settings.set`, with native
validation of the source view, allowed preference keys, and typed values.
