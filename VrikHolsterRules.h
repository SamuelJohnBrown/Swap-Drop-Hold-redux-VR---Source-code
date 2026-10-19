#pragma once

// Rules for the holster API VRIK added in build 80700 (VRIK 0.8.7): when it may be called, how its
// mod events read, what a Grip hold release does at a holster, how long it waits for VRIK, and when
// the controller buzzes. No engine types in here, so tools_vrik_holster_tests.cpp can check it
// outside the game; VrikHolsterApi.cpp and GripHold.cpp feed it the live answers.
//
// The design, and the decisions behind it, are in docs/DESIGN-vrik-holster-api.md.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace SwapDropAndHoldRedux
{
	namespace VrikHolster
	{
		// Build 80700 appended GetHolsterSlotInReach, CanBeHolstered, HolsterWeapon and
		// UnHolsterWeapon to vrikinterface001. An older VRIK's vtable has no such slots, so they must
		// never be called on one.
		constexpr unsigned int kFirstApiBuild = 80700;

		// How long a release waits for VRIK to take a weapon it agreed to holster. VRIK does the
		// holster in its Papyrus script, which can lag well behind a frame when scripts are busy.
		constexpr float kWaitSeconds = 3.0f;

		// How long after a HolsterWeapon call VRIK's VRIK_Holstered event may take to arrive.
		constexpr float kEventGraceSeconds = 3.0f;

		// How long a hand must be away from a holster that takes its weapon before arriving again
		// buzzes again - the same 0.15s GripHold's stand-down rides out a boundary with.
		constexpr float kBuzzClearSeconds = 0.15f;

		inline bool HasApi(const bool haveInterface, const unsigned int buildNumber)
		{
			return haveInterface && buildNumber >= kFirstApiBuild;
		}

		// vrikslots.ini numbers its slots 1 to 14. Anything else from VRIK means no slot.
		inline bool IsValidSlot(const int slot)
		{
			return slot >= 1 && slot <= 14;
		}

		// VRIK's own mod event calls the game's right hand "Primary", so its secondary hand is taken
		// to be the game's left hand. This is the only place that assumption lives: if VRIK means
		// something else (the design's contingency C1), change it here.
		inline bool SecondaryHandFor(const bool isLeftGameHand)
		{
			return isLeftGameHand;
		}

		// One VRIK_Holstered or VRIK_Unholstered event. _vrik_qust_system.psc sends strArg
		// "Primary|0xFE123ABC" or "Secondary|0xFE123ABC" (the weapon's base form) and numArg the slot.
		struct Event
		{
			bool holstered = false;
			bool primary = false;
			uint32_t formID = 0;
			int slot = 0;
		};

		inline bool ParseEvent(const char* eventName, const char* strArg, const float numArg, Event& out)
		{
			if (!eventName || !strArg)
			{
				return false;
			}

			bool holstered = false;
			if (std::strcmp(eventName, "VRIK_Holstered") == 0)
			{
				holstered = true;
			}
			else if (std::strcmp(eventName, "VRIK_Unholstered") != 0)
			{
				return false;
			}

			const char* separator = std::strchr(strArg, '|');
			if (!separator)
			{
				return false;
			}

			const size_t handLength = static_cast<size_t>(separator - strArg);
			bool primary = false;
			if (handLength == 7 && std::strncmp(strArg, "Primary", 7) == 0)
			{
				primary = true;
			}
			else if (!(handLength == 9 && std::strncmp(strArg, "Secondary", 9) == 0))
			{
				return false;
			}

			const char* hex = separator + 1;
			if (hex[0] != '0' || (hex[1] != 'x' && hex[1] != 'X'))
			{
				return false;
			}
			hex += 2;

			const size_t digits = std::strlen(hex);
			if (digits == 0 || digits > 8)
			{
				return false;
			}
			for (size_t i = 0; i < digits; ++i)
			{
				if (!std::isxdigit(static_cast<unsigned char>(hex[i])))
				{
					return false;
				}
			}

			const uint32_t formID = static_cast<uint32_t>(std::strtoul(hex, nullptr, 16));
			if (formID == 0)
			{
				return false;
			}

			// numArg carries a whole slot number as a float. The range test also turns away NaN.
			if (!(numArg >= 0.5f && numArg < 14.5f))
			{
				return false;
			}

			out.holstered = holstered;
			out.primary = primary;
			out.formID = formID;
			out.slot = static_cast<int>(numArg + 0.5f);
			return true;
		}

		// Whether a VRIK event is the holster this mod asked VRIK for.
		inline bool EventIsFor(const Event& evn, const uint32_t formID)
		{
			return evn.holstered && evn.formID == formID;
		}

		// Whether a VRIK event names the hand this mod asked VRIK about.
		inline bool EventHandMatches(const Event& evn, const bool secondaryHandAsked)
		{
			return evn.primary == !secondaryHandAsked;
		}

		// Whether a release takes the new path: VRIK 0.8.7 or newer, Grip hold (not Simple), holster
		// awareness on and "Holster when you let go" on. Everywhere else the 2.1.1 code decides.
		inline bool ReleasePathApplies(
			const bool apiAvailable,
			const bool simpleMode,
			const bool holsterAware,
			const bool holsterOnRelease)
		{
			return apiAvailable && !simpleMode && holsterAware && holsterOnRelease;
		}

		// iHolsterAction 1 and 2 sheathe the weapon ourselves, from before VRIK could be asked to
		// holster it. With the API they count as 0.
		inline int EffectiveHolsterAction(const bool apiAvailable, const int configured)
		{
			return apiAvailable ? 0 : configured;
		}

		enum class Release
		{
			kNotThisPath, // not the new path: the 2.1.1 code decides
			kNoSlot,      // no holster in reach: stick or drop as usual
			kSlotRefuses, // VRIK says the holster in reach cannot take the weapon: stick or drop
			kAskVrik,     // call HolsterWeapon
		};

		// What a Grip hold release does about holsters on the new path. The other hand's branches
		// (a two-hander it still grips, a hand-off) come first in DispatchGripRelease and return
		// before this is asked.
		inline Release DecideRelease(const bool pathApplies, const int slotInReach, const bool canBeHolstered)
		{
			if (!pathApplies)
			{
				return Release::kNotThisPath;
			}
			if (!IsValidSlot(slotInReach))
			{
				return Release::kNoSlot;
			}
			return canBeHolstered ? Release::kAskVrik : Release::kSlotRefuses;
		}

		enum class Wait
		{
			kWaiting,   // still in the hand, and time is left
			kHolstered, // the hand is empty: VRIK took it
			kSwapped,   // a different weapon is in the hand: VRIK swapped it with the holster's
			kTimedOut,  // still there when time ran out: keep it, never drop it
		};

		// One physics step of waiting. formInHand is what that game hand holds now, 0 for nothing.
		inline Wait TickWait(
			float& secondsLeft,
			const uint32_t holsteringFormID,
			const uint32_t formInHand,
			const float deltaTime)
		{
			if (formInHand == 0)
			{
				return Wait::kHolstered;
			}
			if (formInHand != holsteringFormID)
			{
				return Wait::kSwapped;
			}

			secondsLeft -= deltaTime;
			return secondsLeft <= 0.0f ? Wait::kTimedOut : Wait::kWaiting;
		}

		// How a kept weapon is held after a timeout: still held when the grip is closed, otherwise it
		// waits for a fresh squeeze, like a weapon equipped from a menu.
		struct KeptHand
		{
			bool armed = false;
			bool needsFreshGripPress = false;
		};

		inline KeptHand HandAfterTimeout(const bool gripClosed)
		{
			KeptHand kept;
			kept.armed = gripClosed;
			kept.needsFreshGripPress = !gripClosed;
			return kept;
		}

		// The "this holster will take it" buzz: once on arriving, and again only after the hand has
		// been away for kBuzzClearSeconds, so a hand resting on the edge buzzes once.
		class BuzzEdge
		{
		public:
			// True when the controller should pulse this step.
			bool Update(const bool takesIt, const float deltaTime)
			{
				if (takesIt)
				{
					m_clearSeconds = 0.0f;
					if (!m_inside)
					{
						m_inside = true;
						return true;
					}
					return false;
				}

				if (m_inside)
				{
					m_clearSeconds += deltaTime;
					if (m_clearSeconds >= kBuzzClearSeconds)
					{
						m_inside = false;
						m_clearSeconds = 0.0f;
					}
				}
				return false;
			}

			void Reset()
			{
				m_inside = false;
				m_clearSeconds = 0.0f;
			}

		private:
			bool m_inside = false;
			float m_clearSeconds = 0.0f;
		};
	}
}
