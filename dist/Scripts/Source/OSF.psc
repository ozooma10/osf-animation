ScriptName OSF Native Hidden

; OSF Animation - native animation and scene framework for Starfield.

; Start* functions return a scene handle (0 = failed) used by navigation, callbacks, stop and queries.
; Once the scene ends the handle is invalid and queries return sentinels.


; Matchmakes by tags + role/gender fit (priority tier, then weighted random) and starts it. Returns the handle (0 = no match).
int Function StartSceneByTags(Actor[] akActors, string[] asTags, OSFTypes:SceneOptions akOpts = None) Global Native

; Starts scene asSceneId. akOpts.Stage enters on that linear stage (out of range fails; non-linear scenes ignore it).
int Function StartScene(Actor[] akActors, string asSceneId, OSFTypes:SceneOptions akOpts = None) Global Native

; Boolean-query StartSceneByTags: asAllOf (all must match), asAnyOf (at least one; empty = ignored), asNoneOf (none may match).
int Function StartSceneByTagsQuery(Actor[] akActors, string[] asAllOf, string[] asAnyOf, string[] asNoneOf, OSFTypes:SceneOptions akOpts = None) Global Native

; Starts an anchor-bound scene built for akFurniture (matched by anchor keyword/base); free scenes are never picked.
; asTags narrows the pool (empty = any). akOpts.Anchor is ignored. Returns the handle (0 = nothing fits / start failed).
int Function StartSceneAtAnchor(Actor[] akActors, ObjectReference akFurniture, string[] asTags, OSFTypes:SceneOptions akOpts = None) Global Native

; asRoles[i] is akActors[i]'s role; every declared role must be filled exactly once. Anchor-bound scenes need StartScene.
; Returns the handle (0 = unknown scene or invalid binding: unknown/duplicate role, null/duplicate actor, count mismatch).
int Function StartSceneRoles(Actor[] akActors, string asSceneId, string[] asRoles) Global Native


; Calls akReceiver.asFn(OSFTypes:SceneEvent) for events in aiEventMask, filtered to scene aiScene when != 0.
; Returns a token (0 = failed).
; Function OnSceneEvent(OSFTypes:SceneEvent akEvent)   ; on akReceiver's script
int Function RegisterSceneCallback(ScriptObject akReceiver, string asFn, int aiScene = 0, int aiEventMask = 65535) Global Native
; Global-function variant for script libraries: calls asScript.asFn(OSFTypes:SceneEvent). Same semantics otherwise.
; Function OnSceneEvent(OSFTypes:SceneEvent akEvent) global   ; on script asScript
int Function RegisterSceneCallbackStatic(string asScript, string asFn, int aiScene = 0, int aiEventMask = 65535) Global Native
bool Function UnregisterSceneCallback(int aiToken) Global Native


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


; Participants in role-declaration order. The roster survives into the async SCENE_END callback so end handlers
; can read it; empty only once the handle is invalid or reclaimed.
Actor[] Function GetSceneParticipants(int aiScene) Global Native


; True while akActor is in an animation or scene
bool Function IsPlaying(Actor akActor) Global Native

; Plays asFile (path relative to Data, e.g. "OSF\MyAnim.glb") on akActor; asAnim optionally selects an animation in the file.
bool Function Play(Actor akActor, string asFile, string asAnim = "") Global Native

; Stops playback on the given actor and returns control to the game.
bool Function Stop(Actor akActor) Global Native

; Puts actors each started with Play onto one shared clock. abAnchor=True also co-locates them as one anchored scene
; at akActors[0]'s transform; False only shares the clock. Needs >= 2 actors.
bool Function Sync(Actor[] akActors, bool abAnchor = true) Global Native


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

; By actor rather than handle, so these also reach a PlaySequence solo sequence. -1 / False if in none.
int Function GetSceneStageForActor(Actor akActor) Global Native
bool Function SetSceneStageForActor(Actor akActor, int aiStage) Global Native

; Stops a live scene by its handle (from a Start* call). False if the handle is invalid/ended.
bool Function StopScene(int aiScene) Global Native

; Actor convenience: stops the live scene akActor participates in. False if it is in none.
bool Function StopSceneForActor(Actor akActor) Global Native

; Rescans Data/OSF/**/*.osf.json and replaces the scene registry. Returns the count now registered.
int Function ReloadPacks() Global Native

; True once OSF is loaded + initialized (hooks installed).
bool Function IsReady() Global Native

; "major.minor.patch"
string Function GetVersion() Global Native

; Reports the oldest OSF Animation this consumer supports; call once at startup. False if OSF is older
; (shows one deduplicated upgrade warning and a System Health entry) or the request is invalid.
bool Function ReportMinimumVersion(string asConsumer, int aiMajor, int aiMinor = 0, int aiPatch = 0) Global Native

; Open the in-game scene browser (the OSF UI overlay)
bool Function OpenBrowser() Global Native

; Self-test: checks IsReady, then plays the built-in "solo" scene on the player for 5s.
; Console: cgf "OSF.Health"
bool Function Health() Global
    if !IsReady()
        Debug.Notification("OSF Health: FAIL - framework not ready (hooks not installed)")
        Debug.Trace("[OSF] Health FAIL: IsReady() == False")
        return False
    endif

    Actor player = Game.GetPlayer()
    Actor[] actors = new Actor[1]
    actors[0] = player

    int handle = StartScene(actors, "solo")
    if handle == 0
        Debug.Notification("OSF Health: FAIL - could not start 'solo' scene (clip load or registry error)")
        Debug.Trace("[OSF] Health FAIL: StartScene('solo') returned handle 0")
        return False
    endif

    Utility.Wait(5.0)

    bool animating = IsPlaying(player)
    StopSceneForActor(player)

    if animating
        Debug.Notification("OSF Health: PASS - all systems nominal (v" + GetVersion() + ")")
        Debug.Trace("[OSF] Health PASS: 'solo' played 5s, player animating (handle " + handle + ")")
        return True
    endif

    Debug.Notification("OSF Health: FAIL - scene started but player never animated")
    Debug.Trace("[OSF] Health FAIL: IsPlaying() == False after StartScene (handle " + handle + ")")
    return False
EndFunction

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

; SceneOptions tri-state values (StripMode / LockPlayerMode / FadeMode); 0 forces off, it doesn't inherit.
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
