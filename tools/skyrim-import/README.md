# Experimental Skyrim human animation importer

Offline conversion through background Blender: Skyrim SE/AE human HKX clips and
their animation skeletons become Starfield human GLBs plus an OSF scene file.
Users do not need to operate Blender manually. This is a developer prototype,
not a general pack converter or an engine runtime dependency.

## Current result

The inspected Skyrim MO2 instance was `C:\Modding\Skyrim\MO2`, profile `Default`.
It contained no installed SLAL pack. The disabled `SexLabFrameworkAE_v` mod did
contain a usable built-in four-stage, two-participant fixture:
`HuggingSex_A1_S1.hkx` through `HuggingSex_A2_S4.hkx`. Its registration and offsets
were read from `sslAnimationDefaults.psc` (`SexLabHuggingSex`, lines 862–883) and
`FNIS_SexLab_List.txt`. No source Papyrus or pack Python scripts were executed.
This is a technical motion fixture, not an SLAL-pack acceptance result.

Source skeletons were extracted from the installed Skyrim SE
`Data/Skyrim - Animations.bsa`:

| Role | Archive resource | SHA-256 |
| --- | --- | --- |
| Male | `meshes/actors/character/character assets/skeleton.hkx` | `2a0bfeb030b9f1ff0c31d0d28de470ea74ddbafb199edb78f1735589f0b97d21` |
| Female | `meshes/actors/character/character assets female/skeleton_female.hkx` | `9713ac3645553376e605dcb07aa95aefb64963ce3cebce953540acdaa08c3437` |

Both have the verified 99-bone vanilla layout. The fixture's 97 unnamed tracks
bind to its first 97 bones; the omitted suffix consists of two camera nodes.
Implicit binding is accepted only for that exact ordered layout and matching
skeleton name. This does not establish compatibility with XPMSSE or other rigs.

The target is `ui/osf-studio/local/rigs/human/skeleton.rig` (97 bones), verified
against the installed Starfield animation archive. Its SHA-256 is
`de4a4d3c7e89aadd93dfc728160770e164d50765e41bcaa369e4943b90dbb209`.

Local inputs, provenance, plans, and generated assets live under the ignored
`build/skyrim-import/` directory. They are not included with the importer source.
The latest conversion's `report.json` records exact input/tool hashes and checks.
The completed local fixture is `build/skyrim-import/hugging-pair/`, including
its eight clips, scene, preview, native results and importer test log.

## Dependencies and local fixture command

- Python 3.11+ for the CLI.
- Existing Blender installation; tested with Blender 5.1.2.
- Existing PyNifly `io_scene_nifly` directory containing
  `hkx/anim_skyrim.py` and `hkx/anim_fo4.py`. No addon installation or enabling is
  required: the worker imports those modules privately and records their hashes.
- Source animation HKX skeleton(s), a Starfield human `skeleton.rig`, and clips.
- Optionally, an existing `osf-import-test.exe` for native decoding/sampling.

From the repository root, reproduce the reviewed local fixture into a **new**
output directory:

```powershell
python -B tools/skyrim-import/import_pack.py `
  --manifest build/skyrim-import/hugging-pair.plan.json `
  --target-rig ui/osf-studio/local/rigs/human/skeleton.rig `
  --pynifly "$env:APPDATA/Blender Foundation/Blender/5.1/scripts/addons/io_scene_nifly" `
  --blender 'C:/Program Files/Blender Foundation/Blender 5.1/blender.exe' `
  --output build/skyrim-import/hugging-pair-next `
  --native-validator build/windows/x64/releasedbg/osf-import-test.exe `
  --save-blend
```

Blender runs with factory settings and automatic script execution disabled.
The importer writes into a temporary sibling directory and publishes the output
only after checks pass. Failed work/logs are retained for diagnosis; existing
output directories are never overwritten. It does not install content into MO2,
launch either game, or change installed addons.

## SLAL input

Use an extracted mod folder with its generated `SLAnims/json/*.json`, FNIS list,
and HKX files. Archive extraction and automatic MO2 conflict resolution are not
implemented. List metadata first; this does not need Blender or skeletons:

```powershell
python -B tools/skyrim-import/import_pack.py `
  --slal-json 'C:/path/to/pack/SLAnims/json/pack.json' --list
```

The list includes unsupported entries. A `candidate` means its metadata fits the
prototype; file resolution and animation validation still happen on conversion.
Select one human scene:

```powershell
python -B tools/skyrim-import/import_pack.py `
  --slal-json 'C:/path/to/pack/SLAnims/json/pack.json' `
  --pack-root 'C:/path/to/pack' --scene 'exact_scene_id' `
  --skeleton 'C:/path/to/skeleton.hkx' `
  --female-skeleton 'C:/path/to/skeleton_female.hkx' `
  --target-rig 'C:/path/to/Starfield/skeleton.rig' `
  --pynifly 'C:/path/to/io_scene_nifly' `
  --blender 'C:/path/to/blender.exe' `
  --output 'C:/path/to/new-output' --save-blend
```

The parser uses generated JSON and FNIS event mappings, preserves role order,
stage timing, tags and actor offsets, and reports omitted behavior. Prop scenes,
missing files, ambiguous mappings, path escapes, and incompatible skeleton
bindings fail explicitly. OStim-specific metadata is not implemented.

For a framework scene without SLAL JSON, `--manifest` accepts a reviewed plan:

```json
{
  "schema": 1,
  "id": "local.motion.test",
  "name": "Local motion test",
  "pack": "Local conversions",
  "source": { "offset_units": "skyrim" },
  "roles": [{ "name": "actor1", "gender": "female", "skeleton": "C:/path/skeleton_female.hkx" }],
  "stages": [{ "clips": ["C:/path/clip.hkx"], "loops": 0,
               "offsets": [{ "x": 0, "y": 0, "z": 0, "heading": 0 }] }]
}
```

Offsets use Skyrim units: X=side, Y=forward, Z=up; headings use SexLab degrees.
A positive `timer` retains an authored timed stage; `loops:0` keeps a manual loop.
Every stage must supply one clip per role, with equal participant durations and
frame counts. Use absolute input paths for reproducibility.

## Conversion and output

The worker decodes HKX tracks, applies model-space rotations relative to each
rig's bind pose, and bakes Starfield local transforms. Extra spine/neck joints do
not receive duplicate rotations. Target limb lengths remain fixed. A common
scale, estimated from the first participant's reference leg length, converts
pelvis motion and participant offsets for the whole scene. Both profiles keep
world Z vertical; metadata headings are negated to match OSF's convention.
This is an initial proportion heuristic, not a contact solver.

The GLB writer preserves OSF's direct local coordinate convention without a
standard Blender glTF exporter's axis conversion. Source pelvis movement is
carried by COM; Root remains at its rest origin. Unmapped auxiliary bones are
preserved by the scene; necessary ancestors of mapped bones remain driven.
`decoder.py` handles shared boundary frames in multi-block HKX animations without
editing or vendoring the installed PyNifly source.

Outputs:

- `Data/OSF/<scene-id>.osf.json`: ordinary OSF stages, roles and clip placements.
  Apparel hiding is disabled for the prototype; normal OSF player locking applies.
- `Data/OSF/Imported/<scene-id>/*.glb`: one clip per stage and participant.
- `report.json`, `input-plan.json`, `blender.log`: provenance, omissions and checks.
- Optional `preview.blend`: skeleton-only playback of the middle stage, with
  actor placements and linear keys. Body meshes are not needed for conversion.

The reviewed framework fixture retains raw offsets `0/-100` rather than applying
SexLab's shared `+50` centering shift. Pair separation is unchanged, but its anchor
origin differs. All four stages remain manual loops, matching the absence of
authored timers. Framework runtime effects are recorded as omitted.

## Validation and remaining work

Run synthetic tests with Blender to include the retargeting tests:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.1/blender.exe' `
  --background --factory-startup --disable-autoexec --python-exit-code 1 `
  --python tools/skyrim-import/run_tests.py
```

`python -B tools/skyrim-import/run_tests.py` runs parser, rig, decoder and GLB tests
but skips Blender math tests. Tests use generated fixtures, not game assets.

The local four-stage conversion produced eight GLBs accepted and sampled by the
existing native importer. That verifies decoding, not an in-game scene launch.
Production registry loading, body deformation/contact, foot sliding and timing
still need in-game acceptance. A real SLAL pack remains an integration gap.

The prototype rejects additive clips and non-unit animated source scales. The
initial Leito kissing candidate contained authored scale compensation, including
negative values; those clips need a separate interpretation profile and are not
silently normalized. Facial animation, physics, props, sounds, framework scripts,
external HKX extracted motion, mismatched-duration resampling, and automatic
contact correction are not converted. No source/game assets are redistributed.
