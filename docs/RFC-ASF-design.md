# ASF — Adult Sex Framework for Starfield: Design Document

> **Executive summary.** ASF is the sex-specific policy and semantics layer built **on top of** OSF Animation: OSF owns native GLB/`.af` playback, the scene node-graph runtime, anchoring, camera, audio, equipment, matchmaking, registry, and save-safe teardown; ASF adds arousal, sex semantics, actor/relationship state, morphs/fluids, autonomy, UX, and — above all — a small, versioned, mutate-capable **public addon API** that is the actual product. This revision is hardened against an adversarial review of the real OSF contract (`OSF.psc`, `OSFTypes.psc`, `SceneRuntime_Dispatch.cpp`, `InputService.h`, `SceneRegistry.{h,cpp}`, and the unified-schema RFC): every claim that the engine does not yet support has been re-scoped to what the code actually delivers, the morph/facial driver has been demoted from "centerpiece" to "de-risk-before-you-believe-it research spike," and the reversible-effects ledger has been moved fully native so ASF does not reintroduce the SexLab save-corruption it swears off. The thinking was senior-grade; the risk accounting now matches it.

---

## 1. Comparative Scorecard

Ratings are 1–5 (5 = best-in-class). "Game" engines differ, so this is about *design*, not raw capability.

| Dimension | SexLab (Skyrim) | AAF (Fallout 4) | OStim NG (Skyrim) | What the leader proves |
|---|---|---|---|---|
| **Scene model** | 2 — flat fixed stages 1..N, no in-scene steering | 3 — position-trees/animation-groups, but hand-authored XML state machines | **5** — real-time navigable node graph w/ BFS multi-hop routing, cross-pack `origin` links | OStim: a *navigable graph* is what makes a framework feel next-gen, not a player. |
| **Alignment** | 2 — "alignment hell": 4 multiplied coord sources, per-mesh JSON, hotkey realign | 2 — per-actor x/y/a baked in XML, nudge subsystem because auto-align is unreliable | **5** — proportion-keyed `ActorKey{sex,height,heels}` → one tuned offset reused across whole population, persisted, live-editable | OStim: key alignment by *body proportions*, not actor identity. The single most-copied subsystem. |
| **Arousal / gameplay** | 3 — native enjoyment calc, Separate Orgasms (per-actor) via SLSO | 1 — schema *defines* stats/reactions but ships **none**; pure mechanism | **4** — per-actor excitement w/ speed-coupled inc, time-until-climax, auto speed control | SexLab/SLSO: **per-actor independent climax**, not a shared scene timer. AAF: shipping no policy fragments the ecosystem. |
| **Addon API / ecosystem** | **5** — 3 composable surfaces (global vtable hooks, string-hook mod events, per-actor/faction tracking); durable `tid` handle | 3 — `StartScene` + tag filters + custom events, but no in-scene mutation hooks | **5** — Papyrus + C++ vtable ABI + InterfaceMap + faction-as-state-broadcast + annotation/event bus | SexLab+OStim tie: observe *and mutate* any scene you didn't start; a stable handle that survives the event boundary. |
| **Data-driven authoring** | 3 — JSON or Papyrus-callback registration; free-form tags | **4** — entire content layer is XSD-validated XML, `<defaults>` inheritance, `loadPriority`, non-destructive `tagData` re-tagging | 4 — recursively-loaded JSON + perks-as-conditions; filename-as-id collisions | AAF: grammar separated from content; the engine ships small, the ecosystem ships the *what*. |
| **Performance** | 1 — Papyrus polling loops, 128-slot tag scans, multi-minute install cache | 2 — heavy XML boot, strict parser fails whole UI on one bad file | **4** — C++ core, scene DB built DLL-side, thin Papyrus surface | OStim/NAF: native core, thin script. SexLab's save bloat + register time is the cautionary tale. |
| **Creature support** | **5** — native race-key abstraction, multi-race scenes, MNC backbone | 3 — raceData→skeleton families, requiresAnimationReset, but content-dependent | 2 — historically weak; a cited reason people *stay* on SexLab | SexLab+MNC: race-key abstraction is the foundation. OStim's biggest hole. |
| **Immersion / pacing** | 2 — discrete stages, manual advance | 3 — sequential stage groups (foreplay→main→climax) | **5** — graph *is* the pacing system; auto speed control intensifies toward climax | OStim: pacing emerges from arousal-gated graph transitions + aftercare nodes. |
| **Undress / morphs** | 3 — per-slot strip%, Strip Editor; visible mid-anim strip glitch | **4** — LooksMenu BodyGen morphs + skin overlays (cum/wetness w/ duration+quantity) + MFG + layered/protected equip + doppelganger | 4 — native+Papyrus undress w/ veto path, 4-layer expression compositing, equip-object system (strapon/tongue/cum variants) | AAF: richest body/overlay layer. OStim: composable 4-layer faces. SexLab: mask transitions with fade. |
| **UX / MCM** | 2 — sprawling MCM, 13 hotkeys, per-anim enable pages: overwhelming | **4** — the "Doctor": numbered ~93-code error catalogue w/ remediation, manifest/perf tabs, graceful degradation | 3 — in-scene radial nav UI + Align menu, but addon-MCM sprawl | AAF: world-class *diagnostics*. WickedWhims (cross-ref): grouped attributes + master toggles + SFW/NSFW split. |

### What each teaches us

**SexLab** teaches that the *API is the moat*. Its dominance came not from being first but from three composable extension surfaces and an open animation registry that turned content into drop-in data — combined with a durable integer handle that survives the mod-event boundary. It also teaches the full catalogue of what *not* to do: Papyrus save bloat, hard slot caps (128 anims / 15 threads / 5 actors / 64 hooks), multi-minute registration, and "alignment hell" born of having no single source of truth for body placement.

**AAF** teaches the power and the peril of *pure mechanism*. Separating an XSD grammar from content produced a huge interoperable ecosystem and the best diagnostics in the genre (the "Doctor"). But shipping **zero policy** — no arousal, no relationships, no "what happens and when" — fragmented the gameplay layer into uncoordinated mods each reinventing arousal and fighting over the same scenes. Its strict XML parser, which rejected a whole UI over one malformed community file, is the canonical argument for lenient, per-file-isolated parsing.

**OStim NG** teaches the *next-gen feel*: a navigable node graph, proportion-keyed alignment, code-driven positioning (decoupling animation from placement), and the faction-rank state broadcast that exposes live scene state to the vanilla condition system. Its weaknesses — combinatorial API sprawl (~300 mechanical Papyrus query permutations), Papyrus in the hot path, weak creature support, filename-collision IDs, and an API version welded to the DLL version — are precise warnings for our own API and schema design.

The meta-lesson: **a small stable native core + a rich, well-versioned addon API + content-as-data**, with a systems layer that runs even without explicit content (the WickedWhims/WonderfulWhims and OBody-NG decoupling lesson). That is exactly the shape OSF already has, and exactly what ASF must extend — *without* importing any predecessor's idiom that the OSF/Starfield engine cannot actually honor.

---

## 2. Design Principles

1. **Native-first, thin script surface.** Every hot path — arousal integration, scene selection, expression/morph driving, fluid state, *and the re-broadcast of OSF events into the ASF vocabulary* — lives in C++ (SFSE), mirroring OStim's DLL-side scene DB and OSF's existing native core. Papyrus is for *policy glue and addon convenience*, never per-frame work and never per-event marshalling. This is a direct rejection of SexLab's `RegisterForSingleUpdate(0.5)` polling.

2. **Data-driven authoring with zero-scripting packs.** A content author ships JSON + clips and nothing else — no ESP, no Papyrus, no registration call. ASF's adult schema is *sugar that compiles down to OSF scenes*, so authors never see node graphs unless they want to.

3. **The addon API is the product.** A small, hand-curated, semantically-meaningful, **versioned** surface (Section 6) — never OStim's auto-generated permutations. The public verbs must match the runtime ASF actually executes (graph navigation, not the dead linear stage index), and the canonical event must be a **superset** of OSF's `SceneEvent`, not a lossy re-wrap.

4. **Clean OSF integration seam.** ASF never reimplements playback, sync, anchoring, or sequencing (Section 3). Where OSF lacks a capability ASF needs, ASF owns it in its own SFSE plugin with its own ledger, and *contributes the generalizable part upstream* — but never forks the runtime.

5. **Modular, optional gameplay layers.** Arousal, relationships, fertility, autonomy, fluids are each a **separately toggleable module with sane defaults and a master switch**, useful and *review-safe* with explicit content disabled.

6. **No save bloat, and reversible state self-heals natively.** ASF persists only durable facts (relationships, arousal baselines, fertility cycles, settings, *and enough applied-effect debt to reconcile on load*) as native co-save blobs — never per-scene script state. **Critically, ASF's reversible-effect ledger is native and hooks the same save-load teardown OSF uses; it does not depend on a Papyrus `SceneEvent` reaching the VM** (the single most dangerous repeat — see §3 and §4.6).

7. **Graceful degradation, per-file isolation.** A malformed pack is skipped with a precise, machine-greppable error (AAF's "Doctor"), never a global failure. A missing optional dependency disables one feature, not the framework.

8. **Capability-driven, skeleton-agnostic actor model.** Roles are bound by *capabilities and tags*, never gender-hardcoded — non-negotiable for Starfield's aliens/robots, and OStim's biggest gap.

9. **Believe nothing the engine hasn't demonstrated.** Every cross-game idiom imported from a predecessor (faction-broadcast, proportion-keyed alignment, content importers, in-scene player steering) is treated as an **unproven assumption gated behind a spike**, not a shipped feature. The roadmap funds the spikes before the features depend on them.

---

## 3. The OSF Integration Seam

OSF's own RFC anticipates "a thin **adult** wrapper [that] will likely ship alongside OSF core." ASF *is* that wrapper, grown up. The seam is sharp — and the rest of this section is grounded in what `SceneRuntime_Dispatch.cpp`, `SceneRegistry.{h,cpp}`, and `OSFTypes.psc` actually do, not in what the idiom suggests.

### ASF delegates to OSF (never reimplements)

| Capability | OSF surface ASF calls |
|---|---|
| All per-frame playback, multi-actor frame-locked sync, the shared clock | `Animation::Scene` / `GraphManager` — opaque; ASF never touches |
| Actor anchoring & code-driven positioning | `ScenePlan` / `ParticipantPlacement`, `rootMode` pin/additive/follow |
| Scene graph: nodes, edges, navigation, four track lanes (cue/action/sound/camera), timed marks | `StartScene*`, `AdvanceScene`, `NavigateScene`, `GetSceneEdge*` |
| Content-neutral mechanisms (ledger-reversed): control lock, camera hold/states, fade, equipment strip/restore, weapon sheathe, audio | `osf.*` built-in actions on the `action` track |
| Selection by tag boolean query + role/gender/keyword/race filters | `StartSceneByTagsQuery(allOf, anyOf, noneOf)` |
| Registry + hot reload + per-file load-error reporting | `ReloadPacks()` (→ `Reload()` post-RFC) |
| Save-safety teardown + the per-handle undo ledger | automatic on world-replacing load; `OnStopAll` |
| Scene lifecycle events → Papyrus | `RegisterSceneCallback` + `SceneEvent` (`EVENT_SCENE_BEGIN/NODE_ENTER/EXIT/CUE/ACTION/SCENE_END`) |

**Critical:** ASF starts scenes **by tag query, never by hardcoded id**, and reacts via `RegisterSceneCallback`. This insulates ASF from OSF's mid-flight schema migration: the RFC guarantees the `Start*ByTags*` + `EVENT_*` surface is stable across that migration. ASF binds to *that* surface and treats registry internals as opaque.

### The custom-verb hook as it ACTUALLY exists (the defining constraint)

This is the boundary the v1 draft half-knew and mis-scoped. In `RunAction` (`SceneRuntime_Dispatch.cpp:326–333`), any non-`osf.` action type falls straight through to `DispatchAction`, which builds a `SceneEvent` and relays it. The payload an addon receives is **only** `{actionType, node, role, anchor, actor}` (`DispatchAction`, lines 173–191). `ActionEntry`'s `hold`/`duration`/`set` fields are hard-wired to `osf.*` mechanisms and are **not** carried for custom verbs; and `ParseActionTrack` (`SceneRegistry.cpp`) **throws** if a custom action declares `required:true`. So `asf.*` verbs are not merely fire-and-forget — they are *statically forbidden from ever being load-bearing, blocking, acknowledged, or argument-carrying.*

**Consequence, taken seriously:** stage semantics cannot ride the verb. `asf.orgasm` can tell you *what beat fired on which role at which node* — nothing more. "How much," "internal vs external," "which fluid set" cannot be encoded. Therefore the verb is a **trigger index only**, and ASF maintains *all* richer data in its own native engine, keyed by `(sceneHandle, role)`. The `asf.*` taxonomy is a **closed enum of beats** (`asf.orgasm`, `asf.penetrate.begin`, `asf.penetrate.end`, `asf.stage.<x>`); everything else resolves from native state. **ASF does not push the proposed opaque-`data` string upstream as an MVP dependency** — the MVP is designed to work without it, and it stays a nice-to-have.

> **Decision kept (with rationale):** ASF still uses the `asf.*` action track as its beat spine rather than polling. *Rationale:* it is the one zero-cost, perfectly-timed, in-graph signal OSF already emits; the fix is to stop treating it as a data channel, not to abandon it.

### ASF owns (the adult layer)

- **Policy / "when & why"** — arousal-driven progression, consent gating, partner selection, scene-start triggers. The decision to start a scene is always ASF's.
- **Adult schema sugar** — an SL/AAF-style linear "act list with sex tags" that compiles to OSF graph scenes + the ASF tag taxonomy (§5).
- **Sex semantics via the custom-verb hook** — ASF emits the closed `asf.*` beat enum; OSF forwards each verbatim as `EVENT_ACTION` and never interprets it.
- **All ASF state + its own native, save-safe reversible ledger** — arousal, relationships, consent, fertility, **and applied-but-unreleased body effects that must self-heal on load.**
- **Selection/filtering UI, HUD, diagnostics, and player config** — OSF ships no UI.

### Gaps ASF must fill (from the OSF capability report)

| Gap | OSF status | ASF response |
|---|---|---|
| Arousal / orgasm / sex-stage semantics | absent | ASF native arousal engine; encode beats via the closed `asf.*` enum |
| Relationship / consent | absent; matchmaking is gender/keyword/race only | ASF gate logic *before* `StartScene*`; native relationship store |
| **Expression / morph / facial** | **absent — body rig only; an unscoped Starfield RE research project** | ASF native morph driver, **gated behind a Phase-0.5 spike** (§4.6, §8) |
| **Equip-object meshes** (strapon/tongue/cum-mesh attach-to-bone) | **absent — `osf.equipment.*` strips, it does not attach animated props** | first-class ASF mechanism, scheduled **ahead of** facial morphs (higher demand, lower RE risk) |
| Fluids / overlays / wetness / cum | absent | ASF-owned, driven off `EVENT_ACTION`, **native reversible ledger** |
| Fertility / pregnancy | absent | fully ASF, optional, off by default |
| NPC autonomy / scene-seeking | absent (OSF is reactive only) | ASF scheduler + AI-package layer, **with a concurrency governor** |
| Scene-selection UI + discovery natives | `FindScenes`/`GetSceneRoles`/`GetSceneTags` documented but not shipped | ASF maintains its own native scene index; contribute discovery natives upstream |
| **Reversible custom mechanisms** | **absent — `asf.*` verbs notify-only, no payload, not ledger-tracked, throw on `required`** | ASF's own native plugin + native ledger (below); not the Papyrus hook |
| Actor **scale / height normalization** for alignment | placement is world-point + heading; **no scale term surfaced** | ASF must verify placement composes with scale, or own normalization, before claiming the alignment win |
| Positioned audio | listener-centered fallback at full volume (`PlaySound`, lines 99–101) | **autonomy is gated on this** — ASF distance-attenuates before `PlaySound`, or autonomy waits for upstream positioned posting |
| Interactive director control | `InputService` is a UI-vtable verb-dispatch channel (`SetVerbHandler`, `Engage/Release`); **no `kReposition`/`kFreecam`** | enumerate the exact verb set ASF needs (below) as an explicit Phase-1 dependency; do not assume freecam |
| Reload coherency | `Reload()` replaces the registry wholesale | ASF re-derives its index on the same reload signal; **request a reload event upstream** (OSF emits none today) |

### The reversible-ledger boundary (the project's defining architectural decision)

OSF's ledger reverses mechanisms in order, once, idempotently, **on every termination path including the hard save-load teardown** (`SaveSafety` / `OnStopAll`). ASF's reversible effects (morphs, fluids, equip-objects) **must replicate that discipline natively** — and the trap is precise: if ASF's revert fired from a Papyrus `SceneEvent`, it would arrive *after* the native event, asynchronously, and **would never fire on the hard-teardown path OSF uses to drop scenes on load**. Save mid-scene, reload, and the morph/fluid debt is recorded against a scene handle that no longer exists, with no event to revert it → a permanently puffy belly or permanent overlay across reload. That is the exact SexLab save-corruption class, reintroduced through the body layer.

**Therefore:** ASF's reversible ledger lives in `asf.dll`, hooks the **same save-load teardown signal OSF uses** (`OnStopAll`/`SaveSafety` pattern), and persists enough debt in the co-save to **reconcile orphaned effects on load** — on game load ASF scans for "applied but unreleased" body state and forcibly reverts it, because the scene that owned it is gone. "Self-heal orphaned body state on load" is an explicit, tested Phase-2 acceptance criterion, not a hope.

---

## 4. Architecture / Module Breakdown

ASF is one SFSE plugin (`asf.dll`) exposing native + Papyrus APIs, plus a content schema and a thin UI. For each module: **responsibility · key data · OSF hook · prior-art lesson.**

### 4.1 Arousal / Excitement Engine (core, non-optional)
- **Responsibility:** per-actor arousal integration, climax detection, drive scene intensity.
- **Key data:** per-actor `{baseline, current, libido, maxExcitement, decayCooldown}`; per-stage/per-action `stimulation` + `maxStimulation`; per-actor `timesClimaxed`.
- **Model:** Baseline / Current (time-integrated toward baseline) / Libido, nudged by *events* (slaps, vibration, orgasm) and *exposure* (armor-keyword modesty classes), à la the Aroused family. Aggregate stimulation with OStim's rule — **highest single contribution + 10% of every other** — so kissing adds arousal but never alone forces climax. Climax is **per-actor and independent** (SLSO's defining win), never a shared scene timer.
- **OSF hook:** reads scene/stage tags; calls `SetSpeed` to make the animation visibly intensify toward climax; emits `asf.orgasm` (beat only) per actor.
- **Lesson:** SLSO — per-actor enjoyment with decay-toward-baseline beats a single timer. OStim — speed-coupled excitement.

### 4.2 Scene / Act Semantics Layer
- **Responsibility:** impose **foreplay → escalation → climax → aftercare** meaning on OSF's content-neutral graph; map ASF "acts" to OSF nodes; gate transitions on arousal.
- **Key data:** stage taxonomy (`foreplay|escalation|main|climax|aftercare`); per-node act-type + intensity tags; arousal thresholds per edge.
- **OSF hook:** authored as OSF **graph** edges; ASF calls `NavigateScene` when an arousal threshold is crossed. **Aftercare is a first-class terminal node**, never an abrupt `$end`. *Because ASF compiles to graphs, the public API navigates edges — it never exposes a linear stage index (see §6 D-fix).*
- **Lesson:** OStim — the graph *is* the pacing system; aftercare is a real node. AAF — sequential stage groups prove ordering matters.

### 4.3 Actor Model (core)
- **Responsibility:** per-actor sex-relevant state: roles, orientation, genital capabilities, consent/aggression.
- **Key data:** `{role, orientation, capabilities[], consentState, aggressionState}`; capabilities (`canPenetrate`, `canReceive`, `hasStrapon`, `oral`, `skeletonClass`) drive slotting.
- **Decision — capability-driven, never gender-hardcoded.** Futa/strapon is a *capability*, not a gender. Slotting is validated at select-time from capabilities, so SexLab's "Forcibly Sort Positions" band-aid is unnecessary. **Skeleton-agnostic from day one** for aliens/robots.
- **OSF hook:** ASF resolves roles, then binds via `StartSceneRoles(actors, sceneId, roles[])`. **Binding is positional** (`ResolveRoleActor`, `SceneRuntime_Dispatch.cpp:206` — `roles[i] ↔ participants[i]` by declaration order). ASF's capability→role resolution must therefore emit the role-name array `StartSceneRoles` consumes *in exactly that order*, and a round-trip test asserts `asf.orgasm role:receiver` resolves to the intended actor. This indirection (capabilities → role names → positional index) has three desync points; the test closes them.
- **Lesson:** OStim's stable actor/target/performer triad; SexLab's decoupled gender + futa axis.

### 4.4 Relationship / Romance Layer (optional)
- **Responsibility:** affinity, traits, jealousy, partner history.
- **Key data:** pairwise affinity store; traits; per-pair act counters.
- **OSF hook:** a *pre-start gate* + a `SceneEvent`-driven *writer*; OSF never sees it. Counters stored **natively** (never faction rank, which saturates ~101 and leaks `-2`). Optional faction mirror only if §4.13 D-/F-spikes prove a reader exists.
- **Lesson:** OStim ORomance + WickedWhims. The faction-saturation bug says: keep the *truth* native.

### 4.5 Fertility / Pregnancy / Cycle (optional, off by default)
- **Responsibility:** cycle sim, contraception, conception, pregnancy progression.
- **Key data:** per-actor cycle phase, fertility window, contraception state, pregnancy timer.
- **OSF hook:** reacts to the `asf.orgasm` beat; drives belly morphs through Module 4.6's native ledger.
- **Lesson:** WonderfulWhims — ship a *real cycle model* as one toggleable module rather than fragmenting it. Off by default for review-safety.

### 4.6 Body Layer: Equip-Objects, Morphs/Expressions, Fluids/Overlays (the largest, riskiest build)
Split deliberately into three sub-systems by **demand × RE-risk**, scheduled in that order:

- **(a) Equip-object attach/detach (highest demand, lowest risk — built first).** The load-bearing visual for any scene where a no-schlong actor penetrates (strapon), plus tongues and cum-meshes. OSF has **no** mechanism for "equip this mesh on this bone and drive it" — `osf.equipment.*` only strips. ASF owns attach/detach as a **first-class reversible mechanism with its own native ledger entry**, and it is the *first* reverter validated against the §3 self-heal path.
- **(b) Facial morph / expression driver (highest risk — a research project, gated by a spike).** Starfield's facegen is **not** LooksMenu/MFG and has **no community-proven, RE'd morph-driver analog**; OStim's 4-layer compositing model stood on a decade of mature Skyrim facegen RE that does not exist here. ASF imports the *model* (4-layer: underlying / looking-override / event / action-override, per-modifier `baseValue/variance/speed/excitementMultiplier/delay`) **only after** proving one reliable layer. See the **Phase-0.5 spike** (§8): set and restore a single facial morph on a live actor, surviving save/load and a cell change. *If the spike fails, expression compositing is vaporware and ASF ships arousal-driven **body** response only* (which OSF's rig already supports), and says so plainly.
- **(c) Fluids / wetness / cum overlays.** Overlay sets with `duration` + `quantity` (AAF's model), driven off the `asf.*` beats. Reversible → **native ledger**, self-healing on load.
- **OSF hook:** `EVENT_ACTION` beats trigger; **all revert state is native** (never the notify-only hook).
- **Lesson:** AAF's overlays + OStim's composable faces and equip-object variants — adopted with eyes open about the missing Starfield RE foundation.

### 4.7 Undress Policy Engine (core)
- **Responsibility:** which slots strip, partial vs full, gradual vs instant, redress-on-end; veto path for protected items.
- **Key data:** policy `{slots, partial|full, gradual|instant, redress}`; per-actor protected-keyword set.
- **OSF hook:** OSF's `osf.equipment.hide/restore` handles the *mechanism* (snapshot + base-skin-aware restore). ASF owns *policy* + a veto callback (so Devious-Devices-style mods keep items). AAF's None/Partial/Full/Compat presets become ASF policy presets.
- **Lesson:** AAF — undress is policy (Compat presets strip *fewer* slots to protect modded-body slots). SexLab — mask mid-anim strip behind `osf.fade.out`.

### 4.8 Furniture / Positioning + Alignment (delegated, thin — but scale-aware)
- **Responsibility:** match scenes to furniture/ship props; live alignment correction.
- **Key data:** furniture-type hierarchy (bench⊂chair); per-actor alignment offsets.
- **OSF hook:** OSF owns positioning math. ASF contributes **proportion-keyed alignment storage** — `ActorKey{sex,height,heels}` → offset, persisted, live-editable — **but only after verifying OSF placement composes with actor scale** (placement is documented as world-point + heading, with no scale term surfaced). `ActorKey` derivation is a concrete, tested function over real Starfield actor data, not a schema field. If placement ignores scale, ASF adds normalization *before* claiming the OStim alignment win.
- **Lesson:** OStim — key by proportions, persist, edit live. SexLab — *don't* multiply 4 coordinate sources.

### 4.9 Scene Selection / Filtering + Tag Taxonomy (core)
- **Responsibility:** translate ASF intent into an OSF tag query; own the adult tag vocabulary; **enforce content flags as a hard native gate** (§5).
- **Key data:** namespaced taxonomy (§5); a native scene index built from ASF's compiled packs, with flags resolved at compile time.
- **OSF hook:** `StartSceneByTagsQuery(allOf, anyOf, noneOf)`; OSF does priority-tier + weighted-random selection and deterministic role binding.
- **Lesson:** SexLab tag taxonomy as lingua franca; AAF Themes as data-layer exclusion. **Avoid OStim's ~300-permutation API — one tag-query DSL collapses it.**

### 4.10 NPC Autonomy / Ambient Scenes (optional — with a governor)
- **Responsibility:** NPCs autonomously initiate scenes by relationship/location/personality; location permission.
- **Key data:** autonomy scheduler; per-cell/ship/zone "intimacy allowed here" flag; **a configurable concurrency budget.**
- **OSF hook:** scheduler decides → `StartSceneByTagsQuery` for NPC-only scenes.
- **Hardened constraints:** "no caps" means "no *arbitrary* caps," **not "no governor."** A configurable **concurrency budget** (max simultaneous NPC scenes, distance/cell gating) is a Phase-3 requirement — unbounded autonomy is a settlement-wide framerate sink (WickedWhims added exactly this throttle). Autonomy is **also gated on positioned audio** (§3): without distance attenuation, every NPC moan plays at full volume at the player — ship-blocking, not "tolerate."
- **Lesson:** WickedWhims — autonomous initiation + location signs + autonomy throttles.

### 4.11 Sound / Voice (delegated, distance-aware for autonomy)
- **Responsibility:** moans/voice keyed to actor identity + stage beats.
- **OSF hook:** OSF's `SoundService` via the `sound` track or `osf.voice.play`. Today `PlaySound` falls back to listener-centered full volume. **For autonomy, ASF distance-attenuates before calling `PlaySound`,** or autonomy waits for upstream positioned posting.
- **Lesson:** BG3SX — companion-voice-matched audio. OStim OSound.

### 4.12 UI / HUD + Settings (core)
- **Responsibility:** scene-selection wizard, in-scene nav HUD, arousal meter, diagnostics panel, grouped settings.
- **In-scene control reality:** drives OSF via the **`InputService` verb channel**, which is verb-dispatch only — there is **no reposition/freecam primitive**. ASF enumerates the exact verbs it needs (below) and tracks "OSF exposes these" as a Phase-1 dependency.
- **Lesson:** AAF's "Doctor" diagnostics; WickedWhims grouped settings + master toggles + SFW/NSFW split. Reject SexLab's flat MCM.

### 4.13 The Public Addon API (the product — §6)
- **Responsibility:** let third parties observe, mutate, and extend scenes without forking ASF.
- **OSF hook:** **natively** re-broadcasts OSF's `SceneEvent` into a stable ASF vocabulary (no Papyrus-side rebroadcast loop — `CreateStruct("OSFTypes#SceneEvent")` per node/cue would double VM allocation under autonomy), preserving *all* native fields and adding arousal/relationship/orgasm events.
- **Lesson:** SexLab's 3 surfaces + durable handle; OStim's listener ABI; the Sensual-Scenes-Adapter lesson — one canonical, *superset* event vocabulary from day one.

---

## 5. Data Model & Authoring

### Authoring goal: zero-scripting, AAF/SexLab-simple, compiles to OSF graphs

One `*.asf.json` per scene-set; ASF's loader compiles it to OSF `*.osf.json` graph scenes. Authors never write node graphs unless they opt into advanced branching.

```jsonc
// MINIMAL adult scene — a linear act list with sex tags (SexLab/AAF ergonomics)
{
  "schema": 1,
  "id": "leito.cowgirl.mf",
  "tags": ["act:vaginal", "pos:cowgirl", "intensity:loving", "actors:2"],
  "roles": [
    { "name": "giver",    "capabilities": ["canPenetrate"] },
    { "name": "receiver", "capabilities": ["canReceive"] }
  ],
  "acts": [                                   // compiles to OSF graph nodes + arousal-gated edges
    { "stage": "foreplay",   "clip": "Leito/cowgirl_fore.glb", "stim": { "receiver": 8 } },
    { "stage": "escalation", "clip": "Leito/cowgirl_main.glb", "stim": { "receiver": 22, "giver": 18 } },
    { "stage": "climax",     "clip": "Leito/cowgirl_peak.glb", "onEnter": ["asf.orgasm"] },
    { "stage": "aftercare",  "clip": "Leito/cowgirl_cuddle.glb", "loops": 0 }
  ]
}
```

ASF lowers this to: OSF roles (capabilities → OSF keyword filters + ASF capability index), one OSF **graph node** per act, arousal-gated `NavigateScene` edges between stages, and the closed `asf.*` beat enum on the `action` track. The `stim` map feeds the arousal engine; OSF never sees it. Note the `onEnter` beat carries **no payload** — internal/external, quantity, and fluid set are resolved from ASF native state keyed by `(sceneHandle, role)` (§3).

### Proposed tag taxonomy (namespaced)

Namespaced prefixes prevent SexLab's typo-driven silent filter failures and OStim's filename-collision IDs. ASF validates tags against a published vocabulary at load and reports unknown tags **per-file** (never globally fatal).

| Namespace | Examples | Purpose |
|---|---|---|
| `act:` | `act:vaginal` `act:anal` `act:oral` `act:handjob` `act:kissing` | the sex act |
| `pos:` | `pos:cowgirl` `pos:missionary` `pos:doggy` `pos:standing` | position |
| `role:` | `role:dom` `role:sub` `role:giver` `role:receiver` | role semantics (position-independent) |
| `intensity:` | `intensity:gentle` `intensity:rough` `intensity:loving` | pacing/mood |
| `body:` | `body:breast` `body:feet` `body:throat` | body-region focus |
| `furn:` | `furn:bed` `furn:chair` `furn:none` `furn:wall` | furniture requirement |
| `actors:` | `actors:2` `actors:3` `actors:solo` | participant count (fast pre-filter) |
| `cap:` | `cap:strapon` `cap:futa` `cap:creature` `cap:robot` | required capabilities (skeleton-agnostic) |
| `flag:` | `flag:consensual` `flag:aggressive` `flag:lore` | **content flags — enforced as a hard native gate** |

**Content flags are a hard gate, not a filter.** A runtime tag-filter is advisory and leaks — and the leak here is "content the user explicitly disabled appears anyway," the single worst trust failure for an adult framework. ASF resolves every scene's flags **at compile time** and the native `StartSex` path **refuses to start a flag-disabled scene regardless of entry path** (id, tag, or addon call), logging a machine-greppable refusal. AAF's "exclude by not installing" is thereby made a true runtime toggle with load-layer enforcement, not a soft query hint.

### Content compatibility / migration — documentation, not an importer

Predecessor "importers" mostly don't materialize, because **the animations themselves don't port** (different skeletons/engines); a metadata-only importer fabricates scenes that reference clips which do not exist — exactly the dangling-`demo` drift the OSF RFC §9 fights. **ASF therefore ships no automated importer.** Instead it publishes a **tag-mapping table as documentation** for human porters (SexLab `Vaginal`→`act:vaginal`; AAF `Stim`/`Dom` codes → `intensity:*`/`role:*`; OStim actor/target/performer → `role:`/capabilities) and an AAF-style **tag-override file** so a third party re-tags another author's *real* pack without editing it. The saved effort funds the equip-object and scale-normalization work. Loading remains lenient, schema-validated, **per-file isolated** (skip the bad file, report code + remediation).

---

## 6. The Addon API as the Product

Content is data, so the *only* thing that makes ASF extensible is this surface. It is a hand-curated, semantic, versioned API whose verbs match the runtime ASF actually executes.

### Surface: three tiers (SexLab's proven shape, OStim's ABI discipline)

**Tier 1 — Papyrus (convenience, policy glue).** One tag-query DSL, not OStim's ~300 mechanical permutations:
- **Start:** `StartSex(actors, tagQuery, opts)` → durable `int handle` (survives the mod-event boundary). `opts` carries an **originator tag** (below).
- **Observe:** `RegisterForASFEvent(eventName, cb)` over the canonical vocabulary.
- **Navigate (not "set stage"):** `GetSceneStages(handle)` → named ASF stages; `GoToStage(handle, "climax")` resolves internally to a `NavigateScene` edge. **There is no `SetStage(handle, int)`** — `GetSceneStage` returns `-1` on a genuine graph and ASF *compiles to graphs*, so a linear stage index would silently no-op on exactly the scenes ASF authors. The public verb matches the graph runtime.
- **Mutate:** `ModArousal(actor, v)`, `ForceOrgasm(actor)`, `RequestEndScene(handle)` (cooperative) and `EndScene(handle)` (force) — both record who ended it.
- **Query:** `GetArousal(actor)`, `GetActorsInScene(handle)`, `FindActorScene(actor)`, `GetRelationship(a,b)`.

**Tier 2 — C++ native ABI (for other SFSE plugins).** A versioned vtable fetched via `GetProcAddress(asf.dll, "ASF_RequestAPI")`, POD structs + `const char*` for ABI stability. An `IASFListener` with `OnSceneStart/StageChange/Orgasm/End(SceneView*)`. **Reversible-mechanism registration is sandboxed:** addons register *pure-data* reverter intents (mechanism id + actor + POD params), **not opaque closures.** ASF stores the intent in its own native ledger and invokes the registered reverter **with exception isolation and a timeout**; if it throws or times out, ASF falls back to its **own forced reset** (the §3 self-heal). The reverter contract is documented as "idempotent, non-throwing, fast." This keeps a buggy addon from corrupting body state or the save during the critical load path.

**Tier 3 — Faction-as-state broadcast (DEFERRED — interop only if a reader exists).** Mirroring arousal/times-climaxed into faction rank for vanilla conditions is OStim's *Skyrim* trick. **It is cut from MVP and gated behind a consumer spike (§8):** confirm a vanilla Starfield perk/dialogue/magic-effect condition can read a faction rank ASF sets, and benchmark per-frame rank-write cost across many NPCs. If the consuming condition surface is thin or the write cost is high, Tier-3 ships nothing — ASF does **not** ship a broadcast nobody can hear. When it does ship, factions are a **read-only mirror**; the truth stays native (no rank-saturation reliance).

### Canonical event vocabulary — a SUPERSET of `SceneEvent`, frozen only when complete

The Sensual Scenes Adapter exists only because SexLab and OStim forked their event vocabularies. ASF ships one, stable set — and it **passes through every field OSF's `SceneEvent` already emits** (`node`, `edge`, `cue`, `actionType`, `anchor`, `time`, `loopIndex`, `result`) so addon authors can sync an effect to a thrust (clip-local `time`) or react to a specific `node`. On top of that it adds the arousal/orgasm/relationship events OSF lacks: `ASF_SceneStart`, `ASF_StageChange`, `ASF_Orgasm` (per-actor), `ASF_AnimationChange`, `ASF_ActorAdded/Removed`, `ASF_SceneEnd` (with `endedBy`/originator). Each carries `{handle, actors[], tags[], stage, actor?, role?, node, anchor, time, …}`. **The freeze is applied only after the event carries everything OSF emits** — a partial freeze would force a breaking add in v1.1 and make the "append-only" promise a lie on day one.

### Registration & versioning

- **Registration:** addons register listeners/reverter-intents at load; ASF requires **no** content registration (packs are data). No SexLab-style multi-minute callback storm.
- **Versioning:** **explicit semver decoupled from the file/Nexus version** (fixing OStim's welded-to-DLL-version flaw). `ASF_GetAPIVersion()` returns the semver; addons query `ASF_HasCapability("reversible-mechanisms")` and **degrade gracefully** rather than hard-crash on mismatch.
- **Stability policy:** the (complete) event vocabulary and Tier-1/Tier-2 signatures are **append-only and ABI-frozen** within a major version — new fields appended, never reordered/removed.
- **Ownership model:** every scene records an **originator tag**; `EndScene`/`RequestEndScene` emit `endedBy`, so addons cooperate instead of silently killing each other's scenes (cheap now, saves an adapter mod later).

---

## 7. Anti-Patterns to Avoid

1. **Save bloat** (SexLab). No per-scene script state; persist only durable native co-save blobs — **including reversible-effect debt for load-time reconciliation.**
2. **Reversible state driven off async script events** (the most dangerous repeat). The morph/fluid/equip ledger is **native and hooks OSF's real teardown**, never a Papyrus `SceneEvent`; orphaned body state self-heals on load (§3, §4.6).
3. **Slot / actor / hook caps** (SexLab). No *arbitrary* caps — but autonomy has a **concurrency governor** (§4.10); "no caps" ≠ "no governor."
4. **Alignment hell** (SexLab's 4 sources; AAF's baked XML). One source of truth (OSF anchor + placement) with proportion-keyed persisted offsets — **after verifying placement composes with actor scale** (§4.8).
5. **MCM / settings overload.** Hierarchical grouped settings, per-module master toggles, content-flag categories, sane defaults.
6. **Dependency hell + version welding.** Minimal hard deps; semver decoupled from file version; capability negotiation + graceful degradation.
7. **Ecosystem fragmentation / event forking.** ONE canonical event vocabulary, defined day one, **a superset of `SceneEvent`**, frozen only when complete (§6).
8. **Registration time / install cache.** Content is data, indexed natively at load; per-file error isolation, never a global parse failure (AAF anti-model).
9. **Papyrus in the hot path.** All per-frame, per-event re-broadcast, and reversible work is native; Papyrus is policy glue only.
10. **API permutation sprawl** (OStim's ~300 query functions). One tag-query DSL.
11. **API verbs that don't match the runtime** (OStim's trap). No linear `SetStage` over a graph runtime — expose `GoToStage`/edge navigation (§6 D1-fix).
12. **Lossy event re-wrap.** Never discard native `SceneEvent` fields — the canonical event is a superset (§6 D2-fix).
13. **Mutating addon callbacks in the teardown path** (a save-corruption blast radius). Sandboxed pure-data reverters with timeouts + exception isolation, never opaque closures (§6 D3-fix).
14. **Unauthenticated cross-scene mutation** (OStim's "who owns this scene" gap). Originator tags + `endedBy` + cooperative `RequestEndScene` (§6 D4-fix).
15. **Filename-as-ID collisions** (OStim). Namespaced IDs (`author.scene`), collision-warned at load.
16. **Gender-hardcoded slotting** (SexLab band-aid). Capability-driven, validated at select-time.
17. **Strict-parser global failure** (AAF). Lenient JSON, schema-validated, skip-and-report per file.
18. **Advisory content flags** (a trust failure). Content flags are a **hard native gate**, enforced on every start path (§5).
19. **Importers that fabricate clip references.** No automated importer; ship a tag-mapping table as documentation instead (§5).
20. **Skyrim idioms ported without a consumer.** Faction-broadcast, freecam control, and proportion-alignment are each **spiked before they are depended on** (§8).

---

## 8. MVP → v1 Roadmap

> **Resourcing assumption (stated, because every dead framework's design doc omits it).** This is six frameworks' worth of scope; each of WickedWhims/WonderfulWhims/OStim/SexLab is *one* of these as a multi-year project. The roadmap below is therefore phased as a **small-team, serial** plan with a **brutal MVP** and an explicit post-1.0 line — not a paper claim that every phase ships in parallel.

### Phase 0 — Foundation + dependency resolution
Bind ASF to OSF's stable seam (`StartSceneByTagsQuery`, `RegisterSceneCallback`, `EVENT_*`, `osf.*`). Stand up `asf.dll`, native co-save serialization, the settings tree, and the **native event re-broadcaster**.

**Resolve the serial blocker explicitly.** ASF's compiler targets `*.osf.json` schema 1, which the RFC marks *accepted, pre-launch, not yet implemented* (a 5-phase native refactor that deletes `PackRegistry`). ASF has **zero shippable surface** until that lands. Pick one, now, in writing:
- **(a)** ASF **drives** the OSF schema migration to done as part of Phase 0 (ASF is its primary consumer); **or**
- **(b)** ASF Phase 1 targets the **current shipping two-file schema** and eats a migration later.

"Track the RFC" is not a plan when the RFC is unbuilt. **Decision (chosen 2026-06-24): (a)** — ASF is schema-2's primary consumer, so the schema-2 migration is finished *as* Phase 0 before ASF feature work begins. *Rationale: building ASF against the soon-to-be-deleted two-file schema would mean writing the compiler twice and carrying a migration debt through the riskiest early phase; finishing schema-2 first gives ASF one stable, intended target.* The cost — no ASF demo until the 5-phase refactor ships — is accepted deliberately. **Risk mitigation:** schema-2 must be scoped and time-boxed as its own deliverable with its own acceptance tests (it is independently valuable to OSF regardless of ASF), so ASF's start date is bounded by a concrete refactor, not an open-ended one.

Also in Phase 0: **enumerate the exact `InputService` verbs ASF needs** (next/prev stage, speed up/down, swap position, end) and register "OSF exposes these verbs" as a tracked Phase-1 dependency.

### Phase 0.5 — De-risking spikes (gate Phase 2 and Tier-3 claims)
Three spikes that must pass before the features depending on them are believed:
- **Facial-morph spike:** set and restore **one** facial morph on a live actor, surviving save/load + cell change. **If it fails, the entire expression-compositing subsystem is cut and parity claims vs. AAF/OStim are withdrawn**; ASF ships body-only response.
- **Scale/alignment spike:** verify OSF placement composes with actor scale; derive `ActorKey` over real Starfield actor data. If placement ignores scale, scope normalization here.
- **Faction-reader spike:** confirm a vanilla Starfield condition can read an ASF-set faction rank and benchmark write cost. **If thin, Tier-3 is cut from the roadmap.**

### Phase 1 — Brutal MVP (the product)
The smallest thing that beats a bare OSF scene — and **labeled honestly: this is the *direction skeleton* of a scene, not yet OStim-class** (OStim-class feel is overwhelmingly the body/face layer in Phase 2; do not market the skeleton as the body):
- **Adult schema + compiler** (`*.asf.json` → OSF graph scenes) with namespaced taxonomy, per-file error isolation, and **content flags as a hard native gate.**
- **Arousal engine** (baseline/current/libido, per-actor independent climax, stimulation aggregation) — native.
- **Scene/Act semantics** (foreplay→escalation→climax→aftercare) via arousal-gated `NavigateScene` edges.
- **Capability-driven actor model** + capability-based selection, with the positional-binding round-trip test (§4.3).
- **Undress policy engine** (presets, veto callback) over `osf.equipment.*`.
- **Tier-1 Papyrus API** (graph-native `GoToStage`, originator tags) + **canonical superset event vocabulary** + **native re-broadcaster.**
- **Minimal UI:** scene-selection wizard, nav HUD (driving the enumerated `InputService` verbs), arousal meter, **AAF-style diagnostics panel.**
- *Optional, only if the §4.6c overlay path proves feasible:* one cheap arousal-driven body overlay (sweat) for a hint of visible reaction.

### Phase 2 — The body layer (largest single engineering item, gated by Phase 0.5)
Scheduled by demand × risk:
- **(a) Equip-object attach/detach** (strapon/tongue/cum-mesh) — first-class reversible mechanism; **the first reverter validated against the §3 native self-heal path.**
- **(b) Fluids / wetness / cum overlays** (duration+quantity) on the native reversible ledger.
- **(c) Facial / expression driver** — *only if the Phase-0.5 spike passed;* otherwise deferred and disclosed.
- **Tier-2 C++ ABI + sandboxed reverter-intent registration** (timeouts, exception isolation).
- **Self-heal orphaned body state on load** — explicit, tested acceptance criterion.
- Proportion-keyed alignment storage + live-adjust UI (per the Phase-0.5 scale outcome).

### Phase 3 — Optional gameplay layers (each toggleable, sensitive ones off by default)
- **Relationship / romance** — pre-start gate + event writer, native store.
- **NPC autonomy** — scheduler + per-cell/ship permission policy + **concurrency governor**, **gated on positioned/attenuated audio** (§4.10–4.11).
- **Initiation policies** (consensual / seduction / aggressive / autonomous) as pluggable gates.
- **Fertility / pregnancy / cycle** (WonderfulWhims-class, off by default).

### Phase 4 — Ecosystem maturation & upstream contributions
- **Tag-mapping documentation + tag-override files** (not an automated importer — §5).
- Contribute upstream where ASF outgrew OSF: positioned audio, discovery natives (`FindScenes`/`GetSceneRoles`/`GetSceneTags`), a **reload event** for index coherency, matured director input (reposition/freecam), and a **native OSF mechanism-registration API** so reversible custom verbs become first-class instead of living in `asf.dll`.
- **Tier-3 faction broadcast** *iff* the Phase-0.5 reader spike passed.
- Harden API stability guarantees; publish the API as a first-class documented deliverable.

**The hard post-1.0 line:** Phase 1 + Phase 2(a)(b) is the product. Facial expressions, relationships, fertility, autonomy, Tier-3 broadcast, and upstream contributions are **explicitly post-1.0** — this is where every predecessor over-invested early and shipped instability. The facial driver is the single biggest unknown and **must not gate the MVP**; everything sensitive (fertility, aggression) is off by default for review-safety.

---

**Key seam files for implementers:** `dist/Scripts/Source/OSF.psc` + `OSFTypes.psc` (the live contract — 28 natives, scene-centric; the `SceneEvent` struct ASF's vocabulary must superset); `src/Scene/SceneRuntime_Dispatch.cpp` (the *real* custom-verb dispatch at lines 326–333, positional role binding at 206, listener-centered audio fallback at 99–101); `src/Registry/SceneRegistry.{h,cpp}` (the `required:true` throw and `ActionEntry` field wiring that bound the `asf.*` taxonomy); `src/Input/InputService.h` (the verb-dispatch channel with no freecam primitive); and `docs/RFC-unified-animation-schema.md` (the unbuilt schema-2 migration that Phase 0 must resolve, not merely "track").
