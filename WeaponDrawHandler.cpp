#include "WeaponDrawHandler.h"

#include "Engine.h"
#include "config.h"
#include "TrackedWeapons.h"

#include <skse64/PapyrusActor.h>
#include <skse64/PapyrusEvents.h>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		bool HasTrackedWeaponEquipped(PlayerCharacter* player)
		{
			if (!player)
			{
				return false;
			}

			TESForm* leftEquipped = player->GetEquippedObject(true);
			TESForm* rightEquipped = player->GetEquippedObject(false);
			return IsTrackedWeaponForm(leftEquipped) || IsTrackedWeaponForm(rightEquipped);
		}

		bool ShouldAllowDrawRefresh(PlayerCharacter* player)
		{
			if (!player)
			{
				return false;
			}

			const UInt32 movementFlags = player->actorState.flags04;
			if (movementFlags & ActorState::kState_Sneaking)
			{
				return false;
			}

			if (movementFlags & ActorState::kState_Swimming)
			{
				return false;
			}

			return true;
		}

		void EnsureWeaponDrawn(PlayerCharacter* player)
		{
			if (!player)
			{
				return;
			}

			if (!player->actorState.IsWeaponDrawn())
			{
				player->DrawSheatheWeapon(true);
			}
		}

		class WeaponSheatheEventHandler : public BSTEventSink<SKSEActionEvent>
		{
		public:
			virtual EventResult ReceiveEvent(SKSEActionEvent* evn, EventDispatcher<SKSEActionEvent>* dispatcher) override
			{
				// Never re-enter draw/sheathe from sheath events — that blocks sneak/jump
				// and can spam equip audio when dual-wielding.
				(void)evn;
				(void)dispatcher;
				return kEvent_Continue;
			}

			static WeaponSheatheEventHandler* GetSingleton()
			{
				static WeaponSheatheEventHandler instance;
				return &instance;
			}

		private:
			WeaponSheatheEventHandler() = default;
		};
	}

	void RedrawTrackedEquippedWeapons(const bool forceRefresh)
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player || !HasTrackedWeaponEquipped(player) || !ShouldAllowDrawRefresh(player))
		{
			return;
		}

		EnsureWeaponDrawn(player);

		if (forceRefresh)
		{
			papyrusActor::QueueNiNodeUpdate(player);
		}
	}

	void ScheduleWeaponDrawMaintenance(const bool isLeftGameHand, const bool forTwoHandedWeapon)
	{
		// One-shot only. Multi-frame draw/sheathe upkeep blocks sneak/jump for seconds
		// after grab-equip and causes equip SFX spam when dual-wielding.
		(void)forTwoHandedWeapon;
		RedrawTrackedEquippedWeapons(isLeftGameHand);
	}

	void RegisterWeaponDrawHandler()
	{
		g_actionEventDispatcher.AddEventSink(WeaponSheatheEventHandler::GetSingleton());
		LOG_INFO("Weapon draw handler registered (one-shot redraw after equip/swap).");
	}

	void RegisterWeaponDrawHiggsCallback()
	{
		// No continuous HIGGS draw upkeep — that was locking sneak/jump after equip.
	}
}
