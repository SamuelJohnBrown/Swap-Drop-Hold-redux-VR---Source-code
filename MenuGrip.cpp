#include "MenuGrip.h"

#include "GameGripGuard.h"
#include "GameGripRules.h"
#include "GripBlinkRules.h"
#include "Helper.h"
#include "config.h"

#include <skse64/GameMenus.h>
#include <skse64/GameVR.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		// Index 0 = right VR controller, 1 = left - the same order as grip-hold's hands.
		//
		// Everything the controller-state callback touches is atomic. That callback runs
		// inside SKSE's GetControllerState hook, under SKSE's lock, on whichever thread
		// asked for the controller - the game, a menu, HIGGS, or this mod.
		std::atomic<bool> s_holding[2]{ { false }, { false } };
		std::atomic<uint32_t> s_device[2]{
			{ vr_1_0_12::k_unTrackedDeviceIndexInvalid },
			{ vr_1_0_12::k_unTrackedDeviceIndexInvalid } };
		std::atomic<bool> s_hiding[2]{ { false }, { false } };
		std::atomic<bool> s_physicalGrip[2]{ { false }, { false } };
		std::atomic<bool> s_releasedInMenu[2]{ { false }, { false } };
		std::atomic<uint64_t> s_gripBits{ (1ull << 2) | (1ull << 34) };
		std::atomic<int> s_openMenus{ 0 };
		std::atomic<int> s_framesWithoutMenu{ 0 };

		// When the grip shown released after a pickup stops being shown released, per hand, in
		// steady_clock nanoseconds. 0: never started. GripBlinkRules.h has the why.
		std::atomic<long long> s_releasedUntil[2]{ { 0 }, { 0 } };
		// And when it began (M3.0.0-test7): the re-press waits for a clear hand, but never longer
		// than GripBlink::kHoldBackMaxSeconds after this.
		std::atomic<long long> s_releasedSince[2]{ { 0 }, { 0 } };
		constexpr long long kTicksPerSecond = 1000000000LL;

		// Only the grip is ever hidden. The trigger selects in menus and the stick scrolls
		// them, so neither is touched - even when the grip button preset counts the
		// trigger as grip.
		constexpr uint64_t kHiddenBits = (1ull << 2) | (1ull << 34);
		constexpr int kSqueezeAxis = 2; // k_EButton_Axis2, the analogue grip squeeze

		// A clock read, not an engine call, so the controller-state callback may use it.
		long long NowTicks()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		bool ShowingReleased(const int hand)
		{
			return GripBlink::Active(NowTicks(), s_releasedUntil[hand].load(std::memory_order_relaxed));
		}

		// Exactly grip-hold's own test (GripHold.cpp, IsGripPressed): any watched button
		// pressed, or k_EButton_Grip touched. "Let go" has to mean what grip-hold means by
		// letting go, or a hand could read as open here and closed there.
		bool GripClosed(const vr_1_0_12::VRControllerState001_t& state)
		{
			const uint64_t watched = s_gripBits.load(std::memory_order_relaxed);
			return (state.ulButtonPressed & watched) != 0 ||
				(state.ulButtonTouched & watched & (1ull << 2)) != 0;
		}

		void OnControllerState(
			uint32_t deviceIndex,
			vr_src::VRControllerState001_t* state,
			uint32_t /*stateSize*/,
			bool& /*keepEvent*/)
		{
			// Flags and clock reads only in here: no engine calls, no logging, no controller reads.
			if (!state)
			{
				return;
			}

			for (int hand = 0; hand < 2; ++hand)
			{
				if (deviceIndex != s_device[hand].load(std::memory_order_relaxed))
				{
					continue;
				}

				const bool hiding = s_hiding[hand].load(std::memory_order_relaxed);

				// The game-grip guard's record (GameGripRules.h, test10): what this grip really is, taken
				// before anything below hides it. Per hand, not per call - the read the game is served need not
				// be this one. A MENU's hiding is marked, so the game never gets that grip back and the menu
				// stays open. test13: on EVERY read, not only around a blink - the game is shown the grip as it
				// was before a pickup until the hand really opens, and this record is how the guard knows.
				GameGripGuardRecord(hand == 1,
					GameGrip::Take(NowTicks(), deviceIndex, GripClosed(*state), hiding, state->ulButtonPressed,
						state->ulButtonTouched, kHiddenBits, state->rAxis[kSqueezeAxis].x,
						state->rAxis[kSqueezeAxis].y));

				if (!hiding && !ShowingReleased(hand))
				{
					continue;
				}

				const bool closed = GripClosed(*state);
				s_physicalGrip[hand].store(closed, std::memory_order_relaxed);

				if (!closed)
				{
					if (hiding)
					{
						// The hand opened. Hiding ends for good: from here on a grip press is a
						// real press, and it closes the menu like any other.
						s_hiding[hand].store(false, std::memory_order_relaxed);
						s_releasedInMenu[hand].store(true, std::memory_order_relaxed);
					}
					// An open hand has nothing to hide: everyone sees it open, grip-hold included.
					continue;
				}

				// The blink hides this grip from the other mods, not from the game: the game-grip guard hands the
				// game back what the record above says (GameGripRules.h).
				state->ulButtonPressed &= ~kHiddenBits;
				state->ulButtonTouched &= ~kHiddenBits;
				state->rAxis[kSqueezeAxis].x = 0.0f;
				state->rAxis[kSqueezeAxis].y = 0.0f;
			}
		}

		void BeginHiding(const char* menuName)
		{
			s_framesWithoutMenu.store(0);

			// Only Grip Hold keeps a grip closed to carry a weapon. Simple mode never needs
			// the grip held, so its menus behave exactly as they do without this mod.
			if (!gripHoldMode || simpleMode)
			{
				return;
			}

			for (int hand = 0; hand < 2; ++hand)
			{
				// A release already recorded stays recorded: grip-hold has not taken it
				// yet, and hiding again here would swallow it.
				const bool hide = s_holding[hand].load() && !s_releasedInMenu[hand].load();
				if (!hide)
				{
					continue;
				}

				s_physicalGrip[hand].store(true);
				s_hiding[hand].store(true);
				LOG_INFO(
					"Menu grip: %s opened while the %s hand holds a weapon - that grip is hidden "
					"from the menu until the hand opens, so the menu stays open.",
					menuName ? menuName : "A menu",
					hand == 1 ? "left" : "right");
			}
		}

		void EndHiding(const char* why)
		{
			for (int hand = 0; hand < 2; ++hand)
			{
				if (s_hiding[hand].exchange(false))
				{
					LOG_INFO(
						"Menu grip: %s with the %s hand still holding its weapon - it stays in hand.",
						why,
						hand == 1 ? "left" : "right");
				}
			}
		}

		class MenuGripListener : public BSTEventSink<MenuOpenCloseEvent>
		{
		public:
			virtual EventResult ReceiveEvent(MenuOpenCloseEvent* evn, EventDispatcher<MenuOpenCloseEvent>* dispatcher) override
			{
				(void)dispatcher;
				if (!evn || !IsInputCapturingMenuName(evn->menuName))
				{
					return kEvent_Continue;
				}

				// Counted, because these menus stack: the tween menu opens the inventory,
				// the inventory opens a quantity box. Hiding starts with the first and
				// ends with the last.
				if (evn->opening)
				{
					if (s_openMenus.fetch_add(1) == 0)
					{
						BeginHiding(evn->menuName.data);
					}
					return kEvent_Continue;
				}

				if (s_openMenus.fetch_sub(1) <= 1)
				{
					s_openMenus.store(0);
					EndHiding("the last menu closed");
				}
				return kEvent_Continue;
			}
		};

		MenuGripListener s_listener;

		// Which tracked device is which hand. The callback is handed a device index, not a hand.
		void PublishDevices()
		{
			BSOpenVR* openVR = *g_openVR;
			if (openVR && openVR->vrSystem)
			{
				s_device[0].store(openVR->vrSystem->GetTrackedDeviceIndexForControllerRole(
					vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand));
				s_device[1].store(openVR->vrSystem->GetTrackedDeviceIndexForControllerRole(
					vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand));
			}
		}
	}

	void RegisterMenuGripMask(const PluginHandle plugin, SKSEVRInterface* const vr)
	{
		if (!vr || !vr->RegisterForControllerState)
		{
			LOG_ERR(
				"Menu grip: SKSE VR offers no controller-state callback, so a menu opened while "
				"holding a weapon will still close on the held grip.");
			return;
		}

		vr->RegisterForControllerState(plugin, 0, OnControllerState);
		LOG_INFO(
			"Menu grip: controller-state callback registered - a grip holding a weapon is "
			"hidden from menus until the hand opens.");
	}

	void RegisterMenuGripMenuListener()
	{
		MenuManager* menuManager = MenuManager::GetSingleton();
		if (!menuManager)
		{
			LOG_ERR("Menu grip: no menu manager at input load, so menus will still close on a held grip.");
			return;
		}

		menuManager->MenuOpenCloseEventDispatcher()->AddEventSink(&s_listener);
		LOG_INFO("Menu grip: listening for menus opening and closing.");
	}

	void MenuGripPublishHands(const bool rightHolding, const bool leftHolding)
	{
		s_holding[0].store(rightHolding);
		s_holding[1].store(leftHolding);
		s_gripBits.store(gripButtonMask);
		PublishDevices();
	}

	void MenuGripOnFrameWithoutMenu()
	{
		// The listener ends hiding when the last menu closes. This is the net under it: a
		// close it never heard, or a menu that opened and closed between two grip-hold
		// frames. Two frames rather than one, so a menu still in the middle of opening is
		// never taken for no menu at all.
		const int frames = s_framesWithoutMenu.fetch_add(1);
		if (frames > 1000)
		{
			s_framesWithoutMenu.store(2);
		}
		if (frames < 1)
		{
			return;
		}

		s_openMenus.store(0);
		if (s_hiding[0].load() || s_hiding[1].load())
		{
			EndHiding("no menu is open any more");
		}
	}

	bool MenuGripTakeReleasedInMenu(const bool isLeftVRController)
	{
		return s_releasedInMenu[isLeftVRController ? 1 : 0].exchange(false);
	}

	bool MenuGripPhysicalGrip(const bool isLeftVRController, const bool readValue)
	{
		const int hand = isLeftVRController ? 1 : 0;

		// A read the callback hid just before a shown release ended can arrive just after it,
		// so the real grip is still taken for a moment past that window.
		const long long graceTicks =
			static_cast<long long>(static_cast<double>(GripBlink::kPhysicalGraceSeconds) * kTicksPerSecond);
		const bool masked = s_hiding[hand].load() ||
			GripBlink::PhysicalGripStillMasked(NowTicks(), s_releasedUntil[hand].load(), graceTicks);

		// Nothing ever hides an open hand, so a closed read is always real.
		return masked ? (readValue || s_physicalGrip[hand].load()) : readValue;
	}

	void MenuGripShowReleased(const bool isLeftVRController, const float seconds)
	{
		const int hand = isLeftVRController ? 1 : 0;
		s_gripBits.store(gripButtonMask);
		PublishDevices();

		// Only ever started on a closed grip, and that is what grip-hold must be told until the
		// callback next reads the controller.
		s_physicalGrip[hand].store(true);
		const long long now = NowTicks();
		s_releasedSince[hand].store(now);
		s_releasedUntil[hand].store(GripBlink::EndTicks(now, seconds, kTicksPerSecond));
	}

	bool MenuGripShowingReleased(const bool isLeftVRController)
	{
		return ShowingReleased(isLeftVRController ? 1 : 0);
	}

	bool MenuGripBlinkWindow(const bool isLeftVRController, long long& startTicks, long long& endTicks)
	{
		const int hand = isLeftVRController ? 1 : 0;
		endTicks = s_releasedUntil[hand].load();
		startTicks = s_releasedSince[hand].load();
		return endTicks != 0;
	}

	void MenuGripHoldBlinkUntil(const bool isLeftVRController, const long long endTicks)
	{
		const int hand = isLeftVRController ? 1 : 0;
		long long current = s_releasedUntil[hand].load();
		// A window that has ended stays ended, and one that already runs later is left alone. The
		// loop only matters if a pickup restarts the window between the load and the store.
		while (GripBlink::Active(NowTicks(), current) && endTicks > current)
		{
			if (s_releasedUntil[hand].compare_exchange_weak(current, endTicks))
			{
				return;
			}
		}
	}

	long long MenuGripNowTicks()
	{
		return NowTicks();
	}

	bool MenuGripRealGripWhileShowing(const bool isLeftVRController)
	{
		return s_physicalGrip[isLeftVRController ? 1 : 0].load(std::memory_order_relaxed);
	}

	unsigned int MenuGripDevice(const bool isLeftVRController)
	{
		return s_device[isLeftVRController ? 1 : 0].load(std::memory_order_relaxed);
	}

	void MenuGripReset()
	{
		EndHiding("the weapon mode changed");
		s_releasedInMenu[0].store(false);
		s_releasedInMenu[1].store(false);
		s_releasedUntil[0].store(0);
		s_releasedUntil[1].store(0);
		s_releasedSince[0].store(0);
		s_releasedSince[1].store(0);
	}
}
