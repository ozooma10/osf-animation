# OSF content pack schema (v2)

This is the canonical reference for every JSON animation pack OSF Animation loads from
`Data/OSF/`. It is the document the parser (`src/Registry/PackRegistry.cpp`) is written
against — **if parsing behavior changes, this file changes in the same commit.**

OSF Animation is the **content-neutral core**: it parses the mechanical structure of a
pack (ids, tags, gender slots, stages, clips, alignment offsets, timer/loop advance) and
nothing else. **Content fields — undress/scene equipment, scheduled voice, stage
intensity/peak, timed cues, sound pools — are ignored by the core** and are the domain of
the **OSF Intimacy** scene engine. They are documented here only as "carried, not read":
a pack may include them for OSF Intimacy and still loads cleanly on the bare core.

OSF scans `Data/OSF/**/*.json` recursively at game start and on `OSF.ReloadPacks()`.
A file's name decides what it is:

| Filename | Meaning |
|---|---|
| `*.scene.json` | OSF Intimacy scene definition (references anim ids) — **skipped by the core** |
| `*.voice.json` | OSF Intimacy voice set — **skipped by the core** |
| `*.dialogue.json` | OSF Intimacy dialogue manifest — **skipped by the core** |
| `*.settings.json` / `settings.json` | settings files, never parsed as a pack |
| any other `*.json` | an animation pack |

General rules:

- **Unknown fields are ignored** (forward compatible — this is exactly why a pack can
  carry OSF Intimacy content fields without breaking on the bare core).
- **`//` comments are tolerated** when reading.
- **Bad entries are skipped, never fatal.** Every skip is logged with the file name and
  reason in `Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log`.
- **Duplicate IDs: first loaded wins** (files sort case-insensitively by filename).
  Duplicates are logged loudly with the winning file named. Namespace your IDs
  (`author.pack.name`) to stay collision-free.
- **Units:** positions are **meters** (Starfield world units), headings/rotations are
  **degrees** in JSON (converted to radians internally). Time is seconds.

## Animation packs

A pack is **stage-major**: a list of `actors` (stage-invariant roles) and a `stages`
timeline, where each stage carries its timing plus one `clip` per actor.

```jsonc
{
  "schema": 2,                       // optional (absent = 1); a pack declaring a version newer
                                     //   than this build understands is skipped loudly. The
                                     //   stage-major layout below is schema 2 — declare it.
  "name": "Pack Name",
  "animations": [{
    "id": "author.pack.bridge",      // global lookup key, case-insensitive, required
    "name": "Bridge",                // display name, optional
    "tags": ["paired", "test"],      // optional; queried by OSF.FindScenes / OSF.StartSceneByTags
    "actors": [{                     // one entry per participant (slot), >= 1. STAGE-INVARIANT
                                     //   metadata ONLY — the clips live in stages[].clips below.
      "gender": "any",               // optional slot gender: "male"/"female"/"any";
                                     //   used by StartSceneByTags slot matchmaking
      "offset": {"x":0,"y":0,"z":0,"heading":0}   // optional DEFAULT placement for all stages;
    }],                              //   meters, heading DEGREES (a clip offset overrides it)
    "stages": [{                     // the timeline; >= 1 stage, played in order. After the LAST
                                     //   timed/loop-counted stage the scene ends on its own.
      "timer": 19.0,                 // seconds; 0/absent = no timed auto-advance
      "loops": 2,                    // completed clip loops; 0/absent = no loop auto-advance
                                     //   (timer 0 AND loops 0 = hold until SetSceneStage/StopScene)
      "clips": [                     // REQUIRED: one clip per actor, in actor order (length MUST
        "NAF/a01.glb",               //   equal actors[]). A bare string is just the file;
        {"file": "NAF/b01.glb",      //   an object is { file, offset } where offset (meters,
         "offset": {"y":1,"heading":180}}  //   heading DEGREES) overrides this actor's default
      ]                              //   offset for THIS stage.
    }]
  }]
}
```

That is the whole schema the core reads. Tags + gender slots drive matchmaking
(`StartSceneByTags`); the per-stage `clips` and `offset`s drive anchored playback; `timer`
and `loops` drive stage auto-advance.

### Scene policy lives in OSF Intimacy, not here

This pack is **pure animation** — clips, placement, gender slots, tags, staging. Scene
**policy** (undress/scene equipment, scheduled voice, intensity/peak, cues) belongs in a
separate OSF Intimacy **scene file** (`*.scene.json`) that references these animation ids; see
**docs/INTIMACY_SEAM.md**. The core ignores unknown fields, so a pack that *carries* legacy
policy keys (`sounds`/`sceneEquipment`, actor `undress`/`sceneEquip`/`voice`/`voiceSet`, stage
`intensity`/`peak`/`cues`) still loads — but the recommended home is the scene file, and the
core never reads them.

### Migrating pre-2.0 packs (actor-major → stage-major)

Schema 1 nested a `stages[]` array **inside each actor** (`actors[].stages[].file`) and kept
timing in a separate top-level `stages[]` correlated only by index. That layout was **removed**:
a pack whose `actors[]` entries still carry a `stages` (or `file`) key is skipped with a
migration hint in the log. To convert an entry:

- Strip the per-actor `stages[]` out of every actor; leave only the stage-invariant metadata
  (`gender`/`offset`, plus any OSF Intimacy content fields).
- For each stage index, gather that index's clip from every actor into the top-level
  `stages[N].clips[]` (actor order); move that stage's `timer`/`loops` alongside the clips.
  Bump `"schema"` to `2`.

A per-stage actor offset that lived on the old `actors[].stages[].offset` becomes the clip
object form `{ "file": "...", "offset": {...} }`.

## Alignment offsets

Offsets are **hand-authored in the JSON** (meters + heading in degrees), per actor, with
optional per-stage overrides via the clip object form. Iterate by editing the JSON and
`OSF.ReloadPacks()` (no game restart). Judge alignment **visually**: the rendered
skeletons are pinned to the scene anchor, while the actors' physics capsules legitimately
settle ~0.3 m off — console position readouts lie to you here. (In-game offset-nudge
hotkeys are not part of the core.)

## File layout convention

GLB clip paths are relative to `Data` and may live anywhere, but the shipped convention is
`Data/OSF/Animations/` for GLBs. Self-contained packs may instead nest everything under
`Data/OSF/<PackName>/`.

## Settings files (`*.settings.json` / `settings.json`)

Not animation packs and not read by the core (the OSF layers that own persistent settings
read them). The pack scanner skips them so they are never misparsed.

## Versioning policy

`schema` is bumped only on changes that would make an OLDER parser misread a pack —
never for additive fields (unknown fields are ignored by design). A pack declaring a
newer schema than the running build understands is skipped with a loud log line telling
the user to update OSF Animation.

**v2** introduced the stage-major layout (top-level `stages[].clips[]`) and removed the v1
actor-major layout (`actors[].stages[]`). Declaring `"schema": 2` means an older (v1) build
skips the pack cleanly with "update OSF Animation" instead of misreading it; an unmigrated v1
pack on a v2 build is rejected with a migration hint (see "Migrating pre-2.0 packs").
