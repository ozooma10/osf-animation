Scriptname OSFDataSlateScript extends ObjectReference Const
{ OSF Data Slate (the BOOK record). Reading it from the inventory Notes section or in the world opens the in-game scene browser. }

Event OnRead()
    OSF.OpenBrowser()
EndEvent
