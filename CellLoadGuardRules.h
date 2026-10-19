#pragma once

// Standing the mod down across a cell load, so a weapon in your hand is not dropped or lost on the way
// through a door.
//
// WHY. Going through a load door tears the player's 3D down and builds it again in the new cell. For a
// few frames either side of that, everything this mod reads is unreliable: the controller may read open
// when the hand never moved, the weapon's node and collision body come and go, and HIGGS's hold is
// rebuilt. Acting on any of it can drop the weapon on a floor that no longer exists. The mod cannot
// improve those readings, so it stops using them: it locks itself exactly as the lock chord does, and
// takes itself back a moment after the new cell is up.
//
// WHEN. SkyrimVR puts a loading screen up for every door transition, fast travel and save load, and this
// mod already listens to menus opening and closing. The screen going up is the earliest moment anything
// can be done - and the whole load happens behind it, with the game frozen - so the lock is in place
// before the transition rather than after it. The screen coming down starts the hold into the new cell.
//
// The first frame in a new cell is worth seconds of real time, so a step is never counted as more than
// kMaxStepSeconds: without that, one frame would swallow the whole hold and the lock would end before
// the cell had settled. And a loading screen that never comes down must not lock the mod for ever, so
// kSafetySeconds gives it back regardless.
//
// No engine types in here, so tools_cell_guard_tests.cpp can drive it outside the game.

namespace SwapDropAndHoldRedux
{
	namespace CellLoadGuard
	{
		// The most one frame may count for. The first frame in a new cell is worth seconds.
		constexpr float kMaxStepSeconds = 0.25f;
		constexpr float kMinHoldSeconds = 0.0f;
		constexpr float kMaxHoldSeconds = 10.0f;
		// A loading screen that never closes gives the mod back after this, rather than locking for ever.
		constexpr float kSafetySeconds = 60.0f;

		enum class Step
		{
			kNothing,
			kLocked,   // report once: the mod has stood down
			kUnlocked, // report once: the mod has itself back
		};

		inline float ClampHold(const float seconds)
		{
			if (!(seconds > kMinHoldSeconds))
			{
				return kMinHoldSeconds;
			}
			return seconds > kMaxHoldSeconds ? kMaxHoldSeconds : seconds;
		}

		class Guard
		{
		public:
			// The loading screen went up.
			Step LoadingStarted()
			{
				m_loading = true;
				m_hold = 0.0f;
				m_holdPending = false;
				if (m_locked)
				{
					return Step::kNothing;
				}
				m_locked = true;
				m_total = 0.0f;
				return Step::kLocked;
			}

			// The loading screen came down. The mod stays locked for the hold, which starts on the next
			// frame - the first one actually spent in the new cell.
			Step LoadingFinished()
			{
				if (!m_loading)
				{
					return Step::kNothing; // a screen we never saw go up is not ours to answer for
				}
				m_loading = false;
				m_holdPending = true;
				return Step::kNothing;
			}

			Step Update(const float deltaTime, const float holdSeconds)
			{
				if (!m_locked)
				{
					return Step::kNothing;
				}

				const float step = deltaTime > kMaxStepSeconds ? kMaxStepSeconds : (deltaTime > 0.0f ? deltaTime : 0.0f);
				m_total += step;
				if (m_total >= kSafetySeconds)
				{
					Clear();
					return Step::kUnlocked;
				}
				if (m_loading)
				{
					return Step::kNothing; // the screen is still up; the hold has not started
				}

				if (m_holdPending)
				{
					m_holdPending = false;
					m_hold = ClampHold(holdSeconds);
				}
				m_hold -= step;
				if (m_hold <= 0.0f)
				{
					Clear();
					return Step::kUnlocked;
				}
				return Step::kNothing;
			}

			void Clear()
			{
				m_loading = false;
				m_holdPending = false;
				m_locked = false;
				m_hold = 0.0f;
				m_total = 0.0f;
			}

			bool Locked() const { return m_locked; }
			bool Loading() const { return m_loading; }
			float HoldLeft() const { return m_hold; }

		private:
			bool m_loading = false;     // a loading screen is up
			bool m_holdPending = false; // the hold starts on the next frame, in the new cell
			bool m_locked = false;
			float m_hold = 0.0f;
			float m_total = 0.0f;       // how long the mod has been locked, for the safety cut-off
		};
	}
}
