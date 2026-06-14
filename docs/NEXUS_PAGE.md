# Nexus Mods page — OSF Animation

SFW-forward, neutral-framework positioning, styled after NAFSF's terse dev-facing
house style. Body below is Nexus BBCode, paste-ready into the description editor.
Supporting fields (summary / tags / requirements) at the bottom.

---

## DESCRIPTION (BBCode)

```bbcode
[center][size=6][b]OSF Animation[/b][/size][/center]
[center]The native animation playback core for Starfield.[/center]

[line]

[size=4][b]Overview[/b][/size]

OSF Animation is an SFSE plugin that plays skeletal animations and drives synchronized, multi-actor scenes entirely in native code — no ESP, no Creation Kit, no Papyrus animation hacks. It is the [b]playback core[/b] of the OSF framework family, a clean GPL replacement for the NAF/SAF playback layer.

It is deliberately [b]content-neutral[/b]: it provides the mechanism — play clips, frame-lock actors on a shared clock, anchor and pin them, run staged scenes, load packs — and nothing about what the animation is for. Scene policy (undressing, scheduled voice, camera takeover, fade choreography, intimate-scene orchestration) is not part of this mod; it lives in the separate OSF Intimacy scene engine that builds on this core. That makes OSF Animation a clean foundation for machinima, dance, and NPC-vignette work without any intimate-framework baggage.

It is a framework, not an animation pack: it ships a couple of small SFW demo scenes to prove it works, and nothing else. Content lives in separate packs. A pack is a folder of GLB animations plus a JSON descriptor; a quest or scene mod drives playback through the [b]OSF.*[/b] Papyrus API.

[line]

[size=4][b]Features[/b][/size]

[list]
[*]Native GLTF/GLB playback of NAF-format clips (bones only), including gzip-compressed GLBs, straight onto Starfield's skeleton.
[*]Solo and synchronized scenes, 1 to N actors, played in lockstep on one shared clock around a world anchor.
[*]Playback primitives below the scene layer: play a single clip in place, frame-lock several already-playing animations onto one clock, pin a pose to a world spot with selectable root-motion handling, scale or freeze speed, run a multi-phase sequence.
[*]Staged scenes that advance on a timer or after a set number of loops, or hold for manual control.
[*]Cross-fade blending on every seam — start, stage change, stop — instead of snapping.
[*]A JSON/GLB pack registry: tags, gender slots, multi-stage timelines, per-stage alignment offsets, with cross-pack animation-id collision detection.
[*]Content-neutral by design — no undress, no voice, no camera takeover. The core is mechanism only; intimate-scene orchestration is the separate OSF Intimacy engine.
[*]Standalone player-control / camera locks available for scenes the player is part of (used by the SAF shim; never auto-applied).
[*]Save-safe teardown on save-load, and automatic re-binding of the Papyrus natives onto the rebuilt VM.
[*]Drop-in SAF compatibility shim — existing SAF-dependent mods run on OSF unchanged.
[/list]

[line]

[size=4][b]Requirements[/b][/size]

[list]
[*][b]Starfield 1.16.244.0 — last verified build.[/b] OSF is built and tested against this version; it's the reference it's known-good on, not a hard lock. Engine bindings gate themselves and self-disable on a different build rather than crash, and the startup log states which build it's running on. On other builds playback may be unavailable until OSF is re-verified — so if it does nothing, check the log for a version note first.
[*]SFSE matching your game version.
[*]Address Library for SFSE (v21 format). The version database for the verified build ([font=Courier New]versionlib-1-16-244-0.bin[/font]) is [b]bundled in the download[/b] and installs next to the plugin under [font=Courier New]Data\SFSE\Plugins\[/font]. Every SFSE plugin needs a version database matching the build it runs on to resolve engine addresses — without one, the plugin cannot load. The bundled copy covers the verified build, so a standard install needs nothing extra; on a different build, your Address Library install must supply that build's database.
[/list]

[line]

[size=4][b]Installation[/b][/size]

[list=1]
[*]Install SFSE and Address Library first; confirm they work.
[*]Install OSF Animation with a mod manager (MO2 recommended) and enable it. The bundled version database ([font=Courier New]versionlib-1-16-244-0.bin[/font]) installs alongside the plugin — leave it in place.
[*]Launch through SFSE.
[*]Check [font=Courier New]Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log[/font] — the first lines state the supported game version and what it is running on, then a one-line feature report says whether the playback hooks installed. Start here if the mod seems to do nothing: a version-mismatch warning means you are not on 1.16.244.
[/list]

[b]Do not run NAFSF or SAF alongside OSF Animation.[/b] They drive the same engine rig buffers and fight over the pose. OSF detects a co-install and warns in the log.

[line]

[size=4][b]For mod authors[/b][/size]

[b]Pack authors:[/b] ship JSON + GLB, no C++ and no plugin records. The schema is SLAL-shaped — actors with gender slots, multi-stage timelines, timers, loop counts, and per-stage alignment offsets. See the getting-started guide and schema reference in the docs.

[b]Quest / scene-mod authors:[/b] drive OSF from a two-layer [b]OSF.*[/b] API. Use primitives (play a clip, frame-lock several, anchor, set speed, run a sequence) for raw bone playback, or one-call mechanical scenes started by id, tags, or files. Gate on a readiness handshake to keep OSF an optional dependency and query state. SAF-dependent mods can use the shim. Natives are never removed or re-signatured within a major version. If your mod needs undress, voice, camera takeover, or intimate-scene orchestration, build on the OSF Intimacy scene engine, which adds that policy layer on top of this core.

[line]

[size=4][b]Known limitations[/b][/size]

[list]
[*]An actor's physics capsule settles ~0.3 m off the visual anchor — cosmetic; judge alignment by what you see, not console position readouts.
[*]Scene actors must keep AI enabled — the engine won't animate AI-disabled NPCs.
[/list]

[line]

[size=4][b]Credits[/b][/size]

[list]
[*][b]Deweh[/b] — Native Animation Framework SF (NAFSF), GPLv3. OSF's GLTF/ozz playback path is ported from and owes a great deal to NAFSF.
[*][b]mielu91m[/b] — Starfield Animation Framework (SAF) and SAF Seduce, studied for technique; the bundled SFW demo assets are used with the Seduce author's permission.
[*]The CommonLibSF template and library maintainers.
[/list]

[line]

[size=4][b]License[/b][/size]

GPL-3.0. Source is public; build instructions are in the repository.
```

---

## SUPPORTING FIELDS

**Short summary (Nexus "Summary" field, ~one sentence):**

> The native animation playback core for Starfield — synchronized multi-actor scenes, staged playback, anchoring, and a JSON/GLB pack format, driven from a small two-layer Papyrus API with a drop-in SAF compatibility shim. Content-neutral: a framework for mod authors, not an animation pack.

**Suggested category:** Modders Resources / Utilities (it's a dependency, not end-user content).

**Suggested tags:** `SFSE`, `Framework`, `Animation`, `Modders Resource`, `Scripted`, `Utilities`.

**Requirements block (the structured "Requirements" tab):**
- Starfield Script Extender (SFSE)
- Address Library for SFSE Plugins
- Starfield 1.16.244.0 (last verified build — not a hard lock; bindings self-disable on other builds, see description)

**Resolved:**
- Address Library version data for 1.16.244 (`versionlib-1-16-244-0.bin`) — **bundled in the zip** under `SFSE\Plugins\` next to the DLL, so the mod works regardless of whether the user's Address Library install already covers 1.16.244 (CommonLibSF hard-fails to load without a version database for the running build). AddrLib-format permission/credit is sorted (LAUNCH.md Phase 3). *Packaging action: include the `.bin` when you build the release archive.*

**Decisions still open for you:**
- Whether to cross-link the OSF Intimacy scene engine and OSF Seduce content from this page once they ship.
- A header image / a short scene clip (gif or YouTube embed) would carry this page more than any copy — `[youtube]ID[/youtube]` slots in cleanly.
```
