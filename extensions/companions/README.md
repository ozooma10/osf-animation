# OSF Companions prototype

A small, separate content addon for OSF Animation: talk to Sarah and choose
**Can I have a hug?** The interaction waits for dialogue to finish, plays a
2.8-second entrance, the corrected 13.33-second Embrace, then a 2.8-second release.
Clothing stays on. OSF owns the temporary input/camera/equipment state and restores
it when the scene ends. This addon does not change affinity or romance state.

Enable **OSF Companions Prototype** and **OSFCompanions.esm** in MO2 alongside
OSF Animation and its requirements. It adds four new records and overrides no
vanilla records. It does not require OSF Seduce or the animation browser.

For direct testing, stand within 1.8 metres of Sarah on level, open ground:

```text
cgf "OSFCompanions.HugSarah"
cgf "OSFCompanions.Stop"
cgf "OSFCompanions.Status"
```

The same checks apply to the console and dialogue entry points. Combat, dead or
unloaded actors, sitting/mounting/flying, very low gravity, different cells,
height differences above 20 cm, and existing scenes prevent the hug. Dialogue
has an eight-second timeout; the hug has a thirty-second watchdog. The stop
command only stops this addon's pending/active interaction. A world-replacing
load discards old runtime handles.

This first version is fitted to Sarah and a human player in ordinary clothing.
The entrance/release are procedural prototype motion, with a short alignment
snap at launch. Furniture/wall clearance, arbitrary actor proportions, bulky
spacesuits, slopes and zero-gravity hugs are not solved. No new voice recordings
are included; the dialogue choice directly starts the hug. The checks cover
distance and height, not a swept collision test. Use an open patch of floor.

## Build

From this repository:

```powershell
pwsh -NoProfile -File extensions/companions/Build.ps1
pwsh -NoProfile -File extensions/companions/Build.ps1 -Package
pwsh -NoProfile -File extensions/companions/Build.ps1 -DeployTo 'C:\Modding\Starfield\MO2\mods\OSF Companions Prototype'
```

The build regenerates the ESM, compiles the three Papyrus scripts, and checks
all six clips with the existing native OSF importer. Close the game before
deploying the ESM/scripts. No core DLL changes are needed for this addon.

`-RebuildAnimations` runs Blender 5.1 against the local, hash-pinned Contact v2
research inputs under `build/hug-animation-research`, plus the target rig and
existing `tools/skyrim-import` utilities. The authoring script is
`tools/build_hug.py`; its editable output is
`build/companion-hug/Sarah-hug.blend`. The input rig/research assets are local
prerequisites, not fetched automatically. Runtime files live under `dist`.

The transitions use IK-authored arm key poses interpolated in joint space,
foot targets for Sarah's short step, and matching endpoints at both stage joins.
Root translation/heading is rebased into scene placement so the actors finish
at their own capsule positions facing each other. The held hug retains the
accepted Contact v2 body/hand fit.

## Sources and validation

Embrace originates from TheRealRufGT's pair distributed with
[Sleep Intimate XOXO](https://www.dracotorre.com/mods/sleepintimatexb1/), converted
from Fallout 4 and corrected locally for Starfield. This is the requested local
functional prototype; distribution permissions have not been reviewed.

The independent ESM writer uses the record layouts in
[TES5Edit's Starfield definitions](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsSF1.pas).
The plugin passes xEdit's Check for Errors. Papyrus compilation, six native GLB
imports, matching stage joins, and the saved Blender preview have been checked.
The preview audit compares 220,578 bone matrices at keys and midpoints; transition
rotation changes remain below 6.1 degrees per frame at 30 fps.

In Starfield 1.16.244, the dialogue option was visibly present in Sarah's normal
conversation menu. Starting its actual SCEN record through the private driver
executed the INFO fragment and manager, played all three animation stages and
ended automatically after about nineteen seconds. Distance rejection was also
observed in game. The public `HugSarah` and `Stop` functions were exercised together:
the hug started, cancellation ended it, and no OSF scenes remained. Physical menu
selection remains a manual check: the game's
`RequestDialogueSelect` test API reported an empty choice list and did not select
the visible option. The direct console entry remains available for testing.

Private game evidence, logs and reports live under `build/companion-hug`.
The `OSFCompanionsQA.esm` driver and its cue scenes are private test assets and
are excluded from the prototype package.
