# OSF Animation — RE AI SLOP DOC

AI GENERATED SLOP DOC TO HELP ORIENT THE AI WHEN RE'ing

Layout/offset/address ground truth for the **lean playback core**, verified by the dedicated
**OSF RE** project (`C:\Modding\Starfield\OSF RE`; Ghidra context repo under
`tools/ghidra/context_repo/modules/`). Pre-2026-06 findings were proven on 1.16.242 and
re-verified at-gate on **1.16.244**; 2026-06-12 findings are native 1.16.244.

Full RE record: osf-re `tools/ghidra/context_repo/modules/gameplay.actor_transforms.json`.

> **Policy-mechanism RE is now IN this repo (Phase C, 2026-06-17).** The scene engine merged back in,
> so the Layer-C mechanism bindings — equipment equip/unequip (`ActorEquipManager` IDs 101949/101951),
> weapon sheathe/draw (`Actor::DrawWeaponMagicHands` vtable slot 0x136),
> the FaderMenu fade poster (114430), Wwise `PostEvent` (150391), the subtitle box
> (`ShowSubtitleEvent::Event::GetEventSource` 86874 / `HideSubtitleEvent` 86875) — are **used by this repo** (see
> `src/Equipment`, `src/Weapon`, `src/UI/FadeService`, `src/UI/Subtitle`, `src/Audio`). Each prologue-gates before use, like every
> binding here. Canonical RE detail still lives in osf-re (`gameplay.actor_equipment`,
> `engine.save_load`, `ui.fader_menu`, `wwise-audio-re-handoff.md`, `ui.subtitle`); this page keeps the core's ground
> truth. **Still NOT in this repo:** cosave save-name hooks / aftermath persistence (deferred), and
> free-fly/orbit camera (`camera.state_machine`, runtime-OPEN — the v1 camera lane uses only the
> already-bound third-person force/hold).

## Version provenance

All AddrLib-backed bindings the core uses (467920 / 400534 / 99963 / 883606 / 82710 / 64149 /
135315 / 135316) were re-verified byte-identical-at-gate on **1.16.244**. Every binding gates
before use and self-disables on mismatch (fail-soft).

## Known-broken inherited IDs — do NOT trust on faith

Inherited NAFSF/SAF AddressLib IDs **silently re-bind to garbage across patches**. Verify every ID
against expected bytes/slots before patching. Already caught re-binding wrong:

**118488 · 73213 · 422688** (vtable; real = **451614**) · **149852 · 881086**.

## Rig pipeline

The renderer consumes **flat rig buffers**, not bone `NiAVObject::local`. Per frame the engine
refreshes rig locals from the live pose, evaluates graphs (AnimationManager::Update, jobified,
~1500/s), composes worlds inside the rig buffers, then memcpy-stamps mapped bones' world (+0x80).
**Writing bone locals does nothing for mapped bones.**

**Chain:**

| Hop | Offset | Meaning |
|---|---|---|
| `BSFadeNode` → BGSModelNode | +0x180 | model node (size 0x90) |
| BGSModelNode → rig | +0x10 | rig buffer holder |
| rig → local / world / prevWorld bufObjs | +0x08 / +0x10 / +0x18 | the three buffer objects |
| bufObj → NiTransform array | +0x10 | raw array, **0x40 stride** |
| BGSModelNode → bone map | +0x18 | `BSArray` of `{u16 rigIndex, pad, NiAVObject* @+8}` |
| rig full-rebuild flag | +0x88 | **never set per frame** |

**Write point** = BGSModelNode **vfunc 2** (vtable AddrLib **400534**, VA 0x144B2F298; impl
**48634**, sig `(modelNode, &fadeNode->local, NiUpdateData*)`), **PRE-orig** — composes + commits
once per skeleton per frame on the scene-update thread; works for AI-frozen actors. Slot-4-post
writes are dead by design (vfunc 7 applier **122231** rewrites the rig local buffer after every
update).

**Slot layout** = Bethesda NiTransform: rotation 3 rows of 4 floats (+0x00/10/20), translate
+0x30, scale +0x3C. Rotations are **row-vector** convention → stored rows are byte-identical to
ozz's column-major storage, so `WriteNiTransformRows` is a **straight memcpy** + scale = 1.0.
**Do NOT transpose** (inverts every bone rotation — fully contorted rig).

**Bind = name match, race-agnostic.** `Graph::ResolveAndBind` builds the `rigIndex → jointIndex`
map by lowercasing each live bone-map entry's name and matching it against the animation's joint
names — no per-race skeleton tables (the SAF-lineage per-race bone lists are obsolete here). The
only name-based exclusion is `IsFaceRigNode`: skip the engine's facial rig, which is uniformly
prefixed **`faceBone_`** on the human skeleton (verified against `HumanRace.json` — every
jaw/eye/lip/ear/cheek/nose control *and* the facial-rig neck bones carry the prefix; the structural
`C_Neck`/`C_Head`/`*_Twist` joints do not). The filter therefore tests only `starts_with("facebone")`
(plus a `morph` expression-track guard). It previously denylisted bare anatomical tokens
(jaw/eye/lip/ear/...), which froze creature **structural** body bones that legitimately use those
words — e.g. a Terrormorph's animated maw `R_Jaw`/`C_Jaw`/`L_Jaw` rendered with a dead jaw on any
body anim. The prefix test leaves those bindable.

## Hooks & identity

| Hook | Binding | Notes |
|---|---|---|
| AnimationManager::Update | CLSF `VTABLE::AnimationManager[0]` AddrLib **467920** slot 4, impl **122232** | clock + sampling |
| BGSModelNode::Update | vtable **400534** slot 2, impl **48634** | the stamp/write point |

From the AnimationManager `this`:
- `*(*(g+0x2C0)+0x08)` = `TESObjectREFR*` (managed actor; identity matching, no collisions for
  co-located actors).
- `*(BSFadeNode**)(g+0x330)` = driven root (disambiguates player 1st/3rd person; currently unused).

`BSAnimationUpdateData`: timeDelta +0x60, modelCulled +0x6B, actor location +0x00.
`Actor::Set3D` = vfunc 0xA8. `NiAVObject` local/world/worldBound +0x40/+0x80/+0x100
(worldBound = `NiBound`: center +0x100, radius +0x10C).

## Participant visibility (pin-side, in the stamp hook)

Two writes ride the compose-root pin in `GraphManager::Hook_ModelNodeUpdate` (BGSModelNode slot 2,
pre-orig) so a pinned scene actor renders correctly where it is drawn rather than where its physics
capsule sits:

- **Cull sphere** — `NiAVObject::worldBound` (center +0x100, radius +0x10C) is recentered on the
  pinned render position each frame, radius floored to **2.5 m**. The engine derives the bound from
  the capsule (`fadeNode->world`, ~0.3 m off and drifting between per-frame re-teleports; worse for
  large placement offsets), so left alone `NiCullingProcess` frustum-pops the actor as the camera
  turns.
- **Near-camera fade** — `BSFadeNode+0x1B4` is a **binary fade float** (1.0 = drawn, 0.0 = faded
  out). The engine flips it to 0.0 when the 3rd-person camera orbits close to an actor (the fade
  that stops the camera seeing "inside" a body); for a pinned partner this popped it out of view as
  the player turned the camera. Held at **1.0** every frame for participants. Empirically RE'd
  **2026-06-14** (dump of the node's uncharacterized `0x160..0x1BF` gap: only +0x1B4 tracked the
  disappearance, toggling 1.0↔0.0 in lockstep with the symptom while the player root held 1.0).
  Forcing it pre-orig of the rig sync (inside `BSFadeNode::Update`) holds → the engine's fade writer
  runs earlier in the frame. Member is BSFadeNode-specific (sizeof **0x1C0**, past
  `bgsModelNode` @ +0x180).

## Positioning

- Transform primitive = fn **99963** `(service, opcode, refr, axisChar, double)`; service = deref
  of global **883606**. **Safe from any thread** (enqueues a refr-AddRef'd cmd). `RE::TransformService` wraps it.
- Opcodes: **0x1007** SetPos · **0x1009** SetAngle (degrees) · **0x109E** MoveToMarker · **0x113C**
  SetScale. Axis `'X'`/`'Y'`/`'Z'`.
- Real fast mover = `SetPositionFast` vfunc **0x137** (AddrLib **101055**) but **game-thread only** —
  use the 99963 queue off-thread.
- OBJ_REFR: angle @ +0x80 (heading z @ +0x88 radians), location @ +0x8C. **Starfield world coords
  are METERS.**

## Movement mode

- `MovementControllerNPC*` @ Actor+0x240 (latched mode byte +0x70).
- fn **135316 = SetAnimationDriven**, fn **135315 = SetMotionDriven** (NPC default). Both
  `void(controller*)`, off-thread-safe (polarity confirmed via console `ToggleMotionDriven`).
  The core sets anchored-scene participants animation-driven at start and reverts on StopScene.
- NOT relied on for alignment (compose-root pinning does that) — the requested switch never latches
  bit 19 from the Papyrus thread (open RE question, deprioritized).
- **AI off is UNUSABLE for scene actors:** the engine stops calling AnimationManager::Update for
  AI-disabled NPCs, freezing the whole anim pipeline.

## Weapon sheathe / draw (Layer-C `osf.weapon.*`)

- `Actor::DrawWeaponMagicHands(bool)` = CLSF Actor virtual **vtable slot 0x136** (RE/A/Actor.h;
  hex-indexed like its neighbours SetPosition 0x137 / Update 0x13F). `false` = sheathe, `true` =
  draw. **Called, not patched.** `WeaponService::Available()` resolves the AddressLib-mapped Actor
  vtable (`RE::Actor::VTABLE[0]`, id 451610), reads slot 0x136, and disables the feature if it
  doesn't resolve on this build (verify-before-call; an absent id throws in `address()` → caught →
  feature off, never dispatched).
- **Weapon-drawn state bit: UNVERIFIED.** No named accessor exists in CLSF (`ActorState::actorState1/
  actorState2` are raw bitfields) and OSF RE hasn't located the weapon-state bits. So `osf.weapon.*`
  is a **symmetric pair** — `restore` re-draws unconditionally (author sheathes only armed roles). A
  state-aware restore (re-draw only an actor that was drawn pre-sheathe) is deferred until the bit is
  proven: log `actorState1` for the player holstered-vs-drawn to find it, then gate restore on it.

## Subtitle box (Layer-C `UI::Subtitle`)

The spoken-line box is driven by an **event Notify**, not a method call — there is **no**
`SubtitleManager::ShowSubtitle` (the singleton at `0x1462C0510` is touched only by its initializer).
Runtime-proven on 1.16.244 (osf-re `ui.subtitle`, 2026-06-26; the `sub spk GLORP HELLOWORLD` probe
rendered "GLORP: HELLOWORLD" live).

- `ShowSubtitleEvent::Event::GetEventSource` = **86874** (`0x141495c90`); `HideSubtitleEvent::Event::
  GetEventSource` = **86875** (`0x141495d00`). `UI::Subtitle` resolves each by AddrLib ID and drives it
  through CLSF `BSTEventSource<T>::Notify` (the same idiom `UI::HudMessage` uses). An unresolved ID →
  fall back to the HUD-message channel (never lost, never crashes).
- **Payload** `ShowSubtitleEvent::Event` (sizeof 0x18): `{ const char* subtitleText @0x00; const char*
  speakerName @0x08; bool isPlayer @0x10; }`. Fields are **raw UTF-8 `const char*`, NOT `BSFixedString`**
  — the sink (`HUDSubtitleDataModel::ProcessEvent` 86881) runs its own byte-wise UTF-8 converter, so a
  `BSFixedString` entry pointer renders mojibake. `HideSubtitleEvent::Event` is **empty** (Notify clears
  the box). Field order + type were runtime-corrected (the static read had speaker/text reversed and
  assumed `BSFixedString`).
- Renders the **standard bottom-of-screen list** reading `speakerName: subtitleText` — **NOT**
  3D-positioned on the speaker, and **bypasses** the user's `bDialogueSubtitles`/`showSubtitles` gate
  (that check lives upstream in the vanilla producer 114395, not in the event path).
- **No auto-hide** on the direct path (the vanilla producer carries the duration). `UI::Subtitle` arms a
  hold deadline, `Tick()` Notify()s Hide once it passes, and `OnStopAll()` hides on save-load teardown.
- **CLSF drift:** these accessors were left `{0}` mislabelled `133631`/`133630` (those are unrelated
  tagged-object accessors). The real IDs are **86874**/**86875**.

## Matchmaking form-refs + role-filter predicates (`roles[].filters`)

Scene role filters (`keyword`/`race`) resolve `"Plugin.esm|0xLocalID"` refs to forms **at scene load**
(`src/Registry/SceneRegistry.cpp`, `ComposeFormID`). RE-sensitive. **Verified in-game 2026-06-17:**
full-master tier composition + the race predicate — `Starfield.esm|0x0021A8D7` (SIF TerrormorphRace)
resolved clean and `StartSceneByTags` bound a live Terrormorph (runtime ref `FF…`) to the filtered role
while rejecting a human. **Still unverified:** the light/medium tiers (no `.esl`/medium ref tested yet)
and the **keyword** predicate (`ActorHasKeyword` — a different call path than race).

- Plugin → index via `TESDataHandler::GetSingleton()->compiledFileCollection` (CLSF offset **0x1580**,
  RE-proven 1.16.242). Match `TESFile::fileName` (char[260] @ **+0x38**) case-insensitively by basename
  across the three tiers and derive the index from the array position:
  - full master (`files[i]`): `formID = (i << 24) | (local & 0x00FFFFFF)`
  - medium (`mediumFiles[i]`, tier 0xFD): `formID = 0xFD000000 | (i << 16) | (local & 0xFFFF)`
  - light (`smallFiles[i]`, tier 0xFE): `formID = 0xFE000000 | (i << 12) | (local & 0xFFF)`
  (`TESFile::compileIndex` @ **+0x1B7** = the tier sentinel for light/medium, not the secondary index,
  so the index comes from the array position.) The local id's high/load-order byte is masked off — the
  plugin name is authoritative, so a whole xEdit FormID resolves regardless of its load-order byte.
- Form + type check via `TESForm::LookupByID<T>` (returns null on not-found OR wrong type). Unresolved /
  wrong-type → the scene is rejected at load (fail-soft; ReloadPacks re-attempts in-game).
- **Keyword predicate** (`Matchmaking::ActorHasKeyword`): an actor matches if its **actorbase**
  (`Actor::GetNPC()`, a `BGSKeywordForm` via `TESActorBase`) OR its **race** (`Actor::race` @ **+0x358**,
  a `BGSKeywordForm`) carries the keyword. The base call is qualified `BGSKeywordForm::HasKeyword(...)`
  because `TESNPC` hides it with a `HasKeyword(string_view)` overload. Whether to also fold in other
  race-derived keyword sources is the one open semantics item (revisit if SIF needs it).
- **Race predicate**: `Actor::race` pointer-equals one of the role's resolved `TESRace*`.

## Save / load (event source + payload)

CLSF declares `SaveLoadEvent` payload-less; OSF reads it by raw offset in SaveSafety.cpp
(RE-proven by the OSF RE `LoadWindowProbe`):

| Field | Offset | Notes |
|---|---|---|
| elapsedMs (u32) | 0x00 | |
| opType (u8) | 0x0C | 4 = quickload, 6 = load-by-name (world-replacing) |
| status (u8) | 0x0D | 0 = begin, 1 = load-ok, 3 = fail/cancel, 4 = save-done |

`TESLoadGameEvent::GetEventSource` = **64149**, `SaveLoadEvent::GetEventSource` = **82710**.
World-replacing opTypes (full enumeration, osf-re `engine.save_load`): {kLoadMostRecent,
kQuickload, kLoad, kLoadNamedFile}; new game / Unity-NG+ fire no SaveLoadEvent (the TESLoadGameEvent
backstop owns them). The core uses these only to drop scene/graph state on a load and re-bind the
natives onto the rebuilt VM — it does NOT persist cosave aftermath (deferred, still unimplemented).

## Misc offsets & fallbacks

- `IAnimationGraphManagerHolder` base @ **REFR+0x60** (CLSF says 0x58 — wrong on 1.16.244).
- **Fallback not yet used:** `BGSSynchronizedAnimationManager` (**99134**, `PlaySyncedAnimationSS`)
  as a native scene anchor — a possible alternative to the compose-root pin for paired scenes.
