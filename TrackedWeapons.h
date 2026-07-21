#pragma once

#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameReferences.h>
#include <skse64/GameObjects.h>
#include <skse64/GameRTTI.h>

namespace SwapDropAndHoldRedux
{
	UInt32 GetFullFormIdMine(const char* espName, UInt32 baseFormId);

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

	inline bool IsStaffWeaponTypeRaw(const UInt8 weaponType)
	{
		using WeaponType = TESObjectWEAP::GameData;

		switch (weaponType)
		{
		case WeaponType::kType_Staff:
		case WeaponType::kType_Staff2:
			return true;
		default:
			return false;
		}
	}

	inline bool IsStaffWeaponType(const UInt8 weaponType)
	{
		return enableStaves && IsStaffWeaponTypeRaw(weaponType);
	}

	// WeapTypeBoundWeapon (Skyrim.esm 0x0010D501). Covers vanilla + most modded bound weapons.
	inline bool IsBoundWeaponForm(TESForm* form)
	{
		if (!form || form->formType != kFormType_Weapon)
		{
			return false;
		}

		auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
		if (!weapon)
		{
			return false;
		}

		static const UInt32 kWeapTypeBoundWeapon = 0x0010D501;
		static BGSKeyword* s_boundKeyword = nullptr;
		static bool s_keywordResolved = false;
		if (!s_keywordResolved)
		{
			s_keywordResolved = true;
			s_boundKeyword = DYNAMIC_CAST(LookupFormByID(kWeapTypeBoundWeapon), TESForm, BGSKeyword);
		}

		if (s_boundKeyword && weapon->keyword.HasKeyword(s_boundKeyword))
		{
			return true;
		}

		// Fallback: vanilla Skyrim.esm bound weapon base IDs (lower 24 bits).
		switch (form->formID & 0x00FFFFFF)
		{
		case 0x0058F5F: // Bound Sword
		case 0x00424F9: // Bound Sword (Mystic)
		case 0x00BA30E: // Bound Sword (alt)
		case 0x0058F5E: // Bound Battleaxe
		case 0x00424F7: // Bound Battleaxe (Mystic)
		case 0x0058F60: // Bound Bow
		case 0x00424F8: // Bound Bow (Mystic)
		case 0x01CE02:  // Bound Dagger (Dawnguard)
			return true;
		default:
			return false;
		}
	}

	inline bool IsTrackedWeaponType(const UInt8 weaponType)
	{
		using WeaponType = TESObjectWEAP::GameData;

		if (IsTwoHandedWeaponType(weaponType))
		{
			return true;
		}

		if (IsStaffWeaponType(weaponType))
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
			return true;
		default:
			return false;
		}
	}

	// Specific weapon records excluded from all Swap Drop & Hold handling.
	// Skyrim.esm always sits at load index 00, so its full form ids are exact.
	inline bool IsExcludedWeaponForm(TESForm* form)
	{
		if (!form)
		{
			return false;
		}

		switch (form->formID)
		{
		case 0x000426C8: // Shiv (Skyrim.esm)
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

		if (IsExcludedWeaponForm(form))
		{
			return false;
		}

		if (IsBoundWeaponForm(form))
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

	// 2H Weapons Unlocked (Weapon Unlocked VR.esp) either-hand proxy records. These are authored as
	// 1H weapons so they pass the 1H type checks, but they stand in for greatswords/warhammers/
	// battleaxes and must be treated as 2H here.
	inline bool IsTwoHandProxyForm(TESForm* form)
	{
		if (!form || form->formType != kFormType_Weapon)
		{
			return false;
		}

		static const char* kProxyPlugin = "Weapon Unlocked VR.esp";
		static const UInt32 kProxyBaseIds[] = {
			0x015C70, // greatsword proxy
			0x015C72, // warhammer proxy
			0x015C74, // battleaxe proxy
		};
		static UInt32 s_proxyFormIds[3] = { 0, 0, 0 };
		static bool s_resolved = false;

		if (!s_resolved)
		{
			if (!DataHandler::GetSingleton())
			{
				return false;
			}

			s_resolved = true;
			for (int i = 0; i < 3; ++i)
			{
				s_proxyFormIds[i] = GetFullFormIdMine(kProxyPlugin, kProxyBaseIds[i]);
			}
		}

		for (int i = 0; i < 3; ++i)
		{
			if (s_proxyFormIds[i] != 0 && form->formID == s_proxyFormIds[i])
			{
				return true;
			}
		}

		return false;
	}

	// Weapons excluded from the cross-body hand-swap (swap pull) feature: real 2H weapons
	// (by animation type, regardless of the EnableTwoHandedWeapons setting) and the 2H Weapons
	// Unlocked proxy forms. Overridden by the EnableTwoHandedHandSwapping INI setting.
	inline bool IsSwapPullExcludedForm(TESForm* form)
	{
		if (enableTwoHandedHandSwapping)
		{
			return false;
		}

		if (!form || form->formType != kFormType_Weapon)
		{
			return false;
		}

		if (IsTwoHandProxyForm(form))
		{
			return true;
		}

		auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
		return weapon && IsTwoHandedWeaponTypeRaw(weapon->type());
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
			return enableStaves ? "Staff" : nullptr;
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

