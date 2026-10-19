#pragma once

// When this mod picks up a weapon a HIGGS hand is holding, HIGGS is made to let go of it through
// its own path, and the hand is switched back on afterwards. No engine or HIGGS types in here, so
// tools_higgs_release_tests.cpp can check it outside the game; HiggsRelease.cpp feeds it HIGGS's
// answers every physics step.
//
// Why, from HIGGS v1.10.10's hand.cpp: picking the weapon up takes its 3D away, and a held object
// with no 3D sends the hand to Idle WITHOUT Deselect (3804-3806), so the hand keeps the weapon's
// handle as its selection. HIGGS only selects an object it finds when that handle does not
// resolve to it (2487) - so when the weapon is dropped again as the same reference, the hand
// treats it as already selected, never highlights it and never pulls it, until some other object
// is selected. A switched-off hand in a held state lets go through the path that ends in
// Deselect (2947, 3182), with or without 3D.

namespace SwapDropAndHoldRedux
{
	namespace HiggsRelease
	{
		struct Config
		{
			// A hand is switched back on as soon as HIGGS says it holds nothing, and never later
			// than this many physics steps or seconds after the pickup, whatever HIGGS says.
			int maxSteps = 30;
			float maxSeconds = 0.5f;
		};

		enum class Step
		{
			kNone,       // nothing to do this step
			kEnable,     // HIGGS let go: switch the hand back on
			kEnableLate, // HIGGS still says it holds something at the limit: switch it on anyway
		};

		// Why a hand HIGGS has already let go of stays off (M3.0.0-test12).
		enum class KeepOff
		{
			kNo,
			kBlink, // Grip Hold is still showing other mods this grip released (GripBlinkRules.h)
			kDraw,  // the weapon just put in this hand is not drawn yet
		};

		// HIGGS treats a hand as EMPTY whenever the game says no weapon is drawn (hand.cpp:4310,
		// CanHoldBasedOnWeapon returns true) - so a hand switched back on before the draw finishes is,
		// for a moment, a free hand with a weapon in it, and then flips to a weapon hand. The 2026-09-23
		// test11 log: every LEFT pickup (23 of 23) was drawn 0.18-0.20s after the equip while the hand came
		// back at 0.11s, the blink's end - a 70-85ms window each time; every RIGHT pickup (18 of 18) was
		// drawn at 0.04-0.08s, before the hand came back, and never had one. The user: the hand stutters
		// and there is a quick double vibration at pickups, "mainly on the left side".
		inline KeepOff WhyKeepOff(const bool showingReleased, const bool weaponInThisHand, const bool weaponDrawn)
		{
			if (showingReleased)
			{
				return KeepOff::kBlink;
			}
			if (weaponInThisHand && !weaponDrawn)
			{
				return KeepOff::kDraw;
			}
			return KeepOff::kNo;
		}

		class Guard
		{
		public:
			// Right before a pickup. higgsHoldsIt: HIGGS says this hand holds the reference being
			// picked up. alreadyDisabled: the hand is switched off right now. Returns true when
			// the hand must be switched off now.
			bool Begin(const bool higgsHoldsIt, const bool alreadyDisabled)
			{
				if (!higgsHoldsIt)
				{
					return false;
				}

				if (m_active)
				{
					// Still off from an earlier pickup and still this mod's to switch back on:
					// wait for HIGGS to let go of this one too.
					m_steps = 0;
					m_seconds = 0.0f;
					m_keptOff = false;
					m_keptOffForDraw = false;
					return false;
				}

				if (alreadyDisabled)
				{
					return false; // something else switched it off, and it switches it back on
				}

				m_active = true;
				m_steps = 0;
				m_seconds = 0.0f;
				m_keptOff = false;
				m_keptOffForDraw = false;
				return true;
			}

			// Once per physics step. higgsStillHolding: HIGGS says the hand holds something. Only
			// a HIGGS update changes that, and every update after the switch-off lets go, so the
			// first step it says no is the step to switch the hand back on - unless keepOff asks
			// for the hand to stay off a little longer: Grip Hold is still showing other mods the
			// grip released after this pickup (GripBlinkRules.h), and a hand back on could act on
			// the grip coming back. The limits apply either way.
			Step Update(const bool higgsStillHolding, const bool keepOff, const float deltaTime, const Config& config)
			{
				if (!m_active)
				{
					return Step::kNone;
				}

				++m_steps;
				m_seconds += deltaTime;

				if (!higgsStillHolding)
				{
					if (!keepOff)
					{
						m_active = false;
						return Step::kEnable;
					}
					m_keptOff = true;
				}

				if (m_steps >= config.maxSteps || m_seconds >= config.maxSeconds)
				{
					m_active = false;
					return higgsStillHolding ? Step::kEnableLate : Step::kEnable;
				}

				return Step::kNone;
			}

			// The same, with nothing asking the hand to stay off.
			Step Update(const bool higgsStillHolding, const float deltaTime, const Config& config)
			{
				return Update(higgsStillHolding, false, deltaTime, config);
			}

			// test12: the same, told WHY the hand should stay off, so the log can say it waited for the draw.
			Step Update(const bool higgsStillHolding, const KeepOff why, const float deltaTime, const Config& config)
			{
				if (m_active && !higgsStillHolding && why == KeepOff::kDraw)
				{
					m_keptOffForDraw = true;
				}
				return Update(higgsStillHolding, why != KeepOff::kNo, deltaTime, config);
			}

			// Before a game loads. Returns true when the hand is still this mod's to switch back on.
			bool Restore()
			{
				const bool owed = m_active;
				*this = Guard{};
				return owed;
			}

			bool Active() const { return m_active; }
			int Steps() const { return m_steps; }
			float Seconds() const { return m_seconds; }
			// HIGGS had let go, but the hand was kept off for a grip release shown to other mods.
			bool KeptOff() const { return m_keptOff; }
			// ...and, at least for a while, because the weapon put in it was not drawn yet (test12).
			bool KeptOffForDraw() const { return m_keptOffForDraw; }

		private:
			bool m_active = false;
			int m_steps = 0;
			float m_seconds = 0.0f;
			bool m_keptOff = false;
			bool m_keptOffForDraw = false;
		};
	}
}
