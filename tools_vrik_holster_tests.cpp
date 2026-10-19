// Console tests for VrikHolsterRules.h: the rules for the holster API VRIK added in build 80700,
// checked without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_vrik_holster_tests.cpp /Fe:vrik_holster_tests.exe
//   vrik_holster_tests.exe
// Exit code 0 means every check passed.

#include "VrikHolsterRules.h"

#include <cstdio>

using namespace SwapDropAndHoldRedux::VrikHolster;

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

	bool Parses(const char* eventName, const char* strArg, const float numArg, Event& out)
	{
		out = Event{};
		return ParseEvent(eventName, strArg, numArg, out);
	}
}

int main()
{
	// Build 80700 appended the holster functions to VRIK's interface.
	Check(HasApi(true, 80700), "build 80700 has the holster API");
	Check(HasApi(true, 90000), "a later build has it too");
	Check(!HasApi(true, 80699), "build 80699 does not");
	Check(!HasApi(true, 80400), "0.8.4, the oldest VRIK this mod accepts, does not");
	Check(!HasApi(false, 80700), "no interface means no API, whatever the number");

	// vrikslots.ini numbers its slots 1 to 14.
	Check(IsValidSlot(1) && IsValidSlot(14), "slots 1 and 14 are slots");
	Check(!IsValidSlot(0) && !IsValidSlot(-1) && !IsValidSlot(15), "0, -1 and 15 are not");

	// VRIK calls the game's right hand Primary, so its secondary hand is the game's left.
	Check(SecondaryHandFor(true), "the game's left hand is VRIK's secondary hand");
	Check(!SecondaryHandFor(false), "the game's right hand is VRIK's primary hand");

	// VRIK_Holstered / VRIK_Unholstered, as SendHolsterModEvent in _vrik_qust_system.psc sends them.
	Event e;
	Check(Parses("VRIK_Holstered", "Primary|0x00012EB7", 2.0f, e), "a holstered Iron Sword parses");
	Check(e.holstered && e.primary && e.formID == 0x00012EB7 && e.slot == 2,
		"and carries holstered, Primary, the form and slot 2");
	Check(Parses("VRIK_Unholstered", "Secondary|0xFE123ABC", 14.0f, e), "a draw of a light plugin's form parses");
	Check(!e.holstered && !e.primary && e.formID == 0xFE123ABC && e.slot == 14,
		"and carries drawn, Secondary, the form and slot 14");
	Check(Parses("VRIK_Holstered", "Primary|0xfe123abc", 1.0f, e) && e.formID == 0xFE123ABC, "lower-case hex parses");
	Check(Parses("VRIK_Holstered", "Primary|0XFE123ABC", 1.0f, e) && e.formID == 0xFE123ABC, "an upper-case 0X parses");
	Check(Parses("VRIK_Holstered", "Primary|0x12EB7", 13.9999f, e) && e.slot == 14, "a slot a hair under 14 is 14");

	Check(!Parses("SDHR_MenuAction", "Primary|0x00012EB7", 2.0f, e), "another mod's event is not VRIK's");
	Check(!Parses("vrik_holstered", "Primary|0x00012EB7", 2.0f, e), "event names are matched exactly");
	Check(!Parses(nullptr, "Primary|0x00012EB7", 2.0f, e), "no event name");
	Check(!Parses("VRIK_Holstered", nullptr, 2.0f, e), "no strArg");
	Check(!Parses("VRIK_Holstered", "", 2.0f, e), "an empty strArg");
	Check(!Parses("VRIK_Holstered", "Primary", 2.0f, e), "no separator");
	Check(!Parses("VRIK_Holstered", "Primary|", 2.0f, e), "nothing after the separator");
	Check(!Parses("VRIK_Holstered", "Primary|00012EB7", 2.0f, e), "no 0x prefix");
	Check(!Parses("VRIK_Holstered", "Left|0x00012EB7", 2.0f, e), "a hand word VRIK never sends");
	Check(!Parses("VRIK_Holstered", "PrimaryX|0x00012EB7", 2.0f, e), "a hand word with extra letters");
	Check(!Parses("VRIK_Holstered", "Primary|0x", 2.0f, e), "0x and no digits");
	Check(!Parses("VRIK_Holstered", "Primary|0x0001ZEB7", 2.0f, e), "a letter that is not hex");
	Check(!Parses("VRIK_Holstered", "Primary|0x100012EB7", 2.0f, e), "nine digits");
	Check(!Parses("VRIK_Holstered", "Primary|0x00000000", 2.0f, e), "form id 0");
	Check(!Parses("VRIK_Holstered", "Primary|0x00012EB7", 0.0f, e), "slot 0");
	Check(!Parses("VRIK_Holstered", "Primary|0x00012EB7", 15.0f, e), "slot 15");
	Check(!Parses("VRIK_Holstered", "Primary|0x00012EB7", -1.0f, e), "a negative slot");

	// Matching VRIK's event to the holster this mod asked for.
	Event holstered;
	holstered.holstered = true;
	holstered.primary = true;
	holstered.formID = 0x00012EB7;
	holstered.slot = 2;
	Check(EventIsFor(holstered, 0x00012EB7), "the holster event for the weapon asked about matches");
	Check(!EventIsFor(holstered, 0x00013790), "a different weapon does not");
	Event drawn = holstered;
	drawn.holstered = false;
	Check(!EventIsFor(drawn, 0x00012EB7), "a draw of that weapon is not its holster");
	Check(EventHandMatches(holstered, false), "Primary answers a question about VRIK's primary hand");
	Check(!EventHandMatches(holstered, true), "Primary does not answer one about the secondary hand");

	// The new release path needs VRIK's API, Grip hold, holster awareness and the toggle - all four.
	int applies = 0;
	for (int bits = 0; bits < 16; ++bits)
	{
		const bool api = (bits & 1) != 0;
		const bool simple = (bits & 2) != 0;
		const bool aware = (bits & 4) != 0;
		const bool toggle = (bits & 8) != 0;
		const bool got = ReleasePathApplies(api, simple, aware, toggle);
		Check(got == (api && !simple && aware && toggle), "ReleasePathApplies is api && Grip hold && aware && toggle");
		applies += got ? 1 : 0;
	}
	Check(applies == 1, "exactly one of the sixteen combinations takes the new path");

	// iHolsterAction's sheathe-it-ourselves values count as 0 once VRIK can holster.
	Check(EffectiveHolsterAction(true, 1) == 0 && EffectiveHolsterAction(true, 2) == 0, "with the API, 1 and 2 count as 0");
	Check(EffectiveHolsterAction(true, 0) == 0, "with the API, 0 stays 0");
	Check(EffectiveHolsterAction(false, 1) == 1 && EffectiveHolsterAction(false, 2) == 2, "older VRIK keeps the setting");

	// The release decision on the new path.
	Check(DecideRelease(false, 2, true) == Release::kNotThisPath, "off the new path nothing is decided here");
	Check(DecideRelease(true, 0, true) == Release::kNoSlot, "no slot in reach");
	Check(DecideRelease(true, -1, true) == Release::kNoSlot, "-1 is no slot");
	Check(DecideRelease(true, 15, true) == Release::kNoSlot, "15 is no slot");
	Check(DecideRelease(true, 2, false) == Release::kSlotRefuses, "a holster in reach that cannot take the weapon");
	Check(DecideRelease(true, 2, true) == Release::kAskVrik, "a holster in reach that can: ask VRIK");
	Check(DecideRelease(true, 14, true) == Release::kAskVrik, "the chest slot too");

	// Waiting for VRIK to take the weapon out of the hand.
	float left = kWaitSeconds;
	Check(TickWait(left, 0x00012EB7, 0x00012EB7, 0.011f) == Wait::kWaiting, "still in the hand a frame later: waiting");
	Check(left < kWaitSeconds, "the wait counts down");
	Check(TickWait(left, 0x00012EB7, 0, 0.011f) == Wait::kHolstered, "gone from the hand: holstered");
	left = kWaitSeconds;
	Check(TickWait(left, 0x00012EB7, 0x00013790, 0.011f) == Wait::kSwapped, "another weapon in the hand: swapped");
	left = kWaitSeconds;
	Wait w = Wait::kWaiting;
	for (int frame = 0; frame < 261 && w == Wait::kWaiting; ++frame) // 2.9 s at 90 Hz
	{
		w = TickWait(left, 0x00012EB7, 0x00012EB7, 1.0f / 90.0f);
	}
	Check(w == Wait::kWaiting, "2.9 seconds with the weapon still there: still waiting");
	for (int frame = 0; frame < 20 && w == Wait::kWaiting; ++frame) // on to about 3.12 s
	{
		w = TickWait(left, 0x00012EB7, 0x00012EB7, 1.0f / 90.0f);
	}
	Check(w == Wait::kTimedOut, "past 3 seconds: timed out");
	left = 0.05f;
	Check(TickWait(left, 0x00012EB7, 0, 0.1f) == Wait::kHolstered, "gone on the last frame counts as holstered");

	const KeptHand closed = HandAfterTimeout(true);
	Check(closed.armed && !closed.needsFreshGripPress, "kept with the grip closed: still held");
	const KeptHand open = HandAfterTimeout(false);
	Check(!open.armed && open.needsFreshGripPress, "kept with the grip open: waits for a fresh squeeze");

	// The "this holster will take it" buzz.
	BuzzEdge buzz;
	Check(buzz.Update(true, 0.011f), "arriving at a holster that takes the weapon buzzes");
	Check(!buzz.Update(true, 0.011f), "staying there does not buzz again");
	Check(!buzz.Update(false, 0.05f), "a moment out of range does not buzz");
	Check(!buzz.Update(true, 0.011f), "back within 0.15s: no second buzz");
	Check(!buzz.Update(false, 0.1f), "out for 0.1s");
	Check(!buzz.Update(false, 0.1f), "out for 0.2s in all");
	Check(buzz.Update(true, 0.011f), "coming back after a real exit buzzes again");
	buzz.Reset();
	Check(buzz.Update(true, 0.011f), "a reset edge buzzes on its first holster");

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
