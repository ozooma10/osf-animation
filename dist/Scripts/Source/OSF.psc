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
; Refused for scene participants (placement is scene-driven); false if no live graph. See docs/ANCHORING.md.
bool Function SetAnchor(Actor akActor, float afX, float afY, float afZ, float afHeadingDeg, int aiRootMode = 0) Global Native

; Releases akActor's anchor — the graph returns to "follow". No-op if unanchored.
bool Function ClearAnchor(Actor akActor) Global Native

; Put N already-playing graphs on one shared clock (frame-lock): Play each actor first, then Sync.
; Scene participants skipped; needs >= 2 playable graphs.
; Each clip keeps its length (mismatched ones loop independently, shared phase origin).
bool Function Sync(Actor[] akActors) Global Native

; Solo multi-phase sequence (primitive — no anchor/policy; follows the actor).
; Parallel arrays of equal non-zero length:
; asFiles[i] = phase clip, aiLoops[i] = loops before advancing (<=0 = hold), afBlends[i] = blend-in secs.
; abLoopWhole restarts at phase 0 after the last instead of ending.
bool Function PlaySequence(Actor akActor, string[] asFiles, int[] aiLoops, float[] afBlends, bool abLoopWhole = false) Global Native

; The source path playing on akActor (as passed to Play), or "" if no live graph.
string Function GetCurrentAnimation(Actor akActor) Global Native

; True while akActor has a live animation graph (scene or solo).
bool Function IsPlaying(Actor akActor) Global Native

; --- Scenes (mechanical: anchored, staged, synced — no policy) -----------------

; Start a registry scene by id (1-actor defs run as full 1-participant scenes).
bool Function StartScene(Actor[] akActors, string asSceneId, int aiStage = 0) Global Native

; Matchmake a registry scene by tags + gender slots and start it. Returns the chosen id, or "".
string Function StartSceneByTags(Actor[] akActors, string[] asTags) Global Native

; Ad-hoc scene from raw files: co-locates akActors at akActors[0], plays asFiles[i] on akActors[i], syncs the clock. Equal-length arrays.
bool Function StartSceneFiles(Actor[] akActors, string[] asFiles, float afSpeed = 1.0, float afBlendIn = 0.4) Global Native

; Jumps the scene containing akActor to stage aiStage (0-based).
bool Function SetSceneStage(Actor akActor, int aiStage) Global Native

; Current stage of the scene akActor is in, or -1 if not in a scene.
int Function GetSceneStage(Actor akActor) Global Native

; Stops the scene that akActor participates in (all of its participants).
bool Function StopScene(Actor akActor) Global Native

; Registry scene ids with aiActorCount actors whose tags contain ALL asTags
; (case-insensitive; empty = any).
string[] Function FindScenes(int aiActorCount, string[] asTags) Global Native

; Rescans Data/OSF/**/*.json and replaces the registry. Returns the count now registered.
int Function ReloadPacks() Global Native

; --- Readiness handshake ------------------------------------------------------

; True once OSF is loaded + initialized (hooks installed).
bool Function IsReady() Global Native

; True when the named feature is effective in this build.
bool Function HasFeature(string asFeature) Global Native

; Framework version (semver). Natives are never removed and signatures never change within a major; minors only add.
string Function GetVersion() Global Native