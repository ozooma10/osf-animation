ScriptName OSF Native Hidden

; OSF Animation - native animation and scene framework for Starfield.

; Scene function return an int handle (0 = failed to start, e.g. invalid id or actor binding; >0 = a live scene instance). 
; The handle is used for scene-specific operations (navigation, callbacks, stop) and queries (id/node/participants). 
; A scene ends when its graph(s) finish or are stopped; the handle becomes invalid and queries return sentinels (e.g. "").

; Matchmake by tags + role/gender fit across scene defs and packs in registry
; chosen by priority tier + weighted-random, and start it. 
; Returns the scene handle (0 = no match); 
; recover the chosen scene id with GetSceneId(handle)
int Function StartSceneByTags(Actor[] akActors, string[] asTags) Global Native

; Start a specific scene by id. aiStage = start stage
int Function StartScene(Actor[] akActors, string asSceneId, int aiStage = 0) Global Native





; --- Primitives ---------------------------------------------------------------

; True while akActor is in an animation or scene
bool Function IsPlaying(Actor akActor) Global Native

; Plays a GLTF/GLB animation on akActor. asFile is path to animation, relative to Data folder (e.g. "OSF\MyAnim.glb");
bool Function Play(Actor akActor, string asFile, string asAnim = "") Global Native

; Stops playback on the given actor and returns control to the game.
bool Function Stop(Actor akActor) Global Native




; ---- Advanced API ------

; Per-graph playback speed: 1.0 = authored, 0 = freeze (clamped 0..100).
; For a participant, sets the shared scene clock. False if no live graph.
bool Function SetSpeed(Actor akActor, float afSpeed) Global Native

; Current playback speed for akActor (scene clock for a participant), or 0.0 if no live graph.
float Function GetSpeed(Actor akActor) Global Native

; Pins akActor's solo graph to a WORLD point + heading (DEGREES) and moves the capsule there.
; aiRootMode: 0 = pin (lock rendered root at the point), 1 = additive (EXPERIMENTAL — currently behaves like pin), 2 = follow (ride the actor).
; Refused for scene participants (placement is scene-driven); false if no live graph.
bool Function SetAnchor(Actor akActor, float afX, float afY, float afZ, float afHeadingDeg, int aiRootMode = 0) Global Native

; Releases akActor's anchor, the graph returns to "follow". No-op if unanchored.
bool Function ClearAnchor(Actor akActor) Global Native

; Bring N already-playing graphs together: Play each actor first, then Sync.
; abAnchor=true (default): anchor the graphs into one shared scene at actor[0]'s spot — same-spot
; overlap, so paired clips arrange themselves about a shared origin + heading (the fix for actors
; standing apart). abAnchor=false: clock-merge only — frame-lock the graphs on one shared clock while
; each actor stays at its own world position (no teleport/anchor), for clips that already carry the
; intended world separation. Scene participants skipped; needs >= 2 playable graphs. Each clip keeps
; its length (mismatched ones loop independently, shared phase origin).
bool Function Sync(Actor[] akActors, bool abAnchor = true) Global Native

; Solo multi-phase sequence (primitive, no anchor/policy; follows the actor).
; Parallel arrays of equal non-zero length:
; asFiles[i] = phase clip, aiLoops[i] = loops before advancing (<=0 = hold), afBlends[i] = blend-in secs.
; abLoopWhole restarts at phase 0 after the last instead of ending.
bool Function PlaySequence(Actor akActor, string[] asFiles, int[] aiLoops, float[] afBlends, bool abLoopWhole = false) Global Native

; The source path playing on akActor (as passed to Play), or "" if no live graph.
string Function GetCurrentAnimation(Actor akActor) Global Native



; Start a scene world-ANCHORED at akAnchor (furniture / bed / marker) instead of co-locating the
; actors at akActors[0] — for furniture/sleep encounters that belong to a thing, not an actor.
; afHeadingDeg < 0 uses akAnchor's own heading; otherwise it is a heading in DEGREES. Id resolution
; (scene-then-pack, scene:/anim: prefixes) mirrors StartScene. Returns the handle (0 = failed).
int Function StartSceneAt(Actor[] akActors, string asSceneId, ObjectReference akAnchor, float afHeadingDeg = -1.0) Global Native

; Start a def-backed scene binding actors to NAMED roles: asRoles[i] is the role for akActors[i]
; (equal lengths; every declared role must be filled exactly once). 
; Returns the handle (0 = no such scene / validation failure: unknown or duplicate role, null/duplicate actor, role count).
int Function StartSceneRoles(Actor[] akActors, string asSceneId, string[] asRoles) Global Native

; Boolean-query form of StartSceneByTags: asAllOf (every tag must match) + asAnyOf (at least one;
; empty = ignored) + asNoneOf (none may match). Same filter-aware matchmaking + weighted pick.
int Function StartSceneByTagsQuery(Actor[] akActors, string[] asAllOf, string[] asAnyOf, string[] asNoneOf) Global Native

; Like StartSceneByTags, but world-ANCHORS the matchmade scene at akAnchor (furniture / bed / marker) instead of co-locating the actors at akActors[0]
; for furniture/sleep encounters that belong to a thing, not an actor. 
; afHeadingDeg < 0 uses akAnchor's own heading; otherwise it is a heading in DEGREES. 
; Returns the scene handle (0 = no match / no anchor / start failed).
int Function StartSceneByTagsAt(Actor[] akActors, string[] asTags, ObjectReference akAnchor, float afHeadingDeg = -1.0) Global Native

; Boolean-query form of StartSceneByTagsAt (asAllOf / asAnyOf / asNoneOf), world-anchored at akAnchor.
int Function StartSceneByTagsQueryAt(Actor[] akActors, string[] asAllOf, string[] asAnyOf, string[] asNoneOf, ObjectReference akAnchor, float afHeadingDeg = -1.0) Global Native

; Ad-hoc scene from raw files: co-locates akActors at akActors[0], plays asFiles[i] on akActors[i], syncs the clock. 
; Equal-length arrays. Returns the scene handle.
int Function StartSceneFiles(Actor[] akActors, string[] asFiles, float afSpeed = 1.0, float afBlendIn = 0.4) Global Native

; Jump a LINEAR scene (by handle) to stage aiStage (0-based): a pack/files scene, or a graph that declares linearStages.
; False on a non-linear graph, out-of-range stage, or invalid handle.
bool Function SetSceneStage(int aiScene, int aiStage) Global Native

; Current stage of a LINEAR scene (by handle), or -1 (non-linear graph / invalid handle).
int Function GetSceneStage(int aiScene) Global Native

; Actor conveniences: read/jump the live scene akActor is in by ACTOR 
; (reaches any scene driving the actor, including a PlaySequence solo sequence that has no handle). -1 / false if in none.
int Function GetSceneStageForActor(Actor akActor) Global Native
bool Function SetSceneStageForActor(Actor akActor, int aiStage) Global Native

; Stops a live scene by its handle (from a Start* call). False if the handle is invalid/ended.
bool Function StopScene(int aiScene) Global Native

; Actor convenience: stops the live scene akActor participates in. False if it is in none.
bool Function StopSceneForActor(Actor akActor) Global Native

; Discovery: ids of scenes (composed defs + packs) with aiActorCount actors whose tags contain ALL
; asTags (case-insensitive; empty = any). Deterministic order (priority desc, then id asc). Count +
; tags only — FILTER-UNAWARE (no actors), so a returned id may not bind; for a filter-correct result
; use FindScenesForActorsQuery or StartSceneByTags*. (See OSF.GetSceneRoles etc. to inspect a result.)
string[] Function FindScenes(int aiActorCount, string[] asTags) Global Native

; Filter-aware discovery: like FindScenes but takes the actors, so keyword/race/gender role filters +
; a complete binding are required for a scene def to appear. Boolean tag sets (allOf/anyOf/noneOf).
string[] Function FindScenesForActorsQuery(Actor[] akActors, string[] asAllOf, string[] asAnyOf, string[] asNoneOf) Global Native

; Rescans Data/OSF/**/*.json and replaces the registry. Returns the count now registered.
int Function ReloadPacks() Global Native

; --- Readiness handshake ------------------------------------------------------

; True once OSF is loaded + initialized (hooks installed).
bool Function IsReady() Global Native

; Framework version (semver). Pre-1.0 (0.x) the surface is still settling and may change between
; releases; from 1.0 on, natives are never removed or re-signatured within a major version (minors only add).
string Function GetVersion() Global Native

; --- Scene-event callbacks (OSFEvent:SceneEvent payload) ----------------------
; Register akReceiver.asFn(OSFEvent:SceneEvent) for events whose bit is set in aiEventMask
; (when aiScene != 0) whose scene handle == aiScene. Returns a generational token (0 = failed). 
; Dispatch is asynchronous: the receiver runs later on the VM, so the payload arrives as a snapshot struct (there are no dispatch-time getters).
; Function OnSceneEvent(OSFEvent:SceneEvent akEvent)   ; on akReceiver's script
int Function RegisterSceneCallback(ScriptObject akReceiver, string asFn, int aiScene = 0, int aiEventMask = 65535) Global Native
bool Function UnregisterSceneCallback(int aiToken) Global Native

; --- Scene state getters (handle-based) ---------------------------------------
; Take an opaque scene handle (from a Start* call).
; Sentinels: id/node "" when the handle is invalid/ended; actor->handle 0.
string Function GetSceneId(int aiScene) Global Native
string Function GetSceneNode(int aiScene) Global Native
int Function GetSceneForActor(Actor akActor) Global Native

; --- Scene-metadata introspection (read-only; by scene id, not handle) ---------
; Inspect a *.scene.json scene's role/gender/tag conventions before binding actors. All take a
; scene id; an unknown id (or a pack id, which is not a scene def) yields the empty/sentinel result.
; Returned arrays are real (possibly empty) — safe to receive (the None-array rule is about passing
; None IN, not getting an array back).
string[] Function GetSceneRoles(string asSceneId) Global Native            ; declared role names, in order ([] if unknown)
string Function GetSceneRoleGender(string asSceneId, string asRole) Global Native  ; "male"/"female"/"any"; "" if unknown
int Function GetSceneActorCount(string asSceneId) Global Native            ; declared role/actor count (0 if unknown / no roles)
string[] Function GetSceneTags(string asSceneId) Global Native             ; scene tags ([] if unknown)

; Problems (errors + warnings, each prefixed [error]/[warn]) from the last scene load / ReloadPacks
; Empty = all scene files loaded cleanly.
string[] Function GetSceneLoadErrors() Global Native

; True iff asSceneId names a scene that LOADED — a scene in the registry passed all validation
; (invalid scenes are skipped at load), so loaded == valid. False for an unknown/failed id or a
; pack id. Use GetSceneValidationErrors(asSceneId) to see why an id is invalid.
bool Function ValidateScene(string asSceneId) Global Native

; The load problems referring to asSceneId (the subset of GetSceneLoadErrors mentioning the id).
; Empty = no recorded problems for that id. For a file that failed before its id could be read,
; use GetSceneLoadErrors (the full list).
string[] Function GetSceneValidationErrors(string asSceneId) Global Native

; --- Scene navigation (handle-based; def-backed scenes) -----------------------
; Take the current node's DEFAULT advance edge (or end the scene if it targets "$end").
; False if the handle is invalid or the node has no default advance edge (never inferred).
bool Function AdvanceScene(int aiScene) Global Native
; Take the current node's branchable advance edge with id == asEdgeId. False if no such edge.
bool Function NavigateScene(int aiScene, string asEdgeId) Global Native
; Branchable (advance) edges of the current node — for building menus.
int Function GetSceneEdgeCount(int aiScene) Global Native
string Function GetSceneEdgeId(int aiScene, int aiIndex) Global Native
string Function GetSceneEdgeLabel(int aiScene, int aiIndex) Global Native

; Event-type bits (compose into aiEventMask; EVENT_ALL = every type). Exposed as global
; getter functions, not properties: a `Native` script's properties cannot be read on the
; type (OSF.X), but global functions can (OSF.EVENT_NODE_ENTER()). (A non-Native companion
; with AutoReadOnly properties doesn't help — Papyrus rejects reading a property on a type
; name, so it would force every consumer to hold an instance. The () is the right tradeoff.)
int Function EVENT_NODE_ENTER() Global
    return 1
EndFunction
int Function EVENT_NODE_EXIT() Global
    return 2
EndFunction
int Function EVENT_CUE() Global
    return 4
EndFunction
int Function EVENT_ACTION() Global
    return 8
EndFunction
int Function EVENT_SCENE_END() Global
    return 16
EndFunction
int Function EVENT_ALL() Global
    return 65535
EndFunction

; Result codes (OSFEvent.Result). Disabled-by-settings is a silent skip, NOT a failure.
int Function RESULT_OK() Global
    return 0
EndFunction
int Function RESULT_BAD_ROLE() Global
    return 1
EndFunction
int Function RESULT_RUNTIME_FAILURE() Global
    return 2
EndFunction
int Function RESULT_NO_HANDLER() Global
    return 3
EndFunction
