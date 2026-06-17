ScriptName OSFEvent
; Accessors over the Var[] scene-event payload dispatched by OSF (RegisterSceneCallback /
; the OnSceneEvent receiver). The index layout is FROZEN with the ABI — read fields through
; these helpers, never by raw index. New fields append at the end (old callbacks ignore them).
;
;   Function OnSceneEvent(Var[] akEvent)
;       If OSFEvent.Event(akEvent) == OSF.EVENT_NODE_ENTER
;           Actor a = OSFEvent.Actor(akEvent)
;           ...
;       EndIf
;   EndFunction

; Layout (kept in sync with src/Scene/SceneEventRelay.h SceneEvent / PackPayload):
;   0 scene(int) 1 event(int) 2 node(string) 3 edge(string) 4 cue(string)
;   5 actionType(string) 6 actor(Actor) 7 role(string) 8 loopIndex(int)
;   9 time(float) 10 anchor(string) 11 result(int)

Int Function Scene(Var[] akEvent) Global
    If akEvent.Length < 12
        return 0
    EndIf
    return akEvent[0] as Int
EndFunction

Int Function EventType(Var[] akEvent) Global
    If akEvent.Length < 12
        return 0
    EndIf
    return akEvent[1] as Int
EndFunction

String Function Node(Var[] akEvent) Global
    If akEvent.Length < 12
        return ""
    EndIf
    return akEvent[2] as String
EndFunction

String Function Edge(Var[] akEvent) Global
    If akEvent.Length < 12
        return ""
    EndIf
    return akEvent[3] as String
EndFunction

String Function Cue(Var[] akEvent) Global
    If akEvent.Length < 12
        return ""
    EndIf
    return akEvent[4] as String
EndFunction

String Function ActionType(Var[] akEvent) Global
    If akEvent.Length < 12
        return ""
    EndIf
    return akEvent[5] as String
EndFunction

Actor Function GetActor(Var[] akEvent) Global
    If akEvent.Length < 12
        return None
    EndIf
    return akEvent[6] as Actor
EndFunction

String Function Role(Var[] akEvent) Global
    If akEvent.Length < 12
        return ""
    EndIf
    return akEvent[7] as String
EndFunction

Int Function LoopIndex(Var[] akEvent) Global
    If akEvent.Length < 12
        return -1
    EndIf
    return akEvent[8] as Int
EndFunction

Float Function Time(Var[] akEvent) Global
    If akEvent.Length < 12
        return -1.0
    EndIf
    return akEvent[9] as Float
EndFunction

String Function Anchor(Var[] akEvent) Global
    If akEvent.Length < 12
        return ""
    EndIf
    return akEvent[10] as String
EndFunction

Int Function Result(Var[] akEvent) Global
    If akEvent.Length < 12
        return 0
    EndIf
    return akEvent[11] as Int
EndFunction
