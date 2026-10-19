#pragma once

// Telling VRIK's holster buzz from every other haptic pulse, and standing aside where it buzzed.
// No engine types in here, so tools_vrik_pulse_tests.cpp can check it outside the game;
// HapticCensus.cpp feeds the detector, GripHold.cpp holds the stand-aside.
//
// THE MEASUREMENT THIS RESTS ON (M2.2.7-test1's census, 2026-09-17). VRIK's holster haptic is a run
// of exactly hapticStrength pulses - 8 on that install - of one duration (749 us there), one per
// frame (~17 ms apart), on the hand holding the weapon, fired ONCE when that hand enters the
// activation range of a slot that would take the weapon - right type, slot empty - and then not
// again until the hand leaves and re-enters. Nothing else in that log looked like it: this mod's
// own buzz is a single pulse, and the other mods' pulses were either singles or bursts fired in the
// same instant. So "a run of same-duration pulses spaced one frame apart, not ours" is VRIK, and it
// is the answer VRIK 0.8.7's CanBeHolstered gives - delivered through the haptics, on any VRIK.

#include <cmath>

namespace SwapDropAndHoldRedux
{
	namespace VrikPulse
	{
		// One frame apart: 5 to 40 ms. The census measured 13 to 18 ms at ~60 fps; 40 covers a 25 fps
		// floor, and 5 excludes bursts sent in the same instant.
		constexpr long long kFrameGapMinNs = 5LL * 1000000LL;
		constexpr long long kFrameGapMaxNs = 40LL * 1000000LL;

		// Fires on the pulse that makes the run this long, ~4 frames after VRIK started buzzing.
		// hapticStrength defaults to 8; a player who turned it down to 4 is still heard.
		constexpr int kMinRun = 4;

		class RunDetector
		{
		public:
			// One pulse, in the order they were sent. hand: 1 left, 0 right, anything else ignored. Returns
			// true exactly once per qualifying run, on the pulse that completes kMinRun.
			bool Feed(const int hand, const bool ours, const unsigned short micros, const long long ticksNs)
			{
				if (hand < 0 || hand > 1)
				{
					return false;
				}
				Track& track = m_hands[hand];
				if (ours)
				{
					// This mod's own buzz is never VRIK's, and a run cannot straddle it.
					track.count = 0;
					track.fired = false;
					track.lastTicks = ticksNs;
					return false;
				}

				const long long gap = ticksNs - track.lastTicks;
				const bool continues = track.count > 0 && micros == track.micros &&
					gap >= kFrameGapMinNs && gap <= kFrameGapMaxNs;
				if (continues)
				{
					++track.count;
				}
				else
				{
					track.count = 1;
					track.micros = micros;
					track.fired = false;
				}
				track.lastTicks = ticksNs;

				if (!track.fired && track.count >= kMinRun)
				{
					track.fired = true;
					return true;
				}
				return false;
			}

		private:
			struct Track
			{
				long long lastTicks = 0;
				unsigned short micros = 0;
				int count = 0;
				bool fired = false;
			};
			Track m_hands[2];
		};

		// Which answer governs Grip hold's stand-aside. VRIK 0.8.7's API always wins; Simple mode is
		// untouched; the player can switch the listening off; and until VRIK has buzzed once this
		// session there is no proof its haptics are on, so the estimate zone keeps governing.
		inline bool VrikDrives(
			const bool listenToVrik,
			const bool apiAvailable,
			const bool simpleMode,
			const bool vrikBuzzedThisSession)
		{
			return listenToVrik && !apiAvailable && !simpleMode && vrikBuzzedThisSession;
		}

		struct Vec3
		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
		};

		inline float Distance(const Vec3& a, const Vec3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		// How far from where it entered a hand must move before it has left the holster. VRIK keeps a
		// slot until slotChangeDistanceMultiplier (1.75) x activation distance from the CENTRE, and the
		// entry point is about one activation distance from that centre. A hand moving straight
		// through leaves VRIK's hold up to 1.75 + 1 = 2.75 reaches from where it entered. Ending the
		// stand-aside sooner would let a release there drop a weapon VRIK would still take; ending it
		// later only means the weapon stays in the hand a little longer before grip-hold is live
		// again - the safe side.
		constexpr float kLeaveFactor = 2.75f;
		constexpr float kMinHoldSeconds = 0.25f;
		constexpr float kMaxHoldSeconds = 20.0f; // a hand parked at a holster is not parked for good

		inline float LeaveDistance(const float activationDistance)
		{
			return kLeaveFactor * activationDistance;
		}

		// One hand's "VRIK said this is a holster" state.
		class StandAside
		{
		public:
			// A VRIK run on this hand: it is at a holster right now. A second buzz while still aside means
			// VRIK left and re-entered, so the entry point moves with it.
			void Enter(const Vec3& handAtEntry, const float now)
			{
				m_active = true;
				m_entry = handAtEntry;
				m_since = now;
			}

			// Each step. False once the hand has moved past the leave distance from where it entered
			// (after the minimum hold), or the maximum hold has passed.
			bool Update(const Vec3& hand, const float now, const float activationDistance)
			{
				if (!m_active)
				{
					return false;
				}
				const float held = now - m_since;
				if (held >= kMaxHoldSeconds)
				{
					m_active = false;
					return false;
				}
				if (held >= kMinHoldSeconds && Distance(hand, m_entry) > LeaveDistance(activationDistance))
				{
					m_active = false;
					return false;
				}
				return true;
			}

			bool Active() const
			{
				return m_active;
			}

			void Clear()
			{
				m_active = false;
			}

			float DistanceFromEntry(const Vec3& hand) const
			{
				return Distance(hand, m_entry);
			}

		private:
			bool m_active = false;
			Vec3 m_entry;
			float m_since = 0.0f;
		};
	}
}
