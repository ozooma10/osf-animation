# Experimental Skyrim human animation importer

Offline conversion through background Blender: Skyrim SE/AE human HKX clips and
their animation skeletons become Starfield human GLBs plus an OSF scene file.
Users do not need to operate Blender manually. This is a developer prototype,
not a general pack converter or an engine runtime dependency.

## Current result

Verified on 2026-10-15: **FB_LoversStand imports with `positive-affine-v1`** from
the real local SLAL pack `%USERPROFILE%/Downloads/FBSLALPACK v12.0SERev1`, module
`07-HumanLover`. Its generated JSON and FNIS mappings produce five stages and ten
Starfield GLBs, all accepted and sampled by the existing native importer. Animated
source scales (`0.8333333` to `1.3333511`) are transferred to explicit GLB scale
tracks. They are not normalized away. See the interpretation and bounds below.

All **66 tests pass in Blender 5.1.2**. Ordinary Python 3.14 runs 51 test entries
with the two Blender modules skipped. The original 48-test baseline also passed
before development changes. The original framework fixture still produces eight
byte-identical GLBs with the default `unit-v1` profile, with native and preview
checks passing.

The real scene's saved stage-3 preview passes **19,982** placed bone matrix checks
at GLB keys and interval midpoints (tolerance `2e-4`). An independent float64 NumPy
audit of all **718** serialized clip frames checks **40,208** mapped bone matrices
against the source affine interpretation. Maximum local/model relative residuals
are approximately `0.000297` / `0.000381`, below the explicit `0.0005` profile limit.
These checks establish offline skeleton transforms, not body contact or complete
in-game acceptance. A later, user-authorized test in the harness's private
`OSF Animation Testing` profile loaded the scene and played stages 1 through 4,
including manual advancement, without an obvious scale explosion. Stage 5 and
pause/resume/stop remain unverified because Computer Use was stopped. Normal
installed mods were unchanged; the test used private staged content and saves.

Local evidence is under `build/skyrim-import/`:

- `fb-lovers-stand/`: final ten-clip output, scene, native receipt, preview and audits.
- `hugging-pair-scale-regression/`: current eight-clip unit-profile regression output.
- `hugging-pair-verified/`: previous eight-clip baseline used for byte comparison.
- `fb-lovers-stand.plan.json`: resolved real SLAL plan.
- `fb-lovers-stand-scale-investigation.json`: source scale observations.
- `fb-lovers-stand-affine-investigation.json`: initial affine transfer measurements.
- `audit_serialized_scales.py`: independent audit; run in background Blender from the repo root.
- `fb-lovers-stand-preflight-verified/`: historical rejection under the strict unit profile.
- `fb-lover-screening.json`, `fb-pack-screening.json`: historical unit-profile screening.
- `scale-profile-tests-final.log`, `scale-profile-python-tests.log`: current test evidence.
- `source-rigs/provenance.json`: extracted skeletons and source archive hashes.
- `preflight-cli-verification.json`: verified preflight exit codes and no-bake behavior.
- `game-test-20261015-151118/`: private game-test staging, source hashes and launch logs.
  Stage 1-4 captures and receipts are in the sibling harness's `artifacts/` folders
  `20261015-192512-338-Capture`, `20261015-192541-513-Capture`,
  `20261015-192557-810-Capture` and `20261015-192615-422-Capture`.

Earlier unit-profile screening rejected nine two-actor scenes in `07-HumanLover`
for non-unit scales (one also had negative values). Screening 280 scenes in other
human modules found 245 with non-unit scales and 35 requiring animation objects,
stopping at the first incompatible clip per scene. Those results do not establish
compatibility with the new profile. Only `FB_LoversStand` has completed this new
real-pack conversion. Creature modules and the four three-actor scenes in
`07-HumanLover` were not screened; negative scales remain unsupported.

### Reconstructed baseline fixture

The inspected Skyrim MO2 instance is `C:\Modding\Skyrim\MO2`, profile `Default`.
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

The target is `build/skyrim-import/target-rig/skeleton.rig` (97 bones), freshly
extracted from the installed Starfield animation archive. The old
`ui/osf-studio/local/rigs/human/skeleton.rig` did not transfer. Its SHA-256 is
`de4a4d3c7e89aadd93dfc728160770e164d50765e41bcaa369e4943b90dbb209`.

Local inputs, provenance, plans, and generated assets live under the ignored
`build/skyrim-import/` directory. They are not included with the importer source.
The latest conversion's `report.json` records exact input/tool hashes and checks.
The current fixture regression is `build/skyrim-import/hugging-pair-scale-regression/`,
including its eight clips, scene, preview and native results.

## Dependencies and local fixture command

- Python 3.11+ for the CLI.
- Existing Blender installation; tested with Blender 5.1.2.
- Existing PyNifly `io_scene_nifly` directory containing
  `hkx/anim_skyrim.py` and `hkx/anim_fo4.py`. No addon installation or enabling is
  required: the worker imports those modules privately and records their hashes.
- Source animation HKX skeleton(s), a Starfield human `skeleton.rig`, and clips.
- Optionally, an existing `osf-import-test.exe` for native decoding/sampling.

Discovered tools on this PC: `C:/Python314/python.exe`,
`C:/Program Files/Blender Foundation/Blender 5.1/blender.exe`, and
`build/windows/x64/releasedbg/osf-import-test.exe`. Both games are under
`C:/Program Files (x86)/Steam/steamapps/common/`; their MO2 folders are under
`C:/Modding/Skyrim/MO2` and `C:/Modding/Starfield/MO2`.

No installed PyNifly decoder was found. A private, unmodified copy of only its
two decoder modules and license was downloaded from
[PyNifly commit 60201d25a70c3d514decae33cc8d91329ba2d7fb](https://github.com/BadDogSkyrim/PyNifly/tree/60201d25a70c3d514decae33cc8d91329ba2d7fb)
into `build/skyrim-import/deps/PyNifly-60201d25a70c3d514decae33cc8d91329ba2d7fb/`.
This is a private source dependency, not an installed/enabled Blender addon or
vendored project source. Its hashes are recorded in each report.

From the repository root, reproduce the reviewed local fixture into a **new**
output directory:

```powershell
python -B tools/skyrim-import/import_pack.py `
  --manifest build/skyrim-import/hugging-pair.plan.json `
  --target-rig build/skyrim-import/target-rig/skeleton.rig `
  --pynifly build/skyrim-import/deps/PyNifly-60201d25a70c3d514decae33cc8d91329ba2d7fb/io_scene_nifly `
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

### Review a plan and preflight the decoded clips

For modular FOMOD downloads, point `--pack-root` at the selected module's Data
root (for this pack, `07-HumanLover`), not an assumed installed MO2 layout.
Use `--plan-only NEW_FILE.json` in place of conversion options to resolve events,
check file presence and save the complete plan without starting Blender. It needs
the source skeleton paths but no target rig, decoder or Blender executable:

```powershell
python -B tools/skyrim-import/import_pack.py `
  --slal-json "$env:USERPROFILE/Downloads/FBSLALPACK v12.0SERev1/07-HumanLover/SLAnims/json/FunnyBiznessLover.json" `
  --pack-root "$env:USERPROFILE/Downloads/FBSLALPACK v12.0SERev1/07-HumanLover" `
  --scene FB_LoversStand `
  --skeleton build/skyrim-import/source-rigs/skeleton.hkx `
  --female-skeleton build/skyrim-import/source-rigs/skeleton_female.hkx `
  --scale-profile positive-affine-v1 `
  --plan-only build/skyrim-import/fb-lovers-stand-next.plan.json

python -B tools/skyrim-import/import_pack.py `
  --manifest build/skyrim-import/fb-lovers-stand-next.plan.json --preflight `
  --target-rig build/skyrim-import/target-rig/skeleton.rig `
  --pynifly build/skyrim-import/deps/PyNifly-60201d25a70c3d514decae33cc8d91329ba2d7fb/io_scene_nifly `
  --blender 'C:/Program Files/Blender Foundation/Blender 5.1/blender.exe' `
  --output build/skyrim-import/fb-lovers-stand-preflight-next
```

`--scale-profile` selects `unit-v1` (the default) or `positive-affine-v1`; it is
saved as `scale_profile` in the plan and reported in each receipt. A CLI override
takes precedence over a manifest's value. Unknown profiles fail explicitly.

`--preflight` decodes every clip in the selected scene, checks binding, duration
agreement, channel validity and the selected scale profile, and writes
`preflight.json`, `report.json`, `input-plan.json` and `blender.log`. Each clip
includes its hash, scale range, non-unit/nonuniform counts and up to eight
non-unit or invalid samples with bone/track/sample indices. A constant channel's
sample index is zero. The affine profile also computes every authored frame's
local/model TRS residual before any GLB is written. No `Data/` or preview is
produced. Exit **0** means compatible decoded input; exit **2**
means a completed compatibility rejection; exit **1** means setup/worker failure.
The modes `--list`, `--plan-only` and `--preflight` are mutually exclusive.
Existing output files/folders are never overwritten.

Ordinary conversion now runs this same full preflight before baking. A rejection
retains the reports in its diagnostic `.partial-*` folder and does not publish a
converted package. Compatible input still needs conversion, native decoding,
production registry and visual/in-game checks; preflight alone does not grant
any of those acceptance results.

Reproduce the completed real scene from its existing resolved plan into a new folder:

```powershell
python -B tools/skyrim-import/import_pack.py `
  --manifest build/skyrim-import/fb-lovers-stand.plan.json `
  --scale-profile positive-affine-v1 `
  --target-rig build/skyrim-import/target-rig/skeleton.rig `
  --pynifly build/skyrim-import/deps/PyNifly-60201d25a70c3d514decae33cc8d91329ba2d7fb/io_scene_nifly `
  --blender 'C:/Program Files/Blender Foundation/Blender 5.1/blender.exe' `
  --output build/skyrim-import/fb-lovers-stand-next `
  --native-validator build/windows/x64/releasedbg/osf-import-test.exe --save-blend
```

The same profile works directly with `--slal-json`, `--pack-root` and `--scene`;
that entry point also completed the ten-clip conversion during verification.

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

The worker decodes HKX tracks, applies model-space transforms relative to each
rig's bind pose, and bakes Starfield local transforms. Extra spine/neck joints do
not receive duplicate rotations or stretch. Target bind translations remain fixed
except for COM; the affine profile allows inherited scale to change effective
limb lengths. The default unit profile retains the original rotation-only policy. A common
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

### Positive scale interpretation

The investigated source clips contain animated, mostly isotropic stretch and
partial parent/child scale compensation. Compensation does not always multiply
back to one. There is also a small anisotropic scale on `NPC R Finger40 [RF40]`,
with an axis spread around `0.001298`. Resetting all scales changes source model
joint origins by up to approximately `0.206` metres at the common scene unit
conversion. These values are treated as authored transform data.

`positive-affine-v1` interprets source hierarchy composition as `parent @ T*R*S`.
For mapped source bone `s` and target bone `i`, with 3x3 coordinate basis `C`, it
forms the desired target model linear transform:

```text
desired[i] = C * sourceModel[s] * inverse(sourceBindModel[s])
               * transpose(C) * targetBindModel[i]
targetLocal[i] = inverse(actualTargetParentModel[i]) * desired[i]
```

Only the 3x3 linear parts enter this equation. Bind-relative transfer includes
unmapped source ancestors and cancels reference-pose scale. Deriving each local
from the actual target parent preserves parent/child compensation and avoids
applying one source scale twice to extra target spine/neck joints. Source pelvis
motion still goes to COM; target Root stays at its rest origin. Target local bind
translations stay intact, while explicit local scale tracks carry animated stretch.

GLB local TRS cannot represent general shear. The profile decomposes and
reconstructs each local transform, then compares both local and accumulated model
linear matrices to their affine references. The metric is
`max(abs(actual-reference)) / max(abs(reference))` over each 3x3 matrix. Any
residual above **`5e-4` (0.05%)** rejects the clip; the limit is fixed, not relaxed
for this pack. Source/local target scale components and cumulative model column
lengths must be positive and within `[1e-4, 100]`; reflections, singular scales
and excessive shear reject explicitly. Receipts record the limits, worst bones
and frames, scale ranges and non-unit target sample counts.

This is an explicit offline affine interpretation with bounded TRS approximation.
Exact Skyrim runtime evaluation of nonuniform scales has not been established.
The residual checks cover every authored key, not every continuous time sample;
the saved preview additionally checks GLB interval midpoints for its selected stage.
The existing native GLTF importer reads scale tracks and the playback math builds
local affine matrices from translation, rotation and scale. No native change was
needed, and this does not substitute for future in-game deformation review.

Outputs:

- `Data/OSF/<scene-id>.osf.json`: ordinary OSF stages, roles and clip placements.
  Apparel hiding is disabled for the prototype; normal OSF player locking applies.
- `Data/OSF/Imported/<scene-id>/*.glb`: one clip per stage and participant.
- `report.json`, `input-plan.json`, `blender.log`: provenance, omissions and checks.
- `preflight.json`: decoded input compatibility for every stage and participant.
- For the reviewed `fb-lovers-stand/` artifact, `scale-audit.json` and
  `verification.json`: independent serialized-transform and receipt/regression checks.
- Optional `preview.blend`: skeleton-only playback of the middle stage, with
  actor placements, animated scale and linear keys. Body meshes are not needed for conversion.
- With `--save-blend`, `preview-audit.json`: the saved file is reopened with
  scripts disabled and compared to the serialized GLBs and scene placements at
  every key and midpoint. A mismatch rejects conversion. Blender quaternion
  keys preserve sign continuity across 180-degree rotations. Between other
  sample fractions, Blender's component-linear quaternion interpolation can
  differ from glTF spherical interpolation; the audit is not a full continuous
  interpolation equivalence proof.

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

The selected real SLAL scene and reconstructed fixture both pass offline native
decoding/sampling. The real scene also loaded in the production registry and
played stages 1 through 4 in the isolated game test described above. This was
partial manual verification, not the harness's full automated smoke suite.
Stage 5, pause/resume/stop, detailed body deformation/contact, foot sliding and
timing still need acceptance. Conversion itself remains offline and does not
deploy to MO2 or launch the game.

The prototype rejects additive clips. Non-unit animated scales require the
explicit affine profile and must satisfy its bounds. The initial Leito kissing
candidate contained negative scale compensation; that remains unsupported and
is not silently normalized. Facial animation, physics, props, sounds, framework scripts,
external HKX extracted motion, mismatched-duration resampling, and automatic
contact correction are not converted. No source/game assets are redistributed.
