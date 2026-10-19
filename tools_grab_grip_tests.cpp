// Console tests for GrabGripRules.h: when a sensed touch is worth telling a grip driver about, and
// exactly what it is told - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_grab_grip_tests.cpp /Fe:grab_grip_tests.exe
//   grab_grip_tests.exe
// Exit code 0 means every check passed.

#include "GrabGripRules.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace SwapDropAndHoldRedux::GrabGrip;

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

	constexpr unsigned int kIronSword = 0x00012EB7;
	constexpr unsigned int kIronDagger = 0x0001397E;
	constexpr unsigned int kLooseRef = 0xFF001ED9;

	// An iron sword taken hold of a little above the middle with the right controller, the natural way round.
	Touch GoodTouch()
	{
		Touch t;
		t.valid = true;
		t.refID = kLooseRef;
		t.formID = kIronSword;
		t.isLeftVRController = false;
		t.weaponType = 1;
		t.longAxis = 2;
		t.axisUsable = true;
		t.axisAmbiguous = false;
		t.alongU = 0.12f;
		t.hiltU = -0.8f;
		t.lengthM = 0.93f;
		t.offAxisM = 0.031f;
		t.tipSign = 1;
		t.reversedAngleDeg = 9.0f;
		t.reversedVerdict = false;
		t.flipDecided = false;
		t.flipAxis = 1;
		return t;
	}
}

int main()
{
	// ---- is it worth sending? ---------------------------------------------------------------------
	Check(Judge(GoodTouch(), kIronSword, 0.4f) == Why::kSend, "a clean touch of the weapon being equipped is sent");

	{
		Touch t = GoodTouch();
		t.valid = false;
		Check(Judge(t, kIronSword, 0.4f) == Why::kNoRecord, "nothing sensed, nothing sent");
	}
	Check(Judge(GoodTouch(), kIronDagger, 0.4f) == Why::kOtherWeapon, "a touch of one weapon says nothing about another being equipped");
	Check(Judge(GoodTouch(), kIronSword, 31.0f) == Why::kStale, "a touch from half a minute ago is not this pickup");
	Check(Judge(GoodTouch(), kIronSword, kMaxAgeSeconds) == Why::kSend, "exactly the maximum age still counts");
	Check(Judge(GoodTouch(), kIronSword, -1.0f) == Why::kStale, "a touch from the future is a broken clock, not a pickup");
	{
		Touch t = GoodTouch();
		t.weaponType = 7;
		Check(Judge(t, kIronSword, 0.4f) == Why::kNotMelee, "a bow has no grip to move");
		t.weaponType = 8;
		Check(Judge(t, kIronSword, 0.4f) == Why::kNotMelee, "nor a staff");
		t.weaponType = 6;
		Check(Judge(t, kIronSword, 0.4f) == Why::kSend, "a battleaxe's grab position is sent");
	}
	{
		Touch t = GoodTouch();
		t.pulled = true;
		Check(Judge(t, kIronSword, 0.4f) == Why::kPulled, "pulled through the air: where it landed in the hand is chance");
	}
	{
		// The player may prefer the weapon held wherever the pull happened to leave it.
		Touch t = GoodTouch();
		t.pulled = true;
		Check(Judge(t, kIronSword, 0.4f, false) == Why::kPulled, "pulled grabs off: a pull is chance, not a grip");
		Check(Judge(t, kIronSword, 0.4f, true) == Why::kSend, "pulled grabs on: a pulled weapon is held where it landed");
	}
	{
		// Allowing pulls does not allow readings that are simply broken.
		Touch t = GoodTouch();
		t.pulled = true;
		t.warped = true;
		Check(Judge(t, kIronSword, 0.4f, true) == Why::kWarped, "a warped reading is still refused when pulls are allowed");
	}
	{
		Touch t = GoodTouch();
		t.pulled = true;
		t.offAxisM = 0.45f;
		Check(Judge(t, kIronSword, 0.4f, true) == Why::kOffTheWeapon, "a pull that threw the palm right off the weapon is still refused");
	}
	{
		Touch t = GoodTouch();
		t.pulled = true;
		t.axisAmbiguous = true;
		Check(Judge(t, kIronSword, 0.4f, true) == Why::kAmbiguousAxis, "no clear long axis still outranks an allowed pull");
	}
	{
		Touch t = GoodTouch();
		t.warped = true;
		Check(Judge(t, kIronSword, 0.4f) == Why::kWarped, "already warped to the hand by HIGGS: there was no touch to measure");
	}
	{
		Touch t = GoodTouch();
		t.axisUsable = false;
		Check(Judge(t, kIronSword, 0.4f) == Why::kNoAxis, "no measurable long axis");
	}
	{
		Touch t = GoodTouch();
		t.axisAmbiguous = true;
		Check(Judge(t, kIronSword, 0.4f) == Why::kAmbiguousAxis, "no clear long axis: hold it the natural way");
	}
	{
		Touch t = GoodTouch();
		t.offAxisM = 0.45f;
		Check(Judge(t, kIronSword, 0.4f) == Why::kOffTheWeapon, "a palm 45 cm from the weapon is a bad reading, not a grip");
		t.offAxisM = kMaxOffAxisM;
		Check(Judge(t, kIronSword, 0.4f) == Why::kSend, "exactly the limit still counts");
	}
	{
		// Order matters for the log: the most basic reason wins.
		Touch t = GoodTouch();
		t.pulled = true;
		t.warped = true;
		t.axisAmbiguous = true;
		Check(Judge(t, kIronDagger, 99.0f) == Why::kOtherWeapon, "another weapon outranks everything about the touch itself");
		Check(Judge(t, kIronSword, 99.0f) == Why::kStale, "then its age");
		Check(Judge(t, kIronSword, 0.4f) == Why::kPulled, "then a pull, before a warp or the axis");
	}

	// ---- what is sent -------------------------------------------------------------------------------
	{
		const SdhGrabGripAPI::GrabGripEvent e = BuildEvent(GoodTouch(), false, true);
		Check(e.structSize == 56 && e.version == 1, "the event says its own size and version");
		Check(e.weaponFormID == kIronSword && e.weaponRefID == kLooseRef, "it names the weapon and the loose reference");
		Check(e.isLeftGameHand == 0, "the right game hand is 0");
		Check(e.source == SdhGrabGripAPI::kLooseGrab, "a pickup is a loose grab");
		Check(e.longAxisLocal == 2 && e.flipAxisLocal == 1, "the axes travel as plain indices");
		Check(Near(e.alongU, 0.12f) && Near(e.hiltU, -0.8f), "the touch and the hilt reference travel unchanged");
		Check(Near(e.lengthM, 0.93f) && Near(e.reversedAngleDeg, 9.0f) && Near(e.palmOffAxisM, 0.031f), "and so do the diagnostics");
		Check((e.flags & SdhGrabGripAPI::kHasAlong) != 0, "a sent touch always carries its position");
		Check((e.flags & SdhGrabGripAPI::kHasHiltRef) != 0, "a known tip end means the hilt reference can be trusted");
		Check((e.flags & SdhGrabGripAPI::kReversed) == 0, "the natural way round is not reversed");
		Check((e.flags & SdhGrabGripAPI::kHasFlipAxis) == 0, "an undecided turn-over axis is not claimed");
		Check((e.flags & SdhGrabGripAPI::kAxisAmbiguous) == 0, "nothing ambiguous is ever sent");
	}
	Check(BuildEvent(GoodTouch(), true, true).isLeftGameHand == 1, "the left game hand is 1");
	{
		Touch t = GoodTouch();
		t.alongU = 1.4f;
		Check(Near(BuildEvent(t, false, true).alongU, 1.0f), "a palm past the tip holds the tip");
		t.alongU = -2.0f;
		Check(Near(BuildEvent(t, false, true).alongU, -1.0f), "a palm past the pommel holds the pommel");
	}
	{
		Touch t = GoodTouch();
		t.tipSign = 0;
		t.hiltU = 0.05f;
		const SdhGrabGripAPI::GrabGripEvent e = BuildEvent(t, false, true);
		Check((e.flags & SdhGrabGripAPI::kHasHiltRef) == 0, "a hilt in the middle of the shape is not offered as a reference");
		Check((e.flags & SdhGrabGripAPI::kHasAlong) != 0, "but the touch itself still is");
	}
	{
		Touch t = GoodTouch();
		t.reversedVerdict = true;
		t.reversedAngleDeg = 171.0f;
		t.flipDecided = true;
		t.flipAxis = 0;
		const SdhGrabGripAPI::GrabGripEvent e = BuildEvent(t, false, true);
		Check((e.flags & SdhGrabGripAPI::kReversed) != 0, "taken hold of backwards: reversed");
		Check((e.flags & SdhGrabGripAPI::kHasFlipAxis) != 0 && e.flipAxisLocal == 0, "with the way to turn it over, decided by how it was held");

		Check((BuildEvent(t, false, false).flags & SdhGrabGripAPI::kReversed) == 0, "reversed grabs switched off in the settings: never reversed");

		t.weaponType = 5;
		Check((BuildEvent(t, false, true).flags & SdhGrabGripAPI::kReversed) == 0, "a greatsword is never reversed");
		Check((BuildEvent(t, false, true).flags & SdhGrabGripAPI::kHasAlong) != 0, "though where it was taken hold of still counts");

		t.weaponType = 2;
		t.tipSign = 0;
		Check((BuildEvent(t, false, true).flags & SdhGrabGripAPI::kReversed) == 0, "no known tip, no reversed: which way is backwards is not known");
	}

	{
		// WHERE a pulled weapon landed is worth using; WHICH WAY ROUND it landed is not. That is
		// chance too, and once a grip driver can turn a weapon over, a flip nobody asked for would
		// be a nasty surprise on an ordinary pull.
		Touch t = GoodTouch();
		t.reversedVerdict = true;
		t.reversedAngleDeg = 171.0f;
		t.flipDecided = true;
		t.flipAxis = 0;

		Check((BuildEvent(t, false, true).flags & SdhGrabGripAPI::kReversed) != 0, "close-grabbed backwards: reversed");

		t.pulled = true;
		const SdhGrabGripAPI::GrabGripEvent pulledEvent = BuildEvent(t, false, true);
		Check((pulledEvent.flags & SdhGrabGripAPI::kReversed) == 0, "the same touch, pulled through the air, is never reported backwards");
		Check((pulledEvent.flags & SdhGrabGripAPI::kHasAlong) != 0, "though where the pull left it still counts");
	}

	{
		// M3.0.0-test8: bGrabGripReversePulled. 35 of the 77 grabs in the 2026-09-22 log were pulls and not one
		// could come out reversed, however the wrist was turned. With it on, a pull is judged like any grab: by
		// which way round the weapon arrives against how the hand is turned when it catches it.
		Touch t = GoodTouch();
		t.reversedVerdict = true;
		t.reversedAngleDeg = 150.0f;
		t.flipDecided = true;
		t.flipAxis = 0;
		t.pulled = true;
		Check((BuildEvent(t, false, true, true).flags & SdhGrabGripAPI::kReversed) != 0,
			"pulled backwards, with pulled reversing allowed: reversed");
		Check((BuildEvent(t, false, true, false).flags & SdhGrabGripAPI::kReversed) == 0,
			"the same pull with it switched off: never reversed, as before");
		Check((BuildEvent(t, false, false, true).flags & SdhGrabGripAPI::kReversed) == 0,
			"reversed grabs switched off altogether still win over a pull being allowed");
		t.reversedVerdict = false;
		Check((BuildEvent(t, false, true, true).flags & SdhGrabGripAPI::kReversed) == 0,
			"a pull that arrived the natural way round stays natural");
		t.reversedVerdict = true;
		t.weaponType = 5;
		Check((BuildEvent(t, false, true, true).flags & SdhGrabGripAPI::kReversed) == 0, "a pulled greatsword is still never reversed");
		t.weaponType = 2;
		t.tipSign = 0;
		Check((BuildEvent(t, false, true, true).flags & SdhGrabGripAPI::kReversed) == 0, "nor a pull with no known tip");
	}
	{
		// The engine asks the same question before it logs its verdict.
		Check(MayReverse(1, false, 1, false, false, false), "a one-handed sword with a known tip, touched: may reverse");
		Check(!MayReverse(5, false, 1, false, false, false), "a two-hander may not");
		Check(!MayReverse(1, true, 1, false, false, false), "an ambiguous axis may not");
		Check(!MayReverse(1, false, 0, false, false, false), "an unknown tip may not");
		Check(!MayReverse(1, false, 1, false, true, false), "a warped grab may not");
		Check(!MayReverse(1, false, 1, true, false, false), "a pull may not, unless pulls are allowed to");
		Check(MayReverse(2, false, -1, true, false, true), "and then a pulled dagger may");
		Check(std::strcmp(WhyNotReverse(5, false, 1, false, false, false), "a two-handed weapon") == 0, "the log names a two-hander");
		Check(std::strcmp(WhyNotReverse(1, false, 1, true, false, false), "it was pulled (bGrabGripReversePulled = 0)") == 0,
			"and a refused pull, with the setting that allows it");
		Check(std::strcmp(WhyNotReverse(1, false, 1, false, false, false), "") == 0, "and nothing when reversing is allowed");
	}
	{
		// M3.0.0-test14: each weapon mode has its own switches; Legacy keeps the ini pair and never reverses a
		// two-hander.
		Choice simple;
		simple.position = false;
		simple.reversed = true;
		simple.reversedTwoHanded = true;
		Choice gripHold;
		gripHold.position = true;
		gripHold.reversed = false;
		gripHold.reversedTwoHanded = false;
		Choice legacy;
		legacy.position = true;
		legacy.reversed = true;
		legacy.reversedTwoHanded = true;
		const Choice s = ChooseForMode(2, simple, gripHold, legacy);
		Check(!s.position && s.reversed && s.reversedTwoHanded, "Simple mode uses the Simple page's switches");
		const Choice g = ChooseForMode(1, simple, gripHold, legacy);
		Check(g.position && !g.reversed && !g.reversedTwoHanded, "Grip hold uses the Grip Hold page's switches");
		const Choice l = ChooseForMode(0, simple, gripHold, legacy);
		Check(l.position && l.reversed && !l.reversedTwoHanded, "Legacy uses the ini pair and never reverses a two-hander");
		const Choice u = ChooseForMode(7, simple, gripHold, legacy);
		Check(u.position && u.reversed && !u.reversedTwoHanded, "an unknown mode number behaves as Legacy");
	}
	{
		// Reverse without the position: the event says reversed and nothing about where.
		Touch t = GoodTouch();
		t.reversedVerdict = true;
		t.reversedAngleDeg = 171.0f;
		const SdhGrabGripAPI::GrabGripEvent e = BuildEvent(t, false, true, false, false);
		Check((e.flags & SdhGrabGripAPI::kReversed) != 0, "position off, taken hold of backwards: still reversed");
		Check((e.flags & SdhGrabGripAPI::kHasAlong) == 0, "and no position is sent: held at its natural grip");
		Check(WorthSending(e), "a reversal alone is worth sending");

		t.reversedVerdict = false;
		const SdhGrabGripAPI::GrabGripEvent n = BuildEvent(t, false, true, false, false);
		Check((n.flags & (SdhGrabGripAPI::kReversed | SdhGrabGripAPI::kHasAlong)) == 0,
			"position off and a normal grab: nothing to say");
		Check(!WorthSending(n), "so nothing is sent");
		Check(WorthSending(BuildEvent(t, false, true)), "with the position on, a normal grab still sends where");
		Check(!WorthSending(BuildEvent(t, false, false, false, false)), "both off: nothing is sent");
	}
	{
		// Reversed two-handers, only when that mode allows them.
		Touch t = GoodTouch();
		t.weaponType = 5;
		t.reversedVerdict = true;
		t.reversedAngleDeg = 170.0f;
		Check((BuildEvent(t, false, true, false, true, false).flags & SdhGrabGripAPI::kReversed) == 0,
			"a greatsword stays unreversed unless allowed");
		Check((BuildEvent(t, false, true, false, true, true).flags & SdhGrabGripAPI::kReversed) != 0,
			"with two-handers allowed, a greatsword is reversed");
		t.weaponType = 6;
		Check((BuildEvent(t, false, true, false, true, true).flags & SdhGrabGripAPI::kReversed) != 0,
			"and a battleaxe or warhammer");
		Check((BuildEvent(t, false, false, false, true, true).flags & SdhGrabGripAPI::kReversed) == 0,
			"Reverse grip off still wins over two-handers allowed");
		Check(IsReversibleMelee(5, true) && IsReversibleMelee(6, true) && !IsReversibleMelee(5, false) &&
			!IsReversibleMelee(6, false), "two-handers reverse only when allowed");
		Check(!IsReversibleMelee(7, true) && !IsReversibleMelee(8, true) && !IsReversibleMelee(9, true) &&
			!IsReversibleMelee(0, true), "bows, staves, crossbows and fists never do");
		Check(IsReversibleMelee(1, false) && IsReversibleMelee(4, false), "one-handers always may");
		Check(MayReverse(5, false, 1, false, false, false, true), "a two-hander may reverse when allowed");
		Check(!MayReverse(5, false, 1, false, false, false, false), "and may not otherwise");
		Check(std::strcmp(WhyNotReverse(5, false, 1, false, false, false, true), "") == 0,
			"an allowed two-hander has nothing to explain");
	}
	{
		// A hand switch: the other hand took the weapon over, so there is no loose reference.
		Touch t = GoodTouch();
		t.handOff = true;
		const SdhGrabGripAPI::GrabGripEvent e = BuildEvent(t, true, true);
		Check(e.source == SdhGrabGripAPI::kHandOff, "a hand switch is sent as a hand-off");
		Check(e.weaponRefID == 0, "with no loose reference");
		Check(BuildEvent(GoodTouch(), false, true).source == SdhGrabGripAPI::kLooseGrab, "a pickup is still a loose grab");
		Check(BuildEvent(GoodTouch(), false, true).weaponRefID == kLooseRef, "with its reference");
	}
	{
		// M3.0.0-test15: "Hold anywhere on the weapon" - above the crossguard too (PCVR keeps the side it was taken on).
		Choice c;
		Check(!c.anywhere, "anywhere is off unless a mode page turns it on");
		Choice simple;
		simple.anywhere = true;
		Check(ChooseForMode(2, simple, Choice{}, Choice{}).anywhere, "Simple carries its own anywhere switch");
		Check(!ChooseForMode(1, simple, Choice{}, Choice{}).anywhere, "Grip hold has its own, off here");
		Choice legacy;
		legacy.anywhere = true;
		Check(!ChooseForMode(0, simple, Choice{}, legacy).anywhere, "Legacy never holds anywhere");

		const Touch t = GoodTouch();
		Check((BuildEvent(t, false, true, false, true, false, true).flags & SdhGrabGripAPI::kAnywhere) != 0,
			"anywhere on, with the position: the driver is told");
		Check((BuildEvent(t, false, true, false, true, false, false).flags & SdhGrabGripAPI::kAnywhere) == 0,
			"anywhere off: never sent");
		Check((BuildEvent(t, false, true, false, false, false, true).flags & SdhGrabGripAPI::kAnywhere) == 0,
			"no position, no anywhere - there is nowhere to hold it but the natural grip");
		Check((BuildEvent(t, false, true).flags & SdhGrabGripAPI::kAnywhere) == 0, "the old call sends no anywhere");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
