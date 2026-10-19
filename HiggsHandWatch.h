#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	// Diagnostics for "after a drop, that hand cannot pull the weapon back with HIGGS".
	//
	// Logs every HIGGS grab, drop, pull, stash and consume, and for HandWatch::Config's window
	// after each of those - and after a drop from the hand, or this mod picking up a weapon
	// HIGGS is holding - HIGGS's own view of that hand on every change: whether it can grab,
	// whether it is idle, what it holds, whether it is disabled, beside this mod's grip and
	// trigger reads. A pull that fails shows up as a grip press in a state that says why.

	// Registers the HIGGS event callbacks. Call once HIGGS's interface is available.
	void HiggsHandWatchRegister();

	// Opens (or extends) the watch on a hand. `why` and `item` label its lines.
	void HiggsHandWatchBegin(bool isLeftVRController, const char* why, TESForm* item);

	// Call right before this mod picks up a reference HIGGS may be holding: logs what HIGGS
	// holds in that hand at that moment, then watches the hand.
	void HiggsHandWatchNotePickup(bool isLeftVRController, TESObjectREFR* pickedRef);

	// Call once per frame.
	void HiggsHandWatchTick(float deltaTime);
}
