#include "CellLoadGuard.h"

#include "CellLoadGuardRules.h"
#include "config.h"

#include <skse64/GameMenus.h>

#include <cstring>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		CellLoadGuard::Guard s_guard;

		// SkyrimVR puts this up for every door transition, fast travel and save load, and holds it until
		// the new cell is ready. Matched by name because the menu manager has no "is this a load" flag.
		bool IsLoadingMenuName(const BSFixedString& name)
		{
			const char* text = name.data;
			if (!text)
			{
				return false;
			}
			return _stricmp(text, "Loading Menu") == 0;
		}

		class CellLoadListener : public BSTEventSink<MenuOpenCloseEvent>
		{
		public:
			virtual EventResult ReceiveEvent(MenuOpenCloseEvent* evn, EventDispatcher<MenuOpenCloseEvent>* dispatcher) override
			{
				(void)dispatcher;
				if (!evn || !cellLoadLock || !IsLoadingMenuName(evn->menuName))
				{
					return kEvent_Continue;
				}

				// Flags and a clock only in here. The hold is counted down on the mod's own tick.
				if (evn->opening)
				{
					if (s_guard.LoadingStarted() == CellLoadGuard::Step::kLocked)
					{
						LOG_INFO(
							"Cell load: the loading screen is up - the mod has stood down so nothing in your hands is "
							"dropped on the way through. It takes itself back %.1fs after the new cell appears.",
							CellLoadGuard::ClampHold(cellLoadLockSeconds));
					}
				}
				else
				{
					s_guard.LoadingFinished();
				}
				return kEvent_Continue;
			}
		};

		CellLoadListener s_listener;
	}

	void RegisterCellLoadGuard()
	{
		MenuManager* menuManager = MenuManager::GetSingleton();
		if (!menuManager)
		{
			LOG_ERR("Cell load: no menu manager at input load, so the mod cannot stand down for loading screens.");
			return;
		}

		menuManager->MenuOpenCloseEventDispatcher()->AddEventSink(&s_listener);
		LOG_INFO(
			"Cell load: %s - the mod stands down while a loading screen is up%s.",
			cellLoadLock ? "listening for loading screens" : "OFF (bCellLoadLock = 0)",
			cellLoadLock ? " and for a moment after the new cell appears" : "");
	}

	void CellLoadGuardTick(const float deltaTime)
	{
		if (!cellLoadLock)
		{
			if (s_guard.Locked())
			{
				s_guard.Clear();
			}
			return;
		}

		switch (s_guard.Update(deltaTime, cellLoadLockSeconds))
		{
		case CellLoadGuard::Step::kUnlocked:
			LOG_INFO("Cell load: the new cell has settled - the mod has itself back.");
			break;
		default:
			break;
		}
	}

	bool CellLoadGuardLocked()
	{
		return cellLoadLock && s_guard.Locked();
	}

	void CellLoadGuardReset()
	{
		s_guard.Clear();
	}
}
