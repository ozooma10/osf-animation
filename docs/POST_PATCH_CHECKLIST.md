# Post-Patch Recovery Checklist

*When Starfield updates, OSF fails **soft**: every engine binding verifies its target before use
and self-disables on mismatch, so the worst case is "plugin loads, features refuse to run" — not
a crash. This document turns recovery from an archaeology dig into a checklist: every
version-sensitive binding the lean core uses, where it lives, how it gates, and how to re-verify it.*

> **Scope: playback core + Layer-C mechanisms.** The scene engine merged back in (Phase C,
> 2026-06-17), so this checklist now also covers the policy-mechanism bindings that ship here:
> equipment equip/unequip (`ActorEquipManager` 101949/101951), the FaderMenu fade poster (114430),
> and Wwise `PostEvent` (150391) — each prologue-gated and self-disabling on a mismatch. **Still NOT
> in this repo** (skip them here): cosave save-name hooks / aftermath persistence (deferred) and
> free-fly/orbit camera (the v1 camera lane only uses the already-listed third-person force/hold).

**Bus-factor note:** someone who is *not* the original author should be able to list every binding
needing re-verification in <10 minutes from this table. Re-deriving a moved address still needs the
[OSF RE project](#re-project) + Ghidra, but knowing *what* broke and *whether* it broke should need
only the log + this page.

## Step 0 — Read the startup log first

`Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log`. `main.cpp` emits a feature report at
`kPostDataLoad`:

- Running game version vs the RE-verified build (currently **1.16.244.0**). If these differ, this
  whole checklist is in play.
- `Feature report: playback hooks INSTALLED/UNAVAILABLE`, then the save-sink registration lines.
  Each binding below logs a distinct WARN/ERROR on mismatch — grep those first; a binding that still
  verifies needs no work even on a new patch (the gate is the source of truth, not the version number).

**Triage order:** if the two **core hooks** (rows 1–2) fail, nothing plays — fix those first.
Everything else degrades a single capability.

## The bindings

Columns: **Gate** = how the code proves the target is right before using it (`vtable-slot` =
compares the resolved vtable slot to the expected impl address; `prologue` = byte-compares the
first N bytes of the function).

### Core (failure = nothing plays) — `src/Animation/GraphManager.cpp`

| # | Binding | AddrLib ID | Gate | Re-verify against |
|---|---|---|---|---|
| 1 | AnimationManager::Update hook | vtable **467920** slot 4 vs impl **122232** | vtable-slot (refuses to patch on mismatch) | osf-re `gameplay.actor_transforms` — sampling/clock hook |
| 2 | BGSModelNode::Update hook | vtable **400534** slot 2 vs impl **48634** | vtable-slot (refuses to patch on mismatch) | same module — the stamp/write point |

Log on failure: `... vtable slot {} = {:X}, expected ... — AddressLib IDs stale ... NOT patching`
(the plugin refuses to install the hook). Both must pass or the framework is inert.

Implicit dependents of the hooks (not separately gated — re-verify if poses are wrong but hooks
installed): rig buffer layout (NiTransform rows, 0x40 stride, `Graph.cpp`), managed-actor resolve
`*(*(animMgr+0x2C0)+0x08)`, compose-root pin write to `&fadeNode->local` translate +0x30, cull
sphere +0x100/+0x10C, near-camera fade float +0x1B4. These are **layout/offset** facts, not ID
bindings — a patch can move them silently with no gate. Symptom: contorted or mis-positioned rig
with hooks reported installed. See `docs/RE.md` "Rig pipeline" / "Participant visibility".

### Save-safety — `src/Serialization/SaveSafety.cpp`

| # | Binding | AddrLib ID | Gate | Notes |
|---|---|---|---|---|
| 3 | SaveLoadEvent::GetEventSource | **82710** | prologue (13 bytes) → begin sink | falls back to row 4 backstop on mismatch |
| 5 | SaveLoadEvent payload layout | — | raw offset: opType @0xC, status @0xD | a layout move is silent — re-verify via osf-re `engine.save_load` if load teardown stops firing |

Log on success: `registered SaveLoadEvent begin sink` + `registered TESLoadGameEvent backstop
sink`. If row 3 fails you'll see `SaveLoadEvent GetEventSource prologue mismatch; relying on
TESLoadGameEvent backstop` — degraded but safe (load-START teardown reverts to load-FINISH).

### Positioning / movement (single-capability, non-core) — IDs in CLSF fork

| # | Binding | AddrLib ID | Gate | Notes |
|---|---|---|---|---|
| 6 | TransformService request | **99963** (svc deref **883606**) | none in-repo (lives in CLSF fork) | thread-safe positioning; symptom = actors not teleporting to anchor |
| 7 | SetAnimationDriven / SetMotionDriven | **135316** / **135315** | none in-repo | anchored-scene movement switch + StopScene revert; not relied on for alignment (the compose-root pin does that); low-impact |

## Known-broken IDs — do NOT trust these from NAFSF/SAF lineage

Inherited AddressLib IDs silently re-bind to garbage across patches. These were already caught
re-binding wrong: **118488, 73213, 422688** (vtable; real = **451614**), **149852, 881086**.
Verify every inherited ID against expected bytes/slots before patching — never reuse one on faith.

## Re-verification procedure

1. **Confirm the break** — launch, read the log, list which rows above logged a mismatch.
2. **Update the version baseline** — if the game moved (e.g. .244 → .245), the AddressLib database
   and a matching versionlib are needed for the new build *first*; OSF ships a self-made versionlib
   for 1.16.244 (see the launch docs / Address Library dependency note).
3. **Re-derive moved targets** — for each failed row, use the OSF RE project to find the new
   address/slot and confirm the prologue bytes; update the in-repo constant (rows 1–2) or the CLSF
   `RE/IDs.h` ID (push the fork branch FIRST, then bump the submodule pointer).
4. **Update expected bytes** — if a prologue legitimately changed, update the expected-byte array in
   the owning file. Keep the gate; never remove it to "make it work".
5. **Rebuild + re-run the gate suite** — `xmake`, then the relevant suites in `TESTSUITE.md`
   (at minimum BOOT-01 + the suite covering the re-derived binding).
6. **Record the new build** — bump the RE-verified version in `main.cpp` + the AGENTS.md version
   line; note the date and what moved.

<a id="re-project"></a>
## RE project

Ground truth lives in the dedicated **OSF RE** project (`C:\Modding\Starfield\OSF RE`), Ghidra
context repo under `tools/ghidra/context_repo/modules/`. Canonical modules for this repo's bindings:
`gameplay.actor_transforms` (hooks, rig, transforms, movement), `engine.save_load` (rows 3–5, opType
enumeration), and — since the Phase-C merge — the Layer-C mechanism modules `gameplay.actor_equipment`,
`ui.fader_menu`, and the Wwise handoff (now used here, not external). `camera.state_machine` is consulted
but the free-fly path stays deferred. All pre-2026-06 findings proven on 1.16.242 and re-verified
at-gate on 1.16.244; 2026-06-12 findings are native 1.16.244.
