#pragma once
#include "skse64/PluginAPI.h"

namespace SwapDropAndHoldRedux
{
	void InitBitingAxesCompatibility(const PluginHandle& pluginHandle, SKSEMessagingInterface* messagingInterface);
	void RegisterWeaponGrabLogger();
}
