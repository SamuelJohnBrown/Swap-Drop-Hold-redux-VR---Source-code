#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	// Makes HIGGS let go of a weapon this mod picks up out of a HIGGS hand the way HIGGS lets go
	// of anything, so that hand does not keep the weapon as its selection. Without this the hand
	// would not select, highlight or pull the weapon again once it is dropped as the same
	// reference, until something else was selected. HiggsReleaseRules.h has the HIGGS code this
	// rests on.

	// Call right before activating a reference a HIGGS hand may be holding. A hand that holds it
	// is switched off through the HIGGS API; HiggsReleaseTick switches it back on.
	void HiggsReleaseBeforePickup(TESObjectREFR* pickedRef);

	// Call for every drop: logs the dropped reference's handle, and whether it is the reference
	// last picked up out of a HIGGS hand - the case the release exists for.
	void HiggsReleaseNoteDrop(TESObjectREFR* droppedRef, TESForm* item);

	// Call once per physics step. Switches a hand back on once HIGGS has let go, or at a limit.
	void HiggsReleaseTick(float deltaTime);

	// Call before a game loads: switches back on any hand this mod still has off.
	void HiggsReleaseOnPreLoadGame();

	// Whether this mod has this controller's HIGGS hand switched off for a pickup right now.
	bool HiggsReleaseHandIsOff(bool isLeftVRController);
}
