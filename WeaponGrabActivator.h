#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	void ScheduleGrabbedWeaponActivation(bool isLeftVRController, TESObjectREFR* grabbedRefr);
	void ScheduleSwapPullToOppositeHand(bool isLeftSourceHand, UInt32 weaponFormID, float pullSpeed);
}
