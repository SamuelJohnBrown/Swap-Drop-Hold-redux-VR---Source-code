#include "Exclusions.h"

#include "Engine.h"
#include "Helper.h"
#include "SkyrimVRESLAPI.h"
#include "TrackedWeapons.h"
#include "config.h"

#include <skse64_common/Utilities.h>

#include <skse64/GameData.h>
#include <skse64/GameReferences.h>
#include <skse64/PapyrusEvents.h>
#include <skse64/PluginAPI.h>

#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		struct SavedExclusion
		{
			std::string esp;
			UInt32 baseFormId = 0;
		};

		// The last weapon seen in each game hand, remembered because of a conflict the
		// mod cannot win: grip-hold means the grip is held the whole time a weapon is
		// carried, and holding grip is also what closes a menu. So you cannot open the
		// MCM while holding the weapon you want to exclude - by the time the page is on
		// screen your hand is empty. The buttons fall back to this.
		static UInt32 s_lastHeld[2] = { 0, 0 };

		static std::vector<SavedExclusion> s_saved;
		static std::unordered_set<UInt32> s_resolved;

		std::string ExclusionsFilePath()
		{
			std::string path = GetRuntimeDirectory();
			if (path.empty())
			{
				return path;
			}
			path += "Data\\SKSE\\Plugins\\SwapDropAndHoldRedux_excluded.ini";
			return path;
		}

		// Runtime form id -> the plugin that owns it and the id within that plugin.
		//
		// The inverse of GetFullFormID, and the reason the list can be saved at all: a
		// runtime id encodes the plugin's position in THIS load order, so writing one
		// down would silently point at some other mod's item after the next install.
		bool DescribeForm(const UInt32 formID, std::string& outEsp, UInt32& outBase)
		{
			const UInt8 modIndex = static_cast<UInt8>(formID >> 24);

			// Skyrim.esm and the other masters resolve by name without a lookup.
			if (modIndex == 0x00)
			{
				outEsp = "Skyrim.esm";
				outBase = formID & 0x00FFFFFF;
				return true;
			}

			const bool isLight = (modIndex == 0xFE);
			const UInt16 lightIndex = static_cast<UInt16>((formID & 0x00FFFFFF) >> 12);

			if (skyrimVRESLInterface)
			{
				const SkyrimVRESLPluginAPI::TESFileCollection* files =
					skyrimVRESLInterface->GetCompiledFileCollection();
				if (files)
				{
					const tArray<ModInfo*>& list = isLight ? files->smallFiles : files->files;
					for (UInt32 i = 0; i < list.count; ++i)
					{
						ModInfo* info = nullptr;
						list.GetNthItem(i, info);
						if (!info)
						{
							continue;
						}

						const bool match = isLight
							? (info->lightIndex == lightIndex)
							: (info->modIndex == modIndex);
						if (match)
						{
							outEsp = info->name;
							outBase = isLight ? (formID & 0x00000FFF) : (formID & 0x00FFFFFF);
							return true;
						}
					}
				}
			}

			// No ESL support present: only regular plugins exist, and the load order is
			// the plain array.
			if (!isLight)
			{
				DataHandler* dataHandler = DataHandler::GetSingleton();
				if (dataHandler && modIndex < 0xFF)
				{
					ModInfo* info = dataHandler->modList.loadedMods[modIndex];
					if (info)
					{
						outEsp = info->name;
						outBase = formID & 0x00FFFFFF;
						return true;
					}
				}
			}

			return false;
		}

		void Save()
		{
			const std::string path = ExclusionsFilePath();
			if (path.empty())
			{
				return;
			}

			std::ofstream file(path.c_str(), std::ios::trunc);
			if (!file)
			{
				LOG_ERR("Exclusions: could not write %s", path.c_str());
				return;
			}

			file << "; Items excluded from grip-hold, added from the mod's own menu.\n";
			file << "; An excluded item behaves the way it did before this mod: it is never\n";
			file << "; grip-bound and never dropped when your hand opens.\n";
			file << "; Delete this file, or use CLEAR THE WHOLE EXCLUDED LIST, to forget them.\n";
			for (const SavedExclusion& entry : s_saved)
			{
				file << entry.esp << ":0x" << std::hex << std::uppercase << entry.baseFormId
					 << std::dec << "\n";
			}
		}

		bool AddForm(TESForm* form)
		{
			if (!form)
			{
				return false;
			}

			if (s_resolved.find(form->formID) != s_resolved.end())
			{
				LOG_INFO("Exclusions: %s is already excluded.", GetSafeFormName(form));
				return false;
			}

			std::string esp;
			UInt32 base = 0;
			if (!DescribeForm(form->formID, esp, base))
			{
				LOG_ERR(
					"Exclusions: could not work out which plugin owns %s (formId=%08X), so it "
					"cannot be saved.",
					GetSafeFormName(form),
					form->formID);
				return false;
			}

			SavedExclusion entry;
			entry.esp = esp;
			entry.baseFormId = base;
			s_saved.push_back(entry);
			s_resolved.insert(form->formID);
			Save();

			LOG_INFO(
				"Exclusions: %s (%s : %06X) will behave as it did before this mod.",
				GetSafeFormName(form),
				esp.c_str(),
				base);
			return true;
		}

		bool RemoveForm(TESForm* form)
		{
			if (!form)
			{
				return false;
			}

			if (s_resolved.erase(form->formID) == 0)
			{
				LOG_INFO("Exclusions: %s was not excluded.", GetSafeFormName(form));
				return false;
			}

			for (size_t i = 0; i < s_saved.size(); ++i)
			{
				const UInt32 full =
					GetFullFormIdFromEspAndFormId(s_saved[i].esp.c_str(), s_saved[i].baseFormId);
				if (full == form->formID)
				{
					s_saved.erase(s_saved.begin() + i);
					break;
				}
			}

			Save();
			LOG_INFO("Exclusions: %s is back to normal grip-hold.", GetSafeFormName(form));
			return true;
		}

		// The in-game message the player actually sees. Same address GripHold uses.
		typedef void (*ShowHUDMessage_t)(const char* message, const char* soundEditorID, bool cancelIfQueued);
		static RelocAddr<ShowHUDMessage_t> ShowHUDMessage(0x00908170);

		void Say(const char* fmt, const char* name)
		{
			char buffer[256];
			sprintf_s(buffer, sizeof(buffer), fmt, name ? name : "that item");
			ShowHUDMessage(buffer, nullptr, true);
		}

		// What the button should act on: whatever is in the hand, or failing that the
		// last thing that was.
		TESForm* HeldForm(const bool isLeftGameHand, bool& outWasRemembered)
		{
			outWasRemembered = false;

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				return nullptr;
			}

			TESForm* now = player->GetEquippedObject(isLeftGameHand);
			if (now)
			{
				return now;
			}

			const UInt32 remembered = s_lastHeld[isLeftGameHand ? 1 : 0];
			if (remembered == 0)
			{
				return nullptr;
			}

			outWasRemembered = true;
			return LookupFormByID(remembered);
		}

		void HandleAction(const int action)
		{
			// 1/2 exclude right/left, 3/4 un-exclude right/left, 5 clear.
			switch (action)
			{
			case 1:
			case 2:
			{
				bool remembered = false;
				TESForm* form = HeldForm(action == 2, remembered);
				if (!form)
				{
					LOG_INFO(
						"Exclusions: nothing in the %s hand, and nothing remembered there either.",
						action == 2 ? "left" : "right");
					ShowHUDMessage("Nothing in that hand to exclude", nullptr, true);
					return;
				}
				if (AddForm(form))
				{
					Say(remembered ? "Excluded %s (the last thing in that hand)" : "Excluded %s",
						GetSafeFormName(form));
				}
				else
				{
					Say("%s was already excluded", GetSafeFormName(form));
				}
				return;
			}

			case 3:
			case 4:
			{
				bool remembered = false;
				TESForm* form = HeldForm(action == 4, remembered);
				if (!form)
				{
					LOG_INFO(
						"Exclusions: nothing in the %s hand, and nothing remembered there either.",
						action == 4 ? "left" : "right");
					ShowHUDMessage("Nothing in that hand to un-exclude", nullptr, true);
					return;
				}
				if (RemoveForm(form))
				{
					Say("%s is back to normal grip-hold", GetSafeFormName(form));
				}
				else
				{
					Say("%s was not excluded", GetSafeFormName(form));
				}
				return;
			}

			case 5:
			{
				const UInt32 had = static_cast<UInt32>(s_saved.size());
				LOG_INFO("Exclusions: clearing all %u excluded item(s).", had);
				s_saved.clear();
				s_resolved.clear();
				Save();
				char buffer[128];
				sprintf_s(buffer, sizeof(buffer), "Cleared %u excluded item(s)", had);
				ShowHUDMessage(buffer, nullptr, true);
				return;
			}

			default:
				LOG_INFO("Exclusions: unknown menu action %d.", action);
				return;
			}
		}

		class MenuActionSink : public BSTEventSink<SKSEModCallbackEvent>
		{
		public:
			virtual EventResult ReceiveEvent(
				SKSEModCallbackEvent* evn, EventDispatcher<SKSEModCallbackEvent>* dispatcher) override
			{
				(void)dispatcher;
				if (!evn)
				{
					return kEvent_Continue;
				}

				if (evn->eventName.data && strcmp(evn->eventName.data, "SDHR_MenuAction") == 0)
				{
					HandleAction(static_cast<int>(evn->numArg));
				}
				return kEvent_Continue;
			}
		};

		static MenuActionSink s_menuSink;
	}

	void RegisterExclusions(SKSEMessagingInterface* const messaging)
	{
		// THIS is why the buttons did nothing, through two test builds in which every
		// single component checked out on its own.
		//
		// The obvious line is g_modCallbackEventDispatcher.AddEventSink(&s_menuSink),
		// and it is wrong. That symbol is DEFINED in skse64/PapyrusEvents.cpp, and this
		// plugin compiles that file into itself by way of the static skse64 library. So
		// the dispatcher we would be sinking to is a private copy sitting in our own
		// DLL, at a different address from the one the SKSE runtime owns and fires. The
		// sink registers, the log says so, and no event can ever arrive - because
		// Form.SendModEvent reaches SKSE's instance, and nothing on earth reaches ours.
		//
		// The messaging interface hands out the real one. The SDK says as much on the
		// declaration: "Use this to acquire SKSE's internal EventDispatchers so that
		// you can sink to them."
		if (!messaging)
		{
			LOG_INFO("Exclusions: no messaging interface - the exclude buttons will do nothing.");
			return;
		}

		EventDispatcher<SKSEModCallbackEvent>* const dispatcher =
			static_cast<EventDispatcher<SKSEModCallbackEvent>*>(
				messaging->GetEventDispatcher(SKSEMessagingInterface::kDispatcher_ModEvent));

		if (!dispatcher)
		{
			LOG_INFO("Exclusions: SKSE returned no mod event dispatcher - the exclude buttons will do nothing.");
			return;
		}

		dispatcher->AddEventSink(&s_menuSink);

		// The address is logged on purpose. It is the one thing that distinguishes this
		// from the version that looked identical and worked for nobody: SKSE's own
		// dispatcher lives in SKSE's module, ours lived in ours.
		LOG_INFO(
			"Exclusions: listening for the menu's exclude buttons on SKSE's dispatcher %p (ours would have been %p).",
			dispatcher,
			&g_modCallbackEventDispatcher);
	}

	void ExclusionsLoad()
	{
		s_saved.clear();
		s_resolved.clear();

		const std::string path = ExclusionsFilePath();
		if (path.empty())
		{
			return;
		}

		std::ifstream file(path.c_str());
		if (!file)
		{
			return;
		}

		std::string line;
		while (std::getline(file, line))
		{
			// Strip comments and whitespace the same way the main ini reader does.
			const size_t comment = line.find_first_of(";#");
			if (comment != std::string::npos)
			{
				line.erase(comment);
			}
			while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
			{
				line.pop_back();
			}
			if (line.empty())
			{
				continue;
			}

			const size_t colon = line.rfind(':');
			if (colon == std::string::npos || colon + 1 >= line.size())
			{
				LOG_INFO("Exclusions: skipping malformed line \"%s\".", line.c_str());
				continue;
			}

			SavedExclusion entry;
			entry.esp = line.substr(0, colon);
			try
			{
				entry.baseFormId = static_cast<UInt32>(std::stoul(line.substr(colon + 1), nullptr, 16));
			}
			catch (...)
			{
				LOG_INFO("Exclusions: skipping unreadable form id in \"%s\".", line.c_str());
				continue;
			}

			s_saved.push_back(entry);

			const UInt32 full = GetFullFormIdFromEspAndFormId(entry.esp.c_str(), entry.baseFormId);
			if (full != 0)
			{
				s_resolved.insert(full);
			}
			else
			{
				// Kept in the saved list on purpose: the plugin may simply be disabled
				// today, and throwing the entry away would silently forget the player's
				// choice the first time they launch without it.
				LOG_INFO(
					"Exclusions: %s : %06X is saved but its plugin is not loaded right now.",
					entry.esp.c_str(),
					entry.baseFormId);
			}
		}

		if (!s_saved.empty())
		{
			LOG_INFO(
				"Exclusions: %u item(s) excluded from grip-hold, %u of them live in this load order.",
				static_cast<UInt32>(s_saved.size()),
				static_cast<UInt32>(s_resolved.size()));
		}
	}

	// Puts one item on the blacklist or takes it off, and says which on the HUD.
	static bool ToggleOnBlacklist(TESForm* const target, const bool fromMemory)
	{
		const char* const name = GetSafeFormName(target);
		const bool wasExcluded = s_resolved.find(target->formID) != s_resolved.end();

		char buffer[256];
		if (wasExcluded)
		{
			RemoveForm(target);
			sprintf_s(
				buffer,
				sizeof(buffer),
				fromMemory ? "%s has been removed from the blacklist (last thing you held)"
						   : "%s has been removed from the blacklist",
				name ? name : "That item");
		}
		else
		{
			if (!AddForm(target))
			{
				// Only reachable when the form's owning plugin cannot be identified,
				// which AddForm has already logged in full.
				ShowHUDMessage("That item could not be blacklisted - check the log", nullptr, true);
				return false;
			}
			sprintf_s(
				buffer,
				sizeof(buffer),
				fromMemory ? "%s has been added to the blacklist (last thing you held)"
						   : "%s has been added to the blacklist",
				name ? name : "That item");
		}

		ShowHUDMessage(buffer, nullptr, true);
		return true;
	}

	bool ExclusionsToggleHand(const bool isLeftGameHand)
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			return false;
		}

		// What is in that hand beats what that hand last held, and the other hand is never
		// consulted: the chord was made with this one, so blacklisting the sword in it can
		// never pick the shield in the other.
		TESForm* target = player->GetEquippedObject(isLeftGameHand);
		bool fromMemory = false;
		if (!target)
		{
			const UInt32 remembered = s_lastHeld[isLeftGameHand ? 1 : 0];
			if (remembered != 0)
			{
				target = LookupFormByID(remembered);
				fromMemory = target != nullptr;
			}
		}

		if (!target)
		{
			LOG_INFO(
				"Exclusions: blacklist chord performed with nothing in the %s hand.",
				isLeftGameHand ? "left" : "right");
			ShowHUDMessage("Nothing in that hand to blacklist", nullptr, true);
			return false;
		}

		return ToggleOnBlacklist(target, fromMemory);
	}

	void ExclusionsNoteHeld()
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			return;
		}

		for (int hand = 0; hand < 2; ++hand)
		{
			TESForm* form = player->GetEquippedObject(hand == 1);
			if (form)
			{
				s_lastHeld[hand] = form->formID;
			}
		}
	}

	bool ExclusionsContains(const UInt32 formID)
	{
		return !s_resolved.empty() && s_resolved.find(formID) != s_resolved.end();
	}

	UInt32 ExclusionsCount()
	{
		return static_cast<UInt32>(s_saved.size());
	}
}
