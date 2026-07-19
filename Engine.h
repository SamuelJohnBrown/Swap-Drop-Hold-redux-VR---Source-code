#pragma once

#include "Helper.h"

namespace SwapDropAndHoldRedux
{
	extern SKSETrampolineInterface* g_trampolineInterface;
	extern SKSETaskInterface* g_task;
	extern HiggsPluginAPI::IHiggsInterface001* higgsInterface;
	extern spellwheelPluginApi::ISpellWheelInterface001* spellwheelInterface;
	extern vrikPluginApi::IVrikInterface001* vrikInterface;
	extern SkyrimVRESLPluginAPI::ISkyrimVRESLInterface001* skyrimVRESLInterface;

	void StartMod();

}