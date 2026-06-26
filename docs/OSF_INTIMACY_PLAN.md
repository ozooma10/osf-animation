# OSF Intimacy - Comprehensive Foundation Plan

Status: design plan  
Working product name: OSF Intimacy  
Public API prefix: `OSFIntimacy`  
Authored content extension: `*.intimacy.json`  
Relationship to OSF Animation: separate gameplay framework built on top of OSF

## 1. Executive Summary

OSF Intimacy is the dedicated intimacy and adult-scene gameplay framework that sits above OSF Animation. OSF remains the content-neutral animation and scene runtime: playback, shared clocks, graph sequencing, anchoring, camera mechanisms, equipment hide/restore, sound, Papyrus bindings, scene events, and save-safe teardown. OSF Intimacy owns the adult/intimacy semantics: actor capability resolution, arousal, scene scoring, context-aware selection, player-facing chooser UI, user preferences, diagnostics, addon APIs, and the higher-level gameplay loop.

The first lovable release should not be a minigame-first product. It should be a calm, polished watch-mode framework that feels alive even when the player does nothing: scenes select well, anchor well, progress with per-actor arousal, pace automatically, provide clear subtle feedback, expose clean controls, and tell authors exactly why content did or did not load or match. Interactivity then layers on top: first reversible equip-object/body-effect infrastructure, then the Dual-Meter soft-mastery game, then governed autonomy and broader world simulation.

The framework should be Starfield-centric in its foundations. Ships, habs, outposts, beds, privacy, nearby furniture, gravity, suits, faction spaces, and companion/crew context should materially influence candidate scoring, scene prompts, anchors, and events. They should not usually hard-block scenes in core. OSF Intimacy should provide facts, scoring, events, and hooks so other mods can be more opinionated.

The ecosystem contract matters as much as the player loop. OSF Intimacy should expose a small, semantic, versioned Papyrus API and a versioned native C ABI. Content authors should be able to ship data-only `*.intimacy.json` packs without Papyrus or ESP registration. Addon authors should be able to start scenes by intent, inspect candidates and reasons, observe events, and extend actor/location/capability resolution without forking the framework.

## 2. Core Decisions

- Product identity: gameplay framework, not only an animation broker.
- Architecture: separate sibling SFSE plugin/project, not an adult module inside OSF.
- OSF boundary: OSF remains content-neutral; OSF Intimacy owns adult/intimacy semantics and policy metadata.
- First release feel: polished calm watch mode with optional controls.
- Public naming: `OSFIntimacy` for Papyrus, native APIs, logs, and docs.
- Authored files: `*.intimacy.json`.
- Content posture: framework-safe release with non-explicit demos, templates, validator, and diagnostics.
- User filters: used by the chooser and scoring model, not as global hard refusal gates.
- Bad content behavior: per-file/per-scene isolation with stable diagnostic codes.
- Scene start model: context prompt and intent query as primary entrypoints.
- Actor scope: adult human actors, solo and paired scenes optimized first.
- Starfield hook: location systems first, especially anchors, privacy, furniture, ships/habs/outposts, suits, and environment scoring.
- Input model: separate control axes: pace, navigation, and camera.
- In-scene stop: stop requests are always accepted; the runtime exits at the nearest safe transition within a short maximum delay.
- Persistence: native co-save for compact actor state, settings, and reversible-effect cleanup debt.
- OSF dependency: finish the unified OSF scene schema before serious OSF Intimacy feature work.
- OSF integration: OSF exposes a versioned native C ABI; OSF Intimacy calls that ABI rather than private internals or Papyrus glue.

## 3. Goals

### 3.1 Product Goals

OSF Intimacy should become the foundation other Starfield adult/intimacy mods build against. It should provide one consistent way to:

- describe intimacy scenes as data;
- select scenes by actor, intent, location, anchor, tags, and preferences;
- bind actors to roles using capabilities instead of brittle gender-only assumptions;
- run scenes through OSF's graph runtime;
- track per-actor arousal and scene state natively;
- expose scene state through stable events;
- provide user-facing controls and diagnostics;
- let addon mods contribute policy, content, actors, locations, UI, and consequences without duplicating the framework.

### 3.2 Engineering Goals

- Keep all hot-path state native. Papyrus is for integration convenience, not per-frame simulation.
- Avoid save bloat. Persist compact durable facts, not full scene logs or script-heavy scene state.
- Make reversible effects native and self-healing on load.
- Keep OSF Intimacy's core robust even when community packs are malformed.
- Treat diagnostics as a product feature, not a debug afterthought.
- Keep public APIs small, semantic, versioned, and append-only within a major version.
- Use stable machine-readable diagnostic codes for tools, logs, UI, and user support.

### 3.3 Ecosystem Goals

- Data-only content packs should be viable.
- Plain OSF scenes should be usable with lower-confidence inferred metadata where possible.
- Third-party mods should be able to ask for an intent rather than hardcode scene ids.
- Addons should receive structured reasons when a scene fails, ranks low, or is skipped.
- Framework settings and content metadata should be transparent enough for modpacks.

## 4. Non-Goals

These are explicitly not part of the first lovable release:

- A full romance or relationship simulator in core.
- NPC autonomy as a Phase 1 feature.
- A hard global morality/policy gate for user category filters.
- A score/XP/perk progression system in core.
- Dynamic facial expressions, overlays, wetness, morphs, or complex body visual systems before technical spikes prove they are reliable in Starfield.
- Importing old SexLab/OStim/AAF content automatically. The skeletons, animation assets, metadata assumptions, and engine behavior differ too much for an importer to be honest.
- A large first-party adult content pack bundled with the framework.

## 5. Relationship To OSF Animation

### 5.1 OSF Owns

OSF Animation remains the engine layer. It owns:

- native `.glb` and `.af` playback;
- ozz sampling and Starfield pose stamping;
- shared scene clocks and sync groups;
- actor graph state and per-frame animation hooks;
- OSF scene graph runtime;
- node transitions, timers, loops, cues, actions, sound, and camera tracks;
- world anchoring and participant placements;
- input grant plumbing and base director verbs;
- player control lock, camera lock, fade, equipment hide/restore, and sound mechanisms;
- OSF scene registry and `*.osf.json` schema;
- save/load teardown for OSF scene and graph state;
- OSF Papyrus API and OSF scene events.

### 5.2 OSF Intimacy Owns

OSF Intimacy owns everything that gives an intimacy scene meaning:

- `*.intimacy.json` loading and validation;
- adult/intimacy tag vocabulary;
- actor capability and role resolution;
- location, privacy, furniture, suit, and anchor scoring;
- scene intent queries and candidate ranking;
- compact chooser UI;
- per-actor arousal and climax state;
- watch-mode auto pacing;
- optional manual and director controls;
- soft outcome and mood/memory summaries;
- user settings and presets;
- diagnostics and failure reasons;
- addon APIs;
- native persistence for OSF Intimacy state;
- reversible OSF Intimacy body/equip-effect ledger.

### 5.3 Required OSF Upstream Work

Phase 0 must complete these OSF-side capabilities before OSF Intimacy's real Phase 1 begins:

- Finish the unified `*.osf.json` schema migration so OSF has one stable scene definition format.
- Add a versioned exported OSF C ABI, for example `OSF_RequestAPI`.
- Add generic in-memory scene registration/import so OSF Intimacy can compile scenes without writing generated `.osf.json` files to disk.
- Add reload coherence: OSF Intimacy needs to know when OSF reloads and must be able to trigger a unified reload.
- Expose scene start/stop/navigate/speed/event functions through the OSF ABI.
- Expose input grant and verb registration needed by OSF Intimacy.
- Extend OSF input verbs beyond the current set where needed for position/focus navigation.

### 5.4 OSF Work That Should Not Block Phase 1

The following are valuable, but should not block the first OSF Intimacy release:

- positioned or distance-attenuated audio;
- mature freecam/reposition tools beyond current OSF capabilities;
- richer custom action payloads;
- structured required/blocking custom actions;
- actor scale normalization;
- native generic reversible mechanism registration for third-party plugins.

For the first release, custom OSF action beats should be treated as timed triggers only. OSF Intimacy resolves rich meaning from its own native scene state keyed by handle, actor, role, node, and stage.

## 6. Project Layout

OSF Intimacy should be developed as a sibling project/repo, not as a permanent subdirectory of OSF Animation.

Suggested layout:

```text
OSF Intimacy/
  xmake.lua
  src/
    API/
    Arousal/
    Authoring/
    Context/
    Diagnostics/
    Events/
    Runtime/
    Save/
    UI/
  include/
    OSFIntimacyAPI.h
  dist/
    Scripts/
      Source/
        OSFIntimacy.psc
        OSFIntimacyTypes.psc
    OSFIntimacy/
      Demos/
      Templates/
      Settings/
  docs/
    API.md
    AUTHORING.md
    DIAGNOSTICS.md
    TAGS.md
```

The build should produce:

- `OSFIntimacy.dll`;
- `OSFIntimacy.psc` and `OSFIntimacyTypes.psc`;
- non-explicit demo/template content;
- JSON schemas and docs;
- optional command-line or in-game validation tooling.

If Papyrus source files are edited, do not compile `.pex` during automated agent work in the OSF repo. Update source and docs only; the user manually recompiles Papyrus scripts before in-game testing.

## 7. Authored Content Model

### 7.1 Authoring Principle

Authors write intimacy semantics, not OSF node graphs. A `*.intimacy.json` file describes roles, tags, acts, stage semantics, stimulation, equipment policy, location preferences, and authored beats. OSF Intimacy compiles that into OSF graph scenes registered through the OSF native ABI.

Advanced authors can still target OSF graphs directly, but the common path should be data-first and approachable.

### 7.2 File Shape

The exact schema will be versioned, but the core shape should be:

```json
{
  "schema": 1,
  "id": "author.pack",
  "name": "Author Pack",
  "defaults": {
    "tone": ["tone:romantic"],
    "equipmentPolicy": "contextual",
    "location": {
      "prefer": ["loc:private", "furniture:bed"],
      "avoid": ["loc:combat", "loc:hazard"]
    }
  },
  "scenes": [
    {
      "id": "paired.basic",
      "name": "Paired Basic",
      "actors": 2,
      "tags": [
        "count:2",
        "stage:arc5",
        "tone:romantic",
        "intensity:low"
      ],
      "roles": [
        {
          "id": "lead",
          "capabilities": ["cap:touch", "cap:active"]
        },
        {
          "id": "partner",
          "capabilities": ["cap:touch", "cap:receive"]
        }
      ],
      "acts": [
        {
          "stage": "foreplay",
          "osf": {
            "clips": {
              "lead": "OSF/Example/Lead_Foreplay.af",
              "partner": "OSF/Example/Partner_Foreplay.af"
            }
          },
          "maxStimulation": 35,
          "stimulation": {
            "lead": 2.0,
            "partner": 2.5
          },
          "speeds": [0.75, 1.0]
        },
        {
          "stage": "main",
          "osf": {
            "clips": {
              "lead": "OSF/Example/Lead_Main.af",
              "partner": "OSF/Example/Partner_Main.af"
            }
          },
          "maxStimulation": 100,
          "stimulation": {
            "lead": 8.0,
            "partner": 8.5
          },
          "speeds": [0.75, 1.0, 1.25, 1.5]
        },
        {
          "stage": "aftercare",
          "osf": {
            "clips": {
              "lead": "OSF/Example/Lead_Aftercare.af",
              "partner": "OSF/Example/Partner_Aftercare.af"
            }
          },
          "maxStimulation": 20,
          "stimulation": {
            "lead": 0.5,
            "partner": 0.5
          },
          "speeds": [1.0]
        }
      ]
    }
  ]
}
```

The schema should avoid making every field mandatory. Minimal valid content should be easy, but high-quality content should be able to provide enough metadata for excellent scoring and diagnostics.

### 7.3 Five-Stage Default Arc

The default scene grammar is:

- `foreplay`
- `escalation`
- `main`
- `climax`
- `aftercare`

Authors can omit stages or define custom stages, but the default compiler should understand the five-stage arc and produce OSF graph edges accordingly.

The five-stage arc is important because it gives scenes a shape without forcing every author to design graphs manually:

- `foreplay`: lower stimulation, lower max arousal, no climax.
- `escalation`: stronger stimulation, movement toward main act.
- `main`: primary act, full arousal range.
- `climax`: beat window or authored climax stage.
- `aftercare`: scene breathing room, afterglow, rest, or resolution.

### 7.4 Compilation To OSF

For each `*.intimacy.json` scene, the compiler should:

- validate schema and tags;
- resolve inheritance/defaults;
- assign stable generated OSF scene ids;
- lower roles to OSF roles in deterministic order;
- lower each act to one or more OSF nodes;
- lower stage flow to OSF graph edges;
- attach `osfintimacy.*` custom action beats at relevant timing marks;
- attach sound/camera cues only where the author provided them or defaults apply;
- register the generated OSF scene through the OSF C ABI;
- store a native mapping from OSF scene handle/node/role back to OSF Intimacy scene/stage/actor state.

Generated OSF scene ids should be deterministic and debuggable:

```text
osfintimacy.<author>.<pack>.<scene>.<variant>
```

If an author-provided id already includes a namespace, OSF Intimacy should still normalize it into the `osfintimacy.*` generated namespace while preserving the author id in diagnostics and public metadata.

## 8. Tag Taxonomy

### 8.1 Tag Policy

OSF Intimacy should use a validated namespaced vocabulary. Unknown non-critical tags should warn, not break unrelated files. Known tags should be documented and stable. Invalid required fields, broken assets, impossible role bindings, and malformed schema should skip only the affected file or scene.

### 8.2 Initial Tag Dimensions

Standardize these dimensions first:

- `count:*` - actor count, such as `count:1`, `count:2`.
- `stage:*` - stage/arc tags, such as `stage:foreplay`, `stage:main`, `stage:aftercare`, `stage:arc5`.
- `role:*` - semantic roles, such as `role:lead`, `role:partner`, `role:active`, `role:receiver`.
- `cap:*` - actor capabilities, body/skeleton requirements, and role constraints.
- `act:*` - clinical act type vocabulary.
- `intensity:*` - low, medium, high, extreme, etc.
- `tone:*` - romantic, casual, transactional, playful, comfort, ritual, aggressive, etc.
- `loc:*` - private, public, ship, hab, outpost, club, hostile, hazardous, etc.
- `furniture:*` - bed, chair, floor, wall, shower, medical, custom marker, etc.
- `equip:*` - clothed, partial, full, suit-safe, contextual, etc.
- `flag:*` - content flags and user-facing filter dimensions.
- `source:*` - first-party, inferred-osf, adapter, addon, test, etc.

### 8.3 Clinical Language

Tags and docs should be direct enough for precision but not sensational. Use clear clinical names where exact act semantics matter. Player UI can soften labels where that improves presentation, but metadata and diagnostics should remain searchable and unambiguous.

## 9. Runtime Architecture

### 9.1 Loader

The loader scans:

```text
Data/OSFIntimacy/**/*.intimacy.json
```

It should:

- parse all files with per-file isolation;
- validate schema version;
- validate known tags and warn on unknown optional tags;
- validate referenced OSF clips/assets where possible;
- build raw content records;
- compile records to OSF scene definitions;
- register generated scenes through OSF;
- build the candidate index;
- store load diagnostics with stable codes.

### 9.2 Candidate Index

The candidate index is the heart of scene selection. It should support:

- actor count;
- role/capability fit;
- actor preferences;
- current actor state;
- caller intent;
- user chooser filters;
- location and anchor scoring;
- furniture compatibility;
- equipment policy compatibility;
- confidence level for inferred metadata;
- priority and weight;
- detailed scoring reasons.

The index should support both authored OSF Intimacy scenes and inferred plain OSF scenes. Inferred OSF scenes should be marked with lower confidence and clear diagnostic warnings where metadata is guessed.

### 9.3 Intent Query

Other mods should ask for scenes by intent:

- actors;
- optional location/context ref;
- optional anchor;
- all-of/any-of/none-of tags;
- desired tone;
- desired stage or act class;
- caller-provided relationship/context hints;
- equipment preference;
- whether UI confirmation is required.

The result should not be only success/failure. It should include:

- selected candidate id;
- generated OSF scene id;
- actor-role binding;
- anchor selected;
- confidence;
- scoring summary;
- rejection reasons for close candidates;
- warnings.

### 9.4 Actor Capability Resolver

Use a layered resolver:

1. Explicit addon/provider data.
2. User overrides.
3. Equipment/body metadata.
4. Actor/base form data.
5. Conservative default inference.

The resolver should provide:

- body/skeleton class;
- role capabilities;
- equipment-dependent capabilities;
- scene participation status;
- preference hints;
- temporary state;
- confidence.

Phase 1 should optimize adult human actors and solo/pair scenes. Non-default skeletons and body classes should be extension-provider territory until proven.

### 9.5 Actor State

Core actor state should be compact:

- current scene handle;
- current role;
- arousal state;
- current stage;
- current equipment policy state;
- cooldowns;
- light preferences;
- last partner/time/outcome summaries;
- afterglow/mood markers;
- addon-provided relationship hints.

Core should not persist detailed scene logs or full sexual history. Addons can subscribe to events if they want richer long-term simulation.

### 9.6 Location And Anchor Scanner

The context scanner should derive:

- current cell/location tags;
- ship/hab/outpost/interior/exterior hints;
- nearby furniture and anchor candidates;
- privacy estimate;
- witness/risk estimate;
- gravity/suit/hazard hints where available;
- camera suitability;
- spacing and collision risk where feasible.

The scanner ranks anchors rather than simply choosing actor[0]. It should consider:

- scene requirements;
- actor count;
- furniture tags;
- distance to selected actors;
- privacy;
- obstruction/camera quality;
- current combat/hazard context;
- whether actors can plausibly reach/face the anchor.

Environment should primarily affect scoring and emitted events, not hard-block scenes.

### 9.7 Equipment Policy

Core OSF Intimacy should provide policy presets:

- `none`
- `partial`
- `full`
- `contextual`
- `author`

The mechanism can still call OSF's equipment hide/restore where appropriate. OSF Intimacy owns the policy decision: what should be hidden, whether suits should be preserved, whether a location implies clothing constraints, and whether an addon vetoes a piece of equipment.

Detailed per-slot compatibility and special equipment mods should be extension points.

### 9.8 Scene Lifecycle

A typical start flow:

1. Caller opens chooser or calls `StartByIntent`.
2. Runtime resolves actors and context.
3. Candidate index ranks compatible scenes.
4. Runtime selects a candidate or displays chooser.
5. Runtime resolves actor-role binding.
6. Runtime chooses an anchor.
7. Runtime applies equipment policy.
8. Runtime starts the generated OSF scene by id or registration handle.
9. Runtime records OSF Intimacy scene state keyed by OSF handle.
10. Runtime emits `OSFIntimacy_SceneStart`.
11. Arousal and auto-pacing begin.
12. OSF events and `osfintimacy.*` custom beats feed OSF Intimacy state.
13. Runtime exits via normal graph end, stop request, failure, reload, save/load teardown, or addon stop.
14. Native ledger reverses effects and reconciles cleanup debt.
15. Runtime emits `OSFIntimacy_SceneEnd`.

### 9.9 Stop Semantics

The user selected "usually, not always" for immediate stop, with a fast clean exit guarantee. Therefore:

- Stop requests must always be accepted.
- The framework should not ignore or swallow a stop request.
- The runtime may route to the nearest safe transition rather than hard-aborting instantly.
- A short maximum delay should be enforced.
- If the safe transition cannot be reached, force stop and run cleanup.
- Stop reason should be visible in events and diagnostics.

This gives authored scenes room to land cleanly without giving content indefinite control over the player.

## 10. Arousal And Watch-Mode Core

### 10.1 First Release Loop

The Phase 1 loop should include:

- per-actor arousal meters;
- independent climax state;
- refractory and afterglow;
- automatic stage/navigation where authored;
- automatic speed pacing driven by arousal;
- subtle feedback through voice/sound, screen/camera effects, rumble, and UI meters;
- manual pace controls;
- director navigation controls where authored;
- clear end/stop behavior.

### 10.2 Arousal State

Per actor/role, track:

- `arousal`
- `baseline`
- `libido`
- `maxArousal`
- `decay`
- `refractoryUntil`
- `orgasmCount`
- `stage`
- `isStimulated`
- `afterglow`

The exact tuning can evolve, but the model should be deterministic, data-tunable, and native. It should not depend on Papyrus polling.

### 10.3 Independent Climax

Climax is per actor, not a shared scene timer. It should occur when:

- the actor's arousal reaches the threshold;
- the actor is currently stimulated;
- the current stage/act allows climax;
- refractory has elapsed.

Climax should emit an event and feed scene progression, but should not necessarily end the scene. Default scene flow should continue to aftercare or resolve once participant conditions are satisfied.

### 10.4 Automatic Speed Pacing

Watch mode should visibly connect arousal and animation pace:

- low arousal favors slower speeds;
- rising arousal ramps toward stronger speeds;
- speed changes are smoothed;
- speed can ramp down when stimulation decreases;
- manual speed input can override or nudge the current pace depending on preset.

### 10.5 Feedback

Phase 1 feedback should use proven or low-risk systems:

- sound and voice pool selection;
- camera/screen effects;
- controller rumble;
- meters and stage labels;
- subtle HUD prompts;
- optional simple reversible visual effect if safe.

Do not make Phase 1 depend on dynamic overlays, facial morphs, or complex body visuals.

## 11. Player UX

### 11.1 Context Prompt

The default start UX is a context prompt that opens a compact chooser. It should be callable by:

- hotkey;
- Papyrus API;
- addon API;
- possible future dialogue integration.

The prompt should inspect selected actors, current location, nearby anchors, user settings, and caller intent.

### 11.2 Compact Chooser

The chooser should show:

- compatible intents/scenes;
- actor fit;
- anchor/location fit;
- confidence;
- short tags;
- warnings;
- why a candidate is recommended;
- filter controls;
- start/cancel.

The chooser should feel like a practical in-game tool, not a landing page or marketing UI.

### 11.3 Control Presets

Expose clear presets:

- Watch: auto pace, auto navigation, locked/cinematic camera.
- Manual: manual pace, auto navigation, normal end controls.
- Director: manual navigation and position/focus controls, advanced camera.

Internally, these map to independent axes:

- pace: Auto or Manual;
- navigation: Auto or Director;
- camera: Locked or Free.

### 11.4 Input Support

Phase 1 should design for keyboard and controller from the start.

Current OSF verbs include:

- advance;
- speed up;
- speed down;
- speed reset;
- pause;
- freecam toggle;
- end.

OSF Intimacy should prioritize missing position/focus verbs:

- next/previous position;
- actor focus;
- role focus;
- variant selection;
- swap/focus selection where authored.

Dual-Meter-specific actions should come later with Phase 2b.

### 11.5 Settings

Settings should exist in both:

- in-game UI for normal users;
- JSON files for advanced users and modpack defaults.

Initial settings groups:

- control preset;
- camera preset;
- feedback intensity;
- screen FX intensity;
- rumble intensity;
- chooser filters;
- pacing speed;
- manual control behavior;
- diagnostics verbosity;
- author/debug mode.

## 12. Diagnostics

Diagnostics are core product surface.

### 12.1 Doctor Panel

Ship an AAF Doctor-style diagnostics panel early. It should report:

- loaded packs;
- skipped files;
- skipped scenes;
- missing clips/assets;
- invalid schema fields;
- unknown tags;
- role/capability mismatches;
- generated OSF ids;
- candidate ranking reasons;
- rejection reasons;
- anchor selection reasons;
- active scene state;
- active input grants;
- arousal state;
- cleanup ledger/debt;
- reload status.

### 12.2 Stable Diagnostic Codes

Every important diagnostic should have a stable machine-readable code:

```text
OSI_SCHEMA_UNKNOWN_VERSION
OSI_SCHEMA_MISSING_FIELD
OSI_TAG_UNKNOWN
OSI_ASSET_MISSING
OSI_ROLE_UNBOUND
OSI_CAPABILITY_MISMATCH
OSI_ANCHOR_NONE
OSI_CANDIDATE_FILTERED
OSI_CANDIDATE_LOW_CONFIDENCE
OSI_RUNTIME_OSF_START_FAILED
OSI_RUNTIME_STOP_TIMEOUT
OSI_LEDGER_RECONCILED
```

The exact prefix can be `OSI_` internally even if the public API is `OSFIntimacy`, or the project can use `OSFINT_`. Choose one before implementation and keep it stable. Recommendation: use `OSI_` for diagnostic codes because it is compact and readable.

### 12.3 Failure Reporting

Failed scene requests should return:

- result code;
- human-readable summary;
- structured diagnostic entries;
- candidate count;
- top rejection reasons;
- whether the failure was technical, no-match, user-filtered, no-anchor, OSF failure, or runtime failure.

Do not force callers to scrape logs for normal failures.

## 13. Public API Shape

### 13.1 Papyrus Surface

Expose a hidden native global script:

```papyrus
ScriptName OSFIntimacy Native Hidden
```

The exact signatures must account for Papyrus limitations, especially arrays inside structs. The API should still present these concepts:

- readiness/version:
  - `IsReady`
  - `GetVersion`
  - `GetAPIVersion`
  - `HasCapability`
- reload:
  - `Reload`
- scene start:
  - `OpenChooser`
  - `StartByIntent`
  - `StartScene`
  - `StopScene`
  - `RequestStop`
- discovery:
  - `FindCandidates`
  - `GetCandidateCount`
  - `GetCandidateId`
  - `GetCandidateScore`
  - `GetCandidateReason`
- state:
  - `GetSceneStage`
  - `GetActorArousal`
  - `GetSceneActors`
  - `GetSceneTags`
- events:
  - `RegisterCallback`
  - `UnregisterCallback`
- diagnostics:
  - `GetLoadDiagnosticCount`
  - `GetLoadDiagnostic`
  - `GetLastFailureCode`
  - `GetLastFailureDetail`

Convenience should matter, but avoid hundreds of mechanical permutations. Prefer one or two expressive query functions over OStim-style API sprawl.

### 13.2 Native ABI

Expose:

```cpp
extern "C" __declspec(dllexport)
bool OSFIntimacy_RequestAPI(std::uint32_t a_requestedVersion, OSFIntimacyAPI** a_out);
```

The native API should be a versioned function table. Use POD structs, fixed-size fields or explicit buffers, and `const char*` for ABI stability.

Core native API groups:

- version/capability;
- scene intent start;
- candidate discovery;
- event listener registration;
- actor capability providers;
- location/context providers;
- reversible effect registration;
- diagnostics access.

### 13.3 Event Vocabulary

OSF Intimacy events should be a superset of OSF scene events. They should include raw OSF context where available:

- handle;
- OSF scene id;
- OSF node id;
- action/cue id;
- clip-local time;
- actor;
- role;
- anchor;
- result.

Add OSF Intimacy fields:

- Intimacy scene id;
- stage;
- intent;
- tags;
- arousal;
- climax state;
- selected anchor;
- equipment policy;
- stop reason;
- diagnostics.

Initial event names:

```text
OSFIntimacy_SceneStart
OSFIntimacy_StageChange
OSFIntimacy_ArousalChanged
OSFIntimacy_Climax
OSFIntimacy_AnimationChange
OSFIntimacy_ActorAdded
OSFIntimacy_ActorRemoved
OSFIntimacy_StopRequested
OSFIntimacy_SceneEnd
OSFIntimacy_CleanupReconciled
OSFIntimacy_Diagnostic
```

Events should be append-only within a major API version.

## 14. Persistence And Save Safety

### 14.1 Native Co-Save

Persist compact native state:

- framework version;
- settings;
- actor summaries;
- relationship/context hints that OSF Intimacy owns;
- cooldowns;
- afterglow/mood markers;
- reversible effect debt;
- migration version.

Do not persist full running scene graphs or Papyrus-heavy scene state.

### 14.2 Reversible Ledger

OSF Intimacy must have its own native reversible ledger for effects it owns:

- equip objects;
- temporary meshes;
- future overlays;
- future morphs;
- future body effects.

The ledger must:

- record applied effect debt;
- reverse in deterministic order;
- be idempotent;
- run on normal scene end;
- run on stop;
- run on OSF teardown/load replacement;
- reconcile orphaned debt on load.

This is mandatory before Phase 2 body/equip visuals become real features.

## 15. Roadmap

### Phase 0 - OSF Dependencies

Deliver in OSF Animation:

- unified `*.osf.json` scene schema;
- versioned OSF C ABI;
- in-memory scene registration/import;
- reload coherence;
- scene control functions through ABI;
- event subscription or event bridge through ABI;
- expanded input verbs for position/focus where needed.

Acceptance:

- OSF can accept a generated scene def from another plugin.
- OSF can reload without stale registered scenes.
- OSF Intimacy can start, navigate, set speed, stop, and observe events without Papyrus glue.
- OSF tests cover schema migration and registration/import behavior.

### Phase 1 - Core Watch-Mode Framework

Deliver in OSF Intimacy:

- `OSFIntimacy.dll`;
- `OSFIntimacy.psc` and `OSFIntimacyTypes.psc` sources;
- `*.intimacy.json` loader;
- schema validator;
- compiler to OSF graph scenes;
- native candidate index;
- intent query;
- compact chooser UI;
- actor capability resolver;
- location/anchor scanner;
- equipment policy presets;
- per-actor arousal core;
- automatic speed pacing;
- Watch/Manual/Director presets;
- event rebroadcaster;
- native co-save;
- structured diagnostics;
- Doctor panel;
- non-explicit demos and author templates.

Acceptance:

- A data-only demo pack loads.
- Bad demo content is isolated and diagnosed.
- A context prompt ranks scenes with reasons.
- A scene starts through OSF using generated registration.
- Actors bind to roles deterministically.
- Arousal changes over time.
- Speed visibly follows watch-mode pacing.
- Scene can cleanly end through natural end and stop request.
- Save/load during or after a scene does not leave stale OSF Intimacy state.
- Papyrus and native APIs expose version, reload, start, candidate reasons, events, and diagnostics.

### Phase 2a - Reversible Equip-Object And Visual Infrastructure

Deliver:

- native reversible ledger hardening;
- attach/detach equip-object system;
- simple mesh-based visual effects;
- save/load reconciliation tests;
- diagnostics for unreverted effects.

Acceptance:

- Equip object applies and reverts on normal end.
- Equip object reverts on stop.
- Equip object reverts on save/load teardown.
- Orphaned effect debt reconciles on next load.

### Phase 2b - Stamina And Dual-Meter Interaction

Deliver:

- stamina/effort model before minigames become load-bearing;
- Dual-Meter soft-mastery mode;
- meter UI;
- accessibility sliders;
- controller and keyboard bindings;
- no hard-fail scene outcomes.

Dual-Meter feel:

- player manages self and partner arousal;
- choices affect pacing, timing, feedback, and outcomes;
- poor play does not hard-fail scenes;
- Watch mode remains fully supported and never feels second-class.

Acceptance:

- Dual-Meter can be disabled completely.
- Watch mode behavior remains stable.
- Manual control and Dual-Meter actions do not fight OSF input.
- Stamina prevents infinite high-speed optimal play.

### Phase 3 - Autonomy And World Layer

Deliver:

- NPC scheduler;
- per-cell/ship/location permission policy;
- privacy/witness events;
- concurrency governor;
- distance/positioned audio dependency;
- addon hooks for social consequences.

Acceptance:

- Autonomy can be globally disabled.
- Concurrency is capped.
- Background scenes do not spam full-volume audio.
- Privacy/witness logic emits events without forcing core reputation consequences.

### Phase 4 - Ecosystem Maturity

Deliver as needed:

- richer creator tools;
- in-game alignment editor;
- preview browser;
- tag override files;
- plain OSF adapter tooling;
- native addon reversible mechanism API;
- optional faction-state broadcast if Starfield consumer conditions prove useful;
- optional dynamic overlays/morphs/facial expression systems if spikes succeed.

## 16. Risk Spikes

These features require proof before public roadmap promises:

### 16.1 Facial Morphs

Goal: set and restore one reliable facial morph/expression layer on a live Starfield actor.

Must prove:

- apply;
- restore;
- survive cell change;
- survive save/load;
- no permanent actor damage.

If this fails, facial expression compositing is cut from the roadmap until the engine path is proven.

### 16.2 Dynamic Overlays

Goal: apply and remove one dynamic overlay/material effect.

Must prove:

- stable application;
- stable removal;
- no permanent visual corruption;
- acceptable performance;
- save/load cleanup.

### 16.3 Positioned Audio

Goal: play intimacy sound from actor/location with distance attenuation.

Must prove:

- actor-positioned playback;
- listener distance attenuation;
- no full-volume background audio;
- works for autonomy scenes.

Autonomy should not ship without a positioned or distance-aware audio solution.

### 16.4 Actor Scale And Alignment

Goal: verify OSF placement composes correctly with actor scale/body differences.

Must prove:

- actor scale affects or does not affect final placement in a known way;
- offsets can be authored predictably;
- alignment tools can correct common mismatch.

### 16.5 Faction-State Broadcast

Goal: verify Starfield systems can use faction rank or similar state as a practical condition reader.

Must prove:

- vanilla conditions can read the value;
- write frequency is safe;
- values do not become save-bloat or compatibility hazards.

If no real consumer exists, do not ship this.

## 17. Testing Strategy

### 17.1 Unit Tests

OSF-side:

- unified schema parsing;
- in-memory scene registration;
- reload replacement;
- generated scene id collision behavior;
- ABI version negotiation;
- scene start/navigate/stop through ABI.

OSF Intimacy:

- `*.intimacy.json` parsing;
- schema validation;
- tag validation;
- defaults/inheritance;
- compiler output;
- candidate scoring;
- actor capability resolution;
- anchor scoring;
- arousal math;
- stop request timeout;
- diagnostic code stability;
- co-save serialization;
- ledger idempotence.

### 17.2 Integration Tests

- Load multiple packs with one bad file and verify healthy files still load.
- Compile an intimacy scene and start it through OSF.
- Start through chooser.
- Start through Papyrus intent.
- Start through native intent.
- Observe OSF events and OSF Intimacy events.
- Save/load mid-scene and verify cleanup.
- Reload packs during author iteration.
- Exercise keyboard and controller controls.
- Verify candidate reasons in Doctor panel.

### 17.3 Manual In-Game Test Scenarios

- Player and one NPC in ship cabin.
- Player and one NPC near bed/furniture.
- Player and one NPC in public space.
- Player and one NPC in hostile/hazardous space.
- Solo scene.
- Pair scene.
- Missing asset pack.
- Unknown tag pack.
- Plain OSF scene inferred as low confidence.
- Stop request during normal stage.
- Stop request during transition.
- Save/load during scene.

## 18. Documentation Plan

Create or update:

- `docs/OSF_INTIMACY_PLAN.md` - this plan.
- `docs/OSF_INTIMACY_API.md` - public Papyrus/native API once signatures settle.
- `docs/OSF_INTIMACY_AUTHORING.md` - `*.intimacy.json` author guide.
- `docs/OSF_INTIMACY_TAGS.md` - stable tag vocabulary.
- `docs/OSF_INTIMACY_DIAGNOSTICS.md` - diagnostic codes and remediation.
- Existing ASF docs should be treated as historical drafts and conceptually renamed in future work.

Authoring docs should include:

- minimal scene example;
- paired scene example;
- stage arc example;
- equipment policy example;
- location preference example;
- common diagnostics and fixes;
- porting guidance for authors coming from SexLab/OStim/AAF without promising automatic import.

## 19. Open Implementation Questions

These do not block the plan, but should be resolved before coding each subsystem:

- Exact OSF C ABI function table shape.
- Exact `OSFIntimacy.psc` signatures that work around Papyrus struct/array limitations.
- Whether diagnostic code prefix is `OSI_` or `OSFINT_`.
- Exact path convention for user settings JSON.
- Exact location scanner data sources available in Starfield/commonlibsf.
- Exact UI technology path for compact chooser and Doctor panel.
- Exact maximum delay for fast clean exit.
- Exact first set of known tags and content flags.
- Exact co-save format and migration story.

## 20. Success Criteria

OSF Intimacy Phase 1 is successful when:

- content authors can ship data-only scenes;
- other mods can request scenes by intent;
- the chooser recommends scenes with understandable reasons;
- scenes feel good in Watch mode;
- arousal and pacing are visible but not intrusive;
- failures are actionable;
- bad content does not break the ecosystem;
- OSF stays content-neutral;
- Papyrus users have a small useful API;
- native addon authors have a stable ABI;
- save/load does not leave stuck actors, controls, equipment, or Intimacy state.

The framework should feel like a clean Starfield-native foundation, not a clone of any one predecessor. The key is to combine OSF's native graph/playback strength with a calm, transparent, context-aware gameplay layer that other mods can trust.
