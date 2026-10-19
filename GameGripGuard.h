#pragma once

#include "GameGripRules.h"

#include <cstdint>

// The game-grip guard (M3.0.0-test9, rebuilt in test10): the GAME is handed the real grip while Grip
// Hold's blink shows the other mods that grip released and pressed again. GameGripRules.h has the why
// and the how.

namespace SwapDropAndHoldRedux
{
	// Arms the guard. It goes in on the first HIGGS pre-physics step that finds the VR system with
	// SkyrimVRTools already in the controller-state slot - the layer Weapon Throw VR reads in, which the
	// guard must sit OUTSIDE. Needs HIGGS. Does nothing without Weapon Throw VR (then there is no blink)
	// or with bBlinkHiddenFromGame = 0.
	void RegisterGameGripGuard();

	// The guard is installed and on: the game is handed the real grip during a blink.
	bool GameGripGuardActive();

	// From MenuGrip's controller-state callback (inside SKSE's hook, on the polling thread), through a
	// blink and its grace: what one hand's grip really is, before anything hides it. One atomic store;
	// safe inside SKSE's lock.
	void GameGripGuardRecord(bool isLeftVRController, const GameGrip::Record& record);
}
