Scriptname SAF extends ScriptObject

; SAF -> OSF compatibility shim. Maintains SAF mods not ported to OSF;
; Do not use these methods, use OSF methods for future mods.

Struct SequencePhase
    Int numLoops = 0            ; -1 = loop forever, 0 = play once
    Float transitionTime = 1.0  ; blend-in seconds (visual only)
    String filePath             ; anim path relative to Data (as in NAF/SAF)
EndStruct

; Logs "[SAF->OSF] ..." to the Papyrus log (and on-screen if DebugNotify).
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

; prepends the SAF animation root to the given animation ID.
String Function ResolveAnim(String asAnimId) Global
    If asAnimId == ""
        return asAnimId
    EndIf
    return "SAF\\Animations\\" + asAnimId
EndFunction

Function EngagePlayerLockIfPlayer(Actor akActor) Global
    If akActor != None && akActor == Game.GetPlayer()
        SAFLog("Engaging player lock -> ControlLock + CameraLock (on)")
        OSFCompat.SetPlayerControlLock(true)
        OSFCompat.SetPlayerCameraLock(true)
    EndIf
EndFunction

Function ReleasePlayerLockIfPlayer(Actor akActor) Global
    If akActor != None && akActor == Game.GetPlayer()
        SAFLog("Releasing player lock -> ControlLock + CameraLock (off)")
        OSFCompat.SetPlayerControlLock(false)
        OSFCompat.SetPlayerCameraLock(false)
    EndIf
EndFunction

Function PlayAnimation(Actor akTarget, String asAnim, Float fTransitionSeconds = 1.0) Global
    SAFLog("PlayAnimation target=" + akTarget + " anim=" + asAnim)
    OSF.Play(akTarget, ResolveAnim(asAnim))
EndFunction

Function PlayAnimationOnce(Actor akTarget, String asAnim, Float fTransitionSeconds = 1.0) Global
    SAFLog("PlayAnimationOnce target=" + akTarget + " anim=" + asAnim)
    OSF.Play(akTarget, ResolveAnim(asAnim))
EndFunction

Bool Function StopAnimation(Actor akTarget, Float fTransitionSeconds = 1.0) Global
    If akTarget == None
        return false
    EndIf
    ; Release any standalone lock this shim engaged directly (solo PlayOnActorLocked /
    ; LockActorForAnimationRestrained). Idempotent + ref-count-guarded, so it's a no-op when the
    ; player wasn't locked; for a scene, the runtime's own lock is released by the stop below.
    ReleasePlayerLockIfPlayer(akTarget)
    ; Stop a scene first: StopSceneForActor reaches ANY scene the actor is in (linear OR graph)
    ; plus a PlaySequence solo sequence -- OSF.Stop would refuse the participant. Falls back to a
    ; solo Stop when the actor is in no scene.
    If OSF.StopSceneForActor(akTarget)
        SAFLog("StopAnimation target=" + akTarget + " (scene)")
        return true
    EndIf
    SAFLog("StopAnimation target=" + akTarget + " (solo -> Stop)")
    return OSF.Stop(akTarget)
EndFunction

; Sync the animation clocks of the given actors, and co-locate them.
Function SyncAnimations(Actor[] akTargets) Global
    Int n = 0
    If akTargets != None
        n = akTargets.Length
    EndIf
    SAFLog("SyncAnimations count=" + n + " -> OSF.Sync(anchor)")
    ; OSF drives playback through the engine's per-actor anim-graph update, which does NOT tick for an
    ; AI-disabled actor — that actor freezes and the scene stalls. Some SAF mods disable a participant's AI
    ; mid-scene (SnuSnu's oral poses, to hold the head for a face morph); re-enable it so OSF can pose the rig.
    ; The actor still stays put via SetRestrained + the scene's animation-driven pin.
    Int i = 0
    While i < n
        If akTargets[i] != None && akTargets[i].IsAIEnabled() == False
            akTargets[i].EnableAI(true)
        EndIf
        i += 1
    EndWhile
    If n >= 2
        OSF.Sync(akTargets, true)
    EndIf
EndFunction

; Removes the given actor from the sync group.
Function StopSyncing(Actor akTarget) Global
    SAFLog("StopSyncing target=" + akTarget + " -> OSF.StopSceneForActor")
    OSF.StopSceneForActor(akTarget)
EndFunction

; Maps to OSFAdvanced.PlaySequence.
; SequencePhase.numLoops: -1 = loop forever (hold until AdvanceSequence), 0 = play once then advance,
; N = N loops then advance.
Function StartSequence(Actor akTarget, SequencePhase[] sPhases, Bool bLoop) Global
    Int phaseCount = 0
    If sPhases != None
        phaseCount = sPhases.Length
    EndIf
    SAFLog("StartSequence target=" + akTarget + " phases=" + phaseCount + " loop=" + bLoop)
    If akTarget == None || phaseCount <= 0
        return
    EndIf
    String[] files = new String[phaseCount]
    Int[] loops = new Int[phaseCount]
    Float[] blends = new Float[phaseCount]
    Int i = 0
    While i < phaseCount
        files[i] = ResolveAnim(sPhases[i].filePath)
        Int nl = sPhases[i].numLoops
        If nl < 0
            loops[i] = 0          ; loop forever -> hold this phase
        ElseIf nl == 0
            loops[i] = 1          ; play once -> advance
        Else
            loops[i] = nl
        EndIf
        blends[i] = sPhases[i].transitionTime
        i += 1
    EndWhile
    OSFAdvanced.PlaySequence(akTarget, files, loops, blends, bLoop)
EndFunction

; advance to the next stage.
Bool Function AdvanceSequence(Actor akTarget, Bool bSmooth) Global
    Int cur = OSF.GetSceneStageForActor(akTarget)
    If cur < 0
        SAFLog("AdvanceSequence target=" + akTarget + " -> false (not in a sequence)")
        return false
    EndIf
    Bool ok = OSF.SetSceneStageForActor(akTarget, cur + 1)
    SAFLog("AdvanceSequence target=" + akTarget + " " + cur + " -> " + (cur + 1) + " = " + ok)
    return ok
EndFunction

Bool Function SetSequencePhase(Actor akTarget, Int iPhase) Global
    SAFLog("SetSequencePhase target=" + akTarget + " phase=" + iPhase)
    ; Only meaningful if akTarget is in an OSF staged scene (StartScene).
    return OSF.SetSceneStageForActor(akTarget, iPhase)
EndFunction

Int Function GetSequencePhase(Actor akTarget) Global
    Int phase = OSF.GetSceneStageForActor(akTarget)  ; -1 if not in a staged scene
    SAFLog("GetSequencePhase target=" + akTarget + " -> " + phase)
    return phase
EndFunction

; Position / AI locking - no-ops:
; OSF anchors/pins participants automatically on scene start.
; Player control is NOT auto-locked by the core (use OSFCompat).

Function SetPositionLocked(Actor akTarget, Bool bLocked) Global
    SAFLog("SetPositionLocked target=" + akTarget + " locked=" + bLocked + " (no-op, OSF auto-pins)")
EndFunction

; OSF anchors/pins NPCs via the scene; only a player participant needs control + camera frozen.
; SAF may pass akActor=None with abIsPlayer=true to mean "player".
Function LockActorForAnimationRestrained(Actor akActor, Float fX, Float fY, Float fZ, Bool abIsPlayer = false) Global
    Actor target = akActor
    If target == None && abIsPlayer
        target = Game.GetPlayer()
    EndIf
    SAFLog("LockActorForAnimationRestrained actor=" + target + " (OSF anchors NPCs; player gets the control lock)")
    EngagePlayerLockIfPlayer(target)
EndFunction

Function UnlockActorAfterAnimationRestrained(Actor akActor, Bool abIsPlayer = false) Global
    Actor target = akActor
    If target == None && abIsPlayer
        target = Game.GetPlayer()
    EndIf
    SAFLog("UnlockActorAfterAnimationRestrained actor=" + target)
    ReleasePlayerLockIfPlayer(target)
EndFunction

Function PlayOnActorLocked(Actor akActor, String asAnim, Float fSpeed = 1.0, Int animIndex = 0) Global
    SAFLog("PlayOnActorLocked actor=" + akActor + " anim=" + asAnim + " speed=" + fSpeed)
    ; animIndex dropped (single-clip GLBs); fSpeed is a multiplier (1.0 = authored).
    If akActor != None
        If OSF.Play(akActor, ResolveAnim(asAnim))
            If fSpeed != 1.0
                OSF.SetSpeed(akActor, fSpeed)
            EndIf
            EngagePlayerLockIfPlayer(akActor)
        EndIf
    EndIf
EndFunction

Function PlayOnPlayerLocked(String asAnim, Float fSpeed = 1.0, Int animIndex = 0) Global
    SAFLog("PlayOnPlayerLocked anim=" + asAnim)
    PlayOnActorLocked(Game.GetPlayer(), asAnim, fSpeed, animIndex)  ; engages the player lock + speed
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
    ; OSF now enforces actor-exclusivity (one live scene per actor); SAF replayed freely,
    ; so stop any existing scene on these actors first to preserve replace-on-replay.
    OSF.StopSceneForActor(akActor1)
    OSF.StopSceneForActor(akActor2)
    OSFTypes:SceneOptions opts = new OSFTypes:SceneOptions
    opts.Speed = fSpeed
    ; Don't lock the player here: the scene runtime locks a player participant's control +
    ; camera on start and releases both on scene end (the ledger). The camera hold is
    ; ref-counted, so a duplicate acquire from this shim would leak past scene end.
    OSFAdvanced.StartSceneFiles(actors, files, opts)
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

; Event registration -- no consumers found; rely on the OSF event system rather
; than replicate the SAF dispatch. Stubbed (callbacks do NOT fire).

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

; Crosshair pickers -- native fast-path (OSFCompat.GetCrosshairActor reads the
; engine crosshair target, PlayerCharacter->commandTarget) restores SAF's native
; crosshairRef behavior; the pure-Papyrus cone search (heading angle + distance)
; remains as a forgiving fallback when the reticle is not directly on a listed actor.

Actor Function PickActorFromCrosshair(Actor[] sceneActors, Float maxAngle = 20.0, Float maxDist = 500.0) Global
    If sceneActors == None || sceneActors.Length == 0
        SAFLog("PickActorFromCrosshair empty list -> None")
        return None
    EndIf

    ; Fast-path: if the engine crosshair is squarely on one of the listed actors,
    ; take it -- precise + occlusion-aware (no heading/distance approximation).
    Actor hit = OSFCompat.GetCrosshairActor()
    If hit && sceneActors.Find(hit) >= 0
        SAFLog("PickActorFromCrosshair native crosshair -> " + hit)
        return hit
    EndIf

    ; Fallback: cone search for when the reticle is not pixel-on a listed actor.
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

    SAFLog("PickActorFromCrosshair count=" + sceneActors.Length + " (cone) -> " + best)
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
