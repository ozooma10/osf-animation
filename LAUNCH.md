# OSF Animation — Launch checklist (v0.x beta)

The beta release plan. This file is the single source of truth for "what's left before we ship";
AGENTS.md references it as the launch roadmap. Companion docs: API audit →
[docs/API_REVIEW.md](docs/API_REVIEW.md) · SAF migration → [docs/SAF_MIGRATION.md](docs/SAF_MIGRATION.md).

> **Status legend:** ✅ done · 🟡 decide / in progress · 🔴 blocker · ⚪ out of scope for v0.1

## 1. Release shape

- **Target: a public `0.x` beta**, *not* a frozen 1.0. The 1.0 freeze (per `SCENE_DESIGN.md` §1.7)
  waits until Phases A+B+C land and are tested.
- **Stability promise:** **Tier 0 primitives are stable**; the **scene API is "beta, may refine."**
  Say this loudly so early adopters know what's safe to build on.
- **Headline: a drop-in SAF replacement.** Existing SAF play/sync/scene content runs unchanged via
  the shim. Secondary: a better framework for new mods (the scene graph).
- **Engine-only.** No bundled content; OSF is a dependency. Showcase content comes from companion
  mods (Seduce, third-party, ported SAF content).
- **Phase C** (equipment/fade/voice/sound/camera mechanisms + services) is in progress on a separate
  track — it does **not** gate the beta. Ship the action lane as-is (control-lock works; other
  `osf.*` actions are recognized but logged-not-executed).

## 2. Readiness checklist

### Code / features
- ✅ Tier 0 primitives (play/stop/speed/anchor/sync/sequence/state).
- ✅ Scene runtime: handles, navigation, auto-advance, linear stage interface, roles, callbacks,
  exclusivity, load-safe handles.
- ✅ Cue track lane (lifecycle + numeric, trigger-edge auto-take).
- ✅ Action track lane: `osf.control.lock`/`release` + custom `EVENT_ACTION` + cleanup ledger.
- ✅ SAF shim (two scripts: `SAF`, `SAFScript`).
- ✅ Save safety, co-load warning, version gating, post-patch checklist.
- 🟡 **Decide:** how an authored-but-unimplemented `osf.*` action behaves — silent log (current) vs a
  load-time warning so authors aren't surprised. (Recommend a warning.)
- ⚪ Phase C mechanisms (equipment/fade/voice/sound/camera) — fast-follow, not v0.1.

### API quality (see API_REVIEW.md) — *touches shared core; coordinate with the Phase C agent*
- 🔴 Lock the **callback struct field names** (`OSFEvent:SceneEvent`) — frozen with the ABI.
- 🟡 `ValidateScene` / `GetSceneValidationErrors` — spec'd, not implemented; good modder DX.
- 🟡 `HasFeature` — recognize the merged-engine capabilities (`cues`/`actions`/`callbacks`).
- 🟡 Constants ergonomics — `OSF.EVENT_X()` works; consider a non-Native `OSFConst.EVENT_X` property.
- 🟡 Ratify `GetSceneStageForActor`/`SetSceneStageForActor` into the contract (currently undocumented).

### Docs
- ✅ **Doc-drift sweep (2026-06-17)** — reconciled the pre-merge "policy lives in a separate OSF
  Intimacy plugin" framing to the merged reality across README, AGENTS.md, `docs/{API, GETTING_STARTED,
  PACK_SCHEMA, INTIMACY_SEAM, NEXUS_PAGE, guide/index, guide/cookbook}.md`, TESTSUITE.md, and fixed the
  broken bare `DESIGN.md` links (→ `SCENE_DESIGN.md`).
- ✅ `docs/API_REVIEW.md`, `docs/SAF_MIGRATION.md`, `LAUNCH.md`.
- 🟡 `docs/RE.md` + `docs/POST_PATCH_CHECKLIST.md` — still say the equipment/fade/voice/Wwise RE
  "belongs to OSF Intimacy / not used by this repo"; **Phase C owns these** (it's adding those very
  bindings) and should reconcile the framing as it lands the gates.
- 🟡 `docs/INTIMACY_SEAM.md` — title + banner + intro fixed; the body still says "OSF Intimacy" but
  is covered by the redirect banner ("read it as the Layer-B scene runtime"). Full body rewrite deferred.
- 🟡 `SCENE_DESIGN.md` — reconcile the stale callback sentinel table (dispatch-time getters were
  replaced by the struct payload). *(Shared with the Phase C agent.)*

### Packaging
- 🔴 **Distributable archive / FOMOD** — there is none yet (only the MO2 deploy folder). Bundle: the
  DLL, `dist/Scripts/*.pex`, `dist/Scripts/Source/*.psc`, `dist/OSF/*.json` schema, the self-made
  `versionlib-1-16-244-0.bin`, and (optionally) the test pack.
- 🟡 State the dependencies explicitly: **Starfield 1.16.244.0**, matching **SFSE**, **Address
  Library**. CLSF is statically linked (not a user dependency).

### Compatibility & safety (verify one pass each on a real save)
- ✅ Co-load warning for SAF / NAFSF (they're mutually exclusive with OSF).
- ✅ Game-version gating (engine bindings self-disable on mismatch).
- ✅ Save safety (handles die across load; `StopAll` drops state).
- ✅ `docs/POST_PATCH_CHECKLIST.md` for game updates.
- 🟡 **Verify:** loading a save made *with SAF installed* after switching to OSF (clean transition?).

### Testing — *the biggest gap*
- 🟡 Everything so far is the synthetic `OSFTest` harness.
- 🔴 **Run real SAF mods through the shim** — the headline is unproven until this passes.
- 🔴 **Load real community SLAL-format animation packs** + play multi-actor scenes.
- 🟡 Save/load/quickload/cell-change stress; multiple concurrent scenes; varied load orders.

### Distribution & community
- 🟡 LoversLab + Nexus pages (`docs/NEXUS_PAGE.md` exists — refresh for beta).
- 🟡 Courtesy heads-up to SAF (mielu91m) and NAFSF (Deweh) — OSF is a replacement; attribution is in
  the headers (GPL-3.0).
- 🟡 Support channel (issue tracker / Discord).

## 3. Top blockers (the few that actually gate the beta)

1. **Real-world testing** — real SAF mods + real animation packs through the shim. (#1 risk.)
2. **Packaging** — a downloadable archive/FOMOD with the deps stated.
3. ✅ **Doc reconciliation** — pre-merge framing killed (2026-06-17); SAF migration guide shipped.
   (RE.md / POST_PATCH_CHECKLIST.md framing left to the Phase C track.)
4. **Lock the callback struct field names** — the one truly expensive-to-change ABI surface.

Everything else is polish or fast-follow.

## 4. API punch-list for the Phase C agent (shared-core edits)

These improve the beta API but touch `OSFScript.cpp` / `SceneRegistry.*` / `OSF.psc`, which Phase C is
actively editing — so they belong on that track, not a parallel one:

- Add `ValidateScene(asId)` + `GetSceneValidationErrors(asId)` (the parser already produces the data).
- Extend `HasFeature` to answer `cues`/`actions`/`callbacks` (and any Phase-C lane names).
- Add `GetSceneStageForActor`/`SetSceneStageForActor` to `OSF.psc`'s documented contract + `docs/API.md`.
- Decide the unimplemented-`osf.*`-action behavior (warn vs silent).
- Confirm the callback struct fills `actorRef`/`role`/`loopIndex` (or document them as reserved).

## 5. Explicitly out of scope for v0.1

Phase C mechanisms (equipment/fade/voice/sound/camera) · numeric-action polish · the multi-mechanism
ordered undo ledger · `RetargetLiveScene` seamless transitions · per-node role→slot remap · bundled
content · the Papyrus scene builder · scene parameters / auto-drive.
