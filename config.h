#pragma once
#include <iostream>
#include <string>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <skse64/NiProperties.h>
#include <skse64/NiNodes.h>

#include "skse64\GameSettings.h"
#include "Utility.hpp"

#include <skse64/GameData.h>

#include "higgsinterface001.h"
#include "vrikinterface001.h"
#include "SkyrimVRESLAPI.h"
#include "spellwheelinterface001.h"

namespace SwapDropAndHoldRedux {

	constexpr const char* PLUGIN_FILE_NAME = "SwapDropAndHoldRedux";
	constexpr const char* PLUGIN_DISPLAY_NAME = "Swap Drop & Hold redux";

	const UInt32 MOD_VERSION = 0x30000;
	const std::string MOD_VERSION_STR = "3.0.0";
	extern int leftHandedMode;

	extern int logging;
	extern float triggerHoldDropSeconds;
	extern float dropGuardTimeoutSeconds;
	extern float swapPullSpeedThreshold;
	extern float swapPullMinDistanceGrowth;
	extern bool enableTwoHandedWeapons;
	extern bool enableTwoHandedHandSwapping;
	extern bool enableStaves;
	extern bool enableTorches;
	extern bool enableShields;
	extern bool enableShieldSwapping;
	extern bool requireDoubleTapForBowCrossbowDrop;
	extern int dropButtonId;
	extern const char* dropButtonName;

	// --- Weapon mode: which of the three ways of holding a weapon is active ---
	//   Legacy   - the original Swap Drop & Hold: hold the trigger to drop, pull to swap.
	//   GripHold - a weapon stays only while grip holds it; opening the hand drops it.
	//   Simple   - grip grabs and equips, the weapon stays, a double tap of grip drops it.
	enum WeaponMode
	{
		kWeaponModeLegacy = 0,
		kWeaponModeGripHold = 1,
		kWeaponModeSimple = 2,
	};
	extern int weaponMode;
	const char* WeaponModeName(int mode);

	// Derived from weaponMode after every load, never parsed. gripHoldMode means "the
	// GripHold engine runs" - true in Grip Hold AND Simple - which is exactly what every
	// check written before Simple existed was asking.
	extern bool gripHoldMode;
	extern bool simpleMode;
	// Simple mode: longest time from the first press of a double tap to the second.
	extern float doubleTapWindow;
	// Simple mode's double tap: does a touched grip count as closed? -1 = decide from the
	// controller (Index: no, everything else: yes), 0 = never, 1 = always, as in 2.2.0 and
	// before. GripTouchRules.h.
	extern int doubleTapCountsTouch;
	// 3.0.0 grab probe, ini only (GrabPose.h): log where and which way round a weapon was taken
	// hold of; also pose HIGGS's hold to match; how close to dead backwards counts as reversed.
	// 3.0.0: tell a listening grip driver (TrueWield VR) where a weapon was taken hold of, and whether
	// a backwards grab may be reported as reversed. GrabGripRules.h.
	// Going through a door: stand the mod down while the loading screen is up, and for a moment
	// after the new cell appears, so nothing in your hands is dropped. CellLoadGuardRules.h.
	extern bool cellLoadLock;
	extern float cellLoadLockSeconds;
	extern bool grabGrip;
	extern bool grabGripReversed;
	// M3.0.0-test14: the Simple and Grip Hold menu pages' own "Holding a weapon you pick up" switches.
	// grabGrip and grabGripReversed above are what LEGACY mode uses. GrabGripRules.h ChooseForMode.
	extern bool grabGripSimple;
	extern bool grabGripReversedSimple;
	extern bool grabGripReversedTwoHandedSimple;
	extern bool grabGripGripHold;
	extern bool grabGripReversedGripHold;
	extern bool grabGripReversedTwoHandedGripHold;
	// M3.0.0-test15: "Hold anywhere on the weapon" per mode - held where it was taken, above the crossguard too
	// (test16: never on the guard or head, never slid through it - PCVR keeps the side it was taken on).
	extern bool grabGripAnywhereSimple;
	extern bool grabGripAnywhereGripHold;
	// A weapon PULLED through the air lands in the hand by chance, not by choice. On, that chance
	// position is kept anyway; off, a pulled weapon is held the natural way. GrabGripRules.h.
	extern bool grabGripPulled;
	// And whether such a pull may come out REVERSED: judged like any grab, by which way round it arrives
	// against how the hand is turned at the catch. Only matters with grabGripPulled on. GrabGripRules.h.
	extern bool grabGripReversePulled;
	extern bool grabGripTrace;
	extern bool grabProbeSetTransform;
	extern float grabGripReversedCone;
	// Both modes: how long a weapon grabbed with HIGGS stays a loose object before it is picked
	// up and equipped. 0 = at once, the behaviour before this setting. GrabDelayRules.h.
	extern float grabEquipDelay;
	// M3.0.0-test15: the same for a weapon it would be a crime to take (the red "Steal" prompt): at least
	// this long. 0 = no extra wait. GrabDelayRules.h DelayFor.
	extern float grabEquipDelayStolen;

	// --- GripHold engine (WPVR merge): grip-held weapons, throw momentum, IWP embed ---
	extern bool throwMomentum;
	extern float throwVelocityScale;
	extern float throwWindow;
	extern int throwStampFrames;
	extern float throwMaxSpeed;
	extern bool stickOnRelease;
	extern float embedMaxSeconds;
	extern float embedSmoothing;
	extern float embedRadius;
	extern float pullReach;
	extern float pullOutDistance;
	extern bool pullOut;
	extern bool resumeIwpEmbed;
	extern float resumeDrawGuard;
	extern float minHoldTime;
	extern float regrabCooldown;
	extern int regrabLoopLimit;
	extern float regrabLoopWindow;
	extern float regrabLoopCooldown;
	extern float lockClickWindow;
	extern int lockClickCount;
	extern float lockChordCooldown;
	extern float lockGripGrace;
	extern bool lockNotifications;
	extern bool lockRequiresSheathed;

	// Grip + a longer run of clicks puts what is in your hand on the blacklist, so
	// nobody has to look up a form id to exclude their favourite sword.
	extern bool blacklistGesture;
	extern int blacklistClickCount;
	extern float throwVrYieldSeconds;
	// After a Grip Hold pickup other mods are shown the grip released for 0.10s, for Weapon Throw VR.
	// On, the grip is shown pressed again only once the hand is clear of anything VR Climbing climbs,
	// so the re-press cannot start a climb on the table the weapon was lying on. GripBlinkRules.h.
	extern bool blinkWaitsForClearHand;
	// And the GAME is handed the real grip while the other mods see the blink (M3.0.0-test9): Skyrim VR binds
	// Ready Weapon to the Oculus LEFT grip, so the re-press put left-hand weapons away. GameGripRules.h.
	extern bool blinkHiddenFromGame;
	extern bool throwTrace;
	extern float gripReleaseGraceSeconds;
	// Which OpenVR button counts as "hand closed". Default bit 2 (k_EButton_Grip).
	// A setup that grabs with something else can add its bit here rather than wait
	// for a build. Mask is rebuilt from the bit list at load.
	extern uint64_t gripButtonMask;
	extern float dropLateralOffset;
	extern float dropDownOffset;
	extern bool holsterAware;
	// Grip hold with VRIK 0.8.7 or newer: letting go at a holster VRIK says takes the weapon holsters it.
	extern bool holsterOnRelease;
	extern bool requireDrawnWeapon;
	extern int holsterAction;
	extern bool holsterTrace;
	extern float holsterExtraRange;
	extern float holsterExtraRangeBehind;
	extern float holsterReleaseMargin;
	extern bool holsterLearning;
	// TEST: measure holsters from the third-person body (GetNiRootNode(0)) with VRIK's own slot
	// positions, instead of the first-person skeleton with learned ones. HolsterBodyRules.h.
	extern bool holsterThirdPersonBody;
	// Old VRIK, Grip hold: once VRIK's holster haptics have buzzed this session, its buzz - the answer
	// its 0.8.7 API gives - governs the stand-aside instead of this mod's estimate. VrikPulseRules.h.
	extern bool holsterListenToVrik;
	// The in-game menu fills itself in during its first session (McmWatch.h). Both ini-only.
	extern bool mcmSelfRepair;     // 0 = only report a blank menu in the log
	extern bool mcmRepairSelfTest; // TEST: blank a working menu once per load so the repair can be watched
	extern bool holsterHaptics;
	extern int holsterHapticMicroseconds;
	extern bool holsterScanSpheres;
	extern bool holsterDumpNodes;
	extern bool useIwpWoundData;
	extern bool embedNoCollision;
	extern bool embedTrace;
	extern bool equipSounds;
	extern bool weaponDrawOnEquip;
	extern bool weaponSkeletonRefresh;
	// A weapon picked up with a grab is told to draw, but that is a REQUEST the engine can
	// swallow, leaving it equipped and SHEATHED - invisible, and not in your hand. On, the
	// draw is followed and asked for again while it has not landed. DrawWatchRules.h.
	extern bool grabDrawVerify;
	extern float grabDrawVerifySeconds;
	extern bool grabDrawTrace;
	// How much closer to the bone a blade's TIP must be than its GRIP before letting go near
	// somebody counts as a stab. Stops a weapon released beside a body being pinned in mid
	// air. 0 = the behaviour before this rule. EmbedBuryRules.h.
	extern float embedMinBuryUnits;
	// And the blade's TIP must be inside one of the victim's body parts (their skeleton's own collision
	// bodies): a blade merely POINTED at somebody passes the gap test too. EmbedBuryRules.h.
	extern bool embedNeedsTipInBody;
	extern int weaponRefreshDelayFrames;
	extern float weaponRefreshMinInterval;
	extern float dropWatchSeconds;
	extern float dropRemoveDelaySeconds;
	extern float equipSilenceSeconds;

	// Weapons excluded from all Swap Drop & Hold handling, identified by owning
	// plugin + base form id (load-order independent). Built-in defaults plus
	// ini entries: ExcludedForms = Plugin.esp:0xFORMID, Other.esp:0xFORMID
	struct FormExclusionEntry
	{
		std::string espName;
		UInt32 baseFormId;
	};
	extern std::vector<FormExclusionEntry> excludedFormEntries;

	void loadConfig();
	
	void Log(const int msgLogLevel, const char* fmt, ...);
	enum eLogLevels
	{
		LOGLEVEL_ERR = 0,
		LOGLEVEL_WARN,
		LOGLEVEL_INFO,
	};


#define LOG(fmt, ...) Log(LOGLEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) Log(LOGLEVEL_ERR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) Log(LOGLEVEL_INFO, fmt, ##__VA_ARGS__)


}