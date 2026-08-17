# Health reporting

OSF Animation reports durable, actionable issues into OSF UI's shared **System
Health** pane through native ABI **1.7** (`Feature::kDiagnostics`). The producer
implementation is `src/API/Health.{h,cpp}`; the host contract is documented in
`OSF UI/docs/native-plugin-api.md` §5d.

Codes are local to the `osf.animation` source. OSF UI exposes them under the
`osf.animation:<code>` namespace.

## Codes

| Condition | Code | Severity | Why it qualifies |
|---|---|---|---|
| Game version not the tested one (`main.cpp` boot check) | `boot.untested-game-version` | warning | True for the whole session; the player just updated Starfield and needs to know why things may misbehave. |
| A consumer requires a newer OSF Animation (`MinimumVersion`) | `compat.needs-newer-osf-animation` | warning | The consumer may be disabled or incompatible until the player upgrades; the card records consumer, installed version, and minimum version. |
| A scene pack could not be loaded (`SceneRegistry`, per file) | `catalog.pack-load` | error / warning | Those animations are silently absent from the browser — until now the only trace was a log line. Severity follows the worst line for that file. |
| A sound pack could not be loaded (`SoundRegistry`, per file) | `sound.pack-load` | error / warning | Same, for voice/sound pools; different code because it is fixed somewhere else. |
| Cross-file problems with no single owner (a dangling `use`) | `catalog.cross-file` | error / warning | No file to name, so they share one card rather than being dropped. |
| Screen fades disabled (`FadeService` prologue mismatch) | `fade.disabled` | warning | A feature is off for the session, and the reason (a game patch moved the poster) is not guessable. |

One card per **file**, not per problem: twelve rejected scenes in one bad pack
are one thing to fix, and twelve cards would bury every other condition in the
pane. The card carries the file name, the problem count, and the first four
lines. Complete registry diagnostics stay in the log; scene problems are also
available from `OSFAdvanced.GetSceneLoadErrors()`.

Deliberately **not** reported:

- A single failed `OSF.Play` (missing `.af`, unresolved rig). Per-attempt, not
  durable — it belongs in the log and in the caller's return value.
- Anything the engine recovers from within a tick or two (lock-leak recovery,
  orbit re-engage). Already corrected by the time it could be read.
- Counts, timings, "loaded N packs". That is a log, and System Health is not one.
- The OSF UI update badge (`kOSFUITested` in `UIBridge.cpp`). If Animation needs
  a newer host, the right expression is a `targetVersion` in its view manifest —
  OSF UI's own `compat.needs-newer-osfui` producer then raises it, with the
  **Update OSF UI** action already wired. Do not hand-roll a second card for it.

## Producer rules

- Report only conditions that are durable, actionable, and worth interrupting
  the player for. Keep the existing log line; the card is additive.
- Reports made before the bridge exists are buffered and replayed by `Connect()`.
- `ReportRegistryLoad()` reconciles only the registry issue IDs it owns; it does
  not clear cards raised by other producers.
- **Never put player-facing prose in a report.** The `code` is machine-readable;
  OSF UI owns the localizable wording.
- **Never pass an absolute path in `context`.** It identifies the player's
  machine and account. The host redacts path-shaped values to their trailing
  component anyway — pass the bare filename yourself and keep the report honest.
- **Withdraw what you raise.** A card that outlives its condition makes the whole
  pane untrustworthy, and there is no dismiss button by design.
