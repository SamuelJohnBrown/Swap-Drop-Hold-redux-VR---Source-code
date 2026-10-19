#pragma once

// The haptic census: every haptic pulse any mod sends through the game's IVRSystem, logged with what
// this mod knows about that hand at that moment. DISCOVERY ONLY - it pins no slot and changes no
// behaviour. HapticCensusRules.h says why; HapticCensus.cpp says what was checked before hooking.

namespace SwapDropAndHoldRedux
{
	// A HIGGS pre-physics step of its own. The hook is installed on the first step that finds the VR
	// system up, and the step drains and logs what the hook caught. Call once HIGGS's interface exists.
	void RegisterHapticCensus();

	// GripHold's PulseController brackets its own TriggerHapticPulse with these, so the census can
	// tell this mod's pulses from everyone else's - and so the first one proves the hook is on the
	// right slot.
	void HapticCensusNoteOwnPulse();
	void HapticCensusOwnPulseDone();
}
