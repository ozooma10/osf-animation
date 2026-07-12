ScriptName CrewLifeMoments extends Quest
{ Crew Life - player-initiated shared moments (meal / dance / stargaze).

  Gates each moment on the companion's affinity tier (via CrewLifeCompanions),
  then starts the matching scene from Data/OSF/CrewLife/moments.osf.json through
  OSF's public API. The scenes are content; this is the policy that decides when
  the player may play them.

  A moment is launched by the Companion App menu (CrewLifeMain) or, for content
  QA, by CrewLifeTest via the console. Every Try* returns the OSF scene handle
  (0 = gated / ineligible / no matching scene / start failed) and shows the
  player a reason on failure. }

; Filled in CK to point at the same Crew Life quest (self-reference).
CrewLifeCompanions Property Companions Auto Const

; --- Shared moments -----------------------------------------------------------

; Dance together - a paired free scene. Gated to Friendship (tier 1) and up.
int Function TryDance(Actor akCompanion)
    If !GateAffinity(akCompanion, 1)
        return 0
    EndIf
    return StartPair(akCompanion, "crewlife.moment.dance.slow")
EndFunction

; Stargaze side by side. Gated to Friendship (tier 1) and up.
int Function TryStargaze(Actor akCompanion)
    If !GateAffinity(akCompanion, 1)
        return 0
    EndIf
    return StartPair(akCompanion, "crewlife.moment.stargaze")
EndFunction

; Shared meal - two solo scenes, one per adjacent seat. No affinity requirement
; (any crew member will share a meal). The player's half is locked for the
; duration; the companion's half runs free. The caller supplies the two seat
; refs (Phase 2's OSF.FindAnchorsNear discovers them automatically; until then
; they come from the App's crosshair pick / the console harness).
int Function TryMeal(Actor akCompanion, ObjectReference akSeatPlayer, ObjectReference akSeatCompanion)
    If !PreCheck(akCompanion)
        return 0
    EndIf
    If akSeatPlayer == None || akSeatCompanion == None
        Debug.Notification("Crew Life: no seats for a meal here.")
        return 0
    EndIf

    Actor[] one = new Actor[1]

    ; Player's seat - anchor the scene by id at the seat (bypasses matchmaking,
    ; so the unlisted meal scene is reachable), locked while seated.
    one[0] = Game.GetPlayer()
    OSFTypes:SceneOptions popts = new OSFTypes:SceneOptions
    popts.Anchor = akSeatPlayer
    popts.LockPlayerMode = OSF.ON()
    int hp = OSF.StartScene(one, "crewlife.moment.meal.chair", popts)

    ; Companion's seat.
    one[0] = akCompanion
    OSFTypes:SceneOptions copts = new OSFTypes:SceneOptions
    copts.Anchor = akSeatCompanion
    int hc = OSF.StartScene(one, "crewlife.moment.meal.chair", copts)

    If hp == 0 || hc == 0
        Debug.Notification("Crew Life: couldn't seat both of you.")
    EndIf
    return hp
EndFunction

; --- Internals ----------------------------------------------------------------

; Common precondition: a real, eligible companion and a free player.
bool Function PreCheck(Actor akCompanion)
    If Companions == None
        Debug.Notification("Crew Life: not set up (Companions unset).")
        return false
    EndIf
    If !Companions.IsEligible(akCompanion)
        Debug.Notification("Crew Life: they can't right now.")
        return false
    EndIf
    If OSF.IsPlaying(Game.GetPlayer())
        Debug.Notification("Crew Life: finish what you're doing first.")
        return false
    EndIf
    return true
EndFunction

; PreCheck + an affinity floor. Shows a "needs to know you better" line if short.
bool Function GateAffinity(Actor akCompanion, int aiMinLevel)
    If !PreCheck(akCompanion)
        return false
    EndIf
    If Companions.GetAffinityLevel(akCompanion) < aiMinLevel
        Debug.Notification("Crew Life: they'd like to know you better first.")
        return false
    EndIf
    return true
EndFunction

; Start a two-role free scene (player + companion) matched by a single tag.
int Function StartPair(Actor akCompanion, string asTag)
    Actor[] pair = new Actor[2]
    pair[0] = Game.GetPlayer()
    pair[1] = akCompanion
    string[] tags = new string[1]
    tags[0] = asTag
    int h = OSF.StartSceneByTags(pair, tags)
    If h == 0
        Debug.Notification("Crew Life: nothing to play here.")
    EndIf
    return h
EndFunction
