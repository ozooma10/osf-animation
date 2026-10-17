ScriptName OSFLoopGlobals Hidden

Function Log(string text) Global
    Debug.TraceUser("OSFLoopGlobals", text)
EndFunction

Function OnSceneEvent(OSFTypes:SceneEvent data) Global
    if data != None
        Log("EVENT|" + data.sceneId + "|" + data.sceneHandle + "|" + data.eventType + "|" + data.cue + "|" + data.cause)
    endif
EndFunction

Function BeginPauseCase(string sceneId) Global
    Actor[] actors = new Actor[1]
    actors[0] = Game.GetPlayer()
    Debug.OpenUserLog("OSFLoopGlobals")
    int handle = OSF.StartScene(actors, sceneId)
    Log("PAUSECASE|" + sceneId + "|" + handle)
EndFunction

Function EndPauseCase() Global
    OSF.StopSceneForActor(Game.GetPlayer())
EndFunction

Function RunCase(string sceneId, int expected, GlobalVariable loopGlobal, float changeAfterStart = -999.0, float scale = 1.0) Global
    Actor[] actors = new Actor[1]
    actors[0] = Game.GetPlayer()
    OSFTypes:SceneOptions opts = new OSFTypes:SceneOptions
    opts.LoopScale = scale
    opts.Camera = "none"
    opts.StripMode = 0
    opts.LockPlayerMode = 0
    opts.PlayerControlMode = 0
    opts.FadeMode = 0
    opts.InPlaceMode = 1
    int handle = OSF.StartScene(actors, sceneId, opts)
    Log("CASE|" + sceneId + "|" + handle + "|" + expected)
    if handle == 0
        Log("FAIL|start|" + sceneId)
        return
    endif
    if changeAfterStart != -999.0
        loopGlobal.SetValue(changeAfterStart)
        Log("CHANGED|" + sceneId + "|" + loopGlobal.GetValue())
    endif
    int waits = 0
    while OSF.IsPlaying(actors[0]) && waits < 200
        Utility.Wait(0.1)
        waits += 1
    endwhile
    if OSF.IsPlaying(actors[0])
        Log("FAIL|timeout|" + sceneId)
        OSF.StopScene(handle)
    endif
    Utility.Wait(0.5)
EndFunction

Function Run(string runId) Global
    Debug.OpenUserLog("OSFLoopGlobals")
    Log("RUN|" + runId)
    GlobalVariable loopGlobal = Game.GetFormFromFile(0x800, "OSFLoopGlobals.esm") as GlobalVariable
    if loopGlobal == None || !OSF.IsReady() || OSF.IsPlaying(Game.GetPlayer())
        Log("FAIL|precondition")
        return
    endif
    int token = OSF.RegisterSceneCallbackStatic("OSFLoopGlobals", "OnSceneEvent")
    if token == 0
        Log("FAIL|callback")
        return
    endif
    loopGlobal.SetValue(2.0)
    Log("GLOBAL|" + loopGlobal.GetValue())
    RunCase("osf.global.live", 2, loopGlobal, 4.0)
    RunCase("osf.global.changed", 4, loopGlobal)
    loopGlobal.SetValue(2.0)
    RunCase("osf.global.graph", 2, loopGlobal)
    RunCase("osf.global.scaled", 4, loopGlobal, -999.0, 2.0)
    RunCase("osf.global.missing-plugin", 3, loopGlobal)
    RunCase("osf.global.missing-form", 3, loopGlobal)
    loopGlobal.SetValue(-1.0)
    RunCase("osf.global.invalid-value", 3, loopGlobal)
    OSF.UnregisterSceneCallback(token)
    Log("DONE|" + runId)
EndFunction
