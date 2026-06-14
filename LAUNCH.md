# Launch Roadmap

*The path from current state to first public release of the **lean OSF Animation core** —
only what gates or shapes the launch. The core is v1 of the three-plugin split (DESIGN.md §8):
ship the content-neutral playback core first; OSF Intimacy (scene engine) and content follow.*

## Decisions locked

- **Three-plugin split executed (the "core carve").** This repo is the lean, content-neutral
  playback core, migrated verbatim from the pre-split repo (now `OSF Animation Archive`). Scene
  policy was carved out to the future OSF Intimacy engine. Fresh-history clean-cut repo — **done**.
- **Seduce assets: permission GRANTED** by the SAF Seduce author. The core ships only the
  baked-in SFW demo entries (`StandSurrender01`/`StandCover01`, covering `solo`/`pair`/
  `test.stages`/`test.loops`); full content lives downstream (OSF Seduce, via OSF Intimacy).
- **CLSF fork: published.** `ozooma10/commonlibsf` branch `forge` (pinned `5df499f`); `.gitmodules`
  points at it → GPL source availability + third-party buildability satisfied.
- **SAF shim is the launch headline:** existing SAF content runs unchanged on the core.

## Phase 0 — Pre-work — DONE

- [x] **Clean-cut core repo** — the carve produced a fresh-history `OSF Animation` (src, dist,
      docs, DESIGN.md, LICENSE + EXCEPTIONS, xmake.lua, .gitmodules). Builds clean from a fresh
      `git clone --recursive` + `xmake` (commonlibsf re-cloned and compiled from the pinned fork).
- [x] **SFW base content** ships as the baked-in test entries (legal to ship as-is).
- [ ] **Verify the stranger-clone build once more** on a clean checkout before tagging.

## Phase 1 — In-game validation round (lean core, never live-verified as a separate build)

**Formal E2E suite with per-case pass signals: `TESTSUITE.md`** — the **GATE** rows are this phase.
The pre-split core validated the migrated engine code; this phase re-confirms it on the lean build.

- [ ] **Boot + readiness** — feature report (`playback hooks INSTALLED`), `GetVersion`,
      `OSFTest.Ready`, NAFSF/SAF co-install warning (BOOT suite).
- [ ] **Solo + scene mechanics** — play/stop/blend, speed, anchor, sequence; 2-actor sync,
      placement, timed + **loop-count** auto-advance, manual stage jump, tag matchmaking (SOLO/SCENE).
- [ ] **SAF shim** — `Ping→IsReady`, `PlaySceneSeparate→StartSceneFiles`, `StopAnimation`,
      `Sync`; and at least one real SAF-dependent mod running unchanged (SHIM suite).
- [ ] **Standalone player locks** — `OSFCompat` control + camera lock, scroll-zoom alive / FP
      bounce, AI-driven flag cleared on load (PLAYER suite).
- [ ] **SaveLoadEvent opType coverage** — quickload, load-by-name, death reload, new game; record
      which sink fires; confirm native re-bind after load (SAVE suite).

## Phase 2 — Pre-release hardening

- [x] **Cross-pack animation-ID collision detection** — PackRegistry: case-insensitive,
      first-load-wins, both packs named.
- [x] **Face-bone skip policy** — `IsFaceRigNode` in Graph.cpp (prefix + token filter, skip count
      logged at bind).
- [x] **Version posture statement** — supported game version **1.16.244**; startup log INFO names
      it every load and WARNs on a mismatch (version-locked bindings self-disable); stated in
      README Requirements and `docs/NEXUS_PAGE.md`.

## Phase 3 — Packaging & distribution

- [x] **Address Library story — DECIDED + DOCUMENTED.** Bundle `versionlib-1-16-244-0.bin` in the
      zip (CommonLibSF loads it from the plugin's own `Data\SFSE\Plugins\` and hard-fails without a
      database for the running build). Documented in README + NEXUS_PAGE. *Remaining: actually
      include the `.bin` when assembling the release archive.*
- [ ] **Tagged release**: semver decision (0.x vs 1.0.0); zip = `OSF Animation.dll` +
      `versionlib-1-16-244-0.bin` (in `SFSE\Plugins\`) + pex (OSF/OSFCompat/OSFTest/SAF/SAFScript) +
      psc + docs + OSFTestPack.json + baked-in SFW GLBs.
- [x] **Player-audience docs pass** — README + docs/ (API / PACK_SCHEMA / GETTING_STARTED / guide)
      curated to the lean content-neutral surface (this doc-cleanup pass). NEXUS_PAGE reframed.
- [ ] **Post-patch checklist** — `docs/POST_PATCH_CHECKLIST.md` curated to the bindings the lean
      core actually uses (the two vtable hooks + save-load event source); carved-feature bindings
      moved to OSF Intimacy's concern.
- [ ] **LoversLab/Nexus page** — `docs/NEXUS_PAGE.md` is paste-ready; add a header image / clip.

## Phase 4 — Launch-window ecosystem moves

- [ ] **Contact Deweh + mielu91m before the public post** (DESIGN.md §9): NAFSF fork-attribution
      courtesy + hand over RE ground truth. Goodwill here decides cooperation vs competition.
- [ ] **`osf-validate` CLI** — biggest adoption accelerant; the parser is already CLSF-free
      (PackRegistry has no engine deps for the mechanical schema). Fine to land the week after launch.
- [ ] **Example-pack tutorial** — annotated OSFTestPack walkthrough on top of PACK_SCHEMA.md.

## After the core ships

- **OSF Intimacy** — the scene-engine plugin that harvests the carved tier from
  `OSF Animation Archive` (ScenePolicy, undress, voice, camera/control, fade, callbacks, cosave
  aftermath) and depends on this core via the C ABI / readiness handshake. Built after the core.
- **OSF Seduce** — content, dependent on OSF Intimacy.

## Explicitly deferred (do not let these delay launch)

Native C ABI (OSF Intimacy needs it, not the core's launch), per-actor 3D audio, Blender/retarget
authoring guide, explicit-anchor option, GraphManager refactor — all post-launch.

## Critical path

Stranger-clone build re-check → lean-core validation session (TESTSUITE GATE rows) → packaging
(include the `.bin`) → coordinated announcement. Phases 2–3 parallelize freely.
