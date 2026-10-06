# Health reporting

OSF Animation reports durable problems to **OSF Settings > Mod Issues**, using the
diagnostics SDK ABI 1.0. `src/API/Health.{h,cpp}` owns the wording and forwards
`Issue` records through `OSFSettings_Diagnostics.h`. OSF UI is not required.

Codes below select Animation's title and next-step text. Stable issue IDs replace
existing reports; clearing an issue removes it.

## Codes

| Condition | Code | Severity | Why it qualifies |
|---|---|---|---|
| Game version not the tested one (`main.cpp` boot check) | `boot.untested-game-version` | warning | True for the whole session; the player just updated Starfield and needs to know why things may misbehave. |
| A consumer requires a newer OSF Animation (`MinimumVersion`) | `compat.needs-newer-osf-animation` | warning | The consumer may be disabled or incompatible until the player upgrades; the card records consumer, installed version, and minimum version. |
| A scene pack could not be loaded (`ContentRegistry`, per file) | `catalog.pack-load` | error / warning | Those animations are silently absent from the browser — until now the only trace was a log line. Severity follows the worst line for that file. |
| A sound pack could not be loaded (`SoundRegistry`, per file) | `sound.pack-load` | error / warning | Same, for voice/sound pools; different code because it is fixed somewhere else. |
| Cross-file problems with no single owner (a dangling `use`) | `catalog.cross-file` | error / warning | No file to name, so they share one card rather than being dropped. |
| Wheel pins cannot be written (`WheelPins`) | `wheel.pins-not-persisted` | warning | Customization is lost on exit and stays lost until the permissions/disk problem is fixed. Cleared when a later save succeeds. |
| Screen fades disabled (`FadeService` prologue mismatch) | `fade.disabled` | warning | A feature is off for the session, and the reason (a game patch moved the poster) is not guessable. |
| Browser assets installed but host/API/assets/registration fail | `browser.integration` | error | The browser cannot open. The reason identifies the missing/unsupported host or invalid installation and gives the corrective action. Engine playback remains independent. |

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
- Counts, timings, "loaded N packs". That is a log, and Mod Issues is not one.
- The optional browser addon is absent. This is a supported core-only installation.
## Producer rules

- Report only conditions that are durable, actionable, and worth interrupting
  the player for. Keep the existing log line; the card is additive.
- Reports made before the bridge exists are buffered and replayed by `Connect()`.
- `ReportRegistryLoad()` reconciles only the registry issue IDs it owns; it does
  not clear cards raised by other producers.
- Animation owns player-facing titles, impact text, and next steps. Context is
  flattened into the impact text and path-sanitized before it reaches OSF Settings.
- **Never pass an absolute path in `context`.** It identifies the player's
  machine and account. Pass bare filenames and sanitize free-form details.
- **Withdraw what you raise.** A card that outlives its condition makes the whole
  pane untrustworthy, and there is no dismiss button by design.
