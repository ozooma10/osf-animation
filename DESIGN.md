# "SexLab for Starfield" — Design & Roadmap

*Working notes, 2026-06-10. This repo (`OSF Animation`) is the native playback-core plugin for the broader OSF framework. The **three-plugin split of §8 has been executed (the "core carve")**: this repo is now the lean, content-neutral core, migrated verbatim from the pre-split repo (archived as `OSF Animation Archive`); the scene-policy tier was carved out for the future OSF Intimacy engine. Sections 1–7 below are the original full-framework design history that led to that split — read §8 for the current shape, and `AGENTS.md` for the live project status.*

## 1. What SexLab actually is (decomposed)

SexLab (and its modern fork SexLab P+) is not an animation mod — it's middleware with five layers:

| Layer | Skyrim implementation | What it does |
|---|---|---|
| Animation registry | SLAL / SLSB → `.slr` registry files | Packs declare scenes: actor count, stages, tags, offsets |
| Playback | FNIS/Nemesis/Pandora-registered paired idles + `PlayIdle` | Gets animation data playing on actors |
| Scene engine | Papyrus "threads" (up to 15 concurrent) + SKSE plugin | Actor prep (undress, AI lock), anchor positioning, stage progression, sync, cleanup |
| Integrations | SOS/3BA bodies, sounds, effects, camera, alignment hotkeys | Makes scenes look/feel right |
| API | Papyrus API (`StartSex(actors, tags)` etc.) | Lets quest/content mods request scenes declaratively |

Content mods (the actual animation packs and the quest mods that trigger scenes) are the ecosystem on top — the framework is only as valuable as the content it attracts.

## 2. The Starfield mapping — and the one big simplification

Skyrim needed FNIS/Nemesis/Pandora because animations had to be compiled into Havok behavior graphs at build time. **Starfield's NAF/SAF lineage makes that whole layer unnecessary**: they play GLTF/GLB files onto actor skeletons at runtime, bypassing behavior graphs. No behavior engine, no load-time registration, no patch merging. Animations are just files in a folder.

So the Starfield stack is:

```
Content mods (quests, scene triggers, animation packs)
        │  Papyrus API + JSON pack format
┌───────▼────────────────────────────────┐
│  THE FRAMEWORK (what you build)        │
│  • Registry: JSON packs → scene DB     │
│  • Scene engine: threads, positioning, │
│    multi-actor sync, stages, cleanup   │
│  • Actor prep: undress/AI/camera       │
└───────┬────────────────────────────────┘
        │  playback core
┌───────▼────────────────────────────────┐
│  GLTF runtime playback (NAF lineage)   │
│  skeleton-level animation, per actor   │
└───────┬────────────────────────────────┘
     SFSE + CommonLibSF + game
```

## 3. Build-vs-reuse decision (the licensing trap)

Three options for the playback core:

1. **Depend on SAF externally** (mielu91m/StarfieldAnimationFramework-): ⚠️ repo has **no license** and mirrors NAFSF's GPLv3 source tree without attribution. Legally murky — do not build on this code without the author clarifying provenance. Talking to the author is still worthwhile (they're active and maintaining the only working playback core).
2. **Fork Deweh's NAFSF** (github.com/Deweh/NativeAnimationFrameworkSF): GPLv3, clean provenance, contains the proven architecture — the animation/sync logic lives in `GraphManager.cpp` / `GraphUpdateHook`. Your framework becomes GPLv3, which is normal for this scene. **← Recommended.**
3. **Write playback from scratch**: months of skeleton/GLTF/hooking work before any framework code. Only if (2) proves unworkable on current game versions.

Recommendation: **fork NAFSF, attribute properly, keep GPLv3.** Owning the playback layer matters because multi-actor sync (the hardest problem) lives inside the graph update hook — you can't do it cleanly through someone else's console-command API.

## 4. The hard problems, ranked

1. **Multi-actor sync + alignment.** SexLab's core trick: place N actors at a shared anchor with per-scene offsets and keep their animations frame-locked. Do this natively: one scene clock in the DLL drives all participating actors' graphs in the same update hook. Per-animation offset data lives in the pack JSON; runtime adjustment hotkeys write corrections back.
2. **Game-update fragility.** CCF dying is what killed NAF. Mitigate: minimize hooked surface area, use CommonLibSF address scanning, version-gate the DLL, design the Papyrus layer so a broken DLL fails soft (scenes refuse to start rather than CTD).
3. **Content pipeline.** A framework with no animations is dead. Ship from day one: a Blender rig template for the Starfield human skeleton, a pack JSON spec + validator, and ideally a retargeting path from the huge FO4/Skyrim animation libraries (this is the single biggest ecosystem accelerant).
4. **Actor prep edge cases.** Spacesuits/equipment slots, AI packages, companions commenting, save/load mid-scene persistence, scenes in zero-G interiors. Papyrus + Creation Kit territory; tedious but known.
5. **UI/config.** No mature MCM equivalent in Starfield. Start with JSON config + BetterConsole commands; in-game menu later.

## 5. Roadmap

- **Phase 0 — toolchain (1-2 wk):** SFSE + CommonLibSF dev env, CK installed, fork NAFSF and get it *building and playing one GLTF on the player* against the current game patch.
- **Phase 1 — playback parity:** Single-actor playback solid on current patch. *This alone is a releasable, community-valuable mod* (working NAF replacement with clean licensing) and earns credibility/contributors.
- **Phase 2 — scene core:** Anchor + offsets positioning, shared-clock sync. Milestone: a 2-actor paired animation that stays aligned.
- **Phase 3 — registry + API:** JSON pack format (learn from SLSB: actors/stages/tags/offsets), scene thread manager, Papyrus `StartScene(actors, tags)` / `StopScene` / events. Milestone: a separate test ESM triggers a scene through the API only.
- **Phase 4 — framework polish:** undress/redress, AI locks, camera, alignment hotkeys, multi-thread scenes, save-safety.
- **Phase 5 — ecosystem:** Blender template + tutorial, pack validator tool, example pack, FO4/Skyrim retarget guide. Release on LoversLab; recruit the existing SAF content authors (e.g. Gergel Ebanex's author) as first adopters.

## 6. Ecosystem hygiene & persistence (audit vs `Animation-Framework-Notes.md`, 2026-06-11)

A deep-research survey of prior frameworks (SexLab/AAF/OStim/NAF/NAFSF/SAF) confirmed the
architecture here is already aligned on the structural points (native core, thin Papyrus facade,
data-driven JSON, shared-clock sync, fail-soft pack loading, deterministic events). Four items
are worth adopting; the rest of its recommendations were rejected as over-engineered for this
scale (binary registry caches, manifest dependency resolvers, AAF-XML importers, IK-as-core,
role-constraint abstractions — see the notes file for the reasoning).

**Adopt before first public release (cheap now, miserable to retrofit):**

1. **Pack schema versioning.** Add a `"schema"`/version field to the pack JSON; registry warns
   on unknown versions. Must exist before third-party packs do.
2. **Animation-ID collision policy.** OStim's hardest lesson: duplicate IDs across loose files
   overwrite non-deterministically. PackRegistry now loads pack files in filename order, treats
   IDs as global/case-insensitive, logs duplicate IDs loudly, and keeps the first-loaded
   definition. The pack schema recommends `author.pack.anim`-style namespaced ids.
3. **Explicit face-bone policy.** SAF deliberately never writes face bones so the game keeps
   owning expressions. Our stamp writes whatever the binding maps by name — currently safe only
   by accident (NAF-lineage GLBs are bones-only). Make it policy: skip-list/prefix-filter face
   bones in `ResolveAndBind` so a future pack with face data can't fight the engine.

**Adopt at Phase 5 (plan now, build later):**

4. **Native plugin API** — a small versioned C ABI over SFSE plugin messaging
   (PlayScene/StopScene/event listener) so other SFSE plugins can request scenes without
   Papyrus. Keep the GraphManager scene API shaped so this is a thin wrapper, not a rework.

**Persistence (supersedes earlier "no cosave" assumption):** stock Starfield SFSE shipped no
serialization interface, but we have since **added cosave serialization to our commonlibsf/SFSE
fork** — so DLL-side persistence IS possible (as of 2026-06 the interface lives in the fork, not
yet in the `lib/commonlibsf` submodule pointer; sync it before building on it). Design rule
either way (NAF precedent): serialize the *minimum* — scene id, participant FormIDs, stage,
normalized clip time, option flags — and rebuild everything else from pack data on load. The
savegame must never be the authoritative copy of registry or scene metadata. Cross-restart
redress/resume can now live DLL-side instead of waiting for the Papyrus/ESM layer; the ESM layer
remains the home for quest-facing orchestration only.

## 7. Public API redesign — two-tier model (4-consumer audit, 2026-06-13)

The Phase 3 API grew into a verb sprawl (`Play`/`PlayScene`/`PlaySceneAt`/`PlayDefined`/`PlayByTags`)
that encoded three orthogonal axes — **source** (raw path / registry id / tags / dialogue),
**cardinality** (1 vs N actors), and **richness** (bare clip vs full production) — as a differently
named native per point. "Defined" leaked implementation jargon, and a 1-actor `PlayDefined`
silently stripped stages/cues/voice/equipment (solo was a second-class path). This redesign
collapses it. **It must land pre-1.0** — the semver contract (API.md) freezes native
names/signatures at 1.0 and we're at 0.1.0, so this is the only cheap break window. **Zero game
RE** — every item is internal to bindings we already own (no AddressLib IDs, nothing for the
post-patch checklist), so design boldly.

**The evidence: 4 real SAF consumers, two usage models — both real.** RE'd the `.pex` of every SAF
consumer on disk; they split into two camps:

| Consumer | Model | Drives the API as |
|---|---|---|
| Gergel Ebanex, SAF_Seduce, Snu Snu | **primitive** — `Play` in place per actor, self-positioned, then `Sync` | a per-actor playback engine + clock-sync primitive; placement is the consumer's job |
| The Last Nova Backrooms | **atomic** — one `PlaySceneSeparate(a1,a2,f1,f2,speed)` (positions, anchors, refreshes capsules, plays, syncs) + `Ping` + `StopAnimation` | a scene engine that owns placement+anchor+sync wholesale |

So the framework needs **two first-class tiers**, not one tier with the other as thin sugar. Real
consumers use ~15 functions total; none use blend-graph variables, MatchActorTransform,
PlayOnActors, or crosshair helpers — explicitly out of scope.

**The model — one rule:** *one actor, bones move, world unchanged → a **primitive** (`Play`);
anything that coordinates actors (shared clock) or touches game state
(placement/control/undress/fade/voice) → a **scene** (`StartScene*`).* Internally unified: `Graph`
is a pure per-actor sampler/stamper; a single Scene/group object owns 1..N graphs + a shared
`FrameClock` + optional {anchor, policy, stages}, giving one teardown / watchdog / save-safety
path. A solo `Play` is a group-of-one with the optionals empty. The atomic facade calls forward
down to the primitives but each is a deliberate, complete contract (auto-position → anchor →
**refresh physics capsules** → play → sync → policy).

**Primitive layer** (per-graph, policy-free by contract; caller positions):

- `Play(actor, file, blendIn, clip)` — solo, in place; hard contract, never teleports/anchors
- `Stop(actor, fadeOut)` — refuses policy-scene participants (use StopScene)
- `Sync(actors[])` — group ≤4 already-playing graphs on one clock (retroactive merge)
- `SetAnchor(actor, x, y, z, headingDeg, rootMode)` / `ClearAnchor(actor)` — see **docs/ANCHORING.md**
- `SetSpeed(actor, mult)` / `GetSpeed(actor)` — 1.0 = authored
- `GetCurrentAnimation(actor)` — current source path
- `PlaySequence(actor, files[], loops[], blends[], loopWhole)` — solo multi-phase

**Facade layer** (atomic, complete contracts; `ScenePolicy apPolicy = None` ⇒ defaults):

- `StartScene(actors[], id, stage, policy)` — registry; a 1-actor def is now a *full* scene
- `StartSceneByTags(actors[], tags[], policy)` — matchmake, returns id
- `StartSceneFiles(actors[], files[], speed, blend, policy)` — ad-hoc raw files, auto-placement (the proper replacement for PlayScene/PlaySceneAt)
- `ApplyScenePolicy(actor, policy)` — promote a primitive group to a scene (align=false keeps your placement)
- `SetSceneStage(actor, stage)` / `StopScene(actor)`

**Readiness handshake** (first-class — this is how the ecosystem does optional-dependency gating):

- `IsReady()` — loaded + initialized; the gate consumers branch on (a SAF shim's `Ping` forwards here)
- `HasFeature(name)` — effective availability ("scenes"/"undress"/"fade"/"voice"/"sound"/"wwise"/"adjust"); features self-disable on version/binding/audio-init failure
- `GetVersion()` — semver

**ScenePolicy** — Papyrus struct bound as `std::optional<RE::BSScript::structure_wrapper<...>>` so
`None` is the safe default rather than the binding-layer assert (a bare struct arg asserts on None,
exactly like a None array). Fields: `align`, `control`, `fade` (**mechanics** — scene
authoritative) and `undress`, `voice` (**content** — effective = `policy AND user settings.json
gate`; user sovereignty wins).

**Content layer kept** (OSF's machinima/vignette value-add, *not* SAF parity, which is why no SAF
consumer uses it): callbacks, audio/voice, fade, alignment hotkeys, registry, lifecycle, getters.
Renames: `FindAnimations→FindScenes`.

**Decisions of record:**

- **No nested Papyrus `SceneSpec`** — custom scenes compose from primitives + `ApplyScenePolicy`;
  multi-stage authoring stays JSON-only. The SceneSpec→stages→slots tree is the internal + pack
  model only. (Avoids the None-struct assert per nested level; the audit shows nobody hand-builds
  multi-stage in script.)
- **`PlayScene`/`PlaySceneAt` removed**, replaced by `StartSceneFiles` — the old
  auto-anchor-at-actor[0] path was under-specified (no policy/speed/capsule refresh).
- **SAF shim validation:** `SAF.psc` becomes thin forwards — `Ping→IsReady`,
  `PlaySceneSeparate→StartSceneFiles`, `StopAnimation→StopScene`. A clean mapping is the proof the
  surface is right.
- **rootMode** (root-bone-vs-anchor convention — the cross-content alignment risk) is its own
  contract: **docs/ANCHORING.md** (pin / additive / follow).

**Implementation order** (bottom-up; each step builds + tests):

1. Cheap additive, no API break: retain source path per graph (`GetCurrentAnimation`); expose
   `SetSpeed`/`GetSpeed`.
2. Unify `Graph` under one Scene/group object — one registry, flat `actor→Graph*` hot-path cache
   for the ~7×/frame stamp hook, one teardown path. Keep old natives working over the new
   internals so the build stays green.
3. Engine work: `Sync` retroactive clock-merge (mutable group membership under one owner token);
   `rootMode` branch in the stamp hook.
4. Primitive natives, then facade natives + `ScenePolicy` binding + precedence gating +
   capsule-refresh on the atomic path + `IsReady`/`HasFeature`.
5. Migrate consumers in the same commit as the removals: `OSFTest.psc`, Seduce, the `SAF.psc`
   shim; recompile pex.
6. Docs: rewrite `OSF.psc` declarations + API.md surface to match; update the AGENTS.md snapshot.

## 8. Project split — three plugins (repositioning, 2026-06-14)

§7 found the right *seam* (primitive vs scene) but drew it as two tiers inside one DLL. **This
section supersedes that conclusion: the seam becomes a repo/deploy boundary.** OSF is now three
independently-shipped layers, mirroring the SexLab stack of §1:

| Tier | Mod (repo == xmake target == MO2 mod) | Analogue | Owns |
|---|---|---|---|
| Animation core | **OSF Animation** (clean repo, this lineage) | NAF / SAF | playback, sync, shared clock, anchoring, clip registry, alignment, the engine hooks |
| Scene engine | **OSF Intimacy** (new) | SexLab / OStim | actor pairing, role assignment, intimate scene orchestration, staging, policy, undress, voice scheduling, camera/control, fade choreography |
| Content | **OSF Seduce** + others | SLAL packs + quest mods | the actual scenes / animation packs / triggers |

**Why split.** (1) *Launch feasibility* — the core is almost entirely [LIVE]; the scene tier carries
nearly all the still-[BUILT] surface (cosave aftermath, save/load opType coverage, loop-count
advance, Wwise) *and* all the content-coding. Cutting it from v1 leaves a launch surface that is
basically live-verified. (2) *Positioning* — a content-neutral core that machinima / dance / vignette
authors adopt without the intimate-scene baggage is the product ([[engine-content-neutrality]],
[[naf-saf-ecosystem]]). (3) *Independent cadence* — the core can freeze a 1.0 ABI while the scene
engine iterates.

**v1 scope = "replace SAF" (core only).** Natives that ship:
- Primitives: `Play` / `Stop` / `Sync` / `SetAnchor` / `ClearAnchor` / `SetSpeed` / `GetSpeed` /
  `GetCurrentAnimation` / `IsPlaying` / `PlaySequence`.
- Internal **group object** (1..N graphs + shared FrameClock + optional anchor) — what `Sync`
  produces. The §7 unification is *kept*: the *mechanism* is unified internally; only *policy*
  leaves.
- One **content-neutral atomic helper** — auto-position + anchor + capsule-refresh + play + sync,
  with **no `ScenePolicy`**. This is full SAF parity: SAF's `PlaySceneSeparate` positioned / anchored
  / synced and never undressed.
- Clip/pack registry (loads existing SAF / NAF / SLAL packs; the OSF content fields —
  voice/equipment/intensity — are ignored by the core).
- Alignment hotkeys · co-load compat warning · version gating · graph-state save-drop · readiness
  handshake (`IsReady` / `HasFeature` / `GetVersion`).
- **`SAF.psc` shim** — `Ping→IsReady`, `PlaySceneSeparate→atomic helper`, `StopAnimation→Stop`. The
  launch headline: *existing SAF content runs unchanged on OSF Animation.*

**Deferred to OSF Intimacy:** `ScenePolicy`, undress/redress, scheduled voice, camera/control, fade
*choreography*, staging-as-gameplay, scene callbacks as gameplay, stall watchdog, cosave aftermath.
The native *mechanisms* the scene engine needs (fade poster, control layers, undress vfunc) it
implements itself via CLSF — **the core stays the only plugin that *hooks* the engine** (keeps the
game-update-fragile surface in one place).

**Inter-plugin contract.** OSF Intimacy depends on OSF Animation through the small versioned C ABI
over SFSE plugin messaging scoped in §6.4, gated by the readiness handshake. Not built for v1.

**Decisions of record:**
- **"Clean" = migrate, not rewrite.** New OSF Animation repo with fresh history and GPL + NAFSF
  attribution from commit 1, but the RE-verified native core (GLTFImport, Graph, the two
  GraphManager hooks, FrameClock, blending, anchoring, sync) is *copied in*, never reimplemented —
  that code is verified against 1.16.244 and [LIVE]; re-deriving it reintroduces the fragility that
  killed NAF (§4.2). "Clean" comes from what is left behind and a fresh history, not from re-typing
  the engine bindings.
- **Repo mechanics:** because repo == xmake target == MO2 mod, the *current* folder is
  archived/renamed first to free the name; the new lean repo takes "OSF Animation"; the archived repo
  is the source the OSF Intimacy harvest later draws from. OSF Intimacy is built *after* the core
  ships — launch does not touch it.
- **OSF Seduce** stays content, dependent on **OSF Intimacy** (not on the core directly).

**Roadmap:** v1 = core / SAF replacement → OSF Intimacy (scene engine) → content (Seduce + recruited
pack authors). This restores DESIGN §5 **Phase 1** ("a releasable, community-valuable NAF
replacement that earns credibility/contributors") as the MVP, with Phases 2-4 relocated into OSF
Intimacy.

## 9. People to talk to before writing code

- **Deweh** (NAFSF author) — fork blessing, known pitfalls, why CCF coupling happened.
- **mielu91m** (SAF) — what they had to fix for current game versions; possible collaboration instead of competition.
- **LoversLab Starfield section** — announce intent early; this community decides whether a framework wins.

## 10. Key references
- CommonLibSF: https://github.com/libxse/commonlibsf
