#pragma once

namespace SwapDropAndHoldRedux
{
	void RegisterWeaponDrawHandler();
	void RegisterWeaponDrawHiggsCallback();
	void RedrawTrackedEquippedWeapons(bool forceRefresh = false);
	void ScheduleWeaponDrawMaintenance(bool isLeftGameHand = false, bool forTwoHandedWeapon = false);
	// Delayed draw with re-check retries; used for crossbows whose equip cocking
	// sequence swallows a same-frame draw request.
	void ScheduleDelayedWeaponDrawMaintenance();
}

