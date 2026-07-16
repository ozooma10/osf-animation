# Launch Packaging & Docs — Plan

## Goal

Ship the launch release: FOMOD gains an optional **"Immersion Actions"** module
(the sit/lean + emote content pack), the archive script stages it, shipped settings
gain default hotkeys, and docs/mod-page copy present the release as an immersion mod
with a framework underneath. Run this LAST — after Hotkeys, EmoteWheel, and SitLean
plans have landed and been tested in-game.

## Constraints

- Read `AGENTS.md` first. The archive is built on Windows
  (`packaging/build-archive.ps1`); on this Mac you can edit the script and XML but
  not run the build — keep changes reviewable and let the user produce/inspect the
  zip.
- FOMOD layout convention: each source folder in the archive maps onto `Data/` at
  install time (existing: `Core\OSF\settings.json`, `Core\Scripts\...`,
  `SafCompat\Scripts\...`).

## Verified current state

- `packaging/fomod/ModuleConfig.xml`: `requiredInstallFiles` = folder `Core`
  (L7-9); ONE install step "Compatibility" with one `SelectAny` group holding the
  "SAF Backwards Compat" plugin (folder `SafCompat`, L22-24), auto-recommended when
  `SnuSnuField.esm`/`GergelEbanex.esm` is active (L26-38). Also `info.xml` (version
  stamped by the build script) and `images/`.
- `packaging/build-archive.ps1` stages: the DLL (`Core\SFSE\Plugins\`), the four
  release `.pex`/`.psc` (OSFTest excluded), the SAF shim scripts,
  `Core\OSF\settings.json` ← `dist/settings.release.json`, and
  **only `dist/OSF/internal.osf.json`** (L64). **It does NOT stage `dist/OSF/vanilla/`
  or any other pack folder — new content will silently not ship unless added here.**
- `dist/settings.release.json` is currently just
  `{"logLevel":"info", "debugNotifications":false}`.
- The browser's F10 open key lives in the external OSFUI.dll, NOT this plugin —
  so shipped hotkey defaults must not double-bind F10.
- docs/: `API.md` (Papyrus API), `GETTING_STARTED.md` (consumer walkthrough),
  `SCENE_SCHEMA.md` (JSON reference). All three self-mark as generated drafts.

## Work items

### 1. Resolve the vanilla-pack staging question (ask the user first)

`dist/OSF/vanilla/**` (the generated clip-library packs, which the browser's
"library" lane and the immersion clips' reference material come from) is not staged
by the script. Determine how it ships today (separate archive? generated at first
run? manual copy?) — **ask the user; do not guess.** If it's simply missing, add a
`Core\OSF\vanilla\` staging block; the immersion pack itself does NOT depend on it
(it references game-archive `.af` paths directly), but the browser's library lane
does.

### 2. FOMOD (`packaging/fomod/ModuleConfig.xml`)

- New install step "Modules" (before/after "Compatibility" — match existing style)
  with a `SelectAny` group **"Gameplay Modules"** containing plugin
  **"Immersion Actions"**: description sells it ("emote wheel — emote in-game and
  target the NPC in your crosshair"), image optional, `folder` = `Immersion` → root,
  default **Recommended** (`Optional` if the user prefers opt-in). This group is
  where "Ambient NPC Life" slots in later.
- Install mapping: `Immersion\OSF\immersion\emotes.osf.json` →
  `Data/OSF/immersion/...`.

### 3. Archive script (`packaging/build-archive.ps1`)

- Stage `dist/OSF/immersion/*.osf.json` → `$stage\Immersion\OSF\immersion\`
  (fail loudly if the folder is missing, matching the SAF `.pex` throw style).
- Keep OSFTest/dev settings exclusions as-is; verify `CrewLife/` (in-progress, not
  part of this release) is NOT staged.

### 4. Shipped settings (`dist/settings.release.json`)

Add default hotkeys — **not F10** (OSFUI owns it):

```jsonc
{
  "logLevel": "info",
  "debugNotifications": false,
  "hotkeys": { "G": "openWheel", "N": "toggleSceneTags:player.sit", "L": "toggleSceneTags:player.lean" }
}
```

Confirm key choices with the user (G/N/L vs conflicts with their other mods);
mirror into `dist/settings.dev.json` with comments. Settings parsing tolerates the
`hotkeys` key even when the Immersion pack isn't installed — the verbs just find no
matching scenes and HUD-error, which is acceptable; note it in the FOMOD description.

### 5. Docs

- `docs/API.md`: no new Papyrus natives this release (hotkeys/wheel are native-side)
  — add a short "Hotkeys" section documenting the open-browser / open-wheel verbs
  and the key-name list; plus the tag contract (`player.emote.<prefix>`) so
  third-party packs can join the wheel.
- `docs/GETTING_STARTED.md`: add "add your own emote to the wheel" as a second
  worked example (a 10-line pack with one `player.emote.*` scene) — it's the
  smallest possible consumer demo.
- `AGENTS.md`: log-tag list gains `[Hotkey]`; note the Immersion module's
  presence-of-folder gating pattern.
- `src/UI/FirstRunHint`: check the hint text (`FirstRunHint.cpp:100,108` mentions
  F10) — extend to mention the wheel key if the copy has room.
- `CHANGELOG.md`: one release entry covering hotkeys, wheel, immersion pack.

### 6. Mod-page copy (deliver as `packaging/modpage.md`, user pastes to Nexus)

Lead with the player features (wheel, sit/lean anywhere, emote your crosshair
target), screenshots list for the user to capture, then the framework pitch
(scene browser, JSON packs, Papyrus API, tag contract) and a "for mod authors"
section linking GETTING_STARTED. Include the stability story (undo ledger, save
safety) — it's the reputation-setter.

## Acceptance criteria

1. A `-Version`-stamped zip stages: Core (DLL, scripts, settings with hotkeys,
   internal pack, vanilla decision from item 1) + Immersion (both JSONs) + SafCompat,
   with FOMOD selecting Immersion as its own module.
2. Installing WITHOUT Immersion: framework works, hotkey verbs HUD-error gracefully,
   zero `[Registry]` warnings.
3. Installing WITH Immersion: wheel + sit/lean work with shipped default keys.
4. Docs updated per item 5; modpage.md drafted; CHANGELOG entry.

## Verification (user, on Windows)

Run `build-archive.ps1 -Version X.Y.Z`; install the zip fresh via MO2 both with and
without the Immersion option; run the in-game checklist from the three feature plans
plus `cgf "OSF.Health"`.
