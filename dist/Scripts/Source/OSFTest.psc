ScriptName OSFTest

; Console wrappers for the OSF API (cgf can't pass Actor[]). NPC/furniture args are hex RefIDs; the player is actors[0].
;
; Raw clip (player only, Data-relative path):
;   cgf "OSFTest.Solo" "meshes\actors\human\animations\common\cower_idle.af"
;   cgf "OSFTest.StopPlay"             stop a solo OSF.Play clip on the player
;
; Shipped smoke-test scenes (vanilla AF clips):
;   cgf "OSFTest.SoloScene"             player-only scene id "solo"
;   cgf "OSFTest.Pair" <npc>            player + npc, scene id "pair"
;   cgf "OSFTest.PairId" <npc> "<scene-id>"   player + npc, any installed scene id
;
; Furniture anchor:
;   cgf "OSFTest.Furniture" <npc> <furn>       player + npc on "pair", anchored AT ref <furn>
;   cgf "OSFTest.FurnitureId" <npc> <furn> <id>  same, any scene id
;   cgf "OSFTest.Furniture2" <npc1> <npc2> <furn>      TWO npcs (no player), on "pair", anchored AT <furn>
;   cgf "OSFTest.Furniture2Id" <npc1> <npc2> <furn> <id>  same, any scene id
;
; Anchor-first matchmaking:
;   cgf "OSFTest.AtAnchor" <npc> <furn>        player + npc; matchmake an anchor-bound scene that fits <furn>
;
; Start* prints the scene handle to the HUD:
;   cgf "OSFTest.Advance" <handle>      take the default advance edge
;   cgf "OSFTest.Nav" <handle> "tease"  take a named branch edge
;   cgf "OSFTest.Stage" <stage>         jump a LINEAR scene the player is in
;
; Self-test (player only, ~5s):
;   cgf "OSF.Health"

; Stop / housekeeping:
;   cgf "OSFTest.Stop"                  stop the scene the player is in
;   cgf "OSFTest.Reload"               rescan Data/OSF/**.osf.json

; cgf doesn't apply Papyrus default arguments, so console entry points take no optional params (hence Pair vs PairId).

; A single clip loops forever.
Function Solo(string file) global
    bool ok = OSF.Play(Game.GetPlayer(), file)
    Debug.Notification("OSF: Play '" + file + "' on player -> " + ok)
EndFunction

; A raw Play has no scene handle, so it needs OSF.Stop rather than OSFTest.Stop.
Function StopPlay() global
    bool ok = OSF.Stop(Game.GetPlayer())
    Debug.Notification("OSF: StopPlay -> " + ok)
EndFunction

; Player-only shipped scene "solo"; holds until OSFTest.Stop.
Function SoloScene() global
    Actor[] a = new Actor[1]
    a[0] = Game.GetPlayer()
    int h = OSF.StartScene(a, "solo")
    Debug.Notification("OSF: StartScene 'solo' -> handle " + h)
EndFunction

Function Pair(Actor npc) global
    Start(npc, "pair")
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

; Player + npc on "pair", anchored at furn via SceneOptions.Anchor (heading -1 = furn's own facing).
; Known: both actors stack at the furniture origin (same cower_idle root), standing rather than seated.
Function Furniture(Actor npc, ObjectReference furn) global
    FurnitureId(npc, furn, "pair")
EndFunction

Function FurnitureId(Actor npc, ObjectReference furn, string id) global
    Actor[] a = new Actor[2]
    a[0] = Game.GetPlayer()
    a[1] = npc
    OSFTypes:SceneOptions opts = new OSFTypes:SceneOptions
    opts.Anchor = furn
    int h = OSF.StartScene(a, id, opts)
    Debug.Notification("OSF: furniture '" + id + "' @ " + furn + " -> handle " + h)
EndFunction

; Two NPCs, no player (watch from a free camera). Stop with OSFTest.Stop2; OSFTest.Stop is player-keyed.
Function Furniture2(Actor npc1, Actor npc2, ObjectReference furn) global
    Furniture2Id(npc1, npc2, furn, "pair")
EndFunction

Function Furniture2Id(Actor npc1, Actor npc2, ObjectReference furn, string id) global
    Actor[] a = new Actor[2]
    a[0] = npc1
    a[1] = npc2
    OSFTypes:SceneOptions opts = new OSFTypes:SceneOptions
    opts.Anchor = furn
    int h = OSF.StartScene(a, id, opts)
    Debug.Notification("OSF: furniture2 '" + id + "' @ " + furn + " -> handle " + h)
EndFunction

; Matchmake an anchor-bound scene that fits furn, for player + npc, with no tag filter.
Function AtAnchor(Actor npc, ObjectReference furn) global
    Actor[] a = new Actor[2]
    a[0] = Game.GetPlayer()
    a[1] = npc
    string[] tags = new string[1]   ; "" element is dropped = no tag filter
    int h = OSF.StartSceneAtAnchor(a, furn, tags)
    Debug.Notification("OSF: AtAnchor @ " + furn + " -> handle " + h)
EndFunction

; Stop an NPC-only furniture scene by one of its participants (OSFTest.Stop is player-keyed).
Function Stop2(Actor npc) global
    bool ok = OSF.StopSceneForActor(npc)
    Debug.Notification("OSF: Stop2 -> " + ok)
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
