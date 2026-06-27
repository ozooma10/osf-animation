ScriptName OSFTypes

; --- Scene options ------------------------------------------------------------
; Optional modifiers shared by the Start* functions. Pass None (the default) for the common case; 
Struct SceneOptions
    ObjectReference Anchor    ; world-anchor the scene at this ref (furniture/bed/marker) instead of co-locating at akActors[0].
    float HeadingDeg = -1.0   ; anchor heading in DEGREES; < 0 = use Anchor's own heading
    int Stage = 0             ; start stage for OSFAdvanced.StartSceneStages; for registry graph scenes use SetSceneStage* after start
    float Speed = 1.0         ; OSFAdvanced dynamic file/stage starts
    float BlendIn = 0.4       ; OSFAdvanced dynamic file/stage starts

    ; --- Per-start overrides -------------------------------------------------
    ; Tri-state ints; write them with the OSF.INHERIT()/OFF()/ON() helpers, NOT bare 0/1
    ; (0 means force-OFF, NOT "leave default"). Default -1 = inherit the scene's pack value.
    int StripMode = -1        ; override the scene's strip-actors policy: INHERIT/OFF/ON
    int LockPlayerMode = -1   ; override player-input lock while the scene plays: INHERIT/OFF/ON
    int FadeMode = -1          ; override the start fade-to-black curtain: INHERIT/OFF/ON
    float LoopScale = 1.0     ; multiply every loop-driven stage's loop count (1.0 = unchanged). Affects only stages with a loop count (loops > 0);
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

