#pragma once

// What to do while a weapon that was just grab-equipped has not actually come out of its sheath.
//
// WHY THIS EXISTS. EnsureWeaponDrawn calls DrawSheatheWeapon(true) and returns "a draw was
// REQUESTED" - not "the weapon is out". Nothing ever checked that it landed, and the request can
// be swallowed: this mod already knew that about crossbows, whose cocking sequence eats a
// same-frame draw and leaves them "equipped but sheathed (invisible)", and gave crossbows alone a
// retrying draw. Every other weapon kept the single shot, so when anything else swallowed it - an
// animation, a busy graph, landing mid-attack - the weapon stayed equipped and sheathed, which in
// VR is a weapon that is simply not in your hand.
//
// WHAT THIS IS NOT. It is not the continuous draw/sheathe upkeep that was removed for good reason:
// that blocked sneak and jump for seconds after a grab and spammed equip audio when dual wielding.
// Neither came from re-asking for a draw that FAILED; both came from re-asking for draws that had
// already succeeded. So the rule here is only ever "ask again while it is genuinely not out, and
// stop the instant it is" - kDrawn ends the watch, and a weapon already out is never asked at all.
//
// SECONDS, NOT TASK REQUEUES. The retry interval is real elapsed time fed in from the HIGGS
// pre-physics callback. Building a wait out of SKSETaskInterface requeues does not wait: a task
// added from inside a task drains in the SAME frame, so a "wait ten frames" loop spends no time
// at all and fires every retry back to back, giving the engine no chance to answer the first one.
//
// No engine types in here, so tools_draw_watch_tests.cpp can drive it outside the game;
// DrawWatch.cpp feeds it whether the player's weapon is out and settled.

namespace SwapDropAndHoldRedux
{
	namespace DrawWatch
	{
		struct Config
		{
			// A draw the engine accepted still plays an animation, so a weapon is not late just
			// because it is not out on the next frame. Long enough that an ordinary draw is never
			// re-requested, short enough that a swallowed one is not a visible wait.
			float retryAfterSeconds = 0.25f;

			// And the same again between re-requests, for the same reason: a request needs time
			// to be answered before asking is worth anything.
			float retryEverySeconds = 0.25f;

			// Six is the crossbow path's own budget, kept. Past this the weapon is being held
			// sheathed by something this mod should not be fighting.
			int maxRetries = 6;

			// Stop watching a weapon that never comes out. Well past the point where asking
			// again could help, so the log still says what happened.
			float giveUpSeconds = 5.0f;
		};

		enum class Step
		{
			kNone,      // nothing to do this frame
			kRetryDraw, // still sheathed: ask for the draw again (never when it is already out)
			kDrawn,     // the weapon is out and settled, and the watch is over
			kGaveUp     // still not out after giveUpSeconds, and the watch is over
		};

		class Watch
		{
		public:
			// A weapon was just grab-equipped and a draw has been requested for it.
			void Begin()
			{
				*this = Watch{};
				m_active = true;
			}

			bool Active() const { return m_active; }

			// One frame. drawnAndSettled: the player's draw state is DRAWN and settled.
			//
			// ⚠ It must be the strict test - state exactly 3. IsWeaponDrawn() is state >= 3, so it
			// stays true through kWantToSheathe(4) and kSheathing(5), and a draw issued during
			// those cancels the sheathe instead of following it.
			Step Update(const bool drawnAndSettled, const float deltaTime, const Config& config)
			{
				if (!m_active)
				{
					return Step::kNone;
				}

				++m_frames;
				m_seconds += deltaTime;

				// First, and before anything else can fire: the weapon is out. Nothing is asked
				// for again once this is true - that is the whole point.
				if (drawnAndSettled)
				{
					m_active = false;
					return Step::kDrawn;
				}

				if (m_seconds >= config.giveUpSeconds)
				{
					m_active = false;
					return Step::kGaveUp;
				}

				// Out of re-requests is not out of hope: the engine may still finish a draw it
				// accepted, so the watch keeps looking until it gives up.
				if (m_retries < config.maxRetries && m_seconds >= NextRetryAt(config))
				{
					++m_retries;
					m_lastRetrySeconds = m_seconds;
					return Step::kRetryDraw;
				}

				return Step::kNone;
			}

			// Time and frames since Begin, as of the last Update.
			float Seconds() const { return m_seconds; }
			int Frames() const { return m_frames; }

			// How many times the draw has been asked for again.
			int Retries() const { return m_retries; }

		private:
			float NextRetryAt(const Config& config) const
			{
				return m_retries == 0
					? config.retryAfterSeconds
					: m_lastRetrySeconds + config.retryEverySeconds;
			}

			bool m_active = false;
			int m_frames = 0;
			int m_retries = 0;
			float m_seconds = 0.0f;
			float m_lastRetrySeconds = 0.0f;
		};
	}
}
