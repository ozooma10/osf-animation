# Nexus Mods page — OSF Animation

SFW-forward, neutral-framework positioning, styled after NAFSF's terse dev-facing
house style. Body below is Nexus BBCode, paste-ready into the description editor.
Supporting fields (summary / tags / requirements) at the bottom.

---

## DESCRIPTION (BBCode)

```bbcode
[center][size=6][b]OSF Animation[/b][/size][/center]
[center]A native animation & scene framework for Starfield.[/center]

[line]

[size=4][b]Overview[/b][/size]

OSF Animation is an SFSE plugin and a [b]complete framework[/b] for animation-driven scenes in Starfield — running entirely in native code, with no ESP, no Creation Kit, and no Papyrus animation hacks. It is the engine of the OSF framework family.

It is two things at once. Underneath is a fast, RE-verified [b]playback core[/b] — it plays skeletal clips and frame-locks any number of actors onto a single shared clock. On top of that is a full [b]scene runtime[/b]: scenes are authored as JSON [b]node graphs[/b] with timed track lanes that fire cues, actions, sound, and camera moves as a clip plays — a real sequencing engine, not just a "press play" call. You drive all of it from a clean two-layer [b]OSF.*[/b] Papyrus API.

It is deliberately [b]content-neutral[/b]. It ships the [i]mechanisms[/i] — play, sync, anchor, scene graphs, pack loading, plus neutral policy actions (player/camera lock, fade, equipment hide/restore, weapon sheathe, scheduled voice) — and nothing about what the animation is [i]for[/i]. Specific adult content and choreography are not part of this mod; they ship in the separate OSF Seduce content mod. That makes OSF Animation a clean foundation for machinima, dance, cutscenes, and NPC vignettes without any adult-content baggage.

It is a framework, not an animation pack: it ships a couple of small SFW demo scenes to prove it works, and nothing else. Content lives in separate packs — a folder of GLB animations plus a JSON descriptor — and a quest or scene mod drives it through the API.

[b]Coming from SAF?[/b] OSF includes a drop-in compatibility shim, so existing SAF playback, sync, and scene content runs on OSF unchanged. But OSF is not just a replacement — it's a new, more capable framework to build on. See [i]Compatibility[/i] below.

[line]

[size=4][b]Features[/b][/size]

[size=3][b]Playback core[/b][/size]
[list]
[*]Native GLTF/GLB playback of NAF-format clips (bones only), including gzip-compressed GLBs, straight onto Starfield's skeleton.
[*]Solo and synchronized scenes, 1 to N actors, played in lockstep on one shared clock.
[*]Low-level primitives below the scene layer: play a single clip in place, frame-lock several already-playing animations onto one clock, pin a pose to a world spot with selectable root-motion handling, scale or freeze speed, run a multi-phase sequence.
[*]Cross-fade blending on every seam — start, stage change, stop — instead of snapping.
[/list]

[size=3][b]Scene framework[/b][/size]
[list]
[*][b]Scenes as node graphs[/b] — author a scene as a graph of nodes (each an animation) with edges for navigation; advance, branch, or hold under script control.
[*][b]Timed track lanes[/b] — every node carries four synchronized lanes that fire on the clip's own clock: [i]cue[/i] (lifecycle + numeric + trigger-edge events), [i]action[/i], [i]sound[/i], and [i]camera[/i]. This is a sequencer, not a one-shot play call.
[*][b]Built-in policy actions[/b] — author [font=Courier New]osf.*[/font] actions directly in scene tracks: control lock/release, fade out/in, equipment hide/restore, weapon sheathe/restore, scheduled voice. A generalized [b]undo ledger[/b] reverses every reversible action automatically on any scene end — no cleanup choreography to author, and nothing left stuck if a scene is interrupted.
[*][b]World anchoring[/b] — start a scene anchored to any ObjectReference (furniture, bed, marker) with per-actor placement offsets; staged scenes auto-advance on a timer or loop count, or hold for manual control.
[*][b]Tag matchmaking[/b] — ask for a scene by tags + role filters (gender / keyword / race) and let OSF pick a fit across every installed scene def and pack, by priority tier and weighted random.
[*][b]Scene-event callbacks[/b] — register a Papyrus receiver and get async struct-payload events as scenes start, advance, hit cues, and end. Hook gameplay onto the timeline without polling.
[*][b]Roles, exclusivity, and load-safe handles[/b] — generational int handles that die cleanly across save-load, role binding, and per-actor scene queries.
[/list]

[size=3][b]Authoring & content[/b][/size]
[list]
[*]A JSON/GLB pack registry: tags, gender slots, multi-stage timelines, per-stage alignment offsets, with cross-pack animation-id collision detection.
[*]Per-user settings precedence (an action silently skips when its mechanism is disabled in [font=Courier New]Data\OSF\settings.json[/font]) and per-action field validation, with loud load-time rejection of unknown actions so authors are never surprised.
[*]Content-neutral by design — the engine names the mechanism, never the content; specific choreography and adult content live in the separate OSF Seduce content mod.
[/list]

[size=3][b]Compatibility & safety[/b][/size]
[list]
[*][b]Drop-in SAF compatibility shim[/b] — existing SAF mods' playback, sync, and scene calls run on OSF unchanged (a few advanced SAF-only entry points have no core equivalent and are inert, logged as SHIM-GAP).
[*]Standalone player-control / camera locks for scenes the player is part of (used by the shim; never auto-applied).
[*]Save-safe teardown on save-load, automatic re-binding of the Papyrus natives onto the rebuilt VM, version-gated engine bindings, and a co-install warning against NAFSF/SAF.
[/list]

[line]

[size=4][b]Roadmap — coming soon[/b][/size]

OSF ships as a [b]0.x beta[/b]: the Tier-0 playback primitives are stable, and the scene API is frozen-candidate but flagged "beta, may refine" until 1.0. The following are designed-in and additive (no API breakage) — landing in later updates:

[list]
[*][b]Free-fly / orbit scene cameras[/b] — scripted cinematic camera states beyond the current hold/lock, for machinima and cutscene framing.
[*][b]Positioned 3D audio[/b] — Wwise sound posted at a scene's world position, not just flat playback.
[*][b]Pool / set → clip resolution[/b] — richer pack metadata so a scene can request a [i]kind[/i] of animation and let OSF resolve the specific clip.
[*][b]Seamless live retargeting[/b] — swap an actor or transition scenes without a visible reset.
[*][b]Per-node role→slot remap[/b] and finer equipment control (slot-filtered hide, per-role restore).
[*][b]A Papyrus scene builder[/b] — assemble scenes at runtime from script, not just from authored JSON.
[/list]

[i]Stability promise:[/i] Tier-0 natives are never removed or re-signatured within a major version. The one allowed pre-1.0 break (scene-start calls returning an int handle) has already landed.

[line]

[size=4][b]Requirements[/b][/size]

[list]
[*][b]Starfield 1.16.244.0 — last verified build.[/b] OSF is built and tested against this version; it's the reference it's known-good on, not a hard lock. Engine bindings gate themselves and self-disable on a different build rather than crash, and the startup log states which build it's running on. On other builds playback may be unavailable until OSF is re-verified — so if it does nothing, check the log for a version note first.
[*]SFSE matching your game version.
[*]Address Library for SFSE (v21 format) — it provides the version database for your game build ([font=Courier New]versionlib-1-16-244-0.bin[/font] for 1.16.244) under [font=Courier New]Data\SFSE\Plugins\[/font]. Every SFSE plugin needs a version database matching the build it runs on to resolve engine addresses — without one, the plugin cannot load. OSF does [b]not[/b] bundle this file; install Address Library for your build (a standard 1.16.244 install already includes it).
[/list]

[line]

[size=4][b]Installation[/b][/size]

[list=1]
[*]Install SFSE and Address Library first; confirm they work.
[*]Install OSF Animation with a mod manager (MO2 recommended) and enable it. Make sure Address Library has supplied the version database for your build ([font=Courier New]versionlib-1-16-244-0.bin[/font]) under [font=Courier New]Data\SFSE\Plugins\[/font].
[*]Launch through SFSE.
[*]Check [font=Courier New]Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log[/font] — the first lines state the supported game version and what it is running on, then a one-line feature report says whether the playback hooks installed. Start here if the mod seems to do nothing: a version-mismatch warning means you are not on 1.16.244.
[/list]

[b]Do not run NAFSF or SAF alongside OSF Animation.[/b] They drive the same engine rig buffers and fight over the pose. OSF detects a co-install and warns in the log.

[line]

[size=4][b]For mod authors[/b][/size]

[b]Pack authors:[/b] ship JSON + GLB, no C++ and no plugin records. The schema is SLAL-shaped — actors with gender slots, multi-stage timelines, timers, loop counts, and per-stage alignment offsets. See the getting-started guide and schema reference in the docs.

[b]Quest / scene-mod authors:[/b] drive OSF from a two-layer [b]OSF.*[/b] API. Drop down to primitives (play a clip, frame-lock several, anchor, set speed, run a sequence) for raw bone playback, or work at the scene layer — start a scene by id, by world anchor, by roles, by tags (matchmade), or from ad-hoc files; then advance/navigate the graph, query state, and register a callback to hook gameplay onto scene events. Gate on a readiness handshake to keep OSF an optional dependency. SAF-dependent mods can use the shim. If your mod needs scene policy — equipment, voice, camera/control lock, fades — author it as [font=Courier New]osf.*[/font] action/cue tracks in the scene file and let the runtime (and its undo ledger) run it; specific adult content ships in the OSF Seduce content mod.

[line]

[size=4][b]Known limitations[/b][/size]

[list]
[*]An actor's physics capsule settles ~0.3 m off the visual anchor — cosmetic; judge alignment by what you see, not console position readouts.
[*]Scene actors must keep AI enabled — the engine won't animate AI-disabled NPCs.
[*]The anchor [i]additive[/i] root-motion mode (travelling root motion, rootMode 1) is experimental — it currently behaves like [i]pin[/i]. Pin and follow are the working modes.
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

> A native animation & scene framework for Starfield — synchronized multi-actor playback plus a scene-graph runtime with timed cue/action/sound/camera tracks, world anchoring, tag matchmaking, and scene-event callbacks, driven from a two-layer Papyrus API. Includes a drop-in SAF compatibility shim. Content-neutral: a framework for mod authors, not an animation pack.

**Suggested category:** Modders Resources / Utilities (it's a dependency, not end-user content).

**Suggested tags:** `SFSE`, `Framework`, `Animation`, `Modders Resource`, `Scripted`, `Utilities`.

**Requirements block (the structured "Requirements" tab):**
- Starfield Script Extender (SFSE)
- Address Library for SFSE Plugins
- Starfield 1.16.244.0 (last verified build — not a hard lock; bindings self-disable on other builds, see description)

**Resolved:**
- Address Library version data for 1.16.244 (`versionlib-1-16-244-0.bin`) — **NOT bundled**; it is provided by the Address Library for SFSE Plugins mod (a standard 1.16.244 install supplies it under `SFSE\Plugins\`). CommonLibSF hard-fails to load without a version database for the running build, so list Address Library as a hard requirement. No `.bin` ships in the OSF zip.

**Decisions still open for you:**
- Whether to cross-link the OSF Seduce content mod from this page once it ships (the scene engine is built into OSF Animation, not a separate download).
- A header image / a short scene clip (gif or YouTube embed) would carry this page more than any copy — `[youtube]ID[/youtube]` slots in cleanly.
```
