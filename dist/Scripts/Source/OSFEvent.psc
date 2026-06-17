ScriptName OSFEvent
; The scene-event payload struct, delivered to a RegisterSceneCallback receiver:
;
;   Function OnSceneEvent(OSFEvent:SceneEvent akEvent)
;       If akEvent.eventType == OSF.EVENT_NODE_ENTER()
;           Actor a = akEvent.actorRef
;           ...
;       EndIf
;   EndFunction
;
; The member set is FROZEN with the ABI 
; (must be kept in sync with src/Scene/SceneEventRelay.h SceneEvent / PackPayload). 
; New fields append at the end (old callbacks ignore them).
; `eventType` (not `event`) and `actorRef` (not `actor`) those are reserved words / type-name clashes. 
; String fields are BSFixedString-interned -> case-insensitives; compare
; with Papyrus `==`.

Struct SceneEvent
    int sceneHandle     ; scene instance handle (named to dodge the vanilla `Scene` type)
    int eventType       ; one OSF.EVENT_*() value
    string node         ; node id
    string edge         ; edge id
    string cue          ; EVENT_CUE id
    string actionType   ; EVENT_ACTION / EVENT_ACTION_FAILED type
    Actor actorRef      ; participant (may be None)
    string role         ; role name
    int loopIndex
    float time          ; clip-local fraction, or -1.0 for a named anchor
    string anchor       ; "", "enter", "exit", "end"
    int result          ; OSF.RESULT_*()
EndStruct
