#pragma once

// Where a weapon you let go of is spawned: its pose in your hand at the release, moved clear of that hand
// (ThrowMomentum.cpp, ThrowCaptureRelease). Spawned at the exact held pose it starts INSIDE HIGGS's hand
// collision, and the two fight - it flies off or wobbles.
//
// Two nudges, both in Skyrim units:
//   lateralUnits (fDropLateralOffset) - sideways, along the player's horizontal right vector: toward the body's
//     midline when positive (left of the right hand, right of the left hand), outward when negative. That clears a
//     thumb-up grip, whose palm faces the midline.
//   downUnits (fDropDownOffset, 3.0.0) - straight down, below the palm. A weapon lying across under a palm-down
//     hand only slides along its own length when nudged sideways and stays in the fist (the user, 2026-10-02:
//     raising the sideways nudge fixed sideways drops only). 0 = off.
//
// No engine types in here, so tools_drop_nudge_tests.cpp can drive it outside the game.

namespace SwapDropAndHoldRedux
{
	namespace DropNudge
	{
		struct Pos
		{
			float x = 0.0f; // Havok metres, world
			float y = 0.0f;
			float z = 0.0f;
		};

		inline Pos Nudged(const Pos& at, const float rightX, const float rightY, const bool isLeftHand,
			const float lateralUnits, const float downUnits, const float havokToSkyrim)
		{
			Pos p = at;
			const float sign = isLeftHand ? 1.0f : -1.0f;
			const float lateralMetres = (lateralUnits * sign) / havokToSkyrim;
			p.x += rightX * lateralMetres;
			p.y += rightY * lateralMetres;
			p.z -= downUnits / havokToSkyrim;
			return p;
		}
	}
}
