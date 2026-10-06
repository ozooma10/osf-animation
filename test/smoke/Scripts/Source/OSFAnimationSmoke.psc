ScriptName OSFAnimationSmoke Native Hidden

Function Report(string runId, bool ready, string version, int token) Global Native
Function ReportEvent(int sceneHandle, int eventType, string cue) Global Native
Function ReportPeer(string runId, Actor peer) Global Native
Function ReportRetired(string runId) Global Native

Function Probe(string runId) Global
    int token = OSF.RegisterSceneCallbackStatic("OSFAnimationSmoke", "OnSceneEvent")
    Report(runId, OSF.IsReady(), OSF.GetVersion(), token)
EndFunction

Function SpawnPeer(string runId) Global
    Actor player = Game.GetPlayer()
    Actor peer = player.PlaceActorAtMe(player.GetBaseObject() as ActorBase)
    ReportPeer(runId, peer)
EndFunction

Function RetirePeer(string runId, Actor peer) Global
    if peer != None
        peer.Disable()
        peer.Delete()
    endif
    ReportRetired(runId)
EndFunction

Function OnSceneEvent(OSFTypes:SceneEvent data) Global
    if data != None
        ReportEvent(data.sceneHandle, data.eventType, data.cue)
    endif
EndFunction
