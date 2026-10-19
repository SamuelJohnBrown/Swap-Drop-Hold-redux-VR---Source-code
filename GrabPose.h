#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	// 3.0.0, "held where you grabbed it": the sensing half. GrabPoseRules.h has the maths and the
	// reasons. M3.0.0-test1 is the MEASURING build (plan spikes S1 and S2): it logs what it senses and,
	// only when bGrabProbeSetTransform = 1, poses HIGGS's hold to match. Nothing is equipped differently.

	// Adds this module's own HIGGS pulled and pre-physics callbacks. Call once HIGGS is known.
	void RegisterGrabPose();

	// Call first thing in the HIGGS grabbed callback - the only moment the weapon is still where the
	// player reached for it. Does nothing unless bGrabGripTrace = 1.
	void GrabPoseOnGrabbed(bool isLeftVRController, TESObjectREFR* grabbedRefr);

	// Call in the equip task just BEFORE the weapon is equipped (and before any equip intercept): if the
	// hand that grabbed it left a touch worth trusting for this weapon, a listening grip driver is told
	// (GrabGripRules.h, SdhGrabGripInterface.h). The record is used up either way.
	void GrabPosePublishForEquip(bool isLeftGameHand, UInt32 weaponFormID);

	// M3.0.0-test14, the hand switch: call when the hand holding a weapon lets go while the other hand, holding it
	// too, is about to take it over - BEFORE the transfer. Measures where, and which way round, the receiving hand
	// holds the weapon, and keeps it for GrabPosePublishForEquip on that hand.
	void GrabPoseOnHandOff(bool isLeftHoldingGameHand, UInt32 weaponFormID);
}
