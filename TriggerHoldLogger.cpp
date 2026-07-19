#include "TriggerHoldLogger.h"

#include "Engine.h"
#include "Helper.h"
#include "TrackedWeapons.h"
#include "config.h"

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
		static RelocPtr<bool> s_leftHandedMode(0x01E71778);

		struct HandTriggerState
		{
			float holdTime = 0.0f;
			bool actionedThisHold = false;
			bool wasPressed = false;
			bool wasGripActive = false;
			bool gripHeldBeforeTrigger = false;
			bool spellWheelBlocked = false;
		};

		static HandTriggerState s_leftTriggerState;
		static HandTriggerState s_rightTriggerState;
		static bool s_registered = false;

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

		bool VRControllerToGameHand(const bool isLeftVRController)
		{
			if (s_leftHandedMode && *s_leftHandedMode)
			{
				return !isLeftVRController;
			}

			return isLeftVRController;
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
			return ((state.ulButtonPressed & GRIP_BUTTON_MASK) != 0) ||
				((state.ulButtonTouched & GRIP_BUTTON_MASK) != 0);
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
			bool& gripActive)
		{
			vr_1_0_12::VRControllerState_t controllerState{};
			if (!GetControllerState(isLeftVRController, controllerState))
			{
				triggerPressed = false;
				gripActive = false;
				return false;
			}

			triggerPressed = IsTriggerActive(controllerState);
			gripActive = IsGripActive(controllerState);
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

		bool IsDropBlockedForHand(const bool isLeftVRController)
		{
			if (IsSpellWheelOpenNow())
			{
				return true;
			}

			const HandTriggerState& state = GetHandTriggerState(isLeftVRController);
			if (state.spellWheelBlocked)
			{
				return true;
			}

			bool triggerPressed = false;
			bool gripActive = false;
			if (!ReadControllerInput(isLeftVRController, triggerPressed, gripActive))
			{
				return false;
			}

			return triggerPressed && gripActive;
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

		bool UnequipAndGrabWeapon(PlayerCharacter* player, const bool isLeftGameHand, const UInt32 expectedWeaponFormID)
		{
			if (!player || !higgsInterface || expectedWeaponFormID == 0)
			{
				return false;
			}

			TESForm* leftEquipped = player->GetEquippedObject(true);
			TESForm* rightEquipped = player->GetEquippedObject(false);
			TESForm* item = isLeftGameHand ? leftEquipped : rightEquipped;

			if (!item || item->formID != expectedWeaponFormID || !IsTrackedWeaponForm(item))
			{
				return false;
			}

			const bool bothHandsSameWeapon = leftEquipped && rightEquipped &&
				leftEquipped->formID == rightEquipped->formID;

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

			BaseExtraList* equipList = isLeftGameHand ? leftEquipList : rightEquipList;
			BGSEquipSlot* equipSlot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
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

			const NiPoint3 spawnPos = GetHandSpawnPosition(player, isLeftGameHand);
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

			if (!bothHandsSameWeapon)
			{
				RemoveItemFromInventory(player, item, 1, true);
			}

			const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
			MarkTriggerHoldDroppedGrab(isLeftVRController, droppedWeapon->formID);
			higgsInterface->GrabObject(droppedWeapon, isLeftVRController);

			LOG_INFO(
				"Trigger hold drop [%s hand]: unequipped, removed from inventory, grabbed world model %s formId=%08X refId=%08X",
				isLeftGameHand ? "left" : "right",
				GetSafeFormName(item),
				item->formID,
				droppedWeapon->formID);

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
				if (IsDropBlockedForHand(m_isLeftVRController))
				{
					return;
				}

				PlayerCharacter* player = *g_thePlayer;
				if (!player)
				{
					return;
				}

				UnequipAndGrabWeapon(player, m_isLeftGameHand, m_weaponFormID);
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

		void QueueTriggerHoldDrop(const bool isLeftGameHand, const UInt32 weaponFormID)
		{
			if (!g_task || weaponFormID == 0)
			{
				return;
			}

			const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
			if (IsDropBlockedForHand(isLeftVRController))
			{
				return;
			}

			g_task->AddTask(new TriggerHoldDropTask(isLeftGameHand, isLeftVRController, weaponFormID));
		}

		void UpdateHandTriggerState(const bool isLeftVRController, HandTriggerState& state, const float deltaTime)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
				state.wasPressed = false;
				state.wasGripActive = false;
				state.gripHeldBeforeTrigger = false;
				state.spellWheelBlocked = false;
				return;
			}

			const bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);
			TESForm* equipped = player->GetEquippedObject(isLeftGameHand);

			bool triggerPressed = false;
			bool gripActive = false;
			ReadControllerInput(isLeftVRController, triggerPressed, gripActive);

			if (!triggerPressed)
			{
				state.gripHeldBeforeTrigger = gripActive;
			}

			if (!IsTrackedWeaponForm(equipped))
			{
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
				state.spellWheelBlocked = false;
				state.wasPressed = triggerPressed;
				state.wasGripActive = gripActive;
				return;
			}

			const UInt32 weaponFormID = equipped->formID;

			if (!triggerPressed)
			{
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
				state.spellWheelBlocked = false;
				state.wasPressed = false;
				state.wasGripActive = gripActive;
				return;
			}

			if (IsSpellWheelOpenNow())
			{
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
				state.wasPressed = triggerPressed;
				state.wasGripActive = gripActive;
				return;
			}

			if (!state.wasPressed)
			{
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
				if (state.gripHeldBeforeTrigger || gripActive)
				{
					state.spellWheelBlocked = true;
				}
			}

			if (gripActive && !state.wasGripActive && state.wasPressed && state.gripHeldBeforeTrigger)
			{
				state.spellWheelBlocked = true;
			}

			if (gripActive && !state.wasGripActive && state.wasPressed && !state.gripHeldBeforeTrigger)
			{
				state.holdTime = 0.0f;
				state.actionedThisHold = false;
			}

			const bool blockDrop = state.spellWheelBlocked || (gripActive && triggerPressed);
			if (blockDrop)
			{
				state.holdTime = 0.0f;
			}
			else
			{
				state.holdTime += deltaTime;

				if (!state.actionedThisHold && state.holdTime >= triggerHoldDropSeconds)
				{
					QueueTriggerHoldDrop(isLeftGameHand, weaponFormID);
					state.actionedThisHold = true;
				}
			}

			state.wasPressed = triggerPressed;
			state.wasGripActive = gripActive;
		}

		void OnPrePhysicsStep(void* /*world*/)
		{
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

			UpdateHandTriggerState(true, s_leftTriggerState, deltaTime);
			UpdateHandTriggerState(false, s_rightTriggerState, deltaTime);

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
			"Trigger hold drop registered (%.2fs hold from ini, grip/spell wheel orb block drop).",
			triggerHoldDropSeconds);
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
