// Console tests for VrikPulseRules.h: telling VRIK's holster buzz from every other haptic pulse, and
// standing aside where it buzzed - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_vrik_pulse_tests.cpp /Fe:vrik_pulse_tests.exe
//   vrik_pulse_tests.exe
// Exit code 0 means every check passed.

#include "VrikPulseRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux::VrikPulse;

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

	constexpr long long kMs = 1000000LL; // nanoseconds

	// Feeds a run of n same-duration pulses one frame (17 ms) apart on one hand; returns how many fired.
	int FeedRun(RunDetector& d, const int hand, const int n, const unsigned short micros, long long& t, const long long gapNs = 17 * kMs)
	{
		int fired = 0;
		for (int i = 0; i < n; ++i)
		{
			t += gapNs;
			if (d.Feed(hand, false, micros, t))
			{
				++fired;
			}
		}
		return fired;
	}
}

int main()
{
	// ---- the detector -------------------------------------------------------------------------
	// The M2.2.7-test1 census: VRIK = 749 us, runs of exactly hapticStrength (8), one per frame.
	{
		RunDetector d;
		long long t = 1000 * kMs;
		Check(FeedRun(d, 0, 8, 749, t) == 1, "a run of 8 one-frame pulses fires exactly once");
		t += 600 * kMs;
		Check(FeedRun(d, 0, 8, 749, t) == 1, "a second run after a gap fires again");
	}
	{
		RunDetector d;
		long long t = 0;
		Check(FeedRun(d, 0, kMinRun - 1, 749, t) == 0, "one short of the minimum run does not fire");
		t += 17 * kMs;
		Check(d.Feed(0, false, 749, t), "the pulse that completes the minimum run fires");
		t += 17 * kMs;
		Check(!d.Feed(0, false, 749, t), "and the rest of the same run does not fire again");
	}
	{
		// The other mods' bursts in that census: many pulses in the SAME instant.
		RunDetector d;
		long long t = 5000 * kMs;
		Check(FeedRun(d, 1, 11, 3999, t, 0) == 0, "eleven pulses in one instant are not a run");
		Check(FeedRun(d, 0, 11, 760, t, 1 * kMs) == 0, "nor eleven a millisecond apart");
	}
	{
		RunDetector d;
		long long t = 0;
		Check(FeedRun(d, 0, 8, 749, t, 60 * kMs) == 0, "pulses further apart than a frame are not a run");
	}
	{
		// Mixed durations are two different senders, not one run.
		RunDetector d;
		long long t = 0;
		t += 17 * kMs; d.Feed(0, false, 749, t);
		t += 17 * kMs; d.Feed(0, false, 749, t);
		t += 17 * kMs; d.Feed(0, false, 1335, t);
		t += 17 * kMs; Check(!d.Feed(0, false, 749, t), "a different duration in the middle restarts the count");
		t += 17 * kMs; d.Feed(0, false, 749, t);
		t += 17 * kMs; d.Feed(0, false, 749, t);
		t += 17 * kMs; Check(d.Feed(0, false, 749, t), "and four of the same duration after it fire");
	}
	{
		// This mod's own buzz never counts, and breaks a run.
		RunDetector d;
		long long t = 0;
		Check(FeedRun(d, 0, 3, 749, t) == 0, "three so far");
		t += 17 * kMs;
		Check(!d.Feed(0, true, 1800, t), "our own pulse does not fire");
		t += 17 * kMs;
		Check(!d.Feed(0, false, 749, t), "and the run has to start over after it");
	}
	{
		// Hands are independent.
		RunDetector d;
		long long t = 0;
		for (int i = 0; i < 3; ++i)
		{
			t += 17 * kMs;
			d.Feed(0, false, 749, t);
			d.Feed(1, false, 749, t);
		}
		t += 17 * kMs;
		Check(d.Feed(1, false, 749, t), "the left hand's fourth fires for the left");
		Check(d.Feed(0, false, 749, t), "and the right hand's fourth fires for the right");
	}
	{
		RunDetector d;
		Check(!d.Feed(-1, false, 749, 17 * kMs), "a pulse to no controller is ignored");
		Check(!d.Feed(2, false, 749, 34 * kMs), "so is a nonsense hand");
	}

	// ---- which answer governs the stand-aside -------------------------------------------------
	Check(!VrikDrives(true, true, false, true), "VRIK 0.8.7+: the API answer governs, never the buzz");
	Check(!VrikDrives(true, false, true, true), "Simple mode is untouched");
	Check(!VrikDrives(false, false, false, true), "bHolsterListenToVrik = 0: the estimate governs");
	Check(!VrikDrives(true, false, false, false), "no VRIK buzz seen yet this session: the estimate governs");
	Check(VrikDrives(true, false, false, true), "old VRIK, Grip hold, listening, and VRIK has buzzed: VRIK governs");

	// ---- standing aside where VRIK buzzed ----------------------------------------------------
	const float kR = 10.0f; // slotActivationDistance
	{
		StandAside s;
		Check(!s.Active(), "not standing aside until VRIK says so");
		s.Enter(Vec3{ 0.0f, 0.0f, 0.0f }, 100.0f);
		Check(s.Active(), "a VRIK buzz starts it");
		Check(s.Update(Vec3{ 5.0f, 0.0f, 0.0f }, 100.1f, kR), "5 units off: still aside");
		Check(s.Update(Vec3{ 20.0f, 0.0f, 0.0f }, 100.2f, kR), "20 units off before the minimum hold: still aside");
		Check(s.Update(Vec3{ 20.0f, 0.0f, 0.0f }, 100.5f, kR), "20 units off after the minimum hold: still aside (leave is 2.75 x reach = 27.5)");
		Check(!s.Update(Vec3{ 28.0f, 0.0f, 0.0f }, 100.6f, kR), "28 units off: the hand has left");
		Check(!s.Active(), "and it stays off");
	}
	{
		// Why 2.75: VRIK keeps a slot until 1.75 x reach from the CENTRE, and the entry point is ~1 x
		// reach from it, so a hand moving straight through leaves VRIK's hold up to 2.75 x reach from
		// where it entered. Ending earlier would let a release there drop a weapon VRIK would take.
		Check(std::fabs(kLeaveFactor - 2.75f) < 0.001f, "the leave factor is 1.75 + 1");
		Check(LeaveDistance(kR) > 1.75f * kR + kR - 0.01f, "leave distance covers the far side of VRIK's hold");
		Check(std::fabs(LeaveDistance(8.0f) - 22.0f) < 0.01f, "at VRIK's default reach of 8 that is 22");
	}
	{
		StandAside s;
		s.Enter(Vec3{ 0.0f, 0.0f, 0.0f }, 0.0f);
		Check(s.Update(Vec3{ 1.0f, 0.0f, 0.0f }, kMaxHoldSeconds - 0.1f, kR), "just under the maximum hold: still aside");
		Check(!s.Update(Vec3{ 1.0f, 0.0f, 0.0f }, kMaxHoldSeconds + 0.1f, kR), "the maximum hold ends it even with the hand still there");
	}
	{
		// A second buzz while still aside moves the entry point: VRIK left and re-entered.
		StandAside s;
		s.Enter(Vec3{ 0.0f, 0.0f, 0.0f }, 0.0f);
		s.Enter(Vec3{ 20.0f, 0.0f, 0.0f }, 1.0f);
		Check(s.Update(Vec3{ 40.0f, 0.0f, 0.0f }, 1.5f, kR), "40 from the first entry but 20 from the second: still aside");
		Check(std::fabs(s.DistanceFromEntry(Vec3{ 40.0f, 0.0f, 0.0f }) - 20.0f) < 0.01f, "distance is from the latest entry");
	}
	{
		StandAside s;
		s.Enter(Vec3{ 0.0f, 0.0f, 0.0f }, 0.0f);
		s.Clear();
		Check(!s.Active(), "Clear ends it");
		Check(!s.Update(Vec3{ 0.0f, 0.0f, 0.0f }, 0.1f, kR), "and Update stays false");
	}
	{
		StandAside s;
		Check(!s.Update(Vec3{ 0.0f, 0.0f, 0.0f }, 1.0f, kR), "Update without an Enter is false");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
