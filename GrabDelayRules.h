#pragma once

// How long a weapon grabbed with HIGGS stays a loose object before this mod picks it up and
// equips it. No engine types in here, so tools_grab_delay_tests.cpp can check it outside the
// game; WeaponGrabLogger.cpp feeds it what HIGGS says each hand is holding.
//
// A player request: grab a weapon and carry it, turn it over or toss it as the physics object
// it is, and only have it become a real weapon once it has been held on purpose for a moment.
// fGrabEquipDelay on the menu's Weapon mode and controls page. Zero - the default - is the
// behaviour from before this setting existed: a grabbed weapon is picked up and equipped at once.

#include <cstdint>

namespace SwapDropAndHoldRedux
{
	namespace GrabDelay
	{
		// The slider's top, raised from 3 to 10 at the user's request.
		constexpr float kMaxSeconds = 10.0f;

		inline float Clamp(const float seconds)
		{
			return seconds < 0.0f ? 0.0f : (seconds > kMaxSeconds ? kMaxSeconds : seconds);
		}

		inline bool Waits(const float delaySeconds)
		{
			return delaySeconds > 0.0f;
		}

		// M3.0.0-test15: a weapon it would be a crime to take - the red "Steal" prompt, the engine's own
		// TESObjectREFR::IsCrimeToActivate - stays a loose object at least this long, so a shop's wares can be
		// picked up and looked at without being stolen on the spot. fGrabEquipDelayStolen; 0 = no extra wait.
		// 20 at most: the grab's measured grip (GrabGripRules.h kMaxAgeSeconds, 30) must still be fresh when the
		// weapon is finally taken, or it would be held the natural way.
		constexpr float kMaxStolenSeconds = 20.0f;

		inline float ClampStolen(const float seconds)
		{
			return seconds < 0.0f ? 0.0f : (seconds > kMaxStolenSeconds ? kMaxStolenSeconds : seconds);
		}

		// The wait for this grab: the ordinary one, or for a weapon that is not yours the longer of the two.
		inline float DelayFor(const float normalSeconds, const float stolenSeconds, const bool offLimits)
		{
			const float normal = Clamp(normalSeconds);
			if (!offLimits)
			{
				return normal;
			}
			const float stolen = ClampStolen(stolenSeconds);
			return stolen > normal ? stolen : normal;
		}

		enum class Step
		{
			kIdle,      // nothing is being timed
			kWaiting,   // still a loose object
			kEquip,     // held long enough: pick it up and equip it now
			kAbandoned, // let go, or swapped for something else, first: it stays loose
		};

		// One hand's grabbed weapon, waiting out the delay.
		class Hold
		{
		public:
			// HIGGS can report the same grab more than once. A repeat for the weapon already being
			// timed keeps its time (and what the first found about whose it is); any other weapon starts
			// from zero. offLimits: taking it would be stealing (test15).
			void Begin(const std::uint32_t refID, const bool offLimits = false)
			{
				if (refID == 0)
				{
					return;
				}
				if (m_active && m_refID == refID)
				{
					return;
				}
				m_refID = refID;
				m_heldSeconds = 0.0f;
				m_active = true;
				m_offLimits = offLimits;
			}

			bool OffLimits() const
			{
				return m_offLimits;
			}

			// heldRefID: what HIGGS says this hand holds right now, 0 for nothing. The delay is
			// read every step, so moving the slider mid-hold takes effect at once - including
			// down to zero, which equips what is held.
			Step Update(const std::uint32_t heldRefID, const float deltaTime, const float delaySeconds)
			{
				if (!m_active)
				{
					return Step::kIdle;
				}

				if (heldRefID != m_refID)
				{
					m_active = false;
					return Step::kAbandoned;
				}

				m_heldSeconds += deltaTime;
				if (m_heldSeconds >= delaySeconds)
				{
					m_active = false;
					return Step::kEquip;
				}
				return Step::kWaiting;
			}

			void Cancel()
			{
				m_active = false;
			}

			bool Active() const
			{
				return m_active;
			}

			std::uint32_t RefID() const
			{
				return m_refID;
			}

			float HeldSeconds() const
			{
				return m_heldSeconds;
			}

		private:
			std::uint32_t m_refID = 0;
			float m_heldSeconds = 0.0f;
			bool m_active = false;
			bool m_offLimits = false;
		};
	}
}
