#include "TriggerHoldLogger.h"

#include "Engine.h"
#include "Helper.h"
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
		static const uint64_t GRIP_BUTTON_MASK = (1ull << 2);

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

		bool UnequipAndGrabWeapon(
			PlayerCharacter* player,
			const bool isLeftGameHand,
			const bool isLeftVRController,
			const UInt32 expectedWeaponFormID)
		{
			if (!player || !higgsInterface || expectedWeaponFormID == 0)
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
				if (rightEquipList)
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

			CALL_MEMBER_FN(equipManager, UnequipItem)(
				player, item, equipList, 1, equipSlot, false, true, true, false, nullptr);

			const NiPoint3 spawnPos = GetHandSpawnPosition(player, resolvedLeftGameHand);
			TESObjectREFR* droppedWeapon = PlaceAtMe_Native(nullptr, 0, player, item, 1, false, false);
			if (!droppedWeapon)
			{
				LOG_ERR("Trigger hold drop failed: PlaceAtMe returned null for formId=%08X", item->formID);
				return false;
			}

			droppedWeapon->pos = spawnPos;
			NiNode* weaponNode = droppedWeapon->GetNiNode();
			if (weaponNode)
			{
				weaponNode->m_worldTransform.pos = spawnPos;
			}

			SetOwnerToPlayer(droppedWeapon);

			// Bows and crossbows (like 2H weapons) occupy both hand slots as a single
			// item, so bothHandsSameWeapon does not mean two copies exist — the dropped
			// copy must still be removed or a phantom stays in inventory and later
			// re-grabs spawn world duplicates.
			if (!bothHandsSameWeapon || isTwoHandedDrop || IsBowWeaponForm(item) || IsCrossbowWeaponForm(item))
			{
				RemoveItemFromInventory(player, item, 1, true);
			}

			MarkTriggerHoldDroppedGrab(isLeftVRController, droppedWeapon->formID);
			higgsInterface->GrabObject(droppedWeapon, isLeftVRController);

			LOG_INFO(
				"Trigger hold drop [%s hand]: unequipped, removed from inventory, grabbed world model %s formId=%08X refId=%08X",
				resolvedLeftGameHand ? "left" : "right",
				GetSafeFormName(item),
				item->formID,
				droppedWeapon->formID);

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

			PlayerCharacter* player = *g_thePlayer;
			if (player && !UpdateTwoHandedTriggerHoldState(player, deltaTime))
			{
				UpdateHandTriggerState(true, s_leftTriggerState, deltaTime);
				UpdateHandTriggerState(false, s_rightTriggerState, deltaTime);
			}

			UpdateDropGuardTimer(s_leftDropGuard, deltaTime);
			UpdateDropGuardTimer(s_rightDropGuard, deltaTime);
		}
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
	}

	bool ShouldSuppressGrabAutoEquip(const bool isLeftVRController, TESObjectREFR* grabbedRefr)
	{
		if (!grabbedRefr)
		{
			return false;
		}

		const HandDropGuardState& guard = GetDropGuard(isLeftVRController);
		return guard.droppedRefFormID != 0 && guard.droppedRefFormID == grabbedRefr->formID;
	}
}
