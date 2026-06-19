# RFC: OSF — Native Scene Framework for Starfield

**OSF** stands for **Open Scene Framework**.

OSF is a work-in-progress native animation and scene framework for Starfield. The goal is not only to play animations. The goal is to provide a shared runtime for running **scenes**: coordinated, multi-actor moments with placement, timing, animation playback, stage transitions, effects, events, and cleanup.

In simpler terms:

- an **animation** moves bones;
- a **scene** coordinates actors, timing, world state, effects, navigation, and cleanup.

Starfield already has important native animation work in the ecosystem. What still feels underdeveloped is the layer above playback: the part that lets mods reliably say, “run this staged interaction between these actors,” without every mod rebuilding its own fragile controller in Papyrus.

OSF is an attempt to build that missing scene layer.

This RFC is being posted before the public format is locked because the important questions are contract questions: what authors need to express, what should be built into the core, what should live in an adult-focused wrapper, and what guarantees other mods need from the runtime.

---

## Current status

Replace this table with the real state before posting.

| Area | Status |
|---|---|
| Native playback core | [implemented / prototype / planned] |
| JSON animation packs | [implemented / prototype / planned] |
| Scene graph runtime | [implemented / prototype / planned] |
| Built-in mechanics | [partial / planned] |
| Scene event callbacks | [implemented / prototype / planned] |
| Matchmaking | [implemented / prototype / planned] |
| SAF compatibility shim | [planned / partial / not started] |
| Adult convenience wrapper | [planned] |
| Save/load scene resume | Not planned for v1 |

---

## The core idea

OSF treats a scene as a first-class runtime object.

That means a scene is not just “play animation A, then animation B.” A scene may need to:

- bind actors into named roles;
- place and anchor those actors together;
- play synchronized animation clips on one shared clock;
- advance through stages or branches;
- run timed actions such as fade, camera hold, control lock, equipment changes, or sound;
- notify other mods when things happen;
- stop safely if the scene ends early, fails, or is interrupted;
- restore the state it changed.

Without a shared framework, each mod that wants this behavior has to build its own version of it. Usually that means Papyrus orchestration, polling, ad hoc cleanup, and scene-specific glue code.

OSF tries to make that infrastructure shared, native, data-driven, and reusable.

The important architectural difference is this:

> A playback framework answers: “How do I play this animation on these actors?”
>
> OSF is trying to answer: “How do I run this whole scene?”

Playback is still essential, but it is only one part of the scene lifecycle.

---

## Who this is for

### Animation authors

You ship animation clips plus JSON metadata. No Papyrus is required just to add content.

You describe the clips, actor count, roles, tags, stage data, and alignment offsets. OSF discovers the pack and makes it available to callers and the matchmaker.

### Scene authors

You compose animations into authored scenes.

A scene can be linear, staged, looping, or graph-based. It can include timed actions, sound, camera behavior, cues, callbacks, and branches. Simple adult-style scene definitions should be possible through a shorthand layer, while more complex scenes can use the full graph format.

### Quest and content modders

You call OSF from Papyrus or native code.

You can start a known scene by ID, or ask OSF to find something that fits a set of actors and tags. The goal is to avoid hardcoding specific animation IDs when what you really want is “find a fitting two-actor standing scene,” “find an intimidation scene,” “find a dance scene,” or whatever your mod needs.

### Integrator mods

You can listen for scene events and define your own namespaced actions.

For example, a scene can contain an action like `yourmod.spawnProp`. OSF does not need to know what that means. It emits the event, and your mod decides how to handle it.

### Users

The user-facing goal is a more consistent scene ecosystem: better cleanup, fewer one-off controllers, fewer mods fighting over the same actor state, and content that can grow without every author reinventing the same runtime.

---

## Terms used in this RFC

### Animation

Bone playback only. It moves a rig.

A bare animation should not imply camera control, stripping, sound, stage navigation, or scene policy. It is just motion data plus metadata.

### Animation pack

A collection of animation clips plus metadata.

Packs describe things like actor count, roles, tags, gender or race restrictions, alignment offsets, stage timing, and clip files.

### Scene

A runtime object that binds actors, anchors them, plays animations, runs timed actions, emits events, handles navigation, and cleans up afterward.

A scene can be as simple as one animation stage, or as complex as a branching graph.

### Role

A named actor slot in a scene.

Examples: `actorA`, `actorB`, `host`, `visitor`, `speaker`, `listener`.

OSF itself does not need to know what those names mean. They are handles for binding actors, targeting actions, and authoring scene logic.

### Mechanic

A built-in OSF engine-side effect.

Examples: control lock, screen fade, equipment hide, weapon sheathe, camera hold, sound playback.

Mechanics are the things OSF can run and undo itself.

### Action

A timed instruction inside a scene.

Built-in actions use the reserved `osf.*` namespace. Third-party actions use another namespace, such as `yourmod.*`.

### Cue

A named moment emitted by a scene.

Cues are useful for callbacks, branching, dialogue integration, UI updates, quest events, or third-party effects.

### Matchmaking

Asking OSF for content that fits a query instead of naming a specific animation or scene ID.

For example: “two actors, standing, consensual tag, no furniture required” could return a fitting authored scene if one exists, or a compatible animation pack entry if no authored scene exists.

### Wrapper

A higher-level convenience layer built on top of the neutral OSF core.

The most obvious wrapper is an adult-focused layer that automates common defaults such as stripping policy, camera defaults, stage conventions, and tag interpretation.

---

## Why another framework?

Starfield has native animation work already, especially through NAFSF and SAF. Those projects matter. They proved that external animation formats, native sampling, and synchronized actor playback are viable in Starfield.

The missing layer is the scene layer.

When a mod needs multiple actors placed together, synchronized, advanced through stages, wrapped in timed effects, exposed to callbacks, and cleaned up safely, that logic usually has to be written per mod. That is the gap OSF is trying to fill.

---

## Relationship to NAFSF and SAF

NAF/NAFSF pioneered native external-format animation playback in Starfield: parse external animation data, sample it, and stamp the pose onto the engine rig each frame.

SAF continued that general style of native playback on current game builds, exposing a Papyrus-facing surface for solo and paired actor playback, synchronization, sequences, and related use cases.

OSF uses the same broad native playback idea, but it is aimed at a different layer.

NAFSF and SAF are primarily playback/sync frameworks. OSF is trying to own the scene lifecycle above playback:

- actor-role binding;
- world anchoring;
- shared scene clock;
- stage and graph navigation;
- timed actions;
- event callbacks;
- animation and scene matchmaking;
- deterministic cleanup;
- built-in reversible mechanics.

This is why OSF is not simply “SAF plus a few scripts on top.” A scene runtime needs a lower-level control plane: the same code surface should own the shared frame clock, actor anchoring, live retargeting, frame-accurate stage advancement, and teardown hooks that feed the undo ledger.

In practice, that is much simpler and more reliable when the scene coordinator and the playback layer are part of the same runtime.

### Compatibility note

OSF is intended to provide a SAF/NAF compatibility path where practical, especially for playback and sync content. That should be treated as a migration bridge, not a promise that every SAF behavior maps perfectly onto OSF.

Some SAF concepts may not have a direct content-neutral equivalent. If you maintain SAF content, one thing I want feedback on is which compatibility gaps actually matter.

---

## Relationship to SexLab, OStim, and AAF

SexLab, OStim, and AAF all prove that a shared scene ecosystem is valuable.

SexLab and AAF standardized how adult animation ecosystems package, register, tag, match, and play coordinated scenes. OStim is especially relevant as a reference point for data-driven, graph-navigated, multi-actor scene design.

The issue is not that those frameworks had bad ideas. The issue is engine fit.

A direct port would also bring assumptions from Skyrim or Fallout 4: Papyrus contracts, actor handling, skeleton assumptions, behavior state, slot systems, event timing, legacy compatibility layers, and adult-specific schema choices. Starfield has a different engine, different tooling, different constraints, and a different native modding surface.

OSF is trying to keep the useful lessons:

- shared scene infrastructure matters;
- tags and metadata matter;
- matchmakers matter;
- scene events matter;
- authoring content as data matters;
- graph navigation can be more expressive than fixed sequences;
- cleanup has to be a first-class concern.

But it is designing the contracts around Starfield and SFSE from the start.

---

## What designing fresh buys

### Native, frame-locked multi-actor playback

Playback runs in C++/SFSE, not in a Papyrus per-frame loop.

Actors in a scene are sampled and stamped on a shared clock. That is important for synchronized multi-actor scenes, stage transitions, and precise timing.

### Scenes as data

Animation packs and scenes are described in JSON.

The long-term goal is that authors can add content by dropping files under `Data/OSF/`, without writing Papyrus just to define animation or scene data.

### A real scene runtime

OSF scenes are not just indexed animation phases. A scene can have named roles, nodes, edges, tracks, cues, actions, sound, camera data, callbacks, and cleanup behavior.

Linear scenes are the simple case. More complex graph-driven scenes should also be possible.

### Hot-reloadable authoring

Authors should be able to add or edit packs and scene files, reload OSF content, and test again without restarting the game.

This is important. Data-driven frameworks only work well if authors can iterate quickly and diagnose failures clearly.

### Matchmaking over animations and scenes

A content mod should be able to ask for what fits instead of hardcoding one specific content ID.

For example, a mod can ask for a two-actor scene with certain tags and role filters. If an authored scene exists, OSF can return it. If not, OSF can fall back to a compatible animation pack entry.

That means the same caller can benefit from new authored scenes later without changing its own code.

### Extensible action events

OSF reserves `osf.*` for built-in mechanics.

Anything outside that namespace belongs to other mods. A scene can contain `yourmod.someAction`, and OSF can emit it as an event for your mod to handle.

OSF does not execute arbitrary code from JSON. Unknown custom actions are notifications, not arbitrary script execution.

### Deterministic cleanup

Scenes touch state that must be restored: player control, camera, equipment, weapons, fades, and other engine-facing effects.

OSF keeps an undo ledger for built-in mechanics it owns. When a scene ends, aborts, or is force-stopped, OSF reverses those mechanics once, in reverse order.

Third-party effects need similar discipline. If your mod reacts to custom actions and changes state, your mod must be able to clean up its own state.

---

## Core design decisions

## 1. Native C++/SFSE, not Papyrus-driven

Playback happens every frame across one or more actors. It needs to be frame-locked to a shared clock.

Papyrus is not the right layer for that. It is useful for API calls, scene requests, mod integration, callbacks, and high-level behavior, but not for per-frame animation stamping.

The cost is that a native framework is version-sensitive. Starfield updates can break native hooks.

The mitigation is feature self-verification. Native hooks should verify themselves on each supported game build and self-disable on mismatch rather than crash the game. If a specific mechanic breaks, that mechanic should fail safely while the rest of the framework continues to operate where possible.

Example: if an equipment hook breaks after a game update, `osf.equipment.hide` may stop working until updated, but animation playback and unrelated scene functionality should not necessarily be broken.

---

## 2. Data-driven content

Adding animations or scenes should not require writing Papyrus.

The schema is the content contract. Authors ship JSON plus animation files. Mods can still call OSF through Papyrus or native code, and integrators can add custom behavior, but content definition itself should be data-first.

The cost is that the schema has to be good. If the schema cannot express common scenes cleanly, authors will either avoid it or build fragile workarounds.

That is one of the main reasons for this RFC.

---

## 3. Content-neutral core, adult-facing wrapper

The OSF core does not know what a scene “means.”

It knows actors, roles, animations, tags, actions, events, mechanics, and cleanup. It does not know whether a scene is adult content, a machinima sequence, a quest animation, a dance, a scripted intimidation, or a staged cutscene.

That does not mean adult use cases are an afterthought. It means adult policy should live above the core.

The likely shape is:

### OSF Core

Content-neutral runtime:

- playback;
- actor binding;
- scene graph;
- events;
- matchmaker;
- built-in mechanics;
- cleanup;
- schema validation.

### Adult wrapper

Opinionated convenience layer:

- common adult scene shorthand;
- stripping defaults;
- camera/control defaults;
- furniture or marker policy;
- common role conventions;
- adult tag conventions;
- filters for scene type, orientation, actors, consent/aggression tags, group size, and other domain-specific metadata;
- user-facing policy settings.

This split keeps the runtime reusable while still making the common LoversLab use case practical.

---

## 4. Stable contracts and reserved namespace

The Papyrus API should be semver'd.

Within a major version, existing native functions should not be removed or re-signatured. New capability should be additive where possible.

The pack and scene schemas are versioned separately from the Papyrus API.

The action namespace rule is simple:

- `osf.*` is reserved for built-in OSF mechanics;
- any other namespace belongs to the mod or author using it.

Examples:

- `osf.control.lock`
- `osf.fade.out`
- `osf.equipment.hide`
- `yourmod.spawnProp`
- `anothermod.playCustomEffect`

Unknown custom verbs are not fatal. They should be reported through diagnostics so authors and users can identify missing dependencies or unsupported integrations.

---

## 5. Cleanup and save/load boundaries

Scenes can touch global state. That means cleanup must be designed in from the start.

For built-in mechanics, OSF keeps a per-scene undo ledger. When a scene ends, OSF reverses every built-in mechanism it engaged, exactly once, in reverse order.

However, scene resume after save/load is not planned for v1.

On a world-replacing load, active scenes should be considered dead. Handles die. Callers must assume the scene is gone and restart if needed.

OSF should attempt to restore built-in state it owns, but third-party effects must treat load as their own recovery boundary. If your mod handles `yourmod.*` actions and changes state, your mod needs to self-heal from its own state on load.

The intended rule is:

> OSF owns cleanup for OSF mechanics.  
> Your mod owns cleanup for your mechanics.

---

## 6. One matchmaking surface over animations and scenes

Callers should not always need to hardcode content IDs.

OSF should expose a single matchmaking surface that can search both authored scenes and raw animation pack entries.

A caller might ask for:

- actor count;
- required tags;
- excluded tags;
- role filters;
- gender filters;
- race or keyword filters;
- furniture or anchor requirements;
- priority or weighting.

If a full authored scene matches, OSF can prefer it. If no authored scene exists, OSF can fall back to a compatible animation pack entry.

This gives content mods a larger installed content pool on day one, while still allowing authored scenes to upgrade the experience later.

---

## Architecture at a glance

| Layer | Job | Examples |
|---|---|---|
| Core animation playback | Move bones, sync clocks, anchor actors, hold the rig each frame | GLB/AF import, ozz sampling, per-actor stamper, shared frame clock, clip registry |
| Scene runtime | Run scene lifecycle | actor binding, node graph, stage navigation, event relay, matchmaker, undo ledger |
| Mechanics | Reversible engine-side effects | fade, control lock, equipment hide, weapon sheathe, camera hold, sound playback |
| Integration surface | Let other mods call into and react to scenes | Papyrus API, SFSE API, callbacks, custom action events |
| Wrapper layer | Optional opinionated convenience above the neutral core | adult shorthand, common tags, default policies, UI helpers |

A bare animation is bones only.

A scene co-locates actors, locks them to a shared scene clock, runs authored stages or graph nodes, emits events, applies optional mechanics, and cleans itself up.

---

# Authoring model

## A. Animation authors: ship animation packs

Animation authors ship a JSON pack plus animation files under `Data/OSF/`.

No Papyrus is required just to add animation content.

Example shape:

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

This is illustrative, not final schema.

Important metadata likely includes:

- animation ID;
- author namespace;
- tags;
- actor count;
- role names;
- gender restrictions;
- race or keyword restrictions;
- stage names;
- loop/timing behavior;
- per-role alignment offsets;
- required skeleton or rig assumptions;
- required furniture or anchor type, if any.

---

## B. Scene authors: ship scene files

Scene authors ship `*.scene.json` files.

A scene uses animations as building blocks and adds lifecycle: stages, graph navigation, actions, cues, sound, camera, callbacks, and cleanup.

The schema needs both a simple form and an advanced form.

Most authors should not need to write a verbose graph for a basic linear scene.

---

## Simple scene shorthand

This is the kind of shorthand I think OSF should support for common linear scenes:

```jsonc
{
  "schema": 1,
  "id": "author.scene.example",
  "tags": ["two_actor", "standing"],
  "roles": [
    { "name": "actorA", "gender": "any" },
    { "name": "actorB", "gender": "any" }
  ],
  "stages": [
    {
      "id": "start",
      "anim": "author.anim.start",
      "duration": 6.0
    },
    {
      "id": "loop",
      "anim": "author.anim.loop",
      "loop": true
    },
    {
      "id": "end",
      "anim": "author.anim.end",
      "duration": 5.0
    }
  ]
}
```

For common adult-style linear scenes, the adult wrapper could make this even shorter by applying default camera, control, fade, stripping, stage, and ending behavior automatically.

The scene author should not have to manually author `fade.out`, `control.lock`, `camera.hold`, and similar boilerplate every time unless they want to override the defaults.

---

## Adding optional actions

A scene can opt into built-in OSF mechanics or third-party actions.

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
      "data": "prop=Example.esm|0x801"
    }
  ]
}
```

Built-in `osf.*` actions are executed by OSF.

Custom `yourmod.*` actions are emitted as events for other mods to handle.

---

## Advanced graph scene

For more complex scenes, OSF should support a graph form.

This is for branching, menu-driven navigation, cue-based transitions, and more complex timing.

```jsonc
{
  "schema": 1,
  "id": "author.scene.greet",
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
      "tracks": {
        "action": [
          { "at": "enter", "type": "osf.control.lock", "role": "host" },
          { "at": 0.5, "type": "osf.fade.out" }
        ],
        "cue": [
          { "at": 1.0, "id": "greeting_started" }
        ],
        "sound": [
          { "at": 0.25, "id": "author.sound.greet" }
        ]
      }
    },
    {
      "id": "loop",
      "anim": "author.anim.greet_loop",
      "loop": true
    },
    {
      "id": "exit",
      "anim": "author.anim.greet_exit",
      "duration": 4.0
    }
  ],
  "edges": [
    {
      "from": "intro",
      "to": "loop",
      "when": "animEnd"
    },
    {
      "from": "loop",
      "to": "exit",
      "label": "End scene",
      "when": "choice"
    }
  ]
}
```

Again, this is illustrative. The exact schema is still the part I want feedback on.

The goal is:

- simple scenes should be simple;
- advanced scenes should still be possible;
- the underlying runtime should not need to know whether the scene is adult content, a quest sequence, machinima, or something else.

---

## C. Quest and content authors: trigger scenes

Content mods call OSF through Papyrus or native code.

Examples:

```papyrus
; Start a specific scene by ID
int sceneHandle = OSF.StartScene(actors, "author.scene.greet")

; Ask OSF to find something that fits
int matchedHandle = OSF.StartSceneByTags(actors, allOfTags, anyOfTags, noneOfTags)

; Stop a running scene
OSF.StopScene(matchedHandle)
```

The API should expose:

- scene handles;
- start by ID;
- start by tags;
- stop/abort;
- query state;
- list available navigation edges;
- advance or navigate;
- inspect scene roles, tags, and actor count;
- check OSF version;
- check feature availability.

The caller should be able to use OSF at different levels:

- “play this exact scene”;
- “find me any matching scene”;
- “find me a matching animation if no scene exists”;
- “let me drive the scene through dialogue/menu choices.”

---

## D. Integrators: react to scenes and define custom verbs

Integrator mods can register for scene events.

Events may include:

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

The custom action rule is:

> If an action is not in the reserved `osf.*` namespace, OSF emits it as an event instead of trying to execute it.

Example scene action:

```jsonc
{
  "at": 0.5,
  "type": "yourmod.spawnProp",
  "role": "host",
  "data": "form=YourMod.esm|0x801;count=1"
}
```

Example Papyrus handler:

```papyrus
Function OnSceneEvent(OSFEvent:SceneEvent akEvent)
    If akEvent.actionType == "yourmod.spawnProp"
        Actor host = akEvent.actorRef

        ; Your mod owns what this means.
        ; Spawn a prop, play an effect, set a quest stage, update UI, etc.
    EndIf
EndFunction
```

Delivery should be async and non-reentrant so handlers can safely call back into OSF.

Custom actions are currently intended to be notifications, not blocking mechanics. They do not pause scene progress or report success/failure to the scene graph.

Anything that needs guaranteed ordering, blocking behavior, or reversible engine-state changes probably needs to be either:

- a built-in OSF mechanic; or
- implemented by an SFSE plugin with an explicit integration contract.

---

# Built-in mechanics

The exact list is still in flux, but likely built-in `osf.*` mechanics include:

- `osf.control.lock`
- `osf.control.release`
- `osf.fade.out`
- `osf.fade.in`
- `osf.equipment.hide`
- `osf.equipment.restore`
- `osf.weapon.sheathe`
- `osf.weapon.restore`
- `osf.sound.play`
- `osf.voice.play`
- `osf.camera.hold`
- `osf.camera.release`

Built-in mechanics should be reversible where possible.

They should also be feature-gated. If a hook cannot validate on the current game version, that feature should self-disable and report diagnostics rather than crash.

---

# Diagnostics and authoring support

Data-driven frameworks live or die by diagnostics.

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
- failed actor match and why;
- disabled feature because a native hook did not validate on this game version;
- scene rejected because a required capability is unavailable.

Ideally, authors should be able to iterate without restarting the game and without guessing why a pack or scene failed to load.

---

# Compatibility and migration

OSF is intended to provide a compatibility path for existing SAF/NAF-style playback content where practical.

That compatibility layer should be understood as a bridge, not a perfect clone.

Some SAF concepts may not have direct OSF equivalents, especially if they assume a playback-layer contract rather than a scene-lifecycle contract.

Known likely problem areas:

- absolute repositioning semantics;
- blend graph variables;
- selection buffers;
- phase/sequence callbacks;
- behaviors that assume SAF owns only playback rather than a full scene handle;
- any feature that does not map cleanly to a content-neutral scene runtime.

If you maintain SAF content, I would like feedback on which pieces actually need parity and which can be treated as legacy-only.

---

# Known constraints for v1

These are not necessarily permanent, but they shape the first version.

## Scenes do not resume after save/load

On a world-replacing load, active scenes stop and handles die.

Callers should assume scenes are gone across load and restart if needed.

OSF should clean up built-in state it owns where possible, but third-party custom effects must self-heal from their own state.

## One actor, one scene

An actor can only be in one OSF scene at a time.

Starting a scene with a busy actor should fail cleanly. The caller must stop the existing scene first or choose different actors.

## Custom actions are notifications

Custom `yourmod.*` actions are fire-and-forget events.

They do not currently:

- block scene progress;
- acknowledge success/failure;
- guarantee completion before the next stage;
- automatically clean up third-party state.

This keeps the scene runtime predictable. More complex integrations may need a native plugin or future registered-mechanic system.

## OSF does not execute arbitrary code from JSON

JSON can describe OSF mechanics and emit custom action events.

It should not be a scripting language and should not execute arbitrary code.

This boundary matters for stability, safety, and user trust.

---

# Adult wrapper and tag standardization

The OSF core is content-neutral, but LoversLab use cases need convenience.

A separate adult-facing layer should probably provide:

- simpler scene shorthand;
- default camera/control behavior;
- stripping/equipment policy;
- standard stage conventions;
- furniture and marker conventions;
- actor filtering helpers;
- user-facing policy settings;
- recommended adult tag vocabulary.

The tag vocabulary may be one of the most important parts.

Matchmaking only works well if packs and scenes use compatible tags. If every author invents different names for the same concepts, the matchmaker becomes much less useful.

Possible tag categories that may need standardization:

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

I am not saying OSF core should enforce all of that. The question is whether OSF or the adult wrapper should publish a recommended tag registry early so packs are interoperable.

---

# Specific questions for comment

## Questions for animation and scene authors

1. Is the simple scene shorthand enough for common linear scenes?

2. What metadata do animation packs need on day one?

3. What tags should be standardized early so matchmaking works across packs?

4. Are role filters like gender, race, and keywords enough, or do you need additional filters such as faction, relationship rank, level, equipped item, distance, voice type, body type, furniture, or location type?

5. Should scene authors be expected to manually author camera/control/fade/stripping actions, or should the wrapper handle those as defaults?

6. Do you prefer semantic role names like `dominant`, `receiver`, `speaker`, `listener`, or neutral names like `actorA`, `actorB`, `role0`, `role1`?

7. Should adult-specific semantics live entirely in tags, or should the adult wrapper have stronger typed fields for common concepts?

---

## Questions for quest and content modders

1. Would you rather start exact scenes by ID, or mostly ask the matchmaker for content by tags?

2. Should scenes expose their available navigation edges so a mod can wire them into dialogue or UI menus?

3. What information do you need before starting a scene: actor count, tags, duration, furniture requirements, role names, required features, author, priority?

4. Should the matchmaker return only the best match, or should callers be able to inspect a ranked list?

5. Should a caller be able to require authored scenes only, raw animations only, or either?

---

## Questions for integrator mods

1. Is an opaque `data` string enough for custom actions?

Example:

```jsonc
{
  "type": "yourmod.spawnProp",
  "data": "form=YourMod.esm|0x801;count=2"
}
```

Or do custom actions need structured JSON fields?

Example:

```jsonc
{
  "type": "yourmod.spawnProp",
  "form": "YourMod.esm|0x801",
  "count": 2,
  "attachTo": "hand"
}
```

2. Should OSF resolve form references for custom actions, or should the receiving mod parse and resolve them?

3. Do custom actions need to block scene progress, or is fire-and-forget notification enough?

4. Do custom actions need acknowledgement/failure reporting?

5. Should third-party effects be user-toggleable through OSF, or should each integration own its own settings?

6. Should custom verbs be able to declare soft dependencies so diagnostics can say “this scene expects YourMod, but it is not installed”?

---

## Questions for users and pack maintainers

1. Is it acceptable that scenes do not resume after save/load in v1?

2. Which compatibility behavior matters most for SAF/NAF migration?

3. What failure modes are most important to report clearly?

4. Should users control individual mechanics, such as “never allow equipment hiding,” or should control happen at a higher policy level, such as scene tags and content filters?

5. Should OSF or the adult wrapper provide a shared tag registry to reduce fragmentation?

---

# Design priorities

The priorities I am aiming for are:

1. **Native playback where native playback matters.**  
   Per-frame animation work should not be Papyrus-driven.

2. **Scenes as the unit of control.**  
   Playback is a component. The scene lifecycle is the framework.

3. **Data-first content.**  
   Adding animations and scenes should not require scripting.

4. **Content-neutral core.**  
   OSF should provide mechanisms, not decide what the content means.

5. **Opinionated wrappers where useful.**  
   Adult content needs shortcuts and conventions. Those should live above the reusable core.

6. **Extensibility without turning JSON into a scripting language.**  
   Custom actions should let mods integrate without making OSF execute arbitrary code.

7. **Deterministic cleanup.**  
   Anything OSF changes, OSF should be able to undo.

8. **Good diagnostics.**  
   Authors need clear reload errors. Users need clear missing-dependency and disabled-feature reporting.

9. **Migration where practical, not legacy lock-in.**  
   Compatibility is useful, but OSF should not be shaped entirely around old assumptions.

---

# Closing

The short version is:

OSF is not meant to be just another animation player. It is meant to be a native scene runtime for Starfield.

It should let animation authors ship data, scene authors compose staged interactions, quest mods request fitting content, integrators react through events, and users get a more reliable ecosystem with better cleanup and fewer one-off controllers.

The main thing I want feedback on now is not branding or hype. It is the contract:

- what the schema needs to express;
- what the core should own;
- what the adult wrapper should own;
- what custom integrations need;
- what compatibility actually matters;
- what guarantees are necessary for authors to build on this without fighting the framework later.