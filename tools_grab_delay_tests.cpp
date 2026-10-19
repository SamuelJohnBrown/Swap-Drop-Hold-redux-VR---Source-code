// Console tests for GrabDelayRules.h: how long a HIGGS-held weapon stays a loose object before it
// is picked up and equipped, without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_grab_delay_tests.cpp /Fe:grab_delay_tests.exe
//   grab_delay_tests.exe
// Exit code 0 means every check passed.

#include "GrabDelayRules.h"
#include "GrabGripRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux::GrabDelay;

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
		return std::fabs(a - b) < 0.0001f;
	}

	constexpr std::uint32_t kSword = 0xFF001ED9;
	constexpr std::uint32_t kAxe = 0xFF002A10;
}

int main()
{
	// The slider's range.
	Check(Near(Clamp(-1.0f), 0.0f), "a negative delay is no delay");
	Check(Near(Clamp(0.0f), 0.0f), "zero stays zero");
	Check(Near(Clamp(1.5f), 1.5f), "a delay in range is kept");
	Check(Near(Clamp(10.0f), 10.0f), "the top of the range is kept");
	Check(Near(Clamp(15.0f), 10.0f), "an ini value past the slider is pulled back to 10");

	// Zero is today's behaviour: nothing waits.
	Check(!Waits(0.0f), "zero: a grabbed weapon equips at once, as before this setting");
	Check(!Waits(-0.5f), "a negative delay does not wait either");
	Check(Waits(0.05f), "any real delay waits");

	// An idle hand does nothing.
	{
		Hold hold;
		Check(!hold.Active(), "a new hold is idle");
		Check(hold.Update(kSword, 0.5f, 1.0f) == Step::kIdle, "an idle hold ignores what the hand carries");
		Check(!hold.Active(), "and stays idle");
	}

	// A grab with no reference is not a grab.
	{
		Hold hold;
		hold.Begin(0);
		Check(!hold.Active(), "Begin(0) starts nothing");
	}

	// Held for the whole delay: equips on the step that reaches it, and only once.
	{
		Hold hold;
		hold.Begin(kSword);
		Check(hold.Active(), "the grab starts the clock");
		Check(hold.Update(kSword, 0.25f, 1.0f) == Step::kWaiting, "0.25s of 1.00s: still loose");
		Check(hold.Update(kSword, 0.25f, 1.0f) == Step::kWaiting, "0.50s: still loose");
		Check(hold.Update(kSword, 0.25f, 1.0f) == Step::kWaiting, "0.75s: still loose");
		Check(Near(hold.HeldSeconds(), 0.75f), "the held time adds up");
		Check(hold.Update(kSword, 0.25f, 1.0f) == Step::kEquip, "1.00s: now it is picked up");
		Check(!hold.Active(), "the equip ends the hold");
		Check(hold.Update(kSword, 0.25f, 1.0f) == Step::kIdle, "and it never equips twice");
	}

	// Let go early: the weapon stays a loose object.
	{
		Hold hold;
		hold.Begin(kSword);
		Check(hold.Update(kSword, 0.40f, 1.0f) == Step::kWaiting, "held 0.40s");
		Check(hold.Update(0, 0.01f, 1.0f) == Step::kAbandoned, "let go at 0.40s of 1.00s: abandoned");
		Check(!hold.Active(), "an abandoned hold is over");
		Check(hold.Update(kSword, 5.0f, 1.0f) == Step::kIdle, "grabbing it again later needs a new grab");
	}

	// The hand is now holding something else.
	{
		Hold hold;
		hold.Begin(kSword);
		Check(hold.Update(kAxe, 0.10f, 1.0f) == Step::kAbandoned, "a different weapon in the hand abandons the first");
	}

	// HIGGS repeats its grab callback for the weapon already being timed: the clock keeps running.
	{
		Hold hold;
		hold.Begin(kSword);
		hold.Update(kSword, 0.60f, 1.0f);
		hold.Begin(kSword);
		Check(Near(hold.HeldSeconds(), 0.60f), "a repeat grab of the same weapon keeps its time");
		Check(hold.Update(kSword, 0.40f, 1.0f) == Step::kEquip, "so it equips at 1.00s, not 1.60s");
	}

	// A grab of a different weapon starts over.
	{
		Hold hold;
		hold.Begin(kSword);
		hold.Update(kSword, 0.60f, 1.0f);
		hold.Begin(kAxe);
		Check(Near(hold.HeldSeconds(), 0.0f), "a new weapon starts from zero");
		Check(hold.RefID() == kAxe, "and is the one being timed");
	}

	// A re-grab after letting go starts over.
	{
		Hold hold;
		hold.Begin(kSword);
		hold.Update(kSword, 0.90f, 1.0f);
		hold.Update(0, 0.01f, 1.0f);
		hold.Begin(kSword);
		Check(Near(hold.HeldSeconds(), 0.0f), "letting go loses the time held");
		Check(hold.Update(kSword, 0.50f, 1.0f) == Step::kWaiting, "0.50s after the re-grab is still loose");
	}

	// The menu changes the delay mid-hold.
	{
		Hold hold;
		hold.Begin(kSword);
		hold.Update(kSword, 1.0f, 2.0f);
		Check(hold.Update(kSword, 0.01f, 0.5f) == Step::kEquip, "lowered below the time already held: equips now");
	}
	{
		Hold hold;
		hold.Begin(kSword);
		Check(hold.Update(kSword, 0.01f, 0.0f) == Step::kEquip, "turned off mid-hold: what you hold equips at once");
	}

	// A hitch that jumps past the delay in one step.
	{
		Hold hold;
		hold.Begin(kSword);
		Check(hold.Update(kSword, 2.5f, 1.0f) == Step::kEquip, "one long step past the delay equips");
	}

	// Cancel.
	{
		Hold hold;
		hold.Begin(kSword);
		hold.Cancel();
		Check(!hold.Active(), "Cancel ends the hold");
		Check(hold.Update(kSword, 5.0f, 1.0f) == Step::kIdle, "and nothing equips after it");
	}

	// M3.0.0-test15: a weapon it would be a crime to take - the red "Steal" prompt - waits longer.
	{
		Check(Near(DelayFor(0.0f, 10.0f, false), 0.0f), "yours: no wait unless Hold before equipping says so");
		Check(Near(DelayFor(0.0f, 10.0f, true), 10.0f), "not yours: held 10 s before it is taken");
		Check(Near(DelayFor(3.0f, 10.0f, true), 10.0f), "the longer of the two delays wins");
		Check(Near(DelayFor(8.0f, 5.0f, true), 8.0f), "even when that is the ordinary delay");
		Check(Near(DelayFor(2.0f, 0.0f, true), 2.0f), "0 turns the stolen wait off: the ordinary delay stands");
		Check(Near(DelayFor(0.0f, 45.0f, true), kMaxStolenSeconds), "clamped to its own top");
		Check(Near(DelayFor(0.0f, -3.0f, true), 0.0f), "and never below zero");
		Check(kMaxStolenSeconds >= 10.0f, "the stolen top leaves room for 10 s");
		Check(kMaxStolenSeconds + 5.0f < SwapDropAndHoldRedux::GrabGrip::kMaxAgeSeconds,
			"the longest stolen wait still leaves the grab's measured grip fresh enough to send");
	}
	{
		Hold hold;
		hold.Begin(kSword, true);
		Check(hold.OffLimits(), "a hold remembers the weapon is not yours");
		hold.Begin(kSword, false);
		Check(hold.OffLimits(), "a repeat grab of the same weapon keeps what the first one found");
		Hold other;
		other.Begin(kAxe);
		Check(!other.OffLimits(), "a plain Begin is a weapon that is yours");
		const float delay = DelayFor(0.0f, 10.0f, hold.OffLimits());
		Check(hold.Update(kSword, 9.0f, delay) == Step::kWaiting, "still loose at 9 of 10 s");
		Check(hold.Update(kSword, 1.5f, delay) == Step::kEquip, "taken after 10 s");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
