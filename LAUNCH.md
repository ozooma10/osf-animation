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
- ✅ `SCENE_DESIGN.md` / `API.md` / `AGENTS.md` / `INTIMACY_SEAM.md` — Phase-C reconciliation pass
  (2026-06-17): status banners + §2.5 phasing + §1.7 freeze + the action/lane/ledger surface; API.md
  quickstart updated to the `StartScene*`→int-handle returns; INTIMACY_SEAM registry table de-staled.
- 🟡 `docs/RE.md` + `docs/POST_PATCH_CHECKLIST.md` — still say the equipment/fade/voice/Wwise RE
  "belongs to OSF Intimacy / not used by this repo." Phase C landed those bindings IN this repo, so the
  framing needs a pass (the RE *content* is still valid — it's the ownership note that's stale).
- ✅ `SCENE_DESIGN.md` §1.2 — removed the stale `GetCallbackEvent`/`Result`/`Time` dispatch-time
  getters from the sentinel table (the struct-payload transport replaced them); points to the
  `OSFEvent:SceneEvent` fields.
- 🟡 `docs/INTIMACY_SEAM.md` — banner + table fixed; the body still says "OSF Intimacy" (covered by
  the redirect banner). Full body rewrite still optional.

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
- 🟡 Add `OSFConst.pex/.psc` to the package when the Phase C agent lands it (the script picks it up
  automatically; warns if absent).

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
2. ✅ **Packaging** — FOMOD structure + assembly script drafted & verified (2026-06-17). Remaining:
   bump the version to `0.x` and rebuild before the real archive.
3. ✅ **Doc reconciliation** — pre-merge framing killed (2026-06-17); SAF migration guide shipped;
   Phase-C reconciliation pass done (RE.md / POST_PATCH_CHECKLIST.md ownership note still pending).
4. **Lock the callback struct field names** — the one truly expensive-to-change ABI surface.

Everything else is polish or fast-follow.

## 4. API punch-list (remaining beta polish)

Small `OSFScript.cpp` / `SceneRegistry.*` / `OSF.psc` edits that improve the beta API. Phase C is done,
so these are no longer blocked on a shared-core track:

- Add `ValidateScene(asId)` + `GetSceneValidationErrors(asId)` (the parser already produces the data;
  `GetSceneLoadErrors` exists, the per-scene forms don't yet).
- Extend `HasFeature` to answer `cues`/`actions`/`sound`/`camera`/`callbacks`.
- Add `GetSceneStageForActor`/`SetSceneStageForActor` to `OSF.psc`'s documented contract + `docs/API.md`.
- ✅ Unimplemented-`osf.*`-action behavior decided — unknown `osf.*` is rejected at load (loud).
- Confirm/populate the callback struct `actorRef`/`role`/`loopIndex` (currently `actorRef` is always
  `None` — real object marshalling for role/actor-bearing events is a follow-up) or document as reserved.

## 5. Explicitly out of scope for v0.1 (now post-v1 / additive)

`RetargetLiveScene` seamless transitions · per-node role→slot remap · bundled content · the Papyrus
scene builder · scene parameters / auto-drive · free-fly/orbit camera · pool/set→clip metadata
resolution · positioned Wwise · the `equipment.hide` `slots` filter · per-role `equipment.restore`.
(Phase-C mechanisms themselves are **done + in v0.1**, no longer out of scope.)
