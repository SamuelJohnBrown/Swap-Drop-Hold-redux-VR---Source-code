#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	void ScheduleGrabbedWeaponActivation(bool isLeftVRController, TESObjectREFR* grabbedRefr);
	void ScheduleSwapPullToOppositeHand(bool isLeftSourceHand, UInt32 weaponFormID, float pullSpeed);

	// GripHold: transfer the equip to the opposite hand on grip release, bypassing
	// the swap-pull exclusion list (grip hand-off is not the swap gesture).
	void ScheduleGripHandOff(bool isLeftSourceHand, UInt32 weaponFormID);
}
