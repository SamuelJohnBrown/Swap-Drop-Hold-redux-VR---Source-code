// Console tests for EmbedBuryRules.h: telling a blade that went INTO a body from one that was
// merely let go near it - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_embed_bury_tests.cpp /Fe:embed_bury_tests.exe
//   embed_bury_tests.exe
// Exit code 0 means every check passed.

#include "EmbedBuryRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux::EmbedBury;

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

	// The case from the user's log, 2026-09-21: a dagger let go beside a dead body, never touching
	// it. It stuck in mid air and then had to be "worked free" as though it were buried.
	// boneDist was 9.1 units, depth 0%, 0 per cent buried.
	void TestLetGoBesideABodyIsNotAStab()
	{
		const Config config{};
		// Held alongside: the whole blade is about the same distance from the bone.
		Check(!LooksBuried(9.5f, 9.1f, config), "a blade let go alongside a body has not gone in");
		Check(!LooksBuried(9.1f, 9.5f, config), "nor one whose tip is the FURTHER end");
		Check(!LooksBuried(30.0f, 28.0f, config), "nor one simply near the body");
	}

	void TestARealStabIsStillAStab()
	{
		const Config config{};
		// Buried: the tip is deep in, the grip is outside. That gap IS the burial.
		Check(LooksBuried(22.0f, 2.0f, config), "a blade driven in is buried");
		Check(LooksBuried(30.0f, 1.0f, config), "and one driven in to the hilt certainly is");
		Check(LooksBuried(14.0f, 7.5f, config), "a shallow but real stab still counts");
	}

	void TestTheEdgeOfTheRule()
	{
		const Config config{};   // minBuryUnits = 6.0
		Check(!LooksBuried(15.0f, 9.5f, config), "5.5 units in is not enough");
		Check(LooksBuried(15.0f, 9.0f, config), "exactly the limit counts");
		Check(LooksBuried(15.0f, 8.5f, config), "and past it certainly does");
	}

	void TestBuryUnitsIsJustTheGap()
	{
		Check(Near(BuryUnits(22.0f, 2.0f), 20.0f), "burial is how much closer the tip is than the grip");
		Check(Near(BuryUnits(9.5f, 9.1f), 0.4f), "a blade laid alongside has almost none");
		Check(Near(BuryUnits(5.0f, 12.0f), -7.0f), "a blade pointing AWAY reads negative");
	}

	void TestNonsenseNeverSticks()
	{
		const Config config{};
		Check(!LooksBuried(-1.0f, 2.0f, config), "a missing grip distance is not a stab");
		Check(!LooksBuried(22.0f, -1.0f, config), "nor a missing tip distance");
		Check(!LooksBuried(std::nanf(""), 2.0f, config), "nor a broken reading");
		Check(!LooksBuried(22.0f, std::nanf(""), config), "nor a broken tip reading");
	}

	void TestTheGateCanBeTurnedOff()
	{
		// 0 keeps exactly the behaviour before this rule existed, for anyone who liked it.
		Config config{};
		config.minBuryUnits = 0.0f;
		Check(LooksBuried(9.5f, 9.1f, config), "with the requirement at zero, near enough is still a stab");
		Check(LooksBuried(9.1f, 9.1f, config), "even dead level");
		Check(!LooksBuried(9.1f, 9.5f, config), "but a blade pointing away never sticks");
	}

	// ---- M3.0.0-test8: the TIP must be inside the body --------------------------------------------
	// The 2026-09-22 log: both geometric pins passed the gap test by 13 units with the tip still 12-14
	// units from the bone (chest, head). A blade pointing AT a body has the same gap as one buried in
	// it, and the player saw both weapons float.
	void TestPointInBox()
	{
		const float lo[3] = { -0.1f, -0.2f, -0.3f };
		const float hi[3] = { 0.1f, 0.2f, 0.3f };
		const float centre[3] = { 0.0f, 0.0f, 0.0f };
		const float onFace[3] = { 0.1f, 0.0f, 0.0f };
		const float justOut[3] = { 0.1001f, 0.0f, 0.0f };
		const float farOut[3] = { 0.0f, 0.0f, 1.3f };
		const float broken[3] = { std::nanf(""), 0.0f, 0.0f };
		Check(InsideBox(centre, lo, hi), "the middle of a box is inside it");
		Check(InsideBox(onFace, lo, hi), "a point on its face counts as inside");
		Check(!InsideBox(justOut, lo, hi), "a hair past the face is outside");
		Check(!InsideBox(farOut, lo, hi), "far past it is outside");
		Check(!InsideBox(broken, lo, hi), "a broken reading is never inside");

		Check(Near(DistanceOutsideBox(centre, lo, hi), 0.0f), "inside is no distance away");
		Check(Near(DistanceOutsideBox(farOut, lo, hi), 1.0f), "past one face: the distance to that face");
		const float corner[3] = { 0.4f, 0.6f, 0.3f };
		Check(Near(DistanceOutsideBox(corner, lo, hi), 0.5f), "past a corner: the straight-line distance to it");
		Check(DistanceOutsideBox(broken, lo, hi) < 0.0f, "a broken reading has no distance");
	}

	void TestTheGeometricVerdict()
	{
		const Config config{};
		Check(config.tipMustBeInBody, "the tip test is on by default");
		// The 2026-09-22 pins: in by 13, tip outside every body part.
		Check(JudgeGeometric(27.3f, 14.1f, 18, false, config) == Verdict::kTipOutsideBody,
			"pointed at the chest from outside it: not a stab (the floating dagger)");
		Check(JudgeGeometric(27.3f, 14.1f, 18, true, config) == Verdict::kStab, "the same blade with its tip inside: a stab");
		Check(JudgeGeometric(9.5f, 9.1f, 18, true, config) == Verdict::kNotBuried,
			"a tip inside the body but laid alongside it is still not buried (test6's rule stands)");
		Check(JudgeGeometric(27.3f, 14.1f, 0, false, config) == Verdict::kNoBodies,
			"no body parts to test: nothing can show it went in, so no stab");
		Config off{};
		off.tipMustBeInBody = false;
		Check(JudgeGeometric(27.3f, 14.1f, 18, false, off) == Verdict::kStab, "with the tip test off: test6's rule alone, as before");
		Check(JudgeGeometric(27.3f, 14.1f, 0, false, off) == Verdict::kStab, "and it needs no body parts");
		Check(JudgeGeometric(9.5f, 9.1f, 18, true, off) == Verdict::kNotBuried, "the gap still has to be there");
		Check(JudgeGeometric(std::nanf(""), 2.0f, 18, true, config) == Verdict::kNotBuried, "a broken reading never sticks");
	}
}

int main()
{
	TestPointInBox();
	TestTheGeometricVerdict();
	TestLetGoBesideABodyIsNotAStab();
	TestARealStabIsStillAStab();
	TestTheEdgeOfTheRule();
	TestBuryUnitsIsJustTheGap();
	TestNonsenseNeverSticks();
	TestTheGateCanBeTurnedOff();

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
