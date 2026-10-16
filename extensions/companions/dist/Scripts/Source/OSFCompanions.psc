Scriptname OSFCompanions Hidden

; Console: cgf "OSFCompanions.HugSarah"
bool Function HugSarah() Global
    OSFCompanionsManager manager = Game.GetFormFromFile(0x800, "OSFCompanions.esm") as OSFCompanionsManager
    if !manager
        Debug.Notification("OSF Companions: enable OSFCompanions.esm.")
        return false
    endif
    return manager.RequestHug()
EndFunction

bool Function Stop() Global
    OSFCompanionsManager manager = Game.GetFormFromFile(0x800, "OSFCompanions.esm") as OSFCompanionsManager
    if !manager
        return false
    endif
    manager.CancelHug()
    return true
EndFunction

Function Status() Global
    OSFCompanionsManager manager = Game.GetFormFromFile(0x800, "OSFCompanions.esm") as OSFCompanionsManager
    if manager
        manager.PrintStatus()
    endif
EndFunction
