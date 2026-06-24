ScriptName OSF Native Hidden

; OSF Animation - native animation and scene framework for Starfield.

; Scene functions return an int handle (0 = failed to start, e.g. invalid id or actor binding; >0 = a live scene instance). 
; The handle is used for scene-specific operations (navigation, callbacks, stop) and queries (id/node/participants). 
; A scene ends when its graph(s) finish or are stopped; the handle becomes invalid and queries return sentinels (e.g. "").

; --- Scene options ------------------------------------------------------------
; Optional modifiers shared by the Start* functions. Pass None (the default) for the common case; 
Struct SceneOptions
    ObjectReference Anchor    ; world-anchor the scene at this ref (furniture/bed/marker) instead of co-locating at akActors[0]. Ignored by StartSceneFiles.
    float HeadingDeg = -1.0   ; anchor heading in DEGREES; < 0 = use Anchor's own heading
    int Stage = 0             ; StartScene by-id / pack: start stage (ignored by def-backed graphs)
    float Speed = 1.0         ; StartSceneFiles: playback speed
    float BlendIn = 0.4       ; StartSceneFiles: blend-in seconds
EndStruct

; Matchmake by tags + role/gender fit across scene defs and packs in registry, chosen by priority tier + weighted-random, and start it.
; Returns the scene handle (0 = no match);
; recover the chosen scene id with GetSceneId(handle).
int Function StartSceneByTags(Actor[] akActors, string[] asTags, SceneOptions akOpts = None) Global Native

; Start a specific scene by id. akOpts.Stage = start stage (packs; ignored by def graphs);
int Function StartScene(Actor[] akActors, string asSceneId, SceneOptions akOpts = None) Global Native

; Boolean-query form of StartSceneByTags: asAllOf (every tag must match) + asAnyOf (at least one; empty = ignored) + asNoneOf (none may match).
int Function StartSceneByTagsQuery(Actor[] akActors, string[] asAllOf, string[] asAnyOf, string[] asNoneOf, SceneOptions akOpts = None) Global Native

; Start a def-backed scene binding actors to NAMED roles: asRoles[i] is the role for akActors[i] (equal lengths; every declared role must be filled exactly once).
; Returns the handle (0 = no such scene / validation failure: unknown or duplicate role, null/duplicate actor, role count).
int Function StartSceneRoles(Actor[] akActors, string asSceneId, string[] asRoles) Global Native

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

; Rescans Data/OSF/**/*.json and replaces the registry. Returns the count now registered.
int Function ReloadPacks() Global Native

; --- Readiness handshake ------------------------------------------------------

; True once OSF is loaded + initialized (hooks installed).
bool Function IsReady() Global Native

; Framework version 
string Function GetVersion() Global Native

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
