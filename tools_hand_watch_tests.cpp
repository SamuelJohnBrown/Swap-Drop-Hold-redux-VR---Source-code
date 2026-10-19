// Console tests for HandWatchRules.h: which frames of a watched HIGGS hand are worth a log
// line, and when the watch ends, without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_hand_watch_tests.cpp /Fe:hand_watch_tests.exe
//   hand_watch_tests.exe
// Exit code 0 means every check passed.

#include "HandWatchRules.h"

#include <cmath>
#include <cstdio>
#include <functional>

using namespace SwapDropAndHoldRedux::HandWatch;

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

	float Frame()
	{
		return 1.0f / s_rate;
	}

	int FrameIndex(const float t)
	{
		return static_cast<int>(t * s_rate + 0.5f);
	}

	struct Totals
	{
		int started = 0;
		int changed = 0;
		int suppressed = 0;
		int ended = 0;
		float endedAt = -1.0f;
		int actualChanges = 0; // state changes fed while the window was open
	};

	// Runs a window at a fixed frame rate for `seconds`, starting the clock at startAt and
	// taking each frame's snapshot from state(t).
	Totals Run(Window& window, const float seconds, const std::function<Snapshot(float)>& state, const float startAt = 0.0f)
	{
		const Config config{};
		Totals totals;
		const float dt = Frame();
		const int frames = static_cast<int>(seconds * s_rate + 0.5f);
		float t = startAt;
		bool haveLast = false;
		Snapshot last;
		for (int i = 0; i < frames; ++i)
		{
			t += dt;
			const Snapshot now = state(t);
			if (window.IsOpen())
			{
				if (haveLast && now != last)
				{
					++totals.actualChanges;
				}
				haveLast = true;
				last = now;
			}

			const Output out = window.Update(now, dt, config);
			totals.started += out.started ? 1 : 0;
			totals.changed += out.changed ? 1 : 0;
			totals.suppressed += out.suppressed;
			if (out.ended)
			{
				++totals.ended;
				totals.endedAt = t;
			}
		}
		return totals;
	}

	Snapshot Idle()
	{
		Snapshot s;
		s.canGrab = true;
		s.grabbable = true;
		return s;
	}

	Snapshot IdleAlways(float)
	{
		return Idle();
	}

	void TestOpeningLogsTheStateOnce()
	{
		Window window;
		window.Open(Config{});
		const Totals totals = Run(window, 5.0f, IdleAlways);
		Check(totals.started == 1, "an open watch logs its starting state once");
		Check(totals.changed == 0 && totals.suppressed == 0, "a hand that never changes logs nothing more");
		Check(window.IsOpen(), "the watch is still open 5s in");
	}

	void TestOneChangeIsOneLine()
	{
		// The shape of the report: HIGGS picks the dropped weapon up again a second later.
		Window window;
		window.Open(Config{});
		const Totals totals = Run(window, 3.0f, [](const float t) {
			Snapshot s = Idle();
			if (t >= 1.0f)
			{
				s.canGrab = false;
				s.grabbable = false;
				s.holding = true;
				s.heldRefID = 0xFF001F1Cu;
			}
			return s;
		});
		Check(totals.started == 1 && totals.changed == 1, "one change after the start is one change line");
		Check(totals.changed + totals.suppressed == totals.actualChanges, "that change is accounted for");
	}

	void TestFlickerIsCappedAndCounted()
	{
		// The grip read flickering every frame for half a second.
		Window window;
		window.Open(Config{});
		const Totals totals = Run(window, 3.0f, [](const float t) {
			Snapshot s = Idle();
			if (t > 0.2f && t < 0.7f)
			{
				s.gripClosed = FrameIndex(t) % 2 == 0;
			}
			return s;
		});
		const Config config{};
		Check(totals.started + totals.changed <= config.maxLinesPerSecond, "no more than the cap is logged in one second");
		Check(totals.suppressed > 0, "the flicker past the cap is counted");
		Check(totals.changed + totals.suppressed == totals.actualChanges, "every change is logged or counted, none lost");
	}

	void TestTheWatchEndsOnce()
	{
		Window window;
		window.Open(Config{});
		const Totals totals = Run(window, 20.0f, IdleAlways);
		Check(totals.ended == 1, "the watch ends exactly once");
		Check(std::fabs(totals.endedAt - 15.0f) <= Frame() + 0.01f, "it ends 15s after it opened");
		Check(!window.IsOpen(), "and is closed afterwards");

		const Totals after = Run(window, 5.0f, [](const float t) {
			Snapshot s = Idle();
			s.gripClosed = FrameIndex(t) % 2 == 0;
			return s;
		}, 20.0f);
		Check(after.started + after.changed + after.suppressed + after.ended == 0,
			"a closed watch logs nothing, even while the hand changes");
	}

	void TestOpeningAgainExtends()
	{
		// A second drop ten seconds into the watch.
		Window window;
		window.Open(Config{});
		Run(window, 10.0f, IdleAlways);
		window.Open(Config{});
		const Totals totals = Run(window, 20.0f, IdleAlways, 10.0f);
		Check(totals.started == 0, "extending an open watch does not log a second start");
		Check(totals.ended == 1 && std::fabs(totals.endedAt - 25.0f) <= Frame() + 0.01f,
			"it ends 15s after the second opening");
	}

	void TestReopeningAfterTheEndStartsFresh()
	{
		Window window;
		window.Open(Config{});
		Run(window, 16.0f, IdleAlways);
		window.Open(Config{});
		const Totals totals = Run(window, 1.0f, IdleAlways, 16.0f);
		Check(totals.started == 1, "a watch opened after the last one ended logs its start again");
		Check(window.Seconds() < 1.0f + Frame() + 0.01f, "and counts its seconds from the new opening");
	}

	void TestChangesHeldBackAtTheEndAreReported()
	{
		// Flicker in the watch's last half second, so the cap is still holding lines back when
		// the watch runs out.
		Window window;
		window.Open(Config{});
		const Totals totals = Run(window, 16.0f, [](const float t) {
			Snapshot s = Idle();
			if (t > 14.5f && t < 14.9f)
			{
				s.triggerClosed = FrameIndex(t) % 2 == 0;
			}
			return s;
		});
		Check(totals.suppressed > 0, "the flicker at the end is capped");
		Check(totals.changed + totals.suppressed == totals.actualChanges,
			"changes held back when the watch runs out are still reported");
	}
}

int main()
{
	for (const float rate : { 72.0f, 90.0f, 120.0f, 144.0f })
	{
		s_rate = rate;
		TestOpeningLogsTheStateOnce();
		TestOneChangeIsOneLine();
		TestFlickerIsCappedAndCounted();
		TestTheWatchEndsOnce();
		TestOpeningAgainExtends();
		TestReopeningAfterTheEndStartsFresh();
		TestChangesHeldBackAtTheEndAreReported();
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
