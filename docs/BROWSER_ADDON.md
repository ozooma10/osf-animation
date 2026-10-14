# Browser native ownership

The repository builds two independently linked SFSE plugins. `src/` builds **OSF Animation.dll**;
`extensions/browser/src/` builds **OSF Animation Browser.dll**. The addon includes the released companion SDKs
and Animation's public API headers, with two pure string helpers under `shared/`. It never compiles
or links private engine implementations. `tools/Test-BrowserBoundary.ps1` checks that boundary.

The engine owns animation graphs, clocks, scene runtime, registries and validation, matchmaking,
mechanisms, save/load cleanup, and the native/Papyrus APIs. The addon owns the browser/wheel bridge,
catalog cards and duration estimates, reference tokens and world picking, presentation generations,
wheel assignments, launch/close policy, preferences, and browser hotkeys. Scene camera/input mechanisms
remain in the engine; the addon pairs its inspection-orbit acquisition/release and cursor ownership.

## Repository layout

Optional addons live under `extensions/<name>/`. Each extension owns its native target and any
web source, tests, and installer definitions it needs:

```text
src/                              Core engine
extensions/browser/
  src/                            Native browser and wheel
  ui/                             Web app, fixtures, and browser tests
  packaging/fomod/                Browser installer definitions
  xmake.lua                       Browser DLL target
shared/                           Engine-independent helpers
test/                             Engine and boundary integration tests
tools/                            Build and deployment entry points
packaging/                        Release entry points and shared validation
xmake.lua                         Root build, including extensions/browser
```

The root build includes each extension explicitly. Future extensions should use the public engine
APIs and their own targets/packages, following this layout. Installed DLLs remain in `SFSE/Plugins`
inside their respective MO2 mods. Generated browser output stays under `build/osfui-animation-browser`.

## APIs and lifetimes

The released `OSFSceneAPI.h` ABI **1.3** is unchanged. Existing consumers continue requesting it and
need neither Browser nor OSF UI. `OSF_RequestEngineAPI(0x00020000)` supplies the optional services in
`src/API/OSFEngineAPI.h`. Copy it alongside `OSFSceneAPI.h`; callers using the typed JSON helpers may
also copy `OSFCatalog.h` (nlohmann/json 3.11.3 or compatible). No import library is required. Services ABI **2.0** replaces the unreleased 1.0 table;
install matching engine/addon builds. Older service requests are rejected, while scene ABI 1.3 stays intact.

The new table provides:

- A verified-frame `Post` callback, world-reset/catalog/reload/scene-change notifications, and unsubscribe.
- Catalog metadata and full structured import diagnostics as schema-2 UTF-8 JSON. `OSFCatalog.h`
  defines the value fields; roles expose names/genders, stages expose counts/timing, and anchors expose
  form IDs. This is metadata, not a second mutable registry. Presentation sorting, wheel filtering,
  labels, truncation, and estimates stay in the addon.
- Positional cast matching and anchor matching, active-scene state, single-stage playback and
  handle-based speed/pause controls. Existing scene starts/stops/advance still use scene API 1.3.
- Actor identity, front-of-view anchor resolution, and the shared inspection camera/input mechanisms.

`src/API/CatalogBuilder.*` owns the registry-to-catalog conversion. The API handler pins one registry
snapshot, supplies the content root, and serializes the returned value. The builder reads only that
snapshot and in-memory durations; it does no file IO or presentation filtering. The scene registry
suite tests the production conversion, including stage resolution, duration precedence, path fallbacks,
rejected-file diagnostics, canonical clip keys, unmeasured clip references from every node (including
nonlinear scenes), and ownership of the returned data. Browser boundary tests cover consumption
and presentation separately.

The engine owns the table for process lifetime. JSON pointers are borrowed **only during the inline
callback**; the caller copies/parses them into its own allocation. No STL allocation crosses the DLL
boundary. Actor pointers and form IDs must be resolved/used in the current world on the game thread.
Active-scene JSON is an array of `{handle,id,stage,participants}` (participant form IDs), plus
`time`, `duration`, and `speed` when playback exists. Match results are arrays of exact scene IDs;
actor identity is `{species,gender}`. Unsupported ABI requests and requests before engine readiness
return null. Connect at `kPostPostDataLoad`, after all plugins finish `kPostDataLoad`.

`Post` accepts a callback/context from any thread and always defers it to the existing engine frame
queue, including posts made during a drain. A successful post owns one eventual callback invocation;
`execute=false` means world replacement canceled it and the consumer must only dispose its context.
A rejected post leaves ownership with the caller. No engine objects may be retained in queued payloads.
Keep callback code loaded until all posts finish. DLL unloading is not supported by the addon.

Notifications run synchronously on the producer thread, outside the scene/registry locks. Unsubscribe
waits for another thread's callback and supports self-removal. WorldReset increments the dispatch epoch
before notifying consumers and before engine teardown. Its callback must invalidate local generations
immediately and post cleanup; it must not perform game mutations during notification. Other notifications
also post game work. The addon retains a separate presentation generation, invalidated on native hide
and world reset, so queued opens, commands, and pointer deltas cannot act on an old presentation/world.
Catalog invalidation is atomic and survives hidden presentations. Reload triggers a duration scan only
when the addon subscribed; scan completion refreshes metadata without starting another scan.

Clip duration recording and lookup are memory-only. `ReadClipDurations` returns a copied snapshot
of decoded values and process-local revisions on any thread. `ProbeClipDuration` synchronously probes
one clip on the caller's worker, optionally reusing a source size/mtime hint. Neither operation schedules
work or touches the browsing cache. The core never imports cached estimates into playback data.

The addon copies canonical clip references from the catalog and owns one coalescing worker in
`Serialization/ClipDurationCache.*`. It loads/probes/prunes/saves the existing version-1
`OSF/clip-durations.json`, merges new decoded revisions around IO, and enriches its own catalog copy.
Authored durations still win. A newer playback decode wins over an overlapping disk load or probe.
Records observed after a save snapshot remain pending until the next scan. Completion invalidates
browser metadata and posts presentation refresh through the engine frame queue; canceled world posts
cannot operate on an old world. Missing Browser means no duration-cache IO or background scanning.

Successful inspection-orbit acquisitions are reference counted and must be released once. WorldReset
revokes all acquisitions; discard the local held flag instead of releasing an old hold into the new world.
The exclusive `SetInputRouting` channel accepts generic raw-mouse suppression and gamepad-consumption
flags, including native-free-camera passthrough. Browser chooses these flags on native show and clears
them on hide/load. The addon converts UI logical pixels into mouse-equivalent orbit deltas; the engine
has no browser cursor state, LMB gesture policy, or CSS-pixel scale. Core scenes retain their own camera/input ownership and restoration.

## Optional installation and compatibility

`OSF.OpenBrowser()` remains bound in the engine. It looks up `OSFBrowser_Open` in the optional addon
DLL and returns false when absent/unavailable. The addon also exports `OSFBrowser_OpenWheel(const char*)`
and `OSFBrowser_IsVisible()` for native callers; these do not add Papyrus functions. Missing Browser or
OSF UI is an inactive, supported configuration. An installed incompatible UI or corrupt addon assets
produce addon-owned diagnostics. The engine never registers views/hotkeys or scans browser durations
merely because it is installed. The addon binds at PostPostDataLoad regardless of DLL enumeration order.

Browser owns `extensions/browser/dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json`,
using Settings ID `osf.animation.browser` for preferences, launch defaults, hotkeys, and launcher ownership.
Its `openBrowser`/`openWheel` hotkeys must be rebound once: released Settings 1.0 derives native
ControlMap event names from the schema filename and has no binding-migration API. Core-only installation
contains no browser settings or hotkey declarations.

Core owns `osf.animation.engine.json`: `logLevel`, `debugNotifications`, and `gear.autoEquip`. On first
normal startup it reads those three values from Settings 1.0's legacy `osf.animation.json` user-values
file and saves them through the released Settings API, leaving the source untouched. Explicit values
already stored for the new engine ID win. A separate `osf.animation.engine.migrated` receipt prevents
later resets from reimporting old values; failed saves remain retryable. Private instrumented builds
skip player-file migration. Browser subscribes to the engine's log level separately.

Browser performs the same one-time import for its valid browser/launch preferences through
`BrowserSettingsMigration`, saving to `osf.animation.browser.json` and recording
`osf.animation.browser.migrated`. Existing destination values win; engine keys and invalid values
are ignored. The legacy user-values file is left intact for both imports.

Replace the old engine/browser installations when upgrading: neither component ships the legacy
`osf.animation.json` schema. Ordinary XMake deployment removes that obsolete schema from both mods.
Removing/reinstalling Browser leaves its saved values intact.
Wheel pins remain in the existing SFSE-log-parent `OSF/wheel-pins.json`, with the same scene/stage IDs
and absent-file versus empty-array semantics. Web bridge endpoints, the `osf.animation` retained-state
namespace, and view ID remain unchanged.

## Verification and live acceptance

`xmake test` includes a boundary suite for absent providers/forwarding, ABI table validation,
cancel/dispose across world replacement (including mid-drain), session reopen, callback unsubscription,
metadata ownership/cache invalidation, wheel pins, duration estimates, and full versus truncated diagnostics.
The browser's normal `npm run verify` still checks the document. Packaging binds both the addon DLL and
assets to one source receipt. Production archive/deployment hashes are evidence about files, not gameplay.

Before live acceptance, test a gameplay consumer with Browser and UI both absent; test Browser with UI
absent; then test the full installation: Launcher/hotkeys, wheel targets/pins across restart, actor and
furniture picking, matching and launching, pause/advance, orbit, player-close versus NPC-survival policy,
rapid hide/reopen, and load/failed-load while commands and pointer input are pending. Check keyboard,
controller, cursor/focus recovery, and shared scene-camera handback separately. No offline test proves
these Starfield behaviors.
