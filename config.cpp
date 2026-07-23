#include "config.h"

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
	bool enableShields = true;
	bool enableShieldSwapping = false;
	int dropButtonId = 33; // OpenVR k_EButton_SteamVR_Trigger / Axis1
	const char* dropButtonName = "Trigger";

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

		std::string ToLowerCopy(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
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

    void loadConfig() 
    {
        std::string runtimeDirectory = GetRuntimeDirectory();

        if (!runtimeDirectory.empty()) 
        {
            std::string filepath = runtimeDirectory + "Data\\SKSE\\Plugins\\" + PLUGIN_FILE_NAME + ".ini";
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

                while (std::getline(file, line)) 
                {
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
                        else if (variableName == "EnableShields")
                        {
                            enableShields = ParseBoolSetting(variableValueStr);
                        }
                        else if (variableName == "EnableShieldSwapping")
                        {
                            enableShieldSwapping = ParseBoolSetting(variableValueStr);
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
                    }                    
                } 
            }

			if (!sawDropButton && legacyUseGripForDrop)
			{
				dropButtonId = 2;
				dropButtonName = "Grip";
			}

            _MESSAGE(
                "Config file is loaded successfully (EnableTwoHandedWeapons=%s, EnableTwoHandedHandSwapping=%s, EnableStaves=%s, EnableShields=%s, EnableShieldSwapping=%s, DropButton=%s id=%d).",
                enableTwoHandedWeapons ? "true" : "false",
                enableTwoHandedHandSwapping ? "true" : "false",
                enableStaves ? "true" : "false",
                enableShields ? "true" : "false",
                enableShieldSwapping ? "true" : "false",
                dropButtonName,
                dropButtonId);
            return;
        }
        return;
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

		_MESSAGE(logBuffer);
	}

}
