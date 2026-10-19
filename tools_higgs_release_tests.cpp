// Console tests for HiggsReleaseRules.h: when a HIGGS hand is switched off for a pickup and when
// it is switched back on, without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_higgs_release_tests.cpp /Fe:higgs_release_tests.exe
//   higgs_release_tests.exe
// Exit code 0 means every check passed.

#include "HiggsReleaseRules.h"

#include <cstdio>

using namespace SwapDropAndHoldRedux::HiggsRelease;

namespace
{
	int s_checks = 0;
	int s_failures = 0;
	float s_rate = 90.0f;

	void Check(const bool ok, const char* what)
	{
		++s_checks;
		if (!ok)
		{
			++s_failures;
			std::printf("FAIL at %.0f Hz: %s\n", s_rate, what);
		}
	}

	struct Result
	{
		int enables = 0;          // kEnable steps seen
		int lateEnables = 0;      // kEnableLate steps seen
		int firstEnableStep = -1; // 1-based step of the first switch back on
	};

	void Tally(Result& result, const Step out, const int step)
	{
		if (out != Step::kEnable && out != Step::kEnableLate)
		{
			return;
		}
		if (result.firstEnableStep < 0)
		{
			result.firstEnableStep = step;
		}
		if (out == Step::kEnable)
		{
			++result.enables;
		}
		else
		{
			++result.lateEnables;
		}
	}

	// Runs `steps` physics steps at the current rate. HIGGS says the hand holds something until
	// step `letGoAt` (1-based), or on every step when letGoAt is 0.
	Result Run(Guard& guard, const int steps, const int letGoAt, const Config& config = Config{})
	{
		Result result;
		const float dt = 1.0f / s_rate;
		for (int step = 1; step <= steps; ++step)
		{
			const bool stillHolding = letGoAt == 0 || step < letGoAt;
			Tally(result, guard.Update(stillHolding, dt, config), step);
		}
		return result;
	}

	// Like Run, with the hand also asked to stay off on steps 1 to `keepOffSteps` (0: never, -1: on
	// every step) - the grip release Grip Hold shows other mods after a pickup (GripBlinkRules.h).
	Result RunKeepOff(Guard& guard, const int steps, const int letGoAt, const int keepOffSteps, const Config& config = Config{})
	{
		Result result;
		const float dt = 1.0f / s_rate;
		for (int step = 1; step <= steps; ++step)
		{
			const bool stillHolding = letGoAt == 0 || step < letGoAt;
			const bool keepOff = keepOffSteps < 0 || step <= keepOffSteps;
			Tally(result, guard.Update(stillHolding, keepOff, dt, config), step);
		}
		return result;
	}

	// test12: a whole Grip Hold pickup. HIGGS lets go on step `letGoAt`; the blink shows other mods the
	// grip released for `blinkSeconds`; the game has the weapon in this hand drawn from `drawnAt`
	// seconds (-1: never). Returns what the guard did and when.
	Result RunPickup(Guard& guard, const int steps, const int letGoAt, const float blinkSeconds, const float drawnAt,
		const bool weaponInHand, const Config& config = Config{})
	{
		Result result;
		const float dt = 1.0f / s_rate;
		for (int step = 1; step <= steps; ++step)
		{
			const float t = step * dt;
			const bool stillHolding = letGoAt == 0 || step < letGoAt;
			const bool showing = t <= blinkSeconds;
			const bool drawn = drawnAt >= 0.0f && t >= drawnAt;
			Tally(result, guard.Update(stillHolding, WhyKeepOff(showing, weaponInHand, drawn), dt, config), step);
		}
		return result;
	}

	// The step at which whichever limit comes first is reached, adding the frame time up the way
	// a running game does.
	int LimitStep(const Config& config)
	{
		const float dt = 1.0f / s_rate;
		float seconds = 0.0f;
		for (int step = 1;; ++step)
		{
			seconds += dt;
			if (step >= config.maxSteps || seconds >= config.maxSeconds)
			{
				return step;
			}
		}
	}
}

int main()
{
	const float rates[] = { 45.0f, 72.0f, 90.0f, 120.0f, 144.0f };
	for (const float rate : rates)
	{
		s_rate = rate;
		const Config config{};

		// The pickup of a weapon HIGGS holds, in a hand nothing has switched off.
		{
			Guard guard;
			Check(guard.Begin(true, false), "picking up what HIGGS holds switches the hand off");
			Check(guard.Active(), "and the hand is this mod's to switch back on");
			const Result r = Run(guard, 60, 2);
			Check(r.enables == 1 && r.lateEnables == 0, "HIGGS letting go switches it back on exactly once");
			Check(r.firstEnableStep == 2, "on the step HIGGS first says the hand is empty");
			Check(guard.Steps() == 2, "and the guard knows how many steps it took");
			Check(!guard.Active(), "and then nothing more is owed");
		}

		// HIGGS is already empty on the first step after the pickup.
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = Run(guard, 10, 1);
			Check(r.enables == 1 && r.firstEnableStep == 1, "a let-go on the first step switches it back on at once");
		}

		// HIGGS does not hold what is picked up.
		{
			Guard guard;
			Check(!guard.Begin(false, false), "a pickup of something HIGGS does not hold leaves the hand on");
			Check(!guard.Active(), "and nothing is owed");
			const Result r = Run(guard, 60, 0);
			Check(r.enables == 0 && r.lateEnables == 0, "and nothing is ever switched on");
		}

		// Another mod switched the hand off already: never this mod's to touch.
		{
			Guard guard;
			Check(!guard.Begin(true, true), "a hand something else switched off is not switched off again");
			Check(!guard.Active(), "and is not this mod's to switch back on");
			const Result r = Run(guard, 60, 0);
			Check(r.enables == 0 && r.lateEnables == 0, "so this mod never switches it back on");
			Check(!guard.Restore(), "not even before a load");
		}

		// A second pickup while the hand is still off from the first.
		{
			Guard guard;
			guard.Begin(true, false);
			Run(guard, 3, 0);
			Check(!guard.Begin(true, true), "a second pickup does not switch the hand off twice");
			Check(guard.Active(), "the hand is still this mod's to switch back on");
			Check(guard.Steps() == 0, "and the wait starts again");
			const Result r = Run(guard, 60, 5);
			Check(r.enables == 1 && r.firstEnableStep == 5, "HIGGS letting go after the second pickup switches it back on");
		}

		// HIGGS never lets go - a paused or stuck hand.
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = Run(guard, 200, 0);
			Check(r.lateEnables == 1 && r.enables == 0, "a hand HIGGS never lets go is switched back on anyway, once");
			Check(r.firstEnableStep == LimitStep(config), "at whichever limit comes first");
			Check(r.firstEnableStep <= config.maxSteps, "and never later than the step limit");
		}

		// Before a load, a hand still off is switched back on.
		{
			Guard guard;
			guard.Begin(true, false);
			Run(guard, 3, 0);
			Check(guard.Restore(), "a hand still off when a game loads is switched back on");
			Check(!guard.Active(), "and nothing is owed afterwards");
			const Result r = Run(guard, 60, 0);
			Check(r.enables == 0 && r.lateEnables == 0, "so it is not switched on a second time");
			Check(!guard.Restore(), "and a second restore does nothing");
		}

		// A guard nobody used.
		{
			Guard guard;
			const Result r = Run(guard, 60, 0);
			Check(r.enables == 0 && r.lateEnables == 0 && guard.Steps() == 0, "an unused guard does nothing and counts nothing");
		}

		// Other mods are still being shown the grip released when HIGGS lets go: the hand stays off
		// until that ends.
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = RunKeepOff(guard, 60, 2, 8);
			Check(r.enables == 1 && r.lateEnables == 0, "a hand kept off for a shown release is switched back on exactly once");
			Check(r.firstEnableStep == 9, "on the first step nothing keeps it off");
			Check(guard.KeptOff(), "and the guard says it was kept off");
		}

		// Still being shown at the limit: the limit wins, and it is a normal let-go.
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = RunKeepOff(guard, 200, 2, -1);
			Check(r.enables == 1 && r.lateEnables == 0, "a hand kept off past the limit is switched back on once, as a let-go");
			Check(r.firstEnableStep == LimitStep(config), "at the limit, never later");
		}

		// Kept off while HIGGS never lets go: still the late switch-on at the limit.
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = RunKeepOff(guard, 200, 0, -1);
			Check(r.lateEnables == 1 && r.enables == 0, "a hand HIGGS never lets go is switched on late even while kept off");
			Check(r.firstEnableStep == LimitStep(config), "at the same limit");
		}

		// Nothing keeps it off: exactly the plain behaviour.
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = RunKeepOff(guard, 60, 3, 0);
			Check(r.enables == 1 && r.firstEnableStep == 3, "with nothing keeping it off, HIGGS letting go switches it on at once");
			Check(!guard.KeptOff(), "and the guard does not say it was kept off");
		}

		// Kept off only while HIGGS still held on: nothing is delayed.
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = RunKeepOff(guard, 60, 5, 3);
			Check(r.enables == 1 && r.firstEnableStep == 5, "a keep-off that ended before HIGGS let go delays nothing");
			Check(!guard.KeptOff(), "and does not count as kept off");
		}

		// The next pickup starts clean.
		{
			Guard guard;
			guard.Begin(true, false);
			RunKeepOff(guard, 60, 2, 8);
			Check(guard.Begin(true, false), "the next pickup switches the hand off again");
			Check(!guard.KeptOff(), "and forgets that the last one was kept off");
		}

		// Kept off with no pickup in progress: nothing happens.
		{
			Guard guard;
			const Result r = RunKeepOff(guard, 60, 0, -1);
			Check(r.enables == 0 && r.lateEnables == 0, "keeping off a hand this mod never switched off does nothing");
		}

		// ---- test12: the hand stays off until the game has the weapon DRAWN ----------------------------
		// The 2026-09-23 test11 log: every LEFT pickup (23 of 23) drawn at +0.18-0.20s while the hand came
		// back at +0.11s, the blink's end - 70-85ms of HIGGS seeing an EMPTY hand with the weapon in it, then
		// a flip to a weapon hand ("my hand stutters", "a double vibration"). Every RIGHT pickup (18 of 18)
		// drawn at +0.04-0.08s, before the hand came back: no window, and far fewer stutters.
		{
			Check(WhyKeepOff(true, true, false) == KeepOff::kBlink, "the blink comes first, as before");
			Check(WhyKeepOff(true, true, true) == KeepOff::kBlink, "drawn or not");
			Check(WhyKeepOff(false, true, false) == KeepOff::kDraw, "blink over, the weapon in this hand not drawn yet: stay off");
			Check(WhyKeepOff(false, true, true) == KeepOff::kNo, "blink over and drawn: back on");
			Check(WhyKeepOff(false, false, false) == KeepOff::kNo, "nothing put in this hand: nothing to wait for");
		}
		{
			Guard guard;
			guard.Begin(true, false);
			const float dt = 1.0f / s_rate;
			const Result r = RunPickup(guard, 90, 3, 0.10f, 0.19f, true);
			const float backAt = r.firstEnableStep * dt;
			Check(r.enables == 1 && r.lateEnables == 0, "a left pickup: switched back on exactly once");
			Check(backAt >= 0.19f - 1e-4f && backAt < 0.19f + dt + 1e-4f,
				"on the first step the game has the weapon drawn - not at the blink's end, the 70-85ms empty-hand window gone");
			Check(guard.KeptOff() && guard.KeptOffForDraw(), "and the guard says it waited for the draw");
		}
		{
			Guard guard;
			guard.Begin(true, false);
			const float dt = 1.0f / s_rate;
			const Result r = RunPickup(guard, 90, 3, 0.10f, 0.05f, true);
			const float backAt = r.firstEnableStep * dt;
			Check(r.enables == 1 && backAt > 0.10f && backAt < 0.10f + dt + 1e-4f,
				"a right pickup, drawn before the blink ended: back on at the blink's end exactly as before");
			Check(guard.KeptOff() && !guard.KeptOffForDraw(), "kept off for the blink, not the draw");
		}
		{
			Guard guard;
			guard.Begin(true, false);
			const Result r = RunPickup(guard, 90, 3, 0.10f, -1.0f, true);
			Check(r.enables == 1 && r.lateEnables == 0 && r.firstEnableStep == LimitStep(config),
				"a draw that never comes: the limit still switches the hand back on, never later");
		}
		{
			Guard guard;
			guard.Begin(true, false);
			const float dt = 1.0f / s_rate;
			const Result r = RunPickup(guard, 90, 3, 0.10f, -1.0f, false);
			const float backAt = r.firstEnableStep * dt;
			Check(r.enables == 1 && backAt > 0.10f && backAt < 0.10f + dt + 1e-4f,
				"nothing put in the hand: back on at the blink's end, no draw to wait for");
			Check(!guard.KeptOffForDraw(), "and not said to have waited for one");
		}
		{
			Guard guard;
			guard.Begin(true, false);
			RunPickup(guard, 90, 3, 0.10f, 0.19f, true);
			guard.Begin(true, false);
			Check(!guard.KeptOffForDraw(), "the next pickup forgets that the last one waited for its draw");
		}
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
