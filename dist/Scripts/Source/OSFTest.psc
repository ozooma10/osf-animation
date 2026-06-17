ScriptName OSFTest

; Console-callable wrappers for the core playback/scene tests (the OSF natives
; take arrays, which cgf can't pass). Usage from the console:
;   cgf "OSFTest.Pair" <refA> <refB>       e.g. cgf "OSFTest.Pair" 14 1A2B3C
;   cgf "OSFTest.PairApart" <refA> <refB>
;   cgf "OSFTest.Defined" "pair" <refA> <refB>          (registry, stage 0)
;   cgf "OSFTest.DefinedStage" "pair" 2 <refA> <refB>   (registry, start stage)
;   cgf "OSFTest.DefinedSolo" "solo" <ref>              (baked-in GLB, no animation pack)
;
; Mechanical per-feature variants (all on the baked-in GLB, NO animation mod):
;   cgf "OSFTest.Stages" <refA> <refB>   timed multi-stage advance + per-stage placement
;   cgf "OSFTest.Loops"  <refA> <refB>   loop-count advance (needs a looping clip)
;
;   cgf "OSFTest.Stage" <refA> 1                        (jump running scene)
;   cgf "OSFTest.PlayTag" "paired" <refA> <refB>        (tag query + gender slots)
;   cgf "OSFTest.Reload"                                (rescan Data/OSF)
;   cgf "OSFTest.StopPair" <refA>

; Baked-in 2-actor scene (face-to-face GLB, NO animation mod required).
Function Pair(Actor a, Actor b) global
    DefinedStage("pair", 0, a, b)
EndFunction

; Placement validation: same pair, but actor B is deliberately placed 1 m to
; the anchor's local +Y and facing the opposite way. Expected: B's skeleton
; renders exactly 1 m from A (rotated by the anchor heading) and turned 180,
; and holds there — a visibly "wrong" pose is the CORRECT result.
Function PairApart(Actor a, Actor b) global
    ; play each in place, anchor B offset ~1 m +Y from A facing opposite, sync.
    OSF.Play(a, "OSF\\Animations\\OSF_Test\\StandSurrender01.glb")
    OSF.Play(b, "OSF\\Animations\\OSF_Test\\StandSurrender01.glb")
    float ax = a.GetPositionX()
    float ay = a.GetPositionY()
    float az = a.GetPositionZ()
    float ah = a.GetAngleZ()                    ; degrees
    OSF.SetAnchor(a, ax, ay, az, ah, 0)         ; pin A where it stands
    OSF.SetAnchor(b, ax, ay + 100.0, az, ah + 180.0, 0)  ; B ~1 m +Y, facing opposite
    Actor[] actors = new Actor[2]
    actors[0] = a
    actors[1] = b
    OSF.Sync(actors)
EndFunction

Function Defined(string id, Actor a, Actor b) global
    DefinedStage(id, 0, a, b)
EndFunction

Function DefinedStage(string id, int stage, Actor a, Actor b) global
    Actor[] actors = new Actor[2]
    actors[0] = a
    actors[1] = b
    OSF.StartScene(actors, id, stage)
EndFunction

Function LoopPair(Actor a, Actor b) global
    DefinedStage("test.loops", 0, a, b)
EndFunction

; --- Mechanical per-feature variants (baked-in GLB, no animation mod) ---

Function Stages(Actor a, Actor b) global
    DefinedStage("test.stages", 0, a, b)
EndFunction

Function Loops(Actor a, Actor b) global
    DefinedStage("test.loops", 0, a, b)
EndFunction

Function DefinedSolo(string id, Actor a) global
    Actor[] actors = new Actor[1]
    actors[0] = a
    OSF.StartScene(actors, id)
EndFunction

Function Stage(Actor a, int stage) global
    OSF.SetSceneStage(a, stage)
EndFunction

Function Reload() global
    int n = OSF.ReloadPacks()
    Debug.Trace("OSFTest: " + n + " animations registered")
EndFunction

Function PlayTag(string tag, Actor a, Actor b) global
    Actor[] actors = new Actor[2]
    actors[0] = a
    actors[1] = b
    string[] tags = new string[1]
    tags[0] = tag
    int h = OSF.StartSceneByTags(actors, tags)
    Debug.Notification("OSF: StartSceneByTags -> handle " + h + " id '" + OSF.GetSceneId(h) + "'")
EndFunction

; --- Primitives (anchor + sync + speed) -------------------------------------
; Play first, then anchor. Anchor pins the rendered skeleton at a WORLD point;
; Unanchor returns it to following the actor. SyncPair frame-locks two
; already-playing graphs. Speed scales playback (0 = freeze).
;   cgf "OSF.Play" <ref> "OSF\Animations\OSF_Test\StandSurrender01.glb"
;   cgf "OSFTest.Anchor" <ref> <x> <y> <z> <hdeg>
;   cgf "OSFTest.Unanchor" <ref>   /   cgf "OSFTest.Speed" <ref> 0.5
Function Anchor(Actor a, float x, float y, float z, float h) global
    bool ok = OSF.SetAnchor(a, x, y, z, h, 0)
    Debug.Trace("OSFTest.Anchor: " + ok)
EndFunction

Function Unanchor(Actor a) global
    OSF.ClearAnchor(a)
EndFunction

Function SyncPair(Actor a, Actor b) global
    Actor[] actors = new Actor[2]
    actors[0] = a
    actors[1] = b
    bool ok = OSF.Sync(actors)
    Debug.Trace("OSFTest.SyncPair: " + ok)
EndFunction

Function Speed(Actor a, float s) global
    OSF.SetSpeed(a, s)
EndFunction

; Solo 2-phase sequence, looping the whole thing: phase x2, phase x2, repeat.
;   cgf "OSFTest.Sequence" <ref>
Function Sequence(Actor a) global
    string[] files = new string[2]
    files[0] = "OSF\\Animations\\OSF_Test\\StandSurrender01.glb"
    files[1] = "OSF\\Animations\\OSF_Test\\StandSurrender01.glb"
    int[] loops = new int[2]
    loops[0] = 2
    loops[1] = 2
    float[] blends = new float[2]
    blends[0] = 0.4
    blends[1] = 0.4
    OSF.PlaySequence(a, files, loops, blends, true)
EndFunction

; --- Readiness handshake ----------------------------------------------------
;   cgf "OSFTest.Ready"                                   (IsReady + HasFeature -> HUD)
;   cgf "OSFTest.StartPair" "pair" <a> <b>                (StartScene)
Function Ready() global
    Debug.Notification("OSF ready: " + OSF.IsReady() + " | scenes: " + OSF.HasFeature("scenes") + " | sync: " + OSF.HasFeature("sync") + " | anchor: " + OSF.HasFeature("anchor"))
EndFunction

Function StartPair(string id, Actor a, Actor b) global
    Actor[] actors = new Actor[2]
    actors[0] = a
    actors[1] = b
    OSF.StartScene(actors, id, 0)
EndFunction

Function StopPair(Actor a) global
    OSF.StopSceneForActor(a)
EndFunction

; --- Scene-event callback transport prototype -------------------------------
; Proves the C++->Papyrus Var[] dispatch (docs/SCENE_DESIGN §1.2). No CK/ESP needed:
; Dbg_FireSceneEventStatic dispatches a synthetic event straight to OnSceneEvent below.
;   cgf "OSFTest.CbTransportTest"   -> fires NODE_ENTER then SCENE_END; watch the log
Function CbTransportTest() global
    Debug.Trace("OSFTest: firing synthetic NODE_ENTER -> OSFTest.OnSceneEvent")
    OSFCompat.Dbg_FireSceneEventStatic("OSFTest", "OnSceneEvent", 123, OSF.EVENT_NODE_ENTER(), "main")
    OSFCompat.Dbg_FireSceneEventStatic("OSFTest", "OnSceneEvent", 123, OSF.EVENT_SCENE_END(), "main")
EndFunction

; Receiver: takes the OSFEvent:SceneEvent struct and reads fields directly. This is the
; shape every real consumer's callback function takes.
Function OnSceneEvent(OSFEvent:SceneEvent akEvent) global
    string msg = "OnSceneEvent: scene=" + akEvent.sceneHandle \
        + " event=" + akEvent.eventType \
        + " node='" + akEvent.node + "'" \
        + " anchor='" + akEvent.anchor + "'"
    OSFCompat.Dbg_Log(msg)              ; -> OSF Animation.log (reliable)
    Debug.Trace("OSFTest." + msg)        ; -> Papyrus log (only if logging enabled)
    Debug.Notification("OSF: " + msg)    ; -> on-screen
EndFunction

; --- Scene-runtime lifecycle prototype (handles + lifecycle events) ----------
; Exercises the Phase-A scene-instance spine: mint a handle, query getters, transition a
; node, stop. Lifecycle events (NODE_ENTER/EXIT/SCENE_END) are logged by the runtime, so
; they're visible with no registered callback (the relay also delivers them to any that are).
;   cgf "OSFTest.SceneTest"   -> watch the OSF Animation.log
Function SceneTest() global
    Actor p = Game.GetPlayer()
    int h = OSFCompat.Dbg_StartScene(p, "author.scenes.demo", "approach")
    OSFCompat.Dbg_Log("SceneTest: started h=" + h + " id='" + OSF.GetSceneId(h) + "' node='" + OSF.GetSceneNode(h) + "'")
    OSFCompat.Dbg_Log("SceneTest: GetSceneForActor(player)=" + OSF.GetSceneForActor(p) + " (expect " + h + ")")
    OSFCompat.Dbg_SetSceneNode(h, "main", 1)
    OSFCompat.Dbg_Log("SceneTest: transitioned -> node='" + OSF.GetSceneNode(h) + "'")
    OSFCompat.Dbg_StopScene(h)
    OSFCompat.Dbg_Log("SceneTest: stopped; id now='" + OSF.GetSceneId(h) + "' (expect empty), GetSceneForActor=" + OSF.GetSceneForActor(p) + " (expect 0)")
EndFunction

; --- .scene.json parser test --------------------------------------------------
; Reloads content (packs + scenes), reports any scene-load problems, and dumps the bundled
; demo scene's parsed graph.
;   cgf "OSFTest.SceneLoadTest"   -> watch the OSF Animation.log
Function SceneLoadTest() global
    OSF.ReloadPacks()
    string[] errors = OSF.GetSceneLoadErrors()
    OSFCompat.Dbg_Log("SceneLoadTest: " + errors.Length + " scene-load problem(s)")
    int i = 0
    While i < errors.Length
        OSFCompat.Dbg_Log("  " + errors[i])
        i += 1
    EndWhile
    OSFCompat.Dbg_DumpScene("author.scenes.demo")
EndFunction

; --- Scene navigation test ----------------------------------------------------
; Starts the demo scene from its def (entry='approach'), exercises Advance/Navigate/edge
; introspection. Lifecycle events are logged by the runtime.
;   cgf "OSFTest.NavTest"   -> watch the OSF Animation.log
Function NavTest() global
    Actor p = Game.GetPlayer()
    int h = OSFCompat.Dbg_StartSceneDef(p, "author.scenes.demo")
    OSFCompat.Dbg_Log("NavTest: started h=" + h + " node='" + OSF.GetSceneNode(h) + "' (expect approach)")
    ; 'approach' has only an auto 'end' edge -> 0 branchable, Advance returns false.
    OSFCompat.Dbg_Log("NavTest: edges@approach=" + OSF.GetSceneEdgeCount(h) + " (expect 0); advance=" + OSF.AdvanceScene(h) + " (expect False)")
    ; Jump to 'main' (debug) to test the branch edges.
    OSFCompat.Dbg_SetSceneNode(h, "main", 0)
    int n = OSF.GetSceneEdgeCount(h)
    OSFCompat.Dbg_Log("NavTest: edges@main=" + n + " (expect 2)")
    int i = 0
    While i < n
        OSFCompat.Dbg_Log("  edge[" + i + "] id='" + OSF.GetSceneEdgeId(h, i) + "' label='" + OSF.GetSceneEdgeLabel(h, i) + "'")
        i += 1
    EndWhile
    bool nav = OSF.NavigateScene(h, "finish")
    OSFCompat.Dbg_Log("NavTest: navigate 'finish'=" + nav + " -> node='" + OSF.GetSceneNode(h) + "' (expect climax)")
    ; 'climax' has only an auto 'loops' edge -> no default advance -> false.
    OSFCompat.Dbg_Log("NavTest: advance@climax=" + OSF.AdvanceScene(h) + " (expect False)")
    OSFCompat.Dbg_StopScene(h)
    OSFCompat.Dbg_Log("NavTest: stopped; node now='" + OSF.GetSceneNode(h) + "' (expect empty)")
EndFunction

; --- Scene PLAYBACK test (the node's anim actually plays) ----------------------
; Point the crosshair at an actor (or none = the player) and run. The actor plays the
; entry node's anim (StandSurrender), then advancing visibly swaps to the next node's anim
; (StandCover), then back, then stop. Needs no animation mod (baked-in test clips).
;   cgf "OSFTest.PbTest"
Function PbTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    int h = OSFCompat.Dbg_StartSceneDef(a, "author.scenes.pbtest")
    OSFCompat.Dbg_Log("PbTest: started h=" + h + " node='" + OSF.GetSceneNode(h) + "' -> StandSurrender (4s)")
    Utility.Wait(4.0)
    OSF.AdvanceScene(h)
    OSFCompat.Dbg_Log("PbTest: advanced -> node='" + OSF.GetSceneNode(h) + "' -> StandCover (4s)")
    Utility.Wait(4.0)
    OSF.AdvanceScene(h)
    OSFCompat.Dbg_Log("PbTest: advanced -> node='" + OSF.GetSceneNode(h) + "' -> StandSurrender (4s)")
    Utility.Wait(4.0)
    OSFCompat.Dbg_StopScene(h)
    OSFCompat.Dbg_Log("PbTest: stopped -> animation ends")
EndFunction

; --- P3 AUTO-advance test (the scene walks its OWN graph; NO manual advance) -----
; Point the crosshair at an actor (or none = the player), run, then DON'T touch it.
; Node 'first' (StandSurrender) holds for a 4s timer, then the runtime auto-takes its
; 'timer' edge to 'second' (StandCover); after another 4s timer it auto-takes a 'timer'
; edge to "$end" and the scene ends — all with zero AdvanceScene/NavigateScene calls.
; Watch OSF Animation.log for the two timer auto-edges and SCENE_END.
;   cgf "OSFTest.AutoTest"
Function AutoTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    int h = OSFCompat.Dbg_StartSceneDef(a, "author.scenes.autotest")
    OSFCompat.Dbg_Log("AutoTest: started h=" + h + " node='" + OSF.GetSceneNode(h) + "' (expect 'first')")
    OSFCompat.Dbg_Log("AutoTest: now WAIT, do NOT advance. ~4s -> 'second' (auto), ~4s more -> scene ends (auto).")
EndFunction

; --- Actor-exclusivity test (one actor -> at most one live scene, SCENE_DESIGN §1.3) ---
; Start a scene on the actor, then try to start a SECOND on the same actor: the second
; start must be REFUSED (handle 0) while the first stays live and GetSceneForActor keeps
; returning it. Uses the hold-based pbtest scene so nothing auto-advances mid-check; the
; test cleans up h1 at the end. No waiting — runs to completion immediately.
;   cgf "OSFTest.ExclusivityTest"
Function ExclusivityTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    int h1 = OSFCompat.Dbg_StartSceneDef(a, "author.scenes.pbtest")
    OSFCompat.Dbg_Log("ExclusivityTest: first start h1=" + h1 + " (expect nonzero)")
    int h2 = OSFCompat.Dbg_StartSceneDef(a, "author.scenes.pbtest")
    OSFCompat.Dbg_Log("ExclusivityTest: re-start same actor h2=" + h2 + " (expect 0 = refused)")
    OSFCompat.Dbg_Log("ExclusivityTest: GetSceneForActor=" + OSF.GetSceneForActor(a) + " (expect " + h1 + ", the live one)")
    OSFCompat.Dbg_StopScene(h1)
    OSFCompat.Dbg_Log("ExclusivityTest: stopped h1; GetSceneForActor=" + OSF.GetSceneForActor(a) + " (expect 0)")
EndFunction

; --- P2: the REAL public StartScene -> handle (routed through the scene runtime) -------
; The public StartScene/StartSceneByTags/StartSceneFiles now return an opaque int HANDLE
; (not bool/string) and route through SceneRuntime — so GetSceneId/GetSceneForActor and the
; handle-based StopScene work on a scene started by the PUBLIC API, and lifecycle events
; (NODE_ENTER on start, SCENE_END on stop) fire with NO Dbg_* native. Uses the baked-in
; "solo" pack (a linear pack auto-exposed as a single-path scene). Crosshair actor or player.
;   cgf "OSFTest.SceneHandleTest"
Function SceneHandleTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "solo", 0)
    OSFCompat.Dbg_Log("SceneHandleTest: StartScene('solo') -> h=" + h + " (expect nonzero)")
    OSFCompat.Dbg_Log("  GetSceneId(h)='" + OSF.GetSceneId(h) + "' (expect 'solo'); GetSceneForActor=" + OSF.GetSceneForActor(a) + " (expect " + h + ")")
    int h2 = OSF.StartScene(actors, "solo", 0)
    OSFCompat.Dbg_Log("  re-StartScene same actor -> h2=" + h2 + " (expect 0 = exclusivity refused)")
    bool stopped = OSF.StopScene(h)
    OSFCompat.Dbg_Log("  StopScene(h)=" + stopped + "; GetSceneForActor=" + OSF.GetSceneForActor(a) + " (expect 0)")
EndFunction
