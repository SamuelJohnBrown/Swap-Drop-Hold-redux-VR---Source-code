#pragma once

#include "GripBlinkRules.h"

namespace SwapDropAndHoldRedux
{
	// Is there something VR Climbing would climb within reach of a hand? (M3.0.0-test7)
	//
	// The same six world-axis rays VR Climbing casts from the same node (the controller's wand node),
	// through the same engine call (bhkWorld::PickObject, vtable slot 0x33 - VR Climbing calls
	// [vtable + 0x198] itself), keeping only the FIRST hit of each ray, as VR Climbing does.
	// GripBlinkRules.h decides what the hits mean; this only casts them.

	// Every physics step, from a HIGGS pre-physics callback: the world that step ran in.
	void SurfaceProbeNoteWorld(void* bhkWorld);

	// On the frame thread only (a HIGGS post-VRIK callback), never inside the physics step. False
	// when it could not look: no fresh world, no wand node, or a loading screen up.
	bool SurfaceProbeHand(bool isLeftVRController, float reachUnits, GripBlink::ProbeRay (&rays)[6]);
}
