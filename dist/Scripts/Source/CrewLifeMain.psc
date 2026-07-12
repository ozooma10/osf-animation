ScriptName CrewLifeMain extends Quest
{ Crew Life - bootstrap + config.

  Attach to a start-game-enabled quest (flags StartGameEnabled + StartsEnabled,
  like OSFDataSlateQuest). Holds the module's config as auto-property defaults so
  it works out of the box; each value has a commented upgrade to a framework
  OSF.GetConfig* native (reads Data/OSF/CrewLife/crewlife.json through the same
  JSON loader Settings.cpp uses) once those land - keeping JSON config the plan's
  way without a second SFSE plugin.

  Sibling scripts (Companions, Moments) attach to the SAME quest and are wired by
  property in CK. The routine director (Phase 2) is a separate script on this
  quest; its heartbeat hook is stubbed here. }

; --- Wiring (fill in CK to point at this same quest) --------------------------
CrewLifeCompanions Property Companions Auto Const
CrewLifeMoments    Property Moments    Auto Const

; --- Config (defaults; upgrade to OSF.GetConfig* in Phase 2) ------------------
float Property RoutineIntervalSec = 30.0 Auto
{ Director heartbeat. UPGRADE: OSF.GetConfigFloat("CrewLife","routine.intervalSec",30.0) }
float Property MomentCooldownSec = 300.0 Auto
{ Per-actor re-cast cooldown. UPGRADE: OSF.GetConfigFloat("CrewLife","moment.cooldownSec",300.0) }
int Property MaxConcurrentRoutines = 3 Auto
{ Live ambient routines at once. UPGRADE: OSF.GetConfigInt(...) }
bool Property RoutinesEnabled = true Auto
{ Master switch for the Phase 2 routine director. }

; --- Lifecycle ----------------------------------------------------------------

Event OnQuestInit()
    Bootstrap()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
EndEvent

; Remote-event registrations don't survive save/load - re-arm each load.
Event Actor.OnPlayerLoadGame(Actor akSender)
    Bootstrap()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
EndEvent

Function Bootstrap()
    If !OSF.IsReady()
        ; OSF not up yet; nothing to arm. It re-arms on the next load anyway.
        return
    EndIf
    Debug.Trace("[CrewLife] ready (OSF " + OSF.GetVersion() + ")")
    ; UPGRADE (Phase 2): reload config from crewlife.json here, then StartTimer.
    ; If RoutinesEnabled
    ;     StartTimer(RoutineIntervalSec)
    ; EndIf
EndFunction

; --- Routine director heartbeat (Phase 2 stub) --------------------------------
; The routine engine (CrewLifeDirector) rides this timer: pick one idle crew
; member, choose a slot by GameHour, discover a seat with OSF.FindAnchorsNear,
; and StartSceneAtAnchor a "crewlife.routine.*" scene. Left unarmed until the
; FindAnchorsNear native + routines.osf.json exist.
Event OnTimer(int aiTimerID)
    ; (Phase 2) director tick, then re-arm:
    ; StartTimer(RoutineIntervalSec)
EndEvent
