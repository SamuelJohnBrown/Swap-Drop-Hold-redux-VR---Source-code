// Console tests for GripBlinkRules.h: when Grip Hold shows other mods a released grip right
// after a pickup, and for how long, without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_grip_blink_tests.cpp /Fe:grip_blink_tests.exe
//   grip_blink_tests.exe
// Exit code 0 means every check passed.

#include "GripBlinkRules.h"
#include "HiggsReleaseRules.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

	// Every fact that lets the grip be shown released.
	GripBlink::PickupFacts Blinkable()
	{
		GripBlink::PickupFacts facts;
		facts.gripHoldMode = true;
		facts.simpleMode = false;
		facts.throwModLoaded = true;
		facts.gripClosed = true;
		facts.higgsHandOff = true;
		facts.holsterInReach = false;
		facts.menuOpen = false;
		return facts;
	}

	const long long kTicksPerSecond = 1000000000LL;
}

int main()
{
	using GripBlink::DecideAfterPickup;
	using GripBlink::Decision;

	// The pickup this exists for.
	Check(DecideAfterPickup(Blinkable()) == Decision::kBlink,
		"Grip Hold, Weapon Throw VR loaded, grip closed, HIGGS hand off, no holster, no menu: show the grip released");

	// Each fact on its own stops it, and says which.
	{
		GripBlink::PickupFacts f = Blinkable();
		f.gripHoldMode = false;
		Check(DecideAfterPickup(f) == Decision::kNotGripHold, "Legacy mode never shows a released grip");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.simpleMode = true;
		Check(DecideAfterPickup(f) == Decision::kNotGripHold, "Simple mode never shows a released grip");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.throwModLoaded = false;
		Check(DecideAfterPickup(f) == Decision::kNoThrowMod, "without Weapon Throw VR nothing is shown");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.gripClosed = false;
		Check(DecideAfterPickup(f) == Decision::kGripOpen, "a grip that is already open has nothing to show");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.menuOpen = true;
		Check(DecideAfterPickup(f) == Decision::kMenuOpen, "a menu owns the grip, so nothing is shown");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.holsterInReach = true;
		Check(DecideAfterPickup(f) == Decision::kAtHolster,
			"at a holster the re-press could make VRIK holster the weapon, so nothing is shown");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.higgsHandOff = false;
		Check(DecideAfterPickup(f) == Decision::kHiggsHandOn,
			"a HIGGS hand already back on could act on the re-press, so nothing is shown");
	}

	// Several at once: the mode is reported first, then whether anyone is listening.
	{
		GripBlink::PickupFacts f = Blinkable();
		f.simpleMode = true;
		f.throwModLoaded = false;
		f.gripClosed = false;
		Check(DecideAfterPickup(f) == Decision::kNotGripHold, "the mode is reported before anything else");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.throwModLoaded = false;
		f.holsterInReach = true;
		f.higgsHandOff = false;
		Check(DecideAfterPickup(f) == Decision::kNoThrowMod, "no Weapon Throw VR is reported before a holster or HIGGS");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.holsterInReach = true;
		f.higgsHandOff = false;
		Check(DecideAfterPickup(f) == Decision::kAtHolster, "a holster in reach is reported before the HIGGS hand");
	}
	{
		const GripBlink::PickupFacts f{};
		Check(DecideAfterPickup(f) != Decision::kBlink, "facts nobody filled in never show a released grip");
	}

	// ---- M3.0.0-test9: the blink is for Weapon Throw VR whatever its hold button -----------------------
	// test8 skipped it for a Trigger hold button and throwing stopped altogether: the 2026-09-22 log has 40
	// trigger presses with a picked-up weapon in hand and not one throw attempt from Weapon Throw VR, where
	// test7 (blink on) had them from both hands. Weapon Throw VR keeps out a hand whose grip has been down
	// since it was empty, whatever button throws. The hold button is read for the log only.
	{
		const int buttons[] = { GripBlink::kThrowButtonUnread, GripBlink::kThrowButtonTrigger, GripBlink::kThrowButtonGrip,
			2, 3, 4, 5, 99, -7 };
		bool allBlink = true;
		for (const int button : buttons)
		{
			GripBlink::PickupFacts f = Blinkable();
			f.throwButton = button;
			allBlink = allBlink && DecideAfterPickup(f) == Decision::kBlink;
		}
		Check(allBlink, "every Weapon Throw VR hold button gets the blink, Trigger included (test8 broke throwing)");
	}
	{
		GripBlink::PickupFacts f = Blinkable();
		f.throwButton = GripBlink::kThrowButtonTrigger;
		f.holsterInReach = true;
		Check(DecideAfterPickup(f) == Decision::kAtHolster, "a holster in reach still stops it, whatever the button");
	}

	// The global's value as a button.
	Check(GripBlink::ThrowButtonFromGlobal(true, 0.0f) == GripBlink::kThrowButtonTrigger, "0.0 is Trigger");
	Check(GripBlink::ThrowButtonFromGlobal(true, 1.0f) == GripBlink::kThrowButtonGrip, "1.0 is Grip");
	Check(GripBlink::ThrowButtonFromGlobal(true, 0.9999f) == GripBlink::kThrowButtonGrip, "a hair under 1 is still Grip");
	Check(GripBlink::ThrowButtonFromGlobal(true, 1.0001f) == GripBlink::kThrowButtonGrip, "a hair over 1 is still Grip");
	Check(GripBlink::ThrowButtonFromGlobal(true, 5.0f) == 5, "5.0 is Touchpad");
	Check(GripBlink::ThrowButtonFromGlobal(true, -0.0001f) == GripBlink::kThrowButtonTrigger, "a hair under 0 is still Trigger");
	Check(GripBlink::ThrowButtonFromGlobal(false, 1.0f) == GripBlink::kThrowButtonUnread,
		"a global that was not found is unread, whatever the value says");
	Check(GripBlink::ThrowButtonFromGlobal(true, std::nanf("")) == GripBlink::kThrowButtonUnread, "not a number is unread");
	Check(GripBlink::ThrowButtonFromGlobal(true, 1.0e30f) == GripBlink::kThrowButtonUnread, "an absurd value is unread");
	Check(GripBlink::ThrowButtonFromGlobal(true, -1.0e30f) == GripBlink::kThrowButtonUnread, "an absurd negative value is unread");

	// The names the log uses, as Weapon Throw VR's menu words them.
	Check(std::strcmp(GripBlink::ThrowButtonName(GripBlink::kThrowButtonTrigger), "Trigger") == 0, "0 is named Trigger");
	Check(std::strcmp(GripBlink::ThrowButtonName(GripBlink::kThrowButtonGrip), "Grip") == 0, "1 is named Grip");
	Check(std::strcmp(GripBlink::ThrowButtonName(2), "A") == 0, "2 is named A");
	Check(std::strcmp(GripBlink::ThrowButtonName(3), "System") == 0, "3 is named System");
	Check(std::strcmp(GripBlink::ThrowButtonName(4), "Application / B") == 0, "4 is named Application / B");
	Check(std::strcmp(GripBlink::ThrowButtonName(5), "Touchpad") == 0, "5 is named Touchpad");
	Check(std::strcmp(GripBlink::ThrowButtonName(GripBlink::kThrowButtonUnread), "not found") == 0, "unread is named as such");
	Check(std::strcmp(GripBlink::ThrowButtonName(42), "an option it does not have") == 0, "anything else says so");

	// The window.
	{
		const long long start = 5 * kTicksPerSecond;
		const long long end = GripBlink::EndTicks(start, GripBlink::kSeconds, kTicksPerSecond);
		const long long expected = static_cast<long long>(static_cast<double>(GripBlink::kSeconds) * kTicksPerSecond);
		const long long length = end - start;
		Check(length >= expected - 1 && length <= expected + 1, "the window lasts kSeconds");
		Check(GripBlink::Active(start, end), "it is showing from the moment it starts");
		Check(GripBlink::Active(end - 1, end), "and up to its last tick");
		Check(!GripBlink::Active(end, end), "and not at its end");
		Check(!GripBlink::Active(end + kTicksPerSecond, end), "or after it");
	}
	{
		const long long now = 7 * kTicksPerSecond;
		Check(!GripBlink::Active(now, GripBlink::EndTicks(now, 0.0f, kTicksPerSecond)), "a zero-length window never shows");
		Check(!GripBlink::Active(now, GripBlink::EndTicks(now, -1.0f, kTicksPerSecond)), "a negative length never shows");
		Check(!GripBlink::Active(3 * kTicksPerSecond, 0), "a window that was never started never shows");
	}
	{
		// A second pickup while the first window still runs: the window runs on from the second.
		const long long first = GripBlink::EndTicks(10 * kTicksPerSecond, GripBlink::kSeconds, kTicksPerSecond);
		const long long secondStart = 10 * kTicksPerSecond + kTicksPerSecond / 20;
		const long long second = GripBlink::EndTicks(secondStart, GripBlink::kSeconds, kTicksPerSecond);
		Check(second > first, "a later pickup's window ends later");
		Check(GripBlink::Active(first, second), "and is still showing when the first would have ended");
	}

	// Grip-hold's own read keeps taking the real grip a moment past the window: a read the callback
	// hid just before the window ended can arrive just after it, and must not look like an open hand.
	{
		const long long start = 20 * kTicksPerSecond;
		const long long end = GripBlink::EndTicks(start, GripBlink::kSeconds, kTicksPerSecond);
		const long long grace =
			static_cast<long long>(static_cast<double>(GripBlink::kPhysicalGraceSeconds) * kTicksPerSecond);
		Check(GripBlink::PhysicalGripStillMasked(start, end, grace), "grip-hold takes the real grip while the window shows");
		Check(GripBlink::PhysicalGripStillMasked(end, end, grace), "and as the window ends");
		Check(GripBlink::PhysicalGripStillMasked(end + grace - 1, end, grace), "and until the grace runs out");
		Check(!GripBlink::PhysicalGripStillMasked(end + grace, end, grace), "but not after it");
		Check(!GripBlink::PhysicalGripStillMasked(start, 0, grace), "and never for a window that was never started");
		Check(GripBlink::kPhysicalGraceSeconds > 0.0f && GripBlink::kPhysicalGraceSeconds <= GripBlink::kSeconds,
			"the grace is shorter than the window");
	}

	// It has to fit inside the time HIGGS's hand stays off for the pickup, at every refresh rate.
	{
		const HiggsRelease::Config config{};
		Check(GripBlink::kSeconds > 0.0f, "the window has a length");
		Check(GripBlink::kSeconds < config.maxSeconds, "the window ends before HIGGS's time limit");
		const float rates[] = { 45.0f, 72.0f, 90.0f, 120.0f, 144.0f };
		for (const float rate : rates)
		{
			Check(GripBlink::kSeconds * rate >= 2.0f, "the window spans at least two frames at this refresh rate");
			Check(GripBlink::kSeconds < static_cast<float>(config.maxSteps) / rate,
				"the window ends before HIGGS's step limit at this refresh rate");
		}
	}

	// ---- the blink after a DRAW from a holster --------------------------------------------
	// A drawn weapon's grip has been closed since before the draw too, so Weapon Throw VR ignores it
	// just as it ignored a pickup (M2.2.8-test1's log: 13 draws, 0 blinks, 0 throws). But the hand is
	// ON the slot at that moment, and VRIK buzzes again within a second because the freed slot would
	// take the weapon back - a fake release-and-press there could re-holster it. So the blink waits
	// until the hand has left the holster.
	{
		GripBlink::DrawFacts f;
		f.gripStillClosed = true;
		f.standingAside = true;
		f.distanceFromDraw = 40.0f;
		f.leaveDistance = 27.5f;
		f.secondsSinceDraw = 0.5f;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kWait, "standing aside at the slot: wait, however far the hand is");
		f.standingAside = false;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kBlink, "off the slot and past the leave distance: blink");
		f.distanceFromDraw = 10.0f;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kWait, "off the slot but still close to where it was drawn: wait");
		f.secondsSinceDraw = GripBlink::kDrawBlinkMaxWaitSeconds;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kBlink, "close, but the maximum wait is up: blink anyway");
		f.secondsSinceDraw = 0.5f;
		f.gripStillClosed = false;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kCancel, "the hand opened: Weapon Throw VR saw a real release, nothing to fake");
		f.gripStillClosed = true;
		f.distanceFromDraw = 27.5f;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kBlink, "exactly the leave distance counts as left");
		f.distanceFromDraw = 27.4f;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kWait, "just under it does not");
	}
	{
		GripBlink::DrawFacts f;
		f.gripStillClosed = false;
		f.standingAside = true;
		Check(GripBlink::DecideAfterDraw(f) == GripBlink::DrawStep::kCancel, "an open hand cancels even while standing aside");
	}
	Check(GripBlink::kDrawBlinkMaxWaitSeconds >= 2.0f && GripBlink::kDrawBlinkMaxWaitSeconds <= 5.0f,
		"the maximum wait is a few seconds: long enough to walk off a slot, short enough to be useful in a fight");

	// ---- M3.0.0-test7: the re-press waits until the hand is clear of anything VR Climbing climbs ----
	// VR Climbing climbs on a grip PRESS when one of six world-axis rays from the hand hits a climbable
	// layer. The blink's re-press is a press, and a weapon picked up off a table leaves the hand on the
	// table: 6 of the 8 climbs in the 2026-09-21 log started 0.1s after a pickup.
	{
		using GripBlink::IsClimbableLayer;
		// Read from VR Climbing's own DLL (its layer jump table): static, animated static, trees,
		// props, terrain, ground.
		const int climbable[] = { 1, 2, 9, 10, 13, 17 };
		for (const int layer : climbable)
		{
			Check(IsClimbableLayer(layer), "a layer VR Climbing climbs counts as climbable");
		}
		const int notClimbable[] = { 0, 3, 4, 5, 6, 7, 8, 11, 12, 14, 15, 16, 18, 30, 32, 56, 127 };
		for (const int layer : notClimbable)
		{
			Check(!IsClimbableLayer(layer), "a layer VR Climbing does not climb is not climbable");
		}
		Check(!IsClimbableLayer(-1), "nonsense is not climbable");
	}
	{
		using GripBlink::NearClimbable;
		using GripBlink::ProbeRay;
		int layer = -1;
		float dist = -1.0f;
		const float reach = GripBlink::kHoldBackReachUnits;

		ProbeRay none[6];
		Check(!NearClimbable(none, 6, reach, layer, dist), "six rays that hit nothing: the hand is clear");

		ProbeRay table[6];
		table[5].hit = true;
		table[5].layer = 1;
		table[5].distUnits = 3.0f;
		Check(NearClimbable(table, 6, reach, layer, dist), "the downward ray hits a static table 3 units away: near");
		Check(layer == 1 && dist == 3.0f, "and it says which surface, and how far");

		ProbeRay own[6];
		own[0].hit = true;
		own[0].layer = 8;
		own[0].distUnits = 1.0f;
		Check(!NearClimbable(own, 6, reach, layer, dist),
			"a ray whose first hit is the player's own body is not a climb - VR Climbing only reads the first hit");

		ProbeRay beyond[6];
		beyond[2].hit = true;
		beyond[2].layer = 1;
		beyond[2].distUnits = reach + 0.5f;
		Check(!NearClimbable(beyond, 6, reach, layer, dist), "a wall beyond the reach is not near");

		ProbeRay two[6];
		two[1].hit = true;
		two[1].layer = 13;
		two[1].distUnits = 9.0f;
		two[4].hit = true;
		two[4].layer = 2;
		two[4].distUnits = 4.0f;
		Check(NearClimbable(two, 6, reach, layer, dist) && layer == 2 && dist == 4.0f, "the nearest climbable hit is the one reported");

		Check(GripBlink::kHoldBackReachUnits > GripBlink::kVrClimbingRayUnits,
			"the reach is longer than VR Climbing's own ray, so a hand moving between the check and the press is still covered");
	}
	{
		using GripBlink::DecideHoldBack;
		using GripBlink::HoldBack;
		GripBlink::HoldBackFacts f;
		f.enabled = true;
		f.showing = true;
		f.gripClosed = true;
		f.probeRan = true;
		f.nearClimbable = true;
		f.secondsSinceBlink = 0.2f;
		Check(DecideHoldBack(f) == HoldBack::kExtend, "the grip shown released, still closed, next to a table: keep it shown released");

		GripBlink::HoldBackFacts g = f;
		g.nearClimbable = false;
		Check(DecideHoldBack(g) == HoldBack::kLetItEnd, "clear of every climbable surface: let the window end, and the re-press come");
		g = f;
		g.showing = false;
		Check(DecideHoldBack(g) == HoldBack::kLetItEnd, "no window showing: never start one");
		g = f;
		g.gripClosed = false;
		Check(DecideHoldBack(g) == HoldBack::kLetItEnd, "the hand opened: there is no press left to hold back");
		g = f;
		g.probeRan = false;
		Check(DecideHoldBack(g) == HoldBack::kLetItEnd, "the probe could not run: the old 0.10s blink, not a guess");
		g = f;
		g.enabled = false;
		Check(DecideHoldBack(g) == HoldBack::kLetItEnd, "bBlinkWaitsForClearHand = 0: the old 0.10s blink");
		g = f;
		g.secondsSinceBlink = GripBlink::kHoldBackMaxSeconds;
		Check(DecideHoldBack(g) == HoldBack::kGiveUp, "at the limit: give up, however near the surface");
		g.secondsSinceBlink = GripBlink::kHoldBackMaxSeconds - 0.01f;
		Check(DecideHoldBack(g) == HoldBack::kExtend, "just under it: still holding back");
		g.nearClimbable = false;
		g.secondsSinceBlink = GripBlink::kHoldBackMaxSeconds + 5.0f;
		Check(DecideHoldBack(g) == HoldBack::kLetItEnd, "clear past the limit is simply clear");
	}
	{
		using GripBlink::HoldBackEndTicks;
		const long long now = 50 * kTicksPerSecond;
		const long long step = static_cast<long long>(static_cast<double>(GripBlink::kHoldBackStepSeconds) * kTicksPerSecond);

		// Inside the first 0.10s the window already runs past a step: holding back never SHORTENS it.
		const long long blinkEnd = GripBlink::EndTicks(now, GripBlink::kSeconds, kTicksPerSecond);
		Check(HoldBackEndTicks(now, blinkEnd, kTicksPerSecond) == blinkEnd, "holding back never shortens the window it was given");
		// After that it keeps the window a step ahead of now.
		const long long later = blinkEnd + 1;
		const long long extended = HoldBackEndTicks(later, blinkEnd, kTicksPerSecond);
		Check(extended >= later + step - 1 && extended <= later + step + 1, "past the first 0.10s, the window is kept one step ahead");
		Check(GripBlink::Active(later, extended), "so it is still showing");
		Check(!GripBlink::Active(extended, extended), "and ends on its own a step later if nobody extends it again");

		Check(GripBlink::kHoldBackStepSeconds >= 0.02f && GripBlink::kHoldBackStepSeconds <= GripBlink::kSeconds,
			"a step outlasts a slow frame but is no longer than the blink itself");
		const float stepRates[] = { 45.0f, 72.0f, 90.0f, 120.0f, 144.0f };
		for (const float rate : stepRates)
		{
			Check(GripBlink::kHoldBackStepSeconds * rate >= 1.5f,
				"a step spans more than one frame at this refresh rate, so an ordinary frame never lets the window lapse");
		}
		Check(GripBlink::kHoldBackMaxSeconds >= 3.0f && GripBlink::kHoldBackMaxSeconds <= 30.0f,
			"the limit is seconds: long enough to lift a weapon off a table, short enough that a stuck probe cannot hide a grip for good");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
