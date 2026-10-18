ScriptName OSFQueryHandoffExample extends Quest

int activeHandle = 0
int runGeneration = 0

; Call from your quest with two distinct, available human actors and a world reference.
; This example is opt-in; it has no automatic startup or input bindings.
bool Function RunExample(Actor leadActor, Actor partnerActor, ObjectReference worldAnchor)
    if activeHandle != 0 || leadActor == None || partnerActor == None || leadActor == partnerActor || worldAnchor == None
        return False
    endif
    if !OSF.IsReady()
        return False
    endif
    runGeneration += 1
    int generation = runGeneration
    activeHandle = -1 ; Reserve this operation while the native launch is pending.

    Actor[] cast = new Actor[2]
    cast[0] = leadActor
    cast[1] = partnerActor
    string[] allOf = new string[1]
    allOf[0] = "osf-example-handoff-start"
    string[] emptyTags = new string[0]
    string[] roles = new string[2]
    roles[0] = "lead"
    roles[1] = "partner"
    OSFTypes:SceneOptions options = new OSFTypes:SceneOptions
    options.Anchor = worldAnchor
    options.Speed = 0.5

    int handle = OSF.StartSceneByQuery(cast, allOf, emptyTags, emptyTags, roles, options)
    if runGeneration != generation || activeHandle != -1
        if handle != 0
            OSF.StopScene(handle)
        endif
        return False
    endif
    activeHandle = handle
    if handle == 0
        return False
    endif
    if !OSF.SetSceneAlignment(handle, "partner", 0.0, 0.1, 0.0, 0.0)
        StopOwnedExample(handle, generation)
        return False
    endif
    ; A live nudge is temporary. SaveSceneAlignment(handle) is a separate user choice.
    Utility.Wait(2.0)
    if !OwnsExample(handle, generation)
        return False
    endif
    if OSF.GetSceneId(handle) != "osf.example.handoff.start"
        StopOwnedExample(handle, generation)
        return False
    endif

    ; Constraints follow GetSceneParticipants order. Initially that is leadActor, partnerActor.
    ; Reverse their target roles while retaining the same scene handle and speed.
    allOf[0] = "osf-example-handoff-target"
    roles[0] = "partner"
    roles[1] = "lead"
    if !OSF.ReplaceSceneByQuery(handle, allOf, emptyTags, emptyTags, roles, worldAnchor)
        ; A refused match leaves the original scene playing; this caller chooses to stop it.
        StopOwnedExample(handle, generation)
        return False
    endif
    Debug.Trace("OSF example: " + OSF.GetSceneId(handle) + ", speed " + OSF.GetSceneSpeed(handle))
    Utility.Wait(2.0)
    if !OwnsExample(handle, generation)
        return False
    endif
    StopOwnedExample(handle, generation)
    return True
EndFunction

bool Function OwnsExample(int handle, int generation)
    return activeHandle == handle && runGeneration == generation
EndFunction

Function StopOwnedExample(int handle, int generation)
    if OwnsExample(handle, generation)
        StopExample()
    endif
EndFunction

Function StopExample()
    int handle = activeHandle
    runGeneration += 1
    activeHandle = 0
    if handle != 0 && handle != -1
        OSF.StopScene(handle)
    endif
EndFunction
