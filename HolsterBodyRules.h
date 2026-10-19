#pragma once

// Measuring holsters from the third-person body, and judging from real VRIK interactions which
// skeleton VRIK's slots belong to. No engine types in here, so tools_holster_body_tests.cpp can
// check it outside the game; VrikHolsters.cpp feeds it the live bones and distances.
//
// THE FINDING THIS TESTS (M2.2.3-test2 and M2.2.5-test1, 2026-09-16). The player has two
// skeletons. GetNiRootNode(0) and GetNiRootNode(1) are different nodes whose every body bone sits
// 8 to 39 units apart - pelvis 9.2, spine2 11.4, right forearm 13.5 in the first probe - while
// BOTH HANDS sit in the identical place on the two. Every holster calculation in this mod has
// always looked its anchor bones up under player->GetNiNode(), and that is NOT fixed to either
// root: M2.2.5-test1's log caught it on GetNiRootNode(1) at 254.8s and on a replaced
// GetNiRootNode(0) from 401.7s on, in one session. The hands matching and the body not is what
// VRIK pinning its avatar's hands to the controllers would look like, which makes the third-person
// skeleton the likely owner of VRIK's holster slots. Likely, not proven: that is what
// bHolsterThirdPersonBody and the A/B log are for.

#include <cmath>

namespace SwapDropAndHoldRedux
{
	namespace HolsterBody
	{
		// Where a slot's centre comes from.
		enum class Centre
		{
			kIni,     // VRIK's own position from vrikslots.ini
			kLearned, // measured off real draws and holsters
			kSeeded,  // borrowed from a learned slot on the same bone
		};

		// Off - the default - is exactly the behaviour before this setting. On, VRIK's own numbers
		// apply as they are: the learned and seeded corrections were measured against the OTHER
		// skeleton and mean nothing on this one. If the third-person body really is VRIK's, those
		// numbers need no correcting at all.
		inline Centre SourceFor(const bool onThirdPersonBody, const bool learned, const bool seeded)
		{
			if (onThirdPersonBody)
			{
				return Centre::kIni;
			}
			if (learned)
			{
				return Centre::kLearned;
			}
			if (seeded)
			{
				return Centre::kSeeded;
			}
			return Centre::kIni;
		}

		// Whether the tight buzz may use a slot's centre. On the third-person body every slot is
		// VRIK's own position on - if the finding holds - VRIK's own body, which is the precise
		// claim under test: the buzz lines up with VRIK everywhere, right shoulder included, or it
		// visibly does not.
		inline bool CentreTrusted(const bool onThirdPersonBody, const bool learnedCentreTrusted)
		{
			return onThirdPersonBody || learnedCentreTrusted;
		}

		// Never learn on the third-person body. SwapDropAndHoldRedux_holsters.ini holds positions
		// measured on the first-person skeleton; adding the other skeleton's would corrupt every
		// slot it touched, and turning the setting back off must find that file as it was.
		inline bool MayLearn(const bool onThirdPersonBody)
		{
			return !onThirdPersonBody;
		}

		// Which skeleton a node is. GetNiNode() is NOT fixed to either: M2.2.5-test1's log caught it on
		// GetNiRootNode(1) at 254.8s and on a REPLACED GetNiRootNode(0) from 401.7s on, in one session.
		// That test's A/B compared GetNiNode() against GetNiRootNode(0), so once they were the same
		// object it measured one skeleton twice and every comparison came back empty.
		enum class Root
		{
			kNeither,
			kThirdPersonBody, // GetNiRootNode(0)
			kFirstPerson,     // GetNiRootNode(1)
			kOnlyOne,         // both roots are this node: there is one skeleton
		};

		inline Root WhichRoot(const void* const node, const void* const root0, const void* const root1)
		{
			if (!node)
			{
				return Root::kNeither;
			}
			const bool isRoot0 = node == root0;
			const bool isRoot1 = node == root1;
			if (isRoot0 && isRoot1)
			{
				return Root::kOnlyOne;
			}
			if (isRoot0)
			{
				return Root::kThirdPersonBody;
			}
			if (isRoot1)
			{
				return Root::kFirstPerson;
			}
			return Root::kNeither;
		}

		inline const char* RootName(const Root root)
		{
			switch (root)
			{
			case Root::kThirdPersonBody:
				return "the third-person body, GetNiRootNode(0)";
			case Root::kFirstPerson:
				return "the first-person skeleton, GetNiRootNode(1)";
			case Root::kOnlyOne:
				return "both roots - there is one skeleton";
			default:
				return "neither root";
			}
		}

		// A bone's heading from the world direction of its local X axis - column 0 of its world
		// rotation, since world = R * local + pos. Degrees, -180 to 180.
		inline float HeadingDegrees(const float axisWorldX, const float axisWorldY)
		{
			return static_cast<float>(std::atan2(axisWorldY, axisWorldX) * 180.0 / 3.14159265358979323846);
		}

		// b - a, wrapped to -180..180 the short way. Opposite headings read 180.
		inline float HeadingDifference(const float a, const float b)
		{
			float d = b - a;
			while (d > 180.0f)
			{
				d -= 360.0f;
			}
			while (d <= -180.0f)
			{
				d += 360.0f;
			}
			return d;
		}

		// This mod measures the hand from HIGGS's palm body; VRIK measures from its own idea of the
		// palm. They differ by a few units - the 2.1.x trace caught reaches landing 3 to 5 units
		// apart on the two hips - so a hand that far outside VRIK's reach still counts as in it.
		constexpr float kPalmTolerance = 4.0f;

		enum class Verdict
		{
			kBothInside,         // does not decide it
			kOnlyFirstPerson,    // evidence AGAINST the third-person body
			kOnlyThirdPersonBody,// evidence FOR it
			kNeither,            // the slot-to-bone mapping, or the hand position, is off as well
		};

		// VRIK activates a slot only with the hand within its activation distance of that slot on its
		// own body. So at an interaction VRIK really performed, the skeleton VRIK uses has to show
		// the hand inside that reach.
		inline Verdict Judge(
			const float fromFirstPerson,
			const float fromThirdPersonBody,
			const float activationDistance,
			const float tolerance)
		{
			const float reach = activationDistance + tolerance;
			const bool firstPersonInside = fromFirstPerson <= reach;
			const bool bodyInside = fromThirdPersonBody <= reach;
			if (firstPersonInside && bodyInside)
			{
				return Verdict::kBothInside;
			}
			if (firstPersonInside)
			{
				return Verdict::kOnlyFirstPerson;
			}
			if (bodyInside)
			{
				return Verdict::kOnlyThirdPersonBody;
			}
			return Verdict::kNeither;
		}
	}
}
