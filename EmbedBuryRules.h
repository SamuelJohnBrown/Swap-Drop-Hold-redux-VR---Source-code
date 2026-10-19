#pragma once

// Did the blade actually go INTO the body, or was it just let go near one?
//
// WHY THIS EXISTS. The geometric stab fallback - the one used when Immersive Weapon Penetration
// has no wound to report - looked for the nearest skeleton bone to the MIDDLE and TIP of the
// blade, within EmbedRadius, and stuck the weapon if it found one. It never asked whether the
// blade had gone in. EmbedRadius is 35 units, about half a metre, so letting a dagger go anywhere
// beside a corpse pinned it in mid air: caught in the user's log on 2026-09-21 as
//
//     Embed: ... sticking it (geometric, boneDist=9.1)
//     Embed: pinned refId=FF001EF7 - tracking the wound now (blade 0u, 0 pct buried, ...)
//
// - pinned with nothing buried at all, and then it had to be "worked free" like a real stab.
//
// THE TEST. When a blade is buried, its TIP is much closer to the bone than its GRIP is; the gap
// between those two distances is how far it went in. Laid alongside a body, both ends are about
// equally far away and the gap is nearly zero. Pointing away, the gap is negative. That one
// number separates the two cases without a raycast, and it needs nothing the caller does not
// already have.
//
// This gates the GEOMETRIC path only. When IWP reports a wound it knows exactly where the blade
// is, and its answer is still taken as it always was.
//
// No engine types in here, so tools_embed_bury_tests.cpp can drive it outside the game.

#include <cmath>

namespace SwapDropAndHoldRedux
{
	namespace EmbedBury
	{
		struct Config
		{
			// How much closer to the bone the tip must be than the grip, in Skyrim units, before
			// a release counts as a stab. Six units is about 8.5 cm - deeper than any amount of
			// "holding it near them" and shallower than any real stab.
			//
			// 0 restores exactly the behaviour before this rule: near enough is a stab.
			float minBuryUnits = 6.0f;

			// M3.0.0-test8: the TIP must also be inside the victim's body (bEmbedNeedsTipInBody). The
			// gap test above cannot tell a blade buried in a body from one POINTED at it: in the
			// 2026-09-22 log both geometric pins passed it by 13 units with the tip still 12-14 units
			// from the bone (chest, head), and the player saw both weapons float. So the tip is now
			// tested against the victim's own collision bodies - the ones the game hangs on its
			// skeleton bones - each in its own frame, against its shape's box. A box is a little
			// generous at a capsule's corners and never short of it. Off = test6's rule alone.
			bool tipMustBeInBody = true;
		};

		// A point inside a box, faces included, all in the same frame and units. False for any
		// reading that is not a number, so a broken one can never pin a weapon.
		inline bool InsideBox(const float p[3], const float lo[3], const float hi[3])
		{
			for (int i = 0; i < 3; ++i)
			{
				if (!(p[i] >= lo[i] && p[i] <= hi[i]))
				{
					return false;
				}
			}
			return true;
		}

		// How far outside a box a point is (0 inside it), for the log: how near the tip came to
		// being in. Negative for a reading that is not a number.
		inline float DistanceOutsideBox(const float p[3], const float lo[3], const float hi[3])
		{
			float sum = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				if (!(p[i] == p[i]) || !(lo[i] == lo[i]) || !(hi[i] == hi[i]))
				{
					return -1.0f;
				}
				const float below = lo[i] - p[i];
				const float above = p[i] - hi[i];
				const float d = below > 0.0f ? below : (above > 0.0f ? above : 0.0f);
				sum += d * d;
			}
			return std::sqrt(sum);
		}

		enum class Verdict
		{
			kStab,           // pin it
			kNotBuried,      // the gap test failed: pointing away, laid alongside, or a broken reading
			kTipOutsideBody, // pointed at the body, but the tip is in none of its body parts
			kNoBodies,       // nothing of the victim's to test the tip against, so it cannot be shown
		};

		// How far the blade has gone in: how much closer the tip is to the bone than the grip is.
		// Roughly the blade length when driven in to the hilt, near zero when laid alongside, and
		// negative when the weapon points away from the body.
		inline float BuryUnits(const float gripDist, const float tipDist)
		{
			return gripDist - tipDist;
		}

		// Both distances are to the SAME bone, in Skyrim units, and a negative one means the
		// caller could not measure it.
		inline bool LooksBuried(const float gripDist, const float tipDist, const Config& config)
		{
			// Written as a positive test so a NaN falls through to false rather than sailing past
			// a "< 0" check, which is how a broken reading would otherwise pin a weapon.
			if (!(gripDist >= 0.0f) || !(tipDist >= 0.0f))
			{
				return false;
			}
			return BuryUnits(gripDist, tipDist) >= config.minBuryUnits;
		}

		// The geometric path's whole verdict: the gap first (test6), then, when asked for, the tip.
		// bodiesLooked is how many of the victim's body parts the tip was tested against.
		inline Verdict JudgeGeometric(const float gripDist, const float tipDist, const int bodiesLooked, const bool tipInside,
			const Config& config)
		{
			if (!LooksBuried(gripDist, tipDist, config))
			{
				return Verdict::kNotBuried;
			}
			if (!config.tipMustBeInBody)
			{
				return Verdict::kStab;
			}
			if (bodiesLooked <= 0)
			{
				return Verdict::kNoBodies;
			}
			return tipInside ? Verdict::kStab : Verdict::kTipOutsideBody;
		}
	}
}
