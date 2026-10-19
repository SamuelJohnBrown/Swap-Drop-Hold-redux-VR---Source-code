#include "WeaponDrawHandler.h"

#include "Engine.h"
#include "config.h"
#include "TrackedWeapons.h"

#include <skse64/PapyrusActor.h>
#include <skse64/PapyrusEvents.h>
#include <skse64/gamethreads.h>

#include <chrono>

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

		// Returns true if it actually had to draw. That distinction matters: the draw
		// is what plays the weapon's unsheathe sound and runs the draw animation, and
		// it only happens when the player was sheathed to begin with — which is why a
		// grab feels and sounds different on the main hand than on the off hand.
		// IsWeaponDrawn() is (flags08 >> 5 & 7) >= 3, so it stays TRUE through
		// kWantToSheathe(4) and kSheathing(5). Only 3 means drawn AND settled, and the
		// whole forced-draw cycle turns on that distinction: a re-draw issued while the
		// player is still in 4 or 5 cancels the sheathe instead of following it.
		UInt32 PlayerDrawState(PlayerCharacter* player)
		{
			return player ? ((player->actorState.flags08 >> 5) & 7) : 99;
		}

		bool EnsureWeaponDrawn(PlayerCharacter* player)
		{
			if (!player || !weaponDrawOnEquip)
			{
				return false;
			}

			// Strict: reporting "already drawn" at state 4 or 5 skips a draw that really
			// is needed and leaves the weapon equipped but invisible.
			if (PlayerDrawState(player) == 3)
			{
				return false;
			}

			player->DrawSheatheWeapon(true);
			return true;
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
		float RefreshNowSeconds()
		{
			static const auto start = std::chrono::steady_clock::now();
			return std::chrono::duration<float>(
				std::chrono::steady_clock::now() - start).count();
		}

		// Straight through, no task requeue: AddTask from inside a task drains in the
		// SAME frame, so a "wait N frames" loop built that way never waits at all. That
		// mistake is what made the forced draw busy-wait a wall clock for 1.5s on every
		// grab, and what made the dual-wield watch complete 90 frames in zero
		// milliseconds without observing anything.
		//
		// The refresh itself was measured and changes nothing for other mods; it stays
		// only because it is harmless, off by default, and someone may want it.
		bool MaybeRefreshSkeleton(const bool alreadyDrawn)
		{
			if (!weaponSkeletonRefresh || !alreadyDrawn)
			{
				return false;
			}

			PlayerCharacter* player = *g_thePlayer;
			if (!player || !HasTrackedWeaponEquipped(player) || IsTwoHandProxyEquipped(player))
			{
				return false;
			}

			papyrusActor::QueueNiNodeUpdate(player);
			return true;
		}

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

				if (weaponDrawOnEquip && PlayerDrawState(player) != 3)
				{
					player->DrawSheatheWeapon(true);

					if (m_drawRetriesRemaining > 0)
					{
						Requeue(kDrawRetryIntervalFrames, m_drawRetriesRemaining - 1);
						return;
					}
				}

				// Reaching here means no draw was needed, which is exactly the case
				// the refresh is for.
				MaybeRefreshSkeleton(true);
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

	UInt32 PlayerWeaponDrawState(PlayerCharacter* player)
	{
		return PlayerDrawState(player);
	}

	bool RequestWeaponDrawAgain()
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player || !weaponDrawOnEquip)
		{
			return false;
		}
		if (!HasTrackedWeaponEquipped(player) || !ShouldAllowDrawRefresh(player))
		{
			return false;
		}
		if (IsTwoHandProxyEquipped(player))
		{
			return false;
		}
		// Already out: never ask again. This is the whole difference between a watch and the
		// upkeep that used to block sneak and jump.
		if (PlayerDrawState(player) == 3)
		{
			return false;
		}

		player->DrawSheatheWeapon(true);
		return true;
	}

	bool RedrawTrackedEquippedWeapons(const bool forceRefresh)
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player || !HasTrackedWeaponEquipped(player) || !ShouldAllowDrawRefresh(player))
		{
			return false;
		}

		const bool drew = EnsureWeaponDrawn(player);

		// Never refresh on top of a draw we just performed: the draw already did the
		// announcing, and stacking a node rebuild on it is pure jolt for nothing.
		const bool refreshed = forceRefresh && MaybeRefreshSkeleton(!drew);

		// Printed on EVERY grab now, not only when something happened. The absence of
		// this line was doing diagnostic work by implication in the last three rounds,
		// and implication is what made the alignment wrong twice.
		LOG_INFO(
			"Weapon draw maintenance: drew=%s skeletonRefresh=%s",
			drew ? "yes" : "no",
			refreshed ? "queued" : (weaponSkeletonRefresh ? "not needed" : "off"));

		return drew;
	}

	void ScheduleWeaponDrawMaintenance(const bool isLeftGameHand, const bool forTwoHandedWeapon)
	{
		// One-shot only. Multi-frame draw/sheathe upkeep blocks sneak/jump for seconds
		// after grab-equip and causes equip SFX spam when dual-wielding.

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
		(void)isLeftGameHand;
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
