#pragma once

#include "skse64/PluginAPI.h"

namespace SwapDropAndHoldRedux
{
	// The only code that calls the holster functions VRIK added in build 80700 (VRIK 0.8.7), and the
	// listener for the holster events VRIK's script sends since then. The rules it follows are in
	// VrikHolsterRules.h, the design in docs/DESIGN-vrik-holster-api.md.

	// Call once VRIK's interface has been asked for, at PostPostLoad. Decides whether the holster API
	// can be used, and logs the answer either way.
	void VrikHolsterApiInit();

	// True when VRIK is present and new enough to be asked about its holsters.
	bool VrikHolsterApiAvailable();

	// The VRIK slot (1-14) in reach of that game hand, or 0 for none - VRIK's own answer.
	int VrikHolsterSlotInReach(bool isLeftGameHand);

	// Whether VRIK says the weapon in that game hand can go into the holster in reach.
	bool VrikCanHolster(bool isLeftGameHand);

	// Asks VRIK to holster the weapon in that game hand. True when VRIK agreed. The request is
	// remembered so VRIK's event for it can be checked.
	bool VrikHolsterWeapon(bool isLeftGameHand, UInt32 weaponFormID);

	// Call when a weapon VRIK agreed to holster has left that game hand.
	void VrikHolsterNoteCompleted(bool isLeftGameHand, UInt32 weaponFormID);

	// Call before a game loads or a new game starts: forgets requests and recorded events from the old game.
	void VrikHolsterOnPreLoadGame();

	// Registers the VRIK_Holstered / VRIK_Unholstered listener on SKSE's own mod event dispatcher.
	void RegisterVrikHolsterEvents(SKSEMessagingInterface* messaging);

	// Call once per physics step, in every mode: logs VRIK's holster events and checks them against
	// this mod's own request.
	void VrikHolsterEventsTick(float deltaTime);
}
