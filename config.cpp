#include "config.h"

#include <chrono>
#include <sstream>

#include <algorithm>
#include <cctype>

namespace SwapDropAndHoldRedux {
		
	int logging = 2;
    int leftHandedMode = 0;
	float triggerHoldDropSeconds = 2.0f;
	float dropGuardTimeoutSeconds = 4.0f;
	float swapPullSpeedThreshold = 40.0f;
	float swapPullMinDistanceGrowth = 10.0f;
	bool enableTwoHandedWeapons = false;
	bool enableTwoHandedHandSwapping = false;
	bool enableStaves = true;
	bool enableTorches = false;
	bool enableShields = true;
	bool enableShieldSwapping = false;
	bool requireDoubleTapForBowCrossbowDrop = true;
	int dropButtonId = 33; // OpenVR k_EButton_SteamVR_Trigger / Axis1
	const char* dropButtonName = "Trigger";

	// Simple is the default from 2.1.0. gripHoldMode and simpleMode are derived from it
	// after every load (ApplyWeaponMode) and never parsed on their own.
	int weaponMode = kWeaponModeSimple;
	bool gripHoldMode = true;
	bool simpleMode = true;
	float doubleTapWindow = 0.40f;
	int doubleTapCountsTouch = -1;
	bool cellLoadLock = true;
	float cellLoadLockSeconds = 1.0f;
	bool grabGrip = true;
	bool grabGripReversed = true;
	bool grabGripSimple = true;
	bool grabGripReversedSimple = true;
	bool grabGripReversedTwoHandedSimple = false;
	bool grabGripGripHold = true;
	bool grabGripReversedGripHold = true;
	bool grabGripReversedTwoHandedGripHold = false;
	bool grabGripAnywhereSimple = false;
	bool grabGripAnywhereGripHold = false;
	bool grabGripPulled = true;
	bool grabGripReversePulled = true;
	bool grabGripTrace = false;
	bool grabProbeSetTransform = false;
	float grabGripReversedCone = 90.0f;
	float grabEquipDelay = 0.0f;
	float grabEquipDelayStolen = 10.0f;

	// --- GripHold engine (WPVR merge). Defaults are WPVR 0.8.0's shipped values. ---
	bool throwMomentum = true;
	float throwVelocityScale = 1.0f;
	float throwWindow = 0.15f;    // s of pose history the release speed is taken from
	int throwStampFrames = 8;     // frames to keep re-asserting the throw velocity
	float throwMaxSpeed = 25.0f;  // m/s ceiling, so one bad frame cannot launch a weapon
	bool stickOnRelease = true;
	float embedMaxSeconds = 0.0f; // 0 = a lodged weapon stays until something disturbs it
	float embedSmoothing = 0.4f;  // 0 = raw bone pose, ->1 = heavily damped
	float embedRadius = 35.0f;    // units from a blade probe point to a victim's bone
	float pullReach = 30.0f;      // units: how close a hand must be to take hold of a lodged weapon
	float pullOutDistance = 22.0f; // units to draw along the blade to free it
	bool pullOut = true;
	bool resumeIwpEmbed = true;
	float resumeDrawGuard = 2.0f; // s keeping the weapon drawn after an IWP resume
	float minHoldTime = 0.1f;     // s a weapon must be held before a release can drop it
	float regrabCooldown = 0.3f;  // s before a just-dropped weapon can auto-equip again
	// Loop guard: this many grab-equips of one weapon within regrabLoopWindow means it
	// is being caught the instant it is dropped, so stop auto-equipping it for
	// regrabLoopCooldown. 0 disables it. Deliberately generous so that ordinary fast
	// play never reaches it.
	int regrabLoopLimit = 5;
	uint64_t gripButtonMask = (1ull << 2) | (1ull << 34);
	bool holsterAware = true;
	bool holsterOnRelease = true;
	bool requireDrawnWeapon = true;
	int holsterAction = 0;
	bool holsterTrace = true;
	float holsterExtraRange = 6.0f;
	float holsterExtraRangeBehind = 14.0f;
	float holsterReleaseMargin = 4.0f;
	bool holsterLearning = true;
	bool holsterThirdPersonBody = false;
	bool holsterListenToVrik = true;
	bool mcmSelfRepair = true;
	bool mcmRepairSelfTest = false;
	bool holsterHaptics = true;
	int holsterHapticMicroseconds = 1800;
	bool holsterScanSpheres = false;
	bool holsterDumpNodes = false;
	float regrabLoopWindow = 2.0f;
	float regrabLoopCooldown = 2.0f;
	// Max gap between consecutive clicks of the lock chord, and how many clicks
	// it takes. Three makes it much harder to fire by accident in combat.
	float lockClickWindow = 0.35f;
	int lockClickCount = 3;
	float lockChordCooldown = 1.0f;
	float lockGripGrace = 0.25f;
	bool lockNotifications = true;
	// Off by default from 2.0.2. Locking used to demand a sheathed weapon, which
	// also meant it was unreachable in a fight - and a fight is exactly when you
	// notice you want the mod out of the way. The old requirement is still here for
	// anyone who kept fumbling it mid-combat; it is just no longer the default.
	bool lockRequiresSheathed = false;
	bool blacklistGesture = true;
	int blacklistClickCount = 5;
	// 0 now. This bought a window for another mod to claim the release gesture
	// before we dropped, and it was measured costing 0.157s on every single drop
	// while buying nothing - the interop it existed for does not work.
	float throwVrYieldSeconds = 0.0f;
	bool blinkWaitsForClearHand = true;
	bool blinkHiddenFromGame = true;
	bool throwTrace = true;
	float gripReleaseGraceSeconds = 0.03f; // just enough to debounce grip flicker
	// Skyrim units a dropped weapon is nudged sideways so it does not spawn inside
	// the hand. Positive = toward the body's midline (left of the right hand,
	// right of the left hand); negative pushes it outward instead.
	float dropLateralOffset = 3.0f;
	// 3.0.0: Skyrim units a dropped weapon is moved straight DOWN, so it starts a little below the palm instead of
	// inside the hand. The sideways nudge cannot clear a weapon lying across a palm-down hand - it only slides it
	// along its own length (DropNudgeRules.h). 0 = off.
	float dropDownOffset = 6.0f;
	// Ask IWP for the exact wound on release. Turn OFF to use the built-in
	// geometric stab detection instead, which never calls into IWP at all.
	bool useIwpWoundData = true;
	// Drop a lodged weapon out of the collision filter so it stops trading
	// impulses with the ragdoll it is stuck in.
	bool embedNoCollision = true;
	// Verbose step-by-step logging through the stab/pin path.
	bool embedTrace = false;
	// Vanilla pickup/putdown sounds when a weapon is grabbed or dropped. Off by
	// default: grip-hold picks weapons up and drops them constantly, and hearing
	// the equip chime every time gets tiring fast.
	bool equipSounds = false;
	// Force the weapon out of the sheath after a grab-equip (Sam's original
	// behaviour, needed or a grabbed weapon can stay invisible). This is what
	// plays the unsheathe sound and runs the draw animation.
	bool weaponDrawOnEquip = true;
	// Full skeleton refresh after a grab-equip. Sam's original behaviour, but it
	// visibly jolts the hand on every grab and testing showed nothing depends on
	// it, so it is off by default here.
	bool weaponSkeletonRefresh = false;
	bool grabDrawVerify = true;
	float grabDrawVerifySeconds = 5.0f;
	bool grabDrawTrace = true;
	float embedMinBuryUnits = 6.0f;
	bool embedNeedsTipInBody = true;
	int weaponRefreshDelayFrames = 3;
	float weaponRefreshMinInterval = 0.35f;
	float dropWatchSeconds = 3.0f;
	float dropRemoveDelaySeconds = 0.05f;
	// How long a weapon's own sounds stay muted around a grab or drop. The engine
	// plays some of them a frame or two after the equip call returns, so this has
	// to outlast the call itself.
	float equipSilenceSeconds = 1.0f;

	// Built-in defaults; ini ExcludedForms entries are appended to this list.
	std::vector<FormExclusionEntry> excludedFormEntries = {
		{ "VR_ImmersiveSmithing.esp", 0x005901 }, // Blacksmith Hammer
	};

	namespace
	{
		bool ParseBoolSetting(const std::string& rawValue)
		{
			// Tolerate inline comments ("1 ; comment") and stray whitespace —
			// strict equality here silently disabled features for users.
			std::string value = rawValue;
			const size_t commentPos = value.find_first_of(";#");
			if (commentPos != std::string::npos)
			{
				value.erase(commentPos);
			}
			value.erase(0, value.find_first_not_of(" \t\r\n"));
			const size_t lastChar = value.find_last_not_of(" \t\r\n");
			value.erase(lastChar == std::string::npos ? 0 : lastChar + 1);

			return value == "1" || value == "true" || value == "True" || value == "TRUE";
		}

		float ParseFloatSetting(const std::string& rawValue, const float minValue, const float maxValue, const float fallback)
		{
			try
			{
				float value = std::stof(rawValue);
				if (value < minValue)
				{
					value = minValue;
				}
				else if (value > maxValue)
				{
					value = maxValue;
				}
				return value;
			}
			catch (...)
			{
				return fallback;
			}
		}

		std::string ToLowerCopy(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		std::string TrimAndStripComment(const std::string& rawValue)
		{
			std::string value = rawValue;
			const size_t commentPos = value.find_first_of(";#");
			if (commentPos != std::string::npos)
			{
				value.erase(commentPos);
			}
			value.erase(0, value.find_first_not_of(" \t\r\n"));
			const size_t lastChar = value.find_last_not_of(" \t\r\n");
			value.erase(lastChar == std::string::npos ? 0 : lastChar + 1);
			return value;
		}

		// Accepts "Plugin.esp:0xFORMID" or "Plugin.esp|FORMID" tokens separated by
		// commas. Form ids are hex, with or without the 0x prefix, and are masked
		// to the plugin-relative base id (load-order prefix is ignored).
		void ParseExcludedFormsList(const std::string& rawValue)
		{
			const std::string value = TrimAndStripComment(rawValue);
			if (value.empty())
			{
				return;
			}

			size_t tokenStart = 0;
			while (tokenStart <= value.size())
			{
				size_t tokenEnd = value.find(',', tokenStart);
				if (tokenEnd == std::string::npos)
				{
					tokenEnd = value.size();
				}

				std::string token = TrimAndStripComment(value.substr(tokenStart, tokenEnd - tokenStart));
				tokenStart = tokenEnd + 1;

				if (token.empty())
				{
					continue;
				}

				const size_t sepPos = token.find_last_of(":|");
				if (sepPos == std::string::npos || sepPos == 0 || sepPos + 1 >= token.size())
				{
					_MESSAGE("ExcludedForms: skipping malformed entry \"%s\" (expected Plugin.esp:0xFORMID).", token.c_str());
					continue;
				}

				std::string espName = TrimAndStripComment(token.substr(0, sepPos));
				std::string formIdStr = TrimAndStripComment(token.substr(sepPos + 1));
				if (espName.empty() || formIdStr.empty())
				{
					_MESSAGE("ExcludedForms: skipping malformed entry \"%s\" (expected Plugin.esp:0xFORMID).", token.c_str());
					continue;
				}

				UInt32 baseFormId = 0;
				try
				{
					baseFormId = static_cast<UInt32>(std::stoul(formIdStr, nullptr, 16)) & 0x00FFFFFF;
				}
				catch (...)
				{
					_MESSAGE("ExcludedForms: skipping entry \"%s\" — could not parse form id \"%s\" as hex.", token.c_str(), formIdStr.c_str());
					continue;
				}

				if (baseFormId == 0)
				{
					_MESSAGE("ExcludedForms: skipping entry \"%s\" — form id resolved to 0.", token.c_str());
					continue;
				}

				excludedFormEntries.push_back({ espName, baseFormId });
				_MESSAGE("ExcludedForms: added exclusion %s : %06X.", espName.c_str(), baseFormId);
			}
		}

		bool TryParseDropButton(const std::string& rawValue, int& outButtonId, const char*& outName)
		{
			const std::string value = ToLowerCopy(rawValue);

			if (value == "trigger" || value == "axis1" || value == "steamvr_trigger")
			{
				outButtonId = 33;
				outName = "Trigger";
				return true;
			}
			if (value == "grip" || value == "squeeze")
			{
				outButtonId = 2;
				outName = "Grip";
				return true;
			}
			if (value == "a" || value == "button_a" || value == "x" || value == "button_x")
			{
				outButtonId = 7;
				outName = "A";
				return true;
			}
			if (value == "b" || value == "button_b" || value == "y" || value == "button_y" ||
				value == "menu" || value == "applicationmenu" || value == "appmenu")
			{
				outButtonId = 1;
				outName = "B/Menu";
				return true;
			}
			if (value == "stick" || value == "stickclick" || value == "joystickclick" ||
				value == "touchpad" || value == "touchpadclick" || value == "pad" ||
				value == "padclick" || value == "axis0" || value == "thumbstick")
			{
				outButtonId = 32;
				outName = "Stick/Touchpad";
				return true;
			}
			if (value == "joystick" || value == "indexjoystick" || value == "axis3")
			{
				outButtonId = 35;
				outName = "Joystick";
				return true;
			}
			if (value == "dpadup" || value == "padup" || value == "up")
			{
				outButtonId = 4;
				outName = "DPadUp";
				return true;
			}
			if (value == "dpaddown" || value == "paddown" || value == "down")
			{
				outButtonId = 6;
				outName = "DPadDown";
				return true;
			}
			if (value == "dpadleft" || value == "padleft" || value == "left")
			{
				outButtonId = 3;
				outName = "DPadLeft";
				return true;
			}
			if (value == "dpadright" || value == "padright" || value == "right")
			{
				outButtonId = 5;
				outName = "DPadRight";
				return true;
			}
			if (value == "system")
			{
				outButtonId = 0;
				outName = "System";
				return true;
			}

			// Raw OpenVR button id (0-63)
			try
			{
				const int id = std::stoi(value);
				if (id >= 0 && id <= 63)
				{
					outButtonId = id;
					outName = "Custom";
					return true;
				}
			}
			catch (...)
			{
			}

			return false;
		}
	}

    // Parses one ini into the settings globals. Called twice - the shipped
    // defaults first, then the MCM's own file - so a value the player set in the
    // menu overrides the default simply by being assigned second. With MCM Helper
    // absent the second file does not exist and behaviour is exactly the defaults.
    static bool ParseConfigFile(const std::string& path);

    // Whether this load has seen a WeaponMode line, and what a pre-2.1.0 GripHoldMode
    // line said, across BOTH files. Reset at the start of every load.
    static bool s_weaponModeRead = false;
    static bool s_legacyGripHoldModeRead = false;
    static bool s_legacyGripHoldMode = true;

    const char* WeaponModeName(const int mode)
    {
        switch (mode)
        {
        case kWeaponModeLegacy:
            return "Legacy";
        case kWeaponModeGripHold:
            return "Grip Hold";
        case kWeaponModeSimple:
            return "Simple";
        default:
            return "unknown";
        }
    }

    // Settles the mode once both files are read. WeaponMode wins wherever it appears; the
    // old on/off GripHoldMode only counts when neither file has a WeaponMode line, so a
    // hand-edited pre-2.1.0 ini keeps the mode it chose instead of quietly turning Simple.
    static void ApplyWeaponMode()
    {
        if (!s_weaponModeRead && s_legacyGripHoldModeRead)
        {
            weaponMode = s_legacyGripHoldMode ? kWeaponModeGripHold : kWeaponModeLegacy;
            _MESSAGE(
                "No WeaponMode line - honouring the pre-2.1.0 GripHoldMode = %d, which is %s.",
                s_legacyGripHoldMode ? 1 : 0,
                WeaponModeName(weaponMode));
        }

        gripHoldMode = weaponMode != kWeaponModeLegacy;
        simpleMode = weaponMode == kWeaponModeSimple;

        if (simpleMode)
        {
            _MESSAGE(
                "Weapon mode: Simple - grip grabs and the weapon stays; a double tap of grip (window %.2fs) lets go.",
                doubleTapWindow);
        }
        else
        {
            _MESSAGE("Weapon mode: %s.", WeaponModeName(weaponMode));
        }
    }

    void loadConfig()
    {
        // The menu re-reads this on every journal close, and the ini's own
        // ExcludedForms entries are APPENDED to the built-in ones - so without a
        // reset the list would grow by its own length every time the player
        // closed a menu.
        static size_t builtinExclusions = static_cast<size_t>(-1);
        if (builtinExclusions == static_cast<size_t>(-1))
        {
            builtinExclusions = excludedFormEntries.size();
        }
        else if (excludedFormEntries.size() > builtinExclusions)
        {
            excludedFormEntries.resize(builtinExclusions);
        }

        const std::string runtimeDirectory = GetRuntimeDirectory();
        if (runtimeDirectory.empty())
        {
            return;
        }

        s_weaponModeRead = false;
        s_legacyGripHoldModeRead = false;

        ParseConfigFile(runtimeDirectory + "Data\\SKSE\\Plugins\\" + PLUGIN_FILE_NAME + ".ini");
        const bool mcmRead =
            ParseConfigFile(runtimeDirectory + "Data\\MCM\\Settings\\" + PLUGIN_FILE_NAME + ".ini");

        ApplyWeaponMode();

        _MESSAGE("Config: defaults loaded; MCM overrides %s.",
            mcmRead ? "applied" : "not present (menu unused, or MCM Helper not installed)");
    }

    static bool ParseConfigFile(const std::string& path)
    {
        {
            std::string filepath = path;
            std::ifstream file(filepath);

            if (!file.is_open()) 
            {
                transform(filepath.begin(), filepath.end(), filepath.begin(), ::tolower);
                file.open(filepath);
            }

			bool legacyUseGripForDrop = false;
			bool sawDropButton = false;

            if (file.is_open()) 
            {
                std::string line;
                std::string currentSection;

                bool firstLine = true;
                while (std::getline(file, line)) 
                {
                    if (firstLine)
                    {
                        firstLine = false;
                        stripUtf8Bom(line);
                    }

                    trim(line);
                    skipComments(line);

                    if (line.empty()) continue;

                    if (line[0] == '[') 
                    {
                        // New section
                        size_t endBracket = line.find(']');
                        if (endBracket != std::string::npos) 
                        {
                            currentSection = line.substr(1, endBracket - 1);
                            trim(currentSection);                            
                        }
                    }
                    else if (currentSection == "Settings") 
                    {
                        std::string variableName;
                        std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);
                        trim(variableValueStr);
                        stripHungarianPrefix(variableName);

                        if (variableName == "Logging") 
                        {
                            logging = std::stoi(variableValueStr);
                        }
                        else if (variableName == "TriggerHoldDropSeconds")
                        {
                            triggerHoldDropSeconds = std::stof(variableValueStr);
                            if (triggerHoldDropSeconds < 0.25f)
                            {
                                triggerHoldDropSeconds = 0.25f;
                            }
                            else if (triggerHoldDropSeconds > 10.0f)
                            {
                                triggerHoldDropSeconds = 10.0f;
                            }
                        }
                        else if (variableName == "DropGuardTimeoutSeconds")
                        {
                            dropGuardTimeoutSeconds = std::stof(variableValueStr);
                            if (dropGuardTimeoutSeconds < 0.5f)
                            {
                                dropGuardTimeoutSeconds = 0.5f;
                            }
                            else if (dropGuardTimeoutSeconds > 30.0f)
                            {
                                dropGuardTimeoutSeconds = 30.0f;
                            }
                        }
                        else if (variableName == "SwapPullSpeedThreshold")
                        {
                            swapPullSpeedThreshold = std::stof(variableValueStr);
                            if (swapPullSpeedThreshold < 25.0f)
                            {
                                swapPullSpeedThreshold = 25.0f;
                            }
                            else if (swapPullSpeedThreshold > 500.0f)
                            {
                                swapPullSpeedThreshold = 500.0f;
                            }
                        }
                        else if (variableName == "SwapPullMinDistanceGrowth")
                        {
                            swapPullMinDistanceGrowth = std::stof(variableValueStr);
                            if (swapPullMinDistanceGrowth < 3.0f)
                            {
                                swapPullMinDistanceGrowth = 3.0f;
                            }
                            else if (swapPullMinDistanceGrowth > 100.0f)
                            {
                                swapPullMinDistanceGrowth = 100.0f;
                            }
                        }
                        else if (variableName == "EnableTwoHandedWeapons")
                        {
                            enableTwoHandedWeapons = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EnableTwoHandedHandSwapping")
                        {
                            enableTwoHandedHandSwapping = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EnableStaves")
                        {
                            enableStaves = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EnableTorches")
                        {
                            enableTorches = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EnableShields")
                        {
                            enableShields = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EnableShieldSwapping")
                        {
                            enableShieldSwapping = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "RequireDoubleTapForBowCrossbowDrop")
                        {
                            requireDoubleTapForBowCrossbowDrop = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "WeaponMode")
                        {
                            int mode = weaponMode;
                            try { mode = std::stoi(variableValueStr); } catch (...) {}
                            if (mode < kWeaponModeLegacy) { mode = kWeaponModeLegacy; }
                            if (mode > kWeaponModeSimple) { mode = kWeaponModeSimple; }
                            weaponMode = mode;
                            s_weaponModeRead = true;
                        }
                        else if (variableName == "GripHoldMode")
                        {
                            // Before 2.1.0 this on/off switch WAS the mode. ApplyWeaponMode
                            // honours it only when no WeaponMode line exists.
                            s_legacyGripHoldMode = ParseBoolSetting(variableValueStr);
                            s_legacyGripHoldModeRead = true;
                        }
                        else if (variableName == "DoubleTapWindow")
                        {
                            doubleTapWindow = ParseFloatSetting(variableValueStr, 0.2f, 0.8f, doubleTapWindow);
                        }
                        else if (variableName == "DoubleTapCountsTouch")
                        {
                            try { doubleTapCountsTouch = std::stoi(variableValueStr); } catch (...) {}
                            if (doubleTapCountsTouch < -1) { doubleTapCountsTouch = -1; }
                            else if (doubleTapCountsTouch > 1) { doubleTapCountsTouch = 1; }
                        }
                        else if (variableName == "CellLoadLock")
                        {
                            cellLoadLock = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "CellLoadLockSeconds")
                        {
                            cellLoadLockSeconds = ParseFloatSetting(variableValueStr, 0.0f, 10.0f, cellLoadLockSeconds);
                        }
                        else if (variableName == "GrabGrip")
                        {
                            grabGrip = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripReversed")
                        {
                            grabGripReversed = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripSimple")
                        {
                            grabGripSimple = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripReversedSimple")
                        {
                            grabGripReversedSimple = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripReversedTwoHandedSimple")
                        {
                            grabGripReversedTwoHandedSimple = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripGripHold")
                        {
                            grabGripGripHold = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripReversedGripHold")
                        {
                            grabGripReversedGripHold = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripReversedTwoHandedGripHold")
                        {
                            grabGripReversedTwoHandedGripHold = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripAnywhereSimple")
                        {
                            grabGripAnywhereSimple = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripAnywhereGripHold")
                        {
                            grabGripAnywhereGripHold = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripPulled")
                        {
                            grabGripPulled = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripReversePulled")
                        {
                            grabGripReversePulled = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripTrace")
                        {
                            grabGripTrace = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabProbeSetTransform")
                        {
                            grabProbeSetTransform = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabGripReversedCone")
                        {
                            grabGripReversedCone = ParseFloatSetting(variableValueStr, 20.0f, 90.0f, grabGripReversedCone);
                        }
                        else if (variableName == "GrabEquipDelay")
                        {
                            grabEquipDelay = ParseFloatSetting(variableValueStr, 0.0f, 10.0f, grabEquipDelay);
                        }
                        else if (variableName == "GrabEquipDelayStolen")
                        {
                            grabEquipDelayStolen = ParseFloatSetting(variableValueStr, 0.0f, 20.0f, grabEquipDelayStolen);
                        }
                        else if (variableName == "ThrowMomentum")
                        {
                            throwMomentum = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "ThrowVelocityScale")
                        {
                            throwVelocityScale = ParseFloatSetting(variableValueStr, 0.0f, 10.0f, throwVelocityScale);
                        }
                        else if (variableName == "ThrowWindow")
                        {
                            throwWindow = ParseFloatSetting(variableValueStr, 0.02f, 1.0f, throwWindow);
                        }
                        else if (variableName == "ThrowStampFrames")
                        {
                            try { throwStampFrames = std::stoi(variableValueStr); } catch (...) {}
                            if (throwStampFrames < 1) { throwStampFrames = 1; }
                            else if (throwStampFrames > 30) { throwStampFrames = 30; }
                        }
                        else if (variableName == "ThrowMaxSpeed")
                        {
                            throwMaxSpeed = ParseFloatSetting(variableValueStr, 1.0f, 100.0f, throwMaxSpeed);
                        }
                        else if (variableName == "StickOnRelease")
                        {
                            stickOnRelease = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EmbedMaxSeconds")
                        {
                            embedMaxSeconds = ParseFloatSetting(variableValueStr, 0.0f, 600.0f, embedMaxSeconds);
                        }
                        else if (variableName == "EmbedSmoothing")
                        {
                            embedSmoothing = ParseFloatSetting(variableValueStr, 0.0f, 0.95f, embedSmoothing);
                        }
                        else if (variableName == "EmbedRadius")
                        {
                            embedRadius = ParseFloatSetting(variableValueStr, 2.0f, 100.0f, embedRadius);
                        }
                        else if (variableName == "PullReach")
                        {
                            pullReach = ParseFloatSetting(variableValueStr, 5.0f, 100.0f, pullReach);
                        }
                        else if (variableName == "PullOutDistance")
                        {
                            pullOutDistance = ParseFloatSetting(variableValueStr, 5.0f, 200.0f, pullOutDistance);
                        }
                        else if (variableName == "PullOut")
                        {
                            pullOut = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "ResumeIwpEmbed")
                        {
                            resumeIwpEmbed = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "ResumeDrawGuard")
                        {
                            resumeDrawGuard = ParseFloatSetting(variableValueStr, 0.0f, 5.0f, resumeDrawGuard);
                        }
                        else if (variableName == "MinHoldTime")
                        {
                            minHoldTime = ParseFloatSetting(variableValueStr, 0.0f, 5.0f, minHoldTime);
                        }
                        else if (variableName == "RequireDrawnWeapon")
                        {
                            requireDrawnWeapon = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterAware")
                        {
                            holsterAware = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterOnRelease")
                        {
                            holsterOnRelease = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterAction")
                        {
                            try { holsterAction = std::stoi(variableValueStr); } catch (...) {}
                            if (holsterAction < 0) { holsterAction = 0; }
                            else if (holsterAction > 2) { holsterAction = 2; }
                        }
                        else if (variableName == "HolsterTrace")
                        {
                            holsterTrace = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterScanSpheres")
                        {
                            holsterScanSpheres = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterDumpNodes")
                        {
                            holsterDumpNodes = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterLearning")
                        {
                            holsterLearning = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterThirdPersonBody")
                        {
                            holsterThirdPersonBody = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterListenToVrik")
                        {
                            holsterListenToVrik = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "McmSelfRepair")
                        {
                            mcmSelfRepair = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "McmRepairSelfTest")
                        {
                            mcmRepairSelfTest = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterHaptics")
                        {
                            holsterHaptics = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "HolsterHapticMicroseconds")
                        {
                            try { holsterHapticMicroseconds = std::stoi(variableValueStr); } catch (...) {}
                            if (holsterHapticMicroseconds < 0) { holsterHapticMicroseconds = 0; }
                            // OpenVR caps a single pulse at 3999us.
                            else if (holsterHapticMicroseconds > 3999) { holsterHapticMicroseconds = 3999; }
                        }
                        else if (variableName == "HolsterExtraRange")
                        {
                            holsterExtraRange = ParseFloatSetting(variableValueStr, 0.0f, 60.0f, holsterExtraRange);
                        }
                        else if (variableName == "HolsterExtraRangeBehind")
                        {
                            holsterExtraRangeBehind = ParseFloatSetting(variableValueStr, 0.0f, 60.0f, holsterExtraRangeBehind);
                        }
                        else if (variableName == "HolsterReleaseMargin")
                        {
                            holsterReleaseMargin = ParseFloatSetting(variableValueStr, 0.0f, 40.0f, holsterReleaseMargin);
                        }
                        else if (variableName == "GripButtonPreset")
                        {
                            // The menu cannot express a comma-separated bit list, so it
                            // offers the three combinations that actually occur. A
                            // GripButtonBits line later in the ini still wins, which is
                            // how an unusual controller gets set by hand.
                            int preset = 1;
                            try { preset = std::stoi(variableValueStr); } catch (...) {}
                            switch (preset)
                            {
                            case 0:  gripButtonMask = (1ull << 2); break;
                            case 2:  gripButtonMask = (1ull << 2) | (1ull << 34) | (1ull << 33); break;
                            default: gripButtonMask = (1ull << 2) | (1ull << 34); break;
                            }
                        }
                        else if (variableName == "GripButtonBits")
                        {
                            // Comma-separated OpenVR button indices, e.g. "2" or "2,33".
                            uint64_t parsed = 0;
                            std::stringstream bitStream(variableValueStr);
                            std::string bitToken;
                            while (std::getline(bitStream, bitToken, ','))
                            {
                                bitToken.erase(0, bitToken.find_first_not_of(" 	"));
                                const size_t end = bitToken.find_last_not_of(" 	");
                                if (end != std::string::npos) { bitToken.erase(end + 1); }
                                if (bitToken.empty()) { continue; }
                                try
                                {
                                    const int bit = std::stoi(bitToken);
                                    if (bit >= 0 && bit <= 63) { parsed |= (1ull << bit); }
                                }
                                catch (...) {}
                            }
                            if (parsed != 0) { gripButtonMask = parsed; }
                        }
                        else if (variableName == "RegrabLoopLimit")
                        {
                            try { regrabLoopLimit = std::stoi(variableValueStr); } catch (...) {}
                            if (regrabLoopLimit < 0) { regrabLoopLimit = 0; }
                            else if (regrabLoopLimit > 50) { regrabLoopLimit = 50; }
                        }
                        else if (variableName == "RegrabLoopWindow")
                        {
                            regrabLoopWindow = ParseFloatSetting(variableValueStr, 0.2f, 30.0f, regrabLoopWindow);
                        }
                        else if (variableName == "RegrabLoopCooldown")
                        {
                            regrabLoopCooldown = ParseFloatSetting(variableValueStr, 0.2f, 30.0f, regrabLoopCooldown);
                        }
                        else if (variableName == "RegrabCooldown")
                        {
                            regrabCooldown = ParseFloatSetting(variableValueStr, 0.0f, 5.0f, regrabCooldown);
                        }
                        else if (variableName == "LockClickWindow" ||
                            variableName == "LockDoubleClickWindow") // legacy name
                        {
                            lockClickWindow = ParseFloatSetting(variableValueStr, 0.1f, 2.0f, lockClickWindow);
                        }
                        else if (variableName == "LockClickCount")
                        {
                            try { lockClickCount = std::stoi(variableValueStr); } catch (...) {}
                            if (lockClickCount < 2) { lockClickCount = 2; }
                            else if (lockClickCount > 5) { lockClickCount = 5; }
                        }
                        else if (variableName == "LockChordCooldown")
                        {
                            lockChordCooldown = ParseFloatSetting(variableValueStr, 0.0f, 10.0f, lockChordCooldown);
                        }
                        else if (variableName == "LockGripGraceSeconds")
                        {
                            lockGripGrace = ParseFloatSetting(variableValueStr, 0.0f, 1.0f, lockGripGrace);
                        }
                        else if (variableName == "ThrowTrace")
                        {
                            throwTrace = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "ThrowVrYieldSeconds")
                        {
                            throwVrYieldSeconds = ParseFloatSetting(variableValueStr, 0.0f, 1.0f, throwVrYieldSeconds);
                        }
                        else if (variableName == "BlinkWaitsForClearHand")
                        {
                            blinkWaitsForClearHand = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "BlinkHiddenFromGame")
                        {
                            blinkHiddenFromGame = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "BlacklistGesture")
                        {
                            blacklistGesture = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "BlacklistClickCount")
                        {
                            try { blacklistClickCount = std::stoi(variableValueStr); } catch (...) {}
                            if (blacklistClickCount < 4) { blacklistClickCount = 4; }
                            else if (blacklistClickCount > 8) { blacklistClickCount = 8; }
                        }
                        else if (variableName == "LockRequiresSheathed")
                        {
                            lockRequiresSheathed = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "LockNotifications")
                        {
                            lockNotifications = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EquipSilenceSeconds")
                        {
                            equipSilenceSeconds = ParseFloatSetting(variableValueStr, 0.0f, 5.0f, equipSilenceSeconds);
                        }
                        else if (variableName == "WeaponDrawOnEquip")
                        {
                            weaponDrawOnEquip = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "WeaponSkeletonRefresh")
                        {
                            weaponSkeletonRefresh = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EmbedMinBuryUnits")
                        {
                            embedMinBuryUnits = ParseFloatSetting(variableValueStr, 0.0f, 40.0f, embedMinBuryUnits);
                        }
                        else if (variableName == "EmbedNeedsTipInBody")
                        {
                            embedNeedsTipInBody = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabDrawVerify")
                        {
                            grabDrawVerify = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "GrabDrawVerifySeconds")
                        {
                            grabDrawVerifySeconds = ParseFloatSetting(variableValueStr, 0.5f, 20.0f, grabDrawVerifySeconds);
                        }
                        else if (variableName == "GrabDrawTrace")
                        {
                            grabDrawTrace = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "WeaponRefreshDelayFrames")
                        {
                            try { weaponRefreshDelayFrames = std::stoi(variableValueStr); } catch (...) {}
                            if (weaponRefreshDelayFrames < 0) { weaponRefreshDelayFrames = 0; }
                            else if (weaponRefreshDelayFrames > 30) { weaponRefreshDelayFrames = 30; }
                        }
                        else if (variableName == "WeaponRefreshMinInterval")
                        {
                            weaponRefreshMinInterval = ParseFloatSetting(variableValueStr, 0.0f, 5.0f, weaponRefreshMinInterval);
                        }
                        else if (variableName == "DropRemoveDelaySeconds")
                        {
                            dropRemoveDelaySeconds = ParseFloatSetting(variableValueStr, 0.0f, 1.0f, dropRemoveDelaySeconds);
                        }
                        else if (variableName == "DropWatchSeconds")
                        {
                            dropWatchSeconds = ParseFloatSetting(variableValueStr, 0.0f, 15.0f, dropWatchSeconds);
                        }
                        else if (variableName == "EquipSounds")
                        {
                            equipSounds = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EmbedTrace")
                        {
                            embedTrace = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EmbedNoCollision")
                        {
                            embedNoCollision = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "UseIwpWoundData")
                        {
                            useIwpWoundData = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "DropLateralOffset")
                        {
                            dropLateralOffset = ParseFloatSetting(variableValueStr, -20.0f, 20.0f, dropLateralOffset);
                        }
                        else if (variableName == "DropDownOffset")
                        {
                            dropDownOffset = ParseFloatSetting(variableValueStr, 0.0f, 20.0f, dropDownOffset);
                        }
                        else if (variableName == "GripReleaseGraceSeconds")
                        {
                            gripReleaseGraceSeconds = ParseFloatSetting(variableValueStr, 0.0f, 1.0f, gripReleaseGraceSeconds);
                        }
                        else if (variableName == "DropButton")
                        {
                            int parsedId = dropButtonId;
                            const char* parsedName = dropButtonName;
                            if (TryParseDropButton(variableValueStr, parsedId, parsedName))
                            {
                                dropButtonId = parsedId;
                                dropButtonName = parsedName;
                                sawDropButton = true;
                            }
                            else
                            {
                                _MESSAGE(
                                    "Unknown DropButton \"%s\" — keeping %s (id %d). Valid: Trigger, Grip, A, B/Menu, Stick/Touchpad, Joystick, DPadUp/Down/Left/Right, System, or 0-63.",
                                    variableValueStr.c_str(),
                                    dropButtonName,
                                    dropButtonId);
                            }
                        }
                        else if (variableName == "UseGripForDrop")
                        {
							// Legacy alias for DropButton=Grip
                            legacyUseGripForDrop = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "ExcludedForms" || variableName == "ExcludedForm")
                        {
                            // Repeatable; each line's entries append to the built-in list.
                            ParseExcludedFormsList(variableValueStr);
                        }
                    }                    
                } 
            }

			if (!sawDropButton && legacyUseGripForDrop)
			{
				dropButtonId = 2;
				dropButtonName = "Grip";
			}

            _MESSAGE(
                "Config file is loaded successfully (EnableTwoHandedWeapons=%s, EnableTwoHandedHandSwapping=%s, EnableStaves=%s, EnableTorches=%s, EnableShields=%s, EnableShieldSwapping=%s, RequireDoubleTapForBowCrossbowDrop=%s, DropButton=%s id=%d, ExcludedForms=%zu).",
                enableTwoHandedWeapons ? "true" : "false",
                enableTwoHandedHandSwapping ? "true" : "false",
                enableStaves ? "true" : "false",
                enableTorches ? "true" : "false",
                enableShields ? "true" : "false",
                enableShieldSwapping ? "true" : "false",
                requireDoubleTapForBowCrossbowDrop ? "true" : "false",
                dropButtonName,
                dropButtonId,
                excludedFormEntries.size());
            if (grabEquipDelay > 0.0f)
            {
                _MESSAGE(
                    "Grab equip delay: %.2fs - a weapon grabbed with HIGGS stays a loose object until it has "
                    "been held that long; let go sooner and it is never picked up.",
                    grabEquipDelay);
            }
            else
            {
                _MESSAGE("Grab equip delay: off - a weapon grabbed with HIGGS is picked up and equipped at once.");
            }
            _MESSAGE(
                "Double tap counts touch: %d (-1 = decide from the controller: Valve Index reads grip presses "
                "only, every other controller a press or a touch; 0 = presses only; 1 = press or touch).",
                doubleTapCountsTouch);
            _MESSAGE(
                "Going through a door: %s; the mod stays locked %.1fs after the new cell appears.",
                cellLoadLock ? "the mod stands down for loading screens" : "OFF",
                cellLoadLockSeconds);
            _MESSAGE(
                "Drawing a weapon you picked up: %s; if the game swallows that request: %s; every pickup logged: %s.",
                weaponDrawOnEquip ? "on" : "OFF",
                grabDrawVerify ? "ask again until it is really out" : "leave it sheathed (bGrabDrawVerify = 0)",
                grabDrawTrace ? "yes" : "no");
            _MESSAGE(
                "Sticking a weapon you let go in somebody: at least %.1f units of the blade must be "
                "in them first (0 = anywhere within EmbedRadius counts, which is how it behaved before); %s.",
                embedMinBuryUnits,
                embedNeedsTipInBody
                    ? "and its tip must be inside one of their body parts (bEmbedNeedsTipInBody = 1)"
                    : "its tip is not checked against their body (bEmbedNeedsTipInBody = 0)");
            _MESSAGE(
                "Grab grip: telling a grip driver where a weapon was taken hold of: %s; reporting backwards grabs as "
                "reversed: %s; a weapon pulled through the air: %s%s.",
                grabGrip ? "on" : "OFF",
                grabGripReversed ? "on" : "OFF",
                grabGripPulled ? "held where the pull left it" : "held the natural way",
                !grabGripPulled ? "" : (grabGripReversePulled
                    ? ", and reversed when it arrives backwards against your hand (bGrabGripReversePulled = 1)"
                    : ", never reversed (bGrabGripReversePulled = 0)"));
            _MESSAGE(
                "Holding a weapon you pick up (menu, per mode) - Simple: where you grab %s, reversed %s, reversed "
                "two-handers %s, anywhere %s | Grip hold: where you grab %s, reversed %s, reversed two-handers %s, "
                "anywhere %s | Legacy uses bGrabGrip / bGrabGripReversed above.",
                grabGripSimple ? "on" : "OFF",
                grabGripReversedSimple ? "on" : "OFF",
                grabGripReversedTwoHandedSimple ? "ON" : "off",
                grabGripAnywhereSimple ? "ON" : "off",
                grabGripGripHold ? "on" : "OFF",
                grabGripReversedGripHold ? "on" : "OFF",
                grabGripReversedTwoHandedGripHold ? "ON" : "off",
                grabGripAnywhereGripHold ? "ON" : "off");
            _MESSAGE(
                "Taking what is not yours (the red Steal prompt): %s.",
                grabEquipDelayStolen > 0.0f ? "held as a loose object for the stolen wait below before it is taken"
                                            : "taken at once, like anything else (fGrabEquipDelayStolen = 0)");
            _MESSAGE("Stolen wait: %.1fs (fGrabEquipDelayStolen; the longer of it and fGrabEquipDelay wins).",
                grabEquipDelayStolen);
            _MESSAGE(
                "Grab probe (3.0.0 test): %s; posing HIGGS's hold to match: %s; a grab counts as reversed within %.0f "
                "degrees of dead backwards (90 = whichever way round it is nearer to).",
                grabGripTrace ? "ON - every melee weapon you take hold of is measured and logged" : "off",
                grabProbeSetTransform ? "ON" : "off",
                grabGripReversedCone);
            if (holsterThirdPersonBody)
            {
                _MESSAGE(
                    "Holsters: TEST - measuring from the third-person body (GetNiRootNode(0)) with VRIK's own slot "
                    "positions. The learned-positions file is not used and not added to.");
            }
            else
            {
                _MESSAGE("Holsters: measuring from the first-person skeleton with learned positions, as before.");
            }
            _MESSAGE(
                "Grip blink after a pickup: bBlinkWaitsForClearHand = %d - %s",
                blinkWaitsForClearHand ? 1 : 0,
                blinkWaitsForClearHand
                    ? "other mods are shown the grip pressed again only once the hand is clear of anything VR Climbing climbs, so picking a weapon up off a table cannot start a climb."
                    : "the grip comes back after 0.10s wherever the hand is, as in 2.2.0.");
            _MESSAGE(
                "Grip blink after a pickup: bBlinkHiddenFromGame = %d - %s",
                blinkHiddenFromGame ? 1 : 0,
                blinkHiddenFromGame
                    ? "the game itself is handed the real grip while other mods see the blink, so it cannot read the re-press as Ready Weapon (the Oculus LEFT grip) and put a left-hand weapon away."
                    : "the game sees the blink too, as in 2.2.0 - a left-hand weapon can be put away by it on Oculus controllers.");
            _MESSAGE(
                "Holsters: bHolsterListenToVrik = %d - %s",
                holsterListenToVrik ? 1 : 0,
                holsterListenToVrik
                    ? "on older VRIK, Grip hold stands aside where VRIK's own holster buzz says a slot will take the weapon, once that buzz has been heard this session."
                    : "on older VRIK, Grip hold uses its estimate of where the slots are, as in 2.2.0.");
            // The two chords are told apart by how many clicks they carry, so they
            // cannot carry the same number. LockClickCount is player-settable up to 5
            // and the blacklist defaults to 5, so this is reachable by anyone who turns
            // the lock chord up rather than a theoretical case.
            if (blacklistClickCount <= lockClickCount)
            {
                const int wanted = blacklistClickCount;
                blacklistClickCount = lockClickCount + 2;
                if (blacklistClickCount > 8) { blacklistClickCount = 8; }
                _MESSAGE(
                    "BlacklistClickCount %d would collide with LockClickCount %d - using %d instead.",
                    wanted,
                    lockClickCount,
                    blacklistClickCount);
            }

            _MESSAGE(
                "Chords (Grip Hold mode, either hand; grip needed unless that hand holds a weapon grip is not holding): lock = %d taps, blacklist = %s (%d taps), weapon must be away = %s.",
                lockClickCount,
                blacklistGesture ? "on" : "off",
                blacklistClickCount,
                lockRequiresSheathed ? "yes" : "no");

            _MESSAGE(
                "GripHold config (WeaponMode=%s so far, ThrowMomentum=%s scale=%.2f, StickOnRelease=%s, PullOut=%s, ResumeIwpEmbed=%s, LockWindow=%.2fs).",
                WeaponModeName(weaponMode),
                throwMomentum ? "true" : "false",
                throwVelocityScale,
                stickOnRelease ? "true" : "false",
                pullOut ? "true" : "false",
                resumeIwpEmbed ? "true" : "false",
                lockClickWindow);
            return true;
        }
        return false;
    }

	void Log(const int msgLogLevel, const char* fmt, ...)
	{
		if (msgLogLevel > logging)
		{
			return;
		}

		va_list args;
		char logBuffer[4096];

		va_start(args, fmt);
		vsprintf_s(logBuffer, sizeof(logBuffer), fmt, args);
		va_end(args);

		// Seconds since the plugin loaded, on every line. Without it a log shows the
		// ORDER of events but not their spacing, and the difference between "the
		// player dropped this twice" and "it left their hand twice in one tenth of a
		// second" is the whole diagnosis.
		static const std::chrono::steady_clock::time_point epoch =
			std::chrono::steady_clock::now();
		const float elapsed = std::chrono::duration<float>(
			std::chrono::steady_clock::now() - epoch).count();

		// "%s", not logBuffer: _MESSAGE is printf-style, so passing the already
		// formatted text as the FORMAT STRING makes any surviving '%' a conversion
		// specifier read against an empty argument list. A line ending up as
		// "depth=100% bladeLen=45" was enough to kill the process with no crash log.
		_MESSAGE("[%8.3f] %s", elapsed, logBuffer);
	}

}
