# OSF Animation — Launch checklist (v0.x beta)

The beta release plan. This file is the single source of truth for "what's left before we ship";
AGENTS.md references it as the launch roadmap. Companion docs: API audit →
[docs/API_REVIEW.md](docs/API_REVIEW.md) · SAF migration → [docs/SAF_MIGRATION.md](docs/SAF_MIGRATION.md).

> **Status legend:** ✅ done · 🟡 decide / in progress · 🔴 blocker · ⚪ out of scope for v0.1

## 1. Release shape

- **Target: a public `0.x` beta**, *not* a frozen 1.0. **Phases A+B+C are now all landed + in-game
  tested (2026-06-17)** — the §1.7 condition for the 1.0 freeze is met, so the surface is at its
  freeze candidate. A `0.x` beta still ships first to gather real-world use before tagging 1.0.
- **Stability promise:** **Tier 0 primitives are stable**; the **scene API is "beta, may refine"**
  until 1.0 (its signatures are now freeze-candidate-stable). Say this loudly so early adopters know
  what's safe to build on.
- **Headline: a drop-in SAF replacement.** Existing SAF play/sync/scene content runs unchanged via
  the shim. Secondary: a better framework for new mods (the scene graph).
- **Engine-only.** No bundled content; OSF is a dependency. Showcase content comes from companion
  mods (Seduce, third-party, ported SAF content).
- **Phase C is DONE** (equipment/fade/voice/sound/camera mechanisms + services, the generalized undo
  ledger, settings precedence, validation) — all built-in `osf.*` actions execute and all four track
  lanes run. (Deferred-but-additive: free-fly camera, pool/set→clip resolution, positioned Wwise.)

## 2. Readiness checklist

### Code / features
- ✅ Tier 0 primitives (play/stop/speed/anchor/sync/sequence/state).
- ✅ Scene runtime: handles, navigation, auto-advance, linear stage interface, roles, callbacks,
  exclusivity, load-safe handles.
- ✅ All four track lanes — cue (lifecycle + numeric + trigger-edge auto-take), action, sound, camera —
  on one generalized clip-timed mark with the §1.3 same-tick order.
- ✅ Built-in `osf.*` actions: `control.lock`/`release`, `fade.out`/`in`, `equipment.hide`/`restore`,
  `voice.play` — all execute; custom actions → `EVENT_ACTION`; generalized reverse-order undo ledger.
- ✅ Settings precedence (silent-skip when disabled, `Data/OSF/settings.json`) + per-action validation.
- ✅ SAF shim (two scripts: `SAF`, `SAFScript`).
- ✅ Save safety, co-load warning, version gating, post-patch checklist.
- ✅ **Resolved:** no `osf.*` action is unimplemented now — an unknown `osf.*` type is *rejected at
  load* (loud), so authors are never surprised by a silently-skipped policy action.
- ⚪ Phase-D camera (free-fly/orbit), pool/set metadata, positioned Wwise — additive, post-v1.

### API quality (see API_REVIEW.md)
- ✅ **Callback struct field names locked** (`OSFEvent:SceneEvent`) — the ABI names are final:
  `sceneHandle`, `eventType`, `node`, `edge`, `cue`, `actionType`, `actorRef`, `role`, `loopIndex`,
  `time`, `anchor`, `result`. `actorRef` is a real Actor object on role-bearing `EVENT_ACTION`;
  `loopIndex` is present and currently reserved/defaults to `-1` until loop-index reporting is wired.
- ✅ `ValidateScene` / `GetSceneValidationErrors` — bound and documented; per-scene diagnostics now
  mirror the `GetSceneLoadErrors` load-error stream.
- ✅ `HasFeature` — recognizes merged-engine capabilities (`cues`/`actions`/`sound`/`camera`/
  `callbacks`/`weapon`) through the aggregate engine gate.
- ✅ Constants ergonomics decided — keep `OSF.EVENT_X()` / `OSF.RESULT_X()` getter functions. A
  non-Native `OSFConst.EVENT_X` property companion was compiler-tested and rejected because Papyrus
  cannot read properties directly on a type name.
- ✅ `GetSceneStageForActor`/`SetSceneStageForActor` ratified into `OSF.psc` + `docs/API.md`.

### Docs
- ✅ **Doc-drift sweep (2026-06-17)** — reconciled the pre-merge "policy lives in a separate OSF
  Intimacy plugin" framing to the merged reality across README, AGENTS.md, `docs/{API, GETTING_STARTED,
  PACK_SCHEMA, INTIMACY_SEAM, NEXUS_PAGE, guide/index, guide/cookbook}.md`, TESTSUITE.md, and fixed the
  broken bare `DESIGN.md` links (→ `SCENE_DESIGN.md`).
- ✅ `docs/API_REVIEW.md`, `docs/SAF_MIGRATION.md`, `LAUNCH.md`.
- ✅ `SCENE_DESIGN.md` / `API.md` / `AGENTS.md` / `INTIMACY_SEAM.md` — Phase-C reconciliation pass
  (2026-06-17): status banners + §2.5 phasing + §1.7 freeze + the action/lane/ledger surface; API.md
  quickstart updated to the `StartScene*`→int-handle returns; INTIMACY_SEAM registry table de-staled.
- ✅ `docs/RE.md` + `docs/POST_PATCH_CHECKLIST.md` — ownership framing updated: Layer-C
  equipment/fade/voice/Wwise/weapon bindings are in this repo, prologue-gated, and covered by the
  post-patch recovery checklist.
- ✅ `SCENE_DESIGN.md` §1.2 — removed the stale `GetCallbackEvent`/`Result`/`Time` dispatch-time
  getters from the sentinel table (the struct-payload transport replaced them); points to the
  `OSFEvent:SceneEvent` fields.
- ✅ `docs/INTIMACY_SEAM.md` — rewritten from the old cross-plugin "OSF Intimacy" boundary to the
  current internal Layer A ↔ Layer B seam.

### Packaging
- ✅ **FOMOD structure drafted (2026-06-17)** — `packaging/fomod/{info,ModuleConfig}.xml` +
  `packaging/build-archive.ps1` (assembles Core / SAF-shim / Examples from `build/` + `dist/`,
  excludes the `.pdb` + dev probes, stamps the version, zips). Verified: produces a valid 1 MB
  archive. See [docs/PACKAGING.md](docs/PACKAGING.md).
- ✅ **Version bumped** — `xmake.lua` set to `0.1.0` (rebuilt, so `GetVersion()` matches). Re-run the
  archive script at package time to stamp the name.
- 🟡 **No `versionlib` bundled** — OSF uses the **Address Library** mod (a hard dependency, *not*
  bundled). Deps to state on the page: **Starfield 1.16.244.0**, matching **SFSE**, **Address
  Library**. CLSF is statically linked (not a user dependency).
- ✅ **No `OSFConst` package artifact** — the companion script was rejected; Core ships
  `OSF`/`OSFCompat`/`OSFEvent` scripts and constants remain `OSF.EVENT_X()` / `OSF.RESULT_X()`.

### Compatibility & safety (verify one pass each on a real save)
- ✅ Co-load warning for SAF / NAFSF (they're mutually exclusive with OSF).
- ✅ Game-version gating (engine bindings self-disable on mismatch).
- ✅ Save safety (handles die across load; `StopAll` drops state).
- ✅ `docs/POST_PATCH_CHECKLIST.md` for game updates.
- 🟡 **Verify:** loading a save made *with SAF installed* after switching to OSF (clean transition?).

### Testing — *the biggest gap*
- ✅ **Plan drafted (2026-06-17):** [docs/REALWORLD_TEST_PLAN.md](docs/REALWORLD_TEST_PLAN.md) — a
  prioritized checklist (§1 GATE smoke test, §2 SAF migration, §3 native framework, §4–6 stability/
  soak/compat) with explicit beta-GATE criteria. Complements the synthetic `OSFTest` harness.
- 🔴 **Execute it** — everything so far is synthetic. The headline is unproven until §2 passes on a
  real SAF mod and §3.1–3.3 on a real community SLAL pack. (Needs actual mods + in-game runs.)
- 🟡 §3.6 callback round-trip needs a minimal **test ESP** (a scripted-form receiver) — the
  method-dispatch callback path is still unproven without a real scripted instance.

### Distribution & community
- 🟡 LoversLab + Nexus pages (`docs/NEXUS_PAGE.md` exists — refresh for beta).
- 🟡 Courtesy heads-up to SAF (mielu91m) and NAFSF (Deweh) — OSF is a replacement; attribution is in
  the headers (GPL-3.0).
- 🟡 Support channel (issue tracker / Discord).

## 3. Top blockers (the few that actually gate the beta)

1. **Real-world testing** — real SAF mods + real animation packs through the shim. (#1 risk.)
2. **Package the exact tested build** — FOMOD structure + assembly script are drafted & verified
   (2026-06-17), and `xmake.lua` is already `0.1.0`. Remaining: rebuild, archive, and hash-match the
   `releasedbg` DLL that passed the real-world gate.
3. ✅ **Doc reconciliation** — pre-merge framing killed (2026-06-17); SAF migration guide shipped;
   Phase-C/API/packaging status now matches the code.
4. **Publish/support prep** — refresh the LoversLab/Nexus copy, pick a support channel, and send the
   courtesy heads-up to SAF / NAFSF maintainers.

Everything else is polish or fast-follow.

## 4. API punch-list (remaining beta polish)

The beta-facing API punch-list is closed:

- ✅ `ValidateScene(asId)` + `GetSceneValidationErrors(asId)` bound.
- ✅ `HasFeature` answers `cues`/`actions`/`sound`/`camera`/`callbacks`/`weapon`.
- ✅ `GetSceneStageForActor`/`SetSceneStageForActor` are documented in `OSF.psc` + `docs/API.md`.
- ✅ Unknown `osf.*` actions are rejected at load (loud).
- ✅ Callback field names are locked; `actorRef`/`role` are populated for role-bearing custom
  actions. `loopIndex` remains a reserved/defaulted field until loop-index reporting is implemented
  or explicitly deferred in the 1.0 contract.

Optional fast-follow only: `ReloadContent` as a clearer alias of `ReloadPacks`.

## 5. Explicitly out of scope for v0.1 (now post-v1 / additive)

`RetargetLiveScene` seamless transitions · per-node role→slot remap · bundled content · the Papyrus
scene builder · scene parameters / auto-drive · free-fly/orbit camera · pool/set→clip metadata
resolution · positioned Wwise · the `equipment.hide` `slots` filter · per-role `equipment.restore`.
(Phase-C mechanisms themselves are **done + in v0.1**, no longer out of scope.)
