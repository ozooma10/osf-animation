# OSF Vanilla Pairs - Audition

Local two-actor test content for Hunter/Walter, Constellation survivor/victim,
and the Key confrontation. Ten scenes, using the installed base-game AF files.
Requires the OSF Animation engine with the 2026-10-22 placement-yaw correction
described below. No new Papyrus, ESP, copied game animation, or DLC is required.

## Audition in game

1. Enable **OSF Animation**, **OSF Animation Browser**, and its required **OSF UI**
   and **OSF Settings** hosts in your intended MO2 profile.
2. Launch through MO2. Open the animation browser with your configured browser
   hotkey. For an already-running session after installation, reload content from
   the browser Imports view or `OSF.ReloadPacks()`.
3. Browse **Scenes**, turn on **SHOW HIDDEN**, and search for **OSF Vanilla Pairs - Audition**.
   The entries are deliberately unlisted so random tag matchmaking cannot pick
   temporary audition scenes.
4. Select two human actors in the displayed role order. The named roles are labels;
   you do not need the original quest NPCs. Survivor/victim take genders are also
   labels, so you can compare every take with the same actors.
5. Use a clear, level area with room for a body to fall. Start the desired scene.
   Use the browser scene controls to advance, align, change speed, or stop. Use
   the scene orbit camera to inspect from several angles.

The source pack lives at `Data/OSF/Auditions/VanillaPairs/`. Local deployment adds
only its three JSON files under the enabled `OSF Animation` MO2 mod. The standalone
ZIP instead installs as its own MO2 content mod; use one installation at a time to
avoid duplicate scene IDs. To remove the local deployment, remove those three
JSON files and reload content or restart.

## What each scene does

| Family | Scene | Behavior |
|---|---|---|
| Hunter/Walter | Hold > Next: bash | Loops the 6.767 s hold until you advance, then plays the 5 s bash/fall/recovery once and ends. |
| Hunter/Walter | Hold loop | Continuous shared hold; stop when done. |
| Hunter/Walter | Bash once | Plays the action immediately, once. |
| Survivor/victim | Male / Female takes | Starts once (8.967 s), then holds the 4.833 s supportive idle. Advance to audition each reaction, returning to a looping idle between reactions. |
| Survivor/victim | Female / Female takes | Same structure, using the corresponding authored tracks. |
| Survivor/victim | Male / Male takes | Same structure, using the corresponding authored tracks. |
| Survivor/victim | Female / Male takes | Same structure; this source combination has four matching reactions rather than five. |
| The Key | Full confrontation once | Plays both 43 s tracks on one clock, then ends. Shove reach near 20.37 s; wrist catch near 22.10 s. |
| The Key | Frozen contact checkpoints | Freezes both actors at source frames 611 (shove reach), 663 (wrist catch), and 700 (fall). Advance between checkpoints. |
| The Key | Frozen start for alignment | Holds source frame zero until stopped or advanced. |

The four survivor/victim scene names list **survivor first, victim second**.
The last support idle can be stopped, or advanced to end the scene. A frozen Key
checkpoint is an inspection pose, not a trimmed moving clip.

## Placement and presentation

- Actors use the original furniture interaction markers, rebased so the first
  role stays at the scene anchor. The initial pack incorrectly used zero offsets
  for both actors; that caused the overlapping Key checkpoint shown in game.
  A second in-game check exposed an engine yaw mismatch: placement rotated one
  way while the rendered actor basis rotated the other. The engine correction
  and this pack's revised headings must be installed together. Restart the game
  after updating the engine, then start the scene again.
- Apparel stays visible. Held items are cleared for the scene through OSF's owned
  equipment mechanism. Player input lock and scene orbit use normal OSF cleanup;
  all scene-control capabilities remain available.
- The pack plays **body animation only**. It does not replay the vanilla AFX
  weapon props, gunshots, damage/death/ragdoll events, facial dialogue, or quest
  scripts. The Key will mime its weapons. No damage or kill action is authored.
- The survivor/victim source contains hand-IK metadata. Exact hand contact on
  different body proportions is an open visual check.
- Loops are limited to the identified idles. Bash, reactions and the full Key
  sequence play once. No fades hide the beginning of an audition.

### Placement provenance

| Family / second role | X | Y | Z | Heading |
|---|---:|---:|---:|---:|
| Hunter/Walter / Walter | 0 | 0.72 | 0 | 0 degrees |
| Survivor/victim / Victim | 0.0821 | 1.1867 | 0 | 90 degrees |
| The Key / Reaver | -0.390767 | -0.878807 | 0 | 15 degrees |

The first role always has zero offset. Values are OSF local units and degrees;
the source clips retain their animated root/COM movement.

These come from `Starfield.esm` furniture records `0038A70D`
(`Scenes_MQ204_HunterWalter`), `0020FFAD` / `00212FD8`
(`Scenes_MQ204A_030_FemaleVictim` / `MaleVictim`), and `000E6A6A`
(`Scenes_DialogueCFTheKey_EstablishingScene`). Their `SNAM` arrays contain XYZ
and marker rotation in radians; `NNAM` supplies the role names. The Key's source
order is Naeva, Reaver, Marauder, so matching by name is essential.
The field layout is documented in
[xEdit's Starfield definitions](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsSF1.pas#L6626-L6637).

`placements.py` records the marker data and retains the relative marker heading:
Starfield's row-vector root matrix renders an active `Rz(-heading)`. The position
differences are rotated into the first role's frame with the inverse matrix.
The earlier offline preview incorrectly treated that root matrix as column-vector
`Rz(+heading)` and missed the engine's opposing placement rotation.

A read-only live capture at the Key's frame 614 and heading 2.400000095 radians
confirmed the convention: decoded COM, chest, head, and wrist positions agree with
the engine's composed world positions within 0.36 mm using `Rz(-heading)`, versus
17-71 cm errors using `Rz(+heading)`. The native placement regression now includes
the captured COM local/world result, rather than relying only on assumed signs.

The first two freeze times were also sound-cue starts, not hand-contact peaks.
Exact-frame decoded poses support frame 611 for the shove reach and frame 663 for
the wrist catch (Marauder left index joint is about 4.3 cm from Reaver's right wrist).
The full motion and its original AFX cue timings are unchanged.

The corrected build still needs in-game visual acceptance, especially for different
body proportions, clothing, and hand IK. These are source-derived placements,
not a claim of pixel-perfect contact. Existing manual alignment adjustments, if
any, still add to the authored placements and should be reset for a clean audition.

## Rebuild and validate

From the repository root:

```powershell
python experiments/vanilla-pairs/generate.py
python experiments/vanilla-pairs/verify.py
```

The verifier checks the limited scene vocabulary used here, cast sizes, timing,
unique IDs, the four original furniture records and their role placements, actual
archive paths/frame counts, and native import/sample results
for every referenced clip. It writes evidence and a standalone ZIP under
`build/vanilla-pairs-audition/`. It does not launch the game, compile code, or deploy.
This is structural/asset validation; live content import and contact alignment
remain part of the user's audition.

This experiment is excluded from the normal engine build and release archive.
