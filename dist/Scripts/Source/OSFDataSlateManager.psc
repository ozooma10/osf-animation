Scriptname OSFDataSlateManager extends Quest
{ OSF Data Slate - discoverability item for the scene browser.

  Grants the player the "OSF Data Slate" note (a BOOK, so it files under the
  inventory Notes section with the other slates) once. Reading the slate opens
  the scene browser - that part lives on the book itself (OSFDataSlateScript).

  Attach to a start-game-enabled quest (see Plugin\Quests). }

Book Property OSFDataSlate Auto Const
{ This plugin's Data Slate note (BOOK). }

; start-game-enabled => fires once for the life of the game. An existing save
; that adds this mod also gets the grant here.
Event OnQuestInit()
    GiveSlate()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
EndEvent

; Remote-event registrations do not survive a save/load, so re-arm each load.
; Also covers a save that predates the grant (GetItemCount guards a re-give).
Event Actor.OnPlayerLoadGame(Actor akSender)
    GiveSlate()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
EndEvent

Function GiveSlate()
    Actor player = Game.GetPlayer()
    If player.GetItemCount(OSFDataSlate) == 0
        player.AddItem(OSFDataSlate, 1, true)
        Debug.Notification("OSF Data Slate added - read it (inventory > Notes) to open the scene browser.")
    EndIf
EndFunction
