# Upgrading to OSF Animation 1.5.0

Version 1.5.0 separates the engine from the optional in-game Browser and moves
settings, hotkeys, and the launcher into OSF Settings. Use the two matching 1.5.0
packages when you want the browser or emote wheel.

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

Engine services ABI **3.0** replaces the unreleased 1.0/2.0 service tables. Rebuild
consumers of those tables against the current public header, and keep engine and
Browser versions matched. Scene API ABI **1.3** remains unchanged by this split;
existing scene-API consumers do not need Browser or OSF UI.

Studio Link, actor overlay routes, and browser inspection previews from earlier
development builds were removed. Ordinary scenes, masks/additive playback, props,
and held stages remain supported. Scene authoring lives outside the core engine;
the Browser's Imports panel remains available for diagnostics and reloads.
