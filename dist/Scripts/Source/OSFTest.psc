ScriptName OSFTest

; Console-callable wrappers for the current handle + SceneOptions OSF API.
; The natives take Actor[] arrays, which the console's `cgf` can't pass — these
; build [player, npc] for you. Pass the NPC's RefID as hex (click it in the
; console to read its RefID, or type it). The player is always actors[0].
;
; Pair animations (baked-in OSF_Test GLBs — no animation pack needed):
;   cgf "OSFTest.Pair" <npc>            player + npc, scene id "pair"
;   cgf "OSFTest.PairId" <npc> "test.stages"   player + npc, any scene id
;   cgf "OSFTest.PairSway" <npc>        scene id "pair.sway" (loops in lockstep)
;   cgf "OSFTest.Demo" <npc>            scene id "author.scenes.demo" (graph w/ edges)
;   cgf "OSFTest.Tags" <npc>            matchmake on tag "paired"
;
; Each Start* prints the scene HANDLE to the HUD. Use it to navigate:
;   cgf "OSFTest.Advance" <handle>      take the default advance edge
;   cgf "OSFTest.Nav" <handle> "tease"  take a named branch edge
;   cgf "OSFTest.Stage" <stage>         jump a LINEAR scene the player is in
;
; Stop / housekeeping:
;   cgf "OSFTest.Stop"                  stop the scene the player is in
;   cgf "OSFTest.Reload"               rescan Data/OSF/**.osf.json

; NOTE: console `cgf` does NOT apply Papyrus default arguments (those are filled by
; the compiler at the call site, which cgf bypasses). So console entry points must
; take no optional params — that's why Pair hardcodes the id and PairId is separate.

Function Pair(Actor npc) global
    Start(npc, "pair")
EndFunction

Function PairId(Actor npc, string id) global
    Start(npc, id)
EndFunction

Function PairSway(Actor npc) global
    Start(npc, "pair.sway")
EndFunction

Function Demo(Actor npc) global
    Start(npc, "author.scenes.demo")
EndFunction

Function Start(Actor npc, string id) global
    Actor[] a = new Actor[2]
    a[0] = Game.GetPlayer()
    a[1] = npc
    int h = OSF.StartScene(a, id)
    Debug.Notification("OSF: StartScene '" + id + "' -> handle " + h)
EndFunction

Function Tags(Actor npc) global
    Actor[] a = new Actor[2]
    a[0] = Game.GetPlayer()
    a[1] = npc
    string[] tags = new string[1]
    tags[0] = "paired"
    int h = OSF.StartSceneByTags(a, tags)
    Debug.Notification("OSF: StartSceneByTags 'paired' -> handle " + h)
EndFunction

Function Advance(int handle) global
    bool ok = OSF.AdvanceScene(handle)
    Debug.Notification("OSF: Advance " + handle + " -> " + ok)
EndFunction

Function Nav(int handle, string edgeId) global
    bool ok = OSF.NavigateScene(handle, edgeId)
    Debug.Notification("OSF: Navigate " + handle + " '" + edgeId + "' -> " + ok)
EndFunction

Function Stage(int stage) global
    bool ok = OSF.SetSceneStageForActor(Game.GetPlayer(), stage)
    Debug.Notification("OSF: SetStage " + stage + " -> " + ok)
EndFunction

Function Stop() global
    bool ok = OSF.StopSceneForActor(Game.GetPlayer())
    Debug.Notification("OSF: Stop -> " + ok)
EndFunction

Function Reload() global
    int n = OSF.ReloadPacks()
    Debug.Notification("OSF: reloaded, " + n + " scenes registered")
EndFunction
