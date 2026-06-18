# Nexus Mods page — OSF Animation

SFW-forward, neutral-framework positioning with a stronger "next evolution of Starfield animation"
launch pitch. Body below is Nexus BBCode, paste-ready into the description editor.
Supporting fields (summary / tags / requirements) at the bottom.

---

## DESCRIPTION (BBCode)

```bbcode
[center][size=6][b]OSF Animation[/b][/size][/center]
[center]Native, synchronized, scene-graph animation for Starfield.[/center]

[line]

[size=4][b]Overview[/b][/size]

OSF Animation is a [b]fully native animation framework[/b] for Starfield — an SFSE plugin built to move actors, synchronize scenes, and drive animation timelines from optimized C++ instead of Papyrus tricks. No ESP. No Creation Kit records. No per-frame Papyrus animation loop. This is the engine layer of the OSF framework family.

Think of it as the next step after "play an animation": OSF gives Starfield a real native animation runtime. It imports GLB clips, samples them on a shared clock, stamps poses directly into the game's skeleton buffers, keeps multi-actor scenes frame-locked, anchors actors to the world, blends stage changes, and exposes the whole thing through a clean [b]OSF.*[/b] Papyrus API.

The playback core is only half of it. Above that is a full [b]scene graph runtime[/b]: scenes are JSON node graphs with branches, roles, tags, callbacks, and synchronized timeline lanes for cues, actions, sound, and camera control. It is a sequencing engine, not a one-shot animation call.

It is also deliberately [b]content-neutral[/b]. OSF ships the mechanisms — native playback, sync, positioning, anchoring, scene graphs, player/camera control locks, fade, equipment hide/restore, weapon sheathe/restore, scheduled voice, sound, camera holds, callbacks, validation, and automatic cleanup — but not adult choreography. Specific adult content lives in companion mods such as OSF Seduce; OSF Animation itself is a foundation for animation packs, machinima, dance, cutscenes, NPC vignettes, and quest scenes.

This is a framework, not an animation pack. It ships small SFW demo/test content so you can prove the install works. Real content lives in separate JSON + GLB packs and scene mods that drive OSF through the API.

[b]Coming from SAF?[/b] OSF includes a drop-in compatibility shim, so existing SAF playback, sync, and scene content runs on OSF unchanged. But OSF is not just a replacement — it's a new, more capable framework to build on. See [i]Compatibility[/i] below.

[line]

[size=4][b]Features[/b][/size]

[size=3][b]Playback core[/b][/size]
[list]
[*][b]Fully native C++ playback[/b] — GLTF/GLB skeletal clips, including gzip-compressed GLBs, imported and played through the SFSE plugin.
[*][b]Optimized pose stamping[/b] — OSF samples the clip once per actor per frame and writes directly to Starfield's rig buffers; Papyrus is only the command surface, not the animation engine.
[*][b]Shared-clock synchronization[/b] — solo, paired, and multi-actor scenes stay frame-locked on one native clock instead of drifting apart.
[*][b]World positioning and anchoring[/b] — pin actors to a marker, furniture, bed, or arbitrary ObjectReference with per-actor offsets and root-mode handling.
[*][b]Stage playback and speed control[/b] — play one clip, sync several clips, run multi-phase sequences, advance stages, scale speed, or freeze playback.
[*][b]Blending instead of snapping[/b] — cross-fade starts, stage changes, and stops.
[/list]

[size=3][b]Scene framework[/b][/size]
[list]
[*][b]Scenes as node graphs[/b] — author a scene as connected animation nodes with edges for advance, branch, trigger, timer, loops, and end.
[*][b]Native timeline lanes[/b] — each node can fire synchronized [i]cue[/i], [i]action[/i], [i]sound[/i], and [i]camera[/i] entries from the clip's own clock.
[*][b]Built-in native mechanisms[/b] — author [font=Courier New]osf.*[/font] actions directly in scene tracks: player/control lock, camera hold, fade out/in, equipment hide/restore, weapon sheathe/restore, and scheduled voice.
[*][b]Automatic cleanup ledger[/b] — reversible actions are tracked per scene and undone on any ending path, including interruption. No stuck fade. No forgotten strip restore. No abandoned control lock.
[*][b]World-anchored productions[/b] — start scenes at a placed ref, furniture, bed, or marker with per-participant placement offsets; staged scenes can auto-advance on timers/loop counts or wait for script control.
[*][b]Tag matchmaking[/b] — ask for a scene by tags + role filters (gender / keyword / race) and let OSF pick a fit across every installed scene def and pack, by priority tier and weighted random.
[*][b]Scene-event callbacks[/b] — register a Papyrus receiver and get async struct-payload events as scenes start, advance, hit cues, and end. Hook gameplay onto the timeline without polling.
[*][b]Roles, exclusivity, and load-safe handles[/b] — generational int handles that die cleanly across save-load, role binding, and per-actor scene queries.
[/list]

[size=3][b]Authoring & content[/b][/size]
[list]
[*][b]JSON + GLB packs[/b] — ship animations as loose GLB files plus a JSON descriptor: tags, gender slots, multi-stage timelines, timers, loop counts, and per-stage offsets.
[*][b]Fast iteration[/b] — reload packs/scenes from [font=Courier New]Data\OSF\[/font], validate scene files, and inspect load errors without building an ESP.
[*][b]User settings win[/b] — equipment, fade, sound/voice, and camera mechanisms respect [font=Courier New]Data\OSF\settings.json[/font]; disabled mechanisms silently skip rather than fighting the player.
[*][b]Load-time validation[/b] — bad scenes are rejected loudly with diagnostics, including unknown [font=Courier New]osf.*[/font] actions, invalid roles, bad refs, and impossible graph edges.
[*]Content-neutral by design — the engine names the mechanism, never the content; specific choreography and adult content live in the separate OSF Seduce content mod.
[/list]

[size=3][b]Compatibility & safety[/b][/size]
[list]
[*][b]Drop-in SAF compatibility shim[/b] — existing SAF mods' playback, sync, and scene calls run on OSF unchanged (a few advanced SAF-only entry points have no core equivalent and are inert, logged as SHIM-GAP).
[*]Standalone player-control / camera locks for scenes the player is part of (used by the shim; never auto-applied).
[*][b]Version-gated native bindings[/b] — engine hooks verify before use and self-disable on mismatch instead of blindly patching a moved target.
[*]Save-safe teardown on save-load, automatic re-binding of the Papyrus natives onto the rebuilt VM, and a co-install warning against NAFSF/SAF.
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

> The next evolution of Starfield animation: a fully native SFSE engine with optimized GLB playback, multi-actor sync, anchoring/positioning, scene graphs, equipment strip/restore, fade, voice/sound, camera/control locks, callbacks, and SAF shim.

**Suggested category:** Modders Resources / Utilities (it's a dependency, not end-user content).

**Suggested tags:** `SFSE`, `Framework`, `Animation`, `Native`, `Performance`, `Modders Resource`, `Scripted`, `Utilities`.

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
