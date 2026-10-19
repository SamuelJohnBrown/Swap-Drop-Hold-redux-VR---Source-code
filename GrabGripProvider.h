#pragma once

#include "SdhGrabGripInterface.h"

#include "skse64/PluginAPI.h"

namespace SwapDropAndHoldRedux
{
	// 3.0.0: the providing side of SdhGrabGripInterface.h. This mod senses where and which way round a
	// weapon was taken hold of (GrabPose.cpp) and tells whichever grip driver is listening, just before the
	// weapon is equipped. It knows nothing about who listens; TrueWield VR is the intended consumer.

	// Broadcast the interface to every plugin with an all-sender listener. Called at PostPostLoad and again
	// at DataLoaded, because a name-addressed Dispatch does not reliably find its receiver on SkyrimVR.
	void GrabGripBroadcastOffer(PluginHandle plugin, SKSEMessagingInterface* messaging, const char* when);

	bool GrabGripHasListener();
	std::uint32_t GrabGripListenerCapabilities();

	// Hands the event to the listener on the calling (frame) thread. Returns its Answer bits, or kIgnored
	// when nobody is listening.
	std::uint32_t GrabGripPublish(const SdhGrabGripAPI::GrabGripEvent& event);
}
