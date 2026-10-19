#pragma once

// Weapon Throw VR's hold button for each hand, read live from its own two globals. Grip Hold's blink is
// only for a hand whose button is Grip - GripBlinkRules.h says why.

namespace SwapDropAndHoldRedux
{
	// Finds the two globals by editor id, as Weapon Throw VR itself looks them up (its DLL carries the
	// names, not form ids), so a renamed or merged WeaponThrowVR.esp still works. Call at DataLoaded.
	void WeaponThrowButtonsFind();

	// The hold button Weapon Throw VR uses for this GAME hand right now: a GripBlink::kThrowButton*
	// value, kThrowButtonUnread when the global was not found. Read every time, because the value
	// lives in the save and Weapon Throw VR's menu can change it at any moment.
	int WeaponThrowButtonFor(bool isLeftGameHand);
}
