# ASF Gameplay & Mechanics Design Document (v2 — hardened)

**Layer:** Gameplay / Mechanics (sits on the native arousal engine + OSF graph)
**Author:** Lead gameplay design pass — revised against adversarial review
**Scope:** Concrete, opinionated, numeric. Respects architecture constraints (native engine, notify-only `asf.*` beats, OSF `SetSpeed`, InputService verb channel, reversible native ledger, content-flag gate).

---

## Executive summary

ASF's lovable core is a **flawless, calm watch experience built entirely on proven tech**: per-actor independent arousal with independent climax, a deterministic excitement→speed loop that visibly tracks the meter, foreplay→main→climax→aftercare gating, and "just let me watch" as a first-class default. Everything interactive — both minigames, stamina, rating, skill progression, dom/sub, and any morph/blush feedback — is **post-MLP** and must layer on top without ever branching content or blocking the watch path. This v2 inverts v1's priorities per critique: it demotes the "Edge-and-Hold" metronome, leads interactivity with the Dual-Meter loop, rebuilds edging as discrete high-stakes decisions, de-stacks the acceleration curve so climaxes crest instead of teleport, collapses AUTO/MANUAL/FREE into two clean orthogonal axes, and guarantees every reward feels good on Proven tech alone.

## Minimum lovable core (ship this first, perfect it, then layer)

The smallest mechanics set that already feels good — and it is **entirely on Proven tech** (§3.1 Tier-1, §3.3) with zero un-RE'd dependencies: **(1)** per-actor arousal with independent climax (§1.4) — *the* differentiator no other framework gets right; **(2)** the deterministic auto-speed loop (§2.3) defaulting to Auto, so speed visibly tracks arousal with no coin-flip stalls; **(3)** foreplay→main→climax→aftercare gating via `maxStimulation<100` (§1.3), free with the architecture, giving every scene shape; **(4)** Proven-only feedback on every beat — voice via `CalcReaction`, climax SFX, screen FX, controller rumble, and a *single additive* mesh-fluid equip on orgasm (no tier-swapping); and **(5)** the "Just let me watch" switch (§7.2) that forces Auto+Locked-camera, caps multi-orgasm, softens screen FX, and keeps "End scene" one press away. That is 80% of the audience's revealed preference (§0.3) served flawlessly, and it ships without stamina, rating, skill XP, minigames, dom/sub, fluid tiers, or morph/blush — all of which are explicitly post-MLP.

---

## 0. Design thesis (read this first)

Four convictions drive every number below:

1. **Per-actor independent meters are non-negotiable.** SLSO's single biggest win over vanilla SexLab and the #1 community complaint it fixed (men finishing instantly, women never; simultaneous-climax immersion break). ASF is per-actor from the ground up.
2. **The excitement↔speed feedback loop is the headline mechanic — but the *watch experience* is the headline product.** OStim proved a self-accelerating `speed → excitement → speed` loop feels great. We adopt it, fix its coin-flip jankiness with a deterministic curve, and — per critique — recognize that the flawless *automatic* version of this loop, not any minigame, is the thing that sells the mod.
3. **The community's revealed preference is "set it and watch."** Even flagship OStim ships without deep in-scene controls and leans on Auto Mode. Auto is the default, depth is opt-in, scenes never hard-fail, and the minigame is an enhancement, never a toll booth.
4. **Every payoff must land on Proven tech.** No reward — not an edge release, not a "good sync," not afterglow — may *depend* on un-RE'd morph/blush or repeated-equip swaps. Those are pure upside; the moment must already feel great with voice + SFX + screen FX + rumble + one mesh equip.

**Non-negotiable safety invariant (stated up front, repeated in §7.2):** "End scene" is **always bound, always immediate, and never gated** by mode, minigame state, refractory, or the climax handshake. The handshake (§1.4) must never be able to swallow a player stop.

Everything is data-tunable. The engine ships sane defaults; scene authors and players adjust.

---

## 1. The arousal core (refined SLSO)

### 1.1 Per-actor state (native, keyed by `(sceneHandle, role)`)

Collapsing vanilla's confusing `BaseEnjoyment`/`QuitEnjoyment`/`FullEnjoyment` triad into **one signed axis** that rises and falls:

| Variable | Type | Default | Meaning | Source |
|---|---|---|---|---|
| `arousal` | float, **−100..+100** | seeded (1.3) | The one signed axis. ≥0 = pleasure, <0 = pain. **Clamped to [0,100] whenever all content flags are off (hard invariant, §1.7).** | Vanilla `FullEnjoyment` signed-axis (`sslActorAlias.psc:1486,1510`) — *kept* |
| `baseline` | float, 0..40 | personality (1.3) | Resting arousal floor; climax discharges toward this. | Vanilla's abandoned `kArousalRate` (`sslActorStats.psc:671-680`) — *revived* |
| `libido` | float mult, 0.5..2.0 | 1.0 | Per-actor fill-rate multiplier. | SLSO `sl_enjoymentrate_*` |
| `maxArousal` | float, 0..100 | from node (2.1) | Ceiling the current node can push you to. <100 ⇒ cannot climax here. | OStim `maxStimulation` (`Thread.cpp:238`) — *kept* |
| `arousalDecay` | float/sec | 0.5 | Decay when above `maxArousal`, after grace. | OStim 0.5/s (`ThreadActor.cpp:35`) |
| `decayGrace` | ms | 5000 | Grace before decay (downward stickiness). | OStim 5000ms (`0xDB4`) |
| `stamina` | float, 0..100 | 100 | **P0/MLP-adjacent brake** (§1.8). Drains with high speedMod; gates "always frantic." | SLSO per-click cost, generalized |
| `orgasms` | int | 0 | Count this scene. | Vanilla `Orgasms`; SLSO `*(1+count)` |
| `refractoryUntil` | float (gametime) | — | Hard gate after climax. | Vanilla `LastOrgasm` (`sslActorAlias.psc:936`) |
| `climaxState` | enum | NONE | NONE / EDGING / AWAITING / REFRACTORY / AFTERGLOW. | OStim handshake |

**Simulation tick: fixed 1.0s** for the arousal model (decoupled from presentation). Speed smoother runs at the 50fps OSF loop; arousal integrates once per second from accumulated stimulation. We reject vanilla's voice-tick-coupled recompute and reject OStim's 20ms loop for the *arousal* layer.

### 1.2 Stimulation aggregation rule (the missing intensity axis)

**Per-second arousal gain for actor A:**

```
gainRaw(A) = sum_blend( stim(action_i, role_of_A) ) × libido(A) × accelMod(arousal_A) × speedMod(A)
gainPerSec(A) = min( gainRaw(A), GAIN_CAP )          // ← NEW: hard per-tick cap, default 25/sec
```

**Per-tick gain cap (critique B2).** The product of three multipliers (rate × accel × speed) is otherwise exploitable: a frantic anal node at high arousal would be `12.0 × 3.0 × 2.0 = 72/sec` — one second to threshold, collapsing the economy into "spam top speed on the highest-rate act." We clamp `gainPerSec` to **`GAIN_CAP = 25/sec`** (tunable §7.1) so no single act trivializes the bar. Combined with the softened accel curve (§1.2 below) and stamina coupling (§1.8), "always frantic" self-limits.

**Multi-source blend** (dominant action full weight, rest at 10% — OStim's `s0 + 0.1·Σ(rest)`):

```
sum_blend(S) = max(S) + 0.1 × (sum(S) − max(S))
```

**Stimulation rate table** (per-second base `stim`, ported from P+ `SexLab.ini`):

| Interaction | giver (active) | receiver (passive) |
|---|---|---|
| Vaginal | 10.0 | 11.2 |
| Anal | 11.4 | 12.0 |
| Deepthroat | 6.8 | 5.5 |
| Oral | 5.0 | 5.8 |
| Licking | 3.7 | 4.7 |
| Handjob | 3.4 | 3.9 |
| Footjob | 3.1 | 4.1 |
| Grinding | 3.8 | 3.2 |
| Facial | 3.0 | 2.6 |
| Stimulation (ambient) | — | 2.5 |

**Acceleration curve — softened and plateaued (critique B1).** v1's `clamp(arousal/30,1,3)` made the *last* 10 points the fastest while speed coupling and the smoothstep envelope were also at maximum — a triple-stacked acceleration that teleported the final third of the bar. We do two things:

```
accelMod(arousal) = clamp(arousal / 40, 1.0, 2.5)                       // softer ramp (was /30→3.0)
crestMod(arousal) = 1.0 − 0.4 × smoothstep(85, 100, arousal)           // plateau: decelerate near the top
gainShaped = gainPerSec × crestMod(arousal)
```

The `crestMod` plateau means the final approach 85→100 *decelerates* slightly, so a climax feels like **cresting** rather than tripping. This also makes the edge band (§2.5) genuinely *holdable* without the v1 "pin" hack — the bar is no longer screaming through 88–98 at max rate.

### 1.3 Lead-in / foreplay & personality seeding

**Foreplay gating via `maxArousal`, not a timer.** Foreplay nodes are annotated `maxStimulation < 100` (kissing = 35, oral foreplay = 70). You physically cannot climax there; to finish you navigate to a penetrative node. OStim's cleanest mechanic, replacing vanilla's discrete LeadIn hack.

**Personality seeding** (deterministic + small noise):

```
baseline(A)  = clamp( 10 + 5×(libidoTrait − 2) , 0, 40 )      // libidoTrait 0..4, default 2 → 10
arousal_init(A) = baseline(A) + noise(±3)
libido(A)    = clamp( 1.0 + 0.15×(libidoTrait − 2) + sexRateSlider , 0.5, 2.0 )
```

Relationship/role modifiers fold into `baseline` (loving partner +5). **Aggression-flavored negative baseline (aggressor −10) is content-flag gated and OFF by default.** With the flag off, `baseline` is clamped ≥ 0 for **every role with no exception** (hard invariant, §1.7).

### 1.4 Per-actor INDEPENDENT climax (the SLSO win)

**One per-actor path only.** Fires when `arousal ≥ 100` AND stimulated AND `maxArousal == 100` AND past refractory:

```
climax(A) fires iff:
    arousal(A) ≥ 100
    AND maxArousal(A) == 100
    AND isStimulated(A)                         // SLSO/vanilla cum-source gate; a full-bar giver does NOT pop
    AND gametime > refractoryUntil(A)
```

**Climax as a handshake** (OStim `awaitingOrgasm → awaitingClimax → awaitingClimaxInner`) so the orgasm syncs to the `asf.orgasm` beat and voice line. **The handshake must never block a player "End scene" (§0 invariant).** On fire:
- emit `asf.orgasm` beat `{actor, role, node, anchor}` (notify-only; §3).
- `orgasms += 1`.
- **Discharge:** `arousal ← min(baseline + 10, baseline + 25)`. We reject OStim's magic `−3` and vanilla's `QuitEnjoyment +=` hack. Meter drops to near baseline — clean, legible.
- `refractoryUntil ← gametime + refractory(sex)` where `refractory = female 10s, male 20s, (creature 30s — content-flag gated)`.

### 1.5 Multi-orgasm — arousal-gated, not a slot machine (critique B3)

v1's pure 25% RNG refill-to-`baseline+40` then re-accelerate made scene length a dice sequence — annoying in Auto where you're watching. Determinism is our thesis (§0.2); we honor it here:

```
on climax, after refractory, only if orgasms < maxOrgasmsPerScene:
    p_multi = baseMulti × edgeFactor                       // baseMulti=20%; edgeFactor scales with
                                                            //   edge stacks banked / band depth at release
    if roll(p_multi): arousal ← baseline + 30              // partial refill funds another round
    else:             enter AFTERGLOW
```

- **`maxOrgasmsPerScene` hard cap (default 2; "Just let me watch" forces 1).** Exposed in §7.2.
- `p_multi` is now driven by *play* (edges banked / how deep into the band the release happened), not pure chance — rewards the minigame and removes the unwanted-RNG-chain failure mode. A pure-Auto watcher with no edges gets near-baseline `p_multi`, i.e. predictable short scenes.

### 1.6 Refractory / afterglow

After the final orgasm, enter **AFTERGLOW** — an explicit state (fixing OStim's missing aftercare):
- arousal decays toward `baseline` at `arousalDecay` (0.5/s).
- scene compiler routes a post-climax `NavigateScene` edge to an aftercare node (cuddle/rest). Scene does **not** auto-end on first climax by default (we reject OStim's `endOnMaleOrgasm` default-ON). Scene ends when **all** human actors have climaxed at least once *or* the player ends it (tunable §7).

### 1.7 Pain axis (kept, gated) — with airtight non-negative invariant (critique F2)

`pain = abs(arousal)` when `arousal < 0`, driving voice/expression via `CalcReaction(clamp(abs(arousal),0,100))`. Negative arousal only occurs in aggression-flagged content.

**Hard invariant (unit-tested):** *with all content flags OFF, `arousal` is clamped to `[0, 100]` for **every** role with **no** exception — including "rough" vanilla-safe scenes.* Any role modifier that would drive `baseline` or `arousal` negative is clamped at 0 before it can produce a pain voice line. One stray negative baseline in a vanilla playthrough is a ship-blocker.

### 1.8 Stamina — promoted to load-bearing brake (critique B2/E2)

v1 listed stamina as a P0 "nice-to-have." It is actually the **wall that makes the entire skill layer possible** (without it, "hold/frantic forever" is free). So:

- **Stamina is a hard dependency of the speed coupling, designed *now*, not later.** It need not ship in the MLP watch core, but it **must** ship before *any* minigame or before `speedMod` is allowed above ~1.5×.
- Drain is **superlinear in speedMod**: `staminaCost/sec = k × max(0, speedMod − 1.5)^2 × (1 / (skill + arousalMod))`. Skilled/aroused actors pay less; "always frantic" drains fast and forces slower acts or scene end.
- Running dry caps achievable `speedMod` at 1.5× until recovered (slow regen in low-speed/foreplay nodes).
- In pure Auto (no minigame), stamina is invisible to the player but still shapes the auto-speed target so frantic stretches naturally ebb.

---

## 2. Excitement ↔ speed coupling (the headline auto-loop)

### 2.1 Node speed ranges & the SetSpeed wire

Every OSF node carries an ordered speed vector (`node->speeds[]`). ASF drives **OSF's scene `SetSpeed(index)`** natively (no Papyrus polling). Author annotates per-node:
- `speeds: [...]` (1..N stages, with `playbackSpeed`/`displaySpeed` time-scales).
- `stimulation` / `maxStimulation` per action role.
- `speedStimScale` (default 1.0).

### 2.2 The coupling formula (honest to on-screen tempo)

We couple on **normalized displaySpeed**, not stage index (OStim's loose `1 + speed/count` is criticized for building differently at equal visual tempo):

```
speedNorm(A) = displaySpeed(currentSpeedIndex) / displaySpeed(maxSpeedIndex)   // 0..1
speedMod(A)  = 1.0 + speedStimScale × speedNorm(A)                              // 1.0 .. 2.0
```

Top speed roughly doubles fill rate vs slowest — the bound OStim validated, honest to tempo.

### 2.3 Auto-speed curve toward climax (deterministic; ramp in displaySpeed units, critique A2)

OStim's probabilistic 2.5–7.5s dice-roll causes jerky pacing. We drive speed directly from excitement with smoothing — and we ramp in **displaySpeed units/sec**, not notches/sec, so the ramp is stage-count-agnostic and self-consistent with §2.2:

```
lead        = max(arousal over stimulated actors)               // OStim getMaxExcitement
targetNorm  = smoothstep(autoMin, autoMax, lead)                // autoMin=15, autoMax=85
targetDisp  = targetNorm × displaySpeed(maxSpeedIndex)          // target tempo in displaySpeed units

every speed tick:
    // close 50% of the remaining displaySpeed gap per second, then snap to nearest available notch
    curDisp += rampFrac × (targetDisp − curDisp)                // rampFrac = 0.5 / sec
    SetSpeed( nearestNotch(curDisp) )                           // ramps DOWN as well as UP
```

`rampFrac = 0.5/sec` means tempo closes half the gap each second regardless of whether the node has 4 or 8 speeds — a 4-speed and an 8-speed node *feel* the same approaching the same tempo (fixes v1's molasses-on-8-stages problem and keeps the displaySpeed honesty of §2.2 intact through the ramp). We also ramp **down** when arousal drops (e.g. switching to foreplay) — an OStim gap.

### 2.4 The two control axes (collapsed from v1's three-mode trap, critique D1/D2)

v1's AUTO/MANUAL/FREE reintroduced exactly the conflation it mocked: FREE was a *camera* concept masquerading as a third value on the *control* axis. v2 ships **two clean orthogonal axes**:

| Axis | Values | Meaning |
|---|---|---|
| **Control** | **Auto** \| **Manual** | Auto = engine drives speed (§2.3). Manual = player drives speed (`speed+/−` verbs). |
| **Navigation** | **Auto** \| **Director** | Auto = engine routes foreplay→main→climax→aftercare. Director = player picks acts/positions. |
| **Camera** | **Locked** \| **Free** | Orthogonal. Free = unlocked scene cam. Never named as a "mode." |

- **"Just let me watch" = Control:Auto + Navigation:Auto + Camera:Locked.** The default.
- **Mainstream Manual = Control:Manual + Navigation:Auto** — "I drive pace, the engine handles act flow." This fixes critique D2: the mainstream Manual user is **not** forced to hand-advance every stage (the ticket-punching anti-pattern §4.5). Manual-pace with auto-navigate is the intended middle ground.
- **Director (Navigation:Director)** — hand-pick every act — is for posers/directors only (§5 P3), not the mainstream Manual user.
- Any of the combinations is selectable by advanced users; the word "FREE" as a mode is **deleted**. One coherent mental model, zero orthogonality-in-disguise.

All speed/navigation actions map onto the **InputService verb channel** (next/prev stage, speed up/down, swap position, end). No new input capture for Auto/Manual/Director/camera toggles — proven today.

### 2.5 The edging / denial loop — rebuilt as discrete decisions (critique A1, **demoted from flagship**)

v1's flagship was a low-frequency tap that pinned a partner in 88–98 for *banked seconds* — i.e. metronome maintenance, tedium-with-a-scoreboard by playthrough 10. We rebuild edging as **discrete, high-stakes decisions**, and we **demote it from flagship**; the lead minigame is now Dual-Meter (§4.2).

**Discrete edge events (no held button):**

```
Drive the partner up toward the top of the band. When arousal crosses 98:
    a single timed "PULL BACK" beat opens — a ~1.5s window, ONE input.
    Nail it  → +1 edge stack (chunky, satisfying); arousal drops to ~70; partner reacts HARD.
    Miss it  → they tip over: soft climax, stack banked-but-capped. Scene continues (never a hard fail).

Edge stacks give DIMINISHING returns (so there is a natural "release now" call):
    edgeValue(n) = base × 0.8^(n-1)        // 1st edge worth most; ~4–5 edges is the practical ceiling
Deliberate release: partner climaxes, payoff scaled by total banked edgeValue.
```

Each edge is **one clean high-stakes moment**, max ~4–5 per scene, with its own rhythm and a real greed/caution decision ("bank another or release now?") that does **not** collapse into busywork once the safe rhythm is learned. The `crestMod` plateau (§1.2) makes the band holdable enough that the pull-back window is fair.

**Payoff is Proven-tech-only (critique C1, see §4.2 win-line):** an edge release feels great using *only* `CalcReaction` voice, climax SFX, screen FX, rumble, and the single mesh-fluid equip — all Proven (§3.1/§3.3). Morph/blush is pure upside and **never** the thing carrying the moment.

---

## 3. Visual feedback systems

Honesty up front: **input is real today, particles are a mesh-equip away, dynamic skin overlays are the one place to stop claiming AAF parity.**

### 3.1 (a) Cum / fluid system — **additive mesh equip is Proven; tier-swapping is Plausible-needs-test (critique C2)**

All triggered off **`asf.*` notify-only beats**, state owned natively.

| Tier | Approach | Rating | Note |
|---|---|---|---|
| **MLP (ship first)** | **Single additive ARMO equip on orgasm** via OSF's existing `ActorEquipManager`/`EquipmentService` path. Equip a drip/wetness mesh **once** at `asf.orgasm`; never swap-down mid-scene; clear on scene end via ledger. **One equip event per beat.** | **Proven** | Low risk. This is the visible-reaction story for v1. |
| **Stretch** | **Swap fluid mesh BY INTENSITY TIER** mid-scene (light→heavy). | **Plausible — needs a hitch test** | Repeated equip/unequip can cause a 1-frame mesh flash / physics re-init / brief hitch. §3.1 evidence proves *EquipmentService exists*, **not** that it swaps cum meshes mid-animation without popping. Demoted from v1's "Proven." Gate behind a hitch spike. |
| **Fidelity** | **Attach particle `.nif` to a bone** (OSF reaches live `NiAVObject`+bone-map, `GraphManager.cpp:1050-1099`): clone node hosting `NiParticleSystem`, `AttachChild`, toggle emission. | Plausible | Med RE. |
| **Research** | Drive the engine's own VFX/projectile system. No precedent. | Hard | — |

**Implementation path:** `asf.orgasm` → native handler → **single additive equip** (MLP) → record in the **reversible ledger** (self-heals on save/load; no orphaned meshes). Tier-swapping and particle attach are upside, not the v1 story.

### 3.2 (b) Texture OVERLAYS (sweat / wetness / cum / blush) — **Hard (dynamic); be honest**

Skyrim's NiOverride/layered-tintmask tech does not port; Starfield's `.mat` model has no additive skin layering; existing "wet skin" mods are static full-body replacers.

| Tier | Approach | Rating |
|---|---|---|
| **Primary** | **Mesh-based fluids/sheen** (= §3.1 MLP additive equip). The per-actor wetness/cum reaction. Reversible via ledger. | **Proven** |
| **Stretch** | **Runtime material swap** to pre-baked skin variants (sweat L1/L2/L3, cum), per-actor, reversible. Actor→skin-material binding is **un-RE'd**; must survive cell-change/save-load. | High RE risk |
| **Don't promise** | True additive overlay compositing (fading blush *layered* on sweat). No port path. Post-1.0 or never. | Hard / research |

**Blush/facial morph — Plausible-but-gated, and NEVER load-bearing (critique C1).** Body Morph Console proves morphs can be driven at runtime via SFSE (only morphs pre-added to the mesh); Engine Fixes proves facial-morph data is reachable. So a blush/expression morph driver is **Plausible-needs-RE behind a Phase-0.5 spike** (set+restore one morph surviving save/load + cell change). **Body-only fallback if the spike fails. No reward anywhere in this doc depends on blush** — it is pure upside layered onto Proven voice/SFX/FX/rumble/mesh.

### 3.3 (c) Expression / moan / screen / rumble — **Proven (this is where every payoff lives)**

| Channel | Driver | Rating | Path |
|---|---|---|---|
| **Moan / voice** | `CalcReaction(clamp(arousal,0,100))` selects intensity; voice delay shrinks as arousal climbs (`VoiceDelay -= stage×0.8`). | Proven | OSF `SoundService` — at-listener Wwise works today |
| **Climax SFX** | `asf.orgasm` → one `Play()`. | Proven | `SoundService` |
| **Screen FX** | Climax nut-blur + slow-mo (game speed 0.3 / 2.5s) + camera shake — **player thread only, reduced-motion-aware (§7.2/F1)**. | Proven | OStim does this today |
| **Controller rumble** | On climax + on edge pull-back success (haptic "good edge" cue). | Proven | Standard rumble API |

**Reduced-motion default (critique F1):** slow-mo + screen-blur are photosensitivity/nausea triggers. They **default-respect an OS/launcher reduced-motion setting**, not just a buried toggle. "Just let me watch" forces screen-FX intensity to *mild*.

**Honest gap:** positioned audio is **at-listener** today (true 3D needs the deferred Wwise follow-up / miniaudio fallback). For sex scenes (camera near actors) at-listener is fine for v1.

---

## 4. In-scene minigame(s)

### 4.1 The layered interactivity model (Auto default; all post-MLP)

**One content layer, three engagement layers** — the *same* scenes play in every mode; only the control surface changes. **Default = Auto. Every minigame is post-MLP.**

```
┌─ CINEMATIC / AUTO  (DEFAULT, = the MLP) ───────────────────┐
│  Watch. Engine drives speed (§2.3) + navigation. Optional  │
│  camera + always-available "end". Meters resolve on their  │  ← ships FIRST, mass-market, Proven
│  own; calm & predictable (capped multi-orgasm, mild FX).   │
├─ MANUAL / DIRECTOR  (post-MLP) ────────────────────────────┤
│  Drive speed (speed+/−); Navigation:Auto by default so you │
│  are NOT forced to hand-advance stages. Light agency, no   │  ← verb channel, proven
│  fail state.                                               │
├─ MINIGAME / ACTIVE  (post-MLP) ────────────────────────────┤
│  LEAD: Dual-Meter Conductor. Then: discrete Edge events.   │  ← opt-in skill layer, needs stamina (§1.8)
│  Earn rating + XP. Cadence is post-1.0 (capture-gated).    │
└────────────────────────────────────────────────────────────┘
```

**Rules that make layering work (all requirements):**
1. **Same meters underlie all three** — Auto resolves, Manual influences, Minigame plays. No content branching.
2. **Per-axis toggles, not one master switch** (Edge on/off, Dual-Meter on/off, Cadence on/off — independent).
3. **Auto-takeover + bail-out at any moment** — stop driving mid-scene, scene completes gracefully.
4. **Never hard-fail.** Timing gates *quality/bonus*, never *progress*. Every minigame is one press from Auto.
5. **Every payoff lands on Proven tech (§0.4/C1).** No reward depends on morph/blush or tier-swapping.

### 4.2 Recommended minigame #1 (**LEAD**): **Dual-Meter Conductor** (satisfy-the-partner)

Promoted to flagship per critique A3 — it respects the player: resource management toward a human goal, real decisions, no reflex, no metronome, scales to group scenes.

- **Core loop:** two meters (You / Partner). Different acts feed different meters at different rates (§1.2 table). Bring partner to climax **at or before** your own; manage the gap.
- **Inputs:** choose act (`swap position`/`next stage` — steers *which* meter), set pace (`speed+/−` — steers *how fast*), **focus-switch** in group scenes (verb; focus buffs the focused meter, others decay slowly). **All on the verb channel — no capture beyond it.**
- **Win/feedback (Proven-only):** partner climaxes within target window → "in sync" bonus → expressed via **voice (CalcReaction), climax SFX, screen FX, rumble, and the single mesh-fluid equip**. Afterglow buff. Climax too early → reduced rating, **never Game Over**. Morph/blush, if the spike lands, is *extra*.
- **Depends on:** stamina (§1.8) as the pacing brake.
- **Why fun:** resource management with a human goal — the most respected pattern in the space.

### 4.3 Recommended minigame #2: **Edge & Hold → Edge Events** (discrete denial)

Mechanically specified in §2.5 (rebuilt from v1's metronome into **discrete pull-back decisions**). Demoted to #2.

- **Core loop:** drive partner toward 98 → single ~1.5s pull-back beat → bank a chunky edge stack (diminishing returns) → release for scaled payoff.
- **Inputs:** `speed+/−` to climb; one timed "pull-back" verb per edge; a "release" verb. **All verb-channel.**
- **Win/feedback (Proven-only):** banked edges → louder voice, stronger climax SFX/FX/rumble, longer afterglow, +skill XP, single mesh equip. Miss → soft climax, capped stack, scene continues.
- **Depends on:** stamina (§1.8) — the wall that stops "edge forever."
- **Why fun:** restraint-as-skill expressed as *decisions*, not a held button; clean greed/caution call.

### 4.4 Optional minigame #3: **Cadence / Rhythm Drive** — post-1.0, **ships only if input-consume REs out (critique C3)**

Tempo-matching: input *cadence* drives speed; a drifting "sweet spot" band rewards matching the partner.

Per the feasibility report, the InputService hook already sees every raw `InputEvent` (device/idCode/value/`heldDownSecs`). This is **Proven-with-small-deltas**, not a new hook — but it needs:
- **release edges + `heldDownSecs` cadence** (currently filtered; "trivial"),
- optional **gamepad idCode mapping** (received, unmapped; Low risk),
- **input consume** (`InputEvent::status=kStop`) so taps don't also move the player.

**Hard gate (critique C3):** **Cadence ships only if input-consume REs out — no observe-only fallback for this mode.** In a scene where the player cam/body may be parented or locked, an un-consumed movement tap fights the scene cam or nudges the player. We will not ship a mode where taps leak into player movement. Other rhythm-genre non-negotiables: forgiveness tiers (GOOD/PERFECT, never miss=over), **ship latency calibration (§7.2)**, cue matches on-screen thrust, low frequency, always-available auto-pace on hold. **Post-1.0.**

### 4.5 Anti-patterns we explicitly design against

Never hard-fail a sex scene · every input visibly moves a meter (no meaningless QTEs) · punish spam (edge miss = soft climax) · cues peripheral/diegetic so Auto is genuinely hands-off · the mechanic *is* the act, not a ticket to a cutscene · no reflex-gating as the only path · no held-button metronomes (the rebuilt §2.5).

---

## 5. Additional mechanics worth having (prioritized — MLP vs post-MLP made explicit)

**MLP (ship first, all Proven):** per-actor arousal + independent climax (§1.4) · deterministic auto-speed (§2.3) · foreplay→main→climax→aftercare gating · Proven-only feedback · "Just let me watch." **Everything in the table below is post-MLP** (critique E1 — v1 flattened months-long features against free ones).

| # | Mechanic | Spec + payoff | Effort / Risk | Phase |
|---|---|---|---|---|
| **P0** | **Foreplay→escalation gating** | `maxStimulation<100` foreplay nodes + arousal-gated `NavigateScene`. Compiler routes foreplay→main→climax→aftercare; can't skip to climax. **Payoff:** pacing has shape; scenes breathe. | **Low** — free with architecture | **In MLP** |
| **P0** | **Stamina / exhaustion** | The load-bearing brake (§1.8), superlinear in speedMod. **Designed now**; must ship before any minigame or speedMod>1.5×. **Payoff:** the limiter that gives the skill layer real tension. | **Med (load-bearing, not nice-to-have)** | Pre-minigame |
| **P1** | **Performance rating + skill progression** | Per-scene rating (sync%, satisfaction, edges, stamina efficiency) → XP → unlock positions, longer stamina, finer pace, dom/sub verbs. **This is a *months* feature** (save data, UI, balance, unlock content) — not a launch blocker. **Payoff:** long arc + reason to engage. | **High (months)** | Post-MLP |
| **P1** | **Dom/sub control dynamics** | Meta-toggle inverting agency on the same systems: dom = you control pace/denial; sub = AI drives, you manage your meter. **Payoff:** doubles play space. | **Med** | Post-MLP |
| **P1** | **Focus-switching (multi-actor)** | Attention buffs focused partner's meter; unfocused decay slowly. Verb cycles focus. **Payoff:** group scenes become a juggling act. | **Low–Med** — verb channel | Post-MLP (with Dual-Meter) |
| **P1** | **Mood / expression feedback loop** | Arousal + context → ~6 reaction buckets → moan set, expression target, afterglow moodlet. **Voice half Proven; facial half gated on §3.2 spike — and never load-bearing (C1).** **Payoff:** same act reads differently by context. | **Med** | Post-MLP |
| **P2** | **Consent / refusal dynamics** | Actor declines if relationship/arousal/trait thresholds unmet. **Content-flag gated, off by default.** (Distinct from the player's unconditional hard-stop, §0/§7.2, which is always on.) **Payoff:** seduction becomes a real precondition. | **Med / sensitive — flag-gated** | Post-MLP |
| **P2** | **Exhibitionism risk/reward** | Public acts risk detection (roll vs nearby NPCs) for bonus XP/arousal. **Content-flag gated.** **Payoff:** the kink loop is the social risk. | **Med–High** (NPC detection plumbing) | Post-1.0 |
| **P3** | **Fertility / cycle** | Menstrual-cycle model. **Hard flag gate, off by default.** **Payoff:** long-arc consequence for opt-in players. | **Med / very sensitive** | Post-1.0 |
| **P3** | **Director / poser mode** | Scene editor (orthogonal). This is the home for "hand-pick every act" (Navigation:Director taken to its extreme). **Payoff:** creative tools + sharing. | **High — separate mode** | Post-1.0 |

**Deliberately NOT recommended:** single monotonic slider (Subverse tedium) · idle/incremental unlock-treadmill (decouples mechanic from content) · undifferentiated click-to-fill (devolves to spam) · **held-button metronomes (v1's original edging).**

---

## 6. How it maps to the data model

**Principle: the act-list is *sugar*; all simulation is native.** A scene author writes a **linear act-list** (foreplay→escalation→climax→aftercare) in `*.asf.json`; the compiler lowers it to OSF graph nodes with arousal-gated `NavigateScene` edges and per-node speed ranges. Authors never touch arousal math.

### 6.1 What the author writes (`*.asf.json`)

```jsonc
{
  "scene": "tender_to_passionate",
  "acts": [
    {
      "act": "foreplay_oral",
      "osfNode": "oral_kneel_01",
      "speeds": ["slow", "med"],
      "maxStimulation": 70,
      "roles": {
        "giver":    { "stimulation": "oral.giver" },     // 5.0/s from §1.2 table
        "receiver": { "stimulation": "oral.receiver" }   // 5.8/s
      },
      "beats": ["asf.stage.foreplay"]
    },
    {
      "act": "main_vaginal",
      "osfNode": "missionary_01",
      "speeds": ["slow", "med", "fast", "frantic"],
      "maxStimulation": 100,
      "speedStimScale": 1.0,
      "roles": {
        "giver":    { "stimulation": "vaginal.giver" },     // 10.0/s
        "receiver": { "stimulation": "vaginal.receiver" }   // 11.2/s
      },
      "beats": ["asf.penetrate.begin", "asf.orgasm"]
    },
    { "act": "aftercare", "osfNode": "cuddle_01", "maxStimulation": 0 }
  ],
  "escalation": "arousal",
  "contentFlags": []                         // empty = vanilla-safe (arousal hard-clamped [0,100], §1.7)
}
```

**Authoring stays simple:** the author exposes only —
1. **stimulation values** as named references into the native rate table (`"vaginal.giver"`), not raw floats;
2. **speed ranges** as an ordered `speeds[]` list (mapped onto OSF `SetSpeed` indices);
3. **beat triggers** as a closed enum (`asf.stage.<x>`, `asf.penetrate.begin/end`, `asf.orgasm`) — **notify-only**, payload limited to `{actionType,node,role,anchor,actor}`.

### 6.2 What stays purely native (never in the act-list)

- All arousal math: `arousal`, `baseline`, `libido`, `accelMod`, `crestMod`, `GAIN_CAP` clamp, blend, decay, climax handshake, refractory, arousal-gated multi-orgasm, afterglow.
- The §2.3 displaySpeed-unit auto-speed ramp and §2.5 discrete-edge logic.
- Per-actor state keyed by `(sceneHandle, role)`; the §1.7 non-negative invariant.
- Stamina (§1.8), skill XP, rating, dom/sub, content-flag gates.
- The reversible visual ledger (§3) — beats *trigger* native handlers; the handler owns state and resolves rich data from native simulation.
- The unconditional "End scene" stop (§0) — bound natively, never gated by handshake.

---

## 7. Tuning & feel

### 7.1 Core defaults (anchor table)

| Knob | Default | Range |
|---|---|---|
| Sim tick (arousal) | 1.0 s | — |
| Speed/coupling loop | 50 fps (0.02s) | — |
| Climax threshold | 100 | — |
| Edge band entry / pull-back trigger | climb toward 98 | — |
| Pull-back window | 1.5 s | 0.8–2.5 s |
| Edge stack falloff | `0.8^(n−1)` | — |
| Acceleration | `clamp(arousal/40, 1, 2.5)` | — |
| Crest plateau | `1 − 0.4·smoothstep(85,100,arousal)` | factor 0–0.6 |
| **Per-tick gain cap** | **25/sec** | 15–40/sec |
| Speed coupling | `1 + speedNorm` (1.0–2.0×) | scale 0.5–2.0 |
| Decay | 0.5/s after 5s grace | 0–2/s |
| Auto-speed envelope | smoothstep(15, 85) | — |
| Auto-speed ramp | close 50% of displaySpeed gap / sec | 0.2–0.9 /s |
| Refractory | F 10s / M 20s | 0–60s |
| Multi-orgasm | base 20% × edgeFactor; **cap 2/scene** | 0–100%; cap 0–5 |
| Post-orgasm reset | `baseline + 10`, cap `baseline+25` | — |
| Stamina drain | `k·max(0,speedMod−1.5)²·/(skill+arousalMod)` | k tunable |

### 7.2 Difficulty / accessibility sliders (player-facing)

| Slider | Default | Effect |
|---|---|---|
| **"Just let me watch" master switch** | — | Forces **Control:Auto + Navigation:Auto + Camera:Locked**; multi-orgasm cap = 1; screen-FX intensity = mild; hides all minigame UI; **"End scene" stays one press away**. Make watch-mode *boring and reliable on purpose* (critique F1). |
| **Control axis** | **Auto** | Auto / Manual (§2.4). |
| **Navigation axis** | **Auto** | Auto / Director (§2.4). Director is the poser path. |
| **Camera axis** | **Locked** | Locked / Free (orthogonal). |
| **Scene length** | medium | Scales rate table ×{1.5 short, 1.0 med, 0.6 long}. |
| **Auto-orgasm** | ON | OFF = scene never auto-climaxes (edging-focused play). |
| **Minigame** | OFF | Per-axis: Edge / Dual-Meter / Cadence independent. |
| **Minigame difficulty** | normal | Widens/narrows pull-back window + sync window (forgiveness tiers). |
| **Per-sex rate** | 1.0 / 1.0 | The SLSO "men finish too fast" fix, per-sex `libido`. |
| **Latency offset** | 0 ms | Cadence only — mandatory per rhythm-genre rules. |
| **Reduced motion** | **respects OS/launcher setting** | Disables slow-mo + screen-blur (photosensitivity/nausea), not a buried toggle (critique F1). |
| **Screen FX** | ON (mild in watch-mode) | Slow-mo / nut-blur / shake / rumble toggles. |
| **Content flags** | all OFF | Aggression, fertility, exhibitionism — hard native gate, opt-in only. |
| **End scene (hard-stop)** | **always bound, never gated** | Non-negotiable. Immediate; never blocked by mode/minigame/refractory/handshake (§0, critique F3). |

### 7.3 Feel principles

- **Slow start, fast-but-cresting finish** — softened accel (`/40`) + the `crestMod` plateau so the climax *crests* rather than teleports (critique B1). No triple-stacked acceleration.
- **Downward stickiness** (5s grace + slow decay above ceiling) so arousal feels like momentum.
- **Speed visibly tracks arousal** via the deterministic displaySpeed-unit ramp — no coin-flip stalls, no notch-walk molasses on high-stage nodes (critique A2).
- **No exploit cliff** — the per-tick gain cap + superlinear stamina drain stop "spam top speed on the highest-rate act" (critique B2).
- **Predictable watching** — multi-orgasm is arousal-gated and capped; watch-mode is calm by design (critique B3/F1).
- **Never punish watching** — Auto is genuinely hands-off, cues peripheral, any minigame bails to Auto in one press, and "End scene" is always immediate.

---

## One-line summary

**Per-actor independent arousal (SLSO) × a deterministic, de-stacked excitement→speed loop (OStim, fixed) defaulting to a flawless calm watch experience — with Dual-Meter as the lead interactive loop, edging rebuilt as discrete decisions, every payoff on Proven tech, two clean control/camera axes, a hard non-negative invariant for vanilla-safe play, and an always-on unconditional player stop.**

---

*Key constraints honored: arousal engine is native C++ keyed by (sceneHandle, role), never Papyrus polling (§1, §6.2); excitement→speed wires to OSF `SetSpeed` (§2); `asf.*` beats are notify-only with the closed payload enum, rich data resolves natively (§3, §6.1); minigames lead with the verb channel and explicitly gate Cadence on input-consume RE (§4); visual effects use the reversible self-healing ledger and the MLP ships a single additive equip, not tier-swapping (§3); content flags are a hard native gate with a unit-tested non-negative invariant, off by default (§1.7, §5, §7.2); the player hard-stop is always bound and never gated by the climax handshake (§0, §7.2).*
