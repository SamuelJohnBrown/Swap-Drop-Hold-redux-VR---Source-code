#include "config.h"

namespace SwapDropAndHoldRedux {
		
	int logging = 2;
    int leftHandedMode = 0;
	float triggerHoldDropSeconds = 2.0f;
	float dropGuardTimeoutSeconds = 4.0f;
	float swapPullSpeedThreshold = 40.0f;
	float swapPullMinDistanceGrowth = 10.0f;

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
                    }                    
                } 
            }
            _MESSAGE("Config file is loaded successfully.");
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