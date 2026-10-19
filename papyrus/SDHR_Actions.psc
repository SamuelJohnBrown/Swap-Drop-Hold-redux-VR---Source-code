Scriptname SDHR_Actions Hidden
{Swap Drop & Hold redux -- the menu's action buttons.

 ATTACHED TO NOTHING, ON PURPOSE. A subclass on the MCM quest emptied every page of a menu
 once already on a sister mod; a script of plain global functions cannot, because nothing
 loads it until a button is actually pressed. If this file is missing the buttons simply do
 nothing and the rest of the menu is untouched.

 The mod event is the bridge to the plugin: SKSE delivers it to the mod callback event
 dispatcher, which the DLL listens on. See Exclusions.cpp.}

; 1 = exclude what is in the RIGHT hand
; 2 = exclude what is in the LEFT hand
; 3 = un-exclude what is in the RIGHT hand
; 4 = un-exclude what is in the LEFT hand
; 5 = clear the whole excluded list
Function Act(int action) global
    ; The player, purely as a sender -- SendModEvent is a Form member, and the event carries
    ; the action in its numeric argument. 0x14 is the player and needs no plugin to look up.
    Form sender = Game.GetFormEx(0x14)
    If sender
        sender.SendModEvent("SDHR_MenuAction", "", action as float)
    EndIf
EndFunction
