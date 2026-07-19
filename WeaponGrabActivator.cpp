#include "WeaponGrabActivator.h"
#include "WeaponDrawHandler.h"

#include "Engine.h"
#include "TrackedWeapons.h"
#include "config.h"

#include <skse64/GameObjects.h>
#include <skse64/GameData.h>
#include <skse64/GameExtraData.h>
#include <skse64/gamethreads.h>

namespace SwapDropAndHoldRedux
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		typedef bool (*_TESObjectREFR_Activate)(TESObjectREFR*, TESObjectREFR*, UInt32, UInt32, UInt32, bool);
		RelocAddr<_TESObjectREFR_Activate> RefActivate(0x2A8300);
		static RelocPtr<bool> s_leftHandedMode(0x01E71778);

		static const int kEquipSettleFrames = 3;

		bool GrabHandToGameHand(const bool isLeftGrabHand)
		{
			if (s_leftHandedMode && *s_leftHandedMode)
			{
				return !isLeftGrabHand;
			}

			return isLeftGrabHand;
		}

		bool ItemInInventory(PlayerCharacter* player, TESForm* item)
		{
			if (!player || !item)
			{
				return false;
			}

			auto* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
			{
				return false;
			}

			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
			return entryData && entryData->countDelta > 0;
		}

		BaseExtraList* FindUnwornInventoryExtraData(PlayerCharacter* player, TESForm* weaponForm)
		{
			if (!player || !weaponForm)
			{
				return nullptr;
			}

			auto* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
			{
				return nullptr;
			}

			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
			if (!entryData || !entryData->extendDataList)
			{
				return nullptr;
			}

			for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
			{
				BaseExtraList* extraList = it.Get();
				if (!extraList)
				{
					continue;
				}

				if (extraList->HasType(kExtraData_Worn) || extraList->HasType(kExtraData_WornLeft))
				{
					continue;
				}

				return extraList;
			}

			return nullptr;
		}

		bool ActivateGrabbedRef(TESObjectREFR* grabbedRefr, PlayerCharacter* player)
		{
			if (!grabbedRefr || !player || !grabbedRefr->baseForm)
			{
				return false;
			}

			if (ItemInInventory(player, grabbedRefr->baseForm))
			{
				return true;
			}

			return RefActivate(grabbedRefr, player, 0, 0, 1, false);
		}

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

		static const int kSwapPullSettleFrames = 8;

		bool TransferWeaponToOppositeHand(
			PlayerCharacter* player,
			const bool isLeftSourceHand,
			const UInt32 expectedWeaponFormID)
		{
			if (!player || expectedWeaponFormID == 0)
			{
				return false;
			}

			TESForm* item = LookupFormByID(expectedWeaponFormID);
			if (!item || !IsTrackedWeaponForm(item) || !item->Has3D())
			{
				return false;
			}

			const bool isLeftDestHand = !isLeftSourceHand;
			if (player->GetEquippedObject(isLeftDestHand) == item)
			{
				return true;
			}

			::EquipManager* equipManager = ::EquipManager::GetSingleton();
			if (!equipManager)
			{
				return false;
			}

			auto* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
			{
				return false;
			}

			InventoryEntryData::EquipData itemData;
			containerChanges->data->GetEquipItemData(itemData, item, 0);
			if (itemData.itemCount <= 0)
			{
				return false;
			}

			BGSEquipSlot* destSlot = isLeftDestHand ? GetLeftHandSlot() : GetRightHandSlot();
			if (!destSlot)
			{
				return false;
			}

			const bool alreadyOnDest =
				(isLeftDestHand && itemData.isItemWornLeft) || (!isLeftDestHand && itemData.isItemWorn);
			if (alreadyOnDest)
			{
				return true;
			}

			const bool isItemEquipped = itemData.isItemWorn || itemData.isItemWornLeft;
			BaseExtraList* newEquipList = itemData.itemExtraList;

			// Unequip from the source hand FIRST, otherwise the engine happily
			// dual-wields the weapon and it stays in both hands.
			if (isItemEquipped)
			{
				BaseExtraList* unequipList = itemData.isItemWornLeft ? itemData.wornLeftExtraList : itemData.wornExtraList;
				if (!unequipList)
				{
					unequipList = itemData.wornExtraList ? itemData.wornExtraList : itemData.wornLeftExtraList;
				}

				if (!unequipList)
				{
					return false;
				}

				BSExtraData* xCannotWear = unequipList->GetByType(kExtraData_CannotWear);
				if (xCannotWear)
				{
					unequipList->Remove(kExtraData_CannotWear, xCannotWear);
				}

				const bool unequipDestroyed = CALL_MEMBER_FN(equipManager, UnequipItem)(
					player, item, unequipList, 1, nullptr, false, false, true, false, nullptr);

				newEquipList = unequipDestroyed ? nullptr : unequipList;
			}

			CALL_MEMBER_FN(equipManager, EquipItem)(
				player, item, newEquipList, 1, destSlot, false, true, false, nullptr);

			if (player->GetEquippedObject(isLeftDestHand) != item)
			{
				CALL_MEMBER_FN(equipManager, EquipItem)(
					player, item, nullptr, 1, destSlot, false, true, false, nullptr);
			}

			if (player->GetEquippedObject(isLeftDestHand) != item)
			{
				return false;
			}

			// If the engine still shows it on the source hand too, force it off.
			if (player->GetEquippedObject(isLeftSourceHand) == item)
			{
				containerChanges->data->GetEquipItemData(itemData, item, 0);

				BaseExtraList* sourceWornList = isLeftSourceHand ? itemData.wornLeftExtraList : itemData.wornExtraList;
				if (sourceWornList)
				{
					BSExtraData* xCannotWear = sourceWornList->GetByType(kExtraData_CannotWear);
					if (xCannotWear)
					{
						sourceWornList->Remove(kExtraData_CannotWear, xCannotWear);
					}

					BGSEquipSlot* sourceSlot = isLeftSourceHand ? GetLeftHandSlot() : GetRightHandSlot();
					CALL_MEMBER_FN(equipManager, UnequipItem)(
						player, item, sourceWornList, 1, sourceSlot, false, true, true, false, nullptr);
				}
			}

			return true;
		}

		bool UnequipTrackedWeaponFromHand(
			PlayerCharacter* player,
			const bool isLeftGameHand,
			const UInt32 expectedWeaponFormID)
		{
			if (!player || expectedWeaponFormID == 0)
			{
				return false;
			}

			TESForm* item = player->GetEquippedObject(isLeftGameHand);
			if (!item || item->formID != expectedWeaponFormID)
			{
				return true;
			}

			if (!IsTrackedWeaponForm(item))
			{
				return false;
			}

			::EquipManager* equipManager = ::EquipManager::GetSingleton();
			if (!equipManager)
			{
				return false;
			}

			auto* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
			{
				return false;
			}

			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
			if (!entryData)
			{
				return false;
			}

			BaseExtraList* rightEquipList = nullptr;
			BaseExtraList* leftEquipList = nullptr;
			entryData->GetExtraWornBaseLists(&rightEquipList, &leftEquipList);

			BaseExtraList* equipList = isLeftGameHand ? leftEquipList : rightEquipList;
			BGSEquipSlot* equipSlot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
			if (!equipList)
			{
				if (rightEquipList)
				{
					equipList = rightEquipList;
					equipSlot = GetRightHandSlot();
				}
				else if (leftEquipList)
				{
					equipList = leftEquipList;
					equipSlot = GetLeftHandSlot();
				}
			}

			if (!equipList || !equipSlot)
			{
				return false;
			}

			BSExtraData* xCannotWear = equipList->GetByType(kExtraData_CannotWear);
			if (xCannotWear)
			{
				equipList->Remove(kExtraData_CannotWear, xCannotWear);
			}

			CALL_MEMBER_FN(equipManager, UnequipItem)(
				player, item, equipList, 1, equipSlot, false, true, true, false, nullptr);

			item = player->GetEquippedObject(isLeftGameHand);
			return !item || item->formID != expectedWeaponFormID;
		}

		void EquipWeaponToGrabHand(PlayerCharacter* player, TESForm* weaponForm, const bool isLeftGameHand)
		{
			if (!player || !weaponForm || !weaponForm->Has3D())
			{
				return;
			}

			TESForm* equippedInGrabHand = player->GetEquippedObject(isLeftGameHand);
			if (equippedInGrabHand == weaponForm)
			{
				return;
			}

			::EquipManager* equipMan = ::EquipManager::GetSingleton();
			if (!equipMan)
			{
				return;
			}

			// extraData may legitimately be null for a plain weapon with no
			// inventory extra lists; EquipItem handles a null extra fine.
			BaseExtraList* extraData = FindUnwornInventoryExtraData(player, weaponForm);
			BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
			CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, extraData, 1, slot, false, true, false, nullptr);
		}

		class EquipGrabbedWeaponTask : public TaskDelegate
		{
		public:
			EquipGrabbedWeaponTask(const bool isLeftGameHand, const UInt32 weaponFormID, const int settleFramesRemaining = kEquipSettleFrames)
				: m_isLeftGameHand(isLeftGameHand)
				, m_weaponFormID(weaponFormID)
				, m_settleFramesRemaining(settleFramesRemaining)
			{
			}

			virtual void Run() override
			{
				if (m_settleFramesRemaining > 0)
				{
					if (g_task)
					{
						g_task->AddTask(new EquipGrabbedWeaponTask(
							m_isLeftGameHand, m_weaponFormID, m_settleFramesRemaining - 1));
					}
					return;
				}

				PlayerCharacter* player = *g_thePlayer;
				TESForm* weaponForm = LookupFormByID(m_weaponFormID);
				if (!player || !weaponForm)
				{
					return;
				}

				if (!ItemInInventory(player, weaponForm))
				{
					LOG_ERR("Equip skipped, weapon not in inventory yet: formId=%08X", m_weaponFormID);
					return;
				}

				EquipWeaponToGrabHand(player, weaponForm, m_isLeftGameHand);
				ScheduleWeaponDrawMaintenance(m_isLeftGameHand);

				LOG_INFO(
					"Weapon equipped to %s hand: %s formId=%08X",
					m_isLeftGameHand ? "left" : "right",
					GetSafeFormName(weaponForm),
					m_weaponFormID);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftGameHand;
			UInt32 m_weaponFormID;
			int m_settleFramesRemaining;
		};

		class SwapPullEquipTask : public TaskDelegate
		{
		public:
			enum Phase
			{
				kSettle = 0,
				kEquip = 1
			};

			SwapPullEquipTask(
				const bool isLeftSourceHand,
				const UInt32 weaponFormID,
				const float pullSpeed,
				const Phase phase = kSettle,
				const int settleFramesRemaining = kSwapPullSettleFrames)
				: m_isLeftSourceHand(isLeftSourceHand)
				, m_weaponFormID(weaponFormID)
				, m_pullSpeed(pullSpeed)
				, m_phase(phase)
				, m_settleFramesRemaining(settleFramesRemaining)
			{
			}

			virtual void Run() override
			{
				PlayerCharacter* player = *g_thePlayer;
				TESForm* weaponForm = LookupFormByID(m_weaponFormID);
				if (!player || !weaponForm || !IsTrackedWeaponForm(weaponForm))
				{
					return;
				}

				if (m_phase == kSettle)
				{
					if (m_settleFramesRemaining > 0)
					{
						if (g_task)
						{
							g_task->AddTask(new SwapPullEquipTask(
								m_isLeftSourceHand,
								m_weaponFormID,
								m_pullSpeed,
								kSettle,
								m_settleFramesRemaining - 1));
						}
						return;
					}

					if (g_task)
					{
						g_task->AddTask(new SwapPullEquipTask(
							m_isLeftSourceHand, m_weaponFormID, m_pullSpeed, kEquip, 0));
					}
					return;
				}

				const bool isLeftDestHand = !m_isLeftSourceHand;
				if (!TransferWeaponToOppositeHand(player, m_isLeftSourceHand, m_weaponFormID))
				{
					LOG_ERR(
						"Swap pull failed: could not equip formId=%08X to %s hand",
						m_weaponFormID,
						isLeftDestHand ? "left" : "right");
					return;
				}

				ScheduleWeaponDrawMaintenance(isLeftDestHand);

				auto* weapon = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
				const char* typeLabel = weapon ? GetTrackedWeaponTypeLabel(weapon->type()) : nullptr;

				LOG_INFO(
					"Swap pull complete: moved %s (%s) from %s hand to %s hand formId=%08X pullSpeed=%.1f",
					GetSafeFormName(weaponForm),
					typeLabel ? typeLabel : "Weapon",
					m_isLeftSourceHand ? "left" : "right",
					isLeftDestHand ? "left" : "right",
					m_weaponFormID,
					m_pullSpeed);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftSourceHand;
			UInt32 m_weaponFormID;
			float m_pullSpeed;
			Phase m_phase;
			int m_settleFramesRemaining;
		};

		class PickupGrabbedWeaponTask : public TaskDelegate
		{
		public:
			PickupGrabbedWeaponTask(const bool isLeftGameHand, const UInt32 weaponFormID, const UInt32 weaponRefID, const int retriesRemaining = 12)
				: m_isLeftGameHand(isLeftGameHand)
				, m_weaponFormID(weaponFormID)
				, m_weaponRefID(weaponRefID)
				, m_retriesRemaining(retriesRemaining)
			{
			}

			virtual void Run() override
			{
				PlayerCharacter* player = *g_thePlayer;
				TESForm* weaponForm = LookupFormByID(m_weaponFormID);
				if (!player || !weaponForm)
				{
					return;
				}

				if (!ItemInInventory(player, weaponForm))
				{
					TESObjectREFR* weaponRef = nullptr;
					if (m_weaponRefID != 0)
					{
						TESForm* refForm = LookupFormByID(m_weaponRefID);
						weaponRef = DYNAMIC_CAST(refForm, TESForm, TESObjectREFR);
					}

					if (weaponRef)
					{
						ActivateGrabbedRef(weaponRef, player);
					}
					else
					{
						AddItem_Native(nullptr, 0, player, weaponForm, 1, true);
					}
				}

				if (!ItemInInventory(player, weaponForm))
				{
					if (m_retriesRemaining > 0 && g_task)
					{
						g_task->AddTask(new PickupGrabbedWeaponTask(
							m_isLeftGameHand, m_weaponFormID, m_weaponRefID, m_retriesRemaining - 1));
					}
					else
					{
						LOG_ERR("Failed to add grabbed weapon to inventory: formId=%08X", m_weaponFormID);
					}
					return;
				}

				if (g_task)
				{
					g_task->AddTask(new EquipGrabbedWeaponTask(m_isLeftGameHand, m_weaponFormID));
				}
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftGameHand;
			UInt32 m_weaponFormID;
			UInt32 m_weaponRefID;
			int m_retriesRemaining;
		};
	}

	void ScheduleGrabbedWeaponActivation(const bool isLeftGrabHand, TESObjectREFR* grabbedRefr)
	{
		if (!grabbedRefr || !grabbedRefr->baseForm || !g_task)
		{
			return;
		}

		const bool isLeftGameHand = GrabHandToGameHand(isLeftGrabHand);
		g_task->AddTask(new PickupGrabbedWeaponTask(
			isLeftGameHand,
			grabbedRefr->baseForm->formID,
			grabbedRefr->formID));
	}

	void ScheduleSwapPullToOppositeHand(
		const bool isLeftSourceHand,
		const UInt32 weaponFormID,
		const float pullSpeed)
	{
		if (!g_task || weaponFormID == 0)
		{
			return;
		}

		g_task->AddTask(new SwapPullEquipTask(isLeftSourceHand, weaponFormID, pullSpeed));
	}
}
