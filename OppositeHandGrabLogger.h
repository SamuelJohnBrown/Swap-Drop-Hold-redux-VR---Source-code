#pragma once

namespace SwapDropAndHoldRedux
{
	void RegisterOppositeHandGrabLogger();

	// True while the opposite hand is engaging the weapon equipped on this game hand
	// (HIGGS two-handing / cross-hand grab latch, including its release grace window).
	bool IsOppositeHandEngaged(bool isLeftEquippedHand);
}
