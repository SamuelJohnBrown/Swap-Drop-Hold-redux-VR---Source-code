#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	void RegisterTriggerHoldLogger();
	bool ShouldSuppressGrabAutoEquip(bool isLeftVRController, TESObjectREFR* grabbedRefr);
}