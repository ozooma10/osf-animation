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
; The member set is frozen as part of the ABI, so keep it in sync with the SceneEvent /
; PackPayload in src/Scene/SceneEventRelay.h. New fields append at the end, so old callbacks
; just ignore them. The names `eventType` and `actorRef` (rather than `event` / `actor`) avoid
; reserved words and type-name clashes. String fields are interned and case-insensitive, so
; compare them with Papyrus `==`.

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
