# Animation browser agent guide

This directory is the editable source for OSF Animation's in-game browser. The generated
`../../build/views/osf.animation/browser/` directory is disposable output; never edit it.

## Required checks

Run `npm run verify` from this directory after source changes. The command runs the unit tests,
strict TypeScript checking, and the production Vite build.

## Architecture

Data flows in one direction:

`OSF UI JSON -> bridge decoder -> browser action -> reducer -> selectors -> Preact components`

User events flow back as a browser action or a typed bridge command. Keep these boundaries:

- `src/app/` owns application state, actions, the reducer, and orchestration.
- `src/bridge/` owns JSON/native transport and payload validation. Components never access
  `window.osfui` directly.
- `src/features/` owns user-facing feature components and feature-local pure helpers.
- `src/input/` owns document-level keyboard, raw-gamepad, and orbit effects. Every installed
  listener must return cleanup.
- `src/dev/` owns standalone mocks and debug-only UI.
- `src/model.ts` owns normalized registry models and readiness evaluation.

Prefer pure selectors and reducer cases over stateful component logic. Do not introduce a second
state library unless the existing reducer demonstrably cannot express the behavior.

## In-game invariants

- Production output is an ES2018 IIFE for OSF UI's Ultralight host. Avoid unsupported browser APIs,
  dynamic imports, network dependencies, and CDN assets.
- Only JSON text crosses `window.osfui`; decode unknown native payloads at the bridge boundary.
- Bridge evolution is additive. Require bridge presence, but do not gate on version strings.
- The player token is `-1`. Other reference tokens are opaque integers and must never be treated as
  game pointers or form IDs.
- The view takes the `osfui.gamepadRaw` grant. D-pad/A/B are recreated in the input adapter while
  sticks remain available to the native orbit camera.
- A hidden view must clear wheel/live transient state, pending orbit deltas, and held-pad repeats.
- NPC-only scenes survive closing the browser. Never infer scene termination from view visibility.
- Preserve focus across updates and keep every interactive control keyboard/gamepad reachable.
- Keep the standalone fixture bridge behaviorally equivalent to the native bridge contract.

## Component conventions

- Components render JSX and receive typed values/callbacks. Do not use `innerHTML`, manually rebuild
  DOM subtrees, or add delegated `data-act` routers.
- Put derived filtering/grouping/readiness logic in selectors, not render bodies.
- Keep feature CSS beside the feature when practical; shared tokens/layout remain under `styles/`.
- Add reducer/selector tests for new behavior and bridge tests for contract changes.

