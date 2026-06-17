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

; --- Dynamic test clip (visibly moving, 1.2s loop) --------------------------------
; The baked-in surrender/cover poses are STATIC (one pose held 20s), so you can't see
; playback/loop/blend/sync working. "solo.sway" oscillates surrender<->cover on a 1.2s loop
; (real upper-body motion). Use these to eyeball that things actually move.
;   cgf "OSFTest.Sway" <ref>          solo dynamic loop (loop + blend-on-stop visible)
;   cgf "OSFTest.SwayPair" <a> <b>    two actors swaying in LOCKSTEP -> proves Sync visually
;   cgf "OSFTest.Speed" <ref> 0.25    then slow it to watch the loop in detail
Function Sway(Actor a) global
    DefinedSolo("solo.sway", a)
EndFunction

Function SwayPair(Actor a, Actor b) global
    DefinedStage("pair.sway", 0, a, b)
EndFunction

Function Stage(Actor a, int stage) global
    OSF.SetSceneStageForActor(a, stage)
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
; Proves the C++->Papyrus Var[] dispatch. No CK/ESP needed:
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
    string actorStr = "None"
    If akEvent.actorRef
        actorStr = akEvent.actorRef.GetFormID() + ""
    EndIf
    string msg = "OnSceneEvent: scene=" + akEvent.sceneHandle \
        + " event=" + akEvent.eventType \
        + " node='" + akEvent.node + "'" \
        + " anchor='" + akEvent.anchor + "'" \
        + " action='" + akEvent.actionType + "'" \
        + " role='" + akEvent.role + "'" \
        + " actorRef=" + actorStr
    OSFCompat.Dbg_Log(msg)              ; -> OSF Animation.log (reliable)
    Debug.Trace("OSFTest." + msg)        ; -> Papyrus log (only if logging enabled)
    Debug.Notification("OSF: " + msg)    ; -> on-screen
EndFunction

; --- Diagnostics natives: ValidateScene / GetSceneValidationErrors + HasFeature ----------
; Reloads, then validates a known-good scene + a bogus id, dumps per-id validation errors, and
; checks the merged HasFeature capability names. Watch the OSF Animation.log.
;   cgf "OSFTest.ValidateTest"
Function ValidateTest() global
    OSF.ReloadPacks()
    OSFCompat.Dbg_Log("ValidateTest: ValidateScene('author.scenes.demo')=" + OSF.ValidateScene("author.scenes.demo") + " (expect True)")
    OSFCompat.Dbg_Log("ValidateTest: ValidateScene('author.scenes.nope')=" + OSF.ValidateScene("author.scenes.nope") + " (expect False)")
    string[] errs = OSF.GetSceneValidationErrors("author.scenes.demo")
    OSFCompat.Dbg_Log("ValidateTest: demo has " + errs.Length + " validation problem(s)")
    OSFCompat.Dbg_Log("ValidateTest: HasFeature scenes=" + OSF.HasFeature("scenes") + " actions=" + OSF.HasFeature("actions") + " sound=" + OSF.HasFeature("sound") + " camera=" + OSF.HasFeature("camera") + " callbacks=" + OSF.HasFeature("callbacks") + " bogus=" + OSF.HasFeature("bogus"))
EndFunction

; --- Callback actorRef marshalling -------------------------------------------------------
; Fires a synthetic EVENT_ACTION carrying the crosshair actor (or player) through the static
; dispatch to OnSceneEvent, proving the Actor -> actorRef struct-member marshalling. Expect an
; 'OnSceneEvent ... action='test.ping' role='lead' actorRef=<formID>' line (not None).
;   cgf "OSFTest.ActorRefTest"
Function ActorRefTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    OSFCompat.Dbg_Log("ActorRefTest: firing EVENT_ACTION with actor " + a.GetFormID() + " -> expect actorRef=" + a.GetFormID())
    OSFCompat.Dbg_FireActionActor(a, "OSFTest", "OnSceneEvent", "lead")
EndFunction

; --- Scene-runtime lifecycle prototype (handles + lifecycle events) ----------
; Exercises the scene-instance spine: mint a handle, query getters, transition a
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

; --- Auto-advance test (the scene walks its OWN graph; NO manual advance) -----
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

; --- Actor-exclusivity test (one actor -> at most one live scene) ---
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

; --- The REAL public StartScene -> handle (routed through the scene runtime) -------
; The public StartScene/StartSceneByTags/StartSceneFiles now return an opaque int HANDLE
; (not bool/string) and route through the scene runtime — so GetSceneId/GetSceneForActor and the
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

; --- Handle-based linear stage interface (GetSceneStage/SetSceneStage by HANDLE) ---
; A multi-stage pack is a linear scene, so the handle-keyed stage getters/setters work on it.
; Starts the baked-in 3-stage "solo.stages" (no timers — holds each stage), reads/jumps stages
; by handle, checks the out-of-range reject, then stops. Crosshair actor or player. No waiting.
;   cgf "OSFTest.StageHandleTest"
Function StageHandleTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "solo.stages", 0)
    OSFCompat.Dbg_Log("StageHandleTest: started h=" + h + " stage=" + OSF.GetSceneStage(h) + " (expect 0)")
    bool ok = OSF.SetSceneStage(h, 2)
    OSFCompat.Dbg_Log("  SetSceneStage(h,2)=" + ok + " -> stage=" + OSF.GetSceneStage(h) + " (expect True / 2)")
    bool bad = OSF.SetSceneStage(h, 5)
    OSFCompat.Dbg_Log("  SetSceneStage(h,5)=" + bad + " (expect False = out of range)")
    OSF.StopScene(h)
    OSFCompat.Dbg_Log("  stopped; GetSceneStage(h)=" + OSF.GetSceneStage(h) + " (expect -1 = invalid handle)")
EndFunction

; --- StartSceneRoles (bind actors to NAMED roles) -------------------------------
; Starts the 1-role "author.scenes.pbtest" graph binding the actor to role "lead", then exercises
; the validation rejects (unknown role, role/actor count mismatch). Crosshair actor or player.
;   cgf "OSFTest.RolesTest"
Function RolesTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    Actor[] actors = new Actor[1]
    actors[0] = a
    string[] good = new string[1]
    good[0] = "lead"
    int h = OSF.StartSceneRoles(actors, "author.scenes.pbtest", good, 0)
    OSFCompat.Dbg_Log("RolesTest: StartSceneRoles(lead) -> h=" + h + " node='" + OSF.GetSceneNode(h) + "' (expect nonzero / 'surrender')")
    OSF.StopScene(h)
    ; Negative: unknown role name.
    string[] bad = new string[1]
    bad[0] = "bogus"
    int h2 = OSF.StartSceneRoles(actors, "author.scenes.pbtest", bad, 0)
    OSFCompat.Dbg_Log("  StartSceneRoles(bogus) -> h2=" + h2 + " (expect 0 = unknown role)")
    ; Negative: role/actor count mismatch (1 actor, 2 role names).
    string[] two = new string[2]
    two[0] = "lead"
    two[1] = "extra"
    int h3 = OSF.StartSceneRoles(actors, "author.scenes.pbtest", two, 0)
    OSFCompat.Dbg_Log("  StartSceneRoles(2 roles, 1 actor) -> h3=" + h3 + " (expect 0 = count mismatch)")
EndFunction

; --- Cue tracks (EVENT_CUE at enter/exit/end + numeric clip fractions) ----------------
; Starts "author.scenes.cuetest" (one held node with a cue track) and waits ~3s. The cue
; firings are logged by the runtime ("SceneRuntime: scene ... CUE '<id>' ..."), so they're
; visible with NO registered receiver. Crosshair actor or player. Keep the game FOCUSED —
; the numeric cues advance on the animation clock, which stalls when the game pauses.
; Expect: CUE 'began' (enter, on start), 'early' (~1s), 'tick' (~2s, repeat:loop first hit),
; then on stop CUE 'done' (exit). ('looped' fires at the ~20s clip end if left running.)
;   cgf "OSFTest.CueTest"
Function CueTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.cuetest", 0)
    OSFCompat.Dbg_Log("CueTest: started h=" + h + " — expect CUE 'began' now, 'early' ~1s, 'tick' ~2s")
    Utility.Wait(3.0)
    OSFCompat.Dbg_Log("CueTest: stopping — expect CUE 'done' (exit)")
    OSF.StopScene(h)
EndFunction

; --- Cue-driven graph transitions (trigger:<cueId> edge auto-take) ---------------------
; Starts "author.scenes.trigtest" and does NOTHING else — the scene walks itself via cues:
; node 'first' fires cue 'go' at ~2s, whose trigger:go edge transitions to 'second'; 'second'
; fires 'done' at ~2s, whose trigger:done edge ends the scene. Keep the game FOCUSED.
; Expect (no manual advance): CUE 'go' -> cue-trigger -> NODE_ENTER 'second' -> CUE 'done' ->
; cue-trigger $end -> SCENE_END.  Crosshair actor or player.
;   cgf "OSFTest.TrigTest"
Function TrigTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.trigtest", 0)
    OSFCompat.Dbg_Log("TrigTest: started h=" + h + " node='" + OSF.GetSceneNode(h) + "' (expect 'first') — now WAIT, cues drive it: 'go' ~2s -> 'second', 'done' ~2s -> end.")
EndFunction

; --- Action track (osf.control.lock + custom EVENT_ACTION + scene-end cleanup) ----------
; Starts "author.scenes.actiontest" on the PLAYER so osf.control.lock applies, waits 5s, stops.
; On enter: control lock engaged (player frozen) + ACTION 'test.notify'. On stop: ACTION
; 'test.cleanup' (exit) + the lock auto-released by the scene-end ledger (NO authored release).
; Try to move while it runs (should be locked); you should be free after. Logged either way.
;   cgf "OSFTest.ActionTest"
Function ActionTest() global
    Actor a = Game.GetPlayer()
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.actiontest", 0)
    OSFCompat.Dbg_Log("ActionTest: started h=" + h + " — expect osf.control.lock + ACTION 'test.notify'. Try to move (locked).")
    Utility.Wait(5.0)
    OSFCompat.Dbg_Log("ActionTest: stopping — expect ACTION 'test.cleanup' + control lock auto-released. Free to move after.")
    OSF.StopScene(h)
EndFunction

; --- Numeric-timed action (generalized Scene timed-mark) ----------------------
; Starts "author.scenes.timedaction" and WAITS — the scene holds and loops. On enter:
; ACTION 'test.begin'. Then EVERY clip loop: ACTION 'test.tick' (numeric at 0.3) and CUE
; 'ping' (numeric at 0.6) — the action fires BEFORE the cue within a tick (they're on
; different fractions here, so just confirm both recur). On stop: ACTION 'test.done' (exit).
; Keep the game FOCUSED (the clock stalls on focus loss). Crosshair actor or player.
;   cgf "OSFTest.TimedActionTest"
Function TimedActionTest() global
    Actor a = OSFCompat.GetCrosshairActor()
    If !a
        a = Game.GetPlayer()
    EndIf
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.timedaction", 0)
    OSFCompat.Dbg_Log("TimedActionTest: started h=" + h + " — expect ACTION 'test.begin' now, then ~2s in ACTION 'test.tick' immediately followed by CUE 'ping' (same tick, action before cue). WAIT ~6s.")
    Utility.Wait(6.0)
    OSFCompat.Dbg_Log("TimedActionTest: stopping — expect ACTION 'test.done' (exit) + NODE_EXIT + SCENE_END.")
    OSF.StopScene(h)
EndFunction

; --- Fade + generalized undo ledger ------------------------------------------
; Starts "author.scenes.fadetest" on the PLAYER: on enter it locks control THEN fades to
; black (no authored release of either). After ~4s (screen black, you're frozen) it stops,
; and the undo ledger replays in REVERSE: fade-in FIRST, then control unlock. Watch the
; SCREEN fade to black on start and back in on stop, and the log order below.
; Expect on stop: NODE_EXIT (0x2) -> 'fade undo — fading back in' -> 'control lock released
; — player unlocked' -> SCENE_END (0x20).  Keep the game FOCUSED.
;   cgf "OSFTest.FadeTest"
Function FadeTest() global
    Actor a = Game.GetPlayer()
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.fadetest", 0)
    OSFCompat.Dbg_Log("FadeTest: started h=" + h + " — screen should fade to BLACK + you're frozen. Stopping in 4s; expect fade-IN then unlock (reverse-order ledger).")
    Utility.Wait(4.0)
    OSFCompat.Dbg_Log("FadeTest: stopping — expect 'fade undo' BEFORE 'control lock released' (reverse replay), then screen fades back in + free to move.")
    OSF.StopScene(h)
EndFunction

; --- Equipment hide/restore --------------------------------------------------
; Starts "author.scenes.equiptest" on the PLAYER: on enter osf.equipment.hide strips your
; worn apparel (skin excluded) and records it in the undo ledger. NO authored restore — on
; stop (~5s) the ledger re-equips. Watch your CHARACTER undress then redress. Run somewhere
; SAFE (an interior); you're briefly undressed.
; Expect: 'osf.equipment.hide ... hid N item(s)' + 'Actor XX: hid N worn item(s)'; on stop
; NODE_EXIT -> 'equipment undo — restoring 1 actor(s)' -> 'Actor XX: restored N worn item(s)'
; -> SCENE_END.
;   cgf "OSFTest.EquipTest"
Function EquipTest() global
    Actor a = Game.GetPlayer()
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.equiptest", 0)
    OSFCompat.Dbg_Log("EquipTest: started h=" + h + " — your apparel should be HIDDEN now. Stopping in 5s; expect it RESTORED (ledger, no authored restore).")
    Utility.Wait(5.0)
    OSFCompat.Dbg_Log("EquipTest: stopping — expect 'equipment undo — restoring' + your apparel re-equipped.")
    OSF.StopScene(h)
EndFunction

; --- Sound lane + osf.voice.play ---------------------------------------------
; Starts "author.scenes.soundtest" on the PLAYER. You should HEAR a short beep on start
; (osf.voice.play + the sound-lane enter entry) and again ~2s in (the numeric 0.1 sound,
; every loop). Confirms the harvested SoundService + the sound lane on the timed-mark path.
; Expect log: 'audio engine ready' (once, at load), then on start 'osf.voice.play' +
; 'sound 'OSF/Sounds/testbeep.wav'' lines; another 'sound ...' ~2s in. Keep the game FOCUSED.
;   cgf "OSFTest.SoundTest"
Function SoundTest() global
    Actor a = Game.GetPlayer()
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.soundtest", 0)
    OSFCompat.Dbg_Log("SoundTest: started h=" + h + " — expect a BEEP now (voice.play + sound-lane enter) and another ~2s in (numeric 0.1). WAIT ~5s.")
    Utility.Wait(5.0)
    OSFCompat.Dbg_Log("SoundTest: stopping.")
    OSF.StopScene(h)
EndFunction

; --- Camera lane (minimal-safe third-person hold) -----------------------------
; Be in FIRST person, then start "author.scenes.cameratest" on the PLAYER: the camera lane
; forces + holds THIRD person (and bounces you back if you scroll into first). NO authored
; release — on stop (~4s) the kCamera undo-ledger entry restores you to first person.
; Expect: 'camera 'thirdperson_hold' — third-person hold engaged' + 'Player camera forced to
; third person'; on stop 'camera undo — releasing the camera hold' + 'restored to first person'.
;   cgf "OSFTest.CameraTest"
Function CameraTest() global
    Actor a = Game.GetPlayer()
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.cameratest", 0)
    OSFCompat.Dbg_Log("CameraTest: started h=" + h + " — should snap to THIRD person + hold. Stopping in 4s; expect restore to first person.")
    Utility.Wait(4.0)
    OSFCompat.Dbg_Log("CameraTest: stopping — expect 'camera undo' + restore to first person.")
    OSF.StopScene(h)
EndFunction

; --- Weapon sheathe/restore (osf.weapon.*) -----------------------------------
; DRAW your weapon first, then run on the PLAYER: on enter osf.weapon.sheathe holsters it and
; records it in the undo ledger. NO authored restore — on stop (~5s) the ledger re-draws (the
; symmetric pair, like EquipTest). Watch your weapon holster then draw.
; Expect: 'osf.weapon.sheathe (role 'lead')' + 'Actor XX: weapon sheathed'; on stop NODE_EXIT ->
; 'weapon undo — re-drawing 1 actor(s)' -> 'Actor XX: weapon drawn' -> SCENE_END.
; If the log shows 'unavailable on this build' the Actor vtable binding didn't resolve (see RE.md).
;   cgf "OSFTest.WeaponTest"
Function WeaponTest() global
    Actor a = Game.GetPlayer()
    Actor[] actors = new Actor[1]
    actors[0] = a
    int h = OSF.StartScene(actors, "author.scenes.weapontest", 0)
    OSFCompat.Dbg_Log("WeaponTest: started h=" + h + " — your weapon should HOLSTER now. Stopping in 5s; expect it RE-DRAWN (ledger, no authored restore).")
    Utility.Wait(5.0)
    OSFCompat.Dbg_Log("WeaponTest: stopping — expect 'weapon undo — re-drawing' + weapon drawn again.")
    OSF.StopScene(h)
EndFunction

; --- StartSceneAt (scene world-anchored at an ObjectReference) ----------------
; Point the crosshair at a piece of FURNITURE / a marker / any nearby ref, then run on the
; PLAYER: the scene anchors at that ref (position + its heading) instead of at the player's feet,
; so the player co-locates AT the ref. Contrast with SceneHandleTest (anchors at the actor).
; Expect the log to report 'anchored at (x,y,z)' = the ref's position, and the player to snap
; there and hold the 'solo' pose. Stops after ~6s.
;   cgf "OSFTest.StartAtTest"
Function StartAtTest() global
    ObjectReference anchor = OSFCompat.GetCrosshairRef()
    If !anchor
        OSFCompat.Dbg_Log("StartAtTest: no crosshair ref — point at furniture / a marker and retry.")
        Return
    EndIf
    Actor[] actors = new Actor[1]
    actors[0] = Game.GetPlayer()
    int h = OSF.StartSceneAt(actors, "solo", anchor)
    OSFCompat.Dbg_Log("StartAtTest: StartSceneAt('solo') at ref " + anchor.GetFormID() + " -> h=" + h + " (expect the player AT the ref, not where they stood). Stopping in 6s.")
    Utility.Wait(6.0)
    OSF.StopScene(h)
    OSFCompat.Dbg_Log("StartAtTest: stopped.")
EndFunction

; --- Scene-metadata introspection (read-only; no scene started) ---------------
; Dumps a scene's role/gender/tag/actor-count WITHOUT starting it. Checks a known-good scene
; (author.scenes.weapontest: 1 role 'lead' any, tags test+solo) and an unknown id (-> empty/0).
;   cgf "OSFTest.IntrospectTest"   (or:  cgf "OSFTest.IntrospectScene" "author.scenes.demo")
Function IntrospectTest() global
    IntrospectScene("author.scenes.weapontest")
    IntrospectScene("author.scenes.nope")
EndFunction

Function IntrospectScene(string id) global
    string[] roles = OSF.GetSceneRoles(id)
    string[] tags = OSF.GetSceneTags(id)
    int n = OSF.GetSceneActorCount(id)
    OSFCompat.Dbg_Log("IntrospectScene '" + id + "': actorCount=" + n + " roles=" + roles.Length + " tags=" + tags.Length)
    int i = 0
    While i < roles.Length
        OSFCompat.Dbg_Log("  role[" + i + "]='" + roles[i] + "' gender='" + OSF.GetSceneRoleGender(id, roles[i]) + "'")
        i += 1
    EndWhile
    i = 0
    While i < tags.Length
        OSFCompat.Dbg_Log("  tag[" + i + "]='" + tags[i] + "'")
        i += 1
    EndWhile
EndFunction
