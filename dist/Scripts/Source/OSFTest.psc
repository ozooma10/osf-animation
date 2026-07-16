ScriptName OSFTest

; Console-callable wrappers for the current handle + SceneOptions OSF API.
; The natives take Actor[] arrays, which the console's `cgf` can't pass — these
; build [player, npc] for you. Pass the NPC's RefID as hex (click it in the
; console to read its RefID, or type it). The player is always actors[0].
;
; Solo (player only) by animation PATH — a single clip that loops forever:
;   cgf "OSFTest.Solo" "OSF\Animations\OSF_Test\TestSway01.glb"   play+loop a clip on the player
;   cgf "OSFTest.StopPlay"             stop a solo OSF.Play clip on the player
;
; Pair animations (baked-in OSF_Test GLBs — no animation pack needed):
;   cgf "OSFTest.Pair" <npc>            player + npc, scene id "pair"
;   cgf "OSFTest.PairId" <npc> "test.stages"   player + npc, any scene id
;   cgf "OSFTest.PairSway" <npc>        scene id "pair.sway" (loops in lockstep)
;   cgf "OSFTest.Demo" <npc>            scene id "author.scenes.demo" (graph w/ edges)
;   cgf "OSFTest.Tags" <npc>            matchmake on tag "paired"
;
; Furniture anchor (the actor-origin-at-furniture smoke test):
;   cgf "OSFTest.Furniture" <npc> <furn>       player + npc on "pair", anchored AT ref <furn>
;   cgf "OSFTest.FurnitureId" <npc> <furn> <id>  same, any scene id
;   cgf "OSFTest.Furniture2" <npc1> <npc2> <furn>      TWO npcs (no player), on "pair", anchored AT <furn>
;   cgf "OSFTest.Furniture2Id" <npc1> <npc2> <furn> <id>  same, any scene id
;     Click each ref in the console to read its RefID, and pass them as hex.
;
; Anchor-FIRST matchmaking (pick a scene BUILT for the furniture, by its anchor keyword/base):
;   cgf "OSFTest.AtAnchor" <npc> <furn>        player + npc; matchmake an anchor-bound scene that fits <furn>
;
; Each Start* prints the scene HANDLE to the HUD. Use it to navigate:
;   cgf "OSFTest.Advance" <handle>      take the default advance edge
;   cgf "OSFTest.Nav" <handle> "tease"  take a named branch edge
;   cgf "OSFTest.Stage" <stage>         jump a LINEAR scene the player is in
;
; Quick health check (player-only, self-contained — no NPC, runs ~5s then cleans up):
;   cgf "OSF.Health"               canonical self-test on the main OSF API (see OSF.psc)

; Stop / housekeeping:
;   cgf "OSFTest.Stop"                  stop the scene the player is in
;   cgf "OSFTest.Reload"               rescan Data/OSF/**.osf.json

; NOTE: console `cgf` does NOT apply Papyrus default arguments (those are filled by
; the compiler at the call site, which cgf bypasses). So console entry points must
; take no optional params — that's why Pair hardcodes the id and PairId is separate.

; Solo: play one clip on the player by PATH. A single clip loops forever (the graph
; wraps its clock), so this is the simplest "solo scene that just loops" test — no JSON,
; no scene id, no NPC. asFile is Data-relative, e.g. "OSF\Animations\OSF_Test\TestSway01.glb".
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

; Solo SCENE (player-only, no NPC) — the minimal base-framework playback check. Plays the
; shipped scene id "solo" on just the player; it holds (loops forever) until OSFTest.Stop.
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
    tags[0] = "pair"
    int h = OSF.StartSceneByTags(a, tags)
    Debug.Notification("OSF: StartSceneByTags 'pair' -> handle " + h)
EndFunction

; Furniture: player + npc on scene id "pair" (two vanilla cower_idle clips), anchored at the
; furniture/bed/marker ref `furn` via SceneOptions.Anchor — so both actors' origins land at the
; furniture's position+heading instead of co-locating at the player. Heading -1 = use furn's own facing.
; NOTE: with the current drop-clip-root engine the two actors STACK at the furniture origin (the same
; cower_idle on both bakes the same root). That's the expected baseline — proving the anchor path. The
; clip-root/placement work is what spreads them onto the furniture. cower_idle is a standing pose, so
; expect two cowering actors AT the furniture, not seated in it (a real furniture clip is authored for it).
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

; Furniture2: same as Furniture but with TWO npcs and no player participant. Lets you watch from a
; free camera (the player isn't in the scene, so there's no control lock; both npcs are still
; stripped + anchored). Stop it with OSFTest.Stop2 (the player-keyed OSFTest.Stop won't reach an
; NPC-only scene), or click an npc and use OSF.StopSceneForActor on it.
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

; Anchor-FIRST: matchmake an anchor-bound scene whose anchor keyword/base matches <furn>, for player+npc,
; anchored at <furn>. No tag filter (empty asTags). Verifies StartSceneAtAnchor + the kRequire pool filter.
Function AtAnchor(Actor npc, ObjectReference furn) global
    Actor[] a = new Actor[2]
    a[0] = Game.GetPlayer()
    a[1] = npc
    string[] tags = new string[1]   ; "" element -> dropped by the native = no tag filter
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
