#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	// Pose-history throw for GripHold's grip-release drops. HIGGS's weapon body is
	// keyframed and reads back zero velocity, so the release velocity is
	// differentiated from poses sampled every frame — and it is the FASTEST motion
	// in the recent window, not the last frame, because a real throw decelerates as
	// the hand opens. All positions are Havok metres; hands are VR controllers.
	void ThrowSampleHeldPose(bool isLeftVRController, float deltaTime);
	void ThrowCaptureRelease(bool isLeftVRController);
	void ThrowBeginFixup(bool isLeftVRController, UInt32 droppedRefID);
	void ThrowServiceFixups();
	// Drops the pose history for one hand, leaving a throw already in flight to
	// finish stamping. Call whenever a hand starts holding a different weapon.
	void ThrowClearHistory(bool isLeftVRController);
	void ThrowResetHand(bool isLeftVRController);
	// Diagnostic: the peak hand speed in the recent window, in m/s - the same
	// number the release throw is built from, and directly comparable to what
	// WeaponThrowVR logs as ThrowSpeed. outSamples is how many poses back it.
	float ThrowPeakSpeedMps(bool isLeftVRController, int& outSamples);
	// The linear speed of the last release this hand captured, in m/s (0 when throwing
	// is off). Tells a catch straight after a still release apart from a caught throw.
	float ThrowLastReleaseSpeedMps(bool isLeftVRController);
	void ThrowResetAll();
}
