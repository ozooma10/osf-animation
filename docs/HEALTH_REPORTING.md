# Health reporting

**Decision: OSF Animation does not get its own diagnostics page.** It reports into
OSF UI's **System Health** pane, which every OSF module and every third-party mod
shares.

Two health pages would mean a player has to know *which mod broke* before they
know *where to look* — exactly backwards for diagnostics. System Health also owns
things Animation cannot see on its own: the host version, the renderer path, view
load failures, settings parse errors. Animation-specific detail still lives here,
but as issue records with stable codes and bounded context, not as a second UI.

The mechanism is OSF UI's native ABI **1.7** (`Feature::kDiagnostics`), added
2026-07-25. Contract and rules: `OSF UI/docs/native-plugin-api.md` §5d.

## What belongs in the pane

Report only conditions that are **durable** (still true when the player reads the
card), **actionable** (they can do something, even if that something is "tell the
author"), and **worth interrupting them for**. Everything else stays in the log.

That test excludes more than it admits, on purpose. A pane full of noise is a
pane players learn to skip, and the pane's whole promise is that everything on it
is true right now.

| Condition | Code | Severity | Why it qualifies |
|---|---|---|---|
| Game version not the tested one (`main.cpp` boot check) | `boot.untested-game-version` | warning | True for the whole session; the player just updated Starfield and needs to know why things may misbehave. |
| A scene pack could not be loaded (`SceneRegistry`, per file) | `catalog.pack-load` | error / warning | Those animations are silently absent from the browser — until now the only trace was a log line. Severity follows the worst line for that file. |
| A sound pack could not be loaded (`SoundRegistry`, per file) | `sound.pack-load` | error / warning | Same, for voice/sound pools; different code because it is fixed somewhere else. |
| Cross-file problems with no single owner (a dangling `use`) | `catalog.cross-file` | error / warning | No file to name, so they share one card rather than being dropped. |
| Wheel pins cannot be written (`WheelPins`) | `wheel.pins-not-persisted` | warning | Customization is lost on exit and stays lost until the permissions/disk problem is fixed. Cleared when a later save succeeds. |
| Screen fades disabled (`FadeService` prologue mismatch) | `fade.disabled` | warning | A feature is off for the session, and the reason (a game patch moved the poster) is not guessable. |

One card per **file**, not per problem: twelve rejected scenes in one bad pack
are one thing to fix, and twelve cards would bury every other condition in the
pane. The card carries the file name, the problem count, and the first four
lines; anything more is in the log and in `OSF.GetSceneLoadErrors`.

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

## Slices

**Slice 1 — host support (done, 2026-07-25, in the OSF UI repo).** ABI 1.7:
`ReportIssue` / `ClearIssue` / `ClearIssuesExcept` on `IOSFUIBridge` + `Client`;
`BridgeApi` validates synchronously and queues, `Runtime::DrainDiagnosticOps`
applies on the main tick with the source, id and code namespaced to the calling
mod; the frontend renders an unrecognised third-party code as a card naming that
mod (`MOD_COPY`) instead of telling the player to update OSF UI, and the rail
severity marker attributes a mod's own reports by `source`.

**Slice 2 — Animation reports (done, 2026-07-25).** `src/API/Health.{h,cpp}` owns
the producer side: `Connect` / `Report` / `Clear` / `KeepOnly` /
`ReportRegistryLoad`, with the mod id baked in so no call site repeats it and
none has to think about version gating. The vendored `src/API/OSFUI_API.h` is
refreshed to 1.7. Wired at `main.cpp` (boot version, registry sweep),
`OSFScript.cpp` (`ReloadPacks` re-reports and reconciles), `WheelPins.cpp`, and
`FadeService.cpp`. Every site keeps its existing log line — the card is additive.

Two things worth knowing before adding a producer:

- **Reports made before the bridge exists are buffered.** The game-version check
  runs at plugin load, long before `Connect()`; `Health` holds up to 32 ops and
  replays them in order. Report freely from anywhere.
- **The host's sweep is mod-wide, not per-producer.** `ClearIssuesExcept`
  withdraws every active issue of *this mod* that is not in the keep list, so a
  producer reconciling its own set must hand over every OTHER live id too or it
  silently clears their cards. `Health` tracks what it has raised for exactly
  this reason (`LiveNonPackIssues`); use that, don't call `KeepOnly` with a bare
  subset.

**Slice 3 — copy.** Once the codes are live and stable, OSF UI can graduate the
common ones from the generic mod card to authored, localizable copy in
`frontend/src/lib/settings/diagnostics.ts` (`COPY`), keyed on the namespaced code
(e.g. `osf.animation:catalog.pack-load`). Not required — the generic card is a
complete, honest fallback — but it is what turns "OSF Animation reported a
problem" into "A scene pack could not be read".

## Rules that are not negotiable

- **Never put player-facing prose in a report.** The `code` is machine-readable;
  OSF UI owns the wording so it stays localizable and no mod can write the words
  on its own card. If a condition needs explaining, the explanation goes in
  slice 3, not in the payload.
- **Never pass an absolute path in `context`.** It identifies the player's
  machine and account. The host redacts path-shaped values to their trailing
  component anyway — pass the bare filename yourself and keep the report honest.
- **Withdraw what you raise.** A card that outlives its condition makes the whole
  pane untrustworthy, and there is no dismiss button by design.
