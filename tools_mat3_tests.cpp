// Console tests for Mat3Rules.h: the 3x3 matrix helpers behind RawHavok.h - that they still compute
// what they always did, and above all that none of them writes one float past the end of the matrix
// it fills - without the game.
//
// WHY: the classic SDK's NiMatrix33 is `float data[3][3]`, 0x24 bytes (skse64/NiTypes.h). These helpers
// were written as if every row had a fourth, padding float: `data[r][3] = 0.0f`. For rows 0 and 1 that
// names the next row's first entry, which the loop fills in straight afterwards. For row 2 it names the
// four bytes BEHIND the matrix - whatever a struct or the compiler happened to put there. In the builds
// that shipped it was padding (in ThrowSampleHeldPose, the four bytes directly under the stack cookie)
// or a flag assigned straight afterwards (ThrowHand::haveDropPose), by luck and nothing else.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_mat3_tests.cpp /Fe:mat3_tests.exe
//   mat3_tests.exe
// Exit code 0 means every check passed.

#include "Mat3Rules.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

using namespace SwapDropAndHoldRedux::RawHavok;

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

	// Nothing the arithmetic below can produce, and nothing like the 0.0f the old code wrote.
	const float kSentinel = 12345.678f;

	// NiMatrix33's exact shape with the float behind it made part of the object: `behind` sits at 0x24,
	// the very address `data[2][3]` names. Every fresh one holds the sentinel there, so a helper that
	// writes a float too many is caught wherever its matrix lives - a local inside the helper, the
	// caller's return slot, or a field of somebody's struct. (A sentinel beside the CALLER's matrix
	// would miss the helpers that return by value: this compiler builds their result in their own
	// frame unless it is optimising.)
	struct GuardedMat
	{
		float data[3][3];
		float behind = kSentinel;
	};
	static_assert(offsetof(GuardedMat, behind) == 0x24, "the guard must sit exactly where data[2][3] lands");

	// NiMatrix33's exact shape AND size, for the case the plugin really has: a matrix that is a field,
	// with a live field right behind it.
	struct PlainMat
	{
		float data[3][3];
	};
	static_assert(sizeof(PlainMat) == 0x24, "the same 0x24 bytes as the SDK's NiMatrix33");

	struct Vec3
	{
		float x, y, z;
	};

	// A sentinel float directly after a matrix.
	struct MatThenFloat
	{
		PlainMat rot;
		float next;
	};
	static_assert(offsetof(MatThenFloat, next) == 0x24, "nothing between the matrix and its neighbour");

	// ThrowMomentum.cpp's ThrowHand, the fields that matter: haveDropPose directly behind dropRot.
	struct ReleaseShape
	{
		PlainMat dropRot;
		bool haveDropPose;
		unsigned int droppedRefID;
	};
	static_assert(offsetof(ReleaseShape, haveDropPose) == 0x24, "the flag sits where data[2][3] lands");

	template <typename M>
	void Fill(M& m, const float v[9])
	{
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
			{
				m.data[r][c] = v[r * 3 + c];
			}
		}
	}

	template <typename M>
	bool Holds(const M& m, const float v[9])
	{
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
			{
				if (!Near(m.data[r][c], v[r * 3 + c]))
				{
					return false;
				}
			}
		}
		return true;
	}

	const float kCounting[9] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f };
	const float kCountingDown[9] = { 9.0f, 8.0f, 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f };
	const float kIdentity[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
	// A quarter turn about z: x -> y, y -> -x.
	const float kQuarterTurnZ[9] = { 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

	// An hkTransform as Havok lays it out: three COLUMN hkVector4s, then the translation, each with a
	// fourth float of padding. Entry "rc" reads as the digits r+1, c+1; 999 marks every padding float.
	const float kHkTransform[16] = {
		11.0f, 21.0f, 31.0f, 999.0f,
		12.0f, 22.0f, 32.0f, 999.0f,
		13.0f, 23.0f, 33.0f, 999.0f,
		41.0f, 42.0f, 43.0f, 999.0f
	};
	const float kHkRows[9] = { 11.0f, 12.0f, 13.0f, 21.0f, 22.0f, 23.0f, 31.0f, 32.0f, 33.0f };
}

int main()
{
	// ---- Mat3Transpose ------------------------------------------------------------------------------
	{
		GuardedMat m;
		Fill(m, kCounting);
		const GuardedMat t = Mat3Transpose(m);
		const float expected[9] = { 1.0f, 4.0f, 7.0f, 2.0f, 5.0f, 8.0f, 3.0f, 6.0f, 9.0f };
		Check(Holds(t, expected), "Mat3Transpose swaps rows for columns");
		Check(Holds(Mat3Transpose(t), kCounting), "and twice is the matrix it started from");
		Check(Holds(m, kCounting), "the matrix it was given is left as it was");
		Check(t.behind == kSentinel, "Mat3Transpose leaves the float behind its result alone");
	}

	// ---- Mat3Mul ------------------------------------------------------------------------------------
	{
		GuardedMat a;
		GuardedMat b;
		GuardedMat identity;
		GuardedMat turn;
		Fill(a, kCounting);
		Fill(b, kCountingDown);
		Fill(identity, kIdentity);
		Fill(turn, kQuarterTurnZ);

		const GuardedMat ab = Mat3Mul(a, b);
		const float expectedAb[9] = { 30.0f, 24.0f, 18.0f, 84.0f, 69.0f, 54.0f, 138.0f, 114.0f, 90.0f };
		Check(Holds(ab, expectedAb), "Mat3Mul is rows of the first times columns of the second");

		const float expectedBa[9] = { 90.0f, 114.0f, 138.0f, 54.0f, 69.0f, 84.0f, 18.0f, 24.0f, 30.0f };
		Check(Holds(Mat3Mul(b, a), expectedBa), "and the other way round is a different matrix");

		Check(Holds(Mat3Mul(identity, a), kCounting), "identity on the left changes nothing");
		Check(Holds(Mat3Mul(a, identity), kCounting), "nor on the right");
		Check(Holds(Mat3Mul(turn, Mat3Transpose(turn)), kIdentity),
			"a rotation times its transpose is the identity - how ThrowMomentum takes one pose from another");
		Check(ab.behind == kSentinel, "Mat3Mul leaves the float behind its result alone");
	}

	// ---- Mat3Lerp (the blend inside EmbedPins.cpp's BlendRot) ------------------------------------------
	{
		GuardedMat a;
		GuardedMat b;
		Fill(a, kCounting);
		Fill(b, kCountingDown);

		Check(Holds(Mat3Lerp(a, b, 0.0f), kCounting), "Mat3Lerp at 0 is the first matrix");
		Check(Holds(Mat3Lerp(a, b, 1.0f), kCountingDown), "at 1 it is the second");

		const GuardedMat quarter = Mat3Lerp(a, b, 0.25f);
		const float expected[9] = { 3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 5.5f, 6.0f, 6.5f, 7.0f };
		Check(Holds(quarter, expected), "a quarter of the way is three parts first to one part second");
		Check(quarter.behind == kSentinel, "Mat3Lerp leaves the float behind its result alone");
	}

	// ---- HkTransformToRows (the body of RawHavok::ReadHkTransform) -------------------------------------
	{
		GuardedMat rot;
		Vec3 pos = { 0.0f, 0.0f, 0.0f };
		HkTransformToRows(kHkTransform, rot, pos);
		Check(Holds(rot, kHkRows), "Havok's three columns come out as three rows, and no padding float with them");
		Check(Near(pos.x, 41.0f) && Near(pos.y, 42.0f) && Near(pos.z, 43.0f), "the translation follows the columns");
		Check(rot.behind == kSentinel, "HkTransformToRows leaves the float behind the matrix alone");
	}

	// A sentinel float directly after a real, 0x24-byte matrix: read into the matrix, look at the float.
	{
		MatThenFloat target;
		Fill(target.rot, kIdentity);
		target.next = kSentinel;
		Vec3 pos = { 0.0f, 0.0f, 0.0f };
		HkTransformToRows(kHkTransform, target.rot, pos);
		Check(Holds(target.rot, kHkRows), "the matrix is read into a struct's field just the same");
		Check(target.next == kSentinel, "and the field behind it is not touched");
	}

	// ThrowCaptureRelease reads the weapon's pose into ThrowHand::dropRot, and haveDropPose is the very
	// next field. It only survived because the call's own result was assigned to it afterwards.
	{
		ReleaseShape hand;
		Fill(hand.dropRot, kIdentity);
		hand.haveDropPose = true;
		hand.droppedRefID = 0xFF001234u;
		Vec3 pos = { 0.0f, 0.0f, 0.0f };
		HkTransformToRows(kHkTransform, hand.dropRot, pos);
		Check(hand.haveDropPose, "reading a pose into dropRot does not clear the flag behind it");
		Check(hand.droppedRefID == 0xFF001234u, "nor anything after that");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
