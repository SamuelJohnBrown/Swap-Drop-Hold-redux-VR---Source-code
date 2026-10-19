#pragma once

// How near a VRIK holster slot a hand has to be before this mod steps aside for VRIK. No
// engine types in here, so tools_holster_range_tests.cpp can check it outside the game;
// VrikHolsters.cpp and GripHold.cpp feed it the live settings and hand position.

namespace SwapDropAndHoldRedux
{
	namespace HolsterRange
	{
		// What one check asks for on top of the plain catch radius.
		struct Reach
		{
			float rangeBonus = 0.0f;    // added last, e.g. the extra distance it takes to leave
			bool vrikReachOnly = false; // VRIK's own activation distance, no extra range
		};

		// The stand-down check for a hand holding a weapon.
		//
		// Grip Hold drops a weapon the moment the hand opens, so its zone is wide: a holster
		// VRIK would have taken must never become a weapon on the floor. Leaving takes a
		// margin more than entering, so a hand resting on the edge cannot flip the state.
		//
		// Simple mode drops only on a double tap, and taps are ignored while standing down,
		// so every unit of extra range is a place where a double tap does nothing. The
		// M2.1.1-test2 log caught seven bursts ignored over the right hip, entered 9.9 to 13.9
		// units from the slot, where VRIK did not holster either. So Simple stands down only
		// within VRIK's own reach, and with no margin: a hand on the edge can at worst restart
		// a tap in progress, and GripHold's leave delay already rides out a frame outside.
		inline Reach ForStandDown(const bool simpleMode, const bool alreadyStandingDown, const float releaseMargin)
		{
			if (simpleMode)
			{
				return Reach{ 0.0f, true };
			}
			return Reach{ alreadyStandingDown ? releaseMargin : 0.0f, false };
		}

		// The catch radius around a slot: VRIK's activation distance, plus the extra range for
		// the side of the body the hand is on unless the check wants VRIK's reach only, plus
		// the check's bonus.
		inline float CatchRange(
			const float vrikActivationDistance,
			const float extraFront,
			const float extraBehind,
			const bool handBehindBody,
			const Reach& reach)
		{
			const float extra = reach.vrikReachOnly ? 0.0f : (handBehindBody ? extraBehind : extraFront);
			return vrikActivationDistance + extra + reach.rangeBonus;
		}

		// The buzz that says "letting go here holsters it".
		//
		// The stand-aside zone above is padded because getting it wrong drops a weapon on the
		// floor, and until now the buzz simply rode along: on VRIK versions with no holster
		// API it fired 6 units before a slot in front of the body and 14 units before one
		// behind it. A buzz costs nothing when it is wrong, so it never needed that padding.
		//
		// Once a slot's centre has been measured off real draws (SlotState::learned), the
		// padding is the only error left in it, so the buzz moves in to VRIK's own activation
		// distance - the range at which VRIK itself would take the weapon.
		//
		// A slot nobody has measured keeps the wide zone. Its centre is still an estimate -
		// the 2026-09-12 log had them 7.9 to 9.9 units out - and a tight circle around a
		// centre that far off is a buzz that never fires at all. Nobody loses a buzz.
		inline Reach ForBuzz()
		{
			return Reach{ 0.0f, true };
		}

		// The old buzz, on entering the padded stand-aside zone. Kept wherever the tight one
		// cannot run: at a slot whose centre has not been measured often enough to trust
		// (HolsterLearn::CentreIsTrusted).
		inline bool BuzzOnStandDownEntry(const bool centreIsTrusted)
		{
			return !centreIsTrusted;
		}
	}
}
