# OSF Animation - native, data-driven animation & scene framework for Starfield

## Overview:
OSF Animation (OSF = Open Starfield? Open Scene Framework?) is a WIP animation and scene engine for Starfield. It plays synced multi-actor animations, and runs **scenes** - graphs of nodes
with cues, actions, camera, sound and callbacks - entirely from JSON. 

The engine is deliberately content-neutral: it ships *mechanisms* (camera/input lock, screen fade, equip/unequip, audio playback, etc... ), not content.

---

## 1. Why another framework?

### The core idea: Starfield does not just need a way to play animations. It needs a way to run scenes.

A scene is more than bones moving. It usually needs actors placed together, synchronized timing, camera/control handling, equipment changes, sound, stage transitions, callbacks to other mods, and guaranteed cleanup when something ends early. Today, most of that has to be hand-built per mod. OSF tries to make that shared infrastructure.


### Q1 — Why not just use the Starfield-native frameworks that already exist (NAFSF / SAF)?

**NAF/NAFSF** pioneered native, external-format animation on
Starfield: parse a GLB, sample it with ozz, and stamp the pose onto the engine rig each frame. That
technique works great for "full override" animations, and OSF's animation playback core is built on the same idea/technique. 

**SAF** kept that NAF-style playback alive and maintained on current game builds, with a Papyrus
surface for handling solo/paired actor playback, owner/member sync, sequences, etc...

The main limitation they have is from **layer and scope**. On Starfield, both are *playback* layers:
they move and sync the rig. The moment you need the things above that: multi-actor coordination as a
first-class object, world-anchoring, a timeline of side-effects, deterministic cleanup, animation matchmaking and audio playback, your back to hand-writing it per-mod in Papyrus. 

Where a SAF "scene" is a convenience wrapper over playback (phases that advance by index, side-effects
you script yourself, etc...), OSF instead has a validated node graph, per-node
animation/action/sound/camera tracks, and a centralized un-doable ledger of side-effects as engine primitives.

#### "Why not just use NAFSF/SAF for playback and add your layer on top?"

In one sense that's exactly what OSF does: its playback core is built on the same ozz technique NAFSF
pioneered. What is difficult to do is keep a shipped NAFSF or SAF installed and have OSF's scene layer drive it as the playback backend. Their API is the wrong shape to build a scene engine on. What NAFSF/SAF expose is a *playback*
surface - "play this clip on this actor, sync these two." 

A scene engine needs a finer control plane *underneath* that: a shared frame clock it can drive, anchoring/pinning it owns, the ability to retarget the clips on a *live* scene without re-anchoring or restarting, frame-accurate staged
advance, and teardown hooks that feed the undo ledger. Its just far simpler to have the coordination layer and the
stamping be inside the same code surface.

### Q2 - Why not port an established framework from another game (SexLab / OStim / AAF)?

Because the parts worth keeping are *ideas*, trying to port the framework directly will also port all the built up tech-debt and compatibility scars.

**SexLab** (Skyrim) and **AAF** (Fallout 4) are category-defining - they standardized how community
animations get packaged, registered, tagged, matched, and played as coordinated scenes, and continue to sustain
huge ecosystems.

Its tempting to keep there designs and just re-base it onto the ozz/GLB stamping anim playback technique, but ultimately its not really something you can cleanly swap out, the "host framework (sl/aaf)" threads assumptions through just about every layer, from the playback to actor/skeleton management, behavior state machines, the papyrus/data contracts, slot systems, event timings, etc... starfield shares little and probably significantly more work than designing for starfield cleanly.

Additionally this is a native framework so just about every aspect of how the framework is structured will need to be different than assuming papyrus capabilities.

**OStim** is the closest spiritual relative, it's the reference implementation for a data-driven, graph-navigated multi-actor scene engine. OSF's scene model is heavily inspired by it, and carries many of its best ideas.  But OStim is just as
inseparably an Adult Skyrim artifact: A Skyrim-only dependency
stack, Papyrus contracts, and an adult-specific schema down to the engine primitives. Ultimately OSF shares a similar philosophy on a different engine with a different content stance.

### What designing fresh actually buys

On Starfield there's a great *playback* layer but no
content-neutral *scene* engine, and the mature scene engines live on engines that don't exist here.
Building fresh lets OSF try and take the best aspects of all the existing frameworks, while trying to abandon as much technical debt as possible:

- **Native, frame-locked multi-actor playback** in C++/SFSE - pose sampling and rig stamping every frame across actors on one shared clock, no Papyrus in the per-frame path.

- **Scenes as data** - a validated graph of nodes/edges with named roles and per-node tracks (`cue`, `action`, `sound`, `camera`), run entirely from JSON definition.

- **A scene event system** - mods subscribe to scene events (node enter/exit, cues, actions,
  scene end/abort) and drive their own logic off them - quests, custom SFX, UI, telemetry - instead of
  polling or guessing what the engine is doing.

- **Hot-reloadable content** - drop an `*.osf.json` scene under `Data/OSF/` and reload it without
  restarting the game; authors iterate in seconds.

- **Branching, navigable scenes** - a scene is a graph, not a fixed sequence: edges auto-advance on a
  timer, loop, branch on a choice, or wait for a cue, and a caller can list/drive those edges (e.g. wire them to a dialogue menu). Linear scenes like SL/AAF are just the trivial case.

- **A deterministic cleanup/undo ledger** - per scene handle, reverses every engaged mechanism exactly once, in reverse order, on every termination path (potentially with some mid-scene save/load caveats )

- **Core OSF Engine Extensibility Hooks** - any mechanic that isnt reserved by the core engine is handed straight to other mods as an event, so a scene can carry `yourmod.spawnProp` and your script reacts to it - the engine never has to know what your verb means. Authors can bolt on their own behaviors easily and consistently.

---

## 2. The thinking behind the major decisions

I would like to get thoughts/opinions behind the major decisions going into the design of this now, rather than after initial launch.

**1. Native C++ (SFSE), not Papyrus-driven.**
Playback occurs over *every frame* across multiple actors and must be frame-locked onto one shared clock. Papyrus can't do that. Beyond that papyrus has a significantly higher performance cost than native invocations. Designing natively from the start tries to avoid the common consequences of laggy scene initiation/transitions

The cost: the engine is bound to game internals, so
it's version-sensitive. CommonLib helps a lot with "smoothing" across game versions, but ultimately there will always be versions that break things and need to be updated.

Mitigation: individual native hooks **self-verify on each game build and self-disable on a mismatch** rather than crash, and everything a *content* author needs to ship is data, not code, so a game update can't break your pack, only (temporarily) disable features under it. (ex. the equip/unequip armor hook breaks in a version and nobody updates to the new offset, rather than OSF being broken/game crash, the "equipment.hide" action no longer function, but everything else functions as normal). Then either the hook is updated, or if nobody active is capable of the RE work, can fallback to papyrus.

**2. Content-neutral engine.**
The engine provides *mechanics* - `control.lock`, `fade`, `equipment.hide`, `voice.play`, camera holds - but has **zero** knowledge of what they're used for. Policy and content live in *your* mods. 

This keeps the engine reusable across totally different genres and keeps a clean seam between "the runtime" and "the content." 
(OSF will ship with a pretty comprehensice set of mechanics, but this is the hook for mods to register new effects/mechanics, which can be enabled/disabled via the json data)

The cost: scene authors compose their own policy instead of getting it for free.

Mitigation: A thin "adult" wrapper will likely ship alongside the OSF core that provides many of the shortcuts/utilities that "adult" oriented modding expects. This allows for "sex" mods to get a lot of setup for free, while still allowing for more general machinima/cutscene/staging use cases outside of adult world

**3 Data-driven: JSON, no scripting to add content.**
Adding animations or scenes requires **no Papyrus** - you drop files under `Data/OSF/`. The schema is the contract and defines all the sequencing. Mods can register new features/functionality, expose macros to shortcut common needs, but the core is just sequences defined by data.

The cost: expressiveness is bounded by the schema, which puts pressure on getting the schema right (hence this RFC).

**4 Stable contracts + a reserved namespace.**
The Papyrus API is **semver'd**; within a major version, natives are never removed or re-signatured, new capability only ever *adds*. The pack/scene schema is versioned independently. And there's one
simple rule that makes the whole thing extensible: **`osf.*` is reserved for built-ins; any other
namespace (`yourmod.*`) belongs to you.** unknown verbs are safely ignored, with diagnosis tooling exposing when verbs are ignored so users can identify missing dependencies.

**5 Deterministic cleanup + save-safety.**
Scenes touch *global* state - they can freeze player control, hide gear, hold the camera. If a scene
ends *any* way (authored stop, navigation to end, a crash mid-scene, or a save-load), that state
**must** be restored. The runtime keeps a per-scene **undo ledger** and reverses every engaged
mechanism exactly once, in reverse order, on every termination path. Any effect *you* add must self-heal
the same way.

**6 One matchmaking surface over all scenes.**
Content mods shouldn't hardcode scene ids - they should ask for *what fits* and let the engine
bind it. A single query (tags plus per-role filters - gender, keyword/race, etc...) over the one scene
registry returns the best candidate, ranked purely by `priority` tier then weighted-random `weight`.
There is one pool: a bare clip-only scene and a richly authored graph scene compete in the same query,
and a richer scene simply declares a higher `priority` than a barer one.
ask "two actors, these tags" and you get a fitting scene today; when someone later ships a
more elaborate scene with the same tags at a higher priority, the *same* query picks it up automatically.

---

## 3. Architecture at a glance

Three layers, each with a narrow job:

| Layer | Job | Examples |
|---|---|---|
| **Core animation playback** | Move bones, sync clocks, anchor actors, hold the rig each frame | GLB->ozz import, per-actor graph sampler/stamper, shared frame-clock, animation clip registry |
| **Scene runtime** | Run the node graph: navigation, timed tracks, cues/actions, callbacks, the undo ledger | scene registry + validation, scene runtime, event relay, matchmaker |
| **Mechanics** | Sside-effects, each self-gating and individually disable-able | fade screen, equip/remove equipment, camera, player-control, weapon holster, play sound effect, etc... |

A bare animation is *bones only, world untouched*. A **scene** co-locates and frame-locks the actors
at an anchor, runs its stages, and layers on whatever policy the author opted into.

---

## 4. Injection points - how you build on OSF


### A. Author scenes - *content authors*
**What you ship:** an `*.osf.json` scene + GLB/AF clips under `Data/OSF/…`. No code.
Everything an author writes is a **scene** — there's no separate "pack" vs "scene" anymore. A scene is
*minimal by default* (just clips) and *grows graph features* (nodes, edges, tracks, roles, policy) only
when you need them. The engine discovers scenes recursively at load and on `OSF.ReloadPacks()` -
iterate without restarting the game. See [SCENE_SCHEMA.md](SCENE_SCHEMA.md) for the full field reference.

A **minimal** scene is just clips, the actors/roles they're for, gender slots, per-stage/per-clip
alignment offsets (metres/degrees), and timer/loop behaviour:

```jsonc
// Data/OSF/MyPack/dance.osf.json  — a minimal, looping solo
{ "schema": 2,
  "id": "yourname.dance.solo",
  "tags": ["dance", "solo"],
  "roles": [{ "gender": "any" }],                          // optional; else inferred from clips
  "stages": [{ "loops": 0, "clips": ["OSF/Anims/dance_loop.glb"] }] }   // loops:0 = hold/loop forever
```

A solo or simple paired clip can stop there — `OSF.StartScene(actors, "yourname.dance.solo")` or a tag
query plays it. When you need phases, branching, furniture anchoring, or declarative immersion, the
**same scene** grows `nodes[]` (each plays an inline `stages` timeline or `use`s another scene by id)
connected by **edges** (auto-advance on timer/loop, branchable choices, or cue-triggered), with
**roles** (named, with gender/keyword/race filters) and four per-node **track lanes** (`cue`, `action`,
`sound`, `camera`). Actions are where you opt into arbitrary mechanics to trigger during the scene:

```jsonc
// Data/OSF/MyPack/greet.osf.json  — a graph scene
{ "schema": 2,
  "id": "yourname.scene.greet", "entry": "intro",
  "roles": [{ "name": "host", "gender": "any" }],
  "nodes": [{
    "id": "intro",
    "stages": [{ "clips": ["OSF/Anims/dance_loop.glb"] }],   // inline; or "use": "yourname.dance.solo"
    "action": [
      { "at": "enter", "type": "osf.control.lock", "role": "host" },
      { "at": 0.5,     "type": "osf.fade.out" }
    ],
    "cue": [{ "at": "enter", "id": "started" }]
  }] }
```
Built-in `osf.*` actions today: `control.lock/release`, `fade.out/in`, `equipment.hide/restore`,
`weapon.sheathe/restore`, `voice.play`, plus camera states.

> Will ship with an "adult" wrapper that supports scene definitions with *much* simpler definition to represent the more "standard" sl style linear sequences. So things like fade, control/camera locks/ sequencing, etc... are all automated away and not in the scene. Scene just acts as override.

### B. Trigger scenes from your mod — *content / quest authors* 
**What you ship:** an ESP with Papyrus (or native sfse) that calls the `OSF.*` API.

```papyrus
  ; by id …
  int sceneHandle1 = OSF.StartScene(actors, "yourname.scene.greet")
  ; …or matchmake by tags + role filters (no hardcoded ids):
  int sceneHandle2 = OSF.StartSceneByTags(actors, allOf, anyOf, noneOf)
  ; branch, stop, query state…
  OSF.StopScene(sceneHandle2)
```
You get handles, navigation (`Advance`/`Navigate`/edge listing for menus), read-only scene
introspection (roles, gender, tags, actor count) so you can bind correctly, and a
`GetVersion()` handshake.

### C. React to scenes & define your own verbs — *integrators*
**What you ship:** a Papyrus script that registers a callback.
`RegisterSceneCallback(receiver, fn, sceneFilter, eventMask)` delivers `OSFTypes:SceneEvent` structs
for node-enter/exit, cues, actions, scene-end/abort. Delivery is async and non-reentrant — your
handler can safely call back in.

The extensibility hook: **any action verb that isn't `osf.*` is emitted to you as `EVENT_ACTION`.**
So you can put a `yourmod.*` verb in a scene and react to it in script - *the engine never needs to
know what it means.*

**Example — a custom `spawnProp` verb.** Drop the verb into a node's `action` track. The engine sees
an unknown `yourmod.*` type, so instead of running it, it forwards it to you:

```jsonc
// inside a node's tracks.action
{ "at": 0.5, "type": "yourmod.spawnProp", "role": "host" }
```

Then register a receiver and handle it in Papyrus:

```papyrus
Function OnSceneEvent(OSFTypes:SceneEvent akEvent)
    If akEvent.actionType == "yourmod.spawnProp"
        Actor host = akEvent.actorRef        ; the actor bound to role "host"
        ; …spawn your prop, fire an fx, set a quest stage — whatever it means to you
    EndIf
EndFunction
```
---

## 5. What it deliberately doesn't do (yet) — known constraints

Stating these up front because they shape what you can build:

- **Scenes don't survive a save-load.** On a world-replacing load, all scenes stop and handles die.
  Callers must assume scenes are gone across a load and restart if needed. *Any side-effect you add
  must self-heal on your own load event* — don't rely on the per-scene undo ledger across a load.
- **An actor can only be in a single scene** at a time. Starting a scene with a busy actor fails; stop first.
- **Custom actions are notifications, not blocking mechanics.** They don't get ordering guarantees, acknowledgement, or the ability to block scene progress. Anything needing guaranteed reversible engine work has to be a built-in or registered via an SFSE plugin

---

## 6. Where I specifically want comment

These are the main things really looking for thoughts/opinions on

**1. Custom-action payload (4C).** A custom `yourmod.*` action carries its `type`, `role`, and timing
today - but no arbitrary data. I'm leaning toward appending an opaque `data` string e.g. `{"type":"yourmod.spawnProp","data":"formId=Mod.esm|0x801;count=2"}`, and you parse it yourself. **Is an opaque string enough, or do you need structured/typed fields and form-ref resolution done for you?**

**2. Should a custom verb ever *block*? (4C/6).** Right now custom actions are fire-and-forget
notifications - they can't make a scene wait on them or report failure. **Does anyone actually need a
verb that holds scene progress ("don't advance until my prop finished spawning"), or is notify-only good enough, with anything blocking needing to be done in a native plugin?**

**3. Save-load / resumable scenes (6).** Scenes don't survive a save-load - they stop and handles
die, and callers restart if they need to. **Is "assume scenes are gone across a load" acceptable, or
is there a real need for scenes that resume mid-graph after a load?**

**4. Matchmaking expressiveness (2).** Roles bind on tags + gender + keyword/race (by form-ref), with
priority/weight. **Is that enough to cast the scenes you care about, or are you missing a filter (faction, level, equipped item, relationship, distance, …)?**

**5. User control over third-party effects.** Built-in mechanisms are individually toggleable, and a user's setting wins over what a scene asks for. Feels like its the wrong control and would cause more issues than it solves (since its more like "dont show me these types of scenes", not "dont ever strip my character") **Should players be able to toggle *your* `yourmod.*`
effects?**
