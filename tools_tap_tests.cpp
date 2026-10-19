// Console tests for GripGestures.h: the double-tap and chord rules, without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_tap_tests.cpp /Fe:tap_tests.exe
//   tap_tests.exe
// Exit code 0 means every check passed.

#include "GripGestures.h"

#include <cstdio>
#include <vector>

using namespace SwapDropAndHoldRedux::Gestures;

namespace
{
	int s_checks = 0;
	int s_failures = 0;
	float s_rate = 0.0f;

	void Check(const bool ok, const char* what)
	{
		++s_checks;
		if (!ok)
		{
			++s_failures;
			std::printf("FAIL at %.0f Hz: %s\n", s_rate, what);
		}
	}

	// A stretch of time with the grip closed or open.
	struct Span
	{
		bool closed;
		float seconds;
	};

	// Feeds a grip timeline to the detector at a fixed frame rate and returns the time
	// of every frame on which it said to let go.
	std::vector<float> Run(
		DoubleTapDetector& detector,
		const std::vector<Span>& spans,
		const DoubleTapConfig& config = DoubleTapConfig{})
	{
		std::vector<float> fired;
		const float dt = 1.0f / s_rate;
		float t = 0.0f;
		for (const Span& span : spans)
		{
			const int frames = static_cast<int>(span.seconds * s_rate + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				t += dt;
				if (detector.Update(span.closed, dt, config))
				{
					fired.push_back(t);
				}
			}
		}
		return fired;
	}

	// The same timeline, recording every frame Released() said the grip had just been let go.
	std::vector<float> RunReleases(
		DoubleTapDetector& detector,
		const std::vector<Span>& spans,
		const DoubleTapConfig& config = DoubleTapConfig{})
	{
		std::vector<float> released;
		const float dt = 1.0f / s_rate;
		float t = 0.0f;
		for (const Span& span : spans)
		{
			const int frames = static_cast<int>(span.seconds * s_rate + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				t += dt;
				detector.Update(span.closed, dt, config);
				if (detector.Released())
				{
					released.push_back(t);
				}
			}
		}
		return released;
	}

	// M3.0.0-test14: Simple mode's hand switch fires on one opening of a held grip.
	void TestReleaseFiresOnceAfterTheDebounce()
	{
		DoubleTapDetector d;
		d.Bind(true);
		const std::vector<float> r = RunReleases(d, { { true, 0.5f }, { false, 0.5f } });
		Check(r.size() == 1, "one opening of a held grip is one release");
		Check(!r.empty() && r[0] > 0.549f && r[0] < 0.62f, "reported once the flicker filter has passed");
	}

	void TestTheGrabPressCountsForARelease()
	{
		DoubleTapDetector d;
		d.Bind(true);
		Check(RunReleases(d, { { false, 0.3f } }).size() == 1, "the squeeze that took hold of the weapon ending is a release");
	}

	void TestNoReleaseWithoutAHold()
	{
		DoubleTapDetector d;
		d.Bind(false);
		Check(RunReleases(d, { { false, 1.0f } }).empty(), "a hand never closed has nothing to release");
	}

	void TestFlickerIsNoRelease()
	{
		DoubleTapDetector d;
		d.Bind(true);
		Check(RunReleases(d, { { true, 0.3f }, { false, 0.03f }, { true, 0.3f } }).empty(), "a 30 ms flicker is not a release");
	}

	void TestEveryOpeningIsItsOwnRelease()
	{
		DoubleTapDetector d;
		d.Bind(true);
		Check(RunReleases(d, { { true, 0.2f }, { false, 0.2f }, { true, 0.2f }, { false, 0.2f } }).size() == 2,
			"two openings, two releases");
	}

	void TestRaisedDebounceDelaysTheRelease()
	{
		DoubleTapConfig config;
		config.releaseGraceSeconds = 0.15f;
		DoubleTapDetector d;
		d.Bind(true);
		Check(RunReleases(d, { { false, 0.10f } }, config).empty(), "0.10 s open under a 0.15 s debounce is not yet a release");
		DoubleTapDetector e;
		e.Bind(true);
		Check(RunReleases(e, { { false, 0.20f } }, config).size() == 1, "0.20 s is");
	}

	void TestTwoQuickTapsDropOnTheSecondRelease()
	{
		DoubleTapDetector detector;
		detector.Bind(false);
		const std::vector<float> fired = Run(detector, {
			{ false, 0.20f }, { true, 0.10f }, { false, 0.12f }, { true, 0.10f }, { false, 0.30f } });
		Check(fired.size() == 1, "two quick taps drop exactly once");
		// The second press ends at 0.52s; the drop must come after it, never during it.
		Check(!fired.empty() && fired[0] > 0.52f + 0.03f, "the drop waits for the second release");
		Check(!fired.empty() && fired[0] < 0.52f + 0.12f, "and does not wait much longer than the flicker filter");
	}

	void TestFlickerInsideATapIsOneTap()
	{
		DoubleTapDetector detector;
		detector.Bind(false);
		// One press with a 33ms flicker in the middle of it. Read raw, that is two taps.
		const std::vector<float> fired = Run(detector, {
			{ false, 0.20f }, { true, 0.05f }, { false, 0.033f }, { true, 0.05f }, { false, 0.60f } });
		Check(fired.empty(), "a flicker inside one tap is not a second tap");
	}

	void TestFlickerInsideAHoldIsNotATap()
	{
		DoubleTapDetector detector;
		detector.Bind(false);
		const std::vector<float> fired = Run(detector, {
			{ true, 0.60f }, { false, 0.033f }, { true, 0.60f }, { false, 0.10f }, { true, 0.10f }, { false, 0.60f } });
		Check(fired.empty(), "a flicker in a long hold, then one tap, does not drop");
	}

	void TestTheGrabPressDoesNotCount()
	{
		DoubleTapDetector detector;
		detector.Bind(true); // the squeeze that took hold of the weapon
		std::vector<float> fired = Run(detector, {
			{ true, 0.10f }, { false, 0.10f }, { true, 0.10f }, { false, 0.60f } });
		Check(fired.empty(), "the grab press plus one tap is not a double tap");

		fired = Run(detector, { { true, 0.10f }, { false, 0.10f }, { true, 0.10f }, { false, 0.30f } });
		Check(fired.size() == 1, "a real double tap after the grab still drops");
	}

	void TestSlowTapsDoNothing()
	{
		DoubleTapDetector detector;
		detector.Bind(false);
		const std::vector<float> fired = Run(detector, {
			{ false, 0.20f }, { true, 0.10f }, { false, 0.45f }, { true, 0.10f }, { false, 0.60f } });
		Check(fired.empty(), "taps further apart than the window do nothing");
	}

	void TestHoldThenTapDoesNothing()
	{
		DoubleTapDetector detector;
		detector.Bind(false);
		const std::vector<float> fired = Run(detector, {
			{ true, 1.00f }, { false, 0.10f }, { true, 0.10f }, { false, 0.60f } });
		Check(fired.empty(), "a long hold followed by one tap is not a double tap");
	}

	void TestHeldSecondPressDoesNothing()
	{
		DoubleTapDetector detector;
		detector.Bind(false);
		const std::vector<float> fired = Run(detector, {
			{ false, 0.20f }, { true, 0.10f }, { false, 0.10f }, { true, 1.00f }, { false, 0.60f } });
		Check(fired.empty(), "a second press that is held becomes a hold, not a drop");
	}

	void TestRaisedReleaseDebounce()
	{
		DoubleTapConfig config;
		config.releaseGraceSeconds = 0.20f;

		DoubleTapDetector detector;
		detector.Bind(false);
		std::vector<float> fired = Run(detector, {
			{ false, 0.20f }, { true, 0.10f }, { false, 0.10f }, { true, 0.10f }, { false, 0.40f } }, config);
		Check(fired.size() == 1, "a raised release debounce still allows a double tap");
		Check(!fired.empty() && fired[0] >= 0.50f + 0.18f, "and the drop waits for the whole debounce");

		DoubleTapDetector kept;
		kept.Bind(false);
		fired = Run(kept, {
			{ false, 0.20f }, { true, 0.10f }, { false, 0.10f }, { true, 0.10f }, { false, 0.10f },
			{ true, 0.50f }, { false, 0.60f } }, config);
		Check(fired.empty(), "closing again before the debounce ends keeps the weapon");
	}

	void TestChordTapRules()
	{
		Check(ChordTapCounts(true, false, false), "empty hand with grip held counts");
		Check(!ChordTapCounts(false, false, false), "empty hand without grip does not count");
		Check(ChordTapCounts(true, true, true), "a grip-held weapon counts (its grip is held)");
		Check(!ChordTapCounts(false, true, true), "a grip-bound weapon with the grip open does not count");
		Check(ChordTapCounts(false, true, false), "a weapon grip is not holding counts without grip");
		Check(ChordTapCounts(true, true, false), "a weapon grip is not holding counts with grip too");
	}

	void TestChordRuns()
	{
		const float dt = 1.0f / 90.0f;
		const float window = 0.35f;
		int clicks = 0;
		bool qualified = false;

		// Advances the run by some seconds; returns true if it ended in that time.
		auto advance = [&](ChordRun& run, const float seconds) {
			const int frames = static_cast<int>(seconds * 90.0f + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				if (run.Tick(dt, window, clicks, qualified))
				{
					return true;
				}
			}
			return false;
		};

		ChordRun run;
		run.Click(true);
		Check(!advance(run, 0.20f), "a run stays open inside the window");
		run.Click(true);
		Check(!advance(run, 0.20f), "a run stays open after a second tap");
		run.Click(true);
		Check(advance(run, 0.40f) && clicks == 3 && qualified, "three counted taps end as a qualified run of 3");

		run.Click(true);
		advance(run, 0.20f);
		run.Click(false);
		advance(run, 0.20f);
		run.Click(true);
		Check(advance(run, 0.40f) && clicks == 3 && !qualified, "one tap that did not count spoils the run");

		run.Click(true);
		Check(advance(run, 0.40f) && clicks == 1, "a gap longer than the window ends a run");
		run.Click(true);
		run.Click(true);
		Check(advance(run, 0.40f) && clicks == 2 && qualified, "the next run starts from zero");

		run.Click(true);
		run.Click(true);
		run.Cancel();
		Check(!advance(run, 0.60f), "a cancelled run never resolves");
	}
}

int main()
{
	const float rates[] = { 90.0f, 72.0f, 45.0f };
	for (const float rate : rates)
	{
		s_rate = rate;
		TestTwoQuickTapsDropOnTheSecondRelease();
		TestFlickerInsideATapIsOneTap();
		TestFlickerInsideAHoldIsNotATap();
		TestTheGrabPressDoesNotCount();
		TestSlowTapsDoNothing();
		TestHoldThenTapDoesNothing();
		TestHeldSecondPressDoesNothing();
		TestRaisedReleaseDebounce();
		TestReleaseFiresOnceAfterTheDebounce();
		TestTheGrabPressCountsForARelease();
		TestNoReleaseWithoutAHold();
		TestFlickerIsNoRelease();
		TestEveryOpeningIsItsOwnRelease();
		TestRaisedDebounceDelaysTheRelease();
	}

	s_rate = 90.0f;
	TestChordTapRules();
	TestChordRuns();

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
