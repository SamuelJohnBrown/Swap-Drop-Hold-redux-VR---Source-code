#pragma once

#include <skse64/GameForms.h>

namespace SwapDropAndHoldRedux
{
	// Grip-hold picks weapons up and puts them down constantly, and each one plays
	// the weapon's own sounds: the pickup/putdown pair and, on the main hand, the
	// equip/unequip pair the engine reaches for while it draws the weapon.
	//
	// The engine does not always play them inside the call we make — the draw runs a
	// frame or two later — so a plain scope guard restores the sounds too early to
	// help. Instead the weapon's sound descriptors are borrowed for a short window
	// and put back when it expires.
	//
	// These live on the SHARED BASE FORM — every Iron Sword in the game shares one —
	// so a missed restore would silently and permanently rob the player of a sound
	// with no clue why. SilentEquip.cpp lists the safeguards that exist for that.

	// Registers the restore service on its own unconditional callback. Until this
	// succeeds, SilenceWeaponSoundsFor refuses to mute anything at all.
	void RegisterWeaponSoundSilence();

	void SilenceWeaponSoundsFor(TESForm* form);
	void RestoreAllWeaponSounds();
}
