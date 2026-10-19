#pragma once

// When a weapon HIGGS is holding LOOSE is given back as a weapon (M3.0.0-test10). No engine types,
// so tools_loose_hold_tests.cpp can drive it outside the game.
//
// A loose hold is what Grip Hold leaves when it refuses to equip a weapon a hand took hold of: the
// re-grab cooldown (a weapon dropped moments ago, by either hand) or the loop guard (a weapon caught
// in a drop/catch/re-equip loop). The hand holding it cannot pull, and the weapon hangs in it as a
// physics object - "I still would most likely hold it as a HIGGS weapon".
//
// Up to test9 only a hold refused by the cooldown AND taken by the SAME hand that dropped it was ever
// given back, once the cooldown was over and the grip was still closed. The test9 log has the other
// hand's catches - the right hand threw, the left caught it 0.22-0.30s later, grip closed - refused
// by the same cooldown and then left loose for 0.72-2.60s every time, because nothing ever looked at
// them again. The cooldown exists to keep a dropped weapon from being equipped by a hand the player
// did not close on it, and a hand still squeezing after the cooldown has plainly closed on it, so
// the hand that dropped it no longer matters. The loop guard's holds are left exactly as they were:
// they stay loose until the weapon is picked up deliberately again.

namespace SwapDropAndHoldRedux
{
	namespace LooseHoldRule
	{
		enum class Why
		{
			kCooldown,  // the re-grab cooldown refused the equip
			kLoopGuard, // the re-grab loop guard refused it
		};

		// `droppedBySameHand` is taken, and deliberately not consulted: see above.
		inline bool GiveBack(const bool simpleMode, const Why why, const bool droppedBySameHand, const bool gripClosed,
			const bool cooldownCovers, const bool loopGuardBlocks)
		{
			(void)droppedBySameHand;
			return !simpleMode && why == Why::kCooldown && gripClosed && !cooldownCovers && !loopGuardBlocks;
		}
	}
}
