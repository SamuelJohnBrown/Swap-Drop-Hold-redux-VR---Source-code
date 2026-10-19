// Console tests for GripTouchRules.h: which grip reading Simple mode's double tap is fed, per
// controller, driven through the REAL double-tap detector (GripGestures.h) with timelines of what a
// controller reports.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_grip_touch_tests.cpp /Fe:grip_touch_tests.exe
//   grip_touch_tests.exe
// Exit code 0 means every check passed.

#include "GripGestures.h"
#include "GripTouchRules.h"

#include <cstdio>

using namespace SwapDropAndHoldRedux;

namespace
{
	int s_checks = 0;
	int s_failures = 0;

	void Check(const bool ok, const char* what)
	{
		++s_checks;
		if (!ok)
		{
			++s_failures;
			std::printf("FAIL: %s\n", what);
		}
	}

	// What the controller reports for one stretch of time: the legacy grip button's pressed bit and its
	// touched bit.
	struct Sample
	{
		bool pressed;
		bool touched;
		float seconds;
	};

	// Plays a timeline at 90 Hz through the reading under test into the shipped detector with its shipped
	// settings, and counts how many times the detector says "let go of the weapon".
	int Drops(const Sample* timeline, const int count, const bool tapsCountTouch)
	{
		Gestures::DoubleTapDetector detector;
		const Gestures::DoubleTapConfig config;
		const float dt = 1.0f / 90.0f;

		// Every timeline starts with the squeeze that took hold of the weapon.
		detector.Bind(GripTouch::TapReading(timeline[0].pressed, timeline[0].touched, tapsCountTouch));

		int drops = 0;
		for (int i = 0; i < count; ++i)
		{
			const bool closed = GripTouch::TapReading(timeline[i].pressed, timeline[i].touched, tapsCountTouch);
			for (float t = 0.0f; t < timeline[i].seconds; t += dt)
			{
				if (detector.Update(closed, dt, config))
				{
					++drops;
				}
			}
		}
		return drops;
	}

	template <int N>
	int Drops(const Sample (&timeline)[N], const bool tapsCountTouch)
	{
		return Drops(timeline, N, tapsCountTouch);
	}
}

int main()
{
	// ---- which controllers have a grip touch that stays on -----------------------------------
	// SkyrimVR's own Index binding (bindings_knuckles.json): grip pressed = 80% squeeze force, grip
	// touched = the capacitive sensor, on for as long as fingers rest on the handle. Both of SteamVR's
	// Index profiles declare controller_type "knuckles".
	Check(GripTouch::TouchStaysOn("knuckles"), "an Index controller's grip touch stays on");
	Check(GripTouch::TouchStaysOn("Knuckles"), "whatever the capitals");
	Check(!GripTouch::TouchStaysOn("oculus_touch"), "a Touch controller's grip touch is only a slight pull");
	Check(!GripTouch::TouchStaysOn("vive_controller"), "a Vive wand has no grip touch at all");
	Check(!GripTouch::TouchStaysOn("holographic_controller"), "a WMR controller is left alone");
	Check(!GripTouch::TouchStaysOn("knuckles_like"), "only the exact type, not anything that starts like it");
	Check(!GripTouch::TouchStaysOn(""), "a controller that has not said what it is, is left alone");
	Check(!GripTouch::TouchStaysOn(nullptr), "and so is no answer at all");

	// ---- the decision ------------------------------------------------------------------------
	Check(!GripTouch::TapsCountTouch(GripTouch::kAuto, "knuckles"), "auto on Index: taps are presses only");
	Check(GripTouch::TapsCountTouch(GripTouch::kAuto, "oculus_touch"), "auto on Touch: press or touch, as before");
	Check(GripTouch::TapsCountTouch(GripTouch::kAuto, "vive_controller"), "auto on a Vive wand: as before");
	Check(GripTouch::TapsCountTouch(GripTouch::kAuto, ""), "auto before the controller is known: as before");
	Check(!GripTouch::TapsCountTouch(GripTouch::kPressOnly, "oculus_touch"), "the ini can force presses only on any controller");
	Check(GripTouch::TapsCountTouch(GripTouch::kPressOrTouch, "knuckles"), "and can force press-or-touch on Index");
	Check(!GripTouch::TapsCountTouch(-5, "knuckles"), "an ini value below the range means auto");
	Check(GripTouch::TapsCountTouch(-5, "oculus_touch"), "auto both ways");
	Check(GripTouch::TapsCountTouch(7, "knuckles"), "an ini value above the range means press or touch");

	// ---- the reading -------------------------------------------------------------------------
	Check(GripTouch::TapReading(true, true, false), "a press is closed, presses only");
	Check(GripTouch::TapReading(true, false, true), "a press is closed, press or touch");
	Check(GripTouch::TapReading(false, true, true), "a touch is closed when touch counts");
	Check(!GripTouch::TapReading(false, true, false), "a touch is NOT closed when only presses count");
	Check(!GripTouch::TapReading(false, false, true), "an untouched grip is open");

	// ---- the report: an Index player double-squeezes and nothing happens --------------------------
	// The fingers never leave the grip, so touched is on the whole time; only the force comes and goes.
	const Sample indexDoubleSqueeze[] = {
		{ true, true, 0.30f },  // the squeeze that grabbed the weapon
		{ false, true, 1.00f }, // carrying it: relaxed, fingers still wrapped round the controller
		{ true, true, 0.12f },  // squeeze
		{ false, true, 0.10f },
		{ true, true, 0.12f },  // squeeze
		{ false, true, 0.30f },
	};
	Check(Drops(indexDoubleSqueeze, GripTouch::TapsCountTouch(GripTouch::kAuto, "knuckles")) == 1,
		"Index, auto: a double squeeze lets go of the weapon, once");
	Check(Drops(indexDoubleSqueeze, GripTouch::TapsCountTouch(GripTouch::kPressOrTouch, "knuckles")) == 0,
		"Index forced to press-or-touch: the same double squeeze is one long hold (2.2.0's behaviour)");

	// One squeeze is not a double tap.
	const Sample indexSingleSqueeze[] = {
		{ true, true, 0.30f },
		{ false, true, 1.00f },
		{ true, true, 0.12f },
		{ false, true, 1.00f },
	};
	Check(Drops(indexSingleSqueeze, GripTouch::TapsCountTouch(GripTouch::kAuto, "knuckles")) == 0,
		"Index, auto: one squeeze keeps the weapon");

	// Squeezing to keep hold of the controller - a long squeeze, then another - is holding, not tapping.
	const Sample indexTwoLongSqueezes[] = {
		{ true, true, 0.30f },
		{ false, true, 1.00f },
		{ true, true, 0.60f },
		{ false, true, 0.10f },
		{ true, true, 0.60f },
		{ false, true, 0.30f },
	};
	Check(Drops(indexTwoLongSqueezes, GripTouch::TapsCountTouch(GripTouch::kAuto, "knuckles")) == 0,
		"Index, auto: two long squeezes keep the weapon");

	// The stopgap on 2.2.0: with touch counted, the hand only reads open when the fingers come off the
	// grip - so open, close, open, close, open lets go on the last opening.
	const Sample indexLiftedFingers[] = {
		{ true, true, 0.30f },
		{ false, true, 1.00f },
		{ false, false, 0.10f }, // fingers off
		{ false, true, 0.12f },  // back on
		{ false, false, 0.10f },
		{ false, true, 0.12f },
		{ false, false, 0.30f },
	};
	Check(Drops(indexLiftedFingers, true) == 1,
		"press-or-touch on Index: lifting the fingers off the grip twice does let go");

	// ---- nobody else changes ---------------------------------------------------------------------
	// A Touch grip springs open, and its "touched" is a slight pull that leads and trails the press.
	const Sample touchDoubleTap[] = {
		{ true, true, 0.30f },
		{ false, false, 1.00f },
		{ false, true, 0.02f }, { true, true, 0.10f }, { false, true, 0.02f },
		{ false, false, 0.08f },
		{ false, true, 0.02f }, { true, true, 0.10f }, { false, true, 0.02f },
		{ false, false, 0.30f },
	};
	Check(Drops(touchDoubleTap, GripTouch::TapsCountTouch(GripTouch::kAuto, "oculus_touch")) == 1,
		"Touch, auto: a double tap lets go, as it always has");

	// A light double tap that never reaches the click point still counts on Touch today. Reading presses
	// only for EVERYONE would break it, which is why the change is Index-only.
	const Sample touchLightDoubleTap[] = {
		{ true, true, 0.30f },
		{ false, false, 1.00f },
		{ false, true, 0.12f },
		{ false, false, 0.10f },
		{ false, true, 0.12f },
		{ false, false, 0.30f },
	};
	Check(Drops(touchLightDoubleTap, GripTouch::TapsCountTouch(GripTouch::kAuto, "oculus_touch")) == 1,
		"Touch, auto: a light double tap still lets go");
	Check(Drops(touchLightDoubleTap, GripTouch::TapsCountTouch(GripTouch::kPressOnly, "oculus_touch")) == 0,
		"Touch forced to presses only: a light double tap no longer counts (the cost of forcing it)");

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
