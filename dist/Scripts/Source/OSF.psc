ScriptName OSF Native Hidden

; OSF Animation — native animation playback core for Starfield.

; --- Primitives ---------------------------------------------------------------

; Plays a GLTF/GLB animation on akActor. asFile is Data-relative (e.g. "OSF\MyAnim.glb");
bool Function Play(Actor akActor, string asFile, string asAnim = "") Global Native

; Stops playback on the given actor and returns control to the game.
bool Function Stop(Actor akActor) Global Native

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

; Put N already-playing graphs on one shared clock (frame-lock): Play each actor first, then Sync.
; Scene participants skipped; needs >= 2 playable graphs.
; Each clip keeps its length (mismatched ones loop independently, shared phase origin).
bool Function Sync(Actor[] akActors) Global Native

; Solo multi-phase sequence (primitive, no anchor/policy; follows the actor).
; Parallel arrays of equal non-zero length:
; asFiles[i] = phase clip, aiLoops[i] = loops before advancing (<=0 = hold), afBlends[i] = blend-in secs.
; abLoopWhole restarts at phase 0 after the last instead of ending.
bool Function PlaySequence(Actor akActor, string[] asFiles, int[] aiLoops, float[] afBlends, bool abLoopWhole = false) Global Native

; The source path playing on akActor (as passed to Play), or "" if no live graph.
string Function GetCurrentAnimation(Actor akActor) Global Native

; True while akActor has a live animation graph (scene or solo).
bool Function IsPlaying(Actor akActor) Global Native

; --- Scenes (mechanical: anchored, staged, synced - no policy) -----------------
; Start* return an opaque scene HANDLE (int; 0 = failed) - pass it to StopScene / the handle-based getters / navigation below. 
; (A bare id resolves a *.scene.json graph first, then a registry pack auto-exposed as a single-path scene; prefix scene:/anim: to force.)

; Start a scene by id, returning its handle. aiStage = pack start stage (ignored for graphs).
int Function StartScene(Actor[] akActors, string asSceneId, int aiStage = 0) Global Native

; Start a scene world-ANCHORED at akAnchor (furniture / bed / marker) instead of co-locating the
; actors at akActors[0] — for furniture/sleep encounters that belong to a thing, not an actor.
; afHeadingDeg < 0 uses akAnchor's own heading; otherwise it is a heading in DEGREES. Id resolution
; (scene-then-pack, scene:/anim: prefixes) mirrors StartScene. Returns the handle (0 = failed).
int Function StartSceneAt(Actor[] akActors, string asSceneId, ObjectReference akAnchor, float afHeadingDeg = -1.0) Global Native

; Start a def-backed scene binding actors to NAMED roles: asRoles[i] is the role for akActors[i]
; (equal lengths; every declared role must be filled exactly once). 
; Returns the handle (0 = no such scene / validation failure: unknown or duplicate role, null/duplicate actor, role count).
int Function StartSceneRoles(Actor[] akActors, string asSceneId, string[] asRoles, int aiStage = 0) Global Native

; Matchmake a registry pack by tags + gender slots and start it. Returns the scene handle
; (0 = no match); recover the chosen id with GetSceneId(handle).
int Function StartSceneByTags(Actor[] akActors, string[] asTags) Global Native

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

; Registry scene ids with aiActorCount actors whose tags contain ALL asTags
; (case-insensitive; empty = any).
string[] Function FindScenes(int aiActorCount, string[] asTags) Global Native

; Rescans Data/OSF/**/*.json and replaces the registry. Returns the count now registered.
int Function ReloadPacks() Global Native

; --- Readiness handshake ------------------------------------------------------

; True once OSF is loaded + initialized (hooks installed).
bool Function IsReady() Global Native

; True when the named feature is effective in this build. One aggregate "is OSF's engine layer
; live?" gate — "scenes"/"playback"/"sync"/"anchor"/"cues"/"actions"/"sound"/"camera"/"callbacks"
; all report the same state (playback hooks installed + verified; they self-disable together on a
; version mismatch). Unknown name -> false.
bool Function HasFeature(string asFeature) Global Native

; Framework version (semver). Natives are never removed and signatures never change within a major; minors only add.
string Function GetVersion() Global Native

; --- Scene-event callbacks (Var[] payload - decode via OSFEvent) ---------------
; Register akReceiver.asFn(Var[]) for events whose bit is set in aiEventMask
; (when aiScene != 0) whose scene handle == aiScene. Returns a generational token (0 = failed). 
; Dispatch is asynchronous: the receiver runs later on the VM, so the payload arrives as the Var[] argument (there are no dispatch-time getters).
; Function OnSceneEvent(Var[] akEvent)   ; on akReceiver's script
int Function RegisterSceneCallback(ScriptObject akReceiver, string asFn, int aiScene = 0, int aiEventMask = 65535) Global Native
bool Function UnregisterSceneCallback(int aiToken) Global Native

; --- Scene state getters (handle-based) ---------------------------------------
; Take an opaque scene handle (from a Start* call).
; Sentinels: id/node "" when the handle is invalid/ended; actor->handle 0.
string Function GetSceneId(int aiScene) Global Native
string Function GetSceneNode(int aiScene) Global Native
int Function GetSceneForActor(Actor akActor) Global Native

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
int Function EVENT_ACTION_FAILED() Global
    return 16
EndFunction
int Function EVENT_SCENE_END() Global
    return 32
EndFunction
int Function EVENT_SCENE_ABORT() Global
    return 64
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