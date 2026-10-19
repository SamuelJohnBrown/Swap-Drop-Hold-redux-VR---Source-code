#pragma once

// Which grip reading Simple mode's double tap is fed, per controller.
//
// Everywhere else in this mod a hand is "closed" when a watched grip button is pressed OR the legacy
// grip button (k_EButton_Grip) is merely touched - GripHold.cpp, IsGripPressed. That is right for
// HOLDING, and proven on Touch controllers, where "touched" is only a slight pull of the grip.
//
// On Valve Index controllers it cannot work for TAPS. SkyrimVR's own Index binding
// (SkyrimVR\bindings\bindings_knuckles.json, "Index Controller Legacy Defaults") sets grip PRESSED at
// 80% squeeze force and grip TOUCHED from the capacitive sensor, which stays on for as long as the
// fingers rest on the handle. A player who squeezes twice never takes their fingers off, so the hand
// reads as one long hold and the double tap never sees a release - reported 2026-09-18 against 2.2.0:
// "double-tapping to drop simply does nothing at all", with every grip button preset.
//
// So on a controller whose grip touch stays on, the double tap reads presses only. Holding keeps
// reading touch there: it is what lets an Index player carry a weapon without squeezing at 80% force,
// and drop it by opening the fingers. Every other controller keeps the reading it has always had -
// a light double tap on Touch reaches "touched" without reaching the click point, and must keep working.
//
// No engine or OpenVR types in here, so tools_grip_touch_tests.cpp can drive it outside the game.

namespace SwapDropAndHoldRedux
{
	namespace GripTouch
	{
		// iDoubleTapCountsTouch in the ini.
		constexpr int kAuto = -1;        // decide from the controller
		constexpr int kPressOnly = 0;    // a tap is a press, on any controller
		constexpr int kPressOrTouch = 1; // a tap is a press or a touch, on any controller (2.2.0 and before)

		inline int Setting(const int raw)
		{
			if (raw < kAuto)
			{
				return kAuto;
			}
			return raw > kPressOrTouch ? kPressOrTouch : raw;
		}

		inline bool EqualsIgnoringCase(const char* a, const char* b)
		{
			for (; *a && *b; ++a, ++b)
			{
				const char x = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
				const char y = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b - 'A' + 'a') : *b;
				if (x != y)
				{
					return false;
				}
			}
			return *a == *b;
		}

		// controllerType is OpenVR's Prop_ControllerType_String for that controller. Both of SteamVR's
		// Index profiles (index_controller_profile.json, knuckles_ev1_profile.json) declare "knuckles".
		inline bool TouchStaysOn(const char* controllerType)
		{
			return controllerType != nullptr && EqualsIgnoringCase(controllerType, "knuckles");
		}

		// Does a touched grip count as closed for the double tap? A controller that has not said what it
		// is yet gets the reading everyone has always had.
		inline bool TapsCountTouch(const int setting, const char* controllerType)
		{
			switch (Setting(setting))
			{
			case kPressOnly:
				return false;
			case kPressOrTouch:
				return true;
			default:
				return !TouchStaysOn(controllerType);
			}
		}

		// The reading the double tap is fed.
		inline bool TapReading(const bool pressed, const bool touched, const bool tapsCountTouch)
		{
			return pressed || (touched && tapsCountTouch);
		}
	}
}
