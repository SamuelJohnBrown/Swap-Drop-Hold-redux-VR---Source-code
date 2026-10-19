#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	void RegisterTriggerHoldLogger();
	bool ShouldSuppressGrabAutoEquip(bool isLeftVRController, TESObjectREFR* grabbedRefr);

	// Unequips the tracked weapon and spawns its world model at the hand.
	// grabIntoHand=true: legacy trigger-hold behaviour (model handed to HIGGS).
	// grabIntoHand=false: GripHold grip-release drop (hand open; spawned ref id
	// reported via outDroppedRefID for ThrowMomentum / embed pinning).
	bool DropEquippedWeaponFromHand(
		bool isLeftGameHand,
		bool isLeftVRController,
		UInt32 weaponFormID,
		bool grabIntoHand,
		UInt32* outDroppedRefID);
}