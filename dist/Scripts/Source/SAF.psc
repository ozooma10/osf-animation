Scriptname SAF extends ScriptObject

; SAF -> OSF compatibility shim. Maintains SAF mods not ported to OSF; not full
; coverage (functions tagged SHIM-GAP have no OSF equivalent yet).

Struct SequencePhase
    Int numLoops = 0            ; -1 = loop forever, 0 = play once
    Float transitionTime = 1.0  ; blend-in seconds (visual only)
    String filePath             ; anim path relative to Data (as in NAF/SAF)
EndStruct

; Logs "[SAF->OSF] ..." to the Papyrus log (and on-screen if DebugNotify). Both
; are compile-time toggles (pure-global Papyrus has no mutable static state).
Bool Function DebugEnabled() Global
    return True
EndFunction

Bool Function DebugNotify() Global
    return False
EndFunction

Function SAFLog(String asMsg) Global
    If DebugEnabled()
        Debug.Trace("[SAF->OSF] " + asMsg)
        If DebugNotify()
            Debug.Notification("[SAF] " + asMsg)
        EndIf
    EndIf
EndFunction

; Prepends the SAF animation root: consumers pass ids without it (e.g.
; "GE\Flr\x.glb") but OSF wants a Data-relative path. Kept local (not a call into
; SAFScript.ResolveAnim) so a mixed setup with the real SAF native still resolves;
; keep this root in sync with SAFScript.ResolveAnim.
String Function ResolveAnim(String asAnimId) Global
    If asAnimId == ""
        return asAnimId
    EndIf
    return "SAF\\Animations\\" + asAnimId
EndFunction

; Single-actor playback. fTransitionSeconds ignored (OSF uses its blend default);
; OSF.Play is already single (non-looping).

Function PlayAnimation(Actor akTarget, String asAnim, Float fTransitionSeconds = 1.0) Global
    SAFLog("PlayAnimation target=" + akTarget + " anim=" + asAnim)
    OSF.Play(akTarget, ResolveAnim(asAnim))
EndFunction

Function PlayAnimationOnce(Actor akTarget, String asAnim, Float fTransitionSeconds = 1.0) Global
    SAFLog("PlayAnimationOnce target=" + akTarget + " anim=" + asAnim)
    OSF.Play(akTarget, ResolveAnim(asAnim))
EndFunction

Bool Function StopAnimation(Actor akTarget, Float fTransitionSeconds = 1.0) Global
    ; OSF.Stop refuses scene participants -- route them to StopScene.
    If OSF.GetSceneStage(akTarget) >= 0
        SAFLog("StopAnimation target=" + akTarget + " (scene -> StopScene)")
        return OSF.StopScene(akTarget)
    EndIf
    SAFLog("StopAnimation target=" + akTarget + " (solo -> Stop)")
    return OSF.Stop(akTarget)
EndFunction

; Frame-locks the group's clocks on one shared clock (SAF's SyncAnimations was a
; clock frame-lock). Mirrors SAFScript.SyncGraphs.
Function SyncAnimations(Actor[] akTargets) Global
    Int n = 0
    If akTargets != None
        n = akTargets.Length
    EndIf
    SAFLog("SyncAnimations count=" + n + " -> OSF.Sync")
    If n <= 0
        return
    EndIf
    OSF.Sync(akTargets)
EndFunction

Function StopSyncing(Actor akTarget) Global
    SAFLog("StopSyncing target=" + akTarget)
    ; OSF has no "leave the sync group"; stop the graph instead.
    OSF.Stop(akTarget)
EndFunction

; SHIM-GAP: no OSF native for an ad-hoc staged sequence from an arbitrary path
; array (staged playback comes from pack-defined scenes). Plays phase 0 only;
; phases do NOT auto-advance.
Function StartSequence(Actor akTarget, SequencePhase[] sPhases, Bool bLoop) Global
    Int phaseCount = 0
    If sPhases != None
        phaseCount = sPhases.Length
    EndIf
    SAFLog("StartSequence target=" + akTarget + " phases=" + phaseCount + " loop=" + bLoop + " (SHIM-GAP, plays phase 0 only)")
    If phaseCount > 0
        OSF.Play(akTarget, ResolveAnim(sPhases[0].filePath))
    EndIf
EndFunction

Bool Function AdvanceSequence(Actor akTarget, Bool bSmooth) Global
    SAFLog("AdvanceSequence target=" + akTarget + " smooth=" + bSmooth + " -> false (SHIM-GAP)")
    ; SHIM-GAP: depends on StartSequence's missing sequence model.
    return false
EndFunction

Bool Function SetSequencePhase(Actor akTarget, Int iPhase) Global
    SAFLog("SetSequencePhase target=" + akTarget + " phase=" + iPhase)
    ; Only meaningful if akTarget is in an OSF staged scene (StartScene).
    return OSF.SetSceneStage(akTarget, iPhase)
EndFunction

Int Function GetSequencePhase(Actor akTarget) Global
    Int phase = OSF.GetSceneStage(akTarget)  ; -1 if not in a staged scene
    SAFLog("GetSequencePhase target=" + akTarget + " -> " + phase)
    return phase
EndFunction

; Position / AI locking -- no-ops: OSF anchors/pins participants automatically on
; scene start. Player control is NOT auto-locked by the core (use OSFCompat).

Function SetPositionLocked(Actor akTarget, Bool bLocked) Global
    SAFLog("SetPositionLocked target=" + akTarget + " locked=" + bLocked + " (no-op, OSF auto-pins)")
EndFunction

Function LockActorForAnimationRestrained(Actor akActor, Float fX, Float fY, Float fZ, Bool abIsPlayer = false) Global
    SAFLog("LockActorForAnimationRestrained actor=" + akActor + " (no-op, OSF auto-locks)")
EndFunction

Function UnlockActorAfterAnimationRestrained(Actor akActor, Bool abIsPlayer = false) Global
    SAFLog("UnlockActorAfterAnimationRestrained actor=" + akActor + " (no-op, OSF drops on StopScene)")
EndFunction

Function PlayOnActorLocked(Actor akActor, String asAnim, Float fSpeed = 1.0, Int animIndex = 0) Global
    SAFLog("PlayOnActorLocked actor=" + akActor + " anim=" + asAnim)
    ; fSpeed / animIndex dropped; locking handled by OSF.
    If akActor != None
        OSF.Play(akActor, ResolveAnim(asAnim))
    EndIf
EndFunction

Function PlayOnPlayerLocked(String asAnim, Float fSpeed = 1.0, Int animIndex = 0) Global
    SAFLog("PlayOnPlayerLocked anim=" + asAnim)
    OSF.Play(Game.GetPlayer(), ResolveAnim(asAnim))
EndFunction

; Two-actor scenes (actor1 is the anchor; OSF teleports actor2 to it).

Function PlaySceneLocked(Actor akActor1, Actor akActor2, String asAnim1, String asAnim2, Float fSpeed = 1.0) Global
    SAFLog("PlaySceneLocked a1=" + akActor1 + " a2=" + akActor2 + " anim1=" + asAnim1 + " anim2=" + asAnim2)
    If akActor1 == None || akActor2 == None
        SAFLog("PlaySceneLocked aborted -- None actor")
        return
    EndIf
    Actor[] actors = new Actor[2]
    actors[0] = akActor1
    actors[1] = akActor2
    String[] files = new String[2]
    files[0] = ResolveAnim(asAnim1)
    files[1] = ResolveAnim(asAnim2)
    OSF.StartSceneFiles(actors, files, fSpeed)
EndFunction

Function PlaySceneLockedPlayerNPC(Actor akNPC, String asPlayerAnim, String asNPCAnim, Float fSpeed = 1.0) Global
    SAFLog("PlaySceneLockedPlayerNPC npc=" + akNPC)
    PlaySceneLocked(Game.GetPlayer(), akNPC, asPlayerAnim, asNPCAnim, fSpeed)
EndFunction

Function PlaySceneSeparateWrapper(Actor akActor1, Actor akActor2, String asAnim1, String asAnim2, Float fSpeed = 1.0) Global
    SAFLog("PlaySceneSeparateWrapper a1=" + akActor1 + " a2=" + akActor2)
    ; OSF.StartSceneFiles co-locates / anchors actor2 (same path), now at fSpeed.
    PlaySceneLocked(akActor1, akActor2, asAnim1, asAnim2, fSpeed)
EndFunction

Function PlaySceneSeparatePlayerNPC(Actor akNPC, String asPlayerAnim, String asNPCAnim, Float fSpeed = 1.0) Global
    SAFLog("PlaySceneSeparatePlayerNPC npc=" + akNPC)
    PlaySceneLocked(Game.GetPlayer(), akNPC, asPlayerAnim, asNPCAnim, fSpeed)
EndFunction

; Approach-then-play: wait loop is plain Papyrus, playback is OSF.

Function PlaySceneWithApproach(Actor akActor1, Actor akActor2, String asAnim1, String asAnim2, Float fSpeed = 1.0, Float fStopDistance = 50.0, Float fTimeout = 10.0, Float fApproachOffset = 40.0) Global
    SAFLog("PlaySceneWithApproach a1=" + akActor1 + " a2=" + akActor2 + " stopDist=" + fStopDistance + " timeout=" + fTimeout)
    If akActor1 == None || akActor2 == None
        SAFLog("PlaySceneWithApproach aborted -- None actor")
        return
    EndIf

    ; Nudge actor2 next to actor1, then wait until it's within range.
    akActor2.MoveTo(akActor1, fApproachOffset, 0.0, 0.0, True)

    Float elapsed = 0.0
    Float step = 0.1
    While elapsed < fTimeout
        If akActor2.GetDistance(akActor1) <= fStopDistance
            elapsed = fTimeout + 1.0
        Else
            Utility.Wait(step)
            elapsed += step
        EndIf
    EndWhile

    SAFLog("PlaySceneWithApproach reached (dist=" + akActor2.GetDistance(akActor1) + "), starting scene")
    PlaySceneLocked(akActor1, akActor2, asAnim1, asAnim2, fSpeed)
EndFunction

Function PlaySceneWithApproachPlayerNPC(Actor akNPC, String asPlayerAnim, String asNPCAnim, Float fSpeed = 1.0, Float fStopDistance = 50.0, Float fTimeout = 10.0) Global
    SAFLog("PlaySceneWithApproachPlayerNPC npc=" + akNPC)
    PlaySceneWithApproach(Game.GetPlayer(), akNPC, asPlayerAnim, asNPCAnim, fSpeed, fStopDistance, fTimeout, 40.0)
EndFunction

; SHIM-GAP: SAF set an ABSOLUTE world position; OSF only exposes a relative nudge
; (AdjustScenePlacement) and a raw SetPosition would fight the compose-root pin.
Function SetActorPosition(Actor akTarget, Float fX, Float fY, Float fZ) Global
    SAFLog("SetActorPosition target=" + akTarget + " (" + fX + "," + fY + "," + fZ + ") (SHIM-GAP, no-op)")
EndFunction

Bool Function SetAnimationSpeed(Actor akTarget, Float fSpeed) Global
    ; SAF/NAF speed is a percentage (100 = normal); OSF.SetSpeed is a multiplier.
    Float mult = fSpeed / 100.0
    SAFLog("SetAnimationSpeed target=" + akTarget + " speed=" + fSpeed + " -> OSF.SetSpeed " + mult)
    return OSF.SetSpeed(akTarget, mult)
EndFunction

Float Function GetAnimationSpeed(Actor akTarget) Global
    ; OSF multiplier -> SAF/NAF percentage.
    Float pct = OSF.GetSpeed(akTarget) * 100.0
    SAFLog("GetAnimationSpeed target=" + akTarget + " -> " + pct)
    return pct
EndFunction

String Function GetCurrentAnimation(Actor akTarget) Global
    ; Returns the FULL Data-relative path (with the root ResolveAnim prepended),
    ; not the bare id SAF returned -- Papyrus has no substring to strip it back.
    String path = OSF.GetCurrentAnimation(akTarget)
    SAFLog("GetCurrentAnimation target=" + akTarget + " -> \"" + path + "\"")
    return path
EndFunction

Bool Function SetBlendGraphVariable(Actor akTarget, String asName, Float fValue) Global
    SAFLog("SetBlendGraphVariable target=" + akTarget + " name=" + asName + " val=" + fValue + " -> false (SHIM-GAP)")
    return false  ; SHIM-GAP: no blend-graph variables in OSF
EndFunction

Float Function GetBlendGraphVariable(Actor akTarget, String asName) Global
    SAFLog("GetBlendGraphVariable target=" + akTarget + " name=" + asName + " -> 0.0 (SHIM-GAP)")
    return 0.0
EndFunction

; Event registration -- SHIM-GAP: SAF dispatches to a ScriptObject INSTANCE
; method; OSF registers a GLOBAL function by name. Bridging needs a stored
; instance + DispatchMethodCall, so these are stubbed (callbacks do NOT fire).

Function RegisterForPhaseBegin(ScriptObject sScript, String sFunctionName) Global
    SAFLog("RegisterForPhaseBegin fn=" + sFunctionName + " (SHIM-GAP, callback will NOT fire)")
EndFunction

Function RegisterForSequenceEnd(ScriptObject sScript, String sFunctionName) Global
    SAFLog("RegisterForSequenceEnd fn=" + sFunctionName + " (SHIM-GAP, callback will NOT fire)")
EndFunction

Function UnregisterForPhaseBegin(ScriptObject sScript) Global
    SAFLog("UnregisterForPhaseBegin (SHIM-GAP, no-op)")
EndFunction

Function UnregisterForSequenceEnd(ScriptObject sScript) Global
    SAFLog("UnregisterForSequenceEnd (SHIM-GAP, no-op)")
EndFunction

; Crosshair pickers -- pure-Papyrus cone search (heading angle + distance); SAF's
; native crosshairRef fast-path is dropped.

Actor Function PickActorFromCrosshair(Actor[] sceneActors, Float maxAngle = 20.0, Float maxDist = 500.0) Global
    If sceneActors == None || sceneActors.Length == 0
        SAFLog("PickActorFromCrosshair empty list -> None")
        return None
    EndIf

    Actor player = Game.GetPlayer()
    Actor best = None
    Float bestScore = maxAngle

    Int i = 0
    While i < sceneActors.Length
        Actor a = sceneActors[i]
        If a
            If player.GetDistance(a) <= maxDist
                Float ang = Math.Abs(player.GetHeadingAngle(a)) ; 0 = dead ahead
                If ang < bestScore
                    bestScore = ang
                    best = a
                EndIf
            EndIf
        EndIf
        i += 1
    EndWhile

    SAFLog("PickActorFromCrosshair count=" + sceneActors.Length + " -> " + best)
    return best
EndFunction

Actor Function PickPairActorFromCrosshair(Actor firstActor, Actor secondActor, Float maxAngle = 20.0, Float maxDist = 500.0) Global
    SAFLog("PickPairActorFromCrosshair a=" + firstActor + " b=" + secondActor)
    Actor[] arr = new Actor[2]
    arr[0] = firstActor
    arr[1] = secondActor
    return PickActorFromCrosshair(arr, maxAngle, maxDist)
EndFunction

; Furniture variants -- teleport + settle (plain Papyrus), then OSF playback.

Function PlayOnActorAtFurniture(Actor akActor, ObjectReference akFurniture, String asAnim, \
        Float fSpeed = 1.0, Float fOffsetX = 0.0, Float fOffsetY = 0.0, Float fOffsetZ = 0.0, \
        Int animIndex = 0) Global
    SAFLog("PlayOnActorAtFurniture actor=" + akActor + " furniture=" + akFurniture + " anim=" + asAnim)
    If akActor == None || akFurniture == None
        SAFLog("PlayOnActorAtFurniture aborted -- None actor/furniture")
        return
    EndIf
    akActor.MoveTo(akFurniture, fOffsetX, fOffsetY, fOffsetZ, True)
    Utility.Wait(0.1) ; let the engine settle the havok capsule before we anchor
    OSF.Play(akActor, ResolveAnim(asAnim))
EndFunction

Function PlaySceneAtFurniture(Actor akActor1, Actor akActor2, ObjectReference akFurniture, \
        String asAnim1, String asAnim2, Float fSpeed = 1.0, \
        Float fOffsetX2 = 40.0, Float fOffsetY2 = 0.0, Float fOffsetZ2 = 0.0) Global
    SAFLog("PlaySceneAtFurniture a1=" + akActor1 + " a2=" + akActor2 + " furniture=" + akFurniture)
    If akActor1 == None || akActor2 == None || akFurniture == None
        SAFLog("PlaySceneAtFurniture aborted -- None actor/furniture")
        return
    EndIf
    akActor1.MoveTo(akFurniture, 0.0, 0.0, 0.0, True)
    akActor2.MoveTo(akFurniture, fOffsetX2, fOffsetY2, fOffsetZ2, True)
    Utility.Wait(0.1)
    PlaySceneLocked(akActor1, akActor2, asAnim1, asAnim2, fSpeed)
EndFunction

Function PlaySceneAtFurniturePlayerNPC(Actor akNPC, ObjectReference akFurniture, \
        String asPlayerAnim, String asNPCAnim, Float fSpeed = 1.0) Global
    SAFLog("PlaySceneAtFurniturePlayerNPC npc=" + akNPC + " furniture=" + akFurniture)
    PlaySceneAtFurniture(Game.GetPlayer(), akNPC, akFurniture, asPlayerAnim, asNPCAnim, fSpeed, 40.0, 0.0, 0.0)
EndFunction
