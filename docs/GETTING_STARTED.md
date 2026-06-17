# Getting started: shipping an OSF animation pack

This is the path from "I have animations" to "they play in-game", with zero C++ and
zero plugin records. A pack is a folder of files — JSON metadata and GLB animations —
that OSF Animation discovers at game start.

OSF Animation is the OSF **engine**, and a pack here is just animation + alignment. Scene
policy — scheduled voice, undress/redress, fades, camera/control, callbacks — is authored
separately in `*.scene.json` **scene files** (the scene runtime), which reference your
pack's animation ids. So a pack stays simple.

## What you need

- **OSF Animation** installed (SFSE plugin; check
  `Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log` shows it loading).
- **Animations as GLB** in NAF format — bones-only skeletal animation over the Starfield
  skeleton (no morphs). Gzip-compressed GLBs and NAF's spec-invalid accessor min/max are
  handled transparently. Sanity-check a file offline with the `osf-import-test` harness
  before going in-game.
- Multi-actor scenes are **separate synchronized clips of equal length**, one per
  actor, authored co-located around a shared origin (the SexLab convention — OSF's
  anchor model is the same).

## Minimal pack

Create `Data/OSF/MyPack.json` (in MO2: a mod folder containing `OSF\MyPack.json`):

```jsonc
{
  "schema": 2,
  "name": "My Pack",
  "animations": [{
    "id": "myname.mypack.embrace",      // namespace it: author.pack.name
    "tags": ["paired", "standing"],
    "actors": [                         // one entry per slot (role) — metadata only
      {},
      {}
    ],
    "stages": [{                        // the timeline; one clip per actor, in order
      "clips": [
        "OSF/MyPack/embrace_a.glb",
        "OSF/MyPack/embrace_b.glb"
      ]
    }]
  }]
}
```

GLB paths are relative to `Data`. Put the GLBs wherever you like under `Data`
(`Data/OSF/MyPack/` keeps the pack self-contained; `Data/OSF/Animations/` is the
convention for shared/standalone GLBs).

Full schema — stages, timers, loop counts, gender slots, alignment offsets:
**[PACK_SCHEMA.md](PACK_SCHEMA.md)**.

## The dev loop

1. Launch via SFSE. Your pack is loaded at the main menu; the log names every pack and
   every skipped entry **with the reason** — read it first when something is silent.
2. Play it from the console on two NPCs you're looking at (the OSFTest helper script
   ships with OSF Animation and wraps the array-taking natives for `cgf`):
   `cgf "OSFTest.Defined" "myname.mypack.embrace" <refA> <refB>`
3. Edit the JSON, then `cgf "OSFTest.Reload"` (or `OSF.ReloadPacks()` from a script) and
   replay — no game restart. Edited GLBs are also re-read (the clip cache clears on
   reload).

## Aligning actors (the part you'll iterate on)

Offsets in the JSON are meters + heading in degrees, per actor, with optional per-stage
overrides (the `{ "file": ..., "offset": ... }` clip form). Edit, `OSF.ReloadPacks()`,
replay. Judge alignment **visually**: the rendered skeletons are pinned to the scene
anchor, while the actors' physics capsules legitimately settle ~0.3 m off — console
position readouts lie to you here.

## Voice, undress, and scene policy

Not part of a *pack*. Scene policy — scheduled voice, undress/redress, fades, camera/control,
and scene/cue callbacks — is authored in `*.scene.json` **scene files** that reference your
pack's animation ids: graphs of nodes with `cue` and `action` tracks (see
[SCENE_DESIGN.md](SCENE_DESIGN.md) §1.3). A pack that only needs mechanical playback needs none
of it. Specific adult content + orchestration ships in the **OSF Seduce** content mod.

## Troubleshooting, in log lines

| Symptom | Look for |
|---|---|
| animation id unknown in-game | "pack skipped"/"entry skipped" line naming your file + reason |
| entry skipped: "stage has N clip(s) but the scene has M actor(s)" | each `stages[].clips` needs exactly one clip per `actors[]` entry, in actor order |
| entry skipped: "actor-major layout removed in schema 2" | a pre-2.0 pack — move `actors[].stages[]` clips into top-level `stages[].clips[]` (see PACK_SCHEMA.md) |
| someone else's pack wins your id | duplicate-id line naming both files; namespace your ids |
| file plays but actor frozen/contorted | the GLB isn't a Starfield-skeleton NAF-format clip; run it through `osf-import-test` |

## For quest/consumer-mod authors

See **[API.md](API.md)** and the documented natives in `Scripts/Source/OSF.psc`.
