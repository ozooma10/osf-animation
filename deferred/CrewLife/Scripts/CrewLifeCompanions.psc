ScriptName CrewLifeCompanions extends Quest
{ Crew Life - the ONLY adapter to the vanilla companion / affinity surface.
  Every other Crew Life script calls THIS one, so if a vanilla form id or an
  ActorValue is wrong, this is the single file that changes.

  Deliberately compiles against BASE types only (Actor, ActorValue, Quest) so it
  builds with just OSF's own sources - no decompiled SQ_CompanionsScript needed.
  Affinity is read straight off the actor's ActorValues, which is exactly what
  vanilla does internally.

  PHASE 0 (xEdit + in-game) fills the property FormIDs below and confirms the
  ActorValues actually read affinity on a romanced save. Until a property is
  filled, GetAffinityLevel() returns -1 ("no data"), which the gating treats as
  "not yet unlocked", so the module degrades safely rather than mis-firing. }

; --- Vanilla ActorValues (fill FormIDs in CK/xEdit, from Starfield.esm) --------
; Research (decompiled companionaffinityscript / com_companionquestscript) says
; affinity lives as these AVs on the companion actor. Confirm the ids in Phase 0.
ActorValue Property COM_AffinityLevel Auto Const
{ 0 Neutral / 1 Friendship / 2 Affection / 3 Commitment. }
ActorValue Property COM_IsRomantic Auto Const
{ >= 1 once romance is active (mirrors vanilla IsCompanionRomantic). }
ActorValue Property COM_IsCommitted Auto Const
{ >= 1 once married / committed. }

; --- Affinity reads -----------------------------------------------------------

; Affinity tier for a companion, or -1 when unknown (AV unfilled / null actor).
; -1 is "no data" and the gating treats it as locked, so an unfilled property
; can never accidentally unlock a romance moment.
int Function GetAffinityLevel(Actor akActor)
    If akActor == None || COM_AffinityLevel == None
        return -1
    EndIf
    return (akActor.GetValue(COM_AffinityLevel)) as int
EndFunction

bool Function IsRomantic(Actor akActor)
    If akActor == None || COM_IsRomantic == None
        return false
    EndIf
    return akActor.GetValue(COM_IsRomantic) >= 1.0
EndFunction

bool Function IsCommitted(Actor akActor)
    If akActor == None || COM_IsCommitted == None
        return false
    EndIf
    return akActor.GetValue(COM_IsCommitted) >= 1.0
EndFunction

; --- Eligibility --------------------------------------------------------------

; Can this actor be cast into a moment right now? (Alive, out of combat, not
; already in an OSF scene.) The caller supplies WHO - active-companion resolution
; is the caller's job in v1 (the App acts on the actor it was opened on; the
; console harness takes the npc as an arg).
bool Function IsEligible(Actor akActor)
    If akActor == None
        return false
    EndIf
    If akActor.IsDead() || akActor.IsInCombat()
        return false
    EndIf
    If OSF.IsPlaying(akActor)
        return false
    EndIf
    return true
EndFunction

; UPGRADE (Phase 0+): precise active-companion + ship-crew roster.
;   Requires the decompiled base sources in the compiler import path, then:
;     SQ_CompanionsScript Property CompanionsQuest Auto Const   ; SQ_Companions
;     SQ_CrewScript       Property CrewQuest       Auto Const   ; SQ_Crew
;   Actor  Function GetActiveCompanion()  return CompanionsQuest.GetActiveActor()
;   Actor[] Function GetCrewOnShip()       filter CrewQuest.GetActiveActors() by
;                                          Is3DLoaded() && shared parent cell.
;   Also expose the ActiveCompanionChanged custom event to CrewLifeMain.
