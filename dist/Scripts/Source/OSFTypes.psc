ScriptName OSFTypes

; --- Scene options ------------------------------------------------------------
; Optional modifiers shared by the Start* functions. Pass None (the default) for the common case; 
Struct SceneOptions
    ObjectReference Anchor    ; world-anchor the scene at this ref (furniture/bed/marker) instead of co-locating at akActors[0]. Ignored by StartSceneFiles.
    float HeadingDeg = -1.0   ; anchor heading in DEGREES; < 0 = use Anchor's own heading
    int Stage = 0             ; StartScene by-id / pack: start stage (ignored by def-backed graphs)
    float Speed = 1.0         ; StartSceneFiles: playback speed
    float BlendIn = 0.4       ; StartSceneFiles: blend-in seconds
EndStruct

; The scene-event payload struct, delivered to a RegisterSceneCallback receiver. The native relay
; marshals into this type via CreateStruct("OSFTypes#SceneEvent"):
;
;   Function OnSceneEvent(OSFTypes:SceneEvent akEvent)
;       If akEvent.eventType == OSF.EVENT_NODE_ENTER()
;           Actor a = akEvent.actorRef
;           ...
;       EndIf
;   EndFunction
;
Struct SceneEvent
    int sceneHandle     ; scene instance handle (named to dodge the vanilla `Scene` type)
    int eventType       ; one OSF.EVENT_*() value
    string node         ; node id
    string edge         ; edge id
    string cue          ; EVENT_CUE id
    string actionType   ; EVENT_ACTION type
    Actor actorRef      ; participant (may be None)
    string role         ; role name
    int loopIndex       ; reserved in v0.1; currently -1
    float time          ; clip-local fraction, or -1.0 for a named anchor
    string anchor       ; "", "enter", "exit", "end"
    int result          ; OSF.RESULT_*()
EndStruct

