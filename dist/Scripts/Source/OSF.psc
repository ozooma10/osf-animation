ScriptName OSF Native Hidden

; OSF Animation - native animation and scene framework for Starfield.

; Scene functions return an int handle (0 = failed to start, e.g. invalid id or actor binding; >0 = a live scene instance). 
; The handle is used for scene-specific operations (navigation, callbacks, stop) and queries (id/node/participants). 
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
