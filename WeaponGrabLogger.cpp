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
		const char* GetTrackedWeaponTypeLabel(const UInt8 weaponType)
		{
			using WeaponType = TESObjectWEAP::GameData;

			switch (weaponType)
			{
			case WeaponType::kType_OneHandSword:
			case WeaponType::kType_1HS:
				return "1H Sword";

			case WeaponType::kType_OneHandDagger:
			case WeaponType::kType_1HD:
				return "Dagger";

			case WeaponType::kType_OneHandAxe:
			case WeaponType::kType_1HA:
				return "1H Axe";

			case WeaponType::kType_OneHandMace:
			case WeaponType::kType_1HM:
				return "1H Mace";

			case WeaponType::kType_Staff:
			case WeaponType::kType_Staff2:
				return "Staff";

			default:
				return nullptr;
			}
		}

		void OnWeaponGrabbed(const bool isLeft, TESObjectREFR* grabbedRefr)
		{
			if (!grabbedRefr || !grabbedRefr->baseForm)
			{
				return;
			}

			auto* weapon = DYNAMIC_CAST(grabbedRefr->baseForm, TESForm, TESObjectWEAP);
			if (!weapon)
			{
				return;
			}

			const UInt8 weaponType = weapon->type();
			const char* typeLabel = GetTrackedWeaponTypeLabel(weaponType);
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

			LOG_INFO(
				"Weapon grabbed [%s hand]: %s (%s) formId=%08X",
				isLeft ? "left" : "right",
				weaponName,
				typeLabel,
				grabbedRefr->formID);

			ScheduleGrabbedWeaponActivation(isLeft, grabbedRefr);
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
		LOG_INFO("Weapon grab handler registered (dagger, 1H sword, 1H axe, 1H mace, staff).");
	}
}
