ScriptName OSFTest

; Console-callable wrappers for the current handle + SceneOptions OSF API.
; The natives take Actor[] arrays, which the console's `cgf` can't pass — these
; build [player, npc] for you. Pass the NPC's RefID as hex (click it in the
; console to read its RefID, or type it). The player is always actors[0].
;
; Solo (player only) by animation PATH — a single clip that loops forever:
;   cgf "OSFTest.Solo" "OSF\Animations\<YourPack>\<clip>.glb"   play+loop a clip on the player
;   cgf "OSFTest.StopPlay"             stop a solo OSF.Play clip on the player
;
; Pair a scene you authored (player + npc) by its scene id:
;   cgf "OSFTest.PairId" <npc> "<your.scene.id>"   player + npc, any scene id
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

; Solo: play one clip on the player by PATH. A single clip loops forever (the graph
; wraps its clock), so this is the simplest "solo scene that just loops" test — no JSON,
; no scene id, no NPC. asFile is Data-relative, e.g. "OSF\Animations\<YourPack>\<clip>.glb".
Function Solo(string file) global
    bool ok = OSF.Play(Game.GetPlayer(), file)
    Debug.Notification("OSF: Play '" + file + "' on player -> " + ok)
EndFunction

; Stop a solo OSF.Play clip. (OSFTest.Stop is for SCENES; a raw Play has no handle, so it
; needs OSF.Stop, not StopSceneForActor.)
Function StopPlay() global
    bool ok = OSF.Stop(Game.GetPlayer())
    Debug.Notification("OSF: StopPlay -> " + ok)
EndFunction

Function PairId(Actor npc, string id) global
    Start(npc, id)
EndFunction

Function Start(Actor npc, string id) global
    Actor[] a = new Actor[2]
    a[0] = Game.GetPlayer()
    a[1] = npc
    int h = OSF.StartScene(a, id)
    Debug.Notification("OSF: StartScene '" + id + "' -> handle " + h)
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
