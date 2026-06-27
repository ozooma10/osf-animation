ScriptName OSFAdvanced Native Hidden

; Advanced OSF Animation API. Common consumers should prefer OSF.psc.

; Start a one-stage synced scene from actor-aligned clip files. asFiles[i] plays on akActors[i].
; Returns a normal scene handle (0 = failed). Supports "naf:Path.glb" and "File.glb:AnimName" specs.
int Function StartSceneFiles(Actor[] akActors, string[] asFiles, OSFTypes:SceneOptions akOpts = None) Global Native

; Start a def-backed scene binding actors to named roles, with SceneOptions support.
int Function StartSceneRolesEx(Actor[] akActors, string asSceneId, string[] asRoles, OSFTypes:SceneOptions akOpts = None) Global Native

; Start a dynamic multi-stage synced scene without JSON.
; asFiles is stage-major: for N actors and S stages, length must be N*S. Stage 0 actor files first, then stage 1, etc.
; afTimers/aiLoops/afBlends may be empty or length S. Empty timers+loops means each stage plays once.
int Function StartSceneStages(Actor[] akActors, string[] asFiles, float[] afTimers, int[] aiLoops, float[] afBlends, OSFTypes:SceneOptions akOpts = None) Global Native

; Solo multi-stage sequence on one actor. Arrays must be equal length.
bool Function PlaySequence(Actor akActor, string[] asFiles, int[] aiLoops, float[] afBlends, bool abLoopWhole = false) Global Native

; Stops every unique scene represented by akActors, then any remaining solo playback. Returns stop operation count.
int Function StopAllForActors(Actor[] akActors) Global Native

; Registry diagnostics from the last load/reload.
string[] Function GetSceneLoadErrors() Global Native
string[] Function GetMissingClipRefs() Global Native