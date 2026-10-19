#pragma once

// The bookkeeping behind the haptic census: the ring that carries pulses from whatever thread fired
// them to the physics step, the per-session log cap, and the summary that turns hundreds of pulses
// into a few rows. No engine types in here, so tools_haptic_census_tests.cpp can check it outside
// the game; HapticCensus.cpp does the hooking, the reading and the logging.
//
// WHY THIS EXISTS. VRIK buzzes a controller when that hand comes within a slot's activation
// distance - on VRIK's own body, by VRIK's own rules. That is the answer this mod has been
// estimating since 2.1.0, and on a VRIK without the holster API it is the only place the answer
// shows. VRIK.dll imports nothing from OpenVR, so its pulses go through the game's shared IVRSystem
// like everyone else's, where one vtable-slot hook sees them all. The census records every pulse
// with what this mod knows about that hand at that moment, so VRIK's pulses can be told apart from
// HIGGS's and this mod's own, and their timing read against the slot positions. It pins nothing.

#include <cstdint>

namespace SwapDropAndHoldRedux
{
	namespace HapticCensus
	{
		// Pulses arrive on any thread and are read on the physics step. A physics step is ~11 ms;
		// nothing sends 64 pulses in that time except a bug, which the drop count would then show.
		constexpr int kRingCapacity = 64;

		// Per-pulse lines stop here; the summaries carry on. Enough for a session of sweeping both
		// hands over every holster.
		constexpr int kMaxLoggedPerSession = 300;
		constexpr int kSummaryEvery = 50;
		constexpr int kSummaryRows = 24;

		constexpr unsigned int kInvalidDevice = 0xFFFFFFFFu; // OpenVR's k_unTrackedDeviceIndexInvalid

		enum class Source
		{
			kOurs,  // this mod's own PulseController, flagged just before it sends
			kOther, // anyone else: VRIK, HIGGS, Weapon Throw VR, ...
		};

		struct Pulse
		{
			unsigned int device = 0;
			unsigned int axis = 0;
			unsigned short micros = 0;
			long long ticks = 0;
			Source source = Source::kOther;
			// test12: where the pulse was sent from (the return address), so the log can name the module -
			// HIGGS, VRIK, the game or another mod - instead of "another mod". 0 when unknown.
			uintptr_t caller = 0;
		};

		// Fixed size, oldest dropped first, and it says how many it dropped. The caller locks.
		class Ring
		{
		public:
			void Push(const Pulse& pulse)
			{
				if (m_count == kRingCapacity)
				{
					m_head = (m_head + 1) % kRingCapacity;
					--m_count;
					++m_dropped;
				}
				m_items[(m_head + m_count) % kRingCapacity] = pulse;
				++m_count;
			}

			int Drain(Pulse* out, const int max)
			{
				const int n = m_count < max ? m_count : max;
				for (int i = 0; i < n; ++i)
				{
					out[i] = m_items[(m_head + i) % kRingCapacity];
				}
				m_head = (m_head + n) % kRingCapacity;
				m_count -= n;
				return n;
			}

			int Count() const
			{
				return m_count;
			}

			int Dropped() const
			{
				return m_dropped;
			}

			int TakeDropped()
			{
				const int dropped = m_dropped;
				m_dropped = 0;
				return dropped;
			}

		private:
			Pulse m_items[kRingCapacity];
			int m_head = 0;
			int m_count = 0;
			int m_dropped = 0;
		};

		inline bool ShouldLog(const int loggedSoFar)
		{
			return loggedSoFar < kMaxLoggedPerSession;
		}

		inline bool ShouldSummarize(const int total)
		{
			return total > 0 && total % kSummaryEvery == 0;
		}

		struct Row
		{
			int hand = 0; // 1 left, 0 right, -1 not a controller
			unsigned short micros = 0;
			Source source = Source::kOther;
			int count = 0;
		};

		// Counts per (hand, duration, source). A mod's pulses have a duration of their own - this mod's
		// is iHolsterHapticMicroseconds - so the rows are what tells VRIK's apart from HIGGS's.
		class Summary
		{
		public:
			void Add(const int hand, const unsigned short micros, const Source source)
			{
				++m_total;
				for (int i = 0; i < m_count; ++i)
				{
					Row& row = m_rows[i];
					if (row.hand == hand && row.micros == micros && row.source == source)
					{
						++row.count;
						return;
					}
				}
				if (m_count == kSummaryRows)
				{
					++m_overflow;
					return;
				}
				Row& row = m_rows[m_count++];
				row.hand = hand;
				row.micros = micros;
				row.source = source;
				row.count = 1;
			}

			const Row* Find(const int hand, const unsigned short micros, const Source source) const
			{
				for (int i = 0; i < m_count; ++i)
				{
					const Row& row = m_rows[i];
					if (row.hand == hand && row.micros == micros && row.source == source)
					{
						return &row;
					}
				}
				return nullptr;
			}

			const Row* RowAt(const int index) const
			{
				return index >= 0 && index < m_count ? &m_rows[index] : nullptr;
			}

			int RowCount() const
			{
				return m_count;
			}

			int Overflow() const
			{
				return m_overflow;
			}

			int Total() const
			{
				return m_total;
			}

		private:
			Row m_rows[kSummaryRows];
			int m_count = 0;
			int m_overflow = 0;
			int m_total = 0;
		};

		// Which hand a device index is: 1 left, 0 right, -1 neither. An unknown controller matches
		// nothing, so an invalid device never reads as a hand.
		inline int HandFromDevice(const unsigned int device, const unsigned int leftDevice, const unsigned int rightDevice)
		{
			if (device == kInvalidDevice)
			{
				return -1;
			}
			if (leftDevice != kInvalidDevice && device == leftDevice)
			{
				return 1;
			}
			if (rightDevice != kInvalidDevice && device == rightDevice)
			{
				return 0;
			}
			return -1;
		}
	}
}
