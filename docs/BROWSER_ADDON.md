# Browser native ownership

The repository builds two independently linked SFSE plugins. `src/` builds **OSF Animation.dll**;
`extensions/browser/src/` builds **OSF Animation Browser.dll**. The addon includes the released companion SDKs
and Animation's public API headers, with general string helpers under `shared/`. It never compiles
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
  sdk/                            Pinned OSF UI native SDK
  tools/                          Browser fixture snapshot generators
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

Scene ABI **2.0** and services ABI **4.0** are the breaking baseline. Copy `OSFEngineAPI.h`
alongside `OSFSceneAPI.h`; typed catalog helpers also use `OSFCatalog.h` and nlohmann/json 3.11.3
or compatible. No import library is required. Requests require a matching major, supported minor
and sufficient table size. Older majors/prefix layouts are rejected. Metadata-only `ReadCatalog`
is permitted from workers; it pins immutable registry data and never accesses game objects.

The new table provides:

- A verified-frame `Post` callback, world-reset/catalog/reload/scene-change notifications, and unsubscribe.
- Catalog metadata and full structured import diagnostics as schema-4 UTF-8 JSON. `OSFCatalog.h`
  defines the value fields; roles expose names/genders, stages expose counts/timing, and anchors expose
  form IDs. This is metadata, not a second mutable registry. Presentation sorting, wheel filtering,
  labels, truncation, and estimates stay in the addon.
- Positional cast matching and anchor matching, active-scene state, single-stage playback and
  handle-based speed/pause controls. Existing scene starts/stops/advance still use scene API 2.0.
- Actor identity, world-anchor/navmesh queries, and the shared inspection camera/input mechanisms.
  Browser owns the front-of-view direction, distance, and snap-acceptance policy.
- Native clip-scene providers: copy `OSFClipScene.h` for the JSON record definition. Providers select
  existing inline clips and supply scene identities and playback defaults through the public API.

`src/API/CatalogBuilder.*` owns the registry-to-catalog conversion. The API handler pins one registry
snapshot, supplies the content root, and serializes the returned value. The builder reads only that
snapshot and in-memory durations; it does no file IO or presentation filtering. The scene registry
suite tests the production conversion, including stage resolution, duration precedence, path fallbacks,
rejected-file diagnostics, canonical clip keys, unmeasured clip references from every node (including
nonlinear scenes), and ownership of the returned data. Browser boundary tests cover consumption
and presentation separately.

Browser refreshes the catalog on one coalescing worker. A request superseded during the read cannot
publish; a failed read retains the last good snapshot and can be retried by the next notification.
Only a verified frame command swaps the completed Browser-owned snapshot, so getter references remain
valid for that command. The worker destroys retired snapshots. Duration-scan completion updates duration
overlays without requesting the full catalog again. Initial/reload completion updates visible catalog,
library, imports and wheel data; an Imports reload reply waits for the refreshed report. World replacement
resumes any metadata publication whose queued completion was canceled. Metadata reads use a pinned
immutable registry and the locked decoded-duration store, never game objects. Older hosts are refused
before Browser starts the worker; the public table layout and JSON schemas use the v2 baseline.

The engine streams full schema-4 catalog JSON directly from the owned catalog values, avoiding a
second full object tree on the producer. `OSFCatalog.h` declares the fields once for both its ordinary
JSON converters and the streaming visitor. The serializer retains UTF-8 replacement, numeric formatting,
all fields and array order; JSON object member order is not a contract. Each call owns its output buffer
through the inline callback. Full-catalog provider inputs use the same serializer. The compact
schema-2 provider input is unchanged.

Browser's full-catalog callback uses the JSON library's SAX lexer to populate an owned typed snapshot
directly, without a generic object tree or a second conversion pass. Mutable visitors use the same
field declarations as the engine serializer. Required fields, field types, array order and schema 4
are checked; unknown fields are skipped without materializing their values. Duplicate known fields and
mismatched input types fall back to the previous DOM decoder to preserve its last-value and conversion
semantics. Malformed responses never publish partial data and retain the last good catalog. The entire
typed decode now occurs inside the inline `ReadCatalog` callback; decorating and publication remain on
the existing worker/frame path. Compact provider input parsing is unchanged.

The browser's private web transport keeps each OSF UI event below the released
host's 1 MiB limit. JSON larger than 512 KiB is split into UTF-8-safe 64 KiB pieces,
paced at four pieces per frame, and decoded before the controller receives the
original event. Transfers are bounded to 32 MiB per document and 64 MiB in flight;
incomplete, stale, conflicting or malformed transfers never replace application
state. Hiding the view cancels pending transfers. Install the browser DLL and web
assets from the same archive. Native boundary and browser tests exercise this
protocol; the live suite also transfers and renders the shipped animation library.

Browser cards use `hideApparel`, `playerInputLock`, `placement`, and explicit `sourceKind` fields.
Launch options use `hideApparel`, `playerInputLock`, and `sceneControls`. Import problems are
structured objects with severity, code, message, and repair context. The private web transport
does not accept older aliases or infer source kinds from IDs. Authored content aliases and the
public engine APIs retain their compatibility behavior.

Browser's `BrowserClipLibrary` provider generates the automatic inspection entries formerly harvested
by core. It retains the `osf.scene-clip/*` hash, source ordering, duplicate precedence, and collision
suffixes, so ordinary existing wheel pins and direct-ID starts continue working while the addon is
installed. Explicit authored `clipLibrary` registrations remain engine content. Core-only installations
do not generate inspection entries, inspect emote tags, or choose inspection launch defaults.

`RegisterClipSceneProvider` receives the unchanged schema-4 immutable base catalog. Services 3.17 also
offers `RegisterClipSourceProvider`, used by Browser, with the smaller schema-1 `ClipSourceSnapshot`
from `OSFCatalog.h`. It includes every base scene's ID, source filename, pack, provider, tags, library
and curated flags, plus every inline clip's source scene and node/stage/clip indexes, raw file,
animation ID, availability, display path, species and authored duration. It omits presentation
nodes/stages, decoded-duration lookups and import diagnostics. Scenes without clips still reserve
their IDs. Both inputs exclude all provider output; neither applies Browser filtering policy.

Both registration kinds share one owner namespace and `UnregisterClipSceneProvider`. They emit one JSON
array of `ClipScene` records synchronously and must not reenter the registry or engine API. Registration
accepts a unique owner and retains the callback until unregistration; callbacks also run on reload's
thread. Each provider batch validates before publication: invalid references, unavailable clips,
duplicate IDs, and collisions with authored content or earlier providers reject the whole batch.
Registration success means the provider is installed; invalid output is logged and retried at the next
reload. Reload rebuilds all providers from base content, and unregister removes future lookups. Existing
runtime handles retain their pinned definitions. These scenes report `providedScene` plus their provider
owner; Browser maps its own entries to `derivedDebugAnimation` and adds its inspection counts to the
Imports presentation. Providers do not change the authored scene count or base import diagnostics.
Browser normalizes scene grouping/ordering metadata once and selects the minimum source address for
each identity directly, retaining the legacy winner and output order without sorting every occurrence.

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
Picked reference tokens have the longer world lifetime: closing/reopening preserves the cast and
placement and rescanning returns the same tokens. Resolution still validates the current reference.
World reset clears the native token table; `osf.animation.world` carries its generation to clear the
view's picks, matches, nearby results and scene handles. Every open replays that generation before
token-bearing data, so a hidden document that missed the reset also drops old-world state.
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
using Settings ID `osf.animation.browser` for hotkeys and launcher ownership. Its Settings page contains
only the two opener bindings; preferences are edited in the Browser itself.
Its `openBrowser`/`openWheel` hotkeys must be rebound once: released Settings 1.0 derives native
ControlMap event names from the schema filename and has no binding-migration API. Core-only installation
contains no browser settings or hotkey declarations.

Core owns two schemas. `osf.animation.engine.json` holds `logLevel`, `gear.autoEquip`, and the six
playback bindings; `osf.animation.align.json` holds the twelve scene-author alignment bindings on a
page of its own, depending on the engine schema without extending it, so the page players read stays
short. The unused `debugNotifications` setting has been removed, and the former `keys.speedUpAlt` /
`keys.speedDownAlt` rows are now fixed top-row `=` / `-` aliases with no schema row. `SceneKeymap`
names the owning schema per key and `EngineSettings` subscribes to both; a hand-pruned install
missing the alignment schema logs a warning and keeps its defaults. OSF Settings loads and saves
each schema's values under its own mod ID, using schema defaults when unsaved. Browser subscribes
to the engine's log level separately.

Browser saves its eight preferences in the SFSE-log-parent `OSF/browser-preferences.json` using a
versioned JSON file and atomic replacement. A missing file uses defaults; changing a preference
creates or updates the file. Failed saves do not publish changes, and malformed/unsupported files
are not overwritten.
Private smoke builds use session-only preferences without accessing player files.

Browsing state is always retained between openings during the game session. The former
`browser.rememberBrowsing`, `launch.speed`, `browser.openTo` and `browser.unavailableScenes` values
are ignored. Launch speed starts at 1x and can be changed in Start Overrides; it resets after
success, when selecting another scene, or on reopening. The browser always reopens on the tab it was
left on, falling back to Browse when Active holds nothing, and scenes that need a different cast or
furniture always collapse behind the in-list reveal button. Library detail/source controls live
beside the Browse filters. Other preferences, launch defaults, and author details remain in the
Browser settings panel. The web endpoint and retained-state format are unchanged except for the
retired keys.

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
