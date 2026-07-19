#pragma once

#include <skse64/GameReferences.h>
#include <skse64/GameObjects.h>
#include <skse64/GameRTTI.h>

namespace SwapDropAndHoldRedux
{
	inline bool IsTrackedWeaponType(const UInt8 weaponType)
	{
		using WeaponType = TESObjectWEAP::GameData;

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
