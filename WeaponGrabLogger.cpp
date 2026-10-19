#include "WeaponGrabLogger.h"
#include "WeaponGrabActivator.h"
#include "TriggerHoldLogger.h"
#include "TrackedWeapons.h"

#include "Engine.h"
#include "GrabDelayRules.h"
#include "GrabPose.h"
#include "Helper.h"
#include "config.h"

#include <skse64/GameObjects.h>
#include <skse64_common/Relocation.h>

#include <chrono>

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

		// ---- fGrabEquipDelay -------------------------------------------------------------------
		//
		// A weapon grabbed with HIGGS can stay the loose physics object it is - carried, turned over,
		// tossed - until it has been held on purpose for fGrabEquipDelay seconds, and only then be
		// picked up and equipped. Let go sooner and it is never picked up. Zero, the default, is the
		// behaviour from before the setting: every grab below goes straight to the pickup.
		// GrabDelayRules.h holds the rules and their tests.
		GrabDelay::Hold s_grabHolds[2]; // [0] = right VR controller, [1] = left

		// M3.0.0-test15: would taking this be a crime - the red "Steal" prompt? The engine's own
		// TESObjectREFR::IsCrimeToActivate: address-library id 19400, VR offset 0x2ABA40 in both the installed
		// version-1-4-15-0.csv and the decompilation's database (confidence 3). CommonLib's IsOffLimits() is this.
		typedef bool (*_TESObjectREFR_IsCrimeToActivate)(TESObjectREFR* refr);
		RelocAddr<_TESObjectREFR_IsCrimeToActivate> IsCrimeToActivate(0x2ABA40);

		void ScheduleOrDelayGrabbedWeaponActivation(const bool isLeft, TESObjectREFR* grabbedRefr)
		{
			// A weapon that is not yours stays a loose object for the stolen wait first (test15), so a shop's
			// wares can be picked up and looked at; the longer of that and "Hold before equipping" wins.
			const bool offLimits = grabbedRefr && IsCrimeToActivate(grabbedRefr);
			const float delay = GrabDelay::DelayFor(grabEquipDelay, grabEquipDelayStolen, offLimits);
			if (!GrabDelay::Waits(delay) || !grabbedRefr)
			{
				ScheduleGrabbedWeaponActivation(isLeft, grabbedRefr);
				return;
			}

			GrabDelay::Hold& hold = s_grabHolds[isLeft ? 1 : 0];
			const bool repeat = hold.Active() && hold.RefID() == grabbedRefr->formID;
			hold.Begin(grabbedRefr->formID, offLimits);
			if (!repeat)
			{
				LOG_INFO(
					"Grab delay: the %s hand holds %s refId=%08X - it stays a loose object until it has "
					"been held %.2fs%s.",
					isLeft ? "left" : "right",
					GetSafeFormName(grabbedRefr->baseForm),
					grabbedRefr->formID,
					delay,
					offLimits ? " (it is not yours - taking it would be stealing)" : "");
			}
		}

		void OnGrabDelayStep(void* /*world*/)
		{
			static auto s_lastStep = std::chrono::high_resolution_clock::now();
			const auto now = std::chrono::high_resolution_clock::now();
			float deltaTime = std::chrono::duration<float>(now - s_lastStep).count();
			s_lastStep = now;

			// A load screen or a hitch is not time spent holding a weapon.
			if (deltaTime > 0.25f)
			{
				deltaTime = 0.25f;
			}

			if (!s_grabHolds[0].Active() && !s_grabHolds[1].Active())
			{
				return;
			}

			// The clock stops while a menu has the controllers.
			if (!higgsInterface || IsInputCapturingMenuOpen())
			{
				return;
			}

			for (int hand = 0; hand < 2; ++hand)
			{
				GrabDelay::Hold& hold = s_grabHolds[hand];
				if (!hold.Active())
				{
					continue;
				}

				const bool isLeft = hand == 1;
				TESObjectREFR* const held = higgsInterface->GetGrabbedObject(isLeft);
				const UInt32 heldRefID = held ? held->formID : 0;
				const float heldBefore = hold.HeldSeconds();
				// Read every step, per hold: a slider moved mid-hold takes effect at once.
				const float delay = GrabDelay::DelayFor(grabEquipDelay, grabEquipDelayStolen, hold.OffLimits());

				switch (hold.Update(heldRefID, deltaTime, delay))
				{
				case GrabDelay::Step::kEquip:
					LOG_INFO(
						"Grab delay: the %s hand held %s for %.2fs - picking it up now%s.",
						isLeft ? "left" : "right",
						GetSafeFormName(held->baseForm),
						hold.HeldSeconds(),
						hold.OffLimits() ? " (it was not yours: taking it is stealing)" : "");
					ScheduleGrabbedWeaponActivation(isLeft, held);
					break;
				case GrabDelay::Step::kAbandoned:
					LOG_INFO(
						"Grab delay: the %s hand let go of refId=%08X after %.2fs of %.2fs - it stays a loose "
						"object and was not picked up.",
						isLeft ? "left" : "right",
						hold.RefID(),
						heldBefore,
						delay);
					break;
				default:
					break;
				}
			}
		}

		void OnWeaponGrabbed(const bool isLeft, TESObjectREFR* grabbedRefr)
		{
			if (!grabbedRefr || !grabbedRefr->baseForm)
			{
				return;
			}

			TESForm* itemForm = grabbedRefr->baseForm;

			// 3.0.0: measure where, and which way round, the hand took hold of it - NOW. This callback
			// is the only moment the weapon is still where the player reached for it (GrabPoseRules.h).
			GrabPoseOnGrabbed(isLeft, grabbedRefr);

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
						"Bow grabbed [off-hand]: %s formId=%08X (auto-equip suppressed)",
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

				ScheduleOrDelayGrabbedWeaponActivation(isLeft, grabbedRefr);
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
						"Crossbow grabbed [main hand]: %s formId=%08X (auto-equip suppressed)",
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

				ScheduleOrDelayGrabbedWeaponActivation(isLeft, grabbedRefr);
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
						"Torch grabbed [off-hand]: %s formId=%08X (auto-equip suppressed)",
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

				ScheduleOrDelayGrabbedWeaponActivation(isLeft, grabbedRefr);
				return;
			}

			// A real two-hander owns the engine's both-hands equip slot and is always
			// rendered on the main hand, exactly the way a bow is always rendered on the
			// off hand. Grabbing one with the other controller must not auto-equip it —
			// it just stays a physics object in that hand, same as a bow grabbed with
			// the main hand.
			//
			// This deliberately does NOT apply to the 2H Weapons Unlocked proxy forms:
			// those are authored as one-handers precisely so they can be equipped to
			// either hand, so they keep the ordinary either-hand behaviour.
			if (IsTwoHandedWeaponForm(itemForm) &&
				!IsTwoHandProxyForm(itemForm) &&
				IsOffHandVRController(isLeft))
			{
				LOG_INFO(
					"2H weapon grabbed [off-hand]: %s formId=%08X (auto-equip suppressed)",
					GetSafeFormName(itemForm),
					grabbedRefr->formID);
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
					"Item grabbed [%s hand]: %s (%s) formId=%08X (auto-equip suppressed)",
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

			ScheduleOrDelayGrabbedWeaponActivation(isLeft, grabbedRefr);
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
		// Its own step, not Grip Hold's: that one returns at once in Legacy mode, and a grab waits
		// in every mode.
		higgsInterface->AddPrePhysicsStepCallback(OnGrabDelayStep);
		RegisterGrabPose();
		LOG_INFO(
			"Item grab handler registered (dagger, 1H sword, 1H axe, 1H mace, off-hand bow, main-hand crossbow%s%s%s%s).",
			enableStaves ? ", staff" : "",
			enableTorches ? ", off-hand torch" : "",
			enableTwoHandedWeapons ? ", 2H sword, 2H axe" : "",
			enableShields ? ", shield" : "");
	}
}
