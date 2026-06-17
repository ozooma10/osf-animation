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
    string id = OSF.StartSceneByTags(actors, tags)
    Debug.Notification("OSF: StartSceneByTags -> '" + id + "'")
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
    OSF.StopScene(a)
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

; Receiver: decodes the Var[] payload through OSFEvent and logs it. This is the shape
; every real consumer's callback function takes.
Function OnSceneEvent(Var[] akEvent) global
    string msg = "OnSceneEvent: scene=" + OSFEvent.Scene(akEvent) \
        + " event=" + OSFEvent.EventType(akEvent) \
        + " node='" + OSFEvent.Node(akEvent) + "'" \
        + " anchor='" + OSFEvent.Anchor(akEvent) + "'"
    OSFCompat.Dbg_Log(msg)              ; -> OSF Animation.log (reliable)
    Debug.Trace("OSFTest." + msg)        ; -> Papyrus log (only if logging enabled)
    Debug.Notification("OSF: " + msg)    ; -> on-screen
EndFunction
