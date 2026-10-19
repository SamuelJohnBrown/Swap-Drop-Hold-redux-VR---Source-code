#pragma once

namespace SwapDropAndHoldRedux
{
	// The re-press after a Grip Hold blink waits until the hand is clear of anything VR Climbing
	// climbs (GripBlinkRules.h, M3.0.0-test7). Registers two HIGGS callbacks: a pre-physics one that
	// hands SurfaceProbe the world, and a post-VRIK one that does the looking on the frame thread,
	// outside the physics step. Needs the HIGGS interface.
	void RegisterBlinkHoldBack();
}
