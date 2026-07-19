#pragma once

#include "config.h"

#include <skse64/GameReferences.h>
#include <skse64/GameObjects.h>
#include <skse64/GameRTTI.h>

namespace SwapDropAndHoldRedux
{
	inline bool IsTwoHandedWeaponTypeRaw(const UInt8 weaponType)
	{
		using WeaponType = TESObjectWEAP::GameData;

		switch (weaponType)
		{
		case WeaponType::kType_TwoHandSword:
		case WeaponType::kType_2HS:
		case WeaponType::kType_TwoHandAxe:
		case WeaponType::kType_2HA:
			return true;
		default:
			return false;
		}
	}

	inline bool IsTwoHandedWeaponType(const UInt8 weaponType)
	{
		return enableTwoHandedWeapons && IsTwoHandedWeaponTypeRaw(weaponType);
	}

	inline bool IsTrackedWeaponType(const UInt8 weaponType)
	{
		using WeaponType = TESObjectWEAP::GameData;

		if (IsTwoHandedWeaponType(weaponType))
		{
			return true;
		}

		switch (weaponType)
		{
		case WeaponType::kType_OneHandSword:
		case WeaponType::kType_1HS:
		case WeaponType::kType_OneHandDagger:
		case WeaponType::kType_1HD:
		case WeaponType::kType_OneHandAxe:
		case WeaponType::kType_1HA:
		case WeaponType::kType_OneHandMace:
		case WeaponType::kType_1HM:
		case WeaponType::kType_Staff:
		case WeaponType::kType_Staff2:
			return true;
		default:
			return false;
		}
	}

	inline bool IsTrackedWeaponForm(TESForm* form)
	{
		if (!form || form->formType != kFormType_Weapon)
		{
			return false;
		}

		auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
		return weapon && IsTrackedWeaponType(weapon->type());
	}

	inline bool IsTwoHandedWeaponForm(TESForm* form)
	{
		if (!form || form->formType != kFormType_Weapon)
		{
			return false;
		}

		auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
		return weapon && IsTwoHandedWeaponType(weapon->type());
	}

	inline bool VRControllerToGameHand(const bool isLeftVRController)
	{
		if (leftHandedMode)
		{
			return !isLeftVRController;
		}

		return isLeftVRController;
	}

	struct TrackedWeaponHandInfo
	{
		TESForm* weapon = nullptr;
		bool isLeftGameHand = false;
	};

	inline TrackedWeaponHandInfo GetTrackedWeaponForVRController(
		PlayerCharacter* player,
		const bool isLeftVRController)
	{
		TrackedWeaponHandInfo info{};
		if (!player)
		{
			return info;
		}

		const bool primaryHand = VRControllerToGameHand(isLeftVRController);
		TESForm* equipped = player->GetEquippedObject(primaryHand);
		if (IsTrackedWeaponForm(equipped))
		{
			info.weapon = equipped;
			info.isLeftGameHand = primaryHand;
			return info;
		}

		if (enableTwoHandedWeapons)
		{
			const bool otherHand = !primaryHand;
			TESForm* otherEquipped = player->GetEquippedObject(otherHand);
			if (IsTwoHandedWeaponForm(otherEquipped))
			{
				info.weapon = otherEquipped;
				info.isLeftGameHand = otherHand;
			}
		}

		return info;
	}

	inline const char* GetTrackedWeaponTypeLabel(const UInt8 weaponType)
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
		case WeaponType::kType_TwoHandSword:
		case WeaponType::kType_2HS:
			return "2H Sword";
		case WeaponType::kType_TwoHandAxe:
		case WeaponType::kType_2HA:
			return "2H Axe";
		default:
			return nullptr;
		}
	}

	// TESForm::GetName() resolves to the wrong vtable slot on Skyrim VR (lands in
	// BelongsInGroup and can CTD). Read the TESFullName component directly instead.
	inline const char* GetSafeFormName(TESForm* form)
	{
		if (!form)
		{
			return "<unnamed>";
		}

		TESFullName* fullName = DYNAMIC_CAST(form, TESForm, TESFullName);
		const char* name = fullName ? fullName->name.data : nullptr;
		return (name && name[0]) ? name : "<unnamed>";
	}
}

