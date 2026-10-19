// Console tests for CellLoadGuardRules.h: standing the mod down across a cell load, so a weapon in your
// hand is not dropped or lost going through a door - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_cell_guard_tests.cpp /Fe:cell_guard_tests.exe
//   cell_guard_tests.exe
// Exit code 0 means every check passed.

#include "CellLoadGuardRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux::CellLoadGuard;

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
		return std::fabs(a - b) < 0.0005f;
	}

	// One second of frames at 90 Hz.
	void RunSeconds(Guard& guard, const float seconds, const float hold)
	{
		const float dt = 1.0f / 90.0f;
		for (float t = 0.0f; t < seconds; t += dt)
		{
			guard.Update(dt, hold);
		}
	}
}

int main()
{
	// ---- the setting's range -------------------------------------------------------------------
	Check(Near(ClampHold(1.0f), 1.0f), "a second is a second");
	Check(Near(ClampHold(-2.0f), kMinHoldSeconds), "a negative hold is no hold");
	Check(Near(ClampHold(50.0f), kMaxHoldSeconds), "an ini value past the top is pulled back");
	Check(Near(ClampHold(0.0f), 0.0f), "zero is allowed: lock only while the loading screen is up");

	// ---- a door: the screen goes up, then comes down ---------------------------------------------
	{
		Guard guard;
		Check(!guard.Locked(), "a fresh guard is not locked");
		Check(guard.Update(1.0f, 1.0f) == Step::kNothing, "and idle time does nothing");

		Check(guard.LoadingStarted() == Step::kLocked, "the loading screen going up locks the mod at once");
		Check(guard.Locked(), "and it stays locked");
		Check(guard.Loading(), "the guard knows a load is in progress");
		Check(guard.LoadingStarted() == Step::kNothing, "a second loading screen says nothing new");

		// A load can take a long time, and the mod must stay locked for all of it.
		RunSeconds(guard, 20.0f, 1.0f);
		Check(guard.Locked(), "twenty seconds of loading: still locked");

		Check(guard.LoadingFinished() == Step::kNothing, "the screen coming down does not unlock on its own");
		Check(guard.Locked(), "the hold keeps it locked into the new cell");
		Check(!guard.Loading(), "but the load itself is over");

		// The FIRST frame after a load is worth seconds of real time. It must not eat the whole hold.
		Check(guard.Update(3.0f, 1.0f) == Step::kNothing, "one enormous frame does not end the hold");
		Check(Near(guard.HoldLeft(), 1.0f - kMaxStepSeconds), "it counts for at most a quarter second");

		RunSeconds(guard, 0.70f, 1.0f);
		Check(guard.Locked(), "three quarters of a second in: still locked");
		RunSeconds(guard, 0.20f, 1.0f);
		Check(!guard.Locked(), "a second after the load: the mod has itself back");
	}

	// ---- exactly one report per transition, for exactly one log line each ------------------------
	{
		Guard guard;
		guard.LoadingStarted();
		guard.LoadingFinished();
		int locked = 0, unlocked = 0;
		for (int i = 0; i < 400; ++i)
		{
			const Step step = guard.Update(1.0f / 90.0f, 1.0f);
			locked += step == Step::kLocked ? 1 : 0;
			unlocked += step == Step::kUnlocked ? 1 : 0;
		}
		Check(unlocked == 1, "the hold ending is reported once, not every frame after it");
		Check(locked == 0, "and nothing re-locks on its own");
	}

	// ---- a load with no hold asked for -----------------------------------------------------------
	{
		Guard guard;
		guard.LoadingStarted();
		guard.LoadingFinished();
		Check(guard.Locked(), "with the hold at zero it is still locked while the screen is up");
		Check(guard.Update(1.0f / 90.0f, 0.0f) == Step::kUnlocked, "and lets go on the first frame in the new cell");
	}

	// ---- one door straight into another ----------------------------------------------------------
	{
		Guard guard;
		guard.LoadingStarted();
		guard.LoadingFinished();
		RunSeconds(guard, 0.50f, 1.0f);
		Check(guard.LoadingStarted() == Step::kNothing, "a second load while still held: already locked, nothing new to say");
		Check(guard.Loading(), "but it is loading again");
		RunSeconds(guard, 5.0f, 1.0f);
		Check(guard.Locked(), "so the hold cannot run out under a loading screen");
		guard.LoadingFinished();
		RunSeconds(guard, 0.50f, 1.0f);
		Check(guard.Locked(), "the new load gets its own full hold");
		RunSeconds(guard, 0.60f, 1.0f);
		Check(!guard.Locked(), "and then lets go");
	}

	// ---- the menu closing when we never saw it open ------------------------------------------------
	{
		Guard guard;
		Check(guard.LoadingFinished() == Step::kNothing, "a loading screen we never saw start does not lock anything");
		Check(!guard.Locked(), "and leaves the mod alone");
		Check(guard.Update(1.0f / 90.0f, 1.0f) == Step::kNothing, "nothing to count down");
	}

	// ---- a loading screen that never comes down ----------------------------------------------------
	{
		Guard guard;
		guard.LoadingStarted();
		RunSeconds(guard, kSafetySeconds - 1.0f, 1.0f);
		Check(guard.Locked(), "just under the safety limit: still locked");
		int unlocked = 0;
		for (int i = 0; i < 400; ++i)
		{
			unlocked += guard.Update(1.0f / 90.0f, 1.0f) == Step::kUnlocked ? 1 : 0;
		}
		Check(unlocked == 1, "past it, the guard gives the mod back exactly once rather than locking for ever");
		Check(!guard.Locked(), "and it is unlocked");
	}

	// ---- the mod being switched off, or a game reload, clears it ------------------------------------
	{
		Guard guard;
		guard.LoadingStarted();
		guard.Clear();
		Check(!guard.Locked() && !guard.Loading(), "Clear gives the mod straight back");
		Check(guard.Update(1.0f, 1.0f) == Step::kNothing, "with nothing left to report");
		Check(guard.LoadingFinished() == Step::kNothing, "and a stale screen-closed after it does nothing");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
