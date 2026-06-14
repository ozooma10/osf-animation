Scriptname SAFScript extends ScriptObject

; SAFScript -> OSF compatibility shim. Functions tagged SHIM-GAP have no OSF
; equivalent yet. ResolveAnim/SAFLog kept local (no SAF.psc dependency) so a
; mixed setup with the real SAF native still resolves.

Bool Function DebugEnabled() Global
    return True
EndFunction

Function SAFLog(String asMsg) Global
    If DebugEnabled()
        Debug.Trace("[SAFScript->OSF] " + asMsg)
    EndIf
EndFunction

String Function ResolveAnim(String asAnimId) Global
    If asAnimId == ""
        return asAnimId
    EndIf
    return "SAF\\Animations\\" + asAnimId
EndFunction

Function ReleasePlayerLockIfPlayer(Actor akActor) Global
    If akActor != None && akActor == Game.GetPlayer()
        SAFLog("Releasing player lock -> ControlLock + CameraLock (off)")
        OSFCompat.SetPlayerControlLock(false)
        OSFCompat.SetPlayerCameraLock(false)
    EndIf
EndFunction

; --- Readiness ---

Bool Function Ping() Global
    Bool ready = OSF.IsReady()
    SAFLog("Ping -> " + ready)
    return ready
EndFunction

; --- Single-actor playback ---

Bool Function PlayOnActor(Actor akActor, String animId, Float fSpeed = 1.0, Int animIndex = 0) Global
    SAFLog("PlayOnActor actor=" + akActor + " anim=" + animId + " speed=" + fSpeed)
    If akActor == None
        return false
    EndIf
    Bool ok = OSF.Play(akActor, ResolveAnim(animId))
    If ok && fSpeed != 1.0
        OSF.SetSpeed(akActor, fSpeed)
    EndIf
    return ok
EndFunction

Bool Function PlayAnimationOnce(Actor akActor, String asAnimId, Float fTransitionTime = 1.0) Global
    SAFLog("PlayAnimationOnce actor=" + akActor + " anim=" + asAnimId)
    If akActor == None
        return false
    EndIf
    return OSF.Play(akActor, ResolveAnim(asAnimId))
EndFunction

Bool Function PlayOnPlayer(String animId, Float fSpeed = 1.0, Int animIndex = 0) Global
    SAFLog("PlayOnPlayer anim=" + animId)
    return PlayOnActor(Game.GetPlayer(), animId, fSpeed, animIndex)
EndFunction

Bool Function PlayOnActors(Actor[] akActors, String animId, Int animIndex = 0) Global
    Int n = 0
    If akActors != None
        n = akActors.Length
    EndIf
    SAFLog("PlayOnActors count=" + n + " anim=" + animId)
    Bool any = false
    Int i = 0
    While i < n
        If akActors[i] != None
            If OSF.Play(akActors[i], ResolveAnim(animId))
                any = true
            EndIf
        EndIf
        i += 1
    EndWhile
    return any
EndFunction

Bool Function StopAnimation(Actor akActor) Global
    If akActor == None
        return false
    EndIf
    ReleasePlayerLockIfPlayer(akActor)
    ; OSF.Stop refuses scene participants -- route them to StopScene.
    If OSF.GetSceneStage(akActor) >= 0
        SAFLog("StopAnimation actor=" + akActor + " (scene -> StopScene)")
        return OSF.StopScene(akActor)
    EndIf
    SAFLog("StopAnimation actor=" + akActor + " (solo -> Stop)")
    return OSF.Stop(akActor)
EndFunction

String Function GetCurrentAnimation(Actor akActor) Global
    String anim = OSF.GetCurrentAnimation(akActor)
    SAFLog("GetCurrentAnimation actor=" + akActor + " -> " + anim)
    return anim
EndFunction

; --- Two-actor scenes (actor1 is the anchor; OSF teleports actor2 to it) ---

Bool Function PlayScene(Actor akActor1, Actor akActor2, String asAnim1, String asAnim2, Float fSpeed = 1.0) Global
    SAFLog("PlayScene a1=" + akActor1 + " a2=" + akActor2 + " anim1=" + asAnim1 + " anim2=" + asAnim2)
    If akActor1 == None || akActor2 == None
        SAFLog("PlayScene aborted -- None actor")
        return false
    EndIf
    Actor[] actors = new Actor[2]
    actors[0] = akActor1
    actors[1] = akActor2
    String[] files = new String[2]
    files[0] = ResolveAnim(asAnim1)
    files[1] = ResolveAnim(asAnim2)
    return OSF.StartSceneFiles(actors, files, fSpeed)
EndFunction

Bool Function PlaySceneSeparate(Actor akActor1, Actor akActor2, String asAnim1, String asAnim2, Float fSpeed = 1.0) Global
    SAFLog("PlaySceneSeparate -> PlayScene")
    ; OSF.StartSceneFiles already refreshes capsules / anchors actor2; same path.
    return PlayScene(akActor1, akActor2, asAnim1, asAnim2, fSpeed)
EndFunction

; --- Speed ---

Function SetAnimationSpeed(Actor akActor, Float fSpeed) Global
    SAFLog("SetAnimationSpeed actor=" + akActor + " speed=" + fSpeed)
    OSF.SetSpeed(akActor, fSpeed)
EndFunction

Float Function GetAnimationSpeed(Actor akActor) Global
    Float s = OSF.GetSpeed(akActor)
    SAFLog("GetAnimationSpeed actor=" + akActor + " -> " + s)
    return s
EndFunction

; --- Position / anchoring -- no-ops: OSF anchors participants and pins the
; compose root automatically. Real anchoring happens in SyncGraphs / scene start.

Function SetGraphControlsPosition(Actor akActor, Bool bLocked) Global
    SAFLog("SetGraphControlsPosition actor=" + akActor + " locked=" + bLocked + " (no-op, OSF auto-pins)")
EndFunction

; SHIM-GAP: SAF set an ABSOLUTE world position; a raw SetPosition would fight the
; compose-root pin. Use LockActorForAnimation / scene start to anchor.
Function SetActorPosition(Actor akActor, Float fX, Float fY, Float fZ) Global
    SAFLog("SetActorPosition actor=" + akActor + " (" + fX + "," + fY + "," + fZ + ") (SHIM-GAP, no-op)")
EndFunction

Function MatchActorTransform(Actor akTarget, Actor akSource) Global
    SAFLog("MatchActorTransform target=" + akTarget + " source=" + akSource + " (SHIM-GAP, no-op)")
EndFunction

; No-op: SAF called this BEFORE play, but OSF.SetAnchor needs a LIVE graph (none
; exists yet). Anchoring is reconstructed in SyncGraphs / StartScene / PlayScene.
Function LockActorForAnimation(Actor akActor, Float fX, Float fY, Float fZ, Bool abIsPlayer = false) Global
    SAFLog("LockActorForAnimation actor=" + akActor + " (" + fX + "," + fY + "," + fZ + ") (no-op; OSF anchors at SyncGraphs/scene start)")
EndFunction

Function UnlockActorAfterAnimation(Actor akActor, Bool abIsPlayer = false) Global
    ; SAF may pass akActor=None with abIsPlayer=true to mean "the player".
    Actor target = akActor
    If target == None && abIsPlayer
        target = Game.GetPlayer()
    EndIf
    SAFLog("UnlockActorAfterAnimation actor=" + target + " -> OSF.ClearAnchor + release")
    If target != None
        OSF.ClearAnchor(target)
    EndIf
    If abIsPlayer
        SAFLog("Releasing player lock -> ControlLock + CameraLock (off)")
        OSFCompat.SetPlayerControlLock(false)
        OSFCompat.SetPlayerCameraLock(false)
    Else
        ReleasePlayerLockIfPlayer(target)
    EndIf
EndFunction

; SAF's SyncGraphs frame-locks a group on one shared clock. OSF.Sync does exactly
; that (the content-neutral core does not auto-manage scene policy).
Function SyncGraphs(Actor[] akTargets) Global
    Int n = 0
    If akTargets != None
        n = akTargets.Length
    EndIf
    SAFLog("SyncGraphs count=" + n + " -> OSF.Sync")
    If n <= 0
        return
    EndIf
    OSF.Sync(akTargets)
EndFunction

Function StopSyncing(Actor akTarget) Global
    SAFLog("StopSyncing target=" + akTarget + " (SHIM-GAP, no-op)")
    ; SHIM-GAP: OSF has no retro-unsync of a single graph from a shared clock.
EndFunction

; --- Sequences -- map onto OSF staged scenes where possible ---

; SHIM-GAP: no OSF native for an ad-hoc staged sequence from a path array; plays
; path 0 only (phases do NOT auto-advance).
Function StartSequence(Actor akActor, String[] asPaths, Bool bLoop) Global
    Int n = 0
    If asPaths != None
        n = asPaths.Length
    EndIf
    SAFLog("StartSequence actor=" + akActor + " paths=" + n + " loop=" + bLoop + " (SHIM-GAP, plays path 0 only)")
    If akActor != None && n > 0
        OSF.Play(akActor, ResolveAnim(asPaths[0]))
    EndIf
EndFunction

Bool Function AdvanceSequence(Actor akActor, Bool bSmooth) Global
    SAFLog("AdvanceSequence actor=" + akActor + " -> false (SHIM-GAP)")
    return false  ; SHIM-GAP: depends on StartSequence's missing sequence model
EndFunction

Bool Function SetSequencePhase(Actor akActor, Int iPhase) Global
    SAFLog("SetSequencePhase actor=" + akActor + " phase=" + iPhase)
    ; Only meaningful if akActor is in an OSF staged scene.
    return OSF.SetSceneStage(akActor, iPhase)
EndFunction

Int Function GetSequencePhase(Actor akActor) Global
    Int phase = OSF.GetSceneStage(akActor)
    SAFLog("GetSequencePhase actor=" + akActor + " -> " + phase)
    return phase
EndFunction

; --- Blend-graph variables -- no equivalent in OSF (SHIM-GAP) ---

Bool Function SetBlendGraphVariable(Actor akActor, String asName, Float fValue) Global
    SAFLog("SetBlendGraphVariable actor=" + akActor + " name=" + asName + " -> false (SHIM-GAP)")
    return false
EndFunction

Float Function GetBlendGraphVariable(Actor akActor, String asName) Global
    SAFLog("GetBlendGraphVariable actor=" + akActor + " name=" + asName + " -> 0.0 (SHIM-GAP)")
    return 0.0
EndFunction

; --- Crosshair / selection buffer -- all SHIM-GAP: SAF read these natively
; (engine crosshairRef, ProcessLists walk, persistent buffer); pure Papyrus has
; no equivalent ---

ObjectReference Function GetCrosshairRef() Global
    SAFLog("GetCrosshairRef -> None (SHIM-GAP)")
    return None
EndFunction

Actor Function GetCrosshairActor() Global
    SAFLog("GetCrosshairActor -> None (SHIM-GAP)")
    return None
EndFunction

Actor Function FindActorNearCrosshair(Float maxAngleDeg = 15.0, Float maxDist = 2000.0) Global
    SAFLog("FindActorNearCrosshair -> None (SHIM-GAP, no candidate enumeration)")
    return None
EndFunction

Int Function AddActorToSelectionBuffer(Float maxAngleDeg = 15.0, Float maxDist = 2000.0) Global
    SAFLog("AddActorToSelectionBuffer -> -1 (SHIM-GAP)")
    return -1
EndFunction

Actor[] Function GetSelectionBuffer() Global
    SAFLog("GetSelectionBuffer -> empty (SHIM-GAP)")
    Actor[] empty = new Actor[1]
    empty[0] = None
    return empty
EndFunction

Int Function GetSelectionBufferSize() Global
    return 0
EndFunction

Function ClearSelectionBuffer() Global
    ; SHIM-GAP no-op.
EndFunction

Int Function SelectActor(Actor akActor) Global
    SAFLog("SelectActor actor=" + akActor + " -> -1 (SHIM-GAP)")
    return -1
EndFunction

; --- Event registration -- SHIM-GAP: SAF dispatches to a ScriptObject INSTANCE
; method; OSF registers a GLOBAL function by name. Bridging needs a stored
; instance + DispatchMethodCall, so these are stubbed (callbacks do NOT fire) ---

Function RegisterForPhaseBegin(ScriptObject akScript, String asFunctionName) Global
    SAFLog("RegisterForPhaseBegin fn=" + asFunctionName + " (SHIM-GAP, callback will NOT fire)")
EndFunction

Function RegisterForSequenceEnd(ScriptObject akScript, String asFunctionName) Global
    SAFLog("RegisterForSequenceEnd fn=" + asFunctionName + " (SHIM-GAP, callback will NOT fire)")
EndFunction

Function UnregisterForPhaseBegin(ScriptObject akScript) Global
    SAFLog("UnregisterForPhaseBegin (SHIM-GAP, no-op)")
EndFunction

Function UnregisterForSequenceEnd(ScriptObject akScript) Global
    SAFLog("UnregisterForSequenceEnd (SHIM-GAP, no-op)")
EndFunction
