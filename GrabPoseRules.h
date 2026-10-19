#pragma once

// Where along a weapon the hand took hold of it, whether it was taken hold of backwards, and the pose
// HIGGS should hold it in - the sensing half of 3.0.0's "held where you grabbed it".
//
// What the game gives us, and when (HIGGS 1.10.10, hand.cpp):
//  - HIGGS's grabbed callback fires at the end of TransitionHeld (:1702), BEFORE the weapon has moved,
//    so the weapon's own body is still where the player reached for it. That instant is the only time
//    the player's real touch can be measured. A grab HIGGS warps to the hand first (GrabExternalObject,
//    :1569) has no such instant, and a weapon pulled through the air lands in the hand by chance -
//    Warped and WasPulled say when to record nothing.
//  - With UseAttachPointForInitialGrab = 1 (HIGGS's default) the grab transform HIGGS reports in that
//    callback is the AS-IF-EQUIPPED pose (:1044-1107, snapped to the palm at :1543-1547). So it is the
//    natural hold, handed to us for free, and the player's touch is NOT in it.
//  - HIGGS rebuilds its hold from that transform every frame (:3917-3926), so writing HoldPose back
//    through SetGrabTransform re-poses the held weapon.
//
// The weapon's own frame is its collision shape's: the long axis is the shape-LOCAL box's dominant
// extent, and the shape's origin is the hilt (Skyrim weapon meshes keep their origin at the grip).
// Distances along it are normalised to the box - "alongU", -1 at the low end to +1 at the high end -
// because the equipped copy of the same weapon is a different size: HIGGS scales the hit shape by the
// player's hand size (:744-749), so metres measured on the loose weapon mean nothing on the equipped one.
//
// A weapon is only ever held the natural way round or exactly reversed, never in between: JudgeReversed
// is a yes or a no, and HoldPose's flip is an exact half turn about the hand.
//
// No engine types in here, so tools_grab_pose_tests.cpp can drive it outside the game. Rotations are
// row-first and act on columns (v' = M v); transforms compose as the engine's do (world = parent * child).

#include <cmath>

namespace SwapDropAndHoldRedux
{
	namespace GrabPose
	{
		struct Vec3
		{
			float x = 0.0f, y = 0.0f, z = 0.0f;
		};

		struct Mat3
		{
			float m[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
		};

		struct Xform
		{
			Mat3 rot;
			Vec3 pos;
			float scale = 1.0f;
		};

		// A collision shape's box in the shape's OWN frame, any length unit.
		struct Box
		{
			Vec3 lo, hi;
		};

		struct LongAxis
		{
			int index = 0;        // 0/1/2: the local cardinal the weapon runs along
			float length = 0.0f;  // the box's extent along it
			float centre = 0.0f;  // the box's middle along it
			float half = 0.0f;    // length / 2
			float ratio = 0.0f;   // length over the next-longest extent
			bool usable = false;  // a real box, long enough to measure
			bool ambiguous = false; // not clearly longer one way than another - hold it the natural way
		};

		struct Reverse
		{
			float angleDeg = 0.0f;
			bool reversed = false;
		};

		struct FlipChoice
		{
			int axis = 0;
			float margin = 0.0f;
			bool decided = false;
		};

		// ---- vectors -----------------------------------------------------------------------------
		inline Vec3 Make(const float x, const float y, const float z)
		{
			Vec3 v;
			v.x = x; v.y = y; v.z = z;
			return v;
		}

		inline float Get(const Vec3& v, const int i)
		{
			return i == 0 ? v.x : (i == 1 ? v.y : v.z);
		}

		inline Vec3 Add(const Vec3& a, const Vec3& b) { return Make(a.x + b.x, a.y + b.y, a.z + b.z); }
		inline Vec3 Sub(const Vec3& a, const Vec3& b) { return Make(a.x - b.x, a.y - b.y, a.z - b.z); }
		inline Vec3 Scale(const Vec3& v, const float s) { return Make(v.x * s, v.y * s, v.z * s); }
		inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

		inline Vec3 AxisUnit(const int index, const int sign)
		{
			const float s = sign < 0 ? -1.0f : 1.0f;
			return Make(index == 0 ? s : 0.0f, index == 1 ? s : 0.0f, index == 2 ? s : 0.0f);
		}

		// ---- rotations ---------------------------------------------------------------------------
		inline Mat3 Identity()
		{
			Mat3 r;
			r.m[0][0] = 1.0f; r.m[1][1] = 1.0f; r.m[2][2] = 1.0f;
			return r;
		}

		inline Mat3 Mul(const Mat3& a, const Mat3& b)
		{
			Mat3 r;
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
				{
					r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
				}
			}
			return r;
		}

		inline Vec3 Mul(const Mat3& a, const Vec3& v)
		{
			return Make(
				a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
				a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
				a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z);
		}

		inline Mat3 Transpose(const Mat3& a)
		{
			Mat3 r;
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
				{
					r.m[i][j] = a.m[j][i];
				}
			}
			return r;
		}

		// A turn of exactly 180 degrees about a unit axis: 2 a a^T - I. Its own inverse.
		inline Mat3 HalfTurnAbout(const Vec3& unitAxis)
		{
			Mat3 r;
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
				{
					r.m[i][j] = 2.0f * Get(unitAxis, i) * Get(unitAxis, j) - (i == j ? 1.0f : 0.0f);
				}
			}
			return r;
		}

		// ---- transforms, composed the way NiTransform composes ------------------------------------------
		inline Vec3 Apply(const Xform& t, const Vec3& v)
		{
			return Add(t.pos, Scale(Mul(t.rot, v), t.scale));
		}

		inline Xform Compose(const Xform& parent, const Xform& child)
		{
			Xform r;
			r.rot = Mul(parent.rot, child.rot);
			r.pos = Apply(parent, child.pos);
			r.scale = parent.scale * child.scale;
			return r;
		}

		inline Xform Inverse(const Xform& t)
		{
			Xform r;
			r.rot = Transpose(t.rot);
			r.scale = t.scale != 0.0f ? 1.0f / t.scale : 1.0f;
			r.pos = Scale(Mul(r.rot, t.pos), -r.scale);
			return r;
		}

		// ---- which way the weapon runs ------------------------------------------------------------
		// Under this, a box is not clearly longer one way than another and nothing is built on it.
		constexpr float kAmbiguousRatio = 1.25f;

		inline LongAxis FindLongAxis(const Box& box, const float minLength)
		{
			LongAxis axis;
			const float extent[3] = { box.hi.x - box.lo.x, box.hi.y - box.lo.y, box.hi.z - box.lo.z };
			if (!(extent[0] > 0.0f) || !(extent[1] > 0.0f) || !(extent[2] > 0.0f))
			{
				return axis;
			}

			int longest = 0;
			for (int i = 1; i < 3; ++i)
			{
				if (extent[i] > extent[longest])
				{
					longest = i;
				}
			}
			float next = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				if (i != longest && extent[i] > next)
				{
					next = extent[i];
				}
			}

			axis.index = longest;
			axis.length = extent[longest];
			axis.half = axis.length * 0.5f;
			axis.centre = (Get(box.lo, longest) + Get(box.hi, longest)) * 0.5f;
			axis.ratio = next > 0.0f ? axis.length / next : 0.0f;
			axis.usable = axis.length >= minLength;
			// A hair under the ratio through float rounding still counts as reaching it.
			axis.ambiguous = axis.ratio < kAmbiguousRatio - 0.0005f;
			return axis;
		}

		// A point's place along the weapon: -1 the box's low end, +1 its high end. Not clamped - a palm
		// past the end says so.
		inline float AlongU(const LongAxis& axis, const Vec3& pointLocal)
		{
			return axis.half > 0.0f ? (Get(pointLocal, axis.index) - axis.centre) / axis.half : 0.0f;
		}

		// The shape's own origin on the same scale: the hilt.
		inline float HiltU(const LongAxis& axis)
		{
			return axis.half > 0.0f ? -axis.centre / axis.half : 0.0f;
		}

		// How far from the weapon's centre line a point is: small for a palm on the weapon, large for a
		// reading that has gone wrong.
		inline float OffAxis(const LongAxis& axis, const Box& box, const Vec3& pointLocal)
		{
			float sum = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				if (i == axis.index)
				{
					continue;
				}
				const float centre = (Get(box.lo, i) + Get(box.hi, i)) * 0.5f;
				const float d = Get(pointLocal, i) - centre;
				sum += d * d;
			}
			return std::sqrt(sum);
		}

		// Which end is the tip: the one further from the hilt. +1 the high end, -1 the low end, 0 when the
		// hilt is too near the middle to say.
		constexpr float kTipDeadZone = 0.15f;

		inline int TipSign(const float hiltU)
		{
			if (hiltU < -(kTipDeadZone + 0.0005f))
			{
				return 1;
			}
			if (hiltU > kTipDeadZone + 0.0005f)
			{
				return -1;
			}
			return 0;
		}

		// For the log a player reads: how far up from the hilt, as a percentage of hilt-to-tip.
		inline float PercentFromHilt(const float alongU, const float hiltU, const int tipSign)
		{
			if (tipSign == 0)
			{
				return 0.0f;
			}
			const float tipU = tipSign > 0 ? 1.0f : -1.0f;
			const float span = tipU - hiltU;
			return span != 0.0f ? (alongU - hiltU) / span * 100.0f : 0.0f;
		}

		inline Vec3 ToLocal(const Mat3& bodyRot, const Vec3& bodyPos, const Vec3& world)
		{
			return Mul(Transpose(bodyRot), Sub(world, bodyPos));
		}

		inline float AngleDeg(const Vec3& a, const Vec3& b)
		{
			const float la = Length(a);
			const float lb = Length(b);
			if (!(la > 0.0f) || !(lb > 0.0f))
			{
				return 0.0f;
			}
			float c = Dot(a, b) / (la * lb);
			c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
			return std::acos(c) * 57.29578f;
		}

		// How far apart two orientations are, in degrees: the angle of the single turn that takes one to
		// the other, read off that turn's trace (1 + 2 cos).
		inline float AngleBetweenDeg(const Mat3& a, const Mat3& b)
		{
			const Mat3 between = Mul(Transpose(a), b);
			float c = (between.m[0][0] + between.m[1][1] + between.m[2][2] - 1.0f) * 0.5f;
			c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
			return std::acos(c) * 57.29578f;
		}

		// ---- backwards? ---------------------------------------------------------------------------
		// Up to 90: the player's own rule from 2026-09-22, "if it's closer to 180 then reverse, if not then
		// normal" - a weapon nearer backwards than forwards is reversed. 90 is the default from M3.0.0-test8;
		// the old 45 wanted the blade within 45 degrees of dead backwards and made a reversed grab hard work.
		inline float ClampCone(const float coneDeg)
		{
			return coneDeg < 20.0f ? 20.0f : (coneDeg > 90.0f ? 90.0f : coneDeg);
		}

		// Reversed only when the blade points within the cone of DEAD backwards. Everything else is a
		// normal grab, so the weapon is held one way or the other - never half way.
		inline Reverse JudgeReversed(const Vec3& bladeActual, const Vec3& bladeNatural, const float coneDeg)
		{
			Reverse r;
			if (!(Length(bladeActual) > 0.0f) || !(Length(bladeNatural) > 0.0f))
			{
				return r;
			}
			r.angleDeg = AngleDeg(bladeActual, bladeNatural);
			r.reversed = r.angleDeg >= 180.0f - ClampCone(coneDeg);
			return r;
		}

		// A reversed weapon can be turned over about either of the two cardinals across its long axis, and
		// they leave the edge facing opposite ways. Pick the one that lands nearer to how the weapon was
		// really held. Undecided when it is a toss-up: the grip's owner then uses its own default.
		constexpr float kFlipMargin = 0.2f;

		inline FlipChoice ChooseFlipAxis(const Mat3& naturalRot, const Mat3& actualRot, const int longAxis)
		{
			FlipChoice choice;
			float best = -10.0f;
			float other = -10.0f;
			for (int f = 0; f < 3; ++f)
			{
				if (f == longAxis)
				{
					continue;
				}
				const Mat3 candidate = Mul(naturalRot, HalfTurnAbout(AxisUnit(f, 1)));
				const Mat3 difference = Mul(Transpose(actualRot), candidate);
				// The trace of the rotation between them: 3 when identical, -1 when a half turn apart.
				const float closeness = difference.m[0][0] + difference.m[1][1] + difference.m[2][2];
				if (closeness > best)
				{
					other = best;
					best = closeness;
					choice.axis = f;
				}
				else if (closeness > other)
				{
					other = closeness;
				}
			}
			choice.margin = best - other;
			choice.decided = choice.margin > kFlipMargin;
			return choice;
		}

		// ---- the hold -----------------------------------------------------------------------------
		// naturalNodeInHand is HIGGS's grab transform as read in the grabbed callback: the weapon node in
		// the hand's frame, as if equipped. The result is that pose slid along the weapon's long axis until
		// the palm sits at touchedLocal (the touch's coordinate along that axis, in the node's own units),
		// then, for a reversed grab, turned exactly half way round about the palm. The palm is the pivot,
		// so the hand stays on the same spot of the weapon whichever way round it ends up.
		inline Xform HoldPose(
			const Xform& naturalNodeInHand,
			const Vec3& palmInHand,
			const int longAxis,
			const float touchedLocal,
			const bool reversed,
			const int flipAxis)
		{
			const Vec3 palmNatural = Apply(Inverse(naturalNodeInHand), palmInHand);
			const float delta = Get(palmNatural, longAxis) - touchedLocal;

			Xform hold = naturalNodeInHand;
			hold.pos = Add(
				naturalNodeInHand.pos,
				Scale(Mul(naturalNodeInHand.rot, AxisUnit(longAxis, 1)), delta * naturalNodeInHand.scale));

			if (reversed && flipAxis != longAxis)
			{
				const Mat3 turn = HalfTurnAbout(Mul(naturalNodeInHand.rot, AxisUnit(flipAxis, 1)));
				hold.rot = Mul(turn, hold.rot);
				hold.pos = Add(palmInHand, Mul(turn, Sub(hold.pos, palmInHand)));
			}
			return hold;
		}

		// ---- when to record nothing --------------------------------------------------------------------
		// A weapon's node and its collision body sit together until HIGGS moves one of them. Further apart
		// than this inside the grabbed callback, and HIGGS has already warped the weapon to the hand.
		constexpr float kWarpDistance = 3.0f; // Skyrim units
		constexpr float kWarpAngleDeg = 8.0f;

		inline bool Warped(const float nodeToBodyDistance, const float nodeToBodyAngleDeg)
		{
			return nodeToBodyDistance > kWarpDistance || nodeToBodyAngleDeg > kWarpAngleDeg;
		}

		// secondsSincePull is negative when that weapon was never pulled.
		constexpr float kPulledWindowSeconds = 3.0f;

		inline bool WasPulled(const float secondsSincePull, const float windowSeconds)
		{
			return secondsSincePull >= 0.0f && secondsSincePull <= windowSeconds;
		}

		// TESObjectWEAP types: 1 sword, 2 dagger, 3 war axe, 4 mace, 5 greatsword, 6 battleaxe / warhammer.
		// 0 is hand to hand, 7 bow, 8 staff, 9 crossbow.
		inline bool IsGripMelee(const int weaponType)
		{
			return weaponType >= 1 && weaponType <= 6;
		}

		inline bool IsOneHandedMelee(const int weaponType)
		{
			return weaponType >= 1 && weaponType <= 4;
		}
	}
}
