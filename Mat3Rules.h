#pragma once

// The 3x3 matrix helpers behind RawHavok.h. No engine types in here, so tools_mat3_tests.cpp can run
// them outside the game: each one works on anything with a `float data[3][3]`, which the plugin's
// NiMatrix33 callers deduce without noticing.
//
// THE RULE: the classic SDK's NiMatrix33 is `float data[3][3]` - nine floats, 0x24 bytes
// (skse64/NiTypes.h, STATIC_ASSERT). A row has NO fourth, padding float; Havok's hkVector4 columns do,
// and that is where the idea came from. These loops used to end each row with `data[r][3] = 0.0f`.
// For rows 0 and 1 that is the next row's first entry, filled in straight afterwards; for row 2 it is
// the four bytes BEHIND the matrix. The builds that shipped got away with it - padding under the stack
// cookie in ThrowSampleHeldPose, and ThrowHand::haveDropPose, which was assigned straight afterwards -
// by luck. Never index a column 3.

namespace SwapDropAndHoldRedux
{
	namespace RawHavok
	{
		template <typename M>
		inline M Mat3Transpose(const M& m)
		{
			M t;
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
				{
					t.data[r][c] = m.data[c][r];
				}
			}
			return t;
		}

		template <typename M>
		inline M Mat3Mul(const M& a, const M& b)
		{
			M m;
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
				{
					m.data[r][c] =
						a.data[r][0] * b.data[0][c] +
						a.data[r][1] * b.data[1][c] +
						a.data[r][2] * b.data[2][c];
				}
			}
			return m;
		}

		// Entry by entry from a (t = 0) to b (t = 1). Not a rotation until it is orthonormalised again:
		// EmbedPins.cpp's BlendRot does that.
		template <typename M>
		inline M Mat3Lerp(const M& a, const M& b, const float t)
		{
			M m;
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
				{
					m.data[r][c] = a.data[r][c] * (1.0f - t) + b.data[r][c] * t;
				}
			}
			return m;
		}

		// An hkTransform's sixteen floats - the rotation as three COLUMN hkVector4s, then the
		// translation - into a row-major rotation and a position. Units are left as they come.
		template <typename M, typename P>
		inline void HkTransformToRows(const float* t, M& outRot, P& outPos)
		{
			const float* c0 = t + 0;
			const float* c1 = t + 4;
			const float* c2 = t + 8;
			const float* tr = t + 12;
			for (int r = 0; r < 3; ++r)
			{
				outRot.data[r][0] = c0[r];
				outRot.data[r][1] = c1[r];
				outRot.data[r][2] = c2[r];
			}
			outPos.x = tr[0];
			outPos.y = tr[1];
			outPos.z = tr[2];
		}
	}
}
