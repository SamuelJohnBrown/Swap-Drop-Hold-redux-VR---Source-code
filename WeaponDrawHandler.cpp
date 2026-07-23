#include "WeaponDrawHandler.h"

#include "Engine.h"
#include "config.h"
#include "TrackedWeapons.h"

#include <skse64/PapyrusActor.h>
#include <skse64/PapyrusEvents.h>
#include <skse64/gamethreads.h>

namespace SwapDropAndHoldRedux
{
	extern SKSETaskInterface* g_task;

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
			return IsGrabEquipDropItemForm(leftEquipped) || IsGrabEquipDropItemForm(rightEquipped);
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

		bool IsTwoHandProxyEquipped(PlayerCharacter* player)
		{
			return IsTwoHandProxyForm(player->GetEquippedObject(true)) ||
				IsTwoHandProxyForm(player->GetEquippedObject(false));
		}

		static const int kDelayedDrawInitialFrames = 3;
		static const int kDrawRetryIntervalFrames = 10;
		static const int kDrawRetryCount = 6;

		// Crossbows play a cocking sequence on equip that swallows a same-frame draw
		// request, leaving them equipped but sheathed (invisible). This task waits a
		// few frames, requests the draw, and re-checks until the weapon is actually
		// drawn before the final skeleton refresh.
		class DelayedWeaponDrawTask : public TaskDelegate
		{
		public:
			DelayedWeaponDrawTask(const int delayFramesRemaining, const int drawRetriesRemaining)
				: m_delayFramesRemaining(delayFramesRemaining)
				, m_drawRetriesRemaining(drawRetriesRemaining)
			{
			}

			virtual void Run() override
			{
				if (m_delayFramesRemaining > 0)
				{
					Requeue(m_delayFramesRemaining - 1, m_drawRetriesRemaining);
					return;
				}

				PlayerCharacter* player = *g_thePlayer;
				if (!player || !HasTrackedWeaponEquipped(player) || !ShouldAllowDrawRefresh(player))
				{
					return;
				}

				if (IsTwoHandProxyEquipped(player))
				{
					return;
				}

				if (!player->actorState.IsWeaponDrawn())
				{
					player->DrawSheatheWeapon(true);

					if (m_drawRetriesRemaining > 0)
					{
						Requeue(kDrawRetryIntervalFrames, m_drawRetriesRemaining - 1);
						return;
					}
				}

				papyrusActor::QueueNiNodeUpdate(player);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			void Requeue(const int delayFrames, const int retries)
			{
				if (g_task)
				{
					g_task->AddTask(new DelayedWeaponDrawTask(delayFrames, retries));
				}
			}

			int m_delayFramesRemaining;
			int m_drawRetriesRemaining;
		};

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
		(void)isLeftGameHand;

		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			return;
		}

		// 2H Weapons Unlocked proxies run their own equip conversion + draw upkeep the
		// moment they're equipped. Forcing draw / NiNode updates on top of that locks
		// posture for a few seconds and can CTD after repeated grab/drop cycles.
		if (IsTwoHandProxyEquipped(player))
		{
			return;
		}

		// Skeleton NiNode refresh is only safe/needed for 1H weapons; it destabilizes
		// 2H equips mid-animation.
		RedrawTrackedEquippedWeapons(!forTwoHandedWeapon);
	}

	void ScheduleDelayedWeaponDrawMaintenance()
	{
		if (g_task)
		{
			g_task->AddTask(new DelayedWeaponDrawTask(kDelayedDrawInitialFrames, kDrawRetryCount));
		}
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
