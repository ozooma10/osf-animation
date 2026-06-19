# RFC: OSF — a native scene framework for Starfield

OSF is a WIP native animation and scene framework for Starfield. (OSF = Open Starfield? Open Scene Framework? not sure what name should represent)

The goal isn't just to play animations. It's to provide a shared runtime for running **scenes**: coordinated, multi-actor sequences with placement, timing, synced animation playback, stage transitions, effects, events, and guaranteed cleanup.

Starfield already has NAFSF/SAF for animation playback. What feels underdeveloped is the layer *above* playback - the part that lets a mod reliably say "run this staged interaction between these actors" without every mod rebuilding its own controller in Papyrus from scratch.

OSF is an attempt at that missing scene layer. Figured id post this "Request for Comment" to get thoughts/opinions from mod authors on how they think things should behave and be structured - especially what belongs in the "Core" vs not, and what guarantees a mod building on top actually needs from the runtime.

---

## The core idea

OSF treats a scene as a first-class runtime object.

A scene is more than "play animation A, then animation B." It usually needs to:

- bind actors into named roles;
- place and anchor those actors together;
- play synchronized animation clips on one shared clock;
- advance through stages or branches;
- run timed side-effects - fade, camera hold, control lock, equipment changes, sound, etc...;
- notify other mods when things happen;
- stop safely if the scene ends early, fails, or gets interrupted;
- restore whatever state it changed.

Today most of that has to be hand-built per mod. Usually that means Papyrus orchestration, polling, ad-hoc cleanup, and a pile of scene-specific glue code. OSF tries to make that infrastructure shared, native, data-driven, and reusable.


## Who is this thread for?

### Animation authors

You ship animation clips plus JSON metadata. No Papyrus required just to add content.

You describe the clips, actor count, roles, tags, stage data, alignment offsets, etc... OSF discovers the pack at load and makes it available to callers and the matchmaker.

### Scene authors

You compose animations into authored scenes.

A scene can be linear, staged, looping, or graph-based. It can carry timed actions, sound, camera behavior, cues, callbacks, and branches. Simple linear scenes should be possible through a shorthand layer; more complex scenes can drop down to the full graph format.

Ideally eventually have authoring tooling built around this so people can visually build scenes with some kind of "sequence editor".

### Quest and content modders

You call OSF from Papyrus (or native code).

You can start a known scene by ID, or ask OSF to find something that fits a set of actors and tags. The whole point is to *not* hardcode specific animation IDs when what you actually want is "find a fitting two-actor standing scene," "find an intimidation scene," "find a dance scene," whatever your mod needs.

### Integrator mods

You can listen for scene events and define your own namespaced actions.

For example, a scene can carry an action like `yourmod.spawnProp`. OSF doesn't need to know what that means - it just emits the event, and your mod decides what to do with it.

### Users

Basically just have a well performing scene playback core that will work with large, complicated modlists with many mods all peacefully working together.


## Terms used (for clarity)

### Animation

Bone playback only. It moves a rig. 

Doesnt imply camera control, stripping, sound, stage navigation, or any scene policy. It's just motion data plus metadata.

### Animation pack

A collection of animation clips plus metadata.

Packs describe things like actor count, roles, tags, gender/race restrictions, alignment offsets, stage timing, and clip files.

### Scene

A runtime object that binds actors, anchors them, plays animations, runs timed actions, emits events, handles navigation, and cleans up afterward.

A scene can be as simple as one animation stage, or as complex as a branching graph of other scenes.

### Role

A named actor slot in a scene.

Examples: `actorA`, `actorB`, `host`, `visitor`, `speaker`, `listener`.

OSF itself doesn't need to know what those names mean, they're just handles for binding actors, targeting actions, and authoring scene logic.

### Mechanic

A built-in OSF engine-side effect. (Kind of terrible name, im sure better term for these "features")

Examples: control lock, screen fade, equipment hide, weapon sheathe, camera hold, sound playback. Mechanics are the things OSF can run *and undo* itself.

### Action

A timed instruction inside a scene. (osf actions trigger mechanics..)

Built-in actions use the reserved `osf.*` namespace. Third-party actions use another namespace, like `yourmod.*`.

### Cue

A named moment emitted by a scene. 

Cues are useful for callbacks, branching, dialogue integration, UI updates, quest events, or third-party effects. Similar to an Action, but an Action *does* something on a specific target, a cue just emits a signal like an event... maybe i should rename to event...

### Matchmaking

Asking OSF for content that fits a query instead of naming a specific animation or scene ID.

For example: "two actors, standing, consensual tag, no furniture required" could return a fitting authored scene if one exists, or a compatible animation pack entry if no authored scene exists.

---

## Why another framework?

Starfield already has solid native animation work, especially NAFSF and SAF. Those work great for what they are.

The missing layer is the scene layer.

The moment a mod needs multiple actors placed together, synchronized, advanced through stages, wrapped in timed effects, exposed to callbacks, and cleaned up safely, that logic ends up written per-mod. That's the gap OSF is trying to fill.


## Relationship to NAFSF and SAF

NAF/NAFSF pioneered native external-format animation on Starfield: parse a GLB, sample it with ozz, and stamp the pose onto the engine rig each frame. That technique works great for "full override" animations, and OSF's playback core is built on the same idea. (Actively working on RE-ing af clips to be able to run arbitrary clips within SF animation system, but not there yet.)

NAFSF and SAF are primarily *playback/sync* frameworks - they move and sync the rig. OSF is trying to own the scene lifecycle *above* playback:

- actor-role binding;
- world anchoring;
- a shared scene clock;
- stage and graph navigation;
- timed actions;
- event callbacks;
- animation and scene matchmaking;
- deterministic cleanup;
- built-in reversible mechanics.

A scene runtime needs a finer control plane *underneath* playback: the same code surface should own the shared frame clock, actor anchoring, live retargeting (swap the clips on a *live* scene without re-anchoring or restarting), frame-accurate stage advance, and teardown hooks that feed a ledger to undo.

Its just far simpler and more reliable when the scene coordinator and the playback layer live inside the same code surface.

#### Compatibility note

OSF will ship with a SAF compatibility shim to support existing SAF mods as-is

## Relationship to SexLab, OStim, and AAF

SexLab, OStim, and AAF all prove a shared scene ecosystem is valuable. The parts worth keeping are *ideas* though - porting a framework directly also ports all the built-up tech debt and compatibility scars.

SexLab (Skyrim) and AAF (Fallout 4) are category-defining. They standardized how community animations get packaged, registered, tagged, matched, and played as coordinated scenes, and they still sustain huge ecosystems.

It's tempting to keep their designs and just re-base them onto the ozz/GLB stamping technique, but that's not something you can cleanly swap out. The host framework threads assumptions through just about every layer: playback, actor/skeleton management, behavior state machines, the Papyrus/data contracts, slot systems, event timings, etc... Starfield shares little of that, and a direct port is probably *more* work than designing for Starfield cleanly. (It's also native, so just about every aspect ends up structured differently than something assuming Papyrus capabilities)

OStim is the closest spiritual relative, it's the reference implementation for a data-driven, graph-navigated multi-actor scene engine, and OSF's scene model is heavily inspired by it.

So OSF tries to keep the useful lessons:

- shared scene infrastructure is good;
- tags and metadata is good;
- matchmakers are good;
- scene events are good;
- authoring content as data is good;
- graph navigation is more expressive than a fixed sequence;
- cleanup has to be a first-class concern; aint no busted saves here.

## What designing fresh actually buys

### Native, frame-locked multi-actor playback

Playback runs in C++/SFSE, not in a Papyrus per-frame loop.

Actors in a scene are sampled and stamped on one shared clock. That's what makes synchronized multi-actor scenes, clean stage transitions, and precise timing actually work.

### Scenes as data

Animation packs and scenes are described in JSON.

The goal is that authors add content by dropping files under `Data/OSF/`, without needing to write any Papyrus to define animation or scene state/data.

### A scene runtime

OSF scenes aren't just indexed animation phases. A scene can have named roles, nodes, edges, tracks, cues, actions, sound, camera data, callbacks, and cleanup behavior.

Linear scenes are the trivial case. More complex graph-driven scenes should be possible too.

### Hot-reloadable authoring

Authors should be able to add/edit packs and scene files, reload OSF content, and test again without restarting the game.

### Matchmaking over animations *and* scenes

A content mod should be able to ask for *what fits* instead of hardcoding one specific content ID.

A mod can ask for a two-actor scene with certain tags and role filters. If an authored scene exists, OSF can return it. If not, OSF can fall back to a compatible animation pack entry.

### Extensible action events

OSF reserves `osf.*` for built-in mechanics.

Anything outside that namespace belongs to other mods. A scene can carry `yourmod.someAction`, and OSF emits it as an event for your mod to handle.

### Deterministic cleanup

Scenes touch state that has to be restored: player control, camera, equipment, weapons, fades, and other engine-facing effects.

OSF keeps a per-scene undo ledger for the built-in mechanics it owns. When a scene ends, aborts, or is force-stopped, OSF reverses those mechanics exactly once, in reverse order.

Third-party effects need the same discipline. If your mod reacts to a custom action and changes state, your mod has to be able to clean up its own state. Dont blow it.

## Core design decisions

I'd like to get thoughts on the major decisions *now*, rather than after launch.

### 1. Native C++/SFSE, not Papyrus-driven

Playback happens every frame across one or more actors and has to be frame-locked onto one shared clock. Papyrus can't do that. Beyond that, Papyrus has a significantly higher per-call cost than native, so designing natively from the start also avoids the usual laggy scene initiation/transitions.

The cost: a native framework is bound to game internals, so it's version-sensitive. CommonLibSF smooths a lot across game versions, but there'll always be versions that break things and need updating.

The mitigation is feature self-verification. Native hooks self-verify on each supported game build and **self-disable on a mismatch** rather than crash. And everything a *content* author ships is data, not code, so a game update can't break your pack - it can only (temporarily) disable a feature under it.

Example: if the equipment hook breaks after an update and nobody's done the RE work yet to fix it, `osf.equipment.hide` stops working until it's fixed - but animation playback and the rest of the scene keep running. Then either the hook gets updated, or worst case it falls back to Papyrus if no maintainer is active that can do the RE work.

### 2. Data-driven content

Adding animations or scenes shouldn't require writing Papyrus. You drop files under `Data/OSF/`.

The schema *is* the content contract. Authors ship JSON plus animation files. Mods can still call OSF through Papyrus or native code, and integrators can register custom behavior, but content definition itself should be data-first.

The cost: expressiveness is bounded by the schema, which puts a lot of pressure on getting the schema right. So yeah dont blow it.

### 3. SFW core, adult-facing wrapper

The OSF core isnt itself a "Sex Mod".

It knows actors, roles, animations, tags, actions, events, mechanics, and cleanup. It does not know whether a scene is adult content, a machinima sequence, a quest animation, a dance, a scripted intimidation, or a staged cutscene.

That doesn't make adult use cases an afterthought - it just means adult *policy* lives above the core. This keeps the runtime reusable across genres and keeps a clean seam between "the runtime" and "the content."

The likely shape:

**OSF Core** - content-neutral runtime:

- playback;
- actor binding;
- scene graph;
- events;
- matchmaker;
- built-in mechanics;
- cleanup;
- schema validation.

**Adult wrapper** - opinionated convenience layer:

- common adult scene shorthand;
- stripping defaults;
- camera/control defaults;
- furniture or marker policy;
- common role conventions;
- adult tag conventions;
- filters for scene type, orientation, actors, consent/aggression tags, group size, and other domain-specific metadata;
- user-facing policy settings.

The split keeps the runtime reusable while still making the common LoversLab case practical, a "sex" mod gets a lot of setup for free, and the more general machinima/cutscene/staging cases still work outside the adult world.

### 4. Cleanup and save/load boundaries

Scenes touch *global* state - they can freeze player control, hide gear, hold the camera.

For built-in mechanics, OSF keeps a per-scene undo ledger and reverses every engaged mechanism exactly once, in reverse order, on every termination path.

Scene resume after save/load is *not* planned for v1, though. On a world-replacing load, active scenes should be considered dead: handles die, and callers must assume the scene is gone and restart if needed.

OSF should restore the built-in state it owns where it can, but third-party effects have to treat load as their own recovery boundary. If your mod handles `yourmod.*` actions and changes state, your mod needs to self-heal from its own state on load.

### 5. One matchmaking surface over animations *and* scenes

Callers shouldn't have to hardcode content IDs.

OSF exposes a single matchmaking surface that searches both authored scenes and raw animation pack entries. A caller might ask for:

- actor count;
- required tags;
- excluded tags;
- role filters;
- gender filters;
- race/keyword filters;
- furniture/anchor requirements;
- priority/weighting.

If a full authored scene matches, OSF prefers it. If no authored scene exists, OSF falls back to a compatible animation pack entry.

---

## Architecture at a glance

| Layer | Job | Examples |
|---|---|---|
| Core animation playback | Move bones, sync clocks, anchor actors, hold the rig each frame | GLB->ozz import, per-actor sampler/stamper, shared frame clock, clip registry |
| Scene runtime | Run the scene lifecycle | actor binding, node graph, stage navigation, event relay, matchmaker, undo ledger |
| Mechanics | Reversible engine-side effects, each self-gating and individually disable-able | fade, control lock, equipment hide, weapon sheathe, camera hold, sound playback |
| Integration surface | Let other mods call into and react to scenes | Papyrus API, SFSE API, callbacks, custom action events |
| Wrapper layer | Optional opinionated convenience above the neutral core | adult shorthand, common tags, default policies, UI helpers |

A bare animation is *bones only, world untouched*. A **scene** co-locates and frame-locks the actors at an anchor, runs its stages/graph nodes, emits events, layers on whatever mechanics the author opted into, and cleans itself up.

# Authoring model

## Animation authors: ship animation packs

You ship a JSON pack plus animation files under `Data/OSF/`.

Example shape (illustrative, not final schema):

```jsonc
{
  "schema": 1,
  "id": "author.dance.solo",
  "tags": ["dance", "solo"],
  "roles": [
    {
      "name": "dancer",
      "gender": "any"
    }
  ],
  "stages": [
    {
      "id": "loop",
      "clips": [
        {
          "role": "dancer",
          "file": "OSF/Author/Anims/dance_loop.glb"
        }
      ],
      "loop": true
    }
  ]
}
```

## Scene authors: ship scene files

You ship `*.scene.json` files.

A scene uses animations as building blocks and adds the lifecycle on top: stages, graph navigation, actions, cues, sound, camera, callbacks, cleanup.

The schema needs both a simple form and an advanced form. Most authors should *not* have to write a verbose graph for a basic linear scene.

### Simple scene shorthand

```jsonc
{
  "schema": 1,
  "id": "author.scene.example",
  "tags": ["pair", "standing"],
  "roles": [
    { "name": "actorA", "gender": "any" },
    { "name": "actorB", "gender": "any" }
  ],
  "stages": [
    {
      "id": "start",
      "anim": "author.anim.start",
    },
    {
      "id": "loop",
      "anim": "author.anim.loop",
      "duration": 15,
      "loop": true
    },
    {
      "id": "end",
      "anim": "author.anim.end",
    }
  ]
}
```

For common adult-style linear scenes, the adult wrapper could make this even shorter by applying default camera, control, fade, stripping, stage, and ending behavior automatically.

The scene author shouldn't have to hand-write `fade.out`, `control.lock`, `camera.hold`, and similar boilerplate every single time unless they want to override the defaults.

### Adding optional actions

A scene can opt into built-in OSF mechanics or third-party actions:

```jsonc
{
  "id": "loop",
  "anim": "author.anim.loop",
  "loop": true,
  "actions": [
    {
      "at": "enter",
      "type": "osf.control.lock",
      "role": "actorA"
    },
    {
      "at": 2.0,
      "type": "yourmod.spawnProp",
      "role": "actorB",
      "data": "Example.esm|0x801"
    }
  ]
}
```

### Advanced graph scene

For more complex scenes, OSF supports a graph form - branching, menu-driven navigation, cue-based transitions, more complex timing.

```jsonc
{
  "schema": 1,
  "id": "author.scene.greet",
  "name": "Greet",
  "entry": "intro",
  "tags": ["greeting", "two_actor"],
  "roles": [
    { "name": "host", "gender": "any" },
    { "name": "visitor", "gender": "any" }
  ],
  "nodes": [
    {
      "id": "intro",
      "anim": "author.anim.greet_intro",
      "loop": { "mode": "once" },
      "edges": [ { "to": "loop", "when": "end" } ],
      "tracks": {
        "action": [
          { "at": "enter", "type": "osf.control.lock", "role": "host" },
          { "at": 0.5, "type": "osf.fade.out" }
        ],
        "cue": [
          { "at": 1.0, "id": "greeting_started" }
        ],
        "sound": [
          { "at": 0.25, "sound": "OSF/Author/Sounds/greet.wav" }
        ],
        "camera": [
          { "at": "enter", "state": "thirdperson_hold" }
        ]
      }
    },
    {
      "id": "loop",
      "anim": "author.anim.greet_loop",
      "loop": { "mode": "hold" },
      "loopForever": true,
      "edges": [
        { "id": "end", "label": "End scene", "to": "exit", "when": "advance", "default": true }
      ]
    },
    {
      "id": "exit",
      "anim": "author.anim.greet_exit",
      "loop": { "mode": "once" }
    }
  ]
}
```
Note: This advanced schema is terrible and needs work.

---

## C. Quest and content authors: trigger scenes

Content mods call OSF through Papyrus (or native SFSE code).

```papyrus
; Start a specific scene by ID
int sceneHandle = OSF.StartScene(actors, "author.scene.greet")

; …or ask OSF to find something that fits (no hardcoded ids)
int matchedHandle = OSF.StartSceneByTags(actors, allOfTags, anyOfTags, noneOfTags)

; Stop a running scene
OSF.StopScene(matchedHandle)
```
The caller should be able to use OSF at different levels:

- "play this exact scene";
- "find me any matching scene";
- "let me drive the scene through dialogue/menu choices."

---

## D. Integrators: react to scenes and define custom verbs

Integrator mods register for scene events. Events may include:

- scene start;
- scene end;
- scene abort;
- node enter;
- node exit;
- cue emitted;
- action emitted;
- navigation event;
- actor bound;
- actor released.

The extensibility hook, the custom-action rule:

> If an action isn't in the reserved `osf.*` namespace, OSF emits it as an event instead of trying to execute it.

So you can put a `yourmod.*` verb in a node and react to it in script - *the engine never needs to know what it means.*

Example scene action:

```jsonc
{
  "at": 0.5,
  "type": "yourmod.spawnProp",
  "role": "host",
  "data": "YourMod.esm|0x801"
}
```

Example Papyrus handler:

```papyrus
Function OnSceneEvent(OSFEvent:SceneEvent akEvent)
    If akEvent.actionType == "yourmod.spawnProp"
        Actor host = akEvent.actorRef     ; the actor bound to role "host"

        ; Your mod owns what this means.
        ; Spawn a prop, fire an fx, set a quest stage, update UI, whatever.
    EndIf
EndFunction
```

Custom actions are notifications, not blocking mechanics. They don't pause scene progress and they don't report success/failure back to the scene graph. Anything that needs guaranteed ordering, blocking behavior, or reversible engine-state changes probably needs to be either:

- a built-in OSF mechanic; or
- an SFSE plugin with an explicit integration contract.

---

# Built-in mechanics

The exact list is still in flux, but the built-in `osf.*` mechanics are roughly:

- `osf.control.lock` / `osf.control.release`
- `osf.fade.out` / `osf.fade.in`
- `osf.equipment.hide` / `osf.equipment.restore`
- `osf.weapon.sheathe` / `osf.weapon.restore`
- `osf.voice.play`
- camera states (hold/release)

Built-in mechanics are reversible where possible, and feature-gated. If a hook can't validate on the current game version, that feature self-disables and reports diagnostics rather than crashing. (`osf.weapon.*`, for instance, depends on RE work that isn't fully nailed down yet - so it self-disables cleanly rather than taking the rest of the scene with it.)

---

# Diagnostics and authoring support

OSF needs to make failure visible to authors and users. Reloading content should clearly report things like:

- invalid JSON;
- unsupported schema version;
- duplicate ID;
- missing animation file;
- invalid actor count;
- missing role;
- unknown animation ID;
- unknown built-in `osf.*` action;
- ignored custom `yourmod.*` action because no listener is installed;
- failed actor match, and *why*;
- disabled feature because a native hook didn't validate on this game version;
- scene rejected because a required capability is unavailable.

Ideally authors iterate without restarting the game and without guessing why a pack or scene failed to load.

---

# Known constraints for v1

These aren't necessarily permanent, but kind of just restraints put in for intitial pass

## Scenes don't resume after save/load

On load, active scenes stop and handles die. Callers must assume scenes are gone across a load and restart if needed.

OSF cleans up the built-in state it owns where possible, but third-party effects must self-heal from their own state - don't rely on the undo ledger surviving a load.

## One actor, one scene

An actor can only be in one OSF scene at a time. Starting a scene with a busy actor fails cleanly - stop the existing scene first, or pick different actors.

## Custom actions are notifications

Custom `yourmod.*` actions are fire-and-forget events. They don't currently:

- block scene progress;
- acknowledge success/failure;
- guarantee completion before the next stage;
- automatically clean up third-party state.

This keeps the scene runtime predictable. More complex integrations may need a native plugin or a future registered-mechanic system.

## OSF doesn't execute arbitrary code from JSON

JSON can describe OSF mechanics and emit custom action events. It is not a scripting language and won't execute arbitrary code. That boundary matters for stability, safety, and user trust.

---

# Adult wrapper and tag standardization

The OSF core is content-neutral, but the LoversLab use cases need convenience. A separate adult-facing layer should probably provide:

- simpler scene shorthand;
- default camera/control behavior;
- stripping/equipment policy;
- standard stage conventions;
- furniture/marker conventions;
- actor filtering helpers;
- user-facing policy settings;
- a recommended adult tag vocabulary.

The tag vocabulary might be one of the most important parts. Matchmaking only works well if packs and scenes use compatible tags, if every author invents different names for the same concepts, the matchmaker gets a lot less useful.

Tag categories that may need standardizing:

- actor count;
- solo / paired / group;
- standing / sitting / furniture / bed / wall / floor;
- human / creature / robot / alien;
- consensual / aggressive / coercive;
- dominant / submissive role hints;
- orientation;
- clothed / partial / nude assumptions;
- climax/finish stage;
- roughness/intensity;
- furniture required;
- zero-G or special environment requirements;
- dialogue-safe / cinematic / gameplay-interrupting.

I guess its a question of whether OSF *or* the adult wrapper should publish a recommended tag registry early, so packs are interoperable from the start.

---

# Specific questions for comment

These are the main things I'm looking for thoughts on.

## For animation and scene authors

1. Is the simple scene shorthand enough for common linear scenes?
2. What metadata do animation packs need on day one?
3. What tags should be standardized early so matchmaking works across packs?
4. Are role filters like gender, race, and keyword enough, or do you need more (faction, relationship rank, level, equipped item, distance, voice type, body type, furniture, location type)?
5. Should scene authors hand-author camera/control/fade/stripping actions, or should the wrapper handle those as defaults?
6. Do you prefer semantic role names (`dominant`, `receiver`, `speaker`, `listener`) or neutral ones (`actorA`, `actorB`, `role0`, `role1`)?
7. Should adult-specific semantics live entirely in tags, or should the adult wrapper have stronger typed fields for common concepts?

## For quest and content modders

1. Would you rather start exact scenes by ID, or mostly ask the matchmaker for content by tags?
2. Should scenes expose their available navigation edges so a mod can wire them into dialogue or UI menus?
3. What do you need to know before starting a scene: actor count, tags, duration, furniture requirements, role names, required features, author, priority?
4. Should the matchmaker return only the best match, or should callers be able to inspect a ranked list?
5. Should a caller be able to require authored scenes only, raw animations only, or either?

## For integrator mods

1. Is an opaque `data` string enough for custom actions?

   ```jsonc
   { "type": "yourmod.spawnProp", "data": "form=YourMod.esm|0x801;count=2" }
   ```

   Or do custom actions need structured/typed JSON fields?

   ```jsonc
   { "type": "yourmod.spawnProp", "form": "YourMod.esm|0x801", "count": 2, "attachTo": "hand" }
   ```

  I guess mostly would be papyrus mods which probably dealing with strings is a pita

2. Should OSF resolve form references for custom actions, or should the receiving mod parse and resolve them?
3. Do custom actions need to *block* scene progress ("don't advance until my prop finished spawning"), or is fire-and-forget enough?
4. Do custom actions need acknowledgement/failure reporting?
5. Should third-party effects be user-toggleable *through OSF*, or should each integration own its own settings? (Honestly this feels like the wrong control to me - it's more "don't show me these *types* of scenes" than "don't ever strip my character" - but I want to hear it.)
6. Should custom verbs be able to declare soft dependencies, so diagnostics can say "this scene expects YourMod, but it isn't installed"?

## For users and pack maintainers

1. Is it acceptable that scenes don't resume after save/load in v1?
2. What failure modes are most important to report clearly?
3. Should users control individual mechanics ("never allow equipment hiding"), or should control happen at a higher policy level (scene tags and content filters)?
4. Should OSF or the adult wrapper provide a shared tag registry to reduce fragmentation?

---

# Closing

Short version: OSF isn't meant to be just another animation player. It's meant to be a native scene runtime for Starfield.

It should let animation authors ship data, scene authors compose staged interactions, quest mods request fitting content, integrators react through events, and users get a more reliable ecosystem with better cleanup and fewer one-off controllers.

The thing I want feedback on largely around:

- what the schema needs to express;
- what the core should own;
- what the adult wrapper should own;
- what custom integrations need;
- what compatibility actually matters;
- what guarantees authors need to build on this without fighting the framework later.

Thanks for reading!