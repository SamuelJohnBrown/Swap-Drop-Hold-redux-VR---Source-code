#include "McmBridge.h"

#include "Engine.h"
#include "GripHold.h"
#include "TrackedWeapons.h"
#include "VrikHolsters.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameMenus.h>
#include <skse64/gamethreads.h>

#include <fstream>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		// MCM Helper commits its settings file when the journal closes, so that is
		// the moment the menu's changes become readable. Re-reading there is what
		// makes the menu apply without a restart.
		class McmMenuSink : public BSTEventSink<MenuOpenCloseEvent>
		{
		public:
			virtual EventResult ReceiveEvent(MenuOpenCloseEvent* evn, EventDispatcher<MenuOpenCloseEvent>* dispatcher) override
			{
				if (!evn || evn->opening)
				{
					return kEvent_Continue;
				}

				UIStringHolder* strings = UIStringHolder::GetSingleton();
				if (!strings || evn->menuName != strings->journalMenu)
				{
					return kEvent_Continue;
				}

				if (g_task)
				{
					g_task->AddTask(new ReloadTask());
				}

				return kEvent_Continue;
			}

		private:
			class ReloadTask : public TaskDelegate
			{
			public:
				virtual void Run() override
				{
					loadConfig();

					// Anything DERIVED from a setting has to be recomputed here too, or
					// the ini says one thing and the game does another until the next
					// launch. The holster layout is read from VRIK using our own
					// settings, so it is refreshed alongside them.
					HolsterRefreshSettings();

					// The weapon mode is a setting like any other, but switching it has to
					// start both hands over.
					GripHoldOnSettingsReloaded();

					LOG_INFO("MCM: settings re-read after the journal closed.");
				}

				virtual void Dispose() override
				{
					delete this;
				}
			};
		};

		static McmMenuSink s_menuSink;
	}

	void RegisterMcmBridge()
	{
		MenuManager* menuManager = MenuManager::GetSingleton();
		if (menuManager)
		{
			menuManager->MenuOpenCloseEventDispatcher()->AddEventSink(&s_menuSink);
		}
	}

	// This ran at kInputLoaded, which is BEFORE the game has a load order.
	//
	// LookupModByName therefore could not find our plugin no matter how correctly it
	// was installed, and the verdict below reported a plugin that was loaded, ticked,
	// and registering its menu perfectly well as "in Data but NOT ENABLED". Two
	// rebuilds went looking for a fault in a file that never had one. The check is
	// sound; it was simply asked too early, so it now runs at kDataLoaded.
	void ReportMcmChain()
	{
		// Every link in this chain fails silently, so the verdict is printed here in
		// words the player can act on. PRESENT IS NOT LOADED: a mod manager will
		// happily deploy the plugin into Data without ticking it in the load order,
		// and that is the likeliest failure of the lot - it is what happened on the
		// first build of this menu. Reporting it as "damaged" would have sent the
		// next hour into the plugin's bytes for a file that was perfectly fine.
		const ModInfo* modInfo = DataHandler::GetSingleton()
			? DataHandler::GetSingleton()->LookupModByName(kMcmEspName)
			: nullptr;
		const bool pluginLoaded = modInfo && modInfo->modIndex != 0xFF;

		// LookupModByName only knows plugins the game LOADED. A plugin sitting in
		// Data but absent from the load order simply is not in that list, so a null
		// result cannot be reported as "not installed" - that is exactly what this
		// said while the file was plainly on disk, and it sent the reader looking in
		// the wrong place. Ask the filesystem separately and the two answers together
		// name the real state.
		std::string espPath = GetRuntimeDirectory();
		espPath += "Data\\";
		espPath += kMcmEspName;
		const bool espOnDisk = std::ifstream(espPath).good();
		const UInt32 questFormID = pluginLoaded ? GetFullFormIdMine(kMcmEspName, kMcmQuestFormID) : 0;

		std::string seqPath = GetRuntimeDirectory();
		seqPath += "Data\\SEQ\\";
		seqPath += PLUGIN_FILE_NAME;
		seqPath += ".seq";
		const bool seqPresent = std::ifstream(seqPath).good();

		std::string cfgPath = GetRuntimeDirectory();
		cfgPath += "Data\\MCM\\Config\\";
		cfgPath += PLUGIN_FILE_NAME;
		cfgPath += "\\config.json";
		const bool configPresent = std::ifstream(cfgPath).good();

		if (questFormID != 0 && seqPresent && configPresent)
		{
			LOG_INFO(
				"MCM: ready (quest %08X resolved, SEQ present, config.json present). "
				"Menu changes apply when you close the journal.",
				questFormID);
			return;
		}

		LOG_ERR(
			"MCM: the menu will NOT appear. %s: %s. Quest record %s. SEQ file %s. config.json %s.",
			kMcmEspName,
			pluginLoaded
				? "loaded"
				: espOnDisk
					? "is in Data but NOT ENABLED in the load order - tick it in your mod manager's "
					  "PLUGINS list, which is a separate action from enabling the mod itself, then "
					  "deploy. If the tick will not stick, add a line reading *SwapDropAndHoldRedux.esp "
					  "to plugins.txt in %LOCALAPPDATA%\\Skyrim VR with the game and manager closed"
					: "is NOT INSTALLED - the file is not in Data, so the menu option was never taken",
			questFormID != 0 ? "resolved" : "MISSING",
			seqPresent ? "present" : "MISSING - a start-enabled quest never starts on an existing save",
			configPresent ? "present" : "MISSING");
	}
}
