ScriptName OSF Native Hidden

; OSF Animation - native animation and scene framework for Starfield.

; Scene functions return an int handle (0 = failed to start, e.g. invalid id or actor binding; >0 = a live scene instance). 
; The handle is used for scene-specific operations (navigation, callbacks, stop) and queries (stage/edges/participants).
; A scene ends when its graph(s) finish or are stopped; the handle becomes invalid and queries return sentinels (e.g. "").


; Matchmake by tags + role/gender fit across scene defs and packs in registry, chosen by priority tier + weighted-random, and start it.
; Returns the scene handle (0 = no match);
; recover the chosen scene id with GetSceneId(handle).
int Function StartSceneByTags(Actor[] akActors, string[] asTags, OSFTypes:SceneOptions akOpts = None) Global Native

; Start a specific scene by id. akOpts.Stage = start stage (packs; ignored by def graphs);
int Function StartScene(Actor[] akActors, string asSceneId, OSFTypes:SceneOptions akOpts = None) Global Native

; Boolean-query form of StartSceneByTags: asAllOf (every tag must match) + asAnyOf (at least one; empty = ignored) + asNoneOf (none may match).
int Function StartSceneByTagsQuery(Actor[] akActors, string[] asAllOf, string[] asAnyOf, string[] asNoneOf, OSFTypes:SceneOptions akOpts = None) Global Native

; Start a def-backed scene binding actors to NAMED roles: asRoles[i] is the role for akActors[i] (equal lengths; every declared role must be filled exactly once).
; Returns the handle (0 = no such scene / validation failure: unknown or duplicate role, null/duplicate actor, role count).
int Function StartSceneRoles(Actor[] akActors, string asSceneId, string[] asRoles) Global Native


; --- Scene-event callbacks (OSFTypes:SceneEvent payload) ----------------------
; Register akReceiver.asFn(OSFTypes:SceneEvent) for events whose bit is set in aiEventMask (when aiScene != 0) whose scene handle == aiScene.
; Returns a generational token (0 = failed).
; Function OnSceneEvent(OSFTypes:SceneEvent akEvent)   ; on akReceiver's script
int Function RegisterSceneCallback(ScriptObject akReceiver, string asFn, int aiScene = 0, int aiEventMask = 65535) Global Native
; Instance-free variant for Papyrus script LIBRARIES (global functions, with no `self` to pass as a receiver):
; dispatches to the GLOBAL function asScript.asFn(OSFTypes:SceneEvent). Same scene-filter / event-mask /
; token semantics as RegisterSceneCallback; release with UnregisterSceneCallback.
; Function OnSceneEvent(OSFTypes:SceneEvent akEvent) global   ; on script asScript
int Function RegisterSceneCallbackStatic(string asScript, string asFn, int aiScene = 0, int aiEventMask = 65535) Global Native
bool Function UnregisterSceneCallback(int aiToken) Global Native


; --- Navigation  -------------------------------------------
; Operate on the int handle a Start* call returned.

; Take the current node's DEFAULT advance edge (or end the scene if it targets "$end").
; False if aiScene is invalid or the current node has no default advance edge.
bool Function AdvanceScene(int aiScene) Global Native

; Take the current node's branchable advance edge whose id == asEdgeId.
; False if aiScene is invalid or the current node has no such edge.
bool Function NavigateScene(int aiScene, string asEdgeId) Global Native

; Number of branchable (advance) edges on the current node, for building a choice menu. 0 if invalid.
int Function GetSceneEdgeCount(int aiScene) Global Native

; Id of the aiIndex-th branchable edge (0 .. GetSceneEdgeCount-1) of the current node. "" if out of range/invalid.
string Function GetSceneEdgeId(int aiScene, int aiIndex) Global Native

; Resolved label (labelKey or literal) of the aiIndex-th branchable edge of the current node. "" if out of range/invalid.
string Function GetSceneEdgeLabel(int aiScene, int aiIndex) Global Native


; --- Participants -------------------------------------------------------------

; The scene's participants (by handle), in scene-internal (role-declaration) order.
; Valid for a live scene AND for a just-ended one: the roster survives into the (async)
; SCENE_END callback, so an end handler can call GetSceneParticipants(akEvent.sceneHandle)
; to learn who took part. Empty array only if the handle is invalid or already reclaimed.
Actor[] Function GetSceneParticipants(int aiScene) Global Native


; --- Primitives ---------------------------------------------------------------

; True while akActor is in an animation or scene
bool Function IsPlaying(Actor akActor) Global Native

; Plays an animation on akActor. asFile is path to animation, relative to Data folder (e.g. "OSF\MyAnim.glb");
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
; aiRootMode: 0 = pin (lock rendered root at the point), 1 = follow (ride the actor).
; Refused for scene participants (placement is scene-driven); false if no live graph.
bool Function SetAnchor(Actor akActor, float afX, float afY, float afZ, float afHeadingDeg, int aiRootMode = 0) Global Native

; Releases akActor's anchor, the graph returns to "follow". No-op if unanchored.
bool Function ClearAnchor(Actor akActor) Global Native

; The source path playing on akActor (as passed to Play), or "" if no live graph.
string Function GetCurrentAnimation(Actor akActor) Global Native

; Jump a LINEAR scene (by handle) to stage aiStage (0-based): a files scene, or a scene that declares linearStages.
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

; Rescans Data/OSF/**/*.osf.json and replaces the scene registry. Returns the count now registered.
int Function ReloadPacks() Global Native

; --- Readiness handshake ------------------------------------------------------

; True once OSF is loaded + initialized (hooks installed).
bool Function IsReady() Global Native

; Framework version 
string Function GetVersion() Global Native

; Event-type bits (compose into aiEventMask; EVENT_ALL = every type). 
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
int Function EVENT_SCENE_BEGIN() Global
    return 32
EndFunction
int Function EVENT_ALL() Global
    return 65535
EndFunction

; SceneOptions tri-state helpers (StripMode / LockPlayerMode / FadeMode). Pass these instead of
; bare 0/1 so call sites read clearly and nobody mistakes 0 ("force off") for "leave default".
int Function INHERIT() Global
    return -1
EndFunction
int Function OFF() Global
    return 0
EndFunction
int Function ON() Global
    return 1
EndFunction

; Result codes (decode OSFTypes:SceneEvent.result).
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