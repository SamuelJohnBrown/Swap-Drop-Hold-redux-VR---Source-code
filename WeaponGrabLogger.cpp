#include "WeaponGrabLogger.h"
#include "WeaponGrabActivator.h"
#include "TriggerHoldLogger.h"
#include "TrackedWeapons.h"

#include "Engine.h"
#include "config.h"

#include <skse64/GameObjects.h>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		struct BitingAxesMessage
		{
			enum : std::uint32_t { kMessage_GetInterface = 0xB17E4871u };
			void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
		};

		struct IBitingAxesInterface001Compat
		{
			virtual std::uint32_t GetBuildNumber() = 0;
			virtual bool IsHandEmbedded(bool isLeft) = 0;
			virtual bool GetHandEmbedSnapshot(bool isLeft, void* outSnapshot) = 0;
			virtual bool IsAnyHandEmbedded() = 0;
			virtual bool IsHandWorldAxeEmbed(bool isLeft) = 0;
		};

		IBitingAxesInterface001Compat* g_bitingAxesApi = nullptr;

		bool ShouldSuppressForBitingAxesWorldEmbed()
		{
			if (!g_bitingAxesApi)
			{
				return false;
			}

			// If Biting Axes currently owns a world-model embed on either hand, don't
			// auto-pickup grabbed refs here. BA handles that flow itself.
			return g_bitingAxesApi->IsHandWorldAxeEmbed(false) ||
				g_bitingAxesApi->IsHandWorldAxeEmbed(true);
		}

		void OnWeaponGrabbed(const bool isLeft, TESObjectREFR* grabbedRefr)
		{
			if (!grabbedRefr || !grabbedRefr->baseForm)
			{
				return;
			}

			TESForm* itemForm = grabbedRefr->baseForm;

			if (IsBowWeaponForm(itemForm))
			{
				const char* itemName = GetSafeFormName(itemForm);

				if (IsMainHandVRController(isLeft))
				{
					LOG_INFO(
						"Bow grabbed [main hand]: %s formId=%08X (auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				if (ShouldSuppressGrabAutoEquip(isLeft, grabbedRefr))
				{
					LOG_INFO(
						"Bow grabbed [off-hand]: %s formId=%08X (trigger hold drop, auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				if (ShouldSuppressForBitingAxesWorldEmbed())
				{
					LOG_INFO(
						"Bow grabbed [off-hand]: %s formId=%08X (Biting Axes world embed active, auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				LOG_INFO(
					"Bow grabbed [off-hand]: %s formId=%08X",
					itemName,
					grabbedRefr->formID);

				ScheduleGrabbedWeaponActivation(isLeft, grabbedRefr);
				return;
			}

			if (IsCrossbowWeaponForm(itemForm))
			{
				const char* itemName = GetSafeFormName(itemForm);

				if (IsOffHandVRController(isLeft))
				{
					LOG_INFO(
						"Crossbow grabbed [off-hand]: %s formId=%08X (auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				if (ShouldSuppressGrabAutoEquip(isLeft, grabbedRefr))
				{
					LOG_INFO(
						"Crossbow grabbed [main hand]: %s formId=%08X (trigger hold drop, auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				if (ShouldSuppressForBitingAxesWorldEmbed())
				{
					LOG_INFO(
						"Crossbow grabbed [main hand]: %s formId=%08X (Biting Axes world embed active, auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				LOG_INFO(
					"Crossbow grabbed [main hand]: %s formId=%08X",
					itemName,
					grabbedRefr->formID);

				ScheduleGrabbedWeaponActivation(isLeft, grabbedRefr);
				return;
			}

			if (IsTorchLightForm(itemForm))
			{
				const char* itemName = GetSafeFormName(itemForm);

				if (IsMainHandVRController(isLeft))
				{
					LOG_INFO(
						"Torch grabbed [main hand]: %s formId=%08X (auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				if (ShouldSuppressGrabAutoEquip(isLeft, grabbedRefr))
				{
					LOG_INFO(
						"Torch grabbed [off-hand]: %s formId=%08X (trigger hold drop, auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				if (ShouldSuppressForBitingAxesWorldEmbed())
				{
					LOG_INFO(
						"Torch grabbed [off-hand]: %s formId=%08X (Biting Axes world embed active, auto-equip suppressed)",
						itemName,
						grabbedRefr->formID);
					return;
				}

				LOG_INFO(
					"Torch grabbed [off-hand]: %s formId=%08X",
					itemName,
					grabbedRefr->formID);

				ScheduleGrabbedWeaponActivation(isLeft, grabbedRefr);
				return;
			}

			if (!IsTrackedItemForm(itemForm))
			{
				return;
			}

			const char* typeLabel = GetTrackedItemTypeLabel(itemForm);
			if (!typeLabel)
			{
				return;
			}

			const char* itemName = GetSafeFormName(itemForm);

			if (ShouldSuppressGrabAutoEquip(isLeft, grabbedRefr))
			{
				LOG_INFO(
					"Item grabbed [%s hand]: %s (%s) formId=%08X (trigger hold drop, auto-equip suppressed)",
					isLeft ? "left" : "right",
					itemName,
					typeLabel,
					grabbedRefr->formID);
				return;
			}

			if (ShouldSuppressForBitingAxesWorldEmbed())
			{
				LOG_INFO(
					"Item grabbed [%s hand]: %s (%s) formId=%08X (Biting Axes world embed active, auto-equip suppressed)",
					isLeft ? "left" : "right",
					itemName,
					typeLabel,
					grabbedRefr->formID);
				return;
			}

			LOG_INFO(
				"Item grabbed [%s hand]: %s (%s) formId=%08X",
				isLeft ? "left" : "right",
				itemName,
				typeLabel,
				grabbedRefr->formID);

			ScheduleGrabbedWeaponActivation(isLeft, grabbedRefr);
		}
	}

	void InitBitingAxesCompatibility(const PluginHandle& pluginHandle, SKSEMessagingInterface* messagingInterface)
	{
		g_bitingAxesApi = nullptr;
		if (!messagingInterface)
		{
			return;
		}

		BitingAxesMessage message{};
		const bool dispatched = messagingInterface->Dispatch(
			pluginHandle,
			BitingAxesMessage::kMessage_GetInterface,
			&message,
			static_cast<UInt32>(sizeof(message)),
			"BitingAxesVR");
		if (!dispatched || !message.GetApiFunction)
		{
			return;
		}

		g_bitingAxesApi = static_cast<IBitingAxesInterface001Compat*>(message.GetApiFunction(1));
		if (g_bitingAxesApi)
		{
			LOG_INFO("Biting Axes compatibility active (API build %u).", g_bitingAxesApi->GetBuildNumber());
		}
	}

	void RegisterWeaponGrabLogger()
	{
		if (!higgsInterface)
		{
			LOG_ERR("Weapon grab logging requires HIGGS. Install HIGGS and reload the game.");
			return;
		}

		higgsInterface->AddGrabbedCallback(OnWeaponGrabbed);
		LOG_INFO(
			"Item grab handler registered (dagger, 1H sword, 1H axe, 1H mace, off-hand bow, main-hand crossbow%s%s%s%s).",
			enableStaves ? ", staff" : "",
			enableTorches ? ", off-hand torch" : "",
			enableTwoHandedWeapons ? ", 2H sword, 2H axe" : "",
			enableShields ? ", shield" : "");
	}
}
