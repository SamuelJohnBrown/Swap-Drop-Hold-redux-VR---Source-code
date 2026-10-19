#pragma once

namespace SwapDropAndHoldRedux
{
	// Simple mode's double tap: does a touched grip count as closed on this controller?
	//
	// GripTouchRules.h has the rule and the reason. This looks the controller's type up (OpenVR's
	// Prop_ControllerType_String), remembers it per hand until the device behind that hand changes or
	// the ini setting changes, and logs what it decided. Main thread only.
	bool GripTouchTapsCountTouch(bool isLeftVRController);
}
