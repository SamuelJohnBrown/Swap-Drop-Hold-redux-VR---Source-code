// Console tests for DrawWatchRules.h: what to do while a grab-equipped weapon has not actually
// come out of its sheath yet - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_draw_watch_tests.cpp /Fe:draw_watch_tests.exe
//   draw_watch_tests.exe
// Exit code 0 means every check passed.

#include "DrawWatchRules.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SwapDropAndHoldRedux::DrawWatch;

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

	struct Event
	{
		Step step;
		float seconds;
	};

	float Frame()
	{
		return 1.0f / s_rate;
	}

	// Runs a watch at a fixed frame rate for `seconds`. The weapon is drawn and settled on every
	// frame at or after drawnAtSeconds; a negative drawnAtSeconds means it never comes out.
	std::vector<Event> Run(Watch& watch, const float drawnAtSeconds, const float seconds)
	{
		const Config config{};
		std::vector<Event> events;
		const float dt = Frame();
		const int frames = static_cast<int>(seconds * s_rate + 0.5f);
		float t = 0.0f;
		for (int i = 0; i < frames; ++i)
		{
			t += dt;
			const bool drawn = drawnAtSeconds >= 0.0f && t >= drawnAtSeconds - 0.0001f;
			const Step step = watch.Update(drawn, dt, config);
			if (step != Step::kNone)
			{
				events.push_back({ step, t });
			}
		}
		return events;
	}

	bool IsSequence(const std::vector<Event>& events, const std::vector<Step>& steps)
	{
		if (events.size() != steps.size())
		{
			return false;
		}
		for (size_t i = 0; i < steps.size(); ++i)
		{
			if (events[i].step != steps[i])
			{
				return false;
			}
		}
		return true;
	}

	int CountOf(const std::vector<Event>& events, const Step step)
	{
		int n = 0;
		for (size_t i = 0; i < events.size(); ++i)
		{
			if (events[i].step == step)
			{
				++n;
			}
		}
		return n;
	}

	// ---- the happy path: the draw lands, and nothing is ever re-requested --------------------

	void TestDrawLandsAtOnce()
	{
		// The weapon was already out - the overwhelmingly common case, and the one that must
		// never cost a second DrawSheatheWeapon call. Re-requesting a draw that already
		// succeeded is what spammed equip audio when dual wielding.
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, 0.0f, 1.0f);
		Check(IsSequence(events, { Step::kDrawn }), "a weapon already out only reports drawn");
		Check(watch.Retries() == 0, "and is never asked to draw again");
		Check(!watch.Active(), "the watch ends the moment the weapon is out");
	}

	void TestDrawLandsBeforeTheFirstRetry()
	{
		// A draw the engine accepted takes a few frames of animation. That is not a swallowed
		// draw and must not be treated as one.
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, 0.10f, 1.0f);
		Check(IsSequence(events, { Step::kDrawn }), "a draw that lands in 0.10s only reports drawn");
		Check(watch.Retries() == 0, "a draw still settling is not re-requested");
	}

	// ---- the bug: the draw was swallowed -----------------------------------------------------

	void TestSwallowedDrawIsRetriedThenLands()
	{
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, 0.40f, 2.0f);
		Check(IsSequence(events, { Step::kRetryDraw, Step::kDrawn }),
			"a swallowed draw is asked for again, then lands");
		Check(watch.Retries() == 1, "exactly one re-request was needed");
		Check(!watch.Active(), "and the watch ends there");
	}

	void TestTheFirstRetryWaitsForRetryAfterSeconds()
	{
		const Config config{};
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, -1.0f, 1.0f);
		Check(!events.empty(), "a draw that never lands is eventually re-requested");
		if (!events.empty())
		{
			Check(events[0].step == Step::kRetryDraw, "the first thing that happens is a re-request");
			Check(events[0].seconds >= config.retryAfterSeconds - Frame() - 0.0001f,
				"and not before the engine has been given time to answer");
			Check(events[0].seconds <= config.retryAfterSeconds + Frame() + 0.0001f,
				"and not long after either");
		}
	}

	void TestRetriesAreSpacedOut()
	{
		// Back-to-back re-requests inside one frame are what the crossbow path does today by
		// building its wait out of task requeues. Real seconds, or the engine never gets a
		// chance to answer the first one.
		const Config config{};
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, -1.0f, 3.0f);
		bool spaced = true;
		float previous = -1.0f;
		for (size_t i = 0; i < events.size(); ++i)
		{
			if (events[i].step != Step::kRetryDraw)
			{
				continue;
			}
			if (previous >= 0.0f && events[i].seconds - previous < config.retryEverySeconds - Frame() - 0.0001f)
			{
				spaced = false;
			}
			previous = events[i].seconds;
		}
		Check(spaced, "no two re-requests land closer together than the interval");
	}

	void TestNeverMoreThanMaxRetries()
	{
		const Config config{};
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, -1.0f, 4.0f);
		Check(CountOf(events, Step::kRetryDraw) <= config.maxRetries,
			"a weapon that refuses to come out is not asked for ever");
		Check(watch.Retries() <= config.maxRetries, "and the count agrees");
	}

	void TestNeverDrawnGivesUp()
	{
		const Config config{};
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, -1.0f, config.giveUpSeconds + 1.0f);
		Check(!events.empty() && events.back().step == Step::kGaveUp,
			"a weapon that never comes out ends in giving up");
		Check(CountOf(events, Step::kGaveUp) == 1, "and only once");
		Check(!watch.Active(), "the watch is over after giving up");
		Check(CountOf(events, Step::kDrawn) == 0, "and it never claims the weapon was drawn");
	}

	void TestRetriesStopButTheWatchKeepsLooking()
	{
		// Out of re-requests is not out of hope: the engine may still finish the draw it
		// accepted, and the log should say so rather than report a failure that did not happen.
		const Config config{};
		Watch watch;
		watch.Begin();
		const float afterAllRetries = config.retryAfterSeconds + config.retryEverySeconds * config.maxRetries + 0.5f;
		const std::vector<Event> events = Run(watch, afterAllRetries, config.giveUpSeconds);
		Check(!events.empty() && events.back().step == Step::kDrawn,
			"a weapon that comes out after the last re-request still reports drawn");
		Check(CountOf(events, Step::kGaveUp) == 0, "and never gives up");
	}

	// ---- a watch that is not running does nothing --------------------------------------------

	void TestUnbegunWatchIsInert()
	{
		Watch watch;
		Check(!watch.Active(), "a fresh watch is not running");
		const std::vector<Event> events = Run(watch, -1.0f, 2.0f);
		Check(events.empty(), "and never asks for anything");
		Check(watch.Retries() == 0, "nor counts a re-request");
	}

	void TestFinishedWatchIsInert()
	{
		Watch watch;
		watch.Begin();
		Run(watch, 0.0f, 0.5f);
		Check(!watch.Active(), "the watch ended when the weapon came out");
		const std::vector<Event> events = Run(watch, -1.0f, 2.0f);
		Check(events.empty(), "a finished watch never speaks again");
	}

	void TestBeginStartsOver()
	{
		const Config config{};
		Watch watch;
		watch.Begin();
		Run(watch, -1.0f, 2.0f);
		const int retriesBefore = watch.Retries();
		Check(retriesBefore > 0, "the first pickup used some re-requests");

		watch.Begin();
		Check(watch.Active(), "a second pickup starts the watch again");
		Check(watch.Retries() == 0, "with its re-requests back at zero");
		Check(watch.Seconds() == 0.0f, "and its clock back at zero");

		const std::vector<Event> events = Run(watch, 0.40f, 2.0f);
		Check(IsSequence(events, { Step::kRetryDraw, Step::kDrawn }),
			"and behaves exactly as it did the first time");
		Check(watch.Retries() == 1, "using its own budget, not the last pickup's");
		(void)config;
	}

	void TestClockCounts()
	{
		Watch watch;
		watch.Begin();
		Run(watch, 0.40f, 2.0f);
		Check(std::fabs(watch.Seconds() - 0.40f) < 2.0f * Frame() + 0.0001f,
			"Seconds is when the weapon came out");
		const int expectedFrames = static_cast<int>(std::ceil(0.40f * s_rate - 0.001f));
		Check(std::abs(watch.Frames() - expectedFrames) <= 1, "Frames is the frame it came out on");
	}
}

int main()
{
	for (const float rate : { 45.0f, 72.0f, 90.0f, 120.0f, 144.0f })
	{
		s_rate = rate;
		TestDrawLandsAtOnce();
		TestDrawLandsBeforeTheFirstRetry();
		TestSwallowedDrawIsRetriedThenLands();
		TestTheFirstRetryWaitsForRetryAfterSeconds();
		TestRetriesAreSpacedOut();
		TestNeverMoreThanMaxRetries();
		TestNeverDrawnGivesUp();
		TestRetriesStopButTheWatchKeepsLooking();
		TestUnbegunWatchIsInert();
		TestFinishedWatchIsInert();
		TestBeginStartsOver();
		TestClockCounts();
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
