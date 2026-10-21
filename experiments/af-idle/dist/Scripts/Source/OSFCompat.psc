ScriptName OSFCompat Native
; Non-public natives for the OSFTest harness: crosshair pickers + debug/test probes.
; NOT public API — consumer mods use the OSF script.

; Engine crosshair target
ObjectReference Function GetCrosshairRef() Global Native

; The crosshair target cast to Actor, or None when the crosshair is on nothing or a non-actor reference.
Actor Function GetCrosshairActor() Global Native

; DEBUG: no-instance transport probe — DispatchStaticCall asScript.asFn(OSFEvent:SceneEvent)
; directly, no registration. Proves the struct marshalling from the console without a scripted form.
Function Dbg_FireSceneEventStatic(string asScript, string asFn, int aiScene, int aiEvent, string asNode) Global Native

; DEBUG: fire a synthetic EVENT_ACTION carrying a REAL actor through the static dispatch — proves
; the actorRef object marshalling (Actor -> struct member) without a scripted-form instance.
Function Dbg_FireActionActor(Actor akActor, string asScript, string asFn, string asRole) Global Native

; DEBUG directly (mint a handle + fire NODE_ENTER; transition fires NODE_EXIT+NODE_ENTER; 
; stop fires NODE_EXIT+SCENE_END and invalidates the handle).
int Function Dbg_StartScene(Actor akActor, string asId, string asNode) Global Native
bool Function Dbg_SetSceneNode(int aiScene, string asNode, int aiStage) Global Native
bool Function Dbg_StopScene(int aiScene) Global Native

; DEBUG: log a parsed *.scene.json graph (nodes/edges) to the OSF Animation.log.
Function Dbg_DumpScene(string asId) Global Native

; DEBUG: start a scene from its *.scene.json def (enter at the def's entry node), so the
; handle-based AdvanceScene/NavigateScene can be exercised directly. Returns the scene
; handle (0 = no such def).
int Function Dbg_StartSceneDef(Actor akActor, string asSceneId) Global Native

; DEBUG: echo a message into OSF Animation.log (REX) from Papyrus, so the callback
; round-trip is provable without enabling the Papyrus script log.
Function Dbg_Log(string asMsg) Global Native

; DEBUG: play a Data-relative loose file through SoundService at the player's position — the
; in-world audible test for the Wwise external-source path (same routing as scene sound cues).
;   cgf "OSFCompat.Dbg_PlaySound" "OSF\Sounds\testbeep.wav"
Function Dbg_PlaySound(string asDataRelPath) Global Native
