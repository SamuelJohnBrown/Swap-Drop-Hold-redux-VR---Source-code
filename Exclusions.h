#pragma once

#include "skse64/GameTypes.h"

struct SKSEMessagingInterface;

namespace SwapDropAndHoldRedux
{
	// Items the player has taken out of grip-hold from the menu.
	//
	// An excluded weapon behaves exactly as it did before this mod: it is never
	// grab-equipped, never grip-bound, and never dropped when the hand opens. That
	// is not new behaviour - IsExcludedWeaponForm has always done it for the ini's
	// ExcludedForms list. All this adds is doing it at runtime, from the menu, for
	// whatever happens to be in the player's hand.
	//
	// Stored as Plugin.esp:0xBASEID rather than a runtime form id, because a runtime
	// id is a load-order artefact and would point at a different item the moment the
	// player installs anything.

	// Listens for the menu's buttons. Safe at kInputLoaded.
	//
	// Takes the messaging interface because SKSE's mod event dispatcher can only be
	// reached through it. See the comment on the implementation - sinking to the SDK
	// global instead is a silent no-op, and was one for two releases.
	void RegisterExclusions(SKSEMessagingInterface* messaging);

	// Reads the saved list and resolves it against the load order. Needs the game
	// data, so kDataLoaded or later.
	void ExclusionsLoad();

	// Remembers what is in each hand, so the menu's buttons still have something to
	// act on after you have put the weapon down to open the menu at all.
	void ExclusionsNoteHeld();

	// Toggles what is in ONE game hand - or the last thing that hand held - on and off
	// the blacklist, and says which on the HUD. The other hand is never consulted: the
	// Grip Hold chord that calls this was made with this hand.
	//
	// The MCM buttons still exist and still work; they are just no longer the only way
	// in, because the menu route asks the player to put the weapon down (grip closes
	// menus) and then trust that the mod remembered it. Doing it with the thing in your
	// hand needs no such trust.
	//
	// Returns false if that hand had nothing to act on.
	bool ExclusionsToggleHand(bool isLeftGameHand);

	bool ExclusionsContains(UInt32 formID);
	UInt32 ExclusionsCount();
}
