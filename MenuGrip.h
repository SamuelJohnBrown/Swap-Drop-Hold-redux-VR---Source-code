#pragma once

#include "skse64/PluginAPI.h"

namespace SwapDropAndHoldRedux
{
	// Menus opened while a weapon is grip-held stay open.
	//
	// SkyrimVR's menus treat the grip button as "back", and grip-hold keeps the grip
	// closed for as long as a weapon is carried - so opening any menu with a weapon in
	// hand backed straight out of it again. When a menu opens over a hand that is
	// holding a weapon, that hand's grip is hidden from the game until the player
	// physically lets go. From then on the grip passes through untouched, so a fresh
	// press closes the menu the way it always has.
	//
	// It takes three pieces, because the menus that matter PAUSE the game, and
	// grip-hold's own update runs on the physics step, which does not run while paused:
	//   - the menu-open listener decides to hide a grip, before the menu reads input;
	//   - SKSE VR's controller-state callback does the hiding, and notices the release,
	//     because menus keep reading the controller while the game is paused;
	//   - grip-hold's first frame after the menu turns that release into "equipped but
	//     not held" instead of a drop.

	// SKSE VR controller-state callback. Call from SKSEPlugin_Load.
	void RegisterMenuGripMask(PluginHandle plugin, SKSEVRInterface* vr);

	// Menu open/close listener. Needs the MenuManager, so kInputLoaded or later.
	void RegisterMenuGripMenuListener();

	// Grip-hold, on every frame it runs outside a menu: whether each VR controller is
	// holding a weapon with its grip closed right now.
	void MenuGripPublishHands(bool rightHolding, bool leftHolding);

	// Grip-hold, on a frame with no input-capturing menu open. Ends any hiding the
	// menu listener did not end itself.
	void MenuGripOnFrameWithoutMenu();

	// True once, after a menu, if this controller's grip was let go while the menu was
	// open. The call consumes it.
	bool MenuGripTakeReleasedInMenu(bool isLeftVRController);

	// Grip-hold's grip read, with the hiding undone. While a grip is hidden from the
	// game this returns what the hand is physically doing, not what the game was shown:
	// grip-hold must never take its own mask for an open hand, because that is a drop.
	bool MenuGripPhysicalGrip(bool isLeftVRController, bool readValue);

	// Right after a Grip Hold pickup (GripBlinkRules.h has the why): every other reader of this
	// controller is shown its grip OPEN for `seconds`, while MenuGripPhysicalGrip keeps giving
	// grip-hold the real hand. Only a closed grip is hidden, so a hand that really opens is seen
	// opening by everyone.
	void MenuGripShowReleased(bool isLeftVRController, float seconds);

	// Whether that is still going on.
	bool MenuGripShowingReleased(bool isLeftVRController);

	// M3.0.0-test7, the re-press that waits for a clear hand (GripBlinkRules.h): this hand's blink
	// window - when it began and when it ends, on the clock MenuGripNowTicks reads. False if no blink
	// was ever started for this hand.
	bool MenuGripBlinkWindow(bool isLeftVRController, long long& startTicks, long long& endTicks);

	// Keep this hand's blink window showing until endTicks. Only ever moves the end LATER, and never
	// restarts a window that has already ended: holding back cannot invent a blink.
	void MenuGripHoldBlinkUntil(bool isLeftVRController, long long endTicks);

	// The clock the window runs on, in nanoseconds.
	long long MenuGripNowTicks();

	// The real grip as the controller-state callback last read it while it was showing this hand
	// released (true = closed).
	bool MenuGripRealGripWhileShowing(bool isLeftVRController);

	// M3.0.0-test10: the tracked device index this hand's grip is read from, as the callback matches it.
	// An atomic load: safe from any thread, the game-grip guard's hook included.
	unsigned int MenuGripDevice(bool isLeftVRController);

	// A change of weapon mode: ends any hiding and forgets any release seen in a menu,
	// because both belonged to the mode that is no longer running.
	void MenuGripReset();
}
