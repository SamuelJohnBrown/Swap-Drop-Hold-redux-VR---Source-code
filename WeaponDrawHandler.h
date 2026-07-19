#pragma once

namespace SwapDropAndHoldRedux
{
	void RegisterWeaponDrawHandler();
	void RegisterWeaponDrawHiggsCallback();
	void RedrawTrackedEquippedWeapons(bool forceRefresh = false);
	void ScheduleWeaponDrawMaintenance(bool isLeftGameHand = false, bool forTwoHandedWeapon = false);
}

