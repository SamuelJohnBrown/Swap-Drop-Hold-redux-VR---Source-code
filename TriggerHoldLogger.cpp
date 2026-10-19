#include "TriggerHoldLogger.h"

#include "DrawWatch.h"
#include "DropModelWatch.h"
#include "HiggsHandWatch.h"
#include "HiggsRelease.h"
#include "VrikHolsterApi.h"
#include "Engine.h"
#include "EmbedPins.h"
#include "GripHold.h"
#include "Helper.h"
#include "SilentEquip.h"
#include "TrackedWeapons.h"
#include "config.h"
#include "swapdropandholdreduxinterface001.h"

#include <skse64/GameData.h>
#include <skse64/GameExtraData.h>
#include <skse64/GameObjects.h>
#include <skse64/GameReferences.h>
#include <skse64/GameVR.h>
#include <skse64/gamethreads.h>

#include <chrono>

namespace SwapDropAndHoldRedux
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		static const uint64_t TRIGGER_BUTTON_MASK = (1ull << 33);
		#define GRIP_BUTTON_MASK (gripButtonMask)

		uint64_t ButtonMaskFromId(const int buttonId)
		{
			if (buttonId < 0 || buttonId > 63)
			{
				return 0;
			}

			return 1ull << buttonId;
		}

		bool IsButtonPressedOrTouched(
			const vr_1_0_12::VRControllerState_t& state,
			const uint64_t mask)
		{
			return ((state.ulButtonPressed & mask) != 0) ||
				((state.ulButtonTouched & mask) != 0);
		}

		bool IsConfiguredDropButtonActive(const vr_1_0_12::VRControllerState_t& state)
		{
			const uint64_t mask = ButtonMaskFromId(dropButtonId);
			if (mask == 0)
			{
				return false;
			}

			if (dropButtonId == 33)
			{
				return ((state.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0) ||
					state.rAxis[1].x > 0.5f;
			}

			if (dropButtonId == 2)
			{
				return IsButtonPressedOrTouched(state, GRIP_BUTTON_MASK);
			}

			return (state.ulButtonPressed & mask) != 0;
		}

		struct HandTriggerState
		{
			enum Phase
			{
				kIdle = 0,
				kTapPressing = 1,   // first press — must release quickly to arm
				kTapArmed = 2,     // tap completed; waiting for hold press
				kHoldPressing = 3, // second press — accumulate hold for drop
			};

			Phase phase = kIdle;
			float tapPressTime = 0.0f;
			float tapArmedRemaining = 0.0f;
			float holdTime = 0.0f;
			int tapsCompleted = 0;
			bool actionedThisHold = false;
			bool wasPressed = false;
			bool wasSecondaryActive = false;
			bool secondaryHeldBeforePrimary = false;
			bool spellWheelBlocked = false;
		};

		static const float kMaxTapSeconds = 0.35f;
		static const float kTapArmWindowSeconds = 2.0f;
		// Max gap between the rapid taps required for bow/crossbow drops.
		static const float kNextTapWindowSeconds = 0.5f;

		// Bows/crossbows use the trigger for arrow nocking and firing — require a
		// rapid double tap before the hold so archery doesn't cause accidental drops.
		int RequiredTapCount(TESForm* equipped)
		{
			if (IsBowWeaponForm(equipped) || IsCrossbowWeaponForm(equipped))
			{
				return requireDoubleTapForBowCrossbowDrop ? 2 : 1;
			}

			return 1;
		}

		// Bows occupy both hand slots, so both controllers would otherwise resolve
		// them for the drop gesture. Restrict to the hand that owns the weapon:
		// bows drop from the off-hand controller only, crossbows from the main hand.
		bool IsDropAllowedFromController(TESForm* equipped, const bool isLeftVRController)
		{
			if (IsBowWeaponForm(equipped))
			{
				return IsOffHandVRController(isLeftVRController);
			}

			if (IsCrossbowWeaponForm(equipped))
			{
				return IsMainHandVRController(isLeftVRController);
			}

			if (IsTorchLightForm(equipped))
			{
				return IsOffHandVRController(isLeftVRController);
			}

			return true;
		}

		static HandTriggerState s_leftTriggerState;
		static HandTriggerState s_rightTriggerState;
		static HandTriggerState s_twoHandSharedTriggerState;
		static bool s_registered = false;

		void ResetTriggerDropProgress(HandTriggerState& state, const bool keepTapArmed = false)
		{
			state.holdTime = 0.0f;
			state.actionedThisHold = false;
			state.spellWheelBlocked = false;
			state.tapPressTime = 0.0f;
			if (!keepTapArmed)
			{
				state.phase = HandTriggerState::kIdle;
				state.tapArmedRemaining = 0.0f;
				state.tapsCompleted = 0;
			}
		}

		struct HandDropGuardState
		{
			UInt32 droppedRefFormID = 0;
			float secondsRemaining = 0.0f;
		};

		static HandDropGuardState s_leftDropGuard;
		static HandDropGuardState s_rightDropGuard;

		HandDropGuardState& GetDropGuard(const bool isLeftVRController)
		{
			return isLeftVRController ? s_leftDropGuard : s_rightDropGuard;
		}

		void MarkTriggerHoldDroppedGrab(const bool isLeftVRController, const UInt32 droppedRefFormID)
		{
			HandDropGuardState& guard = GetDropGuard(isLeftVRController);
			guard.droppedRefFormID = droppedRefFormID;
			guard.secondsRemaining = dropGuardTimeoutSeconds;
		}

		void ClearTriggerHoldDroppedGrab(const bool isLeftVRController)
		{
			HandDropGuardState& guard = GetDropGuard(isLeftVRController);
			guard.droppedRefFormID = 0;
			guard.secondsRemaining = 0.0f;
		}

		void UpdateDropGuardTimer(HandDropGuardState& guard, const float deltaTime)
		{
			if (guard.droppedRefFormID == 0)
			{
				return;
			}

			guard.secondsRemaining -= deltaTime;
			if (guard.secondsRemaining <= 0.0f)
			{
				guard.droppedRefFormID = 0;
				guard.secondsRemaining = 0.0f;
			}
		}

		void OnTriggerHoldWeaponDropped(const bool isLeftVRController, TESObjectREFR* droppedRefr)
		{
			if (!droppedRefr)
			{
				return;
			}

			HandDropGuardState& guard = GetDropGuard(isLeftVRController);
			if (guard.droppedRefFormID != 0 && guard.droppedRefFormID == droppedRefr->formID)
			{
				guard.droppedRefFormID = 0;
				guard.secondsRemaining = 0.0f;
			}
		}

		bool GetControllerState(const bool isLeftVRController, vr_1_0_12::VRControllerState_t& outState)
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
			{
				return false;
			}

			vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
			const vr_1_0_12::TrackedDeviceIndex_t controller = vrSystem->GetTrackedDeviceIndexForControllerRole(
				isLeftVRController
					? vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand
					: vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);

			return vrSystem->GetControllerState(controller, &outState, sizeof(outState));
		}

		bool IsGripActive(const vr_1_0_12::VRControllerState_t& state)
		{
			return IsButtonPressedOrTouched(state, GRIP_BUTTON_MASK);
		}

		bool IsTriggerActive(const vr_1_0_12::VRControllerState_t& state)
		{
			return ((state.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0) ||
				state.rAxis[1].x > 0.5f;
		}

		HandTriggerState& GetHandTriggerState(const bool isLeftVRController)
		{
			return isLeftVRController ? s_leftTriggerState : s_rightTriggerState;
		}

		bool ReadControllerInput(
			const bool isLeftVRController,
			bool& triggerPressed,
			bool& gripActive,
			bool& dropButtonPressed)
		{
			vr_1_0_12::VRControllerState_t controllerState{};
			if (!GetControllerState(isLeftVRController, controllerState))
			{
				triggerPressed = false;
				gripActive = false;
				dropButtonPressed = false;
				return false;
			}

			triggerPressed = IsTriggerActive(controllerState);
			gripActive = IsGripActive(controllerState);
			dropButtonPressed = IsConfiguredDropButtonActive(controllerState);
			return true;
		}

		bool IsSpellWheelOpenNow()
		{
			if (!spellwheelInterface)
			{
				return false;
			}

			return spellwheelInterface->IsMainWheelOpen() || spellwheelInterface->IsSecondaryWheelOpen();
		}

		bool IsSecondaryBlockingDrop(
			TESForm* weapon,
			const bool primaryPressed,
			const bool secondaryActive)
		{
			if (!primaryPressed || !secondaryActive)
			{
				return false;
			}

			// 2H weapons commonly keep both buttons active in VR; don't block their drop.
			if (IsTwoHandedWeaponForm(weapon))
			{
				return false;
			}

			return true;
		}

		void ResolveDropButtons(
			const bool triggerPressed,
			const bool gripActive,
			const bool dropButtonPressed,
			bool& outPrimaryPressed,
			bool& outSecondaryActive)
		{
			outPrimaryPressed = dropButtonPressed;

			// Spell-wheel style block uses the "other" common button.
			if (dropButtonId == 2)
			{
				outSecondaryActive = triggerPressed;
			}
			else
			{
				outSecondaryActive = gripActive;
			}
		}

		bool IsDropBlockedForHand(const bool isLeftVRController, TESForm* weapon)
		{
			if (IsInputCapturingMenuOpen())
			{
				return true;
			}

			if (IsSpellWheelOpenNow())
			{
				return true;
			}

			const HandTriggerState& state = GetHandTriggerState(isLeftVRController);
			if (state.spellWheelBlocked && !IsTwoHandedWeaponForm(weapon))
			{
				return true;
			}

			bool triggerPressed = false;
			bool gripActive = false;
			bool dropButtonPressed = false;
			if (!ReadControllerInput(isLeftVRController, triggerPressed, gripActive, dropButtonPressed))
			{
				return false;
			}

			bool primaryPressed = false;
			bool secondaryActive = false;
			ResolveDropButtons(triggerPressed, gripActive, dropButtonPressed, primaryPressed, secondaryActive);
			return IsSecondaryBlockingDrop(weapon, primaryPressed, secondaryActive);
		}

		NiPoint3 GetHandSpawnPosition(PlayerCharacter* player, const bool isLeftGameHand)
		{
			NiPoint3 spawnPos = player->pos;

			NiNode* rootNode = player->GetNiRootNode(0);
			if (!rootNode)
			{
				rootNode = player->GetNiRootNode(1);
			}

			if (!rootNode)
			{
				return spawnPos;
			}

			const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
			const char* handNodeName = isLeftVRController ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]";
			BSFixedString handNodeStr(handNodeName);
			NiAVObject* handNode = rootNode->GetObjectByName(&handNodeStr.data);
			if (handNode)
			{
				spawnPos = handNode->m_worldTransform.pos;
			}

			return spawnPos;
		}

		bool ResolveTrackedWeaponGameHand(
			PlayerCharacter* player,
			const UInt32 expectedWeaponFormID,
			bool& outIsLeftGameHand,
			TESForm*& outWeapon)
		{
			outWeapon = nullptr;

			if (!player || expectedWeaponFormID == 0)
			{
				return false;
			}

			TESForm* leftEquipped = player->GetEquippedObject(true);
			TESForm* rightEquipped = player->GetEquippedObject(false);

			// The caller's hand wins when it really holds this form. Searching left
			// first regardless dropped the WRONG copy when the same weapon was worn in
			// both hands, which read in-game as "the weapon vanished into my inventory".
			TESForm* preferredEquipped = outIsLeftGameHand ? leftEquipped : rightEquipped;
			if (preferredEquipped &&
				preferredEquipped->formID == expectedWeaponFormID &&
				IsGrabEquipDropItemForm(preferredEquipped))
			{
				outWeapon = preferredEquipped;
				return true;
			}

			if (leftEquipped && leftEquipped->formID == expectedWeaponFormID && IsGrabEquipDropItemForm(leftEquipped))
			{
				outWeapon = leftEquipped;
				outIsLeftGameHand = true;
				return true;
			}

			if (rightEquipped && rightEquipped->formID == expectedWeaponFormID && IsGrabEquipDropItemForm(rightEquipped))
			{
				outWeapon = rightEquipped;
				outIsLeftGameHand = false;
				return true;
			}

			return false;
		}

		// How many of this item the player actually owns. Equipped copies are still
		// in the inventory, so dual wielding one form reads as a stack of two.
		SInt32 InventoryItemCount(PlayerCharacter* player, TESForm* item);

		// Watches the hand the player did NOT open, after dropping one of a matched
		// pair, and says out loud what happens to it.
		//
		// Two previous attempts at this were built on AddTask requeueing. AddTask from
		// inside a task drains in the SAME frame, so "watch for 90 frames" completed in
		// zero milliseconds and saw nothing - and both builds duly reported "the other
		// hand kept its weapon" while the player was watching it leave. This ticks on
		// the HIGGS pre-physics callback, which is a genuine per-frame callback.
		//
		// It reports every change to that hand with the time since the drop, so if the
		// weapon does leave, the log finally says WHEN. That is the missing fact: the
		// cause is still unknown, and the two theories tried so far (an inventory stack
		// cascade, and the drop silently switching hands) were both wrong.
		struct DropWatch
		{
			UInt32 formID = 0;
			bool isLeftGameHand = false;
			float secondsLeft = 0.0f;
			float elapsed = 0.0f;
			UInt32 lastSeen = 0;
			bool repaired = false;
		};
		static DropWatch s_dropWatch;

		// A stack removal held back for a few frames.
		//
		// The other hand loses its weapon exactly one frame after the drop, every time,
		// which says the engine unequips it as a consequence of taking a copy out of the
		// shared stack. The suspicion is that UnequipItem on the dropped hand has not
		// finished settling when RemoveItem runs, so the engine still sees two worn
		// instances of one stack and picks the wrong one to unequip.
		//
		// Waiting a few frames costs a window in which the player owns both the world
		// model and the inventory copy. That window is ~50ms and the drop guard already
		// suppresses an immediate re-grab, so the duplication risk is small - but it is
		// the reason this is a setting and not a hard-coded delay.
		struct PendingRemoval
		{
			UInt32 formID = 0;
			float secondsLeft = 0.0f;
		};
		static PendingRemoval s_pendingRemoval;

		void FlushPendingRemoval(PlayerCharacter* player)
		{
			if (s_pendingRemoval.formID == 0)
			{
				return;
			}

			TESForm* form = LookupFormByID(s_pendingRemoval.formID);
			s_pendingRemoval.formID = 0;
			s_pendingRemoval.secondsLeft = 0.0f;

			if (!player || !form)
			{
				return;
			}

			RemoveItemFromInventory(player, form, 1, true);
			LOG_INFO("Drop inventory: deferred removal of %s applied.", GetSafeFormName(form));
		}

		void UpdatePendingRemoval(const float deltaTime)
		{
			if (s_pendingRemoval.formID == 0)
			{
				return;
			}

			s_pendingRemoval.secondsLeft -= deltaTime;
			if (s_pendingRemoval.secondsLeft > 0.0f)
			{
				return;
			}

			FlushPendingRemoval(*g_thePlayer);
		}

		SInt32 InventoryItemCount(PlayerCharacter* player, TESForm* item);

		void StartDropWatch(PlayerCharacter* player, const UInt32 formID, const bool isLeftGameHand)
		{
			if (!player || dropWatchSeconds <= 0.0f)
			{
				return;
			}

			TESForm* inHand = player->GetEquippedObject(isLeftGameHand);

			s_dropWatch.formID = formID;
			s_dropWatch.isLeftGameHand = isLeftGameHand;
			s_dropWatch.secondsLeft = dropWatchSeconds;
			s_dropWatch.elapsed = 0.0f;
			s_dropWatch.lastSeen = inHand ? inHand->formID : 0;
			s_dropWatch.repaired = false;

			LOG_INFO(
				"Drop watch: the %s hand holds %08X, watching it for %.1fs.",
				isLeftGameHand ? "left" : "right",
				s_dropWatch.lastSeen,
				dropWatchSeconds);
		}

		void UpdateDropWatch(const float deltaTime)
		{
			if (s_dropWatch.secondsLeft <= 0.0f)
			{
				return;
			}

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				s_dropWatch.secondsLeft = 0.0f;
				return;
			}

			s_dropWatch.secondsLeft -= deltaTime;
			s_dropWatch.elapsed += deltaTime;

			TESForm* inHand = player->GetEquippedObject(s_dropWatch.isLeftGameHand);
			const UInt32 nowSeen = inHand ? inHand->formID : 0;

			if (nowSeen != s_dropWatch.lastSeen)
			{
				LOG_INFO(
					"Drop watch: the %s hand went %08X -> %08X, %.2fs after the drop.",
					s_dropWatch.isLeftGameHand ? "left" : "right",
					s_dropWatch.lastSeen,
					nowSeen,
					s_dropWatch.elapsed);
				s_dropWatch.lastSeen = nowSeen;

				// It lost the weapon it was holding. Put it back, once.
				if (nowSeen == 0 && !s_dropWatch.repaired)
				{
					TESForm* form = LookupFormByID(s_dropWatch.formID);
					::EquipManager* equipManager = ::EquipManager::GetSingleton();

					if (!form || !equipManager)
					{
						return;
					}

					if (InventoryItemCount(player, form) < 1)
					{
						LOG_INFO(
							"Drop watch: nothing left in inventory to put back into the %s hand.",
							s_dropWatch.isLeftGameHand ? "left" : "right");
						return;
					}

					BGSEquipSlot* slot = s_dropWatch.isLeftGameHand
						? GetLeftHandSlot()
						: GetRightHandSlot();
					SilenceWeaponSoundsFor(form);
					CALL_MEMBER_FN(equipManager, EquipItem)(
						player, form, nullptr, 1, slot, false, true, false, nullptr);
					s_dropWatch.repaired = true;

					LOG_INFO(
						"Drop watch: putting %s back into the %s hand.",
						GetSafeFormName(form),
						s_dropWatch.isLeftGameHand ? "left" : "right");
				}
			}

			if (s_dropWatch.secondsLeft <= 0.0f)
			{
				s_dropWatch.secondsLeft = 0.0f;
				LOG_INFO(
					"Drop watch: ended after %.1fs - the %s hand holds %08X%s.",
					s_dropWatch.elapsed,
					s_dropWatch.isLeftGameHand ? "left" : "right",
					s_dropWatch.lastSeen,
					s_dropWatch.repaired ? " (it was put back once)" : "");
			}
		}

		SInt32 InventoryItemCount(PlayerCharacter* player, TESForm* item)
		{
			if (!player || !item)
			{
				return 0;
			}

			auto* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
			{
				return 0;
			}

			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
			(void)entryData;
			return PlayerItemCount(player, item);
		}

		// TESObjectREFR::RemoveItem - virtual, slot 86 (vtable offset 0x2B0).
		//
		// Read out of SkyrimVR 1.4.15 rather than borrowed from an SE header. The
		// game's OWN DropObject calls this exact slot with reason 3, and so do the
		// console's Drop, DropMe and RemoveMe handlers; in RemoveMe's call IDA types
		// argument six as ExtraWornLeft*, which is what pins that position down as the
		// extra-data list. Every call site found shares this nine-argument shape.
		//
		// Called through the vtable on purpose. There is no hardcoded address to go
		// stale across an address-library release, and the player's own override is
		// dispatched for free.
		const int kVtbl_RemoveItem = 86;
		const int kRemoveReason_Drop = 3;

		typedef void* (*RemoveItem_t)(
			TESObjectREFR* self,
			UInt32* outHandle,
			TESForm* item,
			SInt32 count,
			int reason,
			BaseExtraList* extraList,
			TESObjectREFR* moveToRef,
			NiPoint3* dropLoc,
			NiPoint3* rotate);

		// Drops the exact inventory instance - enchantment, temper, name and all -
		// instead of conjuring a clean copy of its base form. Returns the world
		// reference the engine created, or null if it refused.
		TESObjectREFR* DropInventoryInstance(
			PlayerCharacter* player,
			TESForm* item,
			BaseExtraList* extraList)
		{
			if (!player || !item)
			{
				return nullptr;
			}

			void** vtbl = *reinterpret_cast<void***>(static_cast<TESObjectREFR*>(player));
			if (!vtbl)
			{
				return nullptr;
			}

			UInt32 handle = *g_invalidRefHandle;
			reinterpret_cast<RemoveItem_t>(vtbl[kVtbl_RemoveItem])(
				player,
				&handle,
				item,
				1,
				kRemoveReason_Drop,
				extraList,
				nullptr,
				nullptr,
				nullptr);

			if (handle == *g_invalidRefHandle)
			{
				return nullptr;
			}

			NiPointer<TESObjectREFR> refr;
			if (!LookupREFRByHandle(handle, refr) || !refr.m_pObject)
			{
				return nullptr;
			}

			// Safe to hand back raw: the reference belongs to the cell now, exactly as
			// the PlaceAtMe result did.
			return refr.m_pObject;
		}

		// Is `candidate` still one of the item's live extra lists?
		//
		// Compares pointer VALUES while walking the inventory's own list - the
		// candidate is never dereferenced, because the whole reason to ask is that it
		// may already have been freed. Returns the candidate if it is still there and
		// null if it is not.
		BaseExtraList* FindLiveExtraList(PlayerCharacter* player, TESForm* item, BaseExtraList* candidate)
		{
			if (!player || !item || !candidate)
			{
				return nullptr;
			}

			auto* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
			{
				return nullptr;
			}

			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
			if (!entryData || !entryData->extendDataList)
			{
				return nullptr;
			}

			for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
			{
				if (it.Get() == candidate)
				{
					return candidate;
				}
			}

			return nullptr;
		}

		bool UnequipAndGrabWeapon(
			PlayerCharacter* player,
			const bool isLeftGameHand,
			const bool isLeftVRController,
			const UInt32 expectedWeaponFormID,
			const bool grabIntoHand = true,
			UInt32* outDroppedRefID = nullptr)
		{
			if (outDroppedRefID)
			{
				*outDroppedRefID = 0;
			}

			if (!player || (grabIntoHand && !higgsInterface) || expectedWeaponFormID == 0)
			{
				return false;
			}

			TESForm* item = nullptr;
			bool resolvedLeftGameHand = isLeftGameHand;
			if (!ResolveTrackedWeaponGameHand(player, expectedWeaponFormID, resolvedLeftGameHand, item))
			{
				return false;
			}

			TESForm* leftEquipped = player->GetEquippedObject(true);
			TESForm* rightEquipped = player->GetEquippedObject(false);
			const bool bothHandsSameWeapon = leftEquipped && rightEquipped &&
				leftEquipped->formID == rightEquipped->formID;
			const bool isTwoHandedDrop = IsTwoHandedWeaponForm(item);

			::EquipManager* equipManager = ::EquipManager::GetSingleton();
			if (!equipManager)
			{
				LOG_ERR("Trigger hold drop failed: game EquipManager unavailable.");
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

			BaseExtraList* equipList = resolvedLeftGameHand ? leftEquipList : rightEquipList;
			BGSEquipSlot* equipSlot = resolvedLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
			if (!equipList)
			{
				// A matched pair shares one inventory entry, and the hand the player
				// actually opened may have no worn list of its own on it. Falling back to
				// the other hand's list USED TO ALSO FLIP WHICH HAND WE WERE DROPPING -
				// so opening the left hand unequipped, spawned and removed the weapon in
				// the RIGHT one. Take whatever list exists, but never move the drop to a
				// hand the player did not open.
				if (bothHandsSameWeapon)
				{
					equipList = rightEquipList ? rightEquipList : leftEquipList;
					LOG_INFO(
						"Trigger hold drop: the %s hand has no worn entry for %s though both "
						"hands hold one - using the other list but keeping the drop on the "
						"hand you opened.",
						resolvedLeftGameHand ? "left" : "right",
						GetSafeFormName(item));
				}
				else if (rightEquipList)
				{
					equipList = rightEquipList;
					equipSlot = GetRightHandSlot();
					resolvedLeftGameHand = false;
				}
				else if (leftEquipList)
				{
					equipList = leftEquipList;
					equipSlot = GetLeftHandSlot();
					resolvedLeftGameHand = true;
				}
			}

			if (!equipList || !equipSlot)
			{
				LOG_ERR("Trigger hold drop failed: could not resolve equip data for formId=%08X", item->formID);
				return false;
			}

			BSExtraData* xCannotWear = equipList->GetByType(kExtraData_CannotWear);
			if (xCannotWear)
			{
				equipList->Remove(kExtraData_CannotWear, xCannotWear);
			}

			// Covers the unequip, the world model being spawned and the inventory
			// removal — all three can reach for the form's sounds, and the engine
			// finishes some of them a frame or two later.
			SilenceWeaponSoundsFor(item);

			CALL_MEMBER_FN(equipManager, UnequipItem)(
				player, item, equipList, 1, equipSlot, false, true, true, false, nullptr);

			const NiPoint3 spawnPos = GetHandSpawnPosition(player, resolvedLeftGameHand);

			// Drop the instance the player owns, not a fresh copy of its form.
			//
			// This was PlaceAtMe followed by RemoveItem, and that pair was quietly
			// destroying people's weapons. PlaceAtMe takes a base FORM and nothing else
			// - there is no parameter through which an enchantment or a temper level
			// could travel - so what hit the floor was factory-fresh, and the RemoveItem
			// behind it deleted the real one. Tempering, player-applied enchantments,
			// custom names and poisons all live on the instance's extra list and all
			// went with it. Reported as "drop it, pick it up, it loses its quality", and
			// true since the first commit.
			//
			// Passing equipList also closes a second hole nobody had reported: the old
			// RemoveItem removed BY FORM, so owning a plain and a tempered copy of one
			// weapon meant a drop could destroy the wrong one.
			//
			// But equipList was read BEFORE the unequip above, and must not be trusted
			// after it. Unequipping takes ExtraWorn off that list; when that was all the
			// list held - a plain weapon, no enchantment, no temper, no name - the item
			// folds back into the entry's plain count and the list is destroyed, leaving
			// equipList pointing at freed memory. 2.0.3 passed that pointer straight to
			// RemoveItem, which walks it. A use-after-free on every plain-weapon drop:
			// harmless while the freed block still held its old bytes, fatal once the
			// allocator gave it to something else. Reported as a crash inside
			// BSExtraDataList::GetCountOfExtraDataEntries after dropping a plain bow
			// over and over, on a list whose "next" pointer was the text "mod". Tempered
			// and enchanted weapons never tripped it - their lists survive the unequip
			// - which is exactly the half that got tested.
			//
			// So ask the inventory again. Still there: it survived because it carries
			// instance data worth keeping, so pass it and the drop preserves it. Gone:
			// there was nothing on it to preserve, and null is exactly right.
			BaseExtraList* const liveList = FindLiveExtraList(player, item, equipList);
			if (equipList && !liveList)
			{
				LOG_INFO(
					"Drop: %s's extra list went with the unequip (nothing on it to keep) - "
					"dropping it as a plain item.",
					GetSafeFormName(item));
			}

			TESObjectREFR* droppedWeapon = DropInventoryInstance(player, item, liveList);
			const bool droppedRealInstance = droppedWeapon != nullptr;

			if (!droppedWeapon)
			{
				// Never leave a hand empty because the engine refused. The old path is
				// wrong about extra data but it does put a weapon on the floor, and a
				// weapon that lost its temper beats a weapon that vanished.
				LOG_ERR(
					"Drop: the engine would not drop %s as an instance - falling back to a "
					"base-form copy, which WILL lose enchantment and temper.",
					GetSafeFormName(item));
				droppedWeapon = PlaceAtMe_Native(nullptr, 0, player, item, 1, false, false);
			}

			if (!droppedWeapon)
			{
				LOG_ERR("Trigger hold drop failed: nothing could be dropped for formId=%08X", item->formID);
				return false;
			}

			droppedWeapon->pos = spawnPos;
			NiNode* weaponNode = droppedWeapon->GetNiNode();
			if (weaponNode)
			{
				weaponNode->m_worldTransform.pos = spawnPos;
			}

			SetOwnerToPlayer(droppedWeapon);

			// The same form showing in both hand slots happens two opposite ways, and
			// the inventory count is what tells them apart.
			//
			// ONE item filling both slots (bows, crossbows, two-handers, and the VR
			// edge case this guard was written for): removing it would delete the copy
			// still in hand, so the removal is skipped.
			//
			// TWO real copies, i.e. dual wielding: the player owns a stack, and the
			// dropped copy MUST come out of inventory. Skipping it there spawns a world
			// model while leaving the stack whole, which duplicates the weapon on every
			// drop and leaves the engine an endless supply to re-equip into the hand
			// the player just emptied.
			const SInt32 ownedCount = InventoryItemCount(player, item);

			// A real drop IS the removal. The engine took the item out of the inventory
			// as part of putting it on the floor, so there is no second step here to get
			// wrong - and no window in which the player owns both the world model and
			// the inventory copy, which is the whole thing dropRemoveDelaySeconds was
			// written to manage. That setting still governs the fallback path below.
			if (droppedRealInstance)
			{
				LOG_INFO(
					"Drop inventory: %s left as the instance the player owned, extra data intact "
					"(remaining=%d, bothHands=%s).",
					GetSafeFormName(item),
					ownedCount,
					bothHandsSameWeapon ? "yes" : "no");

				// Same watch as below, one lower: the count has already been decremented
				// by the drop, so "owned 2 before dropping" reads as 1 by the time we ask.
				if (bothHandsSameWeapon && ownedCount >= 1)
				{
					StartDropWatch(player, item->formID, !resolvedLeftGameHand);
				}
			}
			else
			{
				const bool singleItemFillsBothSlots = bothHandsSameWeapon && ownedCount < 2;
				const bool removeFromInventory = !singleItemFillsBothSlots || isTwoHandedDrop ||
					IsBowWeaponForm(item) || IsCrossbowWeaponForm(item);
				if (removeFromInventory)
				{
					// Matched pair only. Every other drop removes immediately, exactly as
					// before - there is no reason to put a delay in a path that works.
					if (bothHandsSameWeapon && ownedCount >= 2 && dropRemoveDelaySeconds > 0.0f)
					{
						FlushPendingRemoval(player);
						s_pendingRemoval.formID = item->formID;
						s_pendingRemoval.secondsLeft = dropRemoveDelaySeconds;
						LOG_INFO(
							"Drop inventory: holding the removal of %s for %.2fs so the unequip can "
							"settle first.",
							GetSafeFormName(item),
							dropRemoveDelaySeconds);
					}
					else
					{
						RemoveItemFromInventory(player, item, 1, true);
					}

					// Watch the hand the player did NOT open. The cause of it losing its
					// weapon is still unknown - two theories have been tried and both were
					// wrong - so this reports what happens rather than assuming it.
					if (bothHandsSameWeapon && ownedCount >= 2)
					{
						StartDropWatch(player, item->formID, !resolvedLeftGameHand);
					}
				}
				LOG_INFO(
					"Drop inventory (fallback copy): %s owned=%d bothHands=%s -> %s",
					GetSafeFormName(item),
					ownedCount,
					bothHandsSameWeapon ? "yes" : "no",
					removeFromInventory ? "removed 1" : "kept (one item in both slots)");
			}

			if (outDroppedRefID)
			{
				*outDroppedRefID = droppedWeapon->formID;
			}

			if (grabIntoHand)
			{
				MarkTriggerHoldDroppedGrab(isLeftVRController, droppedWeapon->formID);
				higgsInterface->GrabObject(droppedWeapon, isLeftVRController);

				LOG_INFO(
					"Trigger hold drop [%s hand]: unequipped, removed from inventory, grabbed world model %s formId=%08X refId=%08X",
					resolvedLeftGameHand ? "left" : "right",
					GetSafeFormName(item),
					item->formID,
					droppedWeapon->formID);
			}
			else
			{
				LOG_INFO(
					"Grip release drop [%s hand]: unequipped, released world model %s formId=%08X refId=%08X",
					resolvedLeftGameHand ? "left" : "right",
					GetSafeFormName(item),
					item->formID,
					droppedWeapon->formID);
			}

			// Timed from here until the engine gives the dropped weapon its model, and moved up
			// the engine's loader if that runs late. DropModelWatch.h has the why.
			DropModelWatchBegin(droppedWeapon, item);
			HiggsHandWatchBegin(isLeftVRController, "after dropping", item);
			HiggsReleaseNoteDrop(droppedWeapon, item);

			// Announced for EVERY drop, not only trigger-hold ones.
			//
			// This event is how other mods learn that a weapon left the player's hand.
			// The API was built partly for Weapon Unlocked VR's proxy system, and a mod
			// that swaps a real weapon for a stand-in has to undo that swap when the
			// stand-in leaves the hand. The announcement used to sit inside the
			// grabIntoHand branch: splitting this drop out for grip-hold (9a34c33) put
			// the grip-release drop on the other side of it, so from 2.0.0 on the
			// default drop told nobody. Suspected - not confirmed - cause of a reported
			// greatsword duplicate with Shields and 2H Weapons Unlocked.
			//
			// The event keeps its trigger-hold name because mods already compiled
			// against it register for exactly this callback. The one difference a
			// listener can see: after a grip release the world model is loose on the
			// floor, not held in a HIGGS hand. Trigger-hold drops announce at the same
			// point as before - after the grab and the log.
			SwapDropAndHoldReduxAPI::WeaponHandEvent event{};
			event.eventType = SwapDropAndHoldReduxAPI::kTriggerHoldDropped;
			event.isLeftGameHand = resolvedLeftGameHand;
			event.isLeftVRController = isLeftVRController;
			event.sourceIsLeftGameHand = resolvedLeftGameHand;
			event.weaponFormID = item->formID;
			event.weaponRefID = droppedWeapon->formID;
			SwapDropAndHoldReduxAPI::NotifyTriggerHoldDropped(event);

			return true;
		}

		class TriggerHoldDropTask : public TaskDelegate
		{
		public:
			TriggerHoldDropTask(
				const bool isLeftGameHand,
				const bool isLeftVRController,
				const UInt32 weaponFormID)
				: m_isLeftGameHand(isLeftGameHand)
				, m_isLeftVRController(isLeftVRController)
				, m_weaponFormID(weaponFormID)
			{
			}

			virtual void Run() override
			{
				TESForm* weaponForm = LookupFormByID(m_weaponFormID);
				if (IsDropBlockedForHand(m_isLeftVRController, weaponForm))
				{
					return;
				}

				PlayerCharacter* player = *g_thePlayer;
				if (!player)
				{
					return;
				}

				UnequipAndGrabWeapon(player, m_isLeftGameHand, m_isLeftVRController, m_weaponFormID);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftGameHand;
			bool m_isLeftVRController;
			UInt32 m_weaponFormID;
		};

		void QueueTriggerHoldDrop(
			const bool isLeftGameHand,
			const bool isLeftVRController,
			const UInt32 weaponFormID)
		{
			if (!g_task || weaponFormID == 0)
			{
				return;
			}

			TESForm* weaponForm = LookupFormByID(weaponFormID);
			if (IsDropBlockedForHand(isLeftVRController, weaponForm))
			{
				return;
			}

			g_task->AddTask(new TriggerHoldDropTask(isLeftGameHand, isLeftVRController, weaponFormID));
		}

		bool HasTwoHandedWeaponEquipped(PlayerCharacter* player)
		{
			if (!player)
			{
				return false;
			}

			return IsTwoHandedWeaponForm(player->GetEquippedObject(true)) ||
				IsTwoHandedWeaponForm(player->GetEquippedObject(false));
		}

		void UpdateTapThenHoldDropState(
			HandTriggerState& state,
			TESForm* equipped,
			const bool isLeftGameHand,
			const bool isLeftVRController,
			const bool triggerPressed,
			const bool gripActive,
			const bool dropButtonPressed,
			const float deltaTime)
		{
			bool primaryPressed = false;
			bool secondaryActive = false;
			ResolveDropButtons(triggerPressed, gripActive, dropButtonPressed, primaryPressed, secondaryActive);

			if (!primaryPressed)
			{
				state.secondaryHeldBeforePrimary = secondaryActive;

				if (state.wasPressed && state.phase == HandTriggerState::kTapPressing)
				{
					if (state.tapPressTime > 0.0f && state.tapPressTime <= kMaxTapSeconds)
					{
						state.tapsCompleted += 1;
						state.phase = HandTriggerState::kTapArmed;
						// More taps still needed (bow/crossbow double tap): allow only a
						// short gap. Final tap done: normal window for the hold press.
						state.tapArmedRemaining = (state.tapsCompleted >= RequiredTapCount(equipped))
							? kTapArmWindowSeconds
							: kNextTapWindowSeconds;
					}
					else
					{
						// Long first press (power attack / staff fire / long grip) — not a tap.
						ResetTriggerDropProgress(state);
					}
				}
				else if (state.phase == HandTriggerState::kHoldPressing)
				{
					// Released early during drop hold — require a fresh tap.
					ResetTriggerDropProgress(state);
				}

				if (state.phase == HandTriggerState::kTapArmed)
				{
					state.tapArmedRemaining -= deltaTime;
					if (state.tapArmedRemaining <= 0.0f)
					{
						ResetTriggerDropProgress(state);
					}
				}

				state.holdTime = 0.0f;
				state.actionedThisHold = false;
				state.wasPressed = false;
				state.wasSecondaryActive = secondaryActive;
				return;
			}

			if (!equipped)
			{
				ResetTriggerDropProgress(state);
				state.wasPressed = primaryPressed;
				state.wasSecondaryActive = secondaryActive;
				return;
			}

			if (IsSpellWheelOpenNow())
			{
				ResetTriggerDropProgress(state);
				state.wasPressed = primaryPressed;
				state.wasSecondaryActive = secondaryActive;
				return;
			}

			if (state.phase == HandTriggerState::kIdle ||
				state.phase == HandTriggerState::kTapPressing)
			{
				if (!state.wasPressed)
				{
					state.phase = HandTriggerState::kTapPressing;
					state.tapPressTime = 0.0f;
					state.holdTime = 0.0f;
					state.actionedThisHold = false;
					if (!IsTwoHandedWeaponForm(equipped) &&
						(state.secondaryHeldBeforePrimary || secondaryActive))
					{
						state.spellWheelBlocked = true;
					}
				}

				state.tapPressTime += deltaTime;
				state.wasPressed = true;
				state.wasSecondaryActive = secondaryActive;
				return;
			}

			if (state.phase == HandTriggerState::kTapArmed)
			{
				if (state.tapsCompleted < RequiredTapCount(equipped))
				{
					// Not enough rapid taps yet (bow/crossbow double tap) —
					// treat this press as the next tap, not the hold.
					state.phase = HandTriggerState::kTapPressing;
					state.tapPressTime = deltaTime;
					state.holdTime = 0.0f;
					state.actionedThisHold = false;
					if (!IsTwoHandedWeaponForm(equipped) &&
						(state.secondaryHeldBeforePrimary || secondaryActive))
					{
						state.spellWheelBlocked = true;
					}
					state.wasPressed = true;
					state.wasSecondaryActive = secondaryActive;
					return;
				}

				state.phase = HandTriggerState::kHoldPressing;
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
				state.spellWheelBlocked = false;
				if (!IsTwoHandedWeaponForm(equipped) &&
					(state.secondaryHeldBeforePrimary || secondaryActive))
				{
					state.spellWheelBlocked = true;
				}
			}

			if (secondaryActive && !state.wasSecondaryActive && state.wasPressed && state.secondaryHeldBeforePrimary)
			{
				if (!IsTwoHandedWeaponForm(equipped))
				{
					state.spellWheelBlocked = true;
				}
			}

			if (secondaryActive && !state.wasSecondaryActive && state.wasPressed && !state.secondaryHeldBeforePrimary)
			{
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
			}

			const bool blockDrop = state.spellWheelBlocked ||
				IsSecondaryBlockingDrop(equipped, primaryPressed, secondaryActive);
			if (blockDrop)
			{
				state.holdTime = 0.0f;
			}
			else if (state.phase == HandTriggerState::kHoldPressing)
			{
				state.holdTime += deltaTime;

				if (!state.actionedThisHold && state.holdTime >= triggerHoldDropSeconds)
				{
					QueueTriggerHoldDrop(isLeftGameHand, isLeftVRController, equipped->formID);
					state.actionedThisHold = true;
					ResetTriggerDropProgress(state);
				}
			}

			state.wasPressed = primaryPressed;
			state.wasSecondaryActive = secondaryActive;
		}

		bool UpdateTwoHandedTriggerHoldState(PlayerCharacter* player, const float deltaTime)
		{
			TESForm* leftEquipped = player->GetEquippedObject(true);
			TESForm* rightEquipped = player->GetEquippedObject(false);
			TESForm* weapon = nullptr;
			bool isLeftGameHand = false;

			if (IsTwoHandedWeaponForm(leftEquipped))
			{
				weapon = leftEquipped;
				isLeftGameHand = true;
			}
			else if (IsTwoHandedWeaponForm(rightEquipped))
			{
				weapon = rightEquipped;
				isLeftGameHand = false;
			}
			else
			{
				return false;
			}

			const bool isLeftVRController = GameHandToVRController(isLeftGameHand);

			bool triggerPressed = false;
			bool gripActive = false;
			bool dropButtonPressed = false;
			ReadControllerInput(isLeftVRController, triggerPressed, gripActive, dropButtonPressed);

			UpdateTapThenHoldDropState(
				s_twoHandSharedTriggerState,
				weapon,
				isLeftGameHand,
				isLeftVRController,
				triggerPressed,
				gripActive,
				dropButtonPressed,
				deltaTime);
			return true;
		}

		void UpdateHandTriggerState(const bool isLeftVRController, HandTriggerState& state, const float deltaTime)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				ResetTriggerDropProgress(state);
				state.wasPressed = false;
				state.wasSecondaryActive = false;
				state.secondaryHeldBeforePrimary = false;
				return;
			}

			if (HasTwoHandedWeaponEquipped(player))
			{
				return;
			}

			const TrackedWeaponHandInfo tracked = GetTrackedWeaponForVRController(player, isLeftVRController);
			TESForm* equipped = tracked.weapon;

			if (equipped && !IsDropAllowedFromController(equipped, isLeftVRController))
			{
				equipped = nullptr;
			}

			bool triggerPressed = false;
			bool gripActive = false;
			bool dropButtonPressed = false;
			ReadControllerInput(isLeftVRController, triggerPressed, gripActive, dropButtonPressed);

			UpdateTapThenHoldDropState(
				state,
				equipped,
				tracked.isLeftGameHand,
				isLeftVRController,
				triggerPressed,
				gripActive,
				dropButtonPressed,
				deltaTime);
		}

		void OnPrePhysicsStep(void* /*world*/)
		{
			if (IsInputCapturingMenuOpen())
			{
				return;
			}

			static auto lastTime = std::chrono::high_resolution_clock::now();
			const auto currentTime = std::chrono::high_resolution_clock::now();
			float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
			lastTime = currentTime;

			if (deltaTime > 0.1f)
			{
				deltaTime = 0.1f;
			}
			if (deltaTime < 0.0001f)
			{
				deltaTime = 0.0001f;
			}

			// GripHold mode replaces the tap-then-hold drop gesture with grip release;
			// the drop guards still tick so grab suppression keeps working.
			if (!gripHoldMode)
			{
				PlayerCharacter* player = *g_thePlayer;
				if (player && !UpdateTwoHandedTriggerHoldState(player, deltaTime))
				{
					UpdateHandTriggerState(true, s_leftTriggerState, deltaTime);
					UpdateHandTriggerState(false, s_rightTriggerState, deltaTime);
				}
			}

			UpdateDropGuardTimer(s_leftDropGuard, deltaTime);
			UpdateDropGuardTimer(s_rightDropGuard, deltaTime);
			UpdatePendingRemoval(deltaTime);
			UpdateDropWatch(deltaTime);
			DropModelWatchTick(deltaTime);
			DrawWatchTick(deltaTime);
			HiggsHandWatchTick(deltaTime);
			HiggsReleaseTick(deltaTime);
			VrikHolsterEventsTick(deltaTime);
		}
	}

	// GripHold: unequip the tracked weapon and spawn its world model at the hand.
	// grabIntoHand=true is the legacy trigger-hold behaviour (model handed to HIGGS);
	// grabIntoHand=false is the grip-release drop (hand is open, ThrowMomentum or the
	// embed pin takes the spawned ref, reported via outDroppedRefID).
	bool DropEquippedWeaponFromHand(
		const bool isLeftGameHand,
		const bool isLeftVRController,
		const UInt32 weaponFormID,
		const bool grabIntoHand,
		UInt32* outDroppedRefID)
	{
		PlayerCharacter* player = *g_thePlayer;
		return UnequipAndGrabWeapon(
			player, isLeftGameHand, isLeftVRController, weaponFormID, grabIntoHand, outDroppedRefID);
	}

	void RegisterTriggerHoldLogger()
	{
		if (s_registered)
		{
			return;
		}

		if (!higgsInterface)
		{
			LOG_ERR("Trigger hold drop requires HIGGS.");
			return;
		}

		higgsInterface->AddPrePhysicsStepCallback(OnPrePhysicsStep);
		higgsInterface->AddDroppedCallback(OnTriggerHoldWeaponDropped);
		s_registered = true;
		LOG_INFO(
			"Trigger hold drop registered (tap then hold %.2fs on %s from ini, spell wheel orb block drop%s, off-hand bow drop, main-hand crossbow drop, optional off-hand torch drop, bows/crossbows need rapid double tap then hold).",
			triggerHoldDropSeconds,
			dropButtonName,
			enableTwoHandedWeapons ? ", 2H weapons enabled" : "");
		DropModelWatchReportArming();
		DrawWatchReportArming();
		HiggsHandWatchRegister();
	}

	bool ShouldSuppressGrabAutoEquip(const bool isLeftVRController, TESObjectREFR* grabbedRefr)
	{
		if (!grabbedRefr)
		{
			return false;
		}

		if (GripHoldShouldSuppressGrab(isLeftVRController, grabbedRefr))
		{
			return true;
		}

		// A weapon still buried in someone is not a pickup. HIGGS can still take hold
		// of the world model, and without this the normal grab-to-equip pipeline
		// consumed it a frame after the extraction drag began — the blade teleported
		// into the hand instead of being worked out of the wound.
		if (EmbedIsRefPinned(grabbedRefr->formID))
		{
			LOG_INFO(
				"Grab ignored: refId=%08X is still lodged in a body — drag it out instead",
				grabbedRefr->formID);
			return true;
		}

		const HandDropGuardState& guard = GetDropGuard(isLeftVRController);
		return guard.droppedRefFormID != 0 && guard.droppedRefFormID == grabbedRefr->formID;
	}
}
