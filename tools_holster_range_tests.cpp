// Console tests for HolsterRangeRules.h: how near a VRIK holster slot a hand has to be before
// this mod stands down for VRIK, without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_holster_range_tests.cpp /Fe:holster_range_tests.exe
//   holster_range_tests.exe
// Exit code 0 means every check passed.

#include "HolsterRangeRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux::HolsterRange;

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

	bool Near(const float a, const float b)
	{
		return std::fabs(a - b) < 0.001f;
	}

	// The user's install: VRIK's slotActivationDistance 8, and the shipped
	// fHolsterExtraRange 6, fHolsterExtraRangeBehind 14 and fHolsterReleaseMargin 4.
	const float kVrik = 8.0f;
	const float kFront = 6.0f;
	const float kBehind = 14.0f;
	const float kMargin = 4.0f;

	// The radius the stand-down check uses, asked for the way ShouldStandDownForHolster asks.
	float StandDownRange(const bool simpleMode, const bool behind, const bool alreadyStandingDown)
	{
		return CatchRange(kVrik, kFront, kBehind, behind, ForStandDown(simpleMode, alreadyStandingDown, kMargin));
	}

	// HolsterNearHand counts a slot only when the hand is strictly inside the radius.
	bool StandsDown(const bool simpleMode, const bool behind, const bool alreadyStandingDown, const float distance)
	{
		return distance < StandDownRange(simpleMode, behind, alreadyStandingDown);
	}
}

int main()
{
	// Grip Hold keeps the wide zone it has always had: letting go drops the weapon there, so
	// a holster VRIK would have taken must never be missed.
	Check(Near(StandDownRange(false, false, false), 14.0f), "Grip Hold, in front: VRIK's 8 + 6");
	Check(Near(StandDownRange(false, true, false), 22.0f), "Grip Hold, behind: VRIK's 8 + 14");
	Check(Near(StandDownRange(false, false, true), 18.0f), "Grip Hold, in front, leaving: + the 4 margin");
	Check(Near(StandDownRange(false, true, true), 26.0f), "Grip Hold, behind, leaving: + the 4 margin");

	// The release-holster check in DispatchGripRelease asks with the defaults.
	Check(Near(CatchRange(kVrik, kFront, kBehind, false, Reach{}), 14.0f), "default reach in front is unchanged");
	Check(Near(CatchRange(kVrik, kFront, kBehind, true, Reach{}), 22.0f), "default reach behind is unchanged");

	// Simple mode stands down only where VRIK itself holsters.
	Check(Near(StandDownRange(true, false, false), 8.0f), "Simple, in front: VRIK's own 8");
	Check(Near(StandDownRange(true, true, false), 8.0f), "Simple, behind: VRIK's own 8, no extra range");
	Check(Near(StandDownRange(true, false, true), 8.0f), "Simple, in front, leaving: no margin on top");
	Check(Near(StandDownRange(true, true, true), 8.0f), "Simple, behind, leaving: still VRIK's 8");
	Check(
		Near(CatchRange(12.0f, kFront, kBehind, true, ForStandDown(true, true, kMargin)), 12.0f),
		"Simple follows VRIK's own setting when it is not 8");

	// The M2.1.1-test2 log: double taps ignored while standing down over the Right Hip, entered
	// at these distances. VRIK never took the weapon at any of them.
	const float ignoredAt[] = { 9.9f, 13.9f, 13.8f, 13.0f, 12.7f, 13.4f };
	for (const float distance : ignoredAt)
	{
		Check(!StandsDown(true, false, false, distance), "Simple: a hand this far out is not at VRIK's holster");
		Check(StandsDown(false, false, false, distance), "Grip Hold: the same hand still stands down");
	}

	// Simple mode's edge.
	Check(StandsDown(true, false, false, 7.9f), "Simple: just inside VRIK's 8 stands down");
	Check(!StandsDown(true, false, false, 8.0f), "Simple: exactly 8 is outside, as in HolsterNearHand");
	Check(StandsDown(true, true, true, 3.1f), "Simple: a hand right on a slot stands down");

	// Grip Hold with every extra set to 0 is VRIK's own reach as well.
	Check(
		Near(CatchRange(kVrik, 0.0f, 0.0f, false, ForStandDown(false, true, 0.0f)), 8.0f),
		"Grip Hold with no extra range and no margin is VRIK's 8");

	// THE BUZZ. A measured slot buzzes at VRIK's own reach; an unmeasured one keeps the wide
	// zone, so no VRIK version loses a buzz it had.
	Check(Near(CatchRange(kVrik, kFront, kBehind, false, ForBuzz()), 8.0f), "buzz in front: VRIK's own 8");
	Check(Near(CatchRange(kVrik, kFront, kBehind, true, ForBuzz()), 8.0f), "buzz behind: VRIK's own 8 too");
	Check(
		Near(CatchRange(10.0f, kFront, kBehind, true, ForBuzz()), 10.0f),
		"buzz follows slotActivationDistance when it is not 8");
	Check(
		CatchRange(kVrik, kFront, kBehind, false, ForBuzz()) <
			CatchRange(kVrik, kFront, kBehind, false, ForStandDown(false, false, kMargin)),
		"the buzz zone is INSIDE Grip Hold's stand-aside zone, in front");
	Check(
		CatchRange(kVrik, kFront, kBehind, true, ForBuzz()) <
			CatchRange(kVrik, kFront, kBehind, true, ForStandDown(false, true, kMargin)),
		"the buzz zone is inside it behind the body, margin included");
	Check(
		Near(
			CatchRange(kVrik, kFront, kBehind, false, ForBuzz()),
			CatchRange(kVrik, kFront, kBehind, false, ForStandDown(true, false, kMargin))),
		"a measured slot buzzes exactly where Simple mode already stands down");
	Check(!BuzzOnStandDownEntry(true), "a measured slot does not buzz on entering the wide zone");
	Check(BuzzOnStandDownEntry(false), "an unmeasured slot still buzzes on entering the wide zone");

	// The M2.1.1-test2 distances again. Every one of them buzzed before this; at a measured
	// slot none of them do, because VRIK would not have taken the weapon at any of them either.
	for (const float distance : ignoredAt)
	{
		Check(
			distance >= CatchRange(kVrik, kFront, kBehind, false, ForBuzz()),
			"a hand this far out no longer buzzes at a measured slot");
		Check(
			distance >= CatchRange(kVrik, kFront, kBehind, true, ForBuzz()),
			"behind the body either");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
