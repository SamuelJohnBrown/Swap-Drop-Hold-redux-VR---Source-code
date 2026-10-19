#include "GrabGripProvider.h"

#include "config.h"

#include <atomic>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		std::atomic<SdhGrabGripAPI::GrabGripCallback> s_listener{ nullptr };
		std::atomic<std::uint32_t> s_capabilities{ 0 };

		void LogCapabilities(const char* what, const std::uint32_t mask)
		{
			LOG_INFO(
				"Grab grip: %s - it can slide the grip: %s, turn a weapon over: %s.",
				what,
				(mask & SdhGrabGripAPI::kCanSlide) ? "yes" : "no",
				(mask & SdhGrabGripAPI::kCanFlip) ? "yes" : "no");
		}

		struct GrabGripImpl : SdhGrabGripAPI::IGrabGrip001
		{
			std::uint32_t GetBuildNumber() override
			{
				return MOD_VERSION;
			}

			std::uint32_t GetApiRevision() override
			{
				return 1;
			}

			void SetListener(const SdhGrabGripAPI::GrabGripCallback callback, const std::uint32_t capabilityMask) override
			{
				s_listener.store(callback);
				s_capabilities.store(capabilityMask);
				LogCapabilities(callback ? "a grip driver is listening" : "the grip driver stopped listening", capabilityMask);
			}

			void SetCapabilities(const std::uint32_t capabilityMask) override
			{
				if (s_capabilities.exchange(capabilityMask) != capabilityMask)
				{
					LogCapabilities("the grip driver's abilities changed", capabilityMask);
				}
			}
		};

		GrabGripImpl s_impl;

		void* GetApi(const unsigned int revision)
		{
			return revision == 1 ? static_cast<SdhGrabGripAPI::IGrabGrip001*>(&s_impl) : nullptr;
		}

		SdhGrabGripAPI::Offer s_offer;
	}

	void GrabGripBroadcastOffer(const PluginHandle plugin, SKSEMessagingInterface* const messaging, const char* const when)
	{
		if (!messaging || !messaging->Dispatch)
		{
			return;
		}

		s_offer.GetApiFunction = GetApi;
		const bool anyone = messaging->Dispatch(plugin, SdhGrabGripAPI::kMessage_Offer, &s_offer, sizeof(s_offer), nullptr);
		LOG_INFO(
			"Grab grip: interface offered to other mods at %s (%s).",
			when,
			anyone ? "delivered to at least one listener" : "no plugin was listening for broadcasts");
	}

	bool GrabGripHasListener()
	{
		return s_listener.load() != nullptr;
	}

	std::uint32_t GrabGripListenerCapabilities()
	{
		return s_capabilities.load();
	}

	std::uint32_t GrabGripPublish(const SdhGrabGripAPI::GrabGripEvent& event)
	{
		const SdhGrabGripAPI::GrabGripCallback listener = s_listener.load();
		return listener ? listener(event) : static_cast<std::uint32_t>(SdhGrabGripAPI::kIgnored);
	}
}
