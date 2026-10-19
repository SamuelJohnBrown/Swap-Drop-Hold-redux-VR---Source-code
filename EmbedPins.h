#pragma once

#include "IwpInterface001.h"

#include "skse64/GameReferences.h"
#include "skse64/PluginAPI.h"

namespace SwapDropAndHoldRedux
{
	// IWP mod-support interface, filled by the broadcast listener (or the request).
	extern IwpAPI::IIwpInterface001* g_iwpInterface;

	// Handle an any-sender SKSE message; returns true if it was IWP's interface offer.
	bool AcceptIwpInterfaceOffer(SKSEMessagingInterface::Message* msg);

	// Secondary path: ask IWP by name. Null result means "not yet", never "not installed".
	void RequestIwpInterface(PluginHandle pluginHandle, SKSEMessagingInterface* messaging);

	// Grip released with the blade in a body: leave it in the wound. Returns true when
	// the release was handled as a stick (a drop+pin task is queued); false = not
	// embedded, caller drops normally. Runs inside the HIGGS frame callback while the
	// weapon is still equipped.
	bool EmbedTryCaptureOnRelease(bool isLeftGameHand, bool isLeftVRController, UInt32 weaponFormID);

	// Is this world reference a weapon we are currently holding in a wound (or about
	// to be)? Grabbing one must not run the normal grab-to-equip pipeline.
	bool EmbedIsRefPinned(UInt32 refID);

	// Per-frame pin maintenance (pose re-assert, liveness, pull progress, pending fixups).
	void EmbedServicePins(float deltaTime);

	// Empty-hand grip press near a lodged weapon: take hold of it. True if a pull began
	// (or the extraction was handed back to IWP).
	bool EmbedTryStartPull(bool isLeftVRController);
	void EmbedReleasePullBy(bool isLeftVRController);

	// A grab-equip landed on this controller — arms a pending IWP resume.
	void EmbedOnHandEquipped(bool isLeftVRController);

	// kDrawn-gated IWP ResumeEmbed + draw guard. Runs per frame per controller.
	void EmbedServiceResume(bool isLeftVRController, float deltaTime);

	// Scene teardown: release every pin (restores collision filters), clear state.
	void EmbedOnPreLoadGame();
}
