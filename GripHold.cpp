#include "GripHold.h"
#include "HapticCensus.h"
#include "VrikPulseRules.h"

#include "CellLoadGuard.h"
#include "EmbedPins.h"
#include "Engine.h"
#include "Helper.h"

#include <skse64/PapyrusActor.h>
#include "OppositeHandGrabLogger.h"
#include "SilentEquip.h"
#include "ThrowMomentum.h"
#include "TrackedWeapons.h"
#include "TriggerHoldLogger.h"
#include "WeaponGrabActivator.h"
#include "Exclusions.h"
#include "GripGestures.h"
#include "GripTouch.h"
#include "GripTouchRules.h"
#include "GameGripGuard.h"
#include "GripBlinkRules.h"
#include "GrabPose.h"
#include "HandSwitchRules.h"
#include "HiggsRelease.h"
#include "LooseHoldRules.h"
#include "MenuGrip.h"
#include "VrikHolsters.h"
#include "VrikHolsterApi.h"
#include "VrikHolsterRules.h"
#include "WeaponThrowButtons.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameObjects.h>
#include <skse64/GameReferences.h>
#include <skse64/GameVR.h>
#include <skse64/gamethreads.h>
#include <skse64_common/Relocation.h>
#include <skse64_common/Utilities.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace SwapDropAndHoldRedux
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		// Configurable via GripButtonBits: some setups do not grab with the button
		// OpenVR calls "grip", and reading a hand as open when it is closed drops the
		// weapon.
		#define GRIP_BUTTON_MASK (gripButtonMask)
		static const uint64_t TRIGGER_BUTTON_MASK = (1ull << 33);

		// HUD notification ("%s" bottom-left message). SkyrimVR 1.4.15: address-library
		// id 52050 -> RVA 0x908170, verified against the decompilation and the installed
		// version-1-4-15-0.csv (signature: message, soundEditorID, cancelIfAlreadyQueued).
		typedef void (*ShowHUDMessage_t)(const char* message, const char* soundEditorID, bool cancelIfQueued);
		static RelocAddr<ShowHUDMessage_t> ShowHUDMessage(0x00908170);

		// Per VR controller (matches how grips are physically pressed; game hands are
		// resolved through GetTrackedWeaponForVRController like the trigger-hold gesture).
		struct GripHand
		{
			UInt32 boundFormID = 0;   // tracked item this controller currently owns
			bool armed = false;        // grip seen down while bound; a release may act
			bool gripDown = false;     // previous poll
			bool dropQueued = false;   // release dispatched, waiting for the task to land
			// A newly bound item stays locked to the hand (vanilla sticky) until the
			// grip has been seen OPEN and then pressed again. Without this, the very
			// grip press used to pull an item out of the inventory arms it, and it
			// falls on the floor the instant the hand opens.
			bool needsFreshGripPress = false;
			bool reportedTriggerStandDown = false;
			float pendingDropSeconds = 0.0f;
			UInt32 pendingDropFormID = 0;
			bool pendingDropIsLeftGameHand = false;
			float heldTime = 0.0f;     // how long this item has been bound
			float releaseGrace = 0.0f; // grip must stay open this long before acting
			float cooldown = 0.0f;     // blocks instant re-pickup of what we just dropped
			UInt32 lastDroppedRefID = 0;
			float lastReleaseSpeed = 0.0f; // m/s of the release that dropped lastDroppedRefID
			// Simple mode: the quick double tap of grip that lets the weapon go.
			Gestures::DoubleTapDetector doubleTap;
			// Grip hold with VRIK's holster API: a release VRIK agreed to holster, waiting for the
			// weapon to leave the hand, and the "this holster takes it" buzz. VrikHolsterRules.h.
			float holsterWaitSeconds = 0.0f;
			UInt32 holsterFormID = 0;
			bool holsterIsLeftGameHand = false;
			int holsterSlot = 0;
			VrikHolster::BuzzEdge holsterBuzz;

			// A draw from a holster, waiting for its grip blink: Weapon Throw VR will not throw with a
			// grip held since the draw either, but the hand is ON the slot at the draw and VRIK buzzes
			// again within a second because the freed slot would take the weapon back - a fake release
			// there could re-holster it. So the blink waits until the hand has left. GripBlinkRules.h.
			bool drawBlinkPending = false;
			float drawBlinkSeconds = 0.0f;
			VrikPulse::Vec3 drawBlinkFrom;
		};

		// A fresh hand that still remembers what it last dropped. The re-grab cooldown
		// has to outlive whatever the hand holds next, or a weapon caught on its way to
		// the floor would re-equip the instant it was dropped.
		GripHand FreshGripHandKeepingDropMemory(const GripHand& old)
		{
			GripHand fresh;
			fresh.cooldown = old.cooldown;
			fresh.lastDroppedRefID = old.lastDroppedRefID;
			fresh.lastReleaseSpeed = old.lastReleaseSpeed;
			return fresh;
		}

		static GripHand s_gripHands[2]; // [0] = right VR controller, [1] = left
		static bool s_registered = false;
		static bool s_globalLock = false;

		// Loop guard. A weapon released within reach of a hand can be caught again
		// immediately, re-equipped, and dropped again — and every one of those drops
		// applies a throw, so it ends up flung across the room. A playtester's log
		// showed thirty such cycles on one dagger, some launching it at 4 m/s.
		//
		// The re-grab cooldown alone cannot prevent this. It is a fixed delay, while
		// how eagerly the weapon gets caught again depends on the player's setup, not
		// on our timing. So count the cycles instead and stop feeding the loop.
		typedef std::chrono::steady_clock LoopClock;

		struct RegrabLoopEntry
		{
			UInt32 formID = 0;
			int cycles = 0;
			LoopClock::time_point windowStart;
			LoopClock::time_point suppressUntil;
		};

		static const int kMaxLoopTracked = 8;
		static RegrabLoopEntry s_regrabLoop[kMaxLoopTracked];

		RegrabLoopEntry* FindLoopEntry(const UInt32 formID)
		{
			for (int i = 0; i < kMaxLoopTracked; ++i)
			{
				if (s_regrabLoop[i].formID == formID)
				{
					return &s_regrabLoop[i];
				}
			}

			return nullptr;
		}

		RegrabLoopEntry* ClaimLoopEntry(const UInt32 formID)
		{
			for (int i = 0; i < kMaxLoopTracked; ++i)
			{
				if (s_regrabLoop[i].formID == 0)
				{
					s_regrabLoop[i] = RegrabLoopEntry{};
					s_regrabLoop[i].formID = formID;
					return &s_regrabLoop[i];
				}
			}

			// Reuse whichever entry started its window longest ago.
			int oldest = 0;
			for (int i = 1; i < kMaxLoopTracked; ++i)
			{
				if (s_regrabLoop[i].windowStart < s_regrabLoop[oldest].windowStart)
				{
					oldest = i;
				}
			}

			s_regrabLoop[oldest] = RegrabLoopEntry{};
			s_regrabLoop[oldest].formID = formID;
			return &s_regrabLoop[oldest];
		}

		// Counts one grab-equip of this weapon, and trips the guard if they are
		// arriving faster than a person could be doing on purpose.
		void NoteGrabEquipForLoopGuard(const UInt32 weaponFormID)
		{
			if (regrabLoopLimit <= 0 || weaponFormID == 0)
			{
				return;
			}

			const LoopClock::time_point now = LoopClock::now();
			RegrabLoopEntry* entry = FindLoopEntry(weaponFormID);
			if (!entry)
			{
				entry = ClaimLoopEntry(weaponFormID);
				entry->windowStart = now;
				entry->cycles = 0;
			}

			const float sinceWindow = std::chrono::duration<float>(now - entry->windowStart).count();
			if (sinceWindow > regrabLoopWindow)
			{
				// Long enough since the last flurry: ordinary play, start counting over.
				entry->windowStart = now;
				entry->cycles = 0;
			}

			entry->cycles += 1;
			if (entry->cycles < regrabLoopLimit)
			{
				return;
			}

			entry->cycles = 0;
			entry->windowStart = now;
			entry->suppressUntil =
				now + std::chrono::milliseconds(static_cast<long long>(regrabLoopCooldown * 1000.0f));

			LOG_ERR(
				"Re-grab loop guard: picked this weapon up %d times in %.1fs — it is being caught "
				"again the instant it is dropped, and every drop throws it. Leaving it alone for "
				"%.1fs. If this keeps happening, raise RegrabCooldown.",
				regrabLoopLimit,
				regrabLoopWindow,
				regrabLoopCooldown);
		}

		bool LoopGuardBlocks(const UInt32 weaponFormID)
		{
			if (regrabLoopLimit <= 0 || weaponFormID == 0)
			{
				return false;
			}

			const RegrabLoopEntry* entry = FindLoopEntry(weaponFormID);
			return entry && LoopClock::now() < entry->suppressUntil;
		}

		// Grip Hold mode's chords, on either hand: a quick run of trigger taps whose length
		// is the lock chord's toggles the global lock, and the blacklist chord's length
		// blacklists what is in that hand. A tap counts when grip is held on that hand, or
		// when that hand holds a weapon grip is not holding (Gestures::ChordTapCounts).
		struct LockChordHand
		{
			bool triggerDown = false;
			Gestures::ChordRun run;

			// A run performed correctly except that the weapon was drawn. Counted so
			// the player can be told why nothing happened.
			Gestures::ChordRun drawnRun;

			// Grip reads flicker; this is how long a dropped grip is forgiven.
			float gripGrace = 0.0f;
		};
		static LockChordHand s_lockChordHands[2]; // [0] = right VR controller, [1] = left

		// Shared by both hands, so two hands tapping together cannot fire twice.
		static float s_lockChordCooldown = 0.0f;
		static float s_lockChordHintCooldown = 0.0f;

		std::string LockStateFilePath()
		{
			const std::string runtimeDirectory = GetRuntimeDirectory();
			if (runtimeDirectory.empty())
			{
				return std::string();
			}

			return runtimeDirectory + "Data\\SKSE\\Plugins\\" + PLUGIN_FILE_NAME + "_state.ini";
		}

		void SaveLockState()
		{
			const std::string path = LockStateFilePath();
			if (path.empty())
			{
				return;
			}

			std::ofstream file(path, std::ios::trunc);
			if (!file.is_open())
			{
				LOG_ERR("GripHold: could not write lock state to %s", path.c_str());
				return;
			}

			file << "; Written by Swap Drop & Hold redux — global GripHold lock state.\n";
			file << "GlobalLock=" << (s_globalLock ? 1 : 0) << "\n";
		}

		// Announce a persisted lock loudly. A locked mod and a broken mod look
		// identical from inside the game, and this state lives in a file rather than
		// the save, so it survives reloading and follows the player between saves.
		void ReportLockStateAtStartup()
		{
			if (!s_globalLock)
			{
				return;
			}

			LOG_ERR(
				"GripHold: THE GLOBAL LOCK IS ON, so the mod is suspended and weapons behave "
				"vanilla. This was saved from a previous session and is NOT affected by "
				"loading a save. In Grip Hold mode, tap the trigger %d times on either hand - with "
				"grip held if that hand is empty%s - to switch it back on, or delete %s.",
				lockClickCount,
				lockRequiresSheathed ? " with your weapon put away" : "",
				LockStateFilePath().c_str());
		}

		void LoadLockState()
		{
			const std::string path = LockStateFilePath();
			if (path.empty())
			{
				return;
			}

			std::ifstream file(path);
			if (!file.is_open())
			{
				return; // no state yet: unlocked
			}

			std::string line;
			while (std::getline(file, line))
			{
				if (line.rfind("GlobalLock=", 0) == 0)
				{
					s_globalLock = line.size() > 11 && line[11] == '1';
				}
			}
		}

		GripHand& GripHandFor(const bool isLeftVRController)
		{
			return s_gripHands[isLeftVRController ? 1 : 0];
		}

		// A short buzz on one controller.
		//
		// VRIK already buzzes when ITS holster detection fires. This is ours saying the
		// same thing, and the point of having both is that when they disagree you can
		// feel it: VRIK buzzes and we do not means a release here will drop the weapon
		// rather than holster it, which until now was something you could only discover
		// by watching your sword hit the floor.
		void PulseController(const bool isLeftVRController)
		{
			if (!holsterHaptics || holsterHapticMicroseconds <= 0)
			{
				return;
			}

			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
			{
				return;
			}

			vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
			const vr_1_0_12::TrackedDeviceIndex_t controller =
				vrSystem->GetTrackedDeviceIndexForControllerRole(
					isLeftVRController
						? vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand
						: vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);
			if (controller == vr_1_0_12::k_unTrackedDeviceIndexInvalid)
			{
				return;
			}

			// Axis 0 is the haptic axis on every controller SkyrimVR supports, and
			// OpenVR caps a single pulse at 3999us - the config clamps to that.
			// The haptic census flags this pulse as ours on the way through its hook, and the first
			// one to pass is what proves that hook is on the right slot.
			HapticCensusNoteOwnPulse();
			vrSystem->TriggerHapticPulse(
				controller, 0, static_cast<unsigned short>(holsterHapticMicroseconds));
			HapticCensusOwnPulseDone();
		}

		bool GetControllerState(const bool isLeftVRController, vr_1_0_12::VRControllerState_t& outState)
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
			{
				return false;
			}

			vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
			const vr_1_0_12::TrackedDeviceIndex_t controller = vrSystem->GetTrackedDeviceIndexForControllerRole(
				isLeftVRController
					? vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand
					: vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);

			return vrSystem->GetControllerState(controller, &outState, sizeof(outState));
		}

		// A controller read can fail or come back empty for reasons that have nothing
		// to do with the player's hand: the role is momentarily unassigned, the
		// controller sleeps and wakes, an overlay takes input focus, SteamVR hiccups.
		// The old code returned false there, which is indistinguishable from an open
		// hand — and an open hand DROPS THE WEAPON. That is the wrong way to fail:
		// letting go is destructive and the player cannot undo it, so an unreadable
		// controller must mean "carry on holding", never "let go".
		struct GripReadCache
		{
			bool lastKnown = false;
			bool everRead = false;
			int consecutiveFailures = 0;
			int reportedFailures = 0;
		};
		static GripReadCache s_gripRead[2];

		bool IsGripPressed(const bool isLeftVRController)
		{
			GripReadCache& cache = s_gripRead[isLeftVRController ? 1 : 0];

			vr_1_0_12::VRControllerState_t state{};
			if (!GetControllerState(isLeftVRController, state))
			{
				++cache.consecutiveFailures;
				// Loud, but only once per run of failures: a player whose weapon keeps
				// leaving a closed hand needs this in their log, not buried under one
				// line per frame.
				if (cache.consecutiveFailures == 1 || cache.consecutiveFailures == 60)
				{
					++cache.reportedFailures;
					LOG_ERR(
						"%s controller state unreadable (%d in a row) — holding the last known grip (%s). "
						"If weapons leave a closed hand, this is why.",
						isLeftVRController ? "Left" : "Right",
						cache.consecutiveFailures,
						cache.lastKnown ? "closed" : "open");
				}

				// Never invent an open hand. Before the first successful read there is
				// nothing to hold, and reporting "closed" would arm a hand the player
				// is not using, so that case alone stays open.
				return cache.everRead ? cache.lastKnown : false;
			}

			if (cache.consecutiveFailures > 0)
			{
				LOG_INFO(
					"%s controller state readable again after %d failed reads",
					isLeftVRController ? "Left" : "Right",
					cache.consecutiveFailures);
				cache.consecutiveFailures = 0;
			}

			cache.everRead = true;
			// "Touched" counts only for k_EButton_Grip, where it has always been read
			// that way and is proven. It must NOT count for the analogue grip axis: on
			// some controllers that bit is touched by a finger merely resting on the
			// grip, which would make the hand read as permanently closed and the weapon
			// impossible to drop — the opposite failure, and just as bad. A squeeze
			// registers as PRESSED, so every other bit has to be pressed.
			static const uint64_t kTouchCountsMask = (1ull << 2);
			const bool shownClosed = ((state.ulButtonPressed & GRIP_BUTTON_MASK) != 0) ||
				((state.ulButtonTouched & GRIP_BUTTON_MASK & kTouchCountsMask) != 0);

			// While a menu is open over a hand holding a weapon, the game is shown that grip
			// as OPEN - and so is this read, because it goes through the same SKSE hook.
			// Grip-hold has to see the hand, not the mask: taking its own mask for an open
			// hand would drop the weapon. See MenuGrip.h.
			cache.lastKnown = MenuGripPhysicalGrip(isLeftVRController, shownClosed);
			return cache.lastKnown;
		}

		// A HIGGS grab only happens because the player squeezed, so at this instant the
		// hand IS closed. If none of the buttons we watch are down, we are watching the
		// wrong ones for this controller — and the bits that ARE down are the answer.
		// Adopt them, so grip-hold calibrates itself to the setup instead of needing an
		// ini edit and another round trip.
		//
		// The stick/touchpad (32) and the trigger (33) are excluded: those are held for
		// moving and attacking, and adopting one would mean a weapon could not be
		// dropped while the player was doing either.
		//
		// Returns true if new bits were adopted, so the caller can re-test the grip.
		bool LearnGrabButton(const bool isLeftVRController)
		{
			static const uint64_t kNeverGrip = (1ull << 32) | (1ull << 33);

			vr_1_0_12::VRControllerState_t state{};
			if (!GetControllerState(isLeftVRController, state))
			{
				return false;
			}

			// Only a PRESS, never a touch: adopting a bit that is merely touched would
			// leave the hand permanently closed.
			const uint64_t down = state.ulButtonPressed & ~kNeverGrip;
			if (down == 0)
			{
				static int quiet[2] = { 0, 0 };
				int& count = quiet[isLeftVRController ? 1 : 0];
				if (count < 3)
				{
					++count;
					LOG_ERR(
						"%s hand grabbed a weapon with NO button down that could be a grip "
						"(trigger axis %.2f). Holding it the vanilla way. If this setup grabs with "
						"the trigger, add 33 to GripButtonBits.",
						isLeftVRController ? "Left" : "Right",
						state.rAxis[1].x);
				}
				return false;
			}

			char bits[128];
			int used = 0;
			bits[0] = 0;
			for (int bit = 0; bit < 64 && used < 100; ++bit)
			{
				if ((down & (1ull << bit)) != 0)
				{
					used += sprintf_s(bits + used, sizeof(bits) - used, "%s%d", used ? ", " : "", bit);
				}
			}

			gripButtonMask |= down;
			LOG_ERR(
				"%s hand grabbed a weapon while the grip buttons we watch were not down. This setup "
				"reports its grip on button [%s], so that is now treated as grip too (mask %llu). "
				"Grip-hold works from here on. Set GripButtonBits = %s in the ini to skip this.",
				isLeftVRController ? "Left" : "Right",
				used ? bits : "none",
				static_cast<unsigned long long>(gripButtonMask),
				used ? bits : "2");
			return true;
		}

		// Simple mode's double tap on a controller whose grip touch stays on - Valve Index, see
		// GripTouchRules.h - reads PRESSES only: an Index player who squeezes twice never stops
		// touching the grip, so the reading everything else uses is one long hold to the detector.
		//
		// Simple mode never hides the grip or shows it released (MenuGrip.cpp BeginHiding;
		// GripBlinkRules.h DecideAfterPickup), so the raw read IS the hand and needs no unmasking.
		//
		// The edges are logged, a few dozen a session: nobody who builds this mod owns an Index, so
		// the log is the only way to see whether a squeeze reached the 80% force the game's own
		// binding asks for, and how far apart two squeezes landed.
		bool ReadGripPressesOnly(const bool isLeftVRController)
		{
			static bool s_last[2] = { false, false };
			static int s_logged[2] = { 0, 0 };
			static LoopClock::time_point s_lastEdge[2];
			const int index = isLeftVRController ? 1 : 0;

			vr_1_0_12::VRControllerState_t state{};
			if (!GetControllerState(isLeftVRController, state))
			{
				return s_last[index]; // an unreadable controller never invents a tap
			}

			const bool pressed = (state.ulButtonPressed & GRIP_BUTTON_MASK) != 0;
			const bool touched = (state.ulButtonTouched & GRIP_BUTTON_MASK & (1ull << 2)) != 0;
			const bool closed = GripTouch::TapReading(pressed, touched, false);

			if (closed != s_last[index])
			{
				const LoopClock::time_point now = LoopClock::now();
				if (s_logged[index] < 60)
				{
					++s_logged[index];
					LOG_INFO(
						"Double tap read [%s controller, presses only]: grip press %s (touch %s), %.2fs after the "
						"last edge.",
						isLeftVRController ? "left" : "right",
						closed ? "DOWN" : "UP",
						touched ? "on" : "off",
						s_logged[index] == 1 ? 0.0f : std::chrono::duration<float>(now - s_lastEdge[index]).count());
				}
				s_lastEdge[index] = now;
				s_last[index] = closed;
			}
			return closed;
		}

		// Exactly kDrawn (3). The classic IsWeaponDrawn() also accepts kWantToSheathe
		// and kSheathing, and a weapon on its way into the sheath must already count as
		// gone from the hand or the holster is undone the moment it is made.
		bool IsPlayerWeaponDrawn()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				return false;
			}

			return ((player->actorState.flags08 >> 5) & 7) == 3;
		}

		// Sheathing is what a holster IS: the weapon goes back on the body and VRIK
		// renders it in its slot. DrawSheatheWeapon is the same call WeaponDrawHandler
		// uses to force a draw, so this is its exact counterpart rather than a second
		// way of doing the same job.
		void SheatheWeaponForHolster()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				return;
			}

			player->DrawSheatheWeapon(false);
		}

		// A holster is only a holster if the sheathe SETTLES. The instant sheathe is
		// issued from a grip release, which can land mid-transition, and the log shows
		// the result: after most holsters the weapon read as drawn again within a few
		// seconds, so it was visible in the slot sometimes and back in the hand other
		// times. This watches the weapon for a few seconds after each holster and
		// re-issues the sheathe if the state relapses to drawn - unless the player
		// took the weapon back, which is theirs to do.
		struct HolsterUpkeep
		{
			UInt32 formID = 0;
			float delay = 0.0f;
			int retries = 0;
		};
		static HolsterUpkeep s_holsterUpkeep;

		// Whether each hand is currently standing down for a holster, so the state
		// change can be logged once instead of every frame.
		static bool s_standingDown[2] = { false, false };

		// WeaponThrowVR.dll, looked for once at PostPostLoad (GripHoldDetectThrowMod). Read by the
		// pickup, hand-off and draw blinks.
		bool s_throwModLoaded = false;

		// VRIK's own answer, heard through its haptics: per hand, where VRIK last buzzed and whether the
		// hand is still there; and whether VRIK has buzzed at all this session, which is the only
		// proof its haptics are on. VrikPulseRules.h.
		static VrikPulse::StandAside s_vrikSaid[2]; // [0] = right VR controller, [1] = left
		static bool s_vrikBuzzedThisSession = false;

		float VrikPulseNowSeconds()
		{
			return std::chrono::duration<float>(LoopClock::now().time_since_epoch()).count();
		}

		bool VrikDrivesStandAside()
		{
			return VrikPulse::VrikDrives(
				holsterListenToVrik, VrikHolsterApiAvailable(), simpleMode, s_vrikBuzzedThisSession);
		}

		// How long each hand has been clear of its holster. One frame out of range
		// is noise, not leaving.
		static float s_standDownClear[2] = { 0.0f, 0.0f };
		constexpr float kHolsterResumeDelay = 0.15f;

		// The distance part of a holster log line, exactly as it has always read for the estimate
		// ("8.2 away", or "8.2 away, palm" with the source), or VRIK's word when VRIK answered.
		// VRIK reports no distance.
		const char* HolsterReachText(
			const float distance,
			const bool withSource,
			char* buffer,
			const size_t bufferSize)
		{
			const char* source = HolsterLastPositionSource();
			if (strcmp(source, "VRIK") == 0)
			{
				return "VRIK says in reach";
			}

			if (withSource)
			{
				sprintf_s(buffer, bufferSize, "%.1f away, %s", distance, source);
			}
			else
			{
				sprintf_s(buffer, bufferSize, "%.1f away", distance);
			}
			return buffer;
		}

		// Near a holster, grip-hold gets out of VRIK's way entirely.
		//
		// This is the model that worked before release-holstering was attempted, and
		// it is robust in a way the other never was. Holstering on RELEASE has to know
		// exactly where a slot is, because being wrong drops the weapon on the floor -
		// and the ini-derived positions turned out to be 7 to 12 units off against a
		// 10-unit radius. Standing down instead means being wrong is nearly free: a
		// false positive just makes the hand behave like lock mode for a moment, which
		// is precisely the behaviour the player already uses to holster. So the radius
		// can be generous, and VRIK - which does know where its slots are - does the
		// holstering with its own grip gesture.
		bool ShouldStandDownForHolster(const bool isLeftVRController, int& outSlot, float& outDistance)
		{
			outSlot = 0;
			outDistance = 0.0f;

			if (!holsterAware ||
				VrikHolster::EffectiveHolsterAction(VrikHolsterApiAvailable(), holsterAction) != 0)
			{
				return false;
			}

			// Grip hold with VRIK's holster API and "Holster when you let go": the hand keeps holding at
			// a holster, so that letting go there can holster the weapon (DispatchGripRelease).
			if (VrikHolster::ReleasePathApplies(VrikHolsterApiAvailable(), simpleMode, holsterAware, holsterOnRelease))
			{
				return false;
			}

			// VRIK's own answer, once it has been heard this session. Its buzz means "this hand is at a
			// slot that will take this weapon" - VRIK 0.8.7's CanBeHolstered, through the haptics - and
			// the stand-aside holds until the hand has moved past VRIK's own release distance from
			// where it entered. The estimate zone below is the fallback for a VRIK whose haptics are
			// off, or until the first buzz proves they are on.
			if (VrikDrivesStandAside())
			{
				VrikPulse::StandAside& said = s_vrikSaid[isLeftVRController ? 1 : 0];
				if (!said.Active())
				{
					return false;
				}
				NiPoint3 handWorld;
				if (!HolsterHandWorldPosition(isLeftVRController, handWorld))
				{
					return false;
				}
				const VrikPulse::Vec3 at{ handWorld.x, handWorld.y, handWorld.z };
				if (!said.Update(at, VrikPulseNowSeconds(), HolsterActivationDistance()))
				{
					return false;
				}
				outSlot = 0; // VRIK does not say which
				outDistance = said.DistanceFromEntry(at);
				return true;
			}

			// Leaving takes a wider radius than entering.
			//
			// The catch radius is one number, and a hand resting at that exact distance
			// crosses it several times a second. The log caught the right hand between
			// 9.4 and 10.0 units from the chest slot, flipping in and out about ninety
			// times in a second and a half - and every flip disarmed the hand and then
			// re-announced the weapon as needing a fresh grip press. A weapon held
			// anywhere near a holster was being un-bound continuously, which is a large
			// part of what "janky" meant.
			//
			// With a margin the boundary is no longer a place the state can oscillate:
			// you must move meaningfully away before grip-hold takes over again.
			//
			// Simple mode's zone is VRIK's own reach with no margin - see HolsterRangeRules.h.
			const bool alreadyStandingDown = s_standingDown[isLeftVRController ? 1 : 0];
			return HolsterNearHand(
				isLeftVRController,
				outSlot,
				outDistance,
				HolsterRange::ForStandDown(simpleMode, alreadyStandingDown, holsterReleaseMargin));
		}

		// Watches for VRIK's OWN holster gesture: a grip press while a drawn weapon is
		// held by no armed hand (the global lock, or vanilla-sticky carry). If VRIK
		// then sheathes within the window, the press position was a true slot - learn
		// it. Our own holster path never triggers this: it arms the upkeep first, and
		// the watcher stands down whenever the upkeep owns the sheathe.
		struct PressHolsterWatch
		{
			bool prevGrip[2] = { false, false };
			float timer = 0.0f;
			bool active = false;
		};
		static PressHolsterWatch s_pressWatch;

		// When the player's weapon was last anything but drawn. A weapon that arrives in a
		// hand while it has been drawn all along did not come out of a holster - the hand
		// was bound again after an unlock - so it says nothing about where a holster is.
		static LoopClock::time_point s_lastNotDrawn;

		// The weapon each hand last took by grabbing, and when. The equip after a grab
		// runs the weapon through its draw, and the hand is bound again with its grip
		// closed when that settles - which looks exactly like a draw from a holster.
		struct GrabEquipMemory
		{
			UInt32 formID = 0;
			LoopClock::time_point when;
		};
		static GrabEquipMemory s_lastGrabEquip[2]; // [0] = right VR controller, [1] = left

		// Whether a weapon just bound to this hand really came out of a holster: it was
		// sheathed a moment ago, and this hand did not just grab it. Before 2.1.0 every
		// grab and every unlock was learned as a holster draw.
		bool CameOutOfAHolster(const bool isLeftVRController, const UInt32 formID)
		{
			const LoopClock::time_point now = LoopClock::now();
			const GrabEquipMemory& grab = s_lastGrabEquip[isLeftVRController ? 1 : 0];
			if (grab.formID == formID && std::chrono::duration<float>(now - grab.when).count() < 1.5f)
			{
				return false;
			}

			return std::chrono::duration<float>(now - s_lastNotDrawn).count() < 0.5f;
		}

		void PressHolsterWatchTick(const float deltaTime)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				return;
			}

			const UInt32 state = (player->actorState.flags08 >> 5) & 7;

			if (s_pressWatch.active)
			{
				s_pressWatch.timer -= deltaTime;
				const bool ourSheathe = s_holsterUpkeep.formID != 0;
				if (state != 3 && !ourSheathe)
				{
					s_pressWatch.active = false;
					HolsterLearnFromCapture();
				}
				else if (s_pressWatch.timer <= 0.0f || ourSheathe)
				{
					s_pressWatch.active = false;
				}
			}

			for (int i = 0; i < 2; ++i)
			{
				const bool isLeftVRController = i == 1;
				const bool grip = IsGripPressed(isLeftVRController);
				const bool pressEdge = grip && !s_pressWatch.prevGrip[i];
				s_pressWatch.prevGrip[i] = grip;

				// Only a press that OUR model is not using: the hand must not be armed,
				// and a weapon must be drawn for VRIK to have anything to holster.
				if (pressEdge && state == 3 && !GripHandFor(isLeftVRController).armed)
				{
					HolsterCapturePressCandidate(isLeftVRController);
					s_pressWatch.timer = 0.8f;
					s_pressWatch.active = true;
				}
			}
		}

		void BeginHolsterUpkeep(const UInt32 formID)
		{
			s_holsterUpkeep.formID = formID;
			s_holsterUpkeep.delay = 0.7f;
			s_holsterUpkeep.retries = 4;
		}

		void HolsterUpkeepTick(const float deltaTime)
		{
			if (s_holsterUpkeep.formID == 0)
			{
				return;
			}


			s_holsterUpkeep.delay -= deltaTime;
			if (s_holsterUpkeep.delay > 0.0f)
			{
				return;
			}

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				s_holsterUpkeep.formID = 0;
				return;
			}

			// The drawn state is GLOBAL, not per weapon. If either hand is armed on
			// ANYTHING - the holstered weapon taken back, or some other weapon grabbed
			// off the ground - then the drawn state belongs to what the player is
			// holding, and re-sheathing would yank a weapon out of their hand.
			if (GripHandFor(false).armed || GripHandFor(true).armed)
			{
				LOG_INFO("Holster upkeep: a hand is holding a weapon - standing down.");
				s_holsterUpkeep.formID = 0;
				return;
			}

			const UInt32 state = (player->actorState.flags08 >> 5) & 7;
			if (state == 0)
			{
				LOG_INFO("Holster upkeep: sheathe settled - weapon is in the holster.");
				s_holsterUpkeep.formID = 0;
				return;
			}

			if (state == 3)
			{
				if (s_holsterUpkeep.retries > 0)
				{
					--s_holsterUpkeep.retries;
					s_holsterUpkeep.delay = 0.7f;
					LOG_INFO(
						"Holster upkeep: weapon relapsed to drawn - re-sheathing (%d %s left).",
						s_holsterUpkeep.retries,
						s_holsterUpkeep.retries == 1 ? "retry" : "retries");
					player->DrawSheatheWeapon(false);
					return;
				}

				LOG_ERR("Holster upkeep: weapon keeps coming back out after 4 re-sheathes - giving up.");
				s_holsterUpkeep.formID = 0;
				return;
			}

			// Mid-transition (drawing or sheathing): look again shortly.
			s_holsterUpkeep.delay = 0.35f;
		}

		// Sheathing alone is NOT what VRIK's holster does. A sheathed weapon is still
		// EQUIPPED, so the next draw - gripping to enter combat, most obviously - pulls
		// it straight back into the hand. That is the whole reported inconsistency: the
		// log shows the weapon drawn again 1.9 to 4.8 seconds after almost every
		// holster. Putting a weapon away properly means unequipping it, which is what
		// leaves the player genuinely unarmed until they take it out again.
		void UnequipWeaponForHolster(TESForm* item, const bool isLeftGameHand)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !item)
			{
				return;
			}

			// Papyrus slot ids: 1 = right hand, 2 = left hand.
			papyrusActor::UnequipItemEx(player, item, isLeftGameHand ? 2 : 1, false);
		}

		// Same ownership rule as the trigger-hold gesture: bows belong to the off-hand
		// controller, crossbows and torches to their designated sides, everything else
		// to the controller of the game hand it is equipped in.
		bool IsGripOwnerController(TESForm* equipped, const bool isLeftVRController)
		{
			if (IsBowWeaponForm(equipped))
			{
				return IsOffHandVRController(isLeftVRController);
			}

			if (IsCrossbowWeaponForm(equipped))
			{
				return IsMainHandVRController(isLeftVRController);
			}

			if (IsTorchLightForm(equipped))
			{
				return IsOffHandVRController(isLeftVRController);
			}

			// A real two-hander fills BOTH equip slots, so GetEquippedObject returns it
			// for either hand and both controllers used to bind to it. That gave the
			// weapon two independent releasers: opening the empty off hand threw the
			// greatsword out of the main hand that was still gripping it. The grab side
			// already restricts these to the main hand; ownership has to match.
			// 2H Weapons Unlocked's proxies are authored as one-handers and keep the
			// ordinary either-hand behaviour, exactly as they do for grabs.
			if (IsTwoHandedWeaponForm(equipped) && !IsTwoHandProxyForm(equipped))
			{
				return IsMainHandVRController(isLeftVRController);
			}

			return true;
		}

		void ResetGripHand(GripHand& hand)
		{
			hand = FreshGripHandKeepingDropMemory(hand);
		}

		// A hand can receive a hand-off only when it is not already holding something of
		// its own. Dual wielding must never move a weapon across hands.
		bool IsHandFreeForHandOff(const bool isLeftGameHand)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				return false;
			}

			TESForm* equipped = player->GetEquippedObject(isLeftGameHand);
			return !equipped || !IsGrabEquipDropItemForm(equipped);
		}

		// M3.0.0-test14, the hand switch: the other hand is on this one-handed weapon (the HIGGS two-handing latch),
		// still gripping it, and holds nothing of its own. HandSwitchRules.h.
		bool HandOffWanted(const bool isLeftVRController, TESForm* equipped, const bool isLeftGameHand)
		{
			HandSwitch::Facts facts;
			facts.twoHandedWeapon = IsTwoHandedWeaponForm(equipped);
			facts.otherHandOnWeapon = IsOppositeHandEngaged(isLeftGameHand);
			facts.otherGripClosed = IsGripPressed(!isLeftVRController);
			facts.otherHandFree = IsHandFreeForHandOff(!isLeftGameHand);
			return HandSwitch::Decide(facts) == HandSwitch::Verdict::kSwitch;
		}

		// The weapon moves to the other hand: measured there first - where, and which way round, that hand holds it
		// (GrabPose.cpp) - then the transfer, which sends that grip on just before it equips.
		void StartHandOff(GripHand& hand, const bool isLeftVRController, TESForm* equipped, const bool isLeftGameHand,
			const char* how)
		{
			LOG_INFO(
				"GripHold: %s controller %s %s while the other hand grips it - handing off",
				isLeftVRController ? "left" : "right",
				how,
				GetSafeFormName(equipped));
			GrabPoseOnHandOff(isLeftGameHand, equipped->formID);
			ResetGripHand(hand);
			ScheduleGripHandOff(isLeftGameHand, equipped->formID);
		}

		void SetGlobalLock(const bool locked)
		{
			if (s_globalLock == locked)
			{
				return;
			}

			s_globalLock = locked;
			SaveLockState();

			// Entering the lock forgets the hands (weapons behave vanilla-sticky from
			// here); pending throw fixups still finish on their own.
			if (locked)
			{
				ResetGripHand(s_gripHands[0]);
				ResetGripHand(s_gripHands[1]);
			}

			LOG_INFO("GripHold: global lock %s (persisted).", locked ? "ON — mod suspended, weapons vanilla" : "OFF — grip hold active");

			if (lockNotifications)
			{
				ShowHUDMessage(
					locked
						? "Swap Drop & Hold: locked — weapons behave normally"
						: "Swap Drop & Hold: unlocked — grip holds your weapons",
					nullptr,
					true);
			}
		}

		bool IsTriggerPressed(const bool isLeftVRController)
		{
			vr_1_0_12::VRControllerState_t state{};
			if (!GetControllerState(isLeftVRController, state))
			{
				return false;
			}

			return ((state.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0) ||
				state.rAxis[1].x > 0.5f;
		}

		bool IsTriggerActiveState(const vr_1_0_12::VRControllerState_t& state)
		{
			return ((state.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0) ||
				state.rAxis[1].x > 0.5f;
		}

		// A finished run of clicks, resolved by its total.
		//
		// The total cannot be acted on the moment it is reached, which is how this
		// worked until 2.0.2. Three clicks and five clicks share their first three, so
		// firing the lock on the third made a five-click chord unreachable - the
		// cooldown that the lock started then swallowed clicks four and five outright.
		// So nothing fires until the run is OVER, and a run is over when the player
		// stops clicking.
		//
		// A total matching neither chord does nothing at all, and that is the safety
		// valve. The trigger is the attack button, so a fight IS this gesture - but a
		// fight is also six, seven, nine attacks, and those resolve to nothing. Only a
		// deliberate run of exactly the right length, followed by a pause, fires.
		void ResolveLockChordRun(const bool isLeftVRController, const int clicks, const bool qualified)
		{
			if (clicks <= 0)
			{
				return;
			}

			// Blacklist first: if the two counts are ever configured equal, the more
			// specific gesture should win rather than silently never happening. The
			// config loader keeps them apart, so this is belt and braces.
			const bool isBlacklist = blacklistGesture && clicks == blacklistClickCount;
			const bool isLock = clicks == lockClickCount;
			if (!isBlacklist && !isLock)
			{
				return;
			}

			// A run of exactly a chord's length that did not qualify is logged: a chord
			// refused in silence is how a chord comes to feel broken.
			if (!qualified)
			{
				LOG_INFO(
					"GripHold: %d trigger taps on the %s hand, but not every tap had grip held or a "
					"weapon in that hand that grip is not holding - the %s chord was ignored.",
					clicks,
					isLeftVRController ? "left" : "right",
					isBlacklist ? "blacklist" : "lock");
				return;
			}

			s_lockChordCooldown = lockChordCooldown;
			if (isBlacklist)
			{
				ExclusionsToggleHand(VRControllerToGameHand(isLeftVRController));
				return;
			}

			SetGlobalLock(!s_globalLock);
		}

		// The same run, performed correctly except that the weapon was drawn while the
		// player has asked for the weapon-away requirement. Says why nothing happened,
		// because silence here is what made the chord feel broken in 1.x.
		void ResolveDrawnChordRun(const int clicks, const bool qualified)
		{
			if (clicks <= 0 || !qualified)
			{
				return;
			}

			const bool wasAChord = (clicks == lockClickCount) ||
				(blacklistGesture && clicks == blacklistClickCount);
			if (!wasAChord || s_lockChordHintCooldown > 0.0f)
			{
				return;
			}

			s_lockChordHintCooldown = 8.0f;
			LOG_INFO(
				"GripHold: chord performed with the weapon DRAWN, so it was ignored - put "
				"the weapon away and repeat it. Turn off \"Only use the chords with weapon away\" in "
				"the MCM to drop this requirement.");
			if (lockNotifications)
			{
				ShowHUDMessage(
					"Swap Drop & Hold: put your weapon away to lock or unlock",
					nullptr,
					true);
			}
		}

		// Whether a trigger tap on this controller counts toward a chord: grip held, or a
		// weapon in that hand that grip is not holding. The rule is Gestures::ChordTapCounts;
		// this only gathers what it needs from the game.
		bool ChordTapCountsForHand(const bool isLeftVRController, const bool gripHeld)
		{
			bool weaponInHand = false;
			bool weaponGripBound = false;

			PlayerCharacter* player = *g_thePlayer;
			if (player)
			{
				const TrackedWeaponHandInfo tracked = GetTrackedWeaponForVRController(player, isLeftVRController);
				TESForm* weapon = tracked.weapon;
				if (weapon && requireDrawnWeapon && !IsPlayerWeaponDrawn())
				{
					weapon = nullptr; // sheathed: on the body, not in the hand
				}

				if (weapon && IsGripOwnerController(weapon, isLeftVRController))
				{
					weaponInHand = true;
					// While locked nothing is grip-bound, whatever the hand last recorded.
					weaponGripBound = !s_globalLock && GripHandFor(isLeftVRController).armed;
				}
			}

			return Gestures::ChordTapCounts(gripHeld, weaponInHand, weaponGripBound);
		}

		// One controller's trigger taps for this frame. Either hand can perform either
		// chord; each keeps its own run, and the item a blacklist chord acts on is the one
		// in the hand that performed it.
		void UpdateLockChordHand(const bool isLeftVRController, const float deltaTime)
		{
			LockChordHand& chord = s_lockChordHands[isLeftVRController ? 1 : 0];

			// The window is the gap between consecutive clicks, not the length of the
			// whole run, so a three-click chord is no harder to land than two.
			int clicks = 0;
			bool qualified = false;
			if (chord.run.Tick(deltaTime, lockClickWindow, clicks, qualified))
			{
				ResolveLockChordRun(isLeftVRController, clicks, qualified);
			}
			if (chord.drawnRun.Tick(deltaTime, lockClickWindow, clicks, qualified))
			{
				ResolveDrawnChordRun(clicks, qualified);
			}

			vr_1_0_12::VRControllerState_t state{};
			if (!GetControllerState(isLeftVRController, state))
			{
				chord.triggerDown = false;
				chord.run.Cancel();
				chord.drawnRun.Cancel();
				return;
			}

			const bool gripPressed = ((state.ulButtonPressed & GRIP_BUTTON_MASK) != 0) ||
				((state.ulButtonTouched & GRIP_BUTTON_MASK) != 0);
			const bool triggerActive = IsTriggerActiveState(state);
			const bool triggerClicked = triggerActive && !chord.triggerDown;
			chord.triggerDown = triggerActive;

			// Grip is judged on every tap, not on every frame.
			//
			// A controller's grip read flickers: this log shows the grip reported open
			// for three frames in the middle of a run the player never loosened.
			// Cancelling on the first open frame threw away chords that were performed
			// correctly, and the player has no way to see why.
			//
			// Opening the grip does not end a run. Because grip is judged at each tap,
			// letting go straight after the last one leaves the run exactly as performed.
			if (gripPressed)
			{
				chord.gripGrace = lockGripGrace;
			}
			else if (chord.gripGrace > 0.0f)
			{
				chord.gripGrace -= deltaTime;
			}
			const bool gripHeld = gripPressed || chord.gripGrace > 0.0f;

			// Optionally, the weapon must also be away. OFF by default since 2.0.2.
			//
			// The trigger is the attack button, so three quick taps of it also describe
			// three fast attacks - and grip is no protection while carrying, because
			// grip-hold keeps the grip closed for the entire time a weapon is carried: a
			// playtester locked the mod five times in one fight without knowing the
			// chord existed. Requiring a sheathed weapon put the chord out of a fight's
			// reach - but it also put it out of reach when a fight is exactly when you
			// notice you want the mod out of the way, and you cannot sheathe a weapon
			// the mod is holding in your hand.
			//
			// What replaces it is resolving a run by its total rather than on reaching
			// a count (see ResolveLockChordRun). Fighting produces runs of every length;
			// only a run of exactly the chord's length, ended deliberately, fires
			// anything. That is a weaker guard than "impossible while drawn", and it is
			// the trade the player asked for. This setting is still here for anyone who
			// wants the old certainty back.
			//
			// Rejecting in silence is what made this feel broken. Every toggle that
			// worked in the log happened with the weapon away; every one that did not
			// happened with it drawn, and the player got no signal either way - the
			// same gesture, performed the same way, working or doing nothing according
			// to a state nobody was thinking about. So a chord that was complete in
			// every other respect still says what stopped it.
			if (lockRequiresSheathed && IsPlayerWeaponDrawn())
			{
				if (triggerClicked && s_lockChordCooldown <= 0.0f)
				{
					chord.drawnRun.Click(ChordTapCountsForHand(isLeftVRController, gripHeld));
				}

				chord.run.Cancel();
				return;
			}

			if (!triggerClicked || s_lockChordCooldown > 0.0f)
			{
				return;
			}

			// A click too long after the last one starts a fresh run rather than
			// counting toward the old one (Gestures::ChordRun).
			chord.run.Click(ChordTapCountsForHand(isLeftVRController, gripHeld));
		}

		// Grip Hold mode's chords on both hands. Runs even while locked (it is the way
		// back), never in menus, and not at all in Simple mode.
		void UpdateLockChord(const float deltaTime)
		{
			if (s_lockChordCooldown > 0.0f)
			{
				s_lockChordCooldown -= deltaTime;
			}

			if (s_lockChordHintCooldown > 0.0f)
			{
				s_lockChordHintCooldown -= deltaTime;
			}

			UpdateLockChordHand(false, deltaTime);
			UpdateLockChordHand(true, deltaTime);
		}

		// A run never spans a menu or a spell wheel. The click window does not advance
		// while one is open, so without this the tap that opens a menu and two taps
		// right after it closes would add up to a lock. The trigger counts as still
		// held, so a press that closes the menu cannot be the first click back.
		void CancelLockChordRun()
		{
			for (LockChordHand& chord : s_lockChordHands)
			{
				chord.run.Cancel();
				chord.drawnRun.Cancel();
				chord.triggerDown = true;
			}
		}

		class GripReleaseDropTask : public TaskDelegate
		{
		public:
			GripReleaseDropTask(
				const bool isLeftGameHand,
				const bool isLeftVRController,
				const UInt32 weaponFormID)
				: m_isLeftGameHand(isLeftGameHand)
				, m_isLeftVRController(isLeftVRController)
				, m_weaponFormID(weaponFormID)
			{
			}

			virtual void Run() override
			{
				GripHand& hand = GripHandFor(m_isLeftVRController);
				hand.dropQueued = false;

				UInt32 droppedRefID = 0;
				if (!DropEquippedWeaponFromHand(
					m_isLeftGameHand, m_isLeftVRController, m_weaponFormID, false, &droppedRefID))
				{
					return;
				}

				hand.lastDroppedRefID = droppedRefID;
				hand.cooldown = regrabCooldown;
				hand.lastReleaseSpeed = ThrowLastReleaseSpeedMps(m_isLeftVRController);
				ThrowBeginFixup(m_isLeftVRController, droppedRefID);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftGameHand;
			bool m_isLeftVRController;
			UInt32 m_weaponFormID;
		};

		void DispatchGripRelease(
			GripHand& hand,
			const bool isLeftVRController,
			TESForm* equipped,
			const bool isLeftGameHand)
		{
			// Every release is measured against VRIK's holsters, whether or not one is
			// in range, because that measurement is how the slot anchors get calibrated.
			HolsterTraceRelease(isLeftVRController);

			// A two-hander whose other grip is still closed has not been released at
			// all, so that case keeps priority over everything below it.
			const bool twoHandStillHeld =
				IsTwoHandedWeaponForm(equipped) && IsGripPressed(!isLeftVRController);

			// The other hand holding this one-handed weapon takes it over (M3.0.0-test14): ahead of holstering too,
			// as the holster design's decision 6 says - a second hand still holding the weapon wins.
			const bool handOff = HandOffWanted(isLeftVRController, equipped, isLeftGameHand);

			// Released over a VRIK holster: the player is putting the weapon AWAY, not
			// throwing it on the floor. VRIK drives holsters from the grip button, but
			// grip-hold keeps the grip closed the whole time a weapon is carried, so the
			// press VRIK waits for can never happen — which is exactly why holstering
			// only ever worked with the mod locked. Sheathing puts the weapon back on
			// the body, where VRIK renders it in its slot.
			int holsterSlot = 0;
			float holsterDistance = 0.0f;
			// Grip hold with VRIK's holster API and "Holster when you let go" asks VRIK further down,
			// after the other hand has had its say, so this check is skipped for it.
			const bool vrikReleasePath =
				VrikHolster::ReleasePathApplies(VrikHolsterApiAvailable(), simpleMode, holsterAware, holsterOnRelease);
			// iHolsterAction's sheathe-it-ourselves options predate VRIK's own holster call.
			const int effectiveHolsterAction =
				VrikHolster::EffectiveHolsterAction(VrikHolsterApiAvailable(), holsterAction);
			// A Simple-mode double tap asks with VRIK's own reach, the zone its stand-down uses:
			// beyond it VRIK does not holster, so "leaving it to VRIK" would only ignore the
			// tap. Grip Hold asks with the default reach, as it always has.
			if (!vrikReleasePath && !twoHandStillHeld && !handOff &&
				HolsterNearHand(
					isLeftVRController,
					holsterSlot,
					holsterDistance,
					HolsterRange::ForStandDown(simpleMode, false, holsterReleaseMargin)))
			{
				char reachText[64];
				LOG_INFO(
					"GripHold: %s controller released %s over the %s holster (%s) — %s",
					isLeftVRController ? "left" : "right",
					GetSafeFormName(equipped),
					HolsterSlotName(holsterSlot),
					HolsterReachText(holsterDistance, true, reachText, sizeof(reachText)),
					effectiveHolsterAction >= 2 ? "sheathing and unequipping it"
						: (effectiveHolsterAction == 1 ? "sheathing it" : "leaving it to VRIK"));

				if (effectiveHolsterAction >= 1)
				{
					SheatheWeaponForHolster();
					BeginHolsterUpkeep(equipped->formID);
				}
				if (effectiveHolsterAction >= 2)
				{
					UnequipWeaponForHolster(equipped, isLeftGameHand);
				}
				ResetGripHand(hand);
				return;
			}

			// 1. Two-handed weapons: the weapon stays as long as EITHER grip holds it —
			// releasing one hand just shifts ownership to the other controller.
			if (IsTwoHandedWeaponForm(equipped))
			{
				if (IsGripPressed(!isLeftVRController))
				{
					LOG_INFO(
						"GripHold: %s controller released 2H weapon, other grip still holds it — keeping equipped",
						isLeftVRController ? "left" : "right");
					ResetGripHand(hand);
					return;
				}
			}
			// 2. One-handed hand-off: the opposite hand is physically on this weapon
			// (HIGGS two-handing latch) with its grip pressed — transfer the equip.
			// The receiving hand must be EMPTY. The latch alone is not enough: HIGGS
			// reports IsTwoHanding() whenever both hands hold weapons, so while dual
			// wielding it is permanently true and every release became a hand-off that
			// displaced the other weapon into the inventory.
			else if (handOff)
			{
				StartHandOff(hand, isLeftVRController, equipped, isLeftGameHand, "released");
				return;
			}

			// Grip hold with VRIK's holster API: a holster in reach that VRIK says can take the weapon
			// gets it. This comes after the two-hander and hand-off branches above, which return when
			// the other hand keeps the weapon. Anything else goes on to stick or drop, as agreed in
			// docs/DESIGN-vrik-holster-api.md.
			if (vrikReleasePath)
			{
				const int slot = VrikHolsterSlotInReach(isLeftGameHand);
				const bool canHolster = slot != 0 && VrikCanHolster(isLeftGameHand);
				switch (VrikHolster::DecideRelease(vrikReleasePath, slot, canHolster))
				{
				case VrikHolster::Release::kAskVrik:
					if (VrikHolsterWeapon(isLeftGameHand, equipped->formID))
					{
						LOG_INFO(
							"GripHold: %s controller let go of %s at the %s holster - VRIK is holstering it.",
							isLeftVRController ? "left" : "right",
							GetSafeFormName(equipped),
							HolsterSlotName(slot));
						ResetGripHand(hand);
						hand.holsterWaitSeconds = VrikHolster::kWaitSeconds;
						hand.holsterFormID = equipped->formID;
						hand.holsterIsLeftGameHand = isLeftGameHand;
						hand.holsterSlot = slot;
						return;
					}
					LOG_INFO(
						"GripHold: VRIK refused to holster %s at %s - dropping it.",
						GetSafeFormName(equipped),
						HolsterSlotName(slot));
					break;

				case VrikHolster::Release::kSlotRefuses:
					LOG_INFO(
						"GripHold: VRIK says %s can't take %s - dropping it.",
						HolsterSlotName(slot),
						GetSafeFormName(equipped));
					break;

				default:
					break;
				}
			}

			// 3. Stick: the blade is buried in someone — leave it in the wound.
			if (EmbedTryCaptureOnRelease(isLeftGameHand, isLeftVRController, equipped->formID))
			{
				ResetGripHand(hand);
				hand.cooldown = regrabCooldown;
				return;
			}

			// 4. Plain drop from the open hand, with the hand's real swing momentum.
			if (!g_task)
			{
				return;
			}

			// The release pose and velocity are captured NOW, whatever happens next,
			// so a deferred drop still throws with the motion the player actually made.
			ThrowCaptureRelease(isLeftVRController);

			// Hand the gesture to any other mod that wants it, before taking it.
			//
			// WeaponThrow VR throws on its own hold-button gesture, and grip-hold was
			// answering first: the weapon was unequipped and lying on the floor before
			// the throw mod could act, which is why throwing only worked with this mod
			// locked. Guessing WHICH button it watches got this wrong once already, so
			// this does not guess. It waits a moment and looks at the OUTCOME - if the
			// weapon stops being equipped in that hand, somebody else took it and there
			// is nothing left to drop. That works for any mod, not just this one.
			if (throwVrYieldSeconds > 0.0f)
			{
				hand.dropQueued = true;
				hand.pendingDropSeconds = throwVrYieldSeconds;
				hand.pendingDropFormID = equipped->formID;
				hand.pendingDropIsLeftGameHand = isLeftGameHand;
				LOG_INFO(
					"GripHold: %s controller opened on %s - yielding %.2fs before dropping it.",
					isLeftVRController ? "left" : "right",
					GetSafeFormName(equipped),
					throwVrYieldSeconds);
				return;
			}

			hand.dropQueued = true;
			g_task->AddTask(new GripReleaseDropTask(isLeftGameHand, isLeftVRController, equipped->formID));
		}

		// Runs while a drop is waiting to see whether another mod claims the weapon.
		// Returns true if the hand is still waiting and the rest of the update should
		// be skipped this frame.
		bool ServicePendingDrop(GripHand& hand, const bool isLeftVRController, const float deltaTime)
		{
			if (hand.pendingDropSeconds <= 0.0f)
			{
				return false;
			}

			PlayerCharacter* player = *g_thePlayer;
			const bool isLeftGameHand = hand.pendingDropIsLeftGameHand;
			TESForm* stillHeld = player ? player->GetEquippedObject(isLeftGameHand) : nullptr;

			// Gone, or swapped for something else: a throw mod (or anything else) has
			// taken it. There is nothing to drop and nothing to say sorry for.
			if (!stillHeld || stillHeld->formID != hand.pendingDropFormID)
			{
				LOG_INFO(
					"GripHold: %s controller released and another mod took the weapon within "
					"%.2fs - a throw, most likely. Standing down.",
					isLeftVRController ? "left" : "right",
					throwVrYieldSeconds - hand.pendingDropSeconds);
				hand.pendingDropSeconds = 0.0f;
				hand.pendingDropFormID = 0;
				ResetGripHand(hand);
				return true;
			}

			// Grip closed again inside the window: the player kept hold of it.
			if (IsGripPressed(isLeftVRController))
			{
				LOG_INFO(
					"GripHold: %s controller closed again before the drop landed - keeping the weapon.",
					isLeftVRController ? "left" : "right");
				hand.pendingDropSeconds = 0.0f;
				hand.pendingDropFormID = 0;
				hand.dropQueued = false;
				hand.gripDown = true;
				hand.releaseGrace = 0.0f;
				if (simpleMode)
				{
					hand.doubleTap.Bind(true); // kept, and waiting for a fresh double tap
				}
				else
				{
					hand.armed = true;
				}
				return true;
			}

			hand.pendingDropSeconds -= deltaTime;
			if (hand.pendingDropSeconds > 0.0f)
			{
				return true;
			}

			// Nobody wanted it. Drop it, with the pose and velocity captured at the
			// moment of release rather than now.
			hand.pendingDropSeconds = 0.0f;
			const UInt32 formID = hand.pendingDropFormID;
			hand.pendingDropFormID = 0;
			if (g_task)
			{
				g_task->AddTask(new GripReleaseDropTask(isLeftGameHand, isLeftVRController, formID));
			}
			return true;
		}

		// A release VRIK agreed to holster, waiting for VRIK to take the weapon out of the hand.
		// Returns true while it waits, and then the rest of that hand's update is skipped. Never
		// drops the weapon: a late holster on top of a drop could leave it in the holster AND on the
		// floor. Rules in VrikHolsterRules.h.
		bool ServicePendingHolster(GripHand& hand, const bool isLeftVRController, const float deltaTime)
		{
			if (hand.holsterFormID == 0)
			{
				return false;
			}

			PlayerCharacter* player = *g_thePlayer;
			TESForm* inHand = player ? player->GetEquippedObject(hand.holsterIsLeftGameHand) : nullptr;
			const UInt32 inHandFormID = inHand ? inHand->formID : 0;
			const float waited = VrikHolster::kWaitSeconds - hand.holsterWaitSeconds;

			switch (VrikHolster::TickWait(hand.holsterWaitSeconds, hand.holsterFormID, inHandFormID, deltaTime))
			{
			case VrikHolster::Wait::kWaiting:
				return true;

			case VrikHolster::Wait::kHolstered:
				LOG_INFO(
					"GripHold: VRIK holstered %s into %s %.2fs after the release.",
					GetSafeFormName(LookupFormByID(hand.holsterFormID)),
					HolsterSlotName(hand.holsterSlot),
					waited);
				VrikHolsterNoteCompleted(hand.holsterIsLeftGameHand, hand.holsterFormID);
				ResetGripHand(hand);
				// A grip closed during the wait is not a new press on the now-empty hand.
				hand.gripDown = IsGripPressed(isLeftVRController);
				return true;

			case VrikHolster::Wait::kSwapped:
				LOG_INFO(
					"GripHold: while VRIK was holstering, the %s hand's weapon became %s - VRIK swapped it "
					"with the weapon in %s.",
					isLeftVRController ? "left" : "right",
					GetSafeFormName(inHand),
					HolsterSlotName(hand.holsterSlot));
				ResetGripHand(hand);
				return false; // the bind path below takes the new weapon this frame

			case VrikHolster::Wait::kTimedOut:
			default:
			{
				const VrikHolster::KeptHand kept = VrikHolster::HandAfterTimeout(IsGripPressed(isLeftVRController));
				LOG_ERR(
					"GripHold: VRIK agreed to holster %s but it is still in the %s hand after %.0fs - keeping it. "
					"VRIK may have refused quietly, its scripts may be busy, or another mod may be "
					"overwriting _vrik_qust_system.pex.",
					GetSafeFormName(inHand),
					isLeftVRController ? "left" : "right",
					VrikHolster::kWaitSeconds);
				ResetGripHand(hand);
				hand.boundFormID = inHandFormID;
				hand.armed = kept.armed;
				hand.needsFreshGripPress = kept.needsFreshGripPress;
				hand.gripDown = kept.armed;
				return true;
			}
			}
		}

		// WeaponThrow VR's gesture is hold trigger, swing, release trigger - all on
		// the TRIGGER, with the grip never involved. Grip-hold does not read or block
		// that input, yet throwing works only with this mod locked, so something else
		// in what we do while unlocked is in the way and three guesses have missed it.
		//
		// This logs the trigger edges and what we were doing at each, so the next
		// attempt starts from evidence instead of a fourth guess.
		void TraceTriggerForThrow(const bool isLeftVRController, const GripHand& hand)
		{
			if (!throwTrace)
			{
				return;
			}

			static bool wasDown[2] = { false, false };
			bool& prev = wasDown[isLeftVRController ? 1 : 0];
			const bool down = IsTriggerPressed(isLeftVRController);
			if (down == prev)
			{
				return;
			}
			prev = down;

			PlayerCharacter* player = *g_thePlayer;
			const UInt32 drawState = player ? ((player->actorState.flags08 >> 5) & 7) : 99;

			// peak is OUR measurement of the same motion WeaponThrowVR scores as
			// ThrowSpeed. Their gate is 125 of their units and their log prints the
			// value, so putting our own number beside it on every gesture says which
			// of the two is seeing the swing.
			int samples = 0;
			const float peak = ThrowPeakSpeedMps(isLeftVRController, samples);

			LOG_INFO(
				"Throw trace: %s trigger %s | lock=%s armed=%s grip=%s bound=%08X "
				"drawState=%u peak=%.2fm/s samples=%d pendingDrop=%.2f holsterStandDown=%s",
				isLeftVRController ? "left" : "right",
				down ? "DOWN" : "UP",
				s_globalLock ? "ON" : "off",
				hand.armed ? "yes" : "no",
				IsGripPressed(isLeftVRController) ? "closed" : "open",
				hand.boundFormID,
				drawState,
				peak,
				samples,
				hand.pendingDropSeconds,
				s_standingDown[isLeftVRController ? 1 : 0] ? "yes" : "no");
		}

		// Simple mode's whole hand: the weapon stays, whatever the grip does, until a quick
		// double tap of grip - and then it is let go exactly the way opening the hand lets
		// go in Grip Hold, through DispatchGripRelease: a two-hander the other grip still
		// holds stays, the other hand takes a one-hander it is gripping, a blade in someone
		// stays in the wound, and anything else is thrown with the swing.
		void UpdateSimpleHand(
			GripHand& hand,
			const bool isLeftVRController,
			TESForm* equipped,
			const bool isLeftGameHand,
			const float deltaTime)
		{
			const bool grip = IsGripPressed(isLeftVRController);
			hand.gripDown = grip;

			// What the double tap is fed. On every controller but Index that is the same reading;
			// on Index, presses only (GripTouchRules.h).
			const bool tapGrip = GripTouchTapsCountTouch(isLeftVRController)
				? grip
				: ReadGripPressesOnly(isLeftVRController);

			Gestures::DoubleTapConfig config;
			config.windowSeconds = doubleTapWindow;
			config.releaseGraceSeconds = gripReleaseGraceSeconds;

			const bool doubleTapped = hand.doubleTap.Update(tapGrip, deltaTime, config);

			// M3.0.0-test14, the hand switch: two-handing, letting go with this hand hands the weapon to the other
			// hand at once - no double tap. A double tap while two-handing therefore switches rather than drops.
			if (hand.doubleTap.Released() && !hand.dropQueued && hand.heldTime >= minHoldTime &&
				HandOffWanted(isLeftVRController, equipped, isLeftGameHand))
			{
				StartHandOff(hand, isLeftVRController, equipped, isLeftGameHand, "let go of");
				return;
			}

			if (!doubleTapped || hand.dropQueued)
			{
				return;
			}

			if (hand.heldTime < minHoldTime)
			{
				return;
			}

			LOG_INFO(
				"GripHold: %s controller double-tapped grip on %s - letting go of it.",
				isLeftVRController ? "left" : "right",
				GetSafeFormName(equipped));
			DispatchGripRelease(hand, isLeftVRController, equipped, isLeftGameHand);
		}

		void UpdateGripHand(const bool isLeftVRController, const float deltaTime)
		{
			GripHand& hand = GripHandFor(isLeftVRController);

			TraceTriggerForThrow(isLeftVRController, hand);

			if (ServicePendingDrop(hand, isLeftVRController, deltaTime))
			{
				return;
			}

			if (ServicePendingHolster(hand, isLeftVRController, deltaTime))
			{
				return;
			}

			if (hand.cooldown > 0.0f)
			{
				hand.cooldown -= deltaTime;
				if (hand.cooldown <= 0.0f)
				{
					hand.cooldown = 0.0f;
					hand.lastDroppedRefID = 0;
				}
			}

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				ResetGripHand(hand);
				return;
			}

			const TrackedWeaponHandInfo tracked = GetTrackedWeaponForVRController(player, isLeftVRController);
			TESForm* equipped = tracked.weapon;
			// A sheathed weapon is on the BODY, not in the hand. Without this, holstering
			// left the weapon equipped and grip-hold simply bound it again: gripping
			// anywhere at all produced an invisible weapon in an empty hand, and opening
			// that hand dropped the weapon that was supposed to be holstered.
			if (equipped && requireDrawnWeapon && !IsPlayerWeaponDrawn())
			{
				equipped = nullptr;
			}

			if (equipped && !IsGripOwnerController(equipped, isLeftVRController))
			{
				equipped = nullptr;
			}

			// Over a holster: hand the whole gesture to VRIK. The weapon stays equipped
			// and in the hand, nothing arms, and nothing is dropped - so the player's
			// grip press reaches VRIK and holsters the way it always has.
			if (equipped)
			{
				int standDownSlot = 0;
				float standDownDistance = 0.0f;
				const bool standDown =
					ShouldStandDownForHolster(isLeftVRController, standDownSlot, standDownDistance);
				const int handIndex = isLeftVRController ? 1 : 0;
				bool& wasStandingDown = s_standingDown[handIndex];
				float& clearTime = s_standDownClear[handIndex];

				// The buzz. This zone is padded so that a holster VRIK would have taken never
				// becomes a weapon on the floor, and the buzz used to ride along with it -
				// announcing a holster 6 units early in front of the body and 14 behind. At a
				// slot whose centre has been measured off real draws that padding is the only
				// error left, so the buzz moves in to VRIK's own reach, below. Unmeasured slots
				// keep this one: their centre is still an estimate, and a tight circle around a
				// centre 9 units out never fires at all.
				//
				// VRIK 0.8.7 and newer answer "will this holster take it" themselves and that
				// answer always wins, so none of this runs there - nor in Simple mode, whose
				// zone is already VRIK's own reach with no padding.
				// And once VRIK's own buzz drives the stand-aside, VRIK's buzz IS the buzz: this mod's
				// would only sound a second time for the same answer.
				const bool vrikDrives = VrikDrivesStandAside();
				const bool buzzAtVrikReach = !VrikHolsterApiAvailable() && !simpleMode && !vrikDrives;

				if (standDown)
				{
					clearTime = 0.0f;
					if (!wasStandingDown)
					{
						wasStandingDown = true;
						if (!vrikDrives &&
							(!buzzAtVrikReach ||
								HolsterRange::BuzzOnStandDownEntry(HolsterSlotCentreIsTrusted(standDownSlot))))
						{
							PulseController(isLeftVRController);
						}
						// At a holster the body is loaded and posed, which is when asking which
						// skeleton the holster maths runs on is worth anything. Both of these are
						// discovery only and write nothing.
						HolsterSkeletonProbe(HolsterSlotName(standDownSlot));
						HolsterCensusAtHand(isLeftVRController, HolsterSlotName(standDownSlot));

						char reachText[64];
						if (standDownSlot > 0)
						{
							LOG_INFO(
								"GripHold: %s hand is over the %s holster (%s) - standing down "
								"so VRIK's own grip-to-holster works.",
								isLeftVRController ? "left" : "right",
								HolsterSlotName(standDownSlot),
								HolsterReachText(standDownDistance, false, reachText, sizeof(reachText)));
						}
						else
						{
							LOG_INFO(
								"GripHold: %s hand is at a holster VRIK buzzed for - standing down so VRIK's own "
								"grip-to-holster works, until the hand moves %.0f units off.",
								isLeftVRController ? "left" : "right",
								VrikPulse::LeaveDistance(HolsterActivationDistance()));
						}
					}
				}
				else if (wasStandingDown)
				{
					// Out of range for one frame is not leaving. Beyond the margin AND
					// stayed there is.
					clearTime += deltaTime;
					if (clearTime >= kHolsterResumeDelay)
					{
						clearTime = 0.0f;
						wasStandingDown = false;
						LOG_INFO(
							"GripHold: %s hand left the holster - grip-hold is live again.",
							isLeftVRController ? "left" : "right");
					}
				}

				if (buzzAtVrikReach)
				{
					int buzzSlot = 0;
					float buzzDistance = 0.0f;
					const bool takesIt = HolsterBuzzNearHand(isLeftVRController, buzzSlot, buzzDistance);
					if (hand.holsterBuzz.Update(takesIt, deltaTime))
					{
						PulseController(isLeftVRController);
						char buzzText[64];
						LOG_INFO(
							"GripHold: %s hand reached the %s holster (%s) - buzzing at VRIK's own "
							"reach instead of the wider stand-aside zone.",
							isLeftVRController ? "left" : "right",
							HolsterSlotName(buzzSlot),
							HolsterReachText(buzzDistance, false, buzzText, sizeof(buzzText)));
					}
				}

				if (wasStandingDown)
				{
					// Disarm rather than merely skip the release: an armed hand that
					// wanders over a holster must not fire its drop the moment it
					// wanders back off again.
					hand.armed = false;
					hand.dropQueued = false;
					hand.releaseGrace = 0.0f;
					hand.needsFreshGripPress = true;
					hand.gripDown = IsGripPressed(isLeftVRController);
					hand.doubleTap.Bind(hand.gripDown); // a press at a holster is VRIK's, not a tap
					hand.heldTime += deltaTime;
					ThrowSampleHeldPose(isLeftVRController, deltaTime);
					return;
				}
			}

			// Bind / rebind. Adopting an item we did not grab-equip (menus, favorites,
			// scripts) starts UNARMED: it only becomes grip-bound once the player has
			// actually squeezed grip while holding it, so closing a menu with an open
			// hand cannot dump the fresh equip on the floor.
			if (!equipped)
			{
				s_vrikSaid[isLeftVRController ? 1 : 0].Clear();
				if (hand.boundFormID != 0 && !hand.dropQueued)
				{
					ResetGripHand(hand);
				}

				// Empty hand: a grip press near a lodged weapon takes hold of it for a
				// pull; opening the grip leaves it wherever it has been drawn to.
				const bool gripNow = IsGripPressed(isLeftVRController);
				if (gripNow && !hand.gripDown && hand.cooldown <= 0.0f)
				{
					EmbedTryStartPull(isLeftVRController);
				}
				else if (!gripNow && hand.gripDown)
				{
					EmbedReleasePullBy(isLeftVRController);
				}
				hand.gripDown = gripNow;
				return;
			}

			if (hand.boundFormID != equipped->formID)
			{
				hand = FreshGripHandKeepingDropMemory(hand);
				hand.boundFormID = equipped->formID;
				// A new weapon in this hand starts a new pose history. Whatever is in
				// the buffer was sampled from the last one and would otherwise be
				// differentiated against this one's position on the first release.
				ThrowClearHistory(isLeftVRController);

				// Equipped by something other than one of our grabs. Two very different
				// things arrive here and they need opposite treatment:
				//
				//   From a MENU the hand is not holding anything, and the grip press that
				//   picked the item belongs to the menu rather than to the weapon. So it
				//   waits for a deliberate squeeze, or closing the inventory with an open
				//   hand dumps the fresh equip on the floor.
				//
				//   From a VRIK HOLSTER the player is ALREADY gripping, because gripping
				//   is how a weapon is pulled out of a holster. Asking them to let go and
				//   re-grip is asking them to drop it.
				//
				// A closed grip with no menu open is the second case.
				const bool gripAlreadyClosed = IsGripPressed(isLeftVRController);
				const bool fromMenu = IsInputCapturingMenuOpen();
				hand.gripDown = gripAlreadyClosed;

				if (simpleMode)
				{
					// Simple mode holds whatever arrives, however it arrived. The one
					// thing to get right is that the squeeze which brought it in - a
					// holster draw - is not the first tap of a double tap.
					hand.doubleTap.Bind(gripAlreadyClosed);
					if (gripAlreadyClosed && !fromMenu && CameOutOfAHolster(isLeftVRController, equipped->formID))
					{
						HolsterLearnFromDraw(isLeftVRController);
					}
				}
				else
				{
					hand.armed = gripAlreadyClosed && !fromMenu;
					hand.needsFreshGripPress = !hand.armed;

					if (hand.armed)
					{
						const bool fromHolster = CameOutOfAHolster(isLeftVRController, equipped->formID);
						LOG_INFO(
							"GripHold: %s controller took hold of %s %s - the grip was already "
							"closed, so it is grip-bound with no extra squeeze",
							isLeftVRController ? "left" : "right",
							GetSafeFormName(equipped),
							fromHolster ? "straight from a draw" : "again after its equip or an unlock");
						if (fromHolster)
						{
							// The hand is ON the slot it just pulled this weapon from - the one
							// place VRIK's true geometry is handed to us for free.
							HolsterLearnFromDraw(isLeftVRController);

							// And the grip blink for Weapon Throw VR waits here until the hand
							// has left that slot - see drawBlinkPending.
							NiPoint3 handWorld;
							if (HolsterHandWorldPosition(isLeftVRController, handWorld))
							{
								hand.drawBlinkPending = true;
								hand.drawBlinkSeconds = 0.0f;
								hand.drawBlinkFrom = VrikPulse::Vec3{ handWorld.x, handWorld.y, handWorld.z };
							}
						}
					}
				}
			}

			hand.heldTime += deltaTime;
			ThrowSampleHeldPose(isLeftVRController, deltaTime);

			// The grip blink a draw is owed, once the hand has left the slot it drew from. This runs only
			// on steps where the hand is NOT standing aside (those return above), so standingAside is
			// false here by construction; the facts still carry it for the rules' sake.
			if (hand.drawBlinkPending)
			{
				hand.drawBlinkSeconds += deltaTime;
				GripBlink::DrawFacts drawFacts;
				drawFacts.gripStillClosed = hand.armed && hand.gripDown;
				drawFacts.standingAside = s_standingDown[isLeftVRController ? 1 : 0];
				drawFacts.secondsSinceDraw = hand.drawBlinkSeconds;
				drawFacts.leaveDistance = VrikPulse::LeaveDistance(HolsterActivationDistance());
				NiPoint3 handWorld;
				drawFacts.distanceFromDraw = HolsterHandWorldPosition(isLeftVRController, handWorld)
					? VrikPulse::Distance(hand.drawBlinkFrom, VrikPulse::Vec3{ handWorld.x, handWorld.y, handWorld.z })
					: 0.0f;

				switch (GripBlink::DecideAfterDraw(drawFacts))
				{
				case GripBlink::DrawStep::kCancel:
					hand.drawBlinkPending = false;
					break;
				case GripBlink::DrawStep::kBlink:
				{
					hand.drawBlinkPending = false;
					// The same go/no-go a pickup gets, except that no HIGGS hand is off for a draw:
					// nothing was grabbed, the weapon arrived equipped, and HIGGS holds nothing that
					// a shown release could make it drop.
					GripBlink::PickupFacts facts;
					facts.gripHoldMode = gripHoldMode;
					facts.simpleMode = simpleMode;
					facts.throwModLoaded = s_throwModLoaded;
					facts.throwButton = WeaponThrowButtonFor(tracked.isLeftGameHand);
					facts.gripClosed = true;
					facts.higgsHandOff = true;
					facts.menuOpen = IsInputCapturingMenuOpen();
					facts.holsterInReach = s_throwModLoaded && VrikHolsterApiAvailable() &&
						VrikHolsterSlotInReach(tracked.isLeftGameHand) != 0;
					const GripBlink::Decision decision = GripBlink::DecideAfterPickup(facts);
					if (decision == GripBlink::Decision::kAtHolster)
					{
						// VRIK 0.8.7+ says a holster is still in reach: keep waiting, up to the maximum.
						hand.drawBlinkPending = hand.drawBlinkSeconds < GripBlink::kDrawBlinkMaxWaitSeconds * 2.0f;
					}
					else if (decision == GripBlink::Decision::kBlink)
					{
						MenuGripShowReleased(isLeftVRController, GripBlink::kSeconds);
						LOG_INFO(
							"GripHold: %s controller drew %s from a holster %.1fs ago and has left it (%.0f units) - other "
							"mods are shown that grip released for %.2fs (for Weapon Throw VR); Grip Hold keeps holding it.",
							isLeftVRController ? "left" : "right",
							GetSafeFormName(equipped),
							hand.drawBlinkSeconds,
							drawFacts.distanceFromDraw,
							GripBlink::kSeconds);
					}
					break;
				}
				default:
					break;
				}
			}

			// Grip hold with VRIK's holster API: buzz once when the hand reaches a holster VRIK says will
			// take this weapon, so the player knows letting go there holsters it.
			if (VrikHolster::ReleasePathApplies(VrikHolsterApiAvailable(), simpleMode, holsterAware, holsterOnRelease))
			{
				const bool takesIt =
					VrikHolsterSlotInReach(tracked.isLeftGameHand) != 0 && VrikCanHolster(tracked.isLeftGameHand);
				if (hand.holsterBuzz.Update(takesIt, deltaTime))
				{
					PulseController(isLeftVRController);
				}
			}

			if (simpleMode)
			{
				UpdateSimpleHand(hand, isLeftVRController, equipped, tracked.isLeftGameHand, deltaTime);
				return;
			}

			const bool grip = IsGripPressed(isLeftVRController);

			if (grip)
			{
				if (!hand.armed && !hand.needsFreshGripPress)
				{
					hand.armed = true;
					LOG_INFO(
						"GripHold: %s controller armed on %s formId=%08X",
						isLeftVRController ? "left" : "right",
						GetSafeFormName(equipped),
						equipped->formID);
				}
				hand.releaseGrace = 0.0f;
			}
			else
			{
				// Grip seen open: the item is no longer locked, the next press takes
				// hold of it and normal grip-hold behaviour resumes.
				if (hand.needsFreshGripPress)
				{
					hand.needsFreshGripPress = false;
					LOG_INFO(
						"GripHold: %s controller unlocked %s formId=%08X — next grip press takes hold of it",
						isLeftVRController ? "left" : "right",
						GetSafeFormName(equipped),
						equipped->formID);
				}
				else if (hand.armed && !hand.dropQueued && hand.heldTime >= minHoldTime)
				{
					hand.releaseGrace += deltaTime;
					if (hand.releaseGrace >= gripReleaseGraceSeconds)
					{
						const bool isLeftGameHand = tracked.isLeftGameHand;
						hand.armed = false;
						hand.releaseGrace = 0.0f;
						DispatchGripRelease(hand, isLeftVRController, equipped, isLeftGameHand);
					}
				}
			}

			hand.gripDown = grip;
		}

		// A weapon HIGGS is holding as a loose physics object because its auto-equip was
		// refused. The hand holding it cannot pull, and the weapon jitters in it - the
		// 2.0.5 log a tester sent had five of these straight after drops. Each one is
		// watched so the log says how it ended and how long it lasted, and one a hand is plainly
		// still holding once the cooldown is over is given back as a weapon - whichever hand dropped
		// it (M3.0.0-test10, LooseHoldRules.h).
		struct LooseHold
		{
			UInt32 refID = 0;
			bool droppedBySameHand = false;
			LooseHoldRule::Why why = LooseHoldRule::Why::kCooldown;
			float seconds = 0.0f;
			bool reportedLong = false;
			float driftTimer = 0.0f;
			NiPoint3 driftStart;
		};
		static LooseHold s_looseHolds[2]; // [0] = right VR controller, [1] = left

		// Below this, a release barely moved: nobody throws a weapon at 1 m/s.
		constexpr float kStillReleaseMps = 1.0f;

		// A catch this soon after the drop is a flicker, or a hand that never really opened:
		// the tester's log has the same hand re-grabbing at 0.09, 0.09, 0.11 and 0.16s.
		constexpr float kFlickerCatchSeconds = 0.2f;

		bool RegrabCooldownCovers(const UInt32 refID)
		{
			for (const GripHand& hand : s_gripHands)
			{
				if (hand.cooldown > 0.0f && hand.lastDroppedRefID != 0 && hand.lastDroppedRefID == refID)
				{
					return true;
				}
			}
			return false;
		}

		bool SticksLookIdle()
		{
			for (int i = 0; i < 2; ++i)
			{
				vr_1_0_12::VRControllerState_t state{};
				if (!GetControllerState(i == 1, state))
				{
					continue;
				}

				// Axis 0 is the stick or pad on every controller SkyrimVR supports and axis 3
				// is the Index stick. Axis 2 is left out: on Touch and Index it is the grip.
				if (std::fabs(state.rAxis[0].x) > 0.2f || std::fabs(state.rAxis[0].y) > 0.2f ||
					std::fabs(state.rAxis[3].x) > 0.2f || std::fabs(state.rAxis[3].y) > 0.2f)
				{
					return false;
				}
			}
			return true;
		}

		void NoteLooseHold(const bool isLeftVRController, TESObjectREFR* refr, const bool droppedBySameHand,
			const LooseHoldRule::Why why)
		{
			LooseHold& hold = s_looseHolds[isLeftVRController ? 1 : 0];
			hold = LooseHold{};
			hold.refID = refr->formID;
			hold.droppedBySameHand = droppedBySameHand;
			hold.why = why;

			PlayerCharacter* player = *g_thePlayer;
			if (player)
			{
				hold.driftStart = player->pos;
			}
		}

		void ServiceLooseHolds(const float deltaTime)
		{
			for (int i = 0; i < 2; ++i)
			{
				LooseHold& hold = s_looseHolds[i];
				if (hold.refID == 0)
				{
					continue;
				}

				const bool isLeftVRController = i == 1;
				hold.seconds += deltaTime;

				TESObjectREFR* held = higgsInterface ? higgsInterface->GetGrabbedObject(isLeftVRController) : nullptr;
				if (!held || held->formID != hold.refID)
				{
					LOG_INFO(
						"GripHold: the %s hand's loose hold on refId=%08X ended after %.2fs.",
						isLeftVRController ? "left" : "right",
						hold.refID,
						hold.seconds);
					hold = LooseHold{};
					continue;
				}

				const bool gripClosed = IsGripPressed(isLeftVRController);

				// Grip Hold only: the cooldown that refused the equip is over, and the player is
				// squeezing - they are holding their weapon, so it goes back to being one, whichever
				// hand dropped it (test10: the other hand's catch used to stay loose for good). In
				// Simple mode a loose hold ends the moment the hand opens, which it does after every
				// double tap. The loop guard's holds stay loose (LooseHoldRules.h).
				if (LooseHoldRule::GiveBack(simpleMode, hold.why, hold.droppedBySameHand, gripClosed,
						RegrabCooldownCovers(held->formID),
						held->baseForm && LoopGuardBlocks(held->baseForm->formID)))
				{
					LOG_INFO(
						"GripHold: the %s hand is still holding refId=%08X loose %.2fs after the re-grab "
						"cooldown refused it (%s hand dropped it), with its grip closed - equipping it.",
						isLeftVRController ? "left" : "right",
						hold.refID,
						hold.seconds,
						hold.droppedBySameHand ? "the same" : "the other");
					hold = LooseHold{};
					ScheduleGrabbedWeaponActivation(isLeftVRController, held);
					continue;
				}

				if (!hold.reportedLong && hold.seconds >= 1.0f)
				{
					hold.reportedLong = true;
					LOG_INFO(
						"GripHold: HIGGS is still holding refId=%08X loose in the %s hand after %.1fs "
						"(grip reads %s) - that hand cannot pull while it holds it.",
						hold.refID,
						isLeftVRController ? "left" : "right",
						hold.seconds,
						gripClosed ? "closed" : "open");
				}

				// For "moving backwards on his own": how far the player goes while this lasts.
				// Room-scale walking moves the player too, so the sticks are reported beside it.
				PlayerCharacter* player = *g_thePlayer;
				if (player)
				{
					hold.driftTimer += deltaTime;
					if (hold.driftTimer >= 1.0f)
					{
						const float dx = player->pos.x - hold.driftStart.x;
						const float dy = player->pos.y - hold.driftStart.y;
						const float moved = std::sqrt(dx * dx + dy * dy);
						if (moved > 20.0f)
						{
							LOG_INFO(
								"GripHold: the player moved %.0f units in %.1fs while the %s hand held "
								"refId=%08X loose (sticks %s).",
								moved,
								hold.driftTimer,
								isLeftVRController ? "left" : "right",
								hold.refID,
								SticksLookIdle() ? "idle" : "in use");
						}
						hold.driftStart = player->pos;
						hold.driftTimer = 0.0f;
					}
				}
			}
		}

		void OnHiggsPulled(const bool isLeft, TESObjectREFR* pulledRefr)
		{
			if (!pulledRefr || !pulledRefr->baseForm || !IsGrabEquipDropItemForm(pulledRefr->baseForm))
			{
				return;
			}

			LOG_INFO(
				"HIGGS: the %s hand pulled %s refId=%08X.",
				isLeft ? "left" : "right",
				GetSafeFormName(pulledRefr->baseForm),
				pulledRefr->formID);
		}

		void OnGripHoldStep(void* /*world*/)
		{
			if (!gripHoldMode)
			{
				// Nothing is grip-held, so a menu has nothing to hide from.
				MenuGripPublishHands(false, false);
				return;
			}

			// Pending throws and lodged weapons are already-committed world state:
			// they keep being serviced regardless of menus or the lock.
			ThrowServiceFixups();
			{
				static auto lastPinTime = std::chrono::high_resolution_clock::now();
				const auto nowPin = std::chrono::high_resolution_clock::now();
				float pinDt = std::chrono::duration<float>(nowPin - lastPinTime).count();
				lastPinTime = nowPin;
				if (pinDt > 0.1f)
				{
					pinDt = 0.1f;
				}
				if (pinDt < 0.0001f)
				{
					pinDt = 0.0001f;
				}
				EmbedServicePins(pinDt);
			}

			if (IsInputCapturingMenuOpen())
			{
				// Grips pressed inside menus must not arm or drop anything on exit.
				s_gripHands[0].releaseGrace = 0.0f;
				s_gripHands[1].releaseGrace = 0.0f;
				s_gripHands[0].gripDown = false;
				s_gripHands[1].gripDown = false;
				// Nor may trigger presses there count toward a chord, or grip presses toward
				// a double tap: the press that closes a menu belongs to the menu.
				CancelLockChordRun();
				s_gripHands[0].doubleTap.Bind(true);
				s_gripHands[1].doubleTap.Bind(true);
				return;
			}

			// Back from a menu, or never in one. MenuGrip.h has the whole story.
			MenuGripOnFrameWithoutMenu();
			for (int controller = 0; controller < 2; ++controller)
			{
				const bool isLeftVRController = controller == 1;
				if (!MenuGripTakeReleasedInMenu(isLeftVRController))
				{
					continue;
				}

				// Let go inside a menu. That is not a drop - nobody opening their hand in
				// their inventory means to throw their sword on the floor - so the weapon
				// becomes what a weapon equipped from the inventory already is: still
				// equipped, not held, and taken back by the next deliberate squeeze.
				GripHand& menuHand = GripHandFor(isLeftVRController);
				if (menuHand.armed)
				{
					menuHand.armed = false;
					menuHand.needsFreshGripPress = true;
					menuHand.releaseGrace = 0.0f;
					menuHand.gripDown = IsGripPressed(isLeftVRController);
					LOG_INFO(
						"Menu grip: the %s hand let go while a menu was open - its weapon stays "
						"equipped and waits for the next squeeze instead of dropping.",
						isLeftVRController ? "left" : "right");
				}
			}
			MenuGripPublishHands(
				GripHandFor(false).armed && GripHandFor(false).gripDown,
				GripHandFor(true).armed && GripHandFor(true).gripDown);

			if (spellwheelInterface &&
				(spellwheelInterface->IsMainWheelOpen() || spellwheelInterface->IsSecondaryWheelOpen()))
			{
				s_gripHands[0].releaseGrace = 0.0f;
				s_gripHands[1].releaseGrace = 0.0f;
				CancelLockChordRun();
				s_gripHands[0].doubleTap.Bind(true);
				s_gripHands[1].doubleTap.Bind(true);
				return;
			}

			static auto lastTime = std::chrono::high_resolution_clock::now();
			const auto currentTime = std::chrono::high_resolution_clock::now();
			float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
			lastTime = currentTime;

			if (deltaTime > 0.1f)
			{
				deltaTime = 0.1f;
			}
			if (deltaTime < 0.0001f)
			{
				deltaTime = 0.0001f;
			}

			// Grip Hold's chords run even while locked - they are the way back out. Simple
			// mode has no chords at all.
			if (!simpleMode)
			{
				UpdateLockChord(deltaTime);
			}

			// Runs whether or not the mod is suspended: an empty hand is the only
			// moment the palm can be measured, and the lock is when hands are emptiest.
			HolsterCalibratePalms();

			// The skeleton probe needs only a loaded body. It ran off a stand-aside in
			// M2.2.3-test1 - which needs Grip hold, a weapon in hand and that hand at a holster -
			// so a session spent looking around produced nothing at all. Here it is on a timer,
			// above the suspend check, so it reports whatever else the player is doing.
			HolsterSkeletonProbeTick(deltaTime);

			// Before anything reads the lock, so the hold counts down even on the frames the mod
			// stands down for.
			CellLoadGuardTick(deltaTime);

			// Runs even while suspended: the menu's exclude buttons need to know what
			// was in your hand before you put it down to open the menu.
			ExclusionsNoteHeld();

			// For CameOutOfAHolster: a draw from a holster starts from a sheathed weapon.
			if (!IsPlayerWeaponDrawn())
			{
				s_lastNotDrawn = LoopClock::now();
			}

			// Simple mode has no lock: a lock saved by Grip Hold waits for Grip Hold.
			// A cell load tears the player's 3D down and builds it again: for a few frames either side
			// of that, the controller read, the weapon's node and HIGGS's hold are all unreliable, and
			// acting on any of them can drop the weapon on a floor that no longer exists. So the mod
			// locks itself, in EVERY mode - Simple can drop a weapon too - and takes itself back a
			// moment after the new cell appears. CellLoadGuardRules.h.
			if (CellLoadGuardLocked() || (s_globalLock && !simpleMode))
			{
				// Suspended, but still watching. A locked-mode gesture is the control
				// case for the unlocked one, and WeaponThrowVR's log carries no
				// timestamps - so unless our log records BOTH modes there is no way to
				// line the two up, and the last round's alignment was a guess our own
				// log then contradicted. Sampling a pose and printing a trigger edge
				// change nothing about how the game behaves while locked.
				ThrowSampleHeldPose(false, deltaTime);
				ThrowSampleHeldPose(true, deltaTime);
				TraceTriggerForThrow(false, GripHandFor(false));
				TraceTriggerForThrow(true, GripHandFor(true));
				return;
			}

			EmbedServiceResume(false, deltaTime);
			EmbedServiceResume(true, deltaTime);

			ServiceLooseHolds(deltaTime);

			UpdateGripHand(false, deltaTime);
			UpdateGripHand(true, deltaTime);
			HolsterUpkeepTick(deltaTime);
			PressHolsterWatchTick(deltaTime);
			HolsterScanSpheresTick(deltaTime);
		}
	}

	void RegisterGripHold()
	{
		if (s_registered)
		{
			return;
		}

		if (!higgsInterface)
		{
			LOG_ERR("GripHold requires HIGGS.");
			return;
		}

		LoadLockState();
		higgsInterface->AddPrePhysicsStepCallback(OnGripHoldStep);
		higgsInterface->AddPulledCallback(OnHiggsPulled);
		s_registered = true;

		// The ini has not been read yet here, so the mode, the chords and a saved lock are
		// reported by GripHoldOnSettingsReloaded once it has.
		LOG_INFO(
			"GripHold registered (saved Grip Hold lock %s). The weapon mode is reported when the ini is read.",
			s_globalLock ? "ON" : "off");
	}


	void GripHoldDetectThrowMod()
	{
		s_throwModLoaded = GetModuleHandleA("WeaponThrowVR.dll") != nullptr;
		if (s_throwModLoaded)
		{
			LOG_INFO(
				"GripHold: Weapon Throw VR is loaded - after a Grip Hold pickup by a hand whose Weapon Throw VR "
				"hold button is Grip, other mods are shown that grip released for %.2fs, because Weapon Throw VR "
				"will not throw with a grip held since the pickup (GripBlinkRules.h).",
				GripBlink::kSeconds);
		}
		else
		{
			LOG_INFO("GripHold: Weapon Throw VR is not loaded - pickups do not show other mods a released grip.");
		}
	}

	namespace
	{
		// A weapon arriving in a hand because THAT HAND took it: a grab-equip pickup, or the other
		// hand handing it over. Both leave the hand holding a weapon with a grip that has been
		// closed since before it had one, and both must be remembered as a grab rather than read
		// as a draw out of a holster - so both run this.
		//
		// countsAsRegrab separates them. A pickup feeds the re-grab loop guard, which watches for
		// a weapon being caught again the instant it is dropped. Passing a weapon from hand to
		// hand is not that, and counting it would stop a player who does it quickly.
		void GripHoldOnWeaponAcquired(
			const bool isLeftGameHand,
			const UInt32 weaponFormID,
			const bool countsAsRegrab,
			const char* const how)
		{
			if (!gripHoldMode || weaponFormID == 0)
			{
				return;
			}

			const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
			GripHand& hand = GripHandFor(isLeftVRController);
			hand = FreshGripHandKeepingDropMemory(hand);
			hand.boundFormID = weaponFormID;
			if (countsAsRegrab)
			{
				NoteGrabEquipForLoopGuard(weaponFormID);
			}

			// The squeeze that grabbed this weapon is not a VRIK press-holster, and the draw
			// its equip starts is not a holster draw. Neither may teach a holster position.
			s_pressWatch.active = false;
			s_lastGrabEquip[isLeftVRController ? 1 : 0].formID = weaponFormID;
			s_lastGrabEquip[isLeftVRController ? 1 : 0].when = LoopClock::now();

			// The grab is normally the grip press, so a weapon picked off the ground skips
			// the unlock step. But that is an ASSUMPTION about which button the player
			// grabbed with, and when it is wrong it is catastrophic: the hand is armed
			// while our poll reads it as open, so the weapon is released the instant
			// MinHoldTime elapses — the "it flew out of a hand I never opened" report,
			// landing at exactly MinHoldTime + GripReleaseGraceSeconds every time.
			//
			// So only arm on a grip we can actually SEE. If the button is not one we
			// recognise, the weapon is held the vanilla way until a real grip press
			// arrives, which is harmless, instead of being thrown on the floor.
			bool gripVisible = IsGripPressed(isLeftVRController);
			if (!gripVisible && LearnGrabButton(isLeftVRController))
			{
				// Now that the right button is being watched, the squeeze that caused this
				// grab is visible — so the weapon arms immediately, exactly as it would on
				// a setup we already understood. No extra press, and the hand-off sees the
				// other hand gripping too.
				gripVisible = IsGripPressed(isLeftVRController);
			}

			if (simpleMode)
			{
				// Simple mode never arms: the weapon stays until a double tap of grip, and the
				// squeeze that just grabbed it must not count as that double tap's first tap.
				hand.gripDown = gripVisible;
				hand.doubleTap.Bind(gripVisible);
			}
			else
			{
				hand.armed = gripVisible;
				hand.gripDown = gripVisible;
				hand.needsFreshGripPress = !gripVisible;
			}

			// Weapon Throw VR will not throw with a hand whose grip has stayed down since the pickup, and
			// Grip Hold never lets that grip come up - so other mods are shown it released for a moment
			// while this mod keeps holding the weapon. GripBlinkRules.h has the measurements.
			GripBlink::PickupFacts facts;
			facts.gripHoldMode = gripHoldMode;
			facts.simpleMode = simpleMode;
			facts.throwModLoaded = s_throwModLoaded;
			facts.throwButton = WeaponThrowButtonFor(isLeftGameHand);
			facts.gripClosed = gripVisible;
			facts.higgsHandOff = HiggsReleaseHandIsOff(isLeftVRController);
			facts.menuOpen = IsInputCapturingMenuOpen();
			// Only asked when it can change the answer, because it is a call into VRIK.
			facts.holsterInReach = s_throwModLoaded && !simpleMode && gripVisible && VrikHolsterApiAvailable() &&
				VrikHolsterSlotInReach(isLeftGameHand) != 0;

			const char* const handName = isLeftVRController ? "left" : "right";
			switch (GripBlink::DecideAfterPickup(facts))
			{
			case GripBlink::Decision::kBlink:
				MenuGripShowReleased(isLeftVRController, GripBlink::kSeconds);
				LOG_INFO(
					"GripHold: %s controller %s %s with its grip closed - other mods are shown that grip "
					"released for %.2fs (for Weapon Throw VR, whose hold button for this hand is %s); the game "
					"itself %s; Grip Hold keeps holding it.",
					handName,
					how,
					GetSafeFormName(LookupFormByID(weaponFormID)),
					GripBlink::kSeconds,
					GripBlink::ThrowButtonName(facts.throwButton),
					GameGripGuardActive() ? "is shown the real grip, so it cannot read a fresh press as Ready Weapon"
					                      : "sees it too (the game-grip guard is not in place)");
				break;
			case GripBlink::Decision::kAtHolster:
				LOG_INFO(
					"GripHold: %s controller %s %s at a holster - other mods are not shown its grip released, "
					"so VRIK cannot holster it. Weapon Throw VR will wait for that grip to open.",
					handName,
					how,
					GetSafeFormName(LookupFormByID(weaponFormID)));
				break;
			case GripBlink::Decision::kHiggsHandOn:
				LOG_INFO(
					"GripHold: %s controller %s %s after its HIGGS hand was already back on - other mods are "
					"not shown its grip released. Weapon Throw VR will wait for that grip to open.",
					handName,
					how,
					GetSafeFormName(LookupFormByID(weaponFormID)));
				break;
			case GripBlink::Decision::kMenuOpen:
				LOG_INFO(
					"GripHold: %s controller %s %s with a menu open - other mods are not shown its grip released.",
					handName,
					how,
					GetSafeFormName(LookupFormByID(weaponFormID)));
				break;
			default:
				break;
			}

			EmbedOnHandEquipped(isLeftVRController);
		}
	}

	void GripHoldOnGrabEquipped(const bool isLeftGameHand, const UInt32 weaponFormID)
	{
		GripHoldOnWeaponAcquired(isLeftGameHand, weaponFormID, true, "picked up");
	}

	void GripHoldOnHandOffComplete(const bool isLeftGameHand, const UInt32 weaponFormID)
	{
		// Not a re-grab: the weapon never left the player's hands.
		GripHoldOnWeaponAcquired(isLeftGameHand, weaponFormID, false, "took over");
	}

	void GripHoldOnVrikHolsterBuzz(const bool isLeftVRController)
	{
		if (!gripHoldMode)
		{
			return;
		}

		if (!s_vrikBuzzedThisSession)
		{
			s_vrikBuzzedThisSession = true;
			if (holsterListenToVrik && !VrikHolsterApiAvailable() && !simpleMode)
			{
				LOG_INFO(
					"VRIK holster buzz: heard for the first time this session - from here on Grip hold stands aside "
					"where VRIK buzzes, its own estimate zone and holster buzz stand down (bHolsterListenToVrik = 1).");
			}
		}

		NiPoint3 handWorld;
		if (!HolsterHandWorldPosition(isLeftVRController, handWorld))
		{
			return;
		}
		s_vrikSaid[isLeftVRController ? 1 : 0].Enter(
			VrikPulse::Vec3{ handWorld.x, handWorld.y, handWorld.z }, VrikPulseNowSeconds());
	}

	const char* GripHoldDescribeHand(const bool isLeftVRController, char* const buffer, const size_t size)
	{
		if (!gripHoldMode)
		{
			snprintf(buffer, size, "Legacy mode");
			return buffer;
		}

		const GripHand& hand = GripHandFor(isLeftVRController);
		if (hand.boundFormID == 0)
		{
			snprintf(buffer, size, "empty hand%s", IsGripPressed(isLeftVRController) ? ", grip down" : "");
			return buffer;
		}

		snprintf(
			buffer,
			size,
			"holding %s%s%s%s",
			GetSafeFormName(LookupFormByID(hand.boundFormID)),
			hand.armed ? ", armed" : "",
			hand.gripDown ? ", grip down" : "",
			s_standingDown[isLeftVRController ? 1 : 0] ? ", standing aside at a holster" : "");
		return buffer;
	}

	bool GripHoldIsGloballyLocked()
	{
		return s_globalLock && !simpleMode;
	}

	void GripHoldOnSettingsReloaded()
	{
		// What the mod does at holsters with these settings, logged when it changes. The VRIK holster API is
		// known from PostPostLoad, and the settings from DataLoaded or a menu close.
		{
			static int s_loggedHolsterBehaviour = -1;
			const int behaviour =
				!gripHoldMode                ? 0 :
				!VrikHolsterApiAvailable()   ? 1 :
				!holsterAware                ? 2 :
				simpleMode                   ? 3 :
				holsterOnRelease             ? 4 : 5;
			if (behaviour != s_loggedHolsterBehaviour)
			{
				s_loggedHolsterBehaviour = behaviour;
				static const char* const kHolsterBehaviour[] = {
					"Holsters: Legacy mode - holsters work as in 2.1.1.",
					"Holsters: VRIK has no holster API (it needs build 80700) - holsters work as in 2.1.1.",
					"Holsters: bHolsterAware is off - the mod ignores holsters.",
					"Holsters: Simple mode - the mod steps aside where VRIK says a holster is in reach.",
					"Holsters: Grip hold - letting go at a holster VRIK says takes the weapon holsters it (bHolsterOnRelease on).",
					"Holsters: Grip hold - the mod steps aside where VRIK says a holster is in reach (bHolsterOnRelease off).",
				};
				LOG_INFO("%s", kHolsterBehaviour[behaviour]);
			}
		}

		static int s_appliedMode = -1;
		if (weaponMode == s_appliedMode)
		{
			return;
		}

		if (s_appliedMode >= 0)
		{
			LOG_INFO(
				"Weapon mode: %s -> %s. Both hands start over; nothing is dropped by the switch.",
				WeaponModeName(s_appliedMode),
				WeaponModeName(weaponMode));
		}
		s_appliedMode = weaponMode;

		// Whatever each hand was in the middle of belonged to the old mode's rules.
		ResetGripHand(s_gripHands[0]);
		ResetGripHand(s_gripHands[1]);
		CancelLockChordRun();
		MenuGripReset();

		if (!s_globalLock)
		{
			return;
		}

		if (simpleMode)
		{
			LOG_INFO(
				"GripHold: a Grip Hold lock is saved, but Simple mode has no lock - it is ignored here "
				"and applies again if you switch back to Grip Hold.");
		}
		else if (gripHoldMode)
		{
			ReportLockStateAtStartup();
		}
	}

	bool GripHoldShouldSuppressGrab(const bool isLeftVRController, TESObjectREFR* grabbedRefr)
	{
		if (!gripHoldMode || !grabbedRefr)
		{
			return false;
		}

		// A weapon caught in a drop/catch/re-equip loop stops being auto-equipped for a
		// moment, which breaks the cycle: it stays a physics object until it is picked
		// up deliberately again.
		if (grabbedRefr->baseForm && LoopGuardBlocks(grabbedRefr->baseForm->formID))
		{
			LOG_INFO(
				"GripHold: %s hand grab of refId=%08X refused by the re-grab loop guard - HIGGS holds it loose.",
				isLeftVRController ? "left" : "right",
				grabbedRefr->formID);
			NoteLooseHold(isLeftVRController, grabbedRefr, false, LooseHoldRule::Why::kLoopGuard);
			return true;
		}

		// EITHER hand, not just this one. A weapon released from one hand often lands
		// within reach of the other, and HIGGS takes hold of it there on its own — so
		// checking only the releasing controller let a bow dropped from the right hand
		// be caught and equipped by the left, without the player ever gripping it.
		for (int i = 0; i < 2; ++i)
		{
			const GripHand& hand = s_gripHands[i];
			if (hand.cooldown <= 0.0f || hand.lastDroppedRefID == 0 ||
				hand.lastDroppedRefID != grabbedRefr->formID)
			{
				continue;
			}

			const bool sameHand = (i == 1) == isLeftVRController;
			const bool gripClosed = IsGripPressed(isLeftVRController);
			const float sinceDrop = regrabCooldown - hand.cooldown;

			// Grip Hold only: the same hand, squeezing again within a moment of a release
			// that barely moved. The player never meant to let go - a grip that flickered
			// open, or a hand that closed again at once. Refusing the equip here is what
			// left HIGGS holding the weapon loose, which is why that hand could not pull
			// and the weapon shook. Simple mode's drops are deliberate double taps, so a
			// quick re-close there is left alone.
			if (!simpleMode && sameHand && gripClosed && hand.lastReleaseSpeed < kStillReleaseMps &&
				sinceDrop <= kFlickerCatchSeconds)
			{
				LOG_INFO(
					"GripHold: the %s hand caught what it let go of %.2fs ago with its grip closed "
					"after a still release (%.2f m/s) - equipping it again instead of leaving HIGGS "
					"holding it loose.",
					isLeftVRController ? "left" : "right",
					sinceDrop,
					hand.lastReleaseSpeed);
				return false;
			}

			LOG_INFO(
				"GripHold: %s hand grab of refId=%08X refused by the re-grab cooldown (%s hand dropped "
				"it %.2fs ago at %.2f m/s, grip reads %s) - HIGGS holds it loose.",
				isLeftVRController ? "left" : "right",
				grabbedRefr->formID,
				sameHand ? "the same" : "the other",
				sinceDrop,
				hand.lastReleaseSpeed,
				gripClosed ? "closed" : "open");
			NoteLooseHold(isLeftVRController, grabbedRefr, sameHand, LooseHoldRule::Why::kCooldown);
			return true;
		}

		return false;
	}

	bool GripHoldIsGripPressed(const bool isLeftVRController)
	{
		return IsGripPressed(isLeftVRController);
	}

	bool GripHoldIsTriggerPressed(const bool isLeftVRController)
	{
		return IsTriggerPressed(isLeftVRController);
	}

	void GripHoldOnPreLoadGame()
	{
		s_gripHands[0] = GripHand{};
		s_gripHands[1] = GripHand{};
		s_looseHolds[0] = LooseHold{};
		s_looseHolds[1] = LooseHold{};
		ThrowResetAll();
		EmbedOnPreLoadGame();
		RestoreAllWeaponSounds();

		for (int i = 0; i < kMaxLoopTracked; ++i)
		{
			s_regrabLoop[i] = RegrabLoopEntry{};
		}
	}
}
