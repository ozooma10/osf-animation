# OSF — Consumer Guide

OSF Animation plays GLTF/GLB animations on Starfield actors and runs mechanical
multi-actor scenes. It is the **content-neutral playback core** — from a quest or content
mod you drive it in Papyrus through global natives on the **`OSF`** script (`OSF.Foo(...)`),
also callable from the console as `cgf "OSF.Foo" ...` for testing.

## Two layers — pick by intent

- **Primitives** (`Play`, `Stop`, `Sync`, `SetAnchor`, `SetSpeed`, `PlaySequence`, …)
  just move bones. The actor is **not** teleported and the world is untouched. *You*
  position the actors.
- **Scenes** (`StartScene`, `StartSceneByTags`, `StartSceneFiles`, …) are **mechanical
  productions**: OSF co-locates and anchors the actors at the pack's offsets, runs the
  pack's stages (timer / loop auto-advance), frame-locks their clocks, and pins the
  rendered skeletons. That is the whole contract — no undress, no voice, no camera, no
  fade.

> **Rule of thumb:** one actor, bones only, world unchanged → a **primitive**.
> Anything that coordinates actors *or* anchors them → a **scene**.

## Always gate on availability

OSF is an optional dependency — never assume it's installed:

```papyrus
If !OSF.IsReady()
    Return    ; OSF absent or failed to load — degrade gracefully
EndIf
```

## 30-second example

```papyrus
; a mechanical two-actor scene from an installed pack
Actor[] pair = new Actor[2]
pair[0] = bottomActor    ; fills the pack's actor slot 0
pair[1] = topActor
OSF.StartScene(pair, "author.pack.bridge")
; ... later ...
OSF.StopScene(pair[0])   ; any participant identifies the scene
```

## Where things live

| Doc | For |
|---|---|
| **[Cookbook](cookbook.md)** | how to do each task (start here) |
| `Scripts/Source/OSF.psc` | canonical per-native reference — every native documented at its declaration |
| [API.md](../API.md) | integration notes + the semver/stability contract |
| [ANCHORING.md](../ANCHORING.md) | the anchor / `rootMode` alignment contract |
| [GETTING_STARTED.md](../GETTING_STARTED.md) + [PACK_SCHEMA.md](../PACK_SCHEMA.md) | authoring animation **packs** (JSON) — not Papyrus |

## Beyond the core — OSF Intimacy

Undress/redress, scheduled voice, camera/control takeover, fade choreography, and
scene/cue callbacks are **not** in this core — they are the **OSF Intimacy** scene engine,
which builds on it. If your mod needs that policy layer, build on OSF Intimacy; if you need
reliable native playback (machinima, dance, NPC vignettes, custom scene logic), build on
the core directly. Existing SAF mods' playback/sync/scene calls run unchanged via the bundled SAF shim (a few advanced SAF-only entry points — phase callbacks, crosshair selection, blend-graph variables — have no core equivalent and are inert).
