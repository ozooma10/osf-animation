ScriptName OSFCompat Native
; COMPATIBILITY-ONLY natives for the SAF->OSF shim's primitive (non-Scene) path:
; standalone player control/camera locks. NOT public API.

; Standalone player control lock for the primitive (non-Scene) path.
Function SetPlayerControlLock(bool abLocked) Global Native

; Standalone camera lock for the primitive path: forces 3rd person, bounces back on zoom-in to 1st.
Function SetPlayerCameraLock(bool abLocked) Global Native

; DEBUG/RE-bisect: replaces the standalone player-lock input-disable bitmasks
; (live-reapplies to an active lock, so the bit layout can be bisected in one session). 
; 0/0 = block nothing. Parked on OSFCompat (NOT the public OSF surface)
Function SetSceneControlMask(int aiUserMask, int aiOtherMask) Global Native

; Engine crosshair target
ObjectReference Function GetCrosshairRef() Global Native

; The crosshair target cast to Actor, or None when the crosshair is on nothing or a non-actor reference.
Actor Function GetCrosshairActor() Global Native

; DEBUG (Phase A transport prototype, NOT public API): synthesize one scene event and
; dispatch it through the real relay to every registered receiver.
Function Dbg_FireSceneEvent(int aiScene, int aiEvent, string asNode) Global Native

; DEBUG: no-instance transport probe — DispatchStaticCall asScript.asFn(Var[]) directly,
; no registration. Proves the Var[] marshalling from the console without a scripted form.
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
; handle-based AdvanceScene/NavigateScene can be exercised before the real StartScene mints
; handles. Returns the scene handle (0 = no such def).
int Function Dbg_StartSceneDef(Actor akActor, string asSceneId) Global Native

; DEBUG: echo a message into OSF Animation.log (REX) from Papyrus, so the callback
; round-trip is provable without enabling the Papyrus script log.
Function Dbg_Log(string asMsg) Global Native
