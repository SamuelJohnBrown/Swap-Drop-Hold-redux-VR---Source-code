// Console tests for HolsterBodyRules.h: measuring holsters from the third-person body, and judging
// from real VRIK interactions which skeleton VRIK's slots belong to - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_holster_body_tests.cpp /Fe:holster_body_tests.exe
//   holster_body_tests.exe
// Exit code 0 means every check passed.

#include "HolsterBodyRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux::HolsterBody;

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
		return std::fabs(a - b) < 0.01f;
	}
}

int main()
{
	// ---- which centre a slot uses -----------------------------------------------------------
	// Off (the default): exactly as before - learned, then seeded, then VRIK's ini position.
	Check(SourceFor(false, true, true) == Centre::kLearned, "off: a learned slot uses its learned centre");
	Check(SourceFor(false, false, true) == Centre::kSeeded, "off: an unlearned seeded slot uses its seed");
	Check(SourceFor(false, false, false) == Centre::kIni, "off: a slot with neither uses VRIK's own position");

	// On: VRIK's own numbers, always. The corrections were measured on the other skeleton.
	Check(SourceFor(true, true, true) == Centre::kIni, "on: a learned slot still uses VRIK's own position");
	Check(SourceFor(true, false, true) == Centre::kIni, "on: a seeded slot uses VRIK's own position");
	Check(SourceFor(true, false, false) == Centre::kIni, "on: so does a plain one");

	// ---- the tight buzz -----------------------------------------------------------------------
	Check(!CentreTrusted(false, false), "off: an untrusted learned centre gets the wide buzz, as before");
	Check(CentreTrusted(false, true), "off: a trusted learned centre gets the tight buzz, as before");
	Check(CentreTrusted(true, false), "on: every slot is VRIK's own position on VRIK's body - tight buzz");
	Check(CentreTrusted(true, true), "on: whatever the learned file says");

	// ---- learning ---------------------------------------------------------------------------
	Check(MayLearn(false), "off: learning carries on as before");
	Check(!MayLearn(true), "on: nothing is learned - the file holds the OTHER skeleton's positions");

	// ---- heading ----------------------------------------------------------------------------
	Check(Near(HeadingDegrees(1.0f, 0.0f), 0.0f), "an X axis along world +x heads 0 degrees");
	Check(Near(HeadingDegrees(0.0f, 1.0f), 90.0f), "along world +y heads 90");
	Check(Near(HeadingDegrees(-1.0f, 0.0f), 180.0f), "along world -x heads 180");
	Check(Near(HeadingDegrees(0.0f, -1.0f), -90.0f), "along world -y heads -90");
	Check(Near(HeadingDegrees(3.0f, 3.0f), 45.0f), "length does not matter, only direction");

	Check(Near(HeadingDifference(10.0f, 25.0f), 15.0f), "a plain difference");
	Check(Near(HeadingDifference(25.0f, 10.0f), -15.0f), "signed");
	Check(Near(HeadingDifference(170.0f, -170.0f), 20.0f), "wraps across 180 the short way");
	Check(Near(HeadingDifference(-170.0f, 170.0f), -20.0f), "and back");
	Check(Near(HeadingDifference(0.0f, 180.0f), 180.0f), "opposite is 180, not -180");

	// ---- the verdict ------------------------------------------------------------------------
	// VRIK activates a slot only with the hand within its activation distance ON ITS OWN BODY, so
	// at an interaction VRIK really performed, VRIK's skeleton must show the hand inside that reach.
	const float kReach = 8.0f;
	Check(Judge(18.0f, 4.0f, kReach, kPalmTolerance) == Verdict::kOnlyThirdPersonBody,
		"only the third-person body puts the hand in reach: that is VRIK's body");
	Check(Judge(4.0f, 18.0f, kReach, kPalmTolerance) == Verdict::kOnlyFirstPerson,
		"only the first-person skeleton does: evidence against the switch");
	Check(Judge(3.0f, 5.0f, kReach, kPalmTolerance) == Verdict::kBothInside,
		"both in reach: this interaction does not decide it");
	Check(Judge(30.0f, 25.0f, kReach, kPalmTolerance) == Verdict::kNeither,
		"neither: the slot-to-bone mapping or the hand position is off as well");

	// The palm tolerance: this mod measures the hand from HIGGS's palm body, VRIK from its own palm.
	Check(kPalmTolerance > 0.0f && kPalmTolerance < kReach, "the tolerance is real but smaller than VRIK's reach");
	Check(Judge(40.0f, kReach + kPalmTolerance, kReach, kPalmTolerance) == Verdict::kOnlyThirdPersonBody,
		"exactly at reach plus tolerance still counts as in reach");
	Check(Judge(40.0f, kReach + kPalmTolerance + 0.1f, kReach, kPalmTolerance) == Verdict::kNeither,
		"just past it does not");

	// The two separations the probe measured, as a sanity check on the scale of the test: a hand ON
	// the true slot of one skeleton sits about that far from the same slot on the other.
	Check(Judge(9.2f + 2.0f, 2.0f, kReach, kPalmTolerance) == Verdict::kBothInside,
		"a 9.2 split (pelvis) with the hand 2 from the slot is NOT decisive at this tolerance");
	Check(Judge(13.5f + 2.0f, 2.0f, kReach, kPalmTolerance) == Verdict::kOnlyThirdPersonBody,
		"a 13.5 split (right forearm) is");

	// ---- which skeleton a node is -----------------------------------------------------------
	// M2.2.5-test1's log caught GetNiNode() on GetNiRootNode(1) at 254.8s and on a replaced
	// GetNiRootNode(0) from 401.7s on, so nothing may assume which one it is.
	int a = 0;
	int b = 0;
	int c = 0;
	Check(WhichRoot(&a, &a, &b) == Root::kThirdPersonBody, "the node is GetNiRootNode(0)");
	Check(WhichRoot(&b, &a, &b) == Root::kFirstPerson, "the node is GetNiRootNode(1)");
	Check(WhichRoot(&a, &a, &a) == Root::kOnlyOne, "both roots are the node: one skeleton");
	Check(WhichRoot(&c, &a, &b) == Root::kNeither, "a node that is neither root");
	Check(WhichRoot(nullptr, &a, &b) == Root::kNeither, "no node at all");
	Check(WhichRoot(&a, nullptr, nullptr) == Root::kNeither, "no roots to be");

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
