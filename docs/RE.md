# OSF Animation — RE Ground Truth

Layout/offset/address ground truth for the **lean playback core**, verified by the dedicated
**OSF RE** project (`C:\Modding\Starfield\OSF RE`; Ghidra context repo under
`tools/ghidra/context_repo/modules/`). Pre-2026-06 findings were proven on 1.16.242 and
re-verified at-gate on **1.16.244**; 2026-06-12 findings are native 1.16.244.

**This doc = the facts the core relies on. The recovery runbook = `docs/POST_PATCH_CHECKLIST.md`.**
Full RE record: osf-re `tools/ghidra/context_repo/modules/gameplay.actor_transforms.json`.

> **Carved features moved.** RE for the carved tier — equipment (equip/unequip/add/remove),
> cosave save-name hooks, the FaderMenu fade poster, Wwise `PostEvent` — is **not used by this
> repo**; it belongs to the OSF Intimacy scene engine. Those findings are banked in osf-re
> (`gameplay.actor_equipment`, `engine.save_load`, `ui.fader_menu`, `wwise-audio-re-handoff.md`)
> and in the `OSF Animation Archive` repo. This page keeps only the core's ground truth.

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
natives onto the rebuilt VM — it does NOT persist cosave aftermath (that is OSF Intimacy).

## Misc offsets & fallbacks

- `IAnimationGraphManagerHolder` base @ **REFR+0x60** (CLSF says 0x58 — wrong on 1.16.244).
- **Fallback not yet used:** `BGSSynchronizedAnimationManager` (**99134**, `PlaySyncedAnimationSS`)
  as a native scene anchor — a possible alternative to the compose-root pin for paired scenes.
