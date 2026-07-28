#include "swapdropandholdreduxinterface001.h"

#include <vector>

namespace SwapDropAndHoldReduxAPI
{
	namespace
	{
		struct SwapDropMessage
		{
			enum : std::uint32_t { kMessage_GetInterface = 0x5D4A4811u };
			void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
		};

		class SwapDropAndHoldReduxInterface001 final : public ISwapDropAndHoldReduxInterface001
		{
		public:
			static SwapDropAndHoldReduxInterface001& GetSingleton()
			{
				static SwapDropAndHoldReduxInterface001 instance;
				return instance;
			}

			unsigned int GetBuildNumber() override
			{
				return 2;
			}

			void AddWeaponGrabEquippedCallback(WeaponHandEventCallback callback) override
			{
				if (callback)
				{
					_grabEquippedCallbacks.push_back(callback);
				}
			}

			void AddTriggerHoldDroppedCallback(WeaponHandEventCallback callback) override
			{
				if (callback)
				{
					_triggerHoldDroppedCallbacks.push_back(callback);
				}
			}

			void AddSwapPullCompleteCallback(WeaponHandEventCallback callback) override
			{
				if (callback)
				{
					_swapPullCompleteCallbacks.push_back(callback);
				}
			}

			void AddWeaponHandEventCallback(WeaponHandEventCallback callback) override
			{
				if (callback)
				{
					_allEventCallbacks.push_back(callback);
				}
			}

			void AddWeaponGrabEquipInterceptCallback(WeaponGrabEquipInterceptCallback callback) override
			{
				if (callback)
				{
					_grabEquipInterceptCallbacks.push_back(callback);
				}
			}

			void AddWeaponGrabPickupInterceptCallback(WeaponGrabPickupInterceptCallback callback) override
			{
				if (callback)
				{
					_grabPickupInterceptCallbacks.push_back(callback);
				}
			}

			void NotifyWeaponGrabEquipped(const WeaponHandEvent& event)
			{
				DispatchEvent(event, _grabEquippedCallbacks);
			}

			void NotifyTriggerHoldDropped(const WeaponHandEvent& event)
			{
				DispatchEvent(event, _triggerHoldDroppedCallbacks);
			}

			void NotifySwapPullComplete(const WeaponHandEvent& event)
			{
				DispatchEvent(event, _swapPullCompleteCallbacks);
			}

			bool QueryWeaponGrabEquipIntercept(const WeaponHandEvent& event)
			{
				for (auto* callback : _grabEquipInterceptCallbacks)
				{
					if (callback && callback(event))
					{
						return true;
					}
				}
				return false;
			}

			bool QueryWeaponGrabPickupIntercept(const WeaponHandEvent& event)
			{
				for (auto* callback : _grabPickupInterceptCallbacks)
				{
					if (callback && callback(event))
					{
						return true;
					}
				}
				return false;
			}

		private:
			void DispatchEvent(const WeaponHandEvent& event, const std::vector<WeaponHandEventCallback>& specificCallbacks)
			{
				for (auto* callback : specificCallbacks)
				{
					callback(event);
				}

				for (auto* callback : _allEventCallbacks)
				{
					callback(event);
				}
			}

			std::vector<WeaponHandEventCallback> _grabEquippedCallbacks;
			std::vector<WeaponHandEventCallback> _triggerHoldDroppedCallbacks;
			std::vector<WeaponHandEventCallback> _swapPullCompleteCallbacks;
			std::vector<WeaponHandEventCallback> _allEventCallbacks;
			std::vector<WeaponGrabEquipInterceptCallback> _grabEquipInterceptCallbacks;
			std::vector<WeaponGrabPickupInterceptCallback> _grabPickupInterceptCallbacks;
		};

		void* GetApiFunction(unsigned int revisionNumber)
		{
			if (revisionNumber == 1)
			{
				return static_cast<ISwapDropAndHoldReduxInterface001*>(&SwapDropAndHoldReduxInterface001::GetSingleton());
			}

			return nullptr;
		}

		void OnSwapDropMessage(SKSEMessagingInterface::Message* msg)
		{
			if (!msg || msg->type != SwapDropMessage::kMessage_GetInterface)
			{
				return;
			}

			if (msg->dataLen != sizeof(SwapDropMessage))
			{
				return;
			}

			auto* apiMsg = static_cast<SwapDropMessage*>(msg->data);
			apiMsg->GetApiFunction = GetApiFunction;
		}
	}

	ISwapDropAndHoldReduxInterface001* GetSwapDropAndHoldReduxInterface001(
		const PluginHandle& pluginHandle,
		SKSEMessagingInterface* messagingInterface)
	{
		static ISwapDropAndHoldReduxInterface001* cached = nullptr;
		if (cached)
		{
			return cached;
		}

		if (!messagingInterface)
		{
			return nullptr;
		}

		SwapDropMessage message{};
		const bool dispatched = messagingInterface->Dispatch(
			pluginHandle,
			SwapDropMessage::kMessage_GetInterface,
			&message,
			static_cast<UInt32>(sizeof(message)),
			kInterfaceRecipient);

		if (!dispatched || !message.GetApiFunction)
		{
			return nullptr;
		}

		cached = static_cast<ISwapDropAndHoldReduxInterface001*>(message.GetApiFunction(1));
		return cached;
	}

	void RegisterSwapDropAndHoldReduxInterface(
		PluginHandle pluginHandle,
		SKSEMessagingInterface* messagingInterface)
	{
		static bool s_registered = false;
		if (s_registered)
		{
			return;
		}

		if (!messagingInterface || pluginHandle == kPluginHandle_Invalid)
		{
			_MESSAGE("Swap Drop & Hold redux: cannot register mod API; messaging interface unavailable.");
			return;
		}

		if (messagingInterface->RegisterListener(pluginHandle, nullptr, OnSwapDropMessage))
		{
			s_registered = true;
			_MESSAGE(
				"Swap Drop & Hold redux: mod-support API registered (revision 1, build 2, recipient \"%s\").",
				kInterfaceRecipient);
		}
		else
		{
			_MESSAGE("Swap Drop & Hold redux: failed to register mod-support API listener.");
		}
	}

	void NotifyWeaponGrabEquipped(const WeaponHandEvent& event)
	{
		SwapDropAndHoldReduxInterface001::GetSingleton().NotifyWeaponGrabEquipped(event);
	}

	void NotifyTriggerHoldDropped(const WeaponHandEvent& event)
	{
		SwapDropAndHoldReduxInterface001::GetSingleton().NotifyTriggerHoldDropped(event);
	}

	void NotifySwapPullComplete(const WeaponHandEvent& event)
	{
		SwapDropAndHoldReduxInterface001::GetSingleton().NotifySwapPullComplete(event);
	}

	bool QueryWeaponGrabEquipIntercept(const WeaponHandEvent& event)
	{
		return SwapDropAndHoldReduxInterface001::GetSingleton().QueryWeaponGrabEquipIntercept(event);
	}

	bool QueryWeaponGrabPickupIntercept(const WeaponHandEvent& event)
	{
		return SwapDropAndHoldReduxInterface001::GetSingleton().QueryWeaponGrabPickupIntercept(event);
	}
}
