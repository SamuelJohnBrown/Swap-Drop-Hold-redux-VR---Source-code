// Console tests for GrabPoseRules.h: where along a weapon the hand took hold of it, whether it was
// taken hold of backwards, and the pose HIGGS should hold it in - all without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_grab_pose_tests.cpp /Fe:grab_pose_tests.exe
//   grab_pose_tests.exe
// Exit code 0 means every check passed.

#include "GrabPoseRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux::GrabPose;

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

	bool Near(const float a, const float b, const float eps = 0.0005f)
	{
		return std::fabs(a - b) <= eps;
	}

	bool NearVec(const Vec3& v, const float x, const float y, const float z, const float eps = 0.0005f)
	{
		return Near(v.x, x, eps) && Near(v.y, y, eps) && Near(v.z, z, eps);
	}

	bool NearMat(const Mat3& a, const Mat3& b, const float eps = 0.0005f)
	{
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
			{
				if (!Near(a.m[r][c], b.m[r][c], eps))
				{
					return false;
				}
			}
		}
		return true;
	}

	// Hand-written rotations, so nothing here is computed by the code under test.
	Mat3 RotZ90()
	{
		Mat3 r;
		r.m[0][0] = 0.0f; r.m[0][1] = -1.0f; r.m[0][2] = 0.0f;
		r.m[1][0] = 1.0f; r.m[1][1] = 0.0f;  r.m[1][2] = 0.0f;
		r.m[2][0] = 0.0f; r.m[2][1] = 0.0f;  r.m[2][2] = 1.0f;
		return r;
	}

	Mat3 RotX90()
	{
		Mat3 r;
		r.m[0][0] = 1.0f; r.m[0][1] = 0.0f; r.m[0][2] = 0.0f;
		r.m[1][0] = 0.0f; r.m[1][1] = 0.0f; r.m[1][2] = -1.0f;
		r.m[2][0] = 0.0f; r.m[2][1] = 1.0f; r.m[2][2] = 0.0f;
		return r;
	}

	Mat3 Diag(const float a, const float b, const float c)
	{
		Mat3 r;
		r.m[0][0] = a; r.m[1][1] = b; r.m[2][2] = c;
		return r;
	}

	// An iron-sword-like collision box in metres: 4 cm thick, 10 cm wide at the guard, a metre long,
	// with the shape's own origin 10 cm from the pommel end - where Skyrim weapon meshes keep the grip.
	Box SwordBox()
	{
		Box b;
		b.lo = Make(-0.02f, -0.05f, -0.10f);
		b.hi = Make(0.02f, 0.05f, 0.90f);
		return b;
	}

	// Blade direction at `degrees` from +Z, tilted toward +X.
	Vec3 Tilted(const float degrees)
	{
		const float rad = degrees * 3.14159265f / 180.0f;
		return Make(std::sin(rad), 0.0f, std::cos(rad));
	}
}

int main()
{
	// ---- vectors and rotations: v' = M v, rows first --------------------------------------------
	Check(NearVec(Make(1.0f, 2.0f, 3.0f), 1.0f, 2.0f, 3.0f), "Make builds the vector it was given");
	Check(Near(Get(Make(1.0f, 2.0f, 3.0f), 0), 1.0f) && Near(Get(Make(1.0f, 2.0f, 3.0f), 2), 3.0f), "Get reads a component by index");
	Check(NearVec(Add(Make(1, 2, 3), Make(10, 20, 30)), 11, 22, 33), "Add");
	Check(NearVec(Sub(Make(10, 20, 30), Make(1, 2, 3)), 9, 18, 27), "Sub");
	Check(NearVec(Scale(Make(1, -2, 3), 2.0f), 2, -4, 6), "Scale");
	Check(Near(Dot(Make(1, 2, 3), Make(4, -5, 6)), 12.0f), "Dot");
	Check(Near(Length(Make(3, 4, 12)), 13.0f), "Length");
	Check(NearVec(AxisUnit(0, 1), 1, 0, 0) && NearVec(AxisUnit(2, -1), 0, 0, -1), "AxisUnit is a signed cardinal");
	Check(NearVec(Mul(Identity(), Make(1, 2, 3)), 1, 2, 3), "the identity leaves a vector alone");
	Check(NearVec(Mul(RotZ90(), Make(1, 0, 0)), 0, 1, 0), "a quarter turn about Z takes +X to +Y");
	Check(NearMat(Mul(Transpose(RotZ90()), RotZ90()), Identity()), "a rotation's transpose undoes it");
	Check(NearVec(Mul(Mul(RotZ90(), RotX90()), Make(0, 1, 0)), 0, 0, 1), "a product applies the right-hand factor first");
	Check(NearVec(Mul(HalfTurnAbout(Make(0, 0, 1)), Make(1, 0, 0)), -1, 0, 0), "a half turn reverses what is across its axis");
	Check(NearVec(Mul(HalfTurnAbout(Make(0, 0, 1)), Make(0, 0, 1)), 0, 0, 1), "and keeps what is along it");
	Check(NearMat(Mul(HalfTurnAbout(Make(1, 0, 0)), HalfTurnAbout(Make(1, 0, 0))), Identity()), "two half turns are no turn");

	// ---- transforms compose the way the engine's do: world = parent * child ----------------------
	{
		Xform parent; parent.rot = RotZ90(); parent.pos = Make(10, 0, 0); parent.scale = 2.0f;
		Xform child; child.rot = Identity(); child.pos = Make(1, 0, 0); child.scale = 1.0f;
		const Xform world = Compose(parent, child);
		Check(NearVec(world.pos, 10, 2, 0), "a child 1 along +X of a parent turned 90 and scaled 2 sits 2 along world +Y");
		Check(Near(world.scale, 2.0f), "scales multiply");
		Check(NearVec(Apply(parent, Make(1, 0, 0)), 10, 2, 0), "Apply matches Compose for a point");

		const Xform back = Compose(parent, Inverse(parent));
		Check(NearMat(back.rot, Identity()) && NearVec(back.pos, 0, 0, 0) && Near(back.scale, 1.0f),
			"a transform times its inverse is the identity, rotation, position and scale");

		Xform grand; grand.rot = RotX90(); grand.pos = Make(0, 5, 0); grand.scale = 1.0f;
		const Vec3 p = Make(0.5f, -1.0f, 2.0f);
		const Vec3 viaCompose = Apply(Compose(grand, parent), p);
		const Vec3 viaSteps = Apply(grand, Apply(parent, p));
		Check(NearVec(viaCompose, viaSteps.x, viaSteps.y, viaSteps.z), "composing then applying equals applying twice");
		// By hand: RotZ90 * (0.5,-1,2) = (1,0.5,2), times 2 plus (10,0,0) = (12,1,4);
		// RotX90 * (12,1,4) = (12,-4,1), plus (0,5,0) = (12,1,1).
		Check(NearVec(viaSteps, 12.0f, 1.0f, 1.0f), "and lands where hand arithmetic says: (12, 1, 1)");
	}

	// ---- which way the weapon runs -------------------------------------------------------------
	{
		const LongAxis axis = FindLongAxis(SwordBox(), 0.05f);
		Check(axis.usable, "a sword-shaped box has a long axis");
		Check(axis.index == 2, "and it is the metre-long one");
		Check(Near(axis.length, 1.0f) && Near(axis.centre, 0.4f) && Near(axis.half, 0.5f), "length 1.0, centre 0.4, half 0.5");
		Check(Near(axis.ratio, 10.0f), "ten times longer than its next extent");
		Check(!axis.ambiguous, "so nothing is ambiguous about it");

		Box sideways; sideways.lo = Make(-0.3f, -0.02f, -0.02f); sideways.hi = Make(0.5f, 0.02f, 0.02f);
		Check(FindLongAxis(sideways, 0.05f).index == 0, "a box long in X reports axis 0");

		Box cube; cube.lo = Make(-0.1f, -0.1f, -0.1f); cube.hi = Make(0.1f, 0.1f, 0.1f);
		Check(FindLongAxis(cube, 0.05f).ambiguous, "a cube has no long axis worth trusting");

		Box stubby; stubby.lo = Make(0, 0, 0); stubby.hi = Make(0.10f, 0.10f, 0.12f);
		Check(FindLongAxis(stubby, 0.05f).ambiguous, "1.2 times longer is still ambiguous");

		Box edge; edge.lo = Make(0, 0, 0); edge.hi = Make(0.10f, 0.08f, 0.125f);
		Check(!FindLongAxis(edge, 0.05f).ambiguous, "exactly 1.25 times longer is accepted");

		Box tiny; tiny.lo = Make(0, 0, 0); tiny.hi = Make(0.001f, 0.001f, 0.03f);
		Check(!FindLongAxis(tiny, 0.05f).usable, "a shape shorter than the minimum length is not a weapon to measure");

		Box inverted; inverted.lo = Make(1, 1, 1); inverted.hi = Make(0, 0, 0);
		Check(!FindLongAxis(inverted, 0.05f).usable, "an inside-out box is refused");
	}

	// ---- where along it the hand is ---------------------------------------------------------------
	{
		const Box box = SwordBox();
		const LongAxis axis = FindLongAxis(box, 0.05f);
		Check(Near(AlongU(axis, Make(0, 0, 0.90f)), 1.0f), "the high end is +1");
		Check(Near(AlongU(axis, Make(0, 0, -0.10f)), -1.0f), "the low end is -1");
		Check(Near(AlongU(axis, Make(0, 0, 0.40f)), 0.0f), "the middle is 0");
		Check(Near(AlongU(axis, Make(0.5f, -0.3f, 0.65f)), 0.5f), "only the coordinate along the axis counts");
		Check(Near(AlongU(axis, Make(0, 0, 1.40f)), 2.0f), "a point past the end is not clamped here");
		Check(Near(HiltU(axis), -0.8f), "the shape's own origin - the hilt - sits at -0.8 on this sword");

		Check(Near(OffAxis(axis, box, Make(0, 0, 0.3f)), 0.0f), "a point on the centre line is on the axis");
		Check(Near(OffAxis(axis, box, Make(0.03f, 0, 0.3f)), 0.03f), "3 cm to the side is 3 cm off it");
		Check(Near(OffAxis(axis, box, Make(0.03f, 0.04f, 0.9f)), 0.05f), "3 and 4 make 5, wherever along it");

		Box offCentre; offCentre.lo = Make(0.10f, -0.02f, -0.1f); offCentre.hi = Make(0.20f, 0.02f, 0.9f);
		Check(Near(OffAxis(FindLongAxis(offCentre, 0.05f), offCentre, Make(0.15f, 0.0f, 0.2f)), 0.0f),
			"the axis runs through the box's centre, not the origin");
	}

	// ---- which end is the tip: the one further from the hilt ------------------------------------
	Check(TipSign(-0.8f) == 1, "hilt near the low end: the tip is the high end");
	Check(TipSign(0.8f) == -1, "hilt near the high end: the tip is the low end");
	Check(TipSign(0.10f) == 0, "a hilt in the middle says nothing about which end is the tip");
	Check(TipSign(-0.15f) == 0 && TipSign(0.15f) == 0, "the dead zone includes its edges");
	Check(TipSign(-0.16f) == 1, "just outside it counts");

	Check(Near(PercentFromHilt(1.0f, -0.8f, 1), 100.0f), "at the tip: 100% up from the hilt");
	Check(Near(PercentFromHilt(-0.8f, -0.8f, 1), 0.0f), "at the hilt: 0%");
	Check(Near(PercentFromHilt(0.1f, -0.8f, 1), 50.0f), "halfway between them: 50%");
	Check(Near(PercentFromHilt(-1.0f, 0.8f, -1), 100.0f), "the same on a mesh whose tip is the low end");
	Check(Near(PercentFromHilt(-1.0f, -0.8f, 1), -11.111f, 0.01f), "behind the hilt, on the pommel, is negative");
	Check(Near(PercentFromHilt(0.3f, 0.0f, 0), 0.0f), "with no known tip there is no percentage");

	// ---- a world point in the weapon's own frame --------------------------------------------------
	Check(NearVec(ToLocal(RotZ90(), Make(1, 2, 3), Make(1, 3, 3)), 1, 0, 0),
		"one unit along world +Y from a body turned 90 about Z is one unit along its own +X");
	Check(NearVec(ToLocal(Identity(), Make(1, 2, 3), Make(1, 2, 3)), 0, 0, 0), "the body's own position is its origin");

	// ---- angles ---------------------------------------------------------------------------------
	Check(Near(AngleDeg(Make(0, 0, 1), Make(0, 0, 5)), 0.0f, 0.01f), "the same direction: 0");
	Check(Near(AngleDeg(Make(0, 0, 1), Make(0, 0, -2)), 180.0f, 0.01f), "opposite: 180");
	Check(Near(AngleDeg(Make(0, 0, 1), Make(3, 0, 0)), 90.0f, 0.01f), "across: 90");
	Check(Near(AngleDeg(Make(0, 0, 1), Tilted(30.0f)), 30.0f, 0.01f), "30 is 30");
	Check(Near(AngleDeg(Make(0, 0, 0), Make(0, 0, 1)), 0.0f), "no direction is no angle");

	// How far apart two orientations are: what "has HIGGS moved it yet" and "did the hold arrive" read.
	Check(Near(AngleBetweenDeg(RotZ90(), RotZ90()), 0.0f, 0.01f), "the same orientation: 0 apart");
	Check(Near(AngleBetweenDeg(Identity(), RotZ90()), 90.0f, 0.01f), "a quarter turn: 90 apart");
	Check(Near(AngleBetweenDeg(Identity(), Diag(1, -1, -1)), 180.0f, 0.01f), "a half turn: 180 apart");
	Check(Near(AngleBetweenDeg(RotX90(), Mul(RotX90(), RotZ90())), 90.0f, 0.01f), "it is the turn between them, whatever they share");

	// ---- was it taken hold of backwards? only a near-exact reverse counts ----------------------------
	Check(Near(ClampCone(10.0f), 20.0f) && Near(ClampCone(45.0f), 45.0f) && Near(ClampCone(90.0f), 90.0f) &&
		Near(ClampCone(120.0f), 90.0f), "the cone is kept between 20 and 90");
	{
		const Vec3 natural = Make(0, 0, 1);
		Check(JudgeReversed(Tilted(180.0f), natural, 45.0f).reversed, "dead backwards is reversed");
		Check(JudgeReversed(Tilted(150.0f), natural, 45.0f).reversed, "30 off dead backwards is reversed");
		Check(JudgeReversed(Tilted(135.5f), natural, 45.0f).reversed, "just inside the 45 cone is reversed");
		Check(!JudgeReversed(Tilted(134.0f), natural, 45.0f).reversed, "just outside it is a normal grab");
		Check(!JudgeReversed(Tilted(90.0f), natural, 45.0f).reversed, "sideways is a normal grab, never half way");
		Check(!JudgeReversed(Tilted(0.0f), natural, 45.0f).reversed, "the natural way round is a normal grab");
		Check(Near(JudgeReversed(Tilted(150.0f), natural, 45.0f).angleDeg, 150.0f, 0.05f), "and the angle is reported");
		Check(JudgeReversed(Tilted(161.0f), natural, 20.0f).reversed && !JudgeReversed(Tilted(158.0f), natural, 20.0f).reversed,
			"a 20 cone wants it within 20 of dead backwards");
		Check(JudgeReversed(Tilted(101.0f), natural, 80.0f).reversed && !JudgeReversed(Tilted(98.0f), natural, 80.0f).reversed,
			"an 80 cone takes anything past 100");
		Check(JudgeReversed(Tilted(91.0f), natural, 500.0f).reversed && !JudgeReversed(Tilted(89.0f), natural, 500.0f).reversed,
			"a silly cone is clamped to 90 first");
		// M3.0.0-test8, the player's own rule: "if its closer to 180 then reverse if not then normal".
		Check(JudgeReversed(Tilted(91.0f), natural, 90.0f).reversed && !JudgeReversed(Tilted(89.0f), natural, 90.0f).reversed,
			"a 90 cone is the round-up: nearer backwards than forwards is reversed");
		Check(JudgeReversed(Tilted(179.0f), natural, 90.0f).reversed && !JudgeReversed(Tilted(1.0f), natural, 90.0f).reversed,
			"and the two ends are still what they were");
		Check(!JudgeReversed(Make(0, 0, 0), natural, 45.0f).reversed, "no blade direction, no verdict");
	}

	// ---- which way to turn it over ------------------------------------------------------------
	{
		// Long axis Z. Turning the natural pose over about X gives diag(1,-1,-1); about Y, diag(-1,1,-1).
		const FlipChoice aboutX = ChooseFlipAxis(Identity(), Diag(1, -1, -1), 2);
		Check(aboutX.decided && aboutX.axis == 0, "held like the natural pose turned over about X: flip about X");
		const FlipChoice aboutY = ChooseFlipAxis(Identity(), Diag(-1, 1, -1), 2);
		Check(aboutY.decided && aboutY.axis == 1, "turned over about Y: flip about Y");
		Check(aboutX.margin > 0.5f, "a clean case is decided by a wide margin");

		// Half way between the two: a half turn about the (1,1,0) diagonal.
		Mat3 between;
		between.m[0][0] = 0.0f; between.m[0][1] = 1.0f; between.m[0][2] = 0.0f;
		between.m[1][0] = 1.0f; between.m[1][1] = 0.0f; between.m[1][2] = 0.0f;
		between.m[2][0] = 0.0f; between.m[2][1] = 0.0f; between.m[2][2] = -1.0f;
		Check(!ChooseFlipAxis(Identity(), between, 2).decided, "exactly between the two is left undecided");

		// A natural pose that is itself rotated: the choice is made in the weapon's own frame.
		const Mat3 natural = RotZ90();
		const Mat3 actual = Mul(natural, Diag(1, -1, -1));
		Check(ChooseFlipAxis(natural, actual, 2).axis == 0, "the natural pose's own rotation does not change the answer");

		// Long axis X: the candidates are Y and Z.
		const FlipChoice longX = ChooseFlipAxis(Identity(), Diag(-1, 1, -1), 0);
		Check(longX.decided && longX.axis == 1, "with X as the long axis, a turn about Y is recognised");
		const FlipChoice longX2 = ChooseFlipAxis(Identity(), Diag(-1, -1, 1), 0);
		Check(longX2.decided && longX2.axis == 2, "and a turn about Z");
	}

	// ---- the pose HIGGS should hold it in ---------------------------------------------------------
	{
		// Natural hold: weapon node at the hand's origin, unrotated. Palm 5 units up the hand's +Z, which
		// is 5 units up the weapon's long axis.
		Xform natural; natural.rot = Identity(); natural.pos = Make(0, 0, 0); natural.scale = 1.0f;
		const Vec3 palm = Make(0, 0, 5);

		const Xform same = HoldPose(natural, palm, 2, 5.0f, false, 0);
		Check(NearMat(same.rot, natural.rot) && NearVec(same.pos, 0, 0, 0), "touched where the natural grip is: the natural hold, unchanged");

		const Xform slid = HoldPose(natural, palm, 2, 25.0f, false, 0);
		Check(NearVec(slid.pos, 0, 0, -20), "touched 20 further up the blade: the weapon moves 20 down through the hand");
		Check(NearVec(Apply(Inverse(slid), palm), 0, 0, 25), "and the palm now sits at 25 along it");
		Check(NearMat(slid.rot, natural.rot), "a slide does not turn the weapon");

		const Xform pommel = HoldPose(natural, palm, 2, -3.0f, false, 0);
		Check(NearVec(Apply(Inverse(pommel), palm), 0, 0, -3), "a touch below the origin is honoured too");
	}
	{
		// A natural hold that is rotated, offset and scaled, with the long axis on X.
		Xform natural; natural.rot = Mul(RotZ90(), RotX90()); natural.pos = Make(1.5f, -2.0f, 0.75f); natural.scale = 2.0f;
		const Vec3 palm = Make(0.0f, -2.4f, 6.0f);
		const Vec3 before = Apply(Inverse(natural), palm);

		const Xform slid = HoldPose(natural, palm, 0, before.x + 7.0f, false, 1);
		const Vec3 after = Apply(Inverse(slid), palm);
		Check(Near(after.x, before.x + 7.0f, 0.002f), "rotated, offset and scaled: the palm lands at the touched coordinate");
		Check(Near(after.y, before.y, 0.002f) && Near(after.z, before.z, 0.002f), "and nowhere else moves");
		Check(NearMat(slid.rot, natural.rot) && Near(slid.scale, 2.0f), "rotation and scale are untouched by a slide");

		const Xform flipped = HoldPose(natural, palm, 0, before.x + 7.0f, true, 1);
		const Vec3 bladeBefore = Mul(slid.rot, AxisUnit(0, 1));
		const Vec3 bladeAfter = Mul(flipped.rot, AxisUnit(0, 1));
		Check(NearVec(bladeAfter, -bladeBefore.x, -bladeBefore.y, -bladeBefore.z, 0.002f), "reversed: the blade points exactly the other way");
		const Vec3 pivot = Apply(Inverse(flipped), palm);
		Check(NearVec(pivot, after.x, after.y, after.z, 0.003f), "and the palm stays on the same spot of the weapon - it turned about the hand");
		const Vec3 axisBefore = Mul(slid.rot, AxisUnit(1, 1));
		const Vec3 axisAfter = Mul(flipped.rot, AxisUnit(1, 1));
		Check(NearVec(axisAfter, axisBefore.x, axisBefore.y, axisBefore.z, 0.002f), "the axis it turned about keeps its direction");
		Check(Near(flipped.scale, 2.0f), "scale survives a flip");

		const Xform flippedOther = HoldPose(natural, palm, 0, before.x + 7.0f, true, 2);
		const Vec3 otherBlade = Mul(flippedOther.rot, AxisUnit(0, 1));
		Check(NearVec(otherBlade, -bladeBefore.x, -bladeBefore.y, -bladeBefore.z, 0.002f), "turning about the other perpendicular reverses the blade too");
		Check(!NearMat(flippedOther.rot, flipped.rot, 0.01f), "but it is a different hold - the edge faces the other way");
	}

	// ---- when to record nothing ------------------------------------------------------------------
	Check(!Warped(0.5f, 1.0f), "node and body together: HIGGS has not moved it yet");
	Check(Warped(10.0f, 1.0f), "the body 10 units from its node: HIGGS already warped it to the hand");
	Check(Warped(0.5f, 20.0f), "the body turned 20 degrees from its node: the same");
	Check(!Warped(kWarpDistance, kWarpAngleDeg), "exactly at the tolerances is still together");

	Check(!WasPulled(-1.0f, kPulledWindowSeconds), "never pulled");
	Check(WasPulled(1.0f, kPulledWindowSeconds), "pulled a second ago: where it lands in the hand is chance, not a choice");
	Check(WasPulled(3.0f, kPulledWindowSeconds), "the window includes its edge");
	Check(!WasPulled(3.1f, kPulledWindowSeconds), "pulled a while ago and since put down");

	for (int type = 0; type <= 9; ++type)
	{
		const bool grip = type >= 1 && type <= 6;
		const bool oneHanded = type >= 1 && type <= 4;
		char what[96];
		std::snprintf(what, sizeof(what), "weapon type %d: grab position %s, reversed %s", type, grip ? "yes" : "no", oneHanded ? "yes" : "no");
		Check(IsGripMelee(type) == grip && IsOneHandedMelee(type) == oneHanded, what);
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
