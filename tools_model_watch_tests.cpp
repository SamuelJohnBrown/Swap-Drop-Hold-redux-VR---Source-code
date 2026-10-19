// Console tests for ModelWatchRules.h: when a dropped weapon's late model is hurried, and
// when the wait for it ends, without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_model_watch_tests.cpp /Fe:model_watch_tests.exe
//   model_watch_tests.exe
// Exit code 0 means every check passed.

#include "ModelWatchRules.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace SwapDropAndHoldRedux::ModelWatch;

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

	struct Event
	{
		Step step;
		float seconds;
	};

	float Frame()
	{
		return 1.0f / s_rate;
	}

	// Runs a watch at a fixed frame rate for `seconds`. The model is there on every frame at
	// or after modelAtSeconds; a negative modelAtSeconds means it never comes.
	std::vector<Event> Run(Watch& watch, const float modelAtSeconds, const float seconds)
	{
		const Config config{};
		std::vector<Event> events;
		const float dt = Frame();
		const int frames = static_cast<int>(seconds * s_rate + 0.5f);
		float t = 0.0f;
		for (int i = 0; i < frames; ++i)
		{
			t += dt;
			const bool hasModel = modelAtSeconds >= 0.0f && t >= modelAtSeconds - 0.0001f;
			const Step step = watch.Update(hasModel, dt, config);
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

	void TestNormalDropIsNeverHurried()
	{
		// Every drop in the logs but two: the model three or four frames later.
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, 0.035f, 1.0f);
		Check(IsSequence(events, { Step::kAppeared }), "a model 35ms late only appears");
		Check(!watch.Hurried(), "a model 35ms late is not hurried");
		Check(!watch.Active(), "the watch ends when the model appears");
		const int expectedFrames = static_cast<int>(std::ceil(0.035f * s_rate - 0.001f));
		Check(std::abs(watch.Frames() - expectedFrames) <= 1, "Frames is the frame the model came on");
	}

	void TestModelAlreadyThere()
	{
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, 0.0f, 0.5f);
		Check(IsSequence(events, { Step::kAppeared }), "a model already there appears");
		Check(watch.Frames() == 1, "a model already there appears on the first frame");
	}

	void TestJustInTimeIsNotHurried()
	{
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, 0.09f, 1.0f);
		Check(IsSequence(events, { Step::kAppeared }), "a model 90ms late only appears");
	}

	void TestLateModelIsHurriedOnceThenAppears()
	{
		// The Dwarven Mace in the report's log: no model for seconds.
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, 7.0f, 10.0f);
		Check(IsSequence(events, { Step::kHurry, Step::kAppeared }), "a model 7s late is hurried once, then appears");
		if (events.size() == 2)
		{
			Check(events[0].seconds >= 0.10f - 0.0001f && events[0].seconds < 0.10f + Frame() + 0.0001f,
				"the hurry comes on the first frame at or past 0.10s");
			Check(std::fabs(watch.HurrySeconds() - events[0].seconds) < 0.001f, "HurrySeconds is when the hurry came");
			Check(std::fabs(watch.Seconds() - events[1].seconds) < 0.001f, "Seconds is when the model came");
		}
		Check(watch.Hurried(), "Hurried reports the hurry");
		Check(!watch.Active(), "the watch ends when the late model appears");
	}

	void TestModelThatNeverComes()
	{
		Watch watch;
		watch.Begin();
		const std::vector<Event> events = Run(watch, -1.0f, 30.0f);
		Check(IsSequence(events, { Step::kHurry, Step::kGaveUp }), "a model that never comes is hurried once, then given up on");
		if (events.size() == 2)
		{
			Check(std::fabs(events[1].seconds - 20.0f) <= Frame() + 0.01f, "the watch gives up at 20s");
		}
		Check(!watch.Active(), "the watch ends when it gives up");
	}

	void TestEndedAndUnbegunWatchesDoNothing()
	{
		Watch never;
		Check(Run(never, -1.0f, 25.0f).empty() && !never.Active(), "a watch never begun does nothing");

		Watch appeared;
		appeared.Begin();
		Run(appeared, 0.03f, 0.5f);
		Check(Run(appeared, -1.0f, 25.0f).empty(), "a watch that saw its model does nothing more");

		Watch gaveUp;
		gaveUp.Begin();
		Run(gaveUp, -1.0f, 30.0f);
		Check(Run(gaveUp, 0.0f, 1.0f).empty(), "a watch that gave up does nothing more, even when the model comes");
	}

	void TestBeginStartsOver()
	{
		Watch watch;
		watch.Begin();
		Run(watch, -1.0f, 30.0f);
		watch.Begin();
		Check(watch.Active() && !watch.Hurried() && watch.Frames() == 0 && watch.Seconds() == 0.0f,
			"Begin forgets the last drop");
		const std::vector<Event> events = Run(watch, 3.0f, 5.0f);
		Check(IsSequence(events, { Step::kHurry, Step::kAppeared }), "the next late drop is hurried on its own");
	}
}

int main()
{
	for (const float rate : { 45.0f, 72.0f, 90.0f, 120.0f, 144.0f })
	{
		s_rate = rate;
		TestNormalDropIsNeverHurried();
		TestModelAlreadyThere();
		TestJustInTimeIsNotHurried();
		TestLateModelIsHurriedOnceThenAppears();
		TestModelThatNeverComes();
		TestEndedAndUnbegunWatchesDoNothing();
		TestBeginStartsOver();
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
