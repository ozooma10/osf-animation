Scriptname Fragments:TopicInfos:TIF_OSFCompanionsHug Extends TopicInfo Hidden Const

Function Fragment_End(ObjectReference akSpeakerRef)
    ; The player speaks this top-level choice. Resolve Sarah explicitly.
    Debug.Trace("[OSF Companions] Dialogue hug selected")
    OSFCompanions.HugSarah()
EndFunction
