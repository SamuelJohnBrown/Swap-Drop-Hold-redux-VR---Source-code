#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	// GripHold mode: weapons belong to the physical grip. Grip a world weapon to
	// equip it (SDH's existing pickup path), release the grip to hand it off /
	// stick it in a wound / drop it with momentum. Registered on HIGGS's
	// pre-physics-step callback; inert while GripHoldMode=0 or the global lock is on.
	void RegisterGripHold();

	// A grab-equip landed on this game hand (WeaponGrabActivator) — bind and arm it.
	void GripHoldOnGrabEquipped(bool isLeftGameHand, UInt32 weaponFormID);

	// The other hand handed its weapon to this one (WeaponGrabActivator's swap pull) — the same
	// bind, arm and grip blink a pickup gets, because to every other mod this hand has just
	// acquired a weapon with its grip already closed.
	void GripHoldOnHandOffComplete(bool isLeftGameHand, UInt32 weaponFormID);

	// The haptic census heard VRIK's holster buzz on this hand: it is at a slot that will take its
	// weapon. Old VRIK, Grip hold: stand aside there. VrikPulseRules.h.
	void GripHoldOnVrikHolsterBuzz(bool isLeftVRController);

	// What this hand is doing, in words, for the haptic census's log lines: "empty hand", "holding
	// Dawnbreaker, armed, grip down, standing aside at a holster". Returns buffer.
	const char* GripHoldDescribeHand(bool isLeftVRController, char* buffer, size_t size);

	// PostPostLoad: whether Weapon Throw VR is loaded, for the grip shown released after a Grip Hold
	// pickup (GripBlinkRules.h). Logged either way.
	void GripHoldDetectThrowMod();

	// Grip Hold mode's global perma-lock, toggled by its trigger chord on either hand.
	// Persisted across sessions. Always false in Simple mode, which has no lock.
	bool GripHoldIsGloballyLocked();

	// This mod's own reads of a controller's grip and trigger, for diagnostics elsewhere.
	bool GripHoldIsGripPressed(bool isLeftVRController);
	bool GripHoldIsTriggerPressed(bool isLeftVRController);

	// After every settings load - at startup and after each MCM journal close. Applies a
	// change of weapon mode (both hands start over; nothing is dropped by the switch) and
	// reports a saved lock the way the current mode sees it.
	void GripHoldOnSettingsReloaded();

	// Suppress SDH's grab auto-equip for a ref this hand just dropped (re-grab cooldown).
	bool GripHoldShouldSuppressGrab(bool isLeftVRController, TESObjectREFR* grabbedRefr);

	// Scene teardown (load/new game): forget hand state.
	void GripHoldOnPreLoadGame();
}
