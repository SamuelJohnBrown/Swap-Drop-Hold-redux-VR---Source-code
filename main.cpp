#include "skse64_common/skse_version.h"
#include <shlobj.h>
#include <intrin.h>
#include <string>
#include <xbyak/xbyak.h>

#include "skse64/PluginAPI.h"
#include "Engine.h"
#include "WeaponGrabLogger.h"
#include "WeaponDrawHandler.h"
#include "TriggerHoldLogger.h"
#include "OppositeHandGrabLogger.h"
#include "GripHold.h"
#include "BlinkHoldBack.h"
#include "EmbedPins.h"
#include "SilentEquip.h"
#include "Exclusions.h"
#include "McmBridge.h"
#include "CellLoadGuard.h"
#include "McmWatch.h"
#include "HapticCensus.h"
#include "GrabGripProvider.h"
#include "MenuGrip.h"
#include "HiggsRelease.h"
#include "VrikHolsterApi.h"
#include "WeaponThrowButtons.h"
#include "GameGripGuard.h"
#include "config.h"
#include "swapdropandholdreduxinterface001.h"

#include "skse64_common/BranchTrampoline.h"

namespace SwapDropAndHoldRedux
{
	static SKSEMessagingInterface* g_messaging = NULL;
	static PluginHandle					g_pluginHandle = kPluginHandle_Invalid;
	static SKSEPapyrusInterface* g_papyrus = NULL;
	static SKSEObjectInterface* g_object = NULL;
	SKSETaskInterface* g_task = NULL;

	static SKSEVRInterface* g_vrInterface = nullptr;

	#pragma comment(lib, "Ws2_32.lib")

	void SetupReceptors()
	{
		_MESSAGE("Building Event Sinks...");
		RegisterWeaponDrawHandler();
		RegisterMcmBridge();
		RegisterExclusions(g_messaging);
		RegisterVrikHolsterEvents(g_messaging);
		RegisterMenuGripMenuListener();
		RegisterCellLoadGuard();
	}

	extern "C" {

		bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info) {	// Called by SKSE to learn about this plugin and check that it's safe to load it
			const std::string logPath = std::string("\\My Games\\Skyrim VR\\SKSE\\") + PLUGIN_FILE_NAME + ".log";
			gLog.OpenRelative(CSIDL_MYDOCUMENTS, logPath.c_str());
			gLog.SetPrintLevel(IDebugLog::kLevel_Error);
			gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);
			//gLog.SetLogLevel(IDebugLog::kLevel_FatalError);

			std::string logMsg(PLUGIN_DISPLAY_NAME);
			logMsg.append(": ");
			logMsg.append(SwapDropAndHoldRedux::MOD_VERSION_STR);
			_MESSAGE(logMsg.c_str());

			// populate info structure
			info->infoVersion = PluginInfo::kInfoVersion;
			info->name = PLUGIN_FILE_NAME;
			info->version = SwapDropAndHoldRedux::MOD_VERSION;

			// store plugin handle so we can identify ourselves later
			g_pluginHandle = skse->GetPluginHandle();

			std::string skseVers = "SKSE Version: ";
			skseVers += std::to_string(skse->runtimeVersion);
			_MESSAGE(skseVers.c_str());

			if (skse->isEditor)
			{
				_MESSAGE("loaded in editor, marking as incompatible");

				return false;
			}
			else if (skse->runtimeVersion < CURRENT_RELEASE_RUNTIME)
			{
				_MESSAGE("unsupported runtime version %08X", skse->runtimeVersion);

				return false;
			}

			// ### do not do anything else in this callback
			// ### only fill out PluginInfo and return true/false

			// supported runtime version
			return true;
		}

		inline bool file_exists(const std::string& name) {
			struct stat buffer;
			return (stat(name.c_str(), &buffer) == 0);
		}

		static const size_t TRAMPOLINE_SIZE = 256;

		// Listener for messages from ANY sender. IWP >= 1.7.2 broadcasts its interface
		// this way because name-addressed dispatch fails on SkyrimVR's SKSE.
		void OnAnySenderMessage(SKSEMessagingInterface::Message* msg)
		{
			SwapDropAndHoldRedux::AcceptIwpInterfaceOffer(msg);
		}

		//Listener for SKSE Messages
		void OnSKSEMessage(SKSEMessagingInterface::Message* msg)
		{
			if (msg)
			{
				if (msg->type == SKSEMessagingInterface::kMessage_PostLoad)
				{

				}
				else if (msg->type == SKSEMessagingInterface::kMessage_InputLoaded)
					SetupReceptors();
				else if (msg->type == SKSEMessagingInterface::kMessage_DataLoaded)
				{
					SwapDropAndHoldReduxAPI::RegisterSwapDropAndHoldReduxInterface(g_pluginHandle, g_messaging);
					SwapDropAndHoldRedux::loadConfig();
					// A second offer, for a grip driver whose listener was not up at PostPostLoad.
					SwapDropAndHoldRedux::GrabGripBroadcastOffer(g_pluginHandle, g_messaging, "DataLoaded");
					SwapDropAndHoldRedux::GripHoldOnSettingsReloaded();
					SwapDropAndHoldRedux::CellLoadGuardReset();

					// Only here is there a load order to ask about.
					SwapDropAndHoldRedux::ReportMcmChain();
					SwapDropAndHoldRedux::ExclusionsLoad();
					SwapDropAndHoldRedux::WeaponThrowButtonsFind();

					// NEW SKSEVR feature: trampoline interface object from QueryInterface() - Use SKSE existing process code memory pool - allow Skyrim to run without ASLR
					if (SwapDropAndHoldRedux::g_trampolineInterface)
					{
						void* branch = SwapDropAndHoldRedux::g_trampolineInterface->AllocateFromBranchPool(g_pluginHandle, TRAMPOLINE_SIZE);
						if (!branch) {
							_ERROR("couldn't acquire branch trampoline from SKSE. this is fatal. skipping remainder of init process.");
							return;
						}

						g_branchTrampoline.SetBase(TRAMPOLINE_SIZE, branch);

						void* local = SwapDropAndHoldRedux::g_trampolineInterface->AllocateFromLocalPool(g_pluginHandle, TRAMPOLINE_SIZE);
						if (!local) {
							_ERROR("couldn't acquire codegen buffer from SKSE. this is fatal. skipping remainder of init process.");
							return;
						}

						g_localTrampoline.SetBase(TRAMPOLINE_SIZE, local);

						_MESSAGE("Using new SKSEVR trampoline interface memory pool alloc for codegen buffers.");
					}
					else  // otherwise if using an older SKSEVR version, fall back to old code
					{

						if (!g_branchTrampoline.Create(TRAMPOLINE_SIZE))  // don't need such large buffers
						{
							_FATALERROR("[ERROR] couldn't create branch trampoline. this is fatal. skipping remainder of init process.");
							return;
						}

						if (!g_localTrampoline.Create(TRAMPOLINE_SIZE, nullptr))
						{
							_FATALERROR("[ERROR] couldn't create codegen buffer. this is fatal. skipping remainder of init process.");
							return;
						}

						_MESSAGE("Using legacy SKSE trampoline creation.");
					}

					SwapDropAndHoldRedux::GameLoad();
					SwapDropAndHoldRedux::StartMod();
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostPostLoad)
				{
					SwapDropAndHoldReduxAPI::RegisterSwapDropAndHoldReduxInterface(g_pluginHandle, g_messaging);
					SwapDropAndHoldRedux::GrabGripBroadcastOffer(g_pluginHandle, g_messaging, "PostPostLoad");
					SwapDropAndHoldRedux::InitBitingAxesCompatibility(g_pluginHandle, g_messaging);

					higgsInterface = HiggsPluginAPI::GetHiggsInterface001(g_pluginHandle, g_messaging);
					if (higgsInterface)
					{
						_MESSAGE("Got HIGGS interface. Buildnumber: %d", higgsInterface->GetBuildNumber());
						SwapDropAndHoldRedux::RegisterWeaponGrabLogger();
						SwapDropAndHoldRedux::RegisterTriggerHoldLogger();
						SwapDropAndHoldRedux::RegisterOppositeHandGrabLogger();
						SwapDropAndHoldRedux::RegisterWeaponDrawHiggsCallback();
						SwapDropAndHoldRedux::RegisterGripHold();
						SwapDropAndHoldRedux::RegisterBlinkHoldBack();
						SwapDropAndHoldRedux::RegisterMcmWatch();
						SwapDropAndHoldRedux::RegisterHapticCensus();
						SwapDropAndHoldRedux::RegisterGameGripGuard();
						SwapDropAndHoldRedux::GripHoldDetectThrowMod();
						SwapDropAndHoldRedux::RegisterWeaponSoundSilence();
					}
					else
					{
						_MESSAGE("Did not get HIGGS interface");
					}

					spellwheelInterface = spellwheelPluginApi::getSpellWheelInterface001(g_pluginHandle, g_messaging);
					if (spellwheelInterface)
					{
						_MESSAGE(
							"Got SpellWheelVR interface. Buildnumber: %u",
							spellwheelInterface->getBuildNumber());
					}
					else
					{
						_MESSAGE("Did not get SpellWheelVR interface");
					}

					vrikInterface = vrikPluginApi::getVrikInterface001(g_pluginHandle, g_messaging);
					if (vrikInterface)
					{
						unsigned int vrikBuildNumber = vrikInterface->getBuildNumber();
						if (vrikBuildNumber < 80400)
						{
							ShowErrorBoxAndTerminate("[CRITICAL] VRIK's older versions are not compatible with Swap Drop & Hold redux. Make sure you have VRIK version 0.8.4 or higher, preferably the latest.");
						}
						_MESSAGE("Got VRIK interface. Buildnumber: %d", vrikBuildNumber);

					}
					else
					{
						_MESSAGE("Did not get VRIK interface");
					}

					// Whether VRIK is new enough to answer holster questions (build 80700). Logged either way.
					SwapDropAndHoldRedux::VrikHolsterApiInit();

					skyrimVRESLInterface = SkyrimVRESLPluginAPI::GetSkyrimVRESLInterface001(g_pluginHandle, g_messaging);
					if (skyrimVRESLInterface)
					{
						_MESSAGE("Got SkyrimVRESL interface");
					}
					else
					{
						_MESSAGE("Did not get SkyrimVRESL interface");
					}

					// Secondary IWP path; the broadcast listener is the reliable one.
					SwapDropAndHoldRedux::RequestIwpInterface(g_pluginHandle, g_messaging);
					if (!SwapDropAndHoldRedux::g_iwpInterface)
					{
						_MESSAGE("IWP interface not yet available (needs IWP 1.7.2+) — lodged weapons will use the built-in draw if it never arrives");
					}
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostLoadGame)
				{
					if ((bool)(msg->data) == true)
					{
						SwapDropAndHoldRedux::PostLoadGame();
						SwapDropAndHoldRedux::McmWatchArm("the game loaded");
					}
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PreLoadGame ||
					msg->type == SKSEMessagingInterface::kMessage_NewGame)
				{
					SwapDropAndHoldRedux::GripHoldOnPreLoadGame();
					SwapDropAndHoldRedux::HiggsReleaseOnPreLoadGame();
					SwapDropAndHoldRedux::VrikHolsterOnPreLoadGame();
					// A load throws away the script objects the menu watch reads; a new game has fresh ones.
					if (msg->type == SKSEMessagingInterface::kMessage_NewGame)
					{
						SwapDropAndHoldRedux::McmWatchArm("a new game");
					}
					else
					{
						SwapDropAndHoldRedux::McmWatchDisarm();
					}
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_SaveGame)
				{
					// Nothing we mute is saved, but a save is a natural moment to be
					// certain every borrowed sound is back where it belongs.
					SwapDropAndHoldRedux::RestoreAllWeaponSounds();
				}
			}
		}

		bool SKSEPlugin_Load(const SKSEInterface* skse) {	// Called by SKSE to load this plugin

			g_task = (SKSETaskInterface*)skse->QueryInterface(kInterface_Task);

			g_papyrus = (SKSEPapyrusInterface*)skse->QueryInterface(kInterface_Papyrus);

			g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
			g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);
			// All-sender listener, registered during plugin load so it exists before
			// any other plugin's broadcast (IWP pushes its interface this way).
			g_messaging->RegisterListener(g_pluginHandle, nullptr, OnAnySenderMessage);

			g_vrInterface = (SKSEVRInterface*)skse->QueryInterface(kInterface_VR);
			if (!g_vrInterface) {
				_MESSAGE("[CRITICAL] Couldn't get SKSE VR interface. You probably have an outdated SKSE version.");
				return false;
			}

			// A grip holding a weapon is hidden from menus until the hand opens - see MenuGrip.h.
			RegisterMenuGripMask(g_pluginHandle, g_vrInterface);

			return true;
		}
	};
}