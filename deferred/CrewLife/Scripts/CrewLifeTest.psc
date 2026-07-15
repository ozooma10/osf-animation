ScriptName CrewLifeTest
{ Crew Life - console harness for content QA (dev-only; exclude from release,
  like OSFTest). These start the SCENES directly and BYPASS affinity gating -
  they test the content pack, not the policy. The gated path lives on the
  CrewLifeMoments quest instance and runs via the Companion App in-game.

  cgf passes no default args, so every entry point takes explicit params.
  First: cgf "OSFTest.Reload"   (rescan Data/OSF after editing the JSON)

    cgf "CrewLifeTest.Dance"    <npc>              player + npc, dance together
    cgf "CrewLifeTest.Stargaze" <npc>              player + npc, stargaze
    cgf "CrewLifeTest.Meal"     <npc> <seat1> <seat2>  seated meal at two seats
    cgf "CrewLifeTest.Stop"                        stop the scene the player is in

  Pass refs as hex (click the ref in console to read its RefID). }

Function Dance(Actor npc) global
    StartPair(npc, "crewlife.moment.dance.slow")
EndFunction

Function Stargaze(Actor npc) global
    StartPair(npc, "crewlife.moment.stargaze")
EndFunction

Function StartPair(Actor npc, string tag) global
    Actor[] a = new Actor[2]
    a[0] = Game.GetPlayer()
    a[1] = npc
    string[] tags = new string[1]
    tags[0] = tag
    int h = OSF.StartSceneByTags(a, tags)
    Debug.Notification("CrewLife: '" + tag + "' -> handle " + h)
EndFunction

; Two solo halves anchored at two seats (mirrors OSFTest.FurnitureId's by-id
; anchor path, which reaches the unlisted meal scene). Player's half is locked.
Function Meal(Actor npc, ObjectReference seatPlayer, ObjectReference seatNpc) global
    Actor[] one = new Actor[1]

    one[0] = Game.GetPlayer()
    OSFTypes:SceneOptions popts = new OSFTypes:SceneOptions
    popts.Anchor = seatPlayer
    popts.LockPlayerMode = OSF.ON()
    int hp = OSF.StartScene(one, "crewlife.moment.meal.chair", popts)

    one[0] = npc
    OSFTypes:SceneOptions copts = new OSFTypes:SceneOptions
    copts.Anchor = seatNpc
    int hc = OSF.StartScene(one, "crewlife.moment.meal.chair", copts)

    Debug.Notification("CrewLife: meal -> player " + hp + " / npc " + hc)
EndFunction

Function Stop() global
    bool ok = OSF.StopSceneForActor(Game.GetPlayer())
    Debug.Notification("CrewLife: Stop -> " + ok)
EndFunction
