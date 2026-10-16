ScriptName OSFTypes

; Optional Start* modifiers; pass None for defaults.
Struct SceneOptions
    ObjectReference Anchor    ; world-anchor at this ref (furniture/bed/marker) instead of akActors[0]
    float HeadingDeg = -1.0   ; < 0 = Anchor's own heading
    int Stage = 0             ; enter on this linear stage (start stage for OSFAdvanced.StartSceneStages); 0 = scene entry
    float Speed = 1.0         ; all starts; retained across node changes, 0 pauses
    float BlendIn = 0.4       ; OSFAdvanced dynamic file/stage starts

    ; Tri-states: use OSF.INHERIT()/OFF()/ON(); 0 forces off. Default -1 inherits the scene's value.
    int StripMode = -1        ; scene's strip policy
    int LockPlayerMode = -1   ; player movement/combat lock
    int PlayerControlMode = -1 ; director input (advance/navigate/end/freecam); OFF = player can't advance or end
    int FadeMode = -1          ; start fade-to-black
    int InPlaceMode = -1       ; ON = play where the actors stand (no teleport or root/heading pin; vanilla camera untouched)
    string Camera = ""        ; "" = inherit; "thirdperson_hold" / "scene_orbit" / "vanity_orbit" / "freefly" / "none". Suppresses authored node cameras.
    float LoopScale = 1.0     ; multiplies the loop count of loop-driven stages (loops > 0)
EndStruct

; RegisterSceneCallback payload, built natively via CreateStruct("OSFTypes#SceneEvent").
; Handler: Function OnSceneEvent(OSFTypes:SceneEvent akEvent)
Struct SceneEvent
    int sceneHandle     ; named to avoid the vanilla Scene type
    int eventType       ; one OSF.EVENT_*() value
    string node
    string edge
    string cue          ; EVENT_CUE id
    string actionType   ; EVENT_ACTION type
    Actor actorRef      ; participant (may be None)
    string role
    int loopIndex       ; reserved, always -1
    float time          ; clip-local fraction, or -1.0 for a named anchor
    string anchor       ; "", "enter", "exit", "end"
    int result          ; OSF.RESULT_*()
    int cause = -1      ; SCENE_END only: CAUSE_COMPLETED/CANCELLED/INTERRUPTED
    string sceneId      ; definition id at dispatch; empty for ad-hoc scenes
EndStruct

