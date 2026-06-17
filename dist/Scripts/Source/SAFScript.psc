Scriptname SAFScript extends ScriptObject

; SAFScript -> OSF compatibility shim. 
; Prefer using the OSF functions directly when possible.

Bool Function DebugEnabled() Global
    return True
EndFunction

; Logs "[SAFScript->OSF] ..." to the Papyrus log. The distinct prefix keeps this
; shim's calls separable from the struct-based SAF shim's "[SAF->OSF]" lines.
Function SAFLog(String asMsg) Global
    If DebugEnabled()
        Debug.Trace("[SAFScript->OSF] " + asMsg)
    EndIf
EndFunction

; Prepends the SAF animation root to the given animation ID. Delegates to
; SAF.ResolveAnim -- both shims share the root, and the helper is pure (no log).
String Function ResolveAnim(String asAnimId) Global
    return SAF.ResolveAnim(asAnimId)
EndFunction

Function ReleasePlayerLockIfPlayer(Actor akActor) Global
    If akActor != None && akActor == Game.GetPlayer()
        SAFLog("Releasing player lock -> ControlLock + CameraLock (off)")
        OSFCompat.SetPlayerControlLock(false)
        OSFCompat.SetPlayerCameraLock(false)
    EndIf
EndFunction


Function EngagePlayerLockIfPlayer(Actor akActor) Global
    If akActor != None && akActor == Game.GetPlayer()
        SAFLog("Engaging player lock -> ControlLock + CameraLock (on)")
        OSFCompat.SetPlayerControlLock(true)
        OSFCompat.SetPlayerCameraLock(true)
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
    ; OSF.Stop refuses scene participants -- route them to StopSceneForActor (StopScene is
    ; handle-keyed now; the actor convenience preserves this shim's actor-centric call).
    If OSF.GetSceneStageForActor(akActor) >= 0
        SAFLog("StopAnimation actor=" + akActor + " (scene -> StopSceneForActor)")
        return OSF.StopSceneForActor(akActor)
    EndIf
    SAFLog("StopAnimation actor=" + akActor + " (solo -> Stop)")
    return OSF.Stop(akActor)
EndFunction

; Returns the FULL Data-relative path (with the ResolveAnim root prepended), not
; the bare id SAF returned -- Papyrus has no substring to strip it back.
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
    ; OSF now enforces actor-exclusivity; SAF replayed freely, so clear any existing scene
    ; on these actors first to preserve replace-on-replay.
    OSF.StopSceneForActor(akActor1)
    OSF.StopSceneForActor(akActor2)
    Bool ok = OSF.StartSceneFiles(actors, files, fSpeed)
    If ok
        ; SAF froze the player in player-participant scenes — restore that.
        EngagePlayerLockIfPlayer(akActor1)
        EngagePlayerLockIfPlayer(akActor2)
    EndIf
    return ok
EndFunction

Bool Function PlaySceneSeparate(Actor akActor1, Actor akActor2, String asAnim1, String asAnim2, Float fSpeed = 1.0) Global
    SAFLog("PlaySceneSeparate -> PlayScene")
    ; OSF.StartSceneFiles already refreshes capsules / anchors actor2; same path.
    return PlayScene(akActor1, akActor2, asAnim1, asAnim2, fSpeed)
EndFunction

; --- Speed ---

Function SetAnimationSpeed(Actor akActor, Float fSpeed) Global
    ; SAF/NAF speed is a percentage (100 = normal); OSF.SetSpeed is a multiplier.
    ; (Mirrors SAF.SetAnimationSpeed — both standalone speed setters take percent.)
    Float mult = fSpeed / 100.0
    SAFLog("SetAnimationSpeed actor=" + akActor + " speed=" + fSpeed + " -> OSF.SetSpeed " + mult)
    OSF.SetSpeed(akActor, mult)
EndFunction

Float Function GetAnimationSpeed(Actor akActor) Global
    ; OSF multiplier -> SAF/NAF percentage.
    Float pct = OSF.GetSpeed(akActor) * 100.0
    SAFLog("GetAnimationSpeed actor=" + akActor + " -> " + pct)
    return pct
EndFunction

; --- Position / anchoring -- no-ops: OSF anchors participants and pins the compose root automatically. 
; Real anchoring happens in SyncGraphs / scene start.

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

; OSF anchors/pins NPCs via the scene, so positional locking is a no-op for them;
; but a player participant still needs control/camera frozen (SAF's lock). 
; SAF may pass akActor=None with abIsPlayer=true to mean "the player".
Function LockActorForAnimation(Actor akActor, Float fX, Float fY, Float fZ, Bool abIsPlayer = false) Global
    Actor target = akActor
    If target == None && abIsPlayer
        target = Game.GetPlayer()
    EndIf
    SAFLog("LockActorForAnimation actor=" + target + " (" + fX + "," + fY + "," + fZ + ") (OSF anchors NPCs at scene start; player gets the control lock)")
    EngagePlayerLockIfPlayer(target)
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

; SAF's SyncGraphs frame-locks a group on one shared clock.
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

; --- Sequences -- map onto OSF.PlaySequence (solo multi-phase) ---

; Each path is one phase; phases play once then auto-advance, bLoop restarts the
; whole sequence after the last (OSF.PlaySequence).
Function StartSequence(Actor akActor, String[] asPaths, Bool bLoop) Global
    Int n = 0
    If asPaths != None
        n = asPaths.Length
    EndIf
    SAFLog("StartSequence actor=" + akActor + " paths=" + n + " loop=" + bLoop)
    If akActor == None || n <= 0
        return
    EndIf
    String[] files = new String[n]
    Int[] loops = new Int[n]
    Float[] blends = new Float[n]
    Int i = 0
    While i < n
        files[i] = ResolveAnim(asPaths[i])
        loops[i] = 1       ; play this phase once, then advance
        blends[i] = 0.3
        i += 1
    EndWhile
    OSF.PlaySequence(akActor, files, loops, blends, bLoop)
EndFunction

; Manual advance to the next phase (PlaySequence builds a staged scene; jump it).
; False when not in a sequence or already past the last phase.
Bool Function AdvanceSequence(Actor akActor, Bool bSmooth) Global
    Int cur = OSF.GetSceneStageForActor(akActor)
    If cur < 0
        SAFLog("AdvanceSequence actor=" + akActor + " -> false (not in a sequence)")
        return false
    EndIf
    Bool ok = OSF.SetSceneStageForActor(akActor, cur + 1)
    SAFLog("AdvanceSequence actor=" + akActor + " " + cur + " -> " + (cur + 1) + " = " + ok)
    return ok
EndFunction

Bool Function SetSequencePhase(Actor akActor, Int iPhase) Global
    SAFLog("SetSequencePhase actor=" + akActor + " phase=" + iPhase)
    ; Only meaningful if akActor is in an OSF staged scene.
    return OSF.SetSceneStageForActor(akActor, iPhase)
EndFunction

Int Function GetSequencePhase(Actor akActor) Global
    Int phase = OSF.GetSceneStageForActor(akActor)
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


ObjectReference Function GetCrosshairRef() Global
    ObjectReference ref = OSFCompat.GetCrosshairRef()
    SAFLog("GetCrosshairRef -> " + ref)
    return ref
EndFunction

Actor Function GetCrosshairActor() Global
    Actor a = OSFCompat.GetCrosshairActor()
    SAFLog("GetCrosshairActor -> " + a)
    return a
EndFunction

Actor Function FindActorNearCrosshair(Float maxAngleDeg = 15.0, Float maxDist = 2000.0) Global
    Actor a = OSFCompat.GetCrosshairActor()
    SAFLog("FindActorNearCrosshair -> " + a)
    return a
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

; --- Event registration -- SHIM-GAP: Couldnt find any true consumers of these events.

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
