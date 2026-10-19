# Upgrading to OSF Animation 1.5.0

Version 1.5.0 separates the engine from the optional in-game Browser and moves
settings, hotkeys, and the launcher into OSF Settings. Use the two matching 1.5.0
packages when you want the browser or emote wheel.

Registered Papyrus scene starts now honor `SceneOptions.Speed`, including matchmade starts;
previously that field was ignored for registered scenes. Leave it at 1 for authored playback.
Speed changes and pause now persist through Advance, stage changes, and automatic node transitions.

Framework foundation additions use scene ABI 1.5 (event identity/cause, query matchmaking) and engine
services 3.5 (alignment, handoff, matchmade handoff and manual progression). The engine serves the released Browser's 3.0 prefix request for
compatibility; newer consumers negotiate same-major/minimum-minor plus table size. New scene
keys are configurable in the engine settings schema. Saved alignment is user-wide in
`SFSE/OSF/alignment-overrides.json`, independent of savegames. See [API.md](API.md) for handoff
options, event order and the accepted positional snap. No saved game migration is required.

## Loop counts controlled in game

Content authors can now bind a stage or node's repeat count to a game GlobalVariable:

```json
"loops": { "global": "MyMod.esm|0x1234", "default": 3 }
```

A mod's settings menu or Papyrus script can update that global, so users do not need
to edit the scene JSON. OSF captures the value when the scene starts or is replaced;
changing it during playback affects the next start, not the active scene.
Missing plugins/forms, non-global forms, and invalid values use `default`.
Global values must be nonnegative whole numbers; zero holds indefinitely.
Existing integer `loops` declarations in `[0, 2147483647]` continue to work, and
`SceneOptions.LoopScale` multiplies the resolved count. See
[SCENE_SCHEMA.md](SCENE_SCHEMA.md) for the full authoring contract.

## Content validation changes

Version 1.5.0 rejects a scene when an authored `loops` or `loops.default` is not a
JSON integer in `[0, 2147483647]`. This applies to linear stages, graph nodes and
inline stages. Version 1.4 accepted numeric values through integer conversion;
fractional and negative values no longer load. Write `2` instead of `2.0` or
`2.5`, and choose an explicit nonnegative count for any old negative value.
Use `0` only when an indefinite hold is intended. Invalid authored counts reject
the scene; an invalid value read from a game global uses its valid authored default.

Other stricter checks can expose previously tolerated content errors:

- File defaults require their documented JSON types: booleans for flags, integer
  `priority`, integer `weight` in `[1, 1000000]`, and string arrays for `tags`.
  Unknown `section` and camera values are errors. Scene controls accept a boolean
  or an object with boolean `enabled`/`locked` and a string-array `disable` list;
  unknown capability names still warn and are ignored.
- Catalog `folder` paths must be nonempty relative paths with `/` separators and
  no empty, `.` or `..` segments. `clipRoot` cannot contain a drive prefix or `:`.
- Track positions and repeat values are validated: numeric `at` is in `[0,1)`
  (use `"end"` for the endpoint), `atFrame` is a nonnegative whole frame number,
  and the two fields cannot be combined. Named anchors cannot use `repeat:loop`.
- Conflicting vocabulary aliases, malformed shared role/prop templates, unknown
  template references and mismatched role/clip counts are load errors. File size
  and registry entry limits also apply.

A bad file default or shared template rejects its content file; a bad scene field
rejects that scene. Check Browser's **Imports** panel or `OSF Animation.log`, repair
the reported field and reload packs. See the [scene schema](SCENE_SCHEMA.md) for
field contracts and limits.

## Choose the packages

| Package | Installs | Requirements |
| --- | --- | --- |
| `OSF Animation v1.5.0.zip` | Engine DLL, Papyrus API, internal scenes, engine settings schema | Starfield, matching SFSE, Address Library for SFSE, OSF Settings 1.0.0+ |
| `OSF Animation Browser v1.5.0.zip` | Browser DLL/view, wheel, browser settings schema; optional vanilla/creature library and emote content | Matching OSF Animation 1.5.0, OSF UI 2.0.0+, OSF Settings 1.0.0+ |

Browser and OSF UI are optional for gameplay mods that call the engine's native or
Papyrus APIs. Install both archives if you previously used the browser or wheel
from the combined Animation package. Choose the library and emote options in the
**Browser** installer to keep those shipped collections available.

## Replace the previous installation

1. End active OSF scenes and close Starfield before replacing DLLs.
2. Keep a backup of your current save and its `.osf` sidecar. Preserve custom
   scene/animation packs, wheel assignments, and OSF Settings user files. If you
   placed custom files directly inside the old Animation mod folder, move those
   custom files into a separate content mod before replacing the old folder.
3. Install the required SFSE/Address Library versions and OSF Settings 1.0.0+.
   Install OSF UI 2.0.0+ if using Browser.
4. In MO2, replace the old **OSF Animation** installation with the engine archive
   rather than merging. Install **OSF Animation Browser** as a separate mod, or
   replace that mod too if you used a development version. In Vortex, remove the
   superseded Animation package from deployment and deploy the new packages.
   Keep only one active version of each DLL.
5. Select the desired library/emote options in the Browser installer and keep
   your separate content mods enabled. An engine-only install does not include
   the former bundled library, wheel, or browser view.
6. Check that the old Animation **schema** is no longer supplied by an enabled mod:
   `Data/SFSE/Plugins/OSF/Settings/schemas/osf.animation.json`.
   The engine now supplies `osf.animation.engine.json`; Browser supplies
   `osf.animation.browser.json`. Replace obsolete mod files, but **keep saved
   settings values** named `osf.animation.json` so the migration can read them.
7. Start through SFSE, open OSF Settings, and use **OSF Animation Browser** in the
   Launcher tab. Assign the browser and wheel keys again under the Browser's
   controls. Both hotkeys start unbound under the new provider ID.

Do not remove other mods' schemas or clear your user settings, wheel pins, or
`.osf` save sidecars as an upgrade step. The packages contain no player values.

## What happens to saved preferences

| Previous installation | 1.5.0 behavior |
| --- | --- |
| Older combined OSF UI settings service | Starts with the new OSF Settings defaults. Old values and hotkeys are not imported. |
| Development build using OSF Settings provider `osf.animation` | Valid engine/browser preferences import once into `osf.animation.engine` and `osf.animation.browser`. Explicit values already saved under either new ID win. |
| Build already using the two new provider IDs | Existing preferences remain in their respective providers. |

The one-time import covers supported preference keys, not keybindings. Rebind the
browser and wheel hotkeys after moving from the combined provider. The obsolete
`Data/OSF/settings.json` file is no longer read.

Scene IDs, generated inspection IDs, and the browser view ID are unchanged. Wheel
pins keep their existing file and IDs; their referenced content must still be
installed. Duration-cache ownership moved to Browser while retaining the existing
version-1 cache format. No manual cache reset is required.

## First launch

- Confirm **OSF Animation** and, when installed, **OSF Animation Browser** appear
  in OSF Settings. Check **Mod Issues** for missing content or incompatible hosts.
- Open Browser from the Launcher tab, check your installed collections, and try
  both rebound hotkeys. Confirm saved wheel assignments still resolve.
- Start and end a familiar scene. Check that input, camera, and equipment return
  as expected, then check save/load in your normal mod setup.

These checks cover your installation and upgrade. Automated installer tests verify
file layout and preservation; the disposable game suite does not replace a real
upgrade or visual, audio, and physical-controller acceptance. See
[testing coverage](SMOKE_TESTING.md#coverage-boundaries).

## Native consumers and development builds

Current public versions are **scene ABI 1.5** and **engine services ABI 3.5**.
The engine retains the released Browser's services 3.0 prefix and existing scene
API prefixes. Rebuild consumers of the unreleased services 1.0/2.0 tables against
the current header. Keep engine and Browser package versions matched; scene API
consumers do not need Browser or OSF UI.

Studio Link, actor overlay routes, and browser inspection previews from earlier
development builds were removed. Ordinary scenes, masks/additive playback, props,
and held stages remain supported. Scene authoring lives outside the core engine;
the Browser's Imports panel remains available for diagnostics and reloads.
