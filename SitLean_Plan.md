# Sit/Lean + Emote Content Pack — Plan

## Goal

Author the **immersion content pack**: `dist/OSF/immersion/` (deploys to
`Data/OSF/immersion/`) containing sit/lean-anywhere scenes (driven by the
`toggleSceneTags` hotkey verb from Hotkeys_Plan.md) and the `player.emote.*` scenes
the emote wheel (EmoteWheel_Plan.md) lists. Pure JSON — no C++, no ESM, no new clips;
everything references vanilla `.af` animation paths that ship in the game's archives,
so the pack is self-sufficient.

## Constraints & workflow

- Read `AGENTS.md` and `docs/SCENE_SCHEMA.md` first (schema doc is the field
  reference; it self-warns as AI-generated — when in doubt the parser is ground truth:
  `src/Registry/SceneRegistry.cpp`).
- Build chain is Windows-only and content must be seen in-game — **you author the
  JSON; the user iterates in-game with `OSF.ReloadPacks()` (re-scans
  `Data/OSF/**/*.osf.json`, comment-tolerant parse) and `cgf "OSFTest.Solo"`.**
  Expect a tuning round-trip: mark every duration/offset guess with a `// TUNE`
  comment (the parser tolerates `//` comments).
- **Clip-suitability ground truth comes from the user's Phase-0 spike** (which clips
  loop vs snap, which look right free-standing). The list below is what exists in the
  vanilla packs; ship it behind that spike.

## Verified schema facts (cross-checked against the parser)

- Registry scans `Data/OSF/**` recursively for `*.osf.json`
  (`SceneRegistry.cpp:1469-1508`); first-loaded-wins on duplicate ids.
- Scene defaults that MATTER here: `stripActors` defaults **true** and `lockPlayer`
  defaults **true** (`SceneRegistry.h:257-258`) — an emote that strips the player is
  the classic authoring bug. Set file-level `"stripActors": false` in BOTH files;
  `"lockPlayer": false` for emotes (short, shouldn't seize controls), keep lock for
  sit/lean (you're seated/leaning; the director channel still allows Space/End).
- File-level defaults each scene may override: `stripActors`, `lockPlayer`, `fade`,
  `camera`, `roles`. File camera default is `scene_orbit`; author `"camera": "none"`
  where the camera should stay untouched (`SceneRegistry.cpp:1277-1288`).
- Camera `state` strings: `thirdperson_hold` (honors `distance`), `freefly`,
  `vanity_orbit`, `scene_orbit` (`SceneRegistry.cpp:434-435`).
- Graph form: `entry` + `nodes[]`; a node has inline `stages[]` XOR `use:`; edges
  `{to, when, id?, label?, default?}` with `when` ∈
  `end|loops|timer|advance|trigger:<id>`; `to:"$end"` ends the scene. `loops:0` =
  hold forever (manual advance). Branchable `advance` edges need `id`+`label`;
  `default:true` marks the Space-key edge.
- Vanilla clip paths resolve under file-level `"clipRoot":
  "meshes/actors/human/animations"` (pattern: `dist/OSF/vanilla/vanilla-common.osf.json:6`).
- Tags are lowercased into a set at parse; matchmaking (`toggleSceneTags` path) ranks
  priority tier then weighted-random. `unlisted:true` removes a scene from
  matchmaking — so sit/lean scenes must NOT be unlisted. Keep emotes listed too
  (simplest; they'll appear in the browser as a bonus — verify the wheel's catalog
  filter sees them either way).
- Weapon-sheathe is NOT automatic — author an `action` lane with
  `osf.weapon.sheathe` on enter where it matters (emotes/waves look wrong armed);
  the ledger restores weapons on end (vocabulary: `docs/SCENE_SCHEMA.md`
  `osf.*` actions).

## Verified clip inventory (what actually exists — differs from earlier assumptions)

- **Sit on ground**: `common/sitonground_pose.af`
  (in `vanilla-common.osf.json:4169`); female variant
  `photomode/female/sitonground_pose.af` (`vanilla-photomode.osf.json:159-170`).
  These are HOLD poses — there are no sit-down/stand-up transition clips, so
  enter/exit is a blend, not an animation.
- **Lean**: **no `walllean*` chains exist.** The vocabulary is
  `common/leanright.af` (`vanilla-common.osf.json:3725`) and its `leanleft`
  sibling — poses, same blend-in/out caveat.
- **Emote poses**: `common/wave_pose.af` (`vanilla-common.osf.json:4463-4475`),
  `common/handsonhips_pose.af` (`:3427`), `common/whatsup_pose.af` (`:4477`).
  No generic cheer/thumbsup (only photomode `photomode/vehicle_thumbsup.af`).
- **Photomode pose set** (`vanilla-photomode.osf.json:59-197`, `photomode/female/*`):
  armscrossed, boxer, captive, cower, jump, kneeling, sitonground, whatsup — note
  these live under `female/` but the spike should confirm they play on male skeletons
  (same rig) before gender-filtering.
- **Prop idles**: the `common/relaxed_idlepartialbody_*` family
  (`vanilla-common.osf.json:113` onward) — coffee cup, data slate, briefcase etc.
  Excellent "casual idle" emotes if the spike shows props render.

## Content design

### File 1: `dist/OSF/immersion/sit-lean.osf.json`

File level: `"clipRoot":"meshes/actors/human/animations"`, `"stripActors":false`,
`"camera":"none"` (per-scene overrides below), one anonymous role.

| id | tags | shape |
|---|---|---|
| `immersion/sit/ground` | `["player.sit","immersion"]` | graph: `sit` node = sitonground_pose, `loops:0`, camera `[{at:"enter","state":"thirdperson_hold","distance":1}]`; advance edge (`id:"stand"`, `label:"Stand up"`, `default:true`) → `$end` |
| `immersion/sit/ground/f` | same + gender-filtered role (female) using the photomode variant — ONLY if the spike shows the male/female poses differ meaningfully; otherwise skip |
| `immersion/lean/left` | `["player.lean","immersion"]` | same graph shape on `leanleft` |
| `immersion/lean/right` | `["player.lean","immersion"]` | same on `leanright.af` |
| `immersion/kneel` | `["player.sit","immersion"]` | photomode kneeling, same shape |

`toggleSceneTags:player.sit` / `:player.lean` from settings.json drives these; the
Space edge gives "press Space to stand" for free via the director channel.
Player lines up with walls manually in v1 (no raycast snap — note as future work).

### File 2: `dist/OSF/immersion/emotes.osf.json`

File level: `"clipRoot":"meshes/actors/human/animations"`, `"stripActors":false`,
`"lockPlayer":false`, `"camera":"none"`, one anonymous role.

| id | tags | clip | notes |
|---|---|---|---|
| `immersion/emote/wave` | `["player.emote.wave","immersion"]` | wave_pose | `timer:4` → `$end`; action lane `osf.weapon.sheathe` at enter |
| `immersion/emote/whatsup` | `["player.emote.whatsup",...]` | whatsup_pose | timer ~4s |
| `immersion/emote/handsonhips` | `["player.emote.handsonhips",...]` | handsonhips_pose | timer ~5s |
| `immersion/emote/armscrossed` | `["player.emote.armscrossed",...]` | photomode armscrossed | timer ~6s |
| `immersion/emote/coffee` | `["player.emote.coffee",...]` | relaxed_idlepartialbody coffee-cup idle | `loops:2` — spike confirms the prop renders |
| `immersion/emote/dataslate` | `["player.emote.dataslate",...]` | relaxed_idlepartialbody data-slate idle | ditto |
| (stretch) boxer/jump/kneeling/cower | `player.emote.*` | photomode set | per spike results |

`title` on each scene = the wheel slice label ("Wave", "What's up?", ...). These same
scenes are what the wheel launches ON an NPC target (anonymous role, no gender
filter, human species implied by the clip paths) — no separate `npc.emote.*` set
needed in v1.

## Tag contract (document in docs/, referenced by Hotkeys + Wheel plans)

- `player.sit`, `player.lean` — matchmade by `toggleSceneTags`; solo; must stay listed.
- `player.emote.<name>` — enumerated by the wheel via prefix `player.emote.`; solo.
- `immersion` — umbrella tag for the pack.

## Out of scope

- Item-use (eat/drink/med) scenes — deferred feature, needs the event spike + ESM.
- Paired NPC-response scenes, raycast wall-snap, custom GLB clips.
- Packaging/FOMOD (Packaging_Plan.md — note the archive script does NOT auto-stage
  new dist/OSF folders).

## Acceptance criteria

1. Both files parse on load with zero `[Registry]` warnings (user checks the log);
   `OSF.ReloadPacks()` returns the increased scene count.
2. Every scene is solo, self-terminating or Space-exitable, `stripActors:false`
   verified by playing an emote with armor equipped (nothing unequips).
3. Sit/lean toggle works end-to-end with `toggleSceneTags:player.sit` /
   `player.lean`; re-press stands up; ledger restores camera/controls.
4. Wheel lists exactly the `player.emote.*` scenes; each plays on player and on a
   crosshair NPC.
5. `// TUNE` comments mark every timer/offset guess; docs tag-contract section added;
   CHANGELOG entry.

## Verification (user, in-game)

`cgf "OSFTest.Solo"` per clip during authoring; then hotkey + wheel flows; save/load
mid-sit → no residue (`cgf "OSF.Health"`); dialogue while sitting → clean interrupt.
