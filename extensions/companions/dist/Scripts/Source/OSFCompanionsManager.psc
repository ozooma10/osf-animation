Scriptname OSFCompanionsManager Extends Quest

Actor partner
int sceneHandle = 0
int callbackToken = 0
int pendingTicks = 0
int activeTicks = 0
bool pending = false

Event OnInit()
    Initialize()
EndEvent

Function Initialize()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
    if callbackToken != 0
        OSF.UnregisterSceneCallback(callbackToken)
    endif
    callbackToken = OSF.RegisterSceneCallback(self, "OnSceneEvent", 0, OSF.EVENT_SCENE_END())
EndFunction

Event Actor.OnPlayerLoadGame(Actor akSender)
    ; Runtime handles do not survive world replacement. Never stop a stale handle.
    sceneHandle = 0
    pending = false
    partner = None
    CancelTimer(1)
    Initialize()
EndEvent

string Function Eligibility(Actor a, bool duringHug = false)
    Actor p = Game.GetPlayer()
    if !OSF.IsReady()
        return "OSF Animation is not ready."
    endif
    if !a || !a.Is3DLoaded() || a.IsDisabled() || a.IsDead() || p.IsDead()
        return "Sarah needs to be here and available."
    endif
    if a.IsInCombat() || p.IsInCombat() || a.IsBleedingOut() != 0 || p.IsBleedingOut() != 0
        return "Let's wait until things are calm."
    endif
    if a.GetSitState() != 0 || p.GetSitState() != 0 || a.IsOnMount() || p.IsOnMount() || a.IsFlying() || p.IsFlying()
        return "Stand together on level ground first."
    endif
    if a.GetParentCell() != p.GetParentCell()
        return "Sarah needs to be in the same area."
    endif
    if a.GetGravityScale() < 0.1 || p.GetGravityScale() < 0.1
        return "This hug needs gravity and solid ground."
    endif
    if !duringHug
        if OSF.IsPlaying(a) || OSF.IsPlaying(p) || a.IsInScene() || p.IsInScene()
            return "Finish the current interaction first."
        endif
        ; Cell-space coordinates are metres, matching the authored OSF clips.
        float[] there = a.GetSpacePosition()
        float[] here = p.GetSpacePosition()
        if there.Length != 3 || here.Length != 3
            return "Unable to find a suitable place for the hug."
        endif
        float dx = there[0] - here[0]
        float dy = there[1] - here[1]
        if dx*dx + dy*dy > 3.24 || Math.Abs(there[2] - here[2]) > 0.2
            return "Move close to Sarah on level, open ground."
        endif
    endif
    return ""
EndFunction

bool Function RequestHug()
    if pending || sceneHandle != 0
        return false
    endif
    partner = Game.GetFormFromFile(0x5986, "Starfield.esm") as Actor
    if !partner || !partner.Is3DLoaded()
        Debug.Notification("Sarah needs to be nearby.")
        return false
    endif
    ; Queue until the dialogue scene and camera have released both actors.
    pending = true
    pendingTicks = 0
    Initialize()
    StartTimer(0.25, 1)
    return true
EndFunction

Event OnTimer(int aiTimerID)
    if aiTimerID != 1
        return
    endif
    Actor p = Game.GetPlayer()
    if pending
        pendingTicks += 1
        if !partner || !partner.Is3DLoaded() || partner.IsInCombat() || p.IsInCombat()
            pending = false
            Debug.Notification("The hug was cancelled.")
            return
        endif
        if partner.IsInDialogueWithPlayer() || partner.IsInScene() || p.IsInScene()
            if pendingTicks >= 32
                pending = false
                Debug.Notification("Finish the conversation, then try again.")
            else
                StartTimer(0.25, 1)
            endif
            return
        endif
        pending = false
        string reason = Eligibility(partner)
        if reason != ""
            Debug.Notification(reason)
            Debug.Trace("[OSF Companions] Refused: " + reason)
            return
        endif
        Actor[] cast = new Actor[2]
        cast[0] = partner
        cast[1] = p
        OSFTypes:SceneOptions opts = new OSFTypes:SceneOptions
        opts.Anchor = p
        opts.StripMode = 0
        opts.FadeMode = 0
        opts.Camera = "scene_orbit"
        ; The converted pair faces along -Y from the player's role.
        float heading = p.GetAngleZ() + p.GetHeadingAngle(partner) + 180.0
        while heading >= 360.0
            heading -= 360.0
        endwhile
        while heading < 0.0
            heading += 360.0
        endwhile
        opts.HeadingDeg = heading
        sceneHandle = OSF.StartScene(cast, "osf.companions.sarah.hug", opts)
        if sceneHandle == 0
            Debug.Notification("The hug could not start. Check OSF Companions' files.")
            return
        endif
        activeTicks = 0
        Debug.Trace("[OSF Companions] Hug started: " + sceneHandle)
        StartTimer(0.25, 1)
    elseif sceneHandle != 0
        activeTicks += 1
        if Eligibility(partner, true) != "" || activeTicks >= 120
            CancelHug()
        elseif !OSF.IsPlaying(p)
            sceneHandle = 0
            partner = None
        else
            StartTimer(0.25, 1)
        endif
    endif
EndEvent

Function OnSceneEvent(OSFTypes:SceneEvent akEvent)
    if akEvent.sceneHandle == sceneHandle && akEvent.eventType == OSF.EVENT_SCENE_END()
        Debug.Trace("[OSF Companions] Hug ended: " + sceneHandle)
        sceneHandle = 0
        partner = None
        CancelTimer(1)
    endif
EndFunction

Function CancelHug()
    pending = false
    CancelTimer(1)
    int ownedScene = sceneHandle
    sceneHandle = 0
    partner = None
    if ownedScene != 0
        OSF.StopScene(ownedScene)
    endif
EndFunction

Function PrintStatus()
    Actor sarah = Game.GetFormFromFile(0x5986, "Starfield.esm") as Actor
    Debug.Trace("[OSF Companions] handle=" + sceneHandle + " pending=" + pending)
    if sarah
        Debug.Trace("[OSF Companions] distance=" + sarah.GetDistance(Game.GetPlayer()) + " dz=" + (sarah.GetPositionZ() - Game.GetPlayer().GetPositionZ()))
        Debug.Trace("[OSF Companions] Sarah space=" + sarah.GetSpacePosition() + " player space=" + Game.GetPlayer().GetSpacePosition())
    endif
    Debug.Notification("OSF Companions: scene " + sceneHandle + ", pending " + pending)
EndFunction
