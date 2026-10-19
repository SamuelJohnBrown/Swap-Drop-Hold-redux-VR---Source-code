// Console tests for LooseHoldRules.h: when a weapon HIGGS is holding loose - because Grip Hold's
// re-grab cooldown refused to equip it - is given back as a weapon.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_loose_hold_tests.cpp /Fe:loose_hold_tests.exe
//   loose_hold_tests.exe
// Exit code 0 means every check passed.

#include "LooseHoldRules.h"

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

	using LooseHoldRule::GiveBack;
	using LooseHoldRule::Why;

	constexpr bool kSimple = true, kGripHold = false;
	constexpr bool kSameHand = true, kOtherHand = false;
	constexpr bool kClosed = true, kOpen = false;
	constexpr bool kCooldownOver = false, kCooldownRunning = true;
	constexpr bool kLoopFree = false, kLoopBlocks = true;

	void TestTheSameHandStillGetsItBack()
	{
		// What 2.2.x already did: the hand that dropped it, squeezing, after the cooldown.
		Check(GiveBack(kGripHold, Why::kCooldown, kSameHand, kClosed, kCooldownOver, kLoopFree),
			"same hand, cooldown over, grip closed: equipped");
	}

	void TestTheOtherHandNowGetsItBackToo()
	{
		// The 2026-09-22 test9 log: the right hand threw, the left caught it 0.22-0.30s later with its grip
		// closed, the cooldown refused it - and it stayed loose for 0.72-2.60s, every time, because only the
		// SAME hand was ever given it back. "I still would most likely hold it as a HIGGS weapon."
		Check(GiveBack(kGripHold, Why::kCooldown, kOtherHand, kClosed, kCooldownOver, kLoopFree),
			"other hand, cooldown over, grip closed: equipped (the fix)");
	}

	void TestTheCooldownItselfStillHolds()
	{
		Check(!GiveBack(kGripHold, Why::kCooldown, kOtherHand, kClosed, kCooldownRunning, kLoopFree),
			"other hand while the cooldown still covers it: stays loose");
		Check(!GiveBack(kGripHold, Why::kCooldown, kSameHand, kClosed, kCooldownRunning, kLoopFree),
			"same hand while the cooldown still covers it: stays loose");
	}

	void TestAnOpenHandIsNotHoldingAWeapon()
	{
		// Why other-hand catches were refused in the first place: HIGGS taking hold of a dropped weapon on its
		// own. A hand that is not squeezing is not holding its weapon, whichever hand dropped it.
		Check(!GiveBack(kGripHold, Why::kCooldown, kOtherHand, kOpen, kCooldownOver, kLoopFree), "other hand, grip open: stays loose");
		Check(!GiveBack(kGripHold, Why::kCooldown, kSameHand, kOpen, kCooldownOver, kLoopFree), "same hand, grip open: stays loose");
	}

	void TestTheLoopGuardIsLeftAlone()
	{
		// A weapon caught in a drop/catch/re-equip loop stays a physics object until it is picked up
		// deliberately again - never handed back on its own, even after the guard lets go of it.
		Check(!GiveBack(kGripHold, Why::kLoopGuard, kOtherHand, kClosed, kCooldownOver, kLoopFree),
			"refused by the loop guard: never given back on its own");
		Check(!GiveBack(kGripHold, Why::kLoopGuard, kOtherHand, kClosed, kCooldownOver, kLoopBlocks), "not while it blocks either");
		Check(!GiveBack(kGripHold, Why::kCooldown, kSameHand, kClosed, kCooldownOver, kLoopBlocks),
			"and a cooldown hold is not given back while the loop guard blocks that weapon");
	}

	void TestSimpleModeIsUntouched()
	{
		// Simple mode's drops are deliberate double taps; a loose hold there ends when the hand opens.
		Check(!GiveBack(kSimple, Why::kCooldown, kSameHand, kClosed, kCooldownOver, kLoopFree), "Simple mode, same hand: never");
		Check(!GiveBack(kSimple, Why::kCooldown, kOtherHand, kClosed, kCooldownOver, kLoopFree), "Simple mode, other hand: never");
	}
}

int main()
{
	TestTheSameHandStillGetsItBack();
	TestTheOtherHandNowGetsItBackToo();
	TestTheCooldownItselfStillHolds();
	TestAnOpenHandIsNotHoldingAWeapon();
	TestTheLoopGuardIsLeftAlone();
	TestSimpleModeIsUntouched();
	std::printf("loose_hold: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
