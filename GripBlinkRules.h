#pragma once

// Weapon Throw VR will not start a throw with a hand whose grip has stayed down since that hand picked
// its weapon up. It reads the controllers itself (through SkyrimVR Tools; it has no HIGGS code), and a
// grip that went down while the hand had nothing to throw keeps that hand out of throwing until the
// grip comes back up. Grip Hold keeps the grip down for as long as it holds the weapon, so a weapon
// picked up in Grip Hold could never be thrown.
//
// Measured 2026-09-15 on M2.2.0-test1: 10 of 10 throws with a weapon picked up and still gripped never
// reached Weapon Throw VR, in both hands. In Simple mode the same kind of pickup threw once the grip had
// opened for a moment, and weapons that arrived without a pickup (inventory, holster draw, Weapon Throw
// VR's own return) threw with the grip held.
//
// So right after Grip Hold picks a weapon up, every other reader of the controller is shown that grip
// OPEN for kSeconds while Grip Hold keeps reading the real hand (MenuGrip.h does the showing), and the
// HIGGS hand switched off for the pickup stays off until it is over (HiggsReleaseRules.h). No engine
// types in here, so tools_grip_blink_tests.cpp can check it outside the game.

namespace SwapDropAndHoldRedux
{
	namespace GripBlink
	{
		// How long other mods see the grip open: at least two frames at any refresh rate, and over well
		// inside the time a HIGGS hand stays switched off for a pickup (HiggsRelease::Config).
		constexpr float kSeconds = 0.10f;

		// ---- Weapon Throw VR's hold button, for the log (M3.0.0-test8, demoted in test9) ----------
		//
		// Weapon Throw VR has a hold button per hand, set in its own menu and kept in two globals of
		// WeaponThrowVR.esp: WT_HoldButton (its log calls it "HoldButton") and WT_HoldButtonSecondary.
		// A global holds the menu's option: 0 Trigger (its default), 1 Grip, 2 A, 3 System,
		// 4 Application / B, 5 Touchpad.
		//
		// ⚠ test8 blinked only for a Grip button, on the reasoning that another button never reads the
		// grip. WRONG, measured: with the player's Trigger button and no blink, the 2026-09-22 test8 log
		// has 40 trigger presses with a picked-up weapon in hand and NOT ONE throw attempt from Weapon
		// Throw VR, where test7 (blink on) had them from both hands. Weapon Throw VR keeps out a hand whose
		// grip has been down since it was empty, whatever button throws. So the blink runs for every
		// button again, and the button is only logged. What test8 was really fixing - the game putting a
		// LEFT-hand weapon away at the re-press, because Skyrim VR binds Ready Weapon to the Oculus LEFT
		// grip (controlmapvr.txt) - is handled where it belongs since test9: the game is handed the real
		// grip (GameGripRules.h) while the other mods still see the blink.
		constexpr int kThrowButtonUnread = -1; // the global was not found
		constexpr int kThrowButtonTrigger = 0;
		constexpr int kThrowButtonGrip = 1;

		// A global's value as the button it names. Rounded, because it is a float.
		inline int ThrowButtonFromGlobal(const bool found, const float value)
		{
			// Also false for not-a-number, so that falls out as unread too.
			if (!found || !(value > -100.0f && value < 100.0f))
			{
				return kThrowButtonUnread;
			}
			return static_cast<int>(value < 0.0f ? value - 0.5f : value + 0.5f);
		}

		// For the log: the name Weapon Throw VR's menu gives the button.
		inline const char* ThrowButtonName(const int button)
		{
			switch (button)
			{
			case kThrowButtonUnread: return "not found";
			case kThrowButtonTrigger: return "Trigger";
			case kThrowButtonGrip: return "Grip";
			case 2: return "A";
			case 3: return "System";
			case 4: return "Application / B";
			case 5: return "Touchpad";
			default: return "an option it does not have";
			}
		}

		struct PickupFacts
		{
			bool gripHoldMode = false;   // the weapon mode is Grip Hold or Simple...
			bool simpleMode = false;     // ...and this says which
			bool throwModLoaded = false; // WeaponThrowVR.dll is loaded
			int throwButton = kThrowButtonUnread; // its hold button for this hand - for the log ONLY (above)
			bool gripClosed = false;     // the hand that picked the weapon up is closed
			bool higgsHandOff = false;   // that hand's HIGGS hand is still switched off for the pickup
			bool holsterInReach = false; // VRIK says a holster is in reach of that hand
			bool menuOpen = false;       // a menu that takes input is open
		};

		enum class Decision
		{
			kBlink,       // show the grip released
			kNotGripHold, // Legacy or Simple: nothing keeps the grip down there
			kNoThrowMod,  // Weapon Throw VR is not loaded, so there is nobody to show it to
			kGripOpen,    // the grip is open already, so nothing is waiting for it
			kMenuOpen,    // a menu owns the grip
			kAtHolster,   // the grip coming back could make VRIK holster the weapon
			kHiggsHandOn, // HIGGS's hand is back on already and could act on the grip coming back
		};

		inline Decision DecideAfterPickup(const PickupFacts& facts)
		{
			if (!facts.gripHoldMode || facts.simpleMode)
			{
				return Decision::kNotGripHold;
			}
			if (!facts.throwModLoaded)
			{
				return Decision::kNoThrowMod;
			}
			if (!facts.gripClosed)
			{
				return Decision::kGripOpen;
			}
			if (facts.menuOpen)
			{
				return Decision::kMenuOpen;
			}
			if (facts.holsterInReach)
			{
				return Decision::kAtHolster;
			}
			if (!facts.higgsHandOff)
			{
				return Decision::kHiggsHandOn;
			}
			return Decision::kBlink;
		}

		// When a window of `seconds` starting at `nowTicks` ends. A window with no length ends at once.
		inline long long EndTicks(const long long nowTicks, const float seconds, const long long ticksPerSecond)
		{
			if (seconds <= 0.0f)
			{
				return nowTicks;
			}
			return nowTicks + static_cast<long long>(static_cast<double>(seconds) * static_cast<double>(ticksPerSecond));
		}

		// Whether the window ending at `endTicks` is still showing at `nowTicks`.
		inline bool Active(const long long nowTicks, const long long endTicks)
		{
			return nowTicks < endTicks;
		}

		// Grip-hold's own grip read goes through the same hook that hides the grip, so while a window
		// shows it takes the real grip instead - and for this long after, because a read the hook hid
		// just before the window ended can arrive just after it and must not look like an open hand.
		constexpr float kPhysicalGraceSeconds = 0.05f;

		// Whether grip-hold should still take the real grip at `nowTicks`, for a window ending at
		// `endTicks` (0: never started) with `graceTicks` of grace.
		inline bool PhysicalGripStillMasked(const long long nowTicks, const long long endTicks, const long long graceTicks)
		{
			return endTicks != 0 && nowTicks < endTicks + graceTicks;
		}

		// ---- the blink after a DRAW from a holster ------------------------------------------
		//
		// A weapon drawn from a VRIK holster arrives with the grip closed since before the draw, so
		// Weapon Throw VR ignores it exactly as it ignored a pickup: M2.2.8-test1's log has 13 draws,
		// 0 blinks and 0 throws. A draw cannot blink at once, though. The hand is ON the slot, and
		// VRIK buzzes again within a second because the freed slot would take the weapon straight
		// back - a shown release-and-press there is the holster gesture. So the blink waits until the
		// hand has left: no longer standing aside, and either past the same leave distance the
		// stand-aside uses or a few seconds on. The hand opening on the way cancels it - Weapon Throw
		// VR saw a real release then.
		constexpr float kDrawBlinkMaxWaitSeconds = 3.0f;

		struct DrawFacts
		{
			bool gripStillClosed = false; // armed and grip down since the draw
			bool standingAside = false;   // Grip hold is standing aside at a holster with this hand
			float distanceFromDraw = 0.0f;
			float leaveDistance = 0.0f;   // VrikPulse::LeaveDistance(activation distance)
			float secondsSinceDraw = 0.0f;
		};

		enum class DrawStep
		{
			kWait,
			kBlink,
			kCancel,
		};

		inline DrawStep DecideAfterDraw(const DrawFacts& facts)
		{
			if (!facts.gripStillClosed)
			{
				return DrawStep::kCancel;
			}
			if (facts.standingAside)
			{
				return DrawStep::kWait;
			}
			if (facts.distanceFromDraw >= facts.leaveDistance || facts.secondsSinceDraw >= kDrawBlinkMaxWaitSeconds)
			{
				return DrawStep::kBlink;
			}
			return DrawStep::kWait;
		}

		// ---- the re-press waits until the hand is clear of anything VR Climbing climbs (M3.0.0-test7) ----
		//
		// VR Climbing (its 2026-01 build, read from its own DLL) starts a climb on a grip PRESS when one of
		// six world-axis rays of kVrClimbingRayUnits from the controller's wand node hits, FIRST, a climbable
		// layer: static, animated static, trees, props, terrain or ground. The blink's re-press IS a press,
		// and a weapon picked up off a table leaves the hand on the table - 6 of the 8 climbs in the
		// 2026-09-21 log started 0.1s after a pickup, each with a surface within reach of the hand. So while
		// the grip is being shown released and the hand is within kHoldBackReachUnits of a climbable surface,
		// the window is kept open a step at a time; the re-press comes once the hand is clear. The user's
		// choice, over hiding a weapon hand's grip altogether, because this keeps Weapon Throw VR's Grip
		// hold button working. A hand that later touches a wall with its grip held can still climb.
		constexpr float kVrClimbingRayUnits = 6.75f;   // VR Climbing's own ray, for reference
		// Ours is longer: the hand keeps moving between this check and the press other mods read, and
		// a few centimetres of margin cost nothing but a slightly later re-press.
		constexpr float kHoldBackReachUnits = 12.0f;
		// Each check near a surface keeps the window this far ahead of now: more than a frame at every
		// refresh rate, so an ordinary frame never lets it lapse, and short enough that when the checks
		// stop (a menu, a load) it ends on its own and the old 0.10s behaviour is what remains.
		constexpr float kHoldBackStepSeconds = 0.05f;
		// Never longer than this after the blink began, whatever the probe says: a probe that is wrong
		// must not be able to hide a grip for good.
		constexpr float kHoldBackMaxSeconds = 10.0f;

		// The layers VR Climbing climbs - its layer jump table covers 1-17 and sends these six to "grab".
		inline bool IsClimbableLayer(const int layer)
		{
			return layer == 1 || layer == 2 || layer == 9 || layer == 10 || layer == 13 || layer == 17;
		}

		// One of the six rays: whether it hit anything, the FIRST thing it hit, and how far away.
		struct ProbeRay
		{
			bool hit = false;
			int layer = -1;
			float distUnits = 0.0f;
		};

		// VR Climbing's own rule: a ray counts only by its FIRST hit, and only if that is climbable and
		// within reach. The nearest such hit is reported.
		inline bool NearClimbable(const ProbeRay* rays, const int count, const float reachUnits, int& layerOut, float& distOut)
		{
			bool found = false; // not "near": <windows.h> defines that as nothing
			for (int i = 0; rays && i < count; ++i)
			{
				const ProbeRay& r = rays[i];
				if (!r.hit || !IsClimbableLayer(r.layer) || !(r.distUnits <= reachUnits))
				{
					continue;
				}
				if (!found || r.distUnits < distOut)
				{
					layerOut = r.layer;
					distOut = r.distUnits;
				}
				found = true;
			}
			return found;
		}

		struct HoldBackFacts
		{
			bool enabled = false;          // bBlinkWaitsForClearHand
			bool showing = false;          // the blink window is still showing the grip released
			bool gripClosed = false;       // the real grip is still closed
			bool probeRan = false;         // the surface probe could look this frame
			bool nearClimbable = false;    // and found a climbable surface within reach
			float secondsSinceBlink = 0.0f;
		};

		enum class HoldBack
		{
			kLetItEnd, // nothing to hold back: the window ends on its own
			kExtend,   // keep the grip shown released a step longer
			kGiveUp,   // near a surface, but past the limit: let it end anyway (say so once)
		};

		inline HoldBack DecideHoldBack(const HoldBackFacts& facts)
		{
			if (!facts.enabled || !facts.showing || !facts.gripClosed || !facts.probeRan || !facts.nearClimbable)
			{
				return HoldBack::kLetItEnd;
			}
			if (facts.secondsSinceBlink >= kHoldBackMaxSeconds)
			{
				return HoldBack::kGiveUp;
			}
			return HoldBack::kExtend;
		}

		// The window's end once held back a step at `nowTicks`: a step ahead of now, and never earlier
		// than it already was - holding back never shortens the blink it was given.
		inline long long HoldBackEndTicks(const long long nowTicks, const long long endTicks, const long long ticksPerSecond)
		{
			const long long wanted = EndTicks(nowTicks, kHoldBackStepSeconds, ticksPerSecond);
			return wanted > endTicks ? wanted : endTicks;
		}
	}
}
