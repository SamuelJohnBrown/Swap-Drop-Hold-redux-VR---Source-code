#pragma once

// Gesture rules with no engine or OpenVR types in them, so tools_tap_tests.cpp can
// drive them outside the game. GripHold feeds them real controller reads; what a tap,
// a double tap or a chord IS gets decided here and nowhere else.

namespace SwapDropAndHoldRedux
{
	namespace Gestures
	{
		// Simple mode's drop gesture: a quick double tap of grip. The weapon is let go
		// when the grip OPENS after the second tap, never on the second press - a press
		// would put the weapon on the floor inside a closed hand, and HIGGS takes hold of
		// anything a closed hand is touching.
		struct DoubleTapConfig
		{
			// Longest time from the start of the first press to the start of the second.
			float windowSeconds = 0.40f;

			// A press held longer than this is holding, not tapping.
			float maxTapHoldSeconds = 0.35f;

			// An opening shorter than this is flicker, not a release. This mod's own logs
			// caught a grip reading open for three frames (33-42ms) in the middle of a
			// hold nobody loosened, and read raw that is a tap.
			float minOpenSeconds = 0.05f;

			// How long the final opening must last before the weapon is let go - the
			// player's release debounce. It never shortens the flicker filter, and it only
			// ever applies to the last release, so raising it cannot make the gap between
			// two taps impossible to land.
			float releaseGraceSeconds = 0.03f;
		};

		class DoubleTapDetector
		{
		public:
			// A weapon has just come into the hand. If the grip is already closed - the
			// squeeze that grabbed it, or drew it out of a holster - that press belongs to
			// taking hold, so nothing counts until the hand has opened once.
			void Bind(const bool gripClosedNow)
			{
				*this = DoubleTapDetector{};
				m_closed = gripClosedNow;
				m_ignoreUntilOpen = gripClosedNow;
				m_heldForRelease = gripClosedNow;
			}

			// One frame of the raw grip. Returns true on the single frame the weapon should
			// be let go.
			bool Update(const bool rawClosed, const float deltaTime, const DoubleTapConfig& config)
			{
				TrackRelease(rawClosed, deltaTime, config);

				if (m_firstPressAge >= 0.0f)
				{
					m_firstPressAge += deltaTime;
				}

				if (rawClosed)
				{
					if (m_closed)
					{
						// The same press, including any flicker it just rode through.
						m_pressSeconds += m_openSeconds + deltaTime;
					}
					else
					{
						m_closed = true;
						m_pressSeconds = 0.0f;
						if (m_dropPending)
						{
							// Closed again before the release settled: the player kept hold.
							m_dropPending = false;
							ResetSequence();
						}
						BeginPress(config);
					}
					m_openSeconds = 0.0f;
					return false;
				}

				m_openSeconds += deltaTime;

				if (m_closed)
				{
					if (m_openSeconds < config.minOpenSeconds)
					{
						return false; // could still be flicker
					}

					m_closed = false;
					EndPress(config);
				}

				if (m_dropPending)
				{
					const float grace = config.releaseGraceSeconds > config.minOpenSeconds
						? config.releaseGraceSeconds
						: config.minOpenSeconds;
					if (m_openSeconds < grace)
					{
						return false;
					}

					m_dropPending = false;
					ResetSequence();
					return true;
				}

				if (m_taps == 1 && m_firstPressAge > config.windowSeconds)
				{
					ResetSequence(); // a lone tap whose second never came
				}
				return false;
			}

			// M3.0.0-test14, Simple mode's hand switch: true on the single Update in which the grip, after being
			// closed, has stayed open for the release debounce (never less than the flicker filter). Unlike the
			// double tap it counts the press that took hold of the weapon - letting go of a weapon the other hand
			// is holding is exactly that press ending. It never changes what Update returns.
			bool Released() const
			{
				return m_released;
			}

		private:
			void TrackRelease(const bool rawClosed, const float deltaTime, const DoubleTapConfig& config)
			{
				m_released = false;
				if (rawClosed)
				{
					m_heldForRelease = true;
					m_releaseOpenSeconds = 0.0f;
					return;
				}
				if (!m_heldForRelease)
				{
					return;
				}
				m_releaseOpenSeconds += deltaTime;
				const float need = config.releaseGraceSeconds > config.minOpenSeconds
					? config.releaseGraceSeconds
					: config.minOpenSeconds;
				if (m_releaseOpenSeconds >= need)
				{
					m_released = true;
					m_heldForRelease = false;
				}
			}

			void BeginPress(const DoubleTapConfig& config)
			{
				if (m_ignoreUntilOpen)
				{
					return;
				}

				if (m_taps == 1 && m_firstPressAge >= 0.0f && m_firstPressAge <= config.windowSeconds)
				{
					m_pressIsSecond = true;
					return;
				}

				m_taps = 0;
				m_pressIsSecond = false;
				m_firstPressAge = 0.0f;
			}

			void EndPress(const DoubleTapConfig& config)
			{
				if (m_ignoreUntilOpen)
				{
					m_ignoreUntilOpen = false;
					ResetSequence();
					return;
				}

				if (m_pressSeconds > config.maxTapHoldSeconds)
				{
					ResetSequence(); // held, not tapped
					return;
				}

				if (m_pressIsSecond)
				{
					m_dropPending = true;
					return;
				}

				m_taps = 1;
			}

			void ResetSequence()
			{
				m_taps = 0;
				m_pressIsSecond = false;
				m_firstPressAge = -1.0f;
			}

			bool m_closed = false;          // debounced: flicker shorter than minOpenSeconds never opens it
			bool m_ignoreUntilOpen = false; // the press that took hold of the weapon
			bool m_pressIsSecond = false;   // this press started inside the window after a first tap
			bool m_dropPending = false;     // second tap released; waiting out the release debounce
			int m_taps = 0;                 // completed first taps in the current sequence (0 or 1)
			float m_pressSeconds = 0.0f;
			float m_openSeconds = 0.0f;
			float m_firstPressAge = -1.0f;  // seconds since the first press began; < 0 = no sequence
			bool m_heldForRelease = false;     // closed since the last release (or bound closed)
			bool m_released = false;           // the release completed on the last Update
			float m_releaseOpenSeconds = 0.0f; // how long the grip has been open since it was last closed
		};

		// A trigger tap counts toward a Grip Hold chord when grip is held on that hand, or
		// when that hand holds a weapon grip is not holding - equipped from the inventory,
		// locked, or at a holster - because there is nothing to squeeze without taking hold
		// of it. A weapon grip IS holding has its grip held, so in play: a weapon in hand
		// needs only the taps, an empty hand needs grip as well.
		inline bool ChordTapCounts(const bool gripHeld, const bool weaponInHand, const bool weaponGripBound)
		{
			return gripHeld || (weaponInHand && !weaponGripBound);
		}

		// One hand's run of trigger taps, judged by its total once it has ended, never on
		// reaching a count - the lock and blacklist chords share their opening taps.
		struct ChordRun
		{
			int clicks = 0;
			float sinceLastClick = -1.0f; // < 0 = no run pending
			bool everyTapCounted = false;

			void Click(const bool tapCounts)
			{
				if (sinceLastClick < 0.0f)
				{
					clicks = 0;
					everyTapCounted = true;
				}

				clicks += 1;
				sinceLastClick = 0.0f;
				everyTapCounted = everyTapCounted && tapCounts;
			}

			// Advances the gap since the last tap. Returns true on the frame the run ends,
			// with its total and whether every tap in it counted.
			bool Tick(const float deltaTime, const float windowSeconds, int& outClicks, bool& outQualified)
			{
				if (sinceLastClick < 0.0f)
				{
					return false;
				}

				sinceLastClick += deltaTime;
				if (sinceLastClick <= windowSeconds)
				{
					return false;
				}

				outClicks = clicks;
				outQualified = everyTapCounted;
				Cancel();
				return true;
			}

			void Cancel()
			{
				clicks = 0;
				sinceLastClick = -1.0f;
				everyTapCounted = false;
			}
		};
	}
}
