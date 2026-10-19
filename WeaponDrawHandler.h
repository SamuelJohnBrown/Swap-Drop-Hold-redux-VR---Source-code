#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	// The player's weapon draw state, raw: 3 means drawn AND SETTLED. 4 and 5 are
	// kWantToSheathe and kSheathing, and IsWeaponDrawn() is state >= 3 - so only a strict 3
	// means the weapon is really in the hand. DrawWatch.cpp needs the raw number to tell
	// "still sheathed" apart from "on its way back in".
	UInt32 PlayerWeaponDrawState(PlayerCharacter* player);

	// Ask the game to draw the equipped weapon again, but ONLY if it is genuinely not out.
	// Every gate the one-shot draw uses is kept: the setting, a tracked weapon, not sneaking
	// or swimming, and no two-handed proxy. Returns true if a draw was actually requested.
	//
	// Re-asking for a draw that already succeeded is what spammed equip audio when dual
	// wielding, so the drawn check here is not optional.
	bool RequestWeaponDrawAgain();
	void RegisterWeaponDrawHandler();
	void RegisterWeaponDrawHiggsCallback();
	// Returns true if a real draw was performed (which announces the weapon by
	// itself); false means the caller should announce it instead.
	bool RedrawTrackedEquippedWeapons(bool forceRefresh = false);

	void ScheduleWeaponDrawMaintenance(bool isLeftGameHand = false, bool forTwoHandedWeapon = false);
	// Delayed draw with re-check retries; used for crossbows whose equip cocking
	// sequence swallows a same-frame draw request.
	void ScheduleDelayedWeaponDrawMaintenance();
}

