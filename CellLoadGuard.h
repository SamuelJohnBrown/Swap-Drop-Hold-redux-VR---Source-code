#pragma once

namespace SwapDropAndHoldRedux
{
	// Standing the mod down across a cell load. CellLoadGuardRules.h has the rules and the reasons.

	// Listens for the loading screen going up and coming down. Call once, at input load, beside the
	// other menu listeners.
	void RegisterCellLoadGuard();

	// Once per tick, BEFORE anything reads the lock, so the hold counts down even on a frame the mod
	// stands down for.
	void CellLoadGuardTick(float deltaTime);

	// True while the mod should keep its hands off: a loading screen is up, or the new cell is still
	// settling. Reads as false when the setting is off.
	bool CellLoadGuardLocked();

	// A game load or a settings change starts it over.
	void CellLoadGuardReset();
}
