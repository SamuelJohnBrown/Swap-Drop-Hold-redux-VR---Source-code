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
			if (!enableGrabToEquip)
			{
				return;
			}

			if (!grabbedRefr || !grabbedRefr->baseForm)
			{
				return;
			}

			TESForm* weaponForm = grabbedRefr->baseForm;
			if (!IsTrackedWeaponForm(weaponForm))
			{
				return;
			}

			auto* weapon = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
			if (!weapon)
			{
				return;
			}

			const char* typeLabel = GetTrackedWeaponTypeLabel(weapon->type());
			if (!typeLabel)
			{
				return;
			}

			const char* weaponName = GetSafeFormName(weapon);

			if (ShouldSuppressGrabAutoEquip(isLeft, grabbedRefr))
			{
				LOG_INFO(
					"Weapon grabbed [%s hand]: %s (%s) formId=%08X (trigger hold drop, auto-equip suppressed)",
					isLeft ? "left" : "right",
					weaponName,
					typeLabel,
					grabbedRefr->formID);
				return;
			}

			if (ShouldSuppressForBitingAxesWorldEmbed())
			{
				LOG_INFO(
					"Weapon grabbed [%s hand]: %s (%s) formId=%08X (Biting Axes world embed active, auto-equip suppressed)",
					isLeft ? "left" : "right",
					weaponName,
					typeLabel,
					grabbedRefr->formID);
				return;
			}

			LOG_INFO(
				"Weapon grabbed [%s hand]: %s (%s) formId=%08X",
				isLeft ? "left" : "right",
				weaponName,
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
			"Weapon grab handler registered (dagger, 1H sword, 1H axe, 1H mace%s%s).",
			enableStaves ? ", staff" : "",
			enableTwoHandedWeapons ? ", 2H sword, 2H axe" : "");
	}
}
