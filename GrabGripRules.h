#pragma once

// When a sensed touch (GrabPoseRules.h, GrabPose.cpp) is worth telling a grip driver about, and exactly
// what it is told (SdhGrabGripInterface.h).
//
// The touch is sensed in HIGGS's grabbed callback and the weapon is equipped some frames later - up to
// ten seconds later with "Hold before equipping" - by a task that knows only a form id. So the record is
// kept per VR hand and judged again at the equip: it must be for the weapon being equipped, recent, and a
// measurement worth trusting. Anything else sends nothing, and the driver holds the weapon the natural
// way, exactly as before 3.0.0.
//
// No engine types in here, so tools_grab_grip_tests.cpp can drive it outside the game.

#include "GrabPoseRules.h"
#include "SdhGrabGripInterface.h"

namespace SwapDropAndHoldRedux
{
	namespace GrabGrip
	{
		// What the sensor found, as plain values.
		struct Touch
		{
			bool valid = false;
			unsigned int refID = 0;
			unsigned int formID = 0;
			bool isLeftVRController = false;
			int weaponType = 0;       // TESObjectWEAP type
			int longAxis = 0;         // 0/1/2 in the collision shape's own frame
			bool axisUsable = false;
			bool axisAmbiguous = false;
			float alongU = 0.0f;      // palm along the long axis, -1 .. +1, not yet clamped
			float hiltU = 0.0f;       // the shape's origin on the same scale
			float lengthM = 0.0f;
			float offAxisM = 0.0f;    // palm distance from the weapon's centre line
			int tipSign = 0;          // +1 tip at the high end, -1 low end, 0 unknown
			bool pulled = false;
			bool warped = false;
			float reversedAngleDeg = 0.0f;
			bool reversedVerdict = false; // within the cone of dead backwards
			bool flipDecided = false;
			int flipAxis = 0;
			bool handOff = false;     // M3.0.0-test14: the other hand took an equipped weapon over (no loose ref)
		};

		enum class Why
		{
			kSend,
			kNoRecord,
			kOtherWeapon,
			kStale,
			kNotMelee,
			kPulled,
			kWarped,
			kNoAxis,
			kAmbiguousAxis,
			kOffTheWeapon,
		};

		// The longest a touch can wait for its equip: "Hold before equipping" tops out at 10 s, and the
		// pickup and equip tasks add a few frames. Half a minute is generous and still not another pickup.
		constexpr float kMaxAgeSeconds = 30.0f;
		// A palm further than this from the weapon's centre line is a reading gone wrong, not a grip.
		constexpr float kMaxOffAxisM = 0.30f;

		// ---- M3.0.0-test14: what the weapon mode in force says ------------------------------------
		// The Simple and Grip Hold menu pages each have their own "Holding a weapon you pick up" switches.
		// Legacy has none: it keeps the ini-only bGrabGrip / bGrabGripReversed and never reverses a two-hander.
		struct Choice
		{
			bool position = true;           // Hold where you grab
			bool reversed = true;           // Reverse grip
			bool reversedTwoHanded = false; // Reverse two-handed weapons too (experimental)
			bool anywhere = false;          // Hold anywhere on the weapon: above the guard too, never through it (test15)
		};

		// weaponMode as config.h numbers it: 0 Legacy, 1 Grip hold, 2 Simple. Anything else is Legacy.
		inline Choice ChooseForMode(const int weaponMode, const Choice& simple, const Choice& gripHold, const Choice& legacy)
		{
			if (weaponMode == 2)
			{
				return simple;
			}
			if (weaponMode == 1)
			{
				return gripHold;
			}
			Choice chosen = legacy;
			chosen.reversedTwoHanded = false;
			chosen.anywhere = false;
			return chosen;
		}

		// May this weapon type be held reversed at all: one-handed melee always, two-handed melee (5 greatsword,
		// 6 battleaxe / warhammer) only when the mode allows it. Never bows, staves, crossbows or fists.
		inline bool IsReversibleMelee(const int weaponType, const bool allowTwoHanded)
		{
			return GrabPose::IsOneHandedMelee(weaponType) || (allowTwoHanded && (weaponType == 5 || weaponType == 6));
		}

		// The most basic reason wins, so the log says the thing that matters.
		inline Why Judge(const Touch& touch, const unsigned int equippedFormID, const float ageSeconds,
			const bool usePulledGrabs = false)
		{
			if (!touch.valid)
			{
				return Why::kNoRecord;
			}
			if (touch.formID != equippedFormID)
			{
				return Why::kOtherWeapon;
			}
			if (ageSeconds < 0.0f || ageSeconds > kMaxAgeSeconds)
			{
				return Why::kStale;
			}
			if (!GrabPose::IsGripMelee(touch.weaponType))
			{
				return Why::kNotMelee;
			}
			// A pull is HIGGS warping the weapon into the hand, so where the palm ends up on it is
			// chance rather than a choice. Some players would rather keep that chance than always be
			// handed the natural grip, so it is theirs to switch on. Everything below still applies:
			// allowing a pull allows a CHANCE reading, never a BROKEN one.
			if (touch.pulled && !usePulledGrabs)
			{
				return Why::kPulled;
			}
			if (touch.warped)
			{
				return Why::kWarped;
			}
			if (!touch.axisUsable)
			{
				return Why::kNoAxis;
			}
			if (touch.axisAmbiguous)
			{
				return Why::kAmbiguousAxis;
			}
			if (touch.offAxisM > kMaxOffAxisM)
			{
				return Why::kOffTheWeapon;
			}
			return Why::kSend;
		}

		// Whether a grab of this weapon may come out reversed at all, whatever the angle says: a one-handed
		// weapon, a clear long axis, a known tip (or which way is backwards is not known), not warped to the
		// hand by HIGGS - and not pulled, unless pulled grabs may reverse (bGrabGripReversePulled).
		//
		// Pulls: up to M3.0.0-test7 a pull was never reversed, on the reasoning that which way round it lands
		// is chance. The 2026-09-22 log says otherwise about how it is USED: 35 of the player's 77 grabs were
		// pulls, not one could reverse however the wrist was turned, and "turning my wrist to grab it reversed
		// does nothing" was the complaint. HIGGS's pull sets only the weapon's LINEAR velocity (hand.cpp
		// 1.10.10, the State::Pulled block): nothing turns it, so a weapon lifted off a table flies about the
		// way round it lay, and turning the hand against it before the pull is a real choice. The verdict is
		// taken against the hand as it is at the catch, exactly as for a weapon taken off a table.
		//
		// Two-handers: only with allowTwoHanded - the mode's "Reverse two-handed weapons too" (M3.0.0-test14).
		inline bool MayReverse(const int weaponType, const bool axisAmbiguous, const int tipSign, const bool pulled,
			const bool warped, const bool allowPulled, const bool allowTwoHanded = false)
		{
			return IsReversibleMelee(weaponType, allowTwoHanded) && !axisAmbiguous && tipSign != 0 && !warped &&
				(!pulled || allowPulled);
		}

		// For the log: why MayReverse said no, or "" when it said yes.
		inline const char* WhyNotReverse(const int weaponType, const bool axisAmbiguous, const int tipSign, const bool pulled,
			const bool warped, const bool allowPulled, const bool allowTwoHanded = false)
		{
			if (!IsReversibleMelee(weaponType, allowTwoHanded))
			{
				return weaponType == 5 || weaponType == 6 ? "a two-handed weapon" : "not a one-handed melee weapon";
			}
			if (axisAmbiguous)
			{
				return "its shape has no clear long axis";
			}
			if (tipSign == 0)
			{
				return "which end is the tip is unclear";
			}
			if (warped)
			{
				return "HIGGS had already moved it to the hand";
			}
			if (pulled && !allowPulled)
			{
				return "it was pulled (bGrabGripReversePulled = 0)";
			}
			return "";
		}

		// Only for a touch Judge said to send. A palm past either end holds that end. sendPosition off (the mode's
		// "Hold where you grab") leaves kHasAlong out: a driver then keeps the natural grip and only turns the
		// weapon over if kReversed says so. Reversed needs all of: the setting, a reversible weapon (two-handers
		// only with allowReversedTwoHanded), a known tip (or which way is backwards is not known), the sensor's
		// verdict - and a real touch or, with allowReversedPulled, a pull (MayReverse says why). A hand switch
		// (touch.handOff) goes out as kHandOff with no loose reference. allowAnywhere (the mode's "Hold anywhere on the
		// weapon", test15) adds kAnywhere to a position: hold it there, above the crossguard too.
		inline SdhGrabGripAPI::GrabGripEvent BuildEvent(const Touch& touch, const bool isLeftGameHand, const bool allowReversed,
			const bool allowReversedPulled = false, const bool sendPosition = true, const bool allowReversedTwoHanded = false,
			const bool allowAnywhere = false)
		{
			SdhGrabGripAPI::GrabGripEvent e;
			e.structSize = sizeof(SdhGrabGripAPI::GrabGripEvent);
			e.version = 1;
			e.weaponFormID = touch.formID;
			e.weaponRefID = touch.handOff ? 0u : touch.refID;
			e.isLeftGameHand = isLeftGameHand ? 1u : 0u;
			e.source = touch.handOff ? SdhGrabGripAPI::kHandOff : SdhGrabGripAPI::kLooseGrab;
			e.longAxisLocal = static_cast<std::uint32_t>(touch.longAxis);
			e.flipAxisLocal = static_cast<std::uint32_t>(touch.flipAxis);
			e.alongU = touch.alongU < -1.0f ? -1.0f : (touch.alongU > 1.0f ? 1.0f : touch.alongU);
			e.hiltU = touch.hiltU;
			e.lengthM = touch.lengthM;
			e.reversedAngleDeg = touch.reversedAngleDeg;
			e.palmOffAxisM = touch.offAxisM;

			e.flags = sendPosition ? static_cast<std::uint32_t>(SdhGrabGripAPI::kHasAlong) : 0u;
			if (touch.tipSign != 0)
			{
				e.flags |= SdhGrabGripAPI::kHasHiltRef;
			}
			if (allowReversed && touch.reversedVerdict && (!touch.pulled || allowReversedPulled) && touch.tipSign != 0 &&
				IsReversibleMelee(touch.weaponType, allowReversedTwoHanded))
			{
				e.flags |= SdhGrabGripAPI::kReversed;
			}
			if (touch.flipDecided)
			{
				e.flags |= SdhGrabGripAPI::kHasFlipAxis;
			}
			if (sendPosition && allowAnywhere)
			{
				e.flags |= SdhGrabGripAPI::kAnywhere;
			}
			return e;
		}

		// A position to hold it at, or a reversal to hold it with: anything less says nothing a driver can use.
		inline bool WorthSending(const SdhGrabGripAPI::GrabGripEvent& e)
		{
			return (e.flags & (SdhGrabGripAPI::kHasAlong | SdhGrabGripAPI::kReversed)) != 0;
		}
	}
}
