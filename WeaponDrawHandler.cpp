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
		static const int kDrawMaintenanceFrames = 45;
		static const int kLeftHandDrawBurstFrames = 90;
		static int s_leftHandDrawBurstFramesRemaining = 0;

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

		bool HasTrackedWeaponOnLeftHandOnly(PlayerCharacter* player)
		{
			if (!player)
			{
				return false;
			}

			TESForm* leftEquipped = player->GetEquippedObject(true);
			TESForm* rightEquipped = player->GetEquippedObject(false);
			return IsTrackedWeaponForm(leftEquipped) && !IsTrackedWeaponForm(rightEquipped);
		}

		void ForceWeaponDrawRefresh(PlayerCharacter* player)
		{
			if (!player)
			{
				return;
			}

			if (player->actorState.IsWeaponDrawn())
			{
				player->DrawSheatheWeapon(false);
			}

			player->DrawSheatheWeapon(true);
			papyrusActor::QueueNiNodeUpdate(player);
		}

		class MaintainWeaponDrawTask : public TaskDelegate
		{
		public:
			explicit MaintainWeaponDrawTask(const int framesRemaining = kDrawMaintenanceFrames)
				: m_framesRemaining(framesRemaining)
			{
			}

			virtual void Run() override
			{
				PlayerCharacter* player = *g_thePlayer;
				if (!player || !HasTrackedWeaponEquipped(player))
				{
					return;
				}

				RedrawTrackedEquippedWeapons(s_leftHandDrawBurstFramesRemaining > 0);

				if (m_framesRemaining > 0 && g_task)
				{
					g_task->AddTask(new MaintainWeaponDrawTask(m_framesRemaining - 1));
				}
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			int m_framesRemaining;
		};

		class WeaponSheatheEventHandler : public BSTEventSink<SKSEActionEvent>
		{
		public:
			virtual EventResult ReceiveEvent(SKSEActionEvent* evn, EventDispatcher<SKSEActionEvent>* dispatcher) override
			{
				if (!evn || !evn->actor || evn->actor != *g_thePlayer)
				{
					return kEvent_Continue;
				}

				if (evn->type != SKSEActionEvent::kType_BeginSheathe &&
					evn->type != SKSEActionEvent::kType_EndSheathe)
				{
					return kEvent_Continue;
				}

				PlayerCharacter* player = *g_thePlayer;
				if (!player || !HasTrackedWeaponEquipped(player))
				{
					return kEvent_Continue;
				}

				RedrawTrackedEquippedWeapons(HasTrackedWeaponOnLeftHandOnly(player));
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

		void MaintainTrackedWeaponDrawState()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !HasTrackedWeaponEquipped(player))
			{
				s_leftHandDrawBurstFramesRemaining = 0;
				return;
			}

			if (s_leftHandDrawBurstFramesRemaining > 0)
			{
				s_leftHandDrawBurstFramesRemaining--;
				ForceWeaponDrawRefresh(player);
				return;
			}

			if (!player->actorState.IsWeaponDrawn())
			{
				player->DrawSheatheWeapon(true);
				return;
			}

			if (HasTrackedWeaponOnLeftHandOnly(player))
			{
				papyrusActor::QueueNiNodeUpdate(player);
			}
		}

		void OnPostVrikPostHiggs()
		{
			MaintainTrackedWeaponDrawState();
		}
	}

	void RedrawTrackedEquippedWeapons(const bool forceRefresh)
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player || !HasTrackedWeaponEquipped(player))
		{
			return;
		}

		if (forceRefresh)
		{
			ForceWeaponDrawRefresh(player);
			return;
		}

		if (!player->actorState.IsWeaponDrawn())
		{
			player->DrawSheatheWeapon(true);
		}
	}

	void ScheduleWeaponDrawMaintenance(const bool isLeftGameHand)
	{
		if (isLeftGameHand)
		{
			s_leftHandDrawBurstFramesRemaining = kLeftHandDrawBurstFrames;
		}

		if (!g_task)
		{
			RedrawTrackedEquippedWeapons(isLeftGameHand);
			return;
		}

		RedrawTrackedEquippedWeapons(isLeftGameHand);
		g_task->AddTask(new MaintainWeaponDrawTask());
	}

	void RegisterWeaponDrawHandler()
	{
		g_actionEventDispatcher.AddEventSink(WeaponSheatheEventHandler::GetSingleton());
		LOG_INFO("Weapon draw handler registered (auto-redraw on sheath).");
	}

	void RegisterWeaponDrawHiggsCallback()
	{
		if (!higgsInterface)
		{
			return;
		}

		higgsInterface->AddPostVrikPostHiggsCallback(OnPostVrikPostHiggs);
	}
}
