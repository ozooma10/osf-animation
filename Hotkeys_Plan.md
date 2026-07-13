# Global Hotkeys — Plan

## Goal

Add a user-configurable **global hotkey system**: an always-on key table read from
`settings.json`, checked in the existing input hook, executing command strings on the
game thread. Three verbs ship in v1:

- `openBrowser` — open the scene browser (what the Data Slate does today; OSF UI's
  built-in F10 stays as-is, this adds a rebindable in-plugin path).
- `openWheel[:tagPrefix]` — open the osf view in emote-wheel mode (the wheel UI itself
  is a separate plan — this plan lands the verb wired to a stub `API::OpenWheel`
  that can simply call `OpenBrowser` behavior until the wheel exists, or log
  `[Hotkey] wheel not available` — see "Phasing" below).
- `toggleSceneTags:<tag[,tag..]>` — if the player is in a live OSF scene matching the
  tags → end it gracefully; else start one via `Scene::LaunchMatched`. This verb IS
  the sit/lean-anywhere feature (content in SitLean_Plan.md).

This is step 2 of the launch foundations. Step 1 (SceneLauncher extraction) is
**already merged** — `src/Scene/SceneLauncher.h` provides `LaunchOpts`,
`MakeOverrides`, and `LaunchMatched(actors, query, opts, overrides, logTag, mode)`.

## Constraints

- Read `AGENTS.md` first and follow it (log tags, style, no settings feature-toggles
  for modules — a `hotkeys` map is user config, not a module toggle, so it's allowed).
- Build chain is Windows-only; this dev box is a Mac. **You cannot compile.** Keep
  changes tight, re-read the diff, grep every symbol. The user compiles + tests.
- The input hot path must stay lock-free: the hotkey table is read-only after startup
  (built before `InputService::Install()` runs), so the Thunk may read it without
  locks. No allocation, no logging in the hot path.

## Verified facts (all line refs current as of this plan)

- The input hook is a vfunc swap on `RE::UI`'s `BSInputEventReceiver::PerformInputProcessing`
  (`src/Input/InputService.cpp:232-249`, slot 1, vtable index 10). The Thunk
  (`InputService.cpp:146-194`) walks the event list ONLY when
  `(g_active || g_captureMouse || g_compatActivate) && !MenuOwnsInput()` — there is
  **no always-on path today**; that gate must be widened for hotkeys.
- Press edge = `ButtonEvent.value != 0.0f && heldDownSecs == 0.0f`
  (`InputService.cpp:81`); keyboard events have `deviceType == kKeyboard` and
  **`idCode` is a Windows VK code** (comment at `InputService.cpp:60`;
  e.g. `kActivateKeyVK = 0x45` = 'E').
- Work is posted to the game thread via `SFSE::GetTaskInterface()->AddTask(...)`
  (pattern at `InputService.cpp:111`).
- `MenuOwnsInput()` (`InputService.cpp:135-144`) = game-menu-paused or console open.
  Hotkeys must respect it (no firing while typing in console).
- While the OSF UI overlay is open it consumes ALL game input at the WndProc, so the
  hook is starved anyway (`InputService.h:50-51`); belt-and-braces: also skip hotkey
  dispatch when `g_uiCursorVisible` is set (set from `UIBridge.cpp:933/938` on
  `osf.opened`/`osf.closed`).
- Settings: `Config::Settings::Load()` (`src/Config/Settings.cpp`) reads
  `Data/OSF/settings.json` (path built at `Settings.cpp:52`), nlohmann parse with
  `//`-comment tolerance, `boolKey`/`stringKey` helpers that ERROR-log
  `[Config] '{}' must be ...` and erase recognized keys, then WARN on leftovers
  (`Settings.cpp:105-107`). **Settings has no exposed struct — it pushes each value
  into the owning service via a setter** (comment `Settings.cpp:73`). Follow that:
  Settings parses the `hotkeys` object and pushes a built table into HotkeyService.
- Load order works in our favor: `Settings::Load()` runs at `kPostDataLoad`
  (`src/main.cpp:29`); `InputService::Install()` at `kPostPostDataLoad`
  (`src/main.cpp:44`). The table exists before the hook is live.
- `API::OpenBrowser()` (`src/API/UIBridge.h:19`, impl `UIBridge.cpp:975-1008`):
  returns false if OSF UI absent or bridge MINOR < 1; otherwise hides
  Book/Inventory/Data menus and calls `g_ui->RequestMenu("osf", true)` on the game
  thread. This is exactly what the `openBrowser` verb calls.
- There is a `@TODO: settings.json keymap override` at `InputService.cpp:61` — this
  plan does NOT implement verb-key remapping for the scene director channel (Space/P/
  End...). Global hotkeys only. Leave that TODO.

## Design

### 1. `src/Input/HotkeyService.{h,cpp}` (new, namespace `OSF::Input`)

```cpp
// A parsed hotkey binding: VK code -> command.
enum class HotkeyCommand { kOpenBrowser, kOpenWheel, kToggleSceneTags };
struct Hotkey
{
    std::uint32_t vk = 0;
    HotkeyCommand cmd;
    std::string   arg;   // openWheel tag prefix / toggleSceneTags comma-joined tags
};

class HotkeyService  // singleton, same shape as InputService
{
public:
    static HotkeyService& GetSingleton();
    // Build-time (before Install): parse "keyName" -> "command[:arg]" entries.
    // Bad key names / unknown commands -> [Config] ERROR + skip (never fatal).
    void Configure(const std::vector<std::pair<std::string, std::string>>& a_entries);
    // Hot path (Thunk): true if a_vk is bound. Lock-free (table immutable post-Configure).
    bool Matches(std::uint32_t a_vk) const;
    // Game thread: execute the command bound to a_vk.
    void Execute(std::uint32_t a_vk);
};
```

- **Key-name parser** (new; none exists): case-insensitive name → VK map covering
  `F1..F24`, `A..Z`, `0..9`, `Numpad0..9`, and a curated named set (`Space`, `Tab`,
  `Enter`, `Backspace`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`,
  `Up/Down/Left/Right`, `Minus`, `Equals`, `Comma`, `Period`, `Slash`, `Backslash`,
  `Semicolon`, `Apostrophe`, `LeftBracket`, `RightBracket`, `Grave`). Also accept
  `"0x2D"`-style hex for anything unlisted. **No modifier support in v1** (the
  ButtonEvent path has no modifier state) — document that.
- **Command parser**: split on the first `:`; `openBrowser` (no arg),
  `openWheel` (optional arg = tag prefix, default `player.emote.`),
  `toggleSceneTags` (required arg; lowercase it — Matchmaker queries are
  pre-lowercased, see `OSFScript.cpp` `ToTags`). Unknown → `[Config]` ERROR + skip.
- Log tag for runtime messages: `[Hotkey]` (add to AGENTS.md's log-tag list).
- Reserve/refuse keys the director channel uses (Space `0x20`, P, End, `+`, `-`, `0`)
  with a `[Config]` ERROR — a grant-armed scene would otherwise double-handle them.

### 2. Thunk integration (`src/Input/InputService.cpp`)

- Add `g_hotkeysArmed` (atomic bool, set true by a new
  `InputService::SetHotkeysArmed(bool)` called from HotkeyService::Configure when ≥1
  binding parsed). Widen the Thunk gate:
  `if ((active || capture || compat || hotkeys) && !MenuOwnsInput())`.
- Inside the button branch, BEFORE the grant-gated `MaybeDispatch` (order irrelevant
  in practice since bound keys are disjoint by the reserved-key rule, but keep hotkeys
  first so the check is obvious): press-edge test, `deviceType == kKeyboard`,
  `!g_uiCursorVisible`, then `HotkeyService::Matches(idCode)` →
  `SFSE::GetTaskInterface()->AddTask([vk = idCode]{ HotkeyService::GetSingleton().Execute(vk); })`.
- The Thunk must keep its "reads input, never consumes" contract
  (`InputService.cpp:192-193`) — the game still sees the keypress. Note in the doc
  comment that users should avoid binding keys the game itself uses heavily.

### 3. Verb implementations (in HotkeyService.cpp, all on the game thread)

- `kOpenBrowser` → `API::OpenBrowser()`; on false, HUD-error
  ("OSF UI not present / too old" is already logged by OpenBrowser).
- `kOpenWheel` → call `API::OpenWheel(arg)` if it exists; **for this plan, declare
  it in UIBridge.h and implement as: log `[Hotkey] wheel UI not yet available` +
  `UI::HudMessage::Error("emote wheel not available yet")`.** EmoteWheel_Plan.md
  replaces the body. This keeps the two plans independent.
- `kToggleSceneTags`:
  1. Player actor = `RE::PlayerCharacter::GetSingleton()` (or however SceneRuntime
     obtains it elsewhere — grep and match the existing idiom).
  2. **Live-scene check**: ask SceneRuntime for the player's live scene handle and its
     scene id/tags (grep `SceneRuntime.h` for `GetSceneForActor` / `StopForActor` /
     tag accessors — use what exists; if there is no "tags of live scene" accessor,
     add a minimal `bool SceneRuntime::ActorInSceneWithTags(RE::Actor*, const
     std::vector<std::string>&)` next to the existing per-actor lookups rather than
     exposing internals).
  3. If in a matching scene → end it the graceful way: prefer advancing to the exit
     node if the runtime exposes it; otherwise `StopForActor` (ledger restores
     control/camera either way). If in a NON-matching OSF scene → do nothing (log
     debug) — never yank a scene the hotkey doesn't own.
  4. Else → `Scene::LaunchOpts{}` (defaults), `Matchmaking::TagQuery{allOf = tags}`,
     `Scene::LaunchMatched(player, query, opts, MakeOverrides(opts),
     "[Hotkey] toggleSceneTags")`. Guards before launching: not in combat
     (match the idiom other code uses for combat checks), weapon drawn is fine to
     allow in v1 (scene actions can sheathe).

### 4. Settings (`src/Config/Settings.{h,cpp}`)

- New optional top-level key `"hotkeys"`: object of `"KeyName": "command[:arg]"`.
  Example documented in the Settings.h header comment block (`Settings.h:7-12` style):
  ```jsonc
  "hotkeys": { "F10": "openBrowser", "G": "openWheel", "N": "toggleSceneTags:player.sit" }
  ```
- Parse: non-object → `[Config] 'hotkeys' must be an object — ignored` ERROR; each
  value must be a string; collect `(key, command)` pairs and push via
  `HotkeyService::Configure(...)` (which does the real validation + logging). Erase
  the key so the leftover-warning loop stays accurate.
- Update `dist/settings.dev.json` with a commented example; **do not** add default
  bindings to `dist/settings.release.json` in this task (Packaging_Plan.md decides
  shipped defaults).

### 5. Wiring (`src/main.cpp`)

- Nothing new needed for order (Settings at `kPostDataLoad` already precedes
  `InputService::Install()` at `kPostPostDataLoad`). Add a `[Feature]` report line
  next to the Input Hook one: `[Feature] Hotkeys ENABLED (N bound)` / `DISABLED (none configured)`.

## Out of scope

- The wheel UI (EmoteWheel_Plan.md), sit/lean content (SitLean_Plan.md), FOMOD/docs
  (Packaging_Plan.md).
- Remapping the scene director channel keys (the `@TODO` at `InputService.cpp:61`).
- Gamepad hotkeys, modifier combos, hold/double-tap gestures.

## Acceptance criteria

1. `src/Input/HotkeyService.{h,cpp}` exist; `settings.json` `hotkeys` parsed with the
   established `[Config]` error style; invalid entries skipped, never fatal.
2. Thunk changes are minimal: one new atomic gate + one press-edge check; the
   unmodified queue is still always forwarded; no locks/allocation/logging added to
   the hot path.
3. `openBrowser` verb works end-to-end (user test: bind F10 or another key, press in
   normal gameplay → browser opens; press while console open → nothing).
4. `toggleSceneTags` launches via `Scene::LaunchMatched` with log tag
   `"[Hotkey] toggleSceneTags"`, and cleanly ends its own scene on re-press; it never
   ends a non-matching scene.
5. `openWheel` logs + HUD-errors gracefully (stub).
6. AGENTS.md log-tag list gains `[Hotkey]`; Settings.h header comment documents the
   new key; CHANGELOG entry added.

## Verification (user, on Windows)

- Build via xmake; report compile errors back rather than guess-editing.
- In-game: bind `"B": "openBrowser"`, `"N": "toggleSceneTags:test.sit"` in
  `Data/OSF/settings.json` (with a matching test scene tagged `test.sit`, or reuse an
  existing pack tag). Verify: browser hotkey; sit toggle on/off; no hotkey firing
  while console/browser open; `cgf "OSF.Health"` passes; a scene's Space/End director
  keys still work while a scene runs.
