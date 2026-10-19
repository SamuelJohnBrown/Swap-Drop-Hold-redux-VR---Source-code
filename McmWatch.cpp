#include "McmWatch.h"

#include "Engine.h"
#include "Helper.h"
#include "McmBridge.h"
#include "McmSelfRepair.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameForms.h>
#include <skse64/GameTypes.h>
#include <skse64/PapyrusArgs.h>
#include <skse64/PapyrusInterfaces.h>
#include <skse64/PapyrusVM.h>
#include <skse64/PapyrusValue.h>

#include <chrono>
#include <string>

// WHAT WAS CHECKED IN SkyrimVR.exe 1.4.15 (unpacked image, 2026-09-16) BEFORE ANY OF THIS WAS WRITTEN.
//
// The classic SDK's headers are older than CommonLib's and disagree with them in places, so each call below was
// read in the binary rather than trusted from either:
//
// - BSScript::Internal::VirtualMachine's vtable is at 0x1418E2148. Its slot 0x28 is 0x14127A660, the
//   DispatchStaticCall already verified for Lethal Unarmed VR, which anchors every other slot.
// - Slot 0x2F (VMClassRegistry::GetHandlePolicy) returns *(vm + 0x90).
// - The handle policy's vtable (0x141755C68) has 12 slots - the classic header's count, not CommonLib's 15. Slot 3
//   (GetInvalidHandle) returns the constant 0x0000FFFF00000000; slot 4 (Create) returns 0xFFFF00000000 | formID for
//   a form, allocating nothing; slot 10 is the Release the engine's own script-object release calls.
// - Slot 0x1E (VMClassRegistry::Unk_1C, CommonLib's FindBoundObject) takes the attached-scripts lock at vm + 0x93A0
//   and calls the worker at 0x141284D30, which assigns its out slot like a smart pointer: a reference is ADDED to
//   the script object it hands back, and whatever the slot held is released. So every object found here holds a
//   reference, and ScriptRef gives it back. VMIdentifier::DecrementLock and ::Destroy in skse64/PapyrusVM.cpp are
//   line-for-line the engine's release (0x14126C8C0: lock bit 0x80000000, spin 10000, 2 -> 1 releases the handle)
//   and destroy (Destroy_Internal 0x14126BB20, then the heap free).
// - VMClassInfo::GetVariable (0x141270A60) finds a VARIABLE by name - not a property - in the class and then every
//   parent, and returns its ABSOLUTE index: the local index plus the variable counts of all parent classes.
// - The script object's destructor (0x14126BB20) walks its variables as `lea rbx,[rdi+30h]` ... `add rbx,10h`, over
//   the count 0x141270550 returns, and only when bit 0 of the object's first word is set. So a variable lives at
//   object + 0x30 + 0x10 * index, which is the storage CommonLib's Object::GetVariable - and so MCM Helper's own VR
//   build - reads and writes.
// - Slot 0x26 (QueueEvent, CommonLib's SendEvent) is the call SKSE's ModCallbackEventFunctor makes to deliver every
//   mod event, SkyUI's SKICP_configManagerReady broadcast included; it copies the arguments before returning.
// - VMValue::Destroy (0x14126F1C0) is the call inside that destructor's loop, and IFunctionArguments::Output::Resize
//   (0x14095AA60) starts a real function.
// - THE SDK'S VMValue::SetArray (0x0126E410) IS MISDECLARED: it is declared to take the array, but the engine
//   function stores *argument - it takes a pointer TO the array. It is not used here.
// - SkyUI's compiled scripts name what is read: SKI_ConfigBase.pex holds ::ModName_var, ::Pages_var, _configID,
//   OnConfigManagerReset and OnConfigManagerReady; SKI_ConfigManager.pex holds _modConfigs and _modNames;
//   MCM_ConfigBase.pex overrides OnConfigManagerReady. The auto properties ModName and Pages are read through their
//   backing variables because GetVariable only finds variables.
//
// Everything runs on the main thread, from a HIGGS pre-physics callback, as MCM Helper's natives do; and it is armed
// only between a finished load and the next one, because a load throws away the script objects it reads.

namespace SwapDropAndHoldRedux
{
	namespace
	{
		constexpr const char* kInternalName = "SwapDropAndHoldRedux";
		// Where MCM Helper looks for SkyUI's manager too. SkyUI VR ships as SkyUI_SE.esp; it is on this install.
		constexpr const char* kSkyUIPlugin = "SkyUI_SE.esp";
		constexpr UInt32 kSkyUIManagerQuest = 0x802;

		// A script object that FindBoundObject handed back, together with the reference it came with.
		class ScriptRef
		{
		public:
			ScriptRef() = default;
			ScriptRef(const ScriptRef&) = delete;
			ScriptRef& operator=(const ScriptRef&) = delete;

			ScriptRef(ScriptRef&& other) noexcept :
				m_object(other.m_object)
			{
				other.m_object = nullptr;
			}

			ScriptRef& operator=(ScriptRef&& other) noexcept
			{
				if (this != &other)
				{
					Reset();
					m_object = other.m_object;
					other.m_object = nullptr;
				}
				return *this;
			}

			~ScriptRef()
			{
				Reset();
			}

			// Empty, so FindBoundObject has nothing of ours to release when it assigns.
			VMIdentifier** Out()
			{
				Reset();
				return &m_object;
			}

			VMIdentifier* Get() const
			{
				return m_object;
			}

			explicit operator bool() const
			{
				return m_object != nullptr;
			}

			void Reset()
			{
				if (m_object && m_object->DecrementLock() == 0)
				{
					m_object->Destroy();
				}
				m_object = nullptr;
			}

		private:
			VMIdentifier* m_object = nullptr;
		};

		VMClassRegistry* Registry()
		{
			SkyrimVM* const vm = g_skyrimVM ? *g_skyrimVM : nullptr;
			return vm ? vm->GetClassRegistry() : nullptr;
		}

		TESQuest* QuestFor(const char* plugin, const UInt32 localId)
		{
			const UInt32 formId = GetFullFormIdMine(plugin, localId);
			TESForm* const form = formId ? LookupFormByID(formId) : nullptr;
			return form && form->formType == kFormType_Quest ? static_cast<TESQuest*>(form) : nullptr;
		}

		UInt64 HandleFor(VMClassRegistry* registry, TESForm* form, bool& valid)
		{
			valid = false;
			IObjectHandlePolicy* const policy = registry ? registry->GetHandlePolicy() : nullptr;
			if (!form || !policy)
			{
				return 0;
			}
			const UInt64 handle = policy->Create(form->formType, form);
			valid = handle != policy->GetInvalidHandle();
			return handle;
		}

		ScriptRef BoundScript(VMClassRegistry* registry, TESForm* form, const char* scriptName)
		{
			ScriptRef out;
			bool valid = false;
			const UInt64 handle = HandleFor(registry, form, valid);
			if (valid)
			{
				registry->Unk_1C(handle, scriptName, out.Out());
			}
			return out;
		}

		// The live storage of a script variable, by the name the compiler gave it.
		VMValue* Variable(VMIdentifier* object, const char* name)
		{
			if (!object || !object->m_type)
			{
				return nullptr;
			}
			// The SDK calls the first word m_refCount; the engine's destructor treats its bit 0 as "the variables
			// exist", and so does this.
			if (!(static_cast<UInt32>(object->m_refCount) & 1))
			{
				return nullptr;
			}
			BSFixedString variableName(name);
			const SInt32 index = CALL_MEMBER_FN(object->m_type, GetVariable)(&variableName);
			if (index < 0)
			{
				return nullptr;
			}
			return reinterpret_cast<VMValue*>(reinterpret_cast<char*>(object) + 0x30) + index;
		}

		VMValue::ArrayData* ArrayOf(VMValue* value)
		{
			return value && value->IsArray() ? value->data.arr : nullptr;
		}

		struct Reading
		{
			mcm_selfrepair::MenuState state;
			TESQuest* quest = nullptr;
			ScriptRef menu;
			ScriptRef manager;
			TESQuest* managerQuest = nullptr;
		};

		Reading Read(VMClassRegistry* registry)
		{
			Reading r;
			r.quest = QuestFor(kMcmEspName, kMcmQuestFormID);
			r.state.questFound = r.quest != nullptr;
			if (!r.quest)
			{
				return r;
			}

			r.menu = BoundScript(registry, r.quest, "MCM_ConfigBase");
			r.state.scriptFound = static_cast<bool>(r.menu);
			if (!r.menu)
			{
				return r;
			}

			if (VMValue* const title = Variable(r.menu.Get(), "::ModName_var");
				title && title->type == VMValue::kType_String)
			{
				r.state.title = title->data.str ? title->data.str : "";
			}
			if (VMValue* const pages = Variable(r.menu.Get(), "::Pages_var"))
			{
				const VMValue::ArrayData* const list = ArrayOf(pages);
				r.state.pageCount = list ? static_cast<int>(list->len) : 0;
			}
			if (VMValue* const id = Variable(r.menu.Get(), "_configID"); id && id->type == VMValue::kType_Int)
			{
				r.state.configId = id->data.i;
			}

			r.managerQuest = QuestFor(kSkyUIPlugin, kSkyUIManagerQuest);
			r.manager = BoundScript(registry, r.managerQuest, "SKI_ConfigManager");
			r.state.managerFound = static_cast<bool>(r.manager);
			if (!r.manager)
			{
				return r;
			}

			VMValue* const configs = Variable(r.manager.Get(), "_modConfigs");
			VMValue* const names = Variable(r.manager.Get(), "_modNames");
			r.state.standardLayout = configs && configs->IsArray() && names && names->IsArray();
			if (!r.state.standardLayout || r.state.configId < 0)
			{
				return r;
			}

			const UInt32 slot = static_cast<UInt32>(r.state.configId);
			if (VMValue::ArrayData* const list = ArrayOf(configs); list && slot < list->len)
			{
				VMValue& entry = list->GetData()[slot];
				r.state.listedAtId =
					entry.GetUnmangledType() == VMValue::kType_Identifier && entry.data.id == r.menu.Get();
			}
			if (VMValue::ArrayData* const list = ArrayOf(names); list && slot < list->len)
			{
				VMValue& entry = list->GetData()[slot];
				if (entry.type == VMValue::kType_String)
				{
					r.state.listName = entry.data.str ? entry.data.str : "";
					r.state.haveListName = true;
				}
			}
			return r;
		}

		// SkyUI's broadcast, exactly as SKSE delivers it: skse64/PapyrusEvents.cpp's ModCallbackEventFunctor. It lives in
		// a .cpp there, so it is repeated here - the event name, an empty string, zero, and SkyUI's manager quest as
		// the sender.
		class ConfigManagerEvent : public IFunctionArguments
		{
		public:
			ConfigManagerEvent(const char* eventName, TESForm* sender) :
				m_eventName(eventName),
				m_strArg(""),
				m_sender(sender)
			{
			}

			bool Copy(Output* dst) override
			{
				VMClassRegistry* const registry = Registry();
				if (!registry)
				{
					return false;
				}
				dst->Resize(4);
				dst->Get(0)->SetString(m_eventName.data);
				dst->Get(1)->SetString(m_strArg.data);
				dst->Get(2)->SetFloat(0.0f);
				PackValue(dst->Get(3), &m_sender, registry);
				return true;
			}

		private:
			BSFixedString m_eventName;
			BSFixedString m_strArg;
			TESForm* m_sender;
		};

		// -> what happened, for the log line.
		std::string Refill(VMClassRegistry* registry, Reading& r)
		{
			VMValue* const id = r.menu ? Variable(r.menu.Get(), "_configID") : nullptr;
			bool valid = false;
			const UInt64 handle = HandleFor(registry, r.quest, valid);
			if (!registry || !valid || !id || id->type != VMValue::kType_Int || !r.managerQuest)
			{
				return "NOT DONE -- the VM, the menu's handle, the script's _configID or SkyUI's quest was missing";
			}

			// So MCM Helper's LoadConfig stops returning early: it does whenever SkyUI's _modConfigs[_configID] is
			// this script.
			id->SetInt(-1);

			BSFixedString resetEvent("OnConfigManagerReset");
			BSFixedString readyEvent("OnConfigManagerReady");
			ConfigManagerEvent resetArgs("SKICP_configManagerReset", r.managerQuest);
			ConfigManagerEvent readyArgs("SKICP_configManagerReady", r.managerQuest);
			registry->QueueEvent(handle, &resetEvent, &resetArgs);
			registry->QueueEvent(handle, &readyEvent, &readyArgs);
			return "the menu's reset and ready events are queued (SkyUI's own broadcast, within 30 s, is the fallback)";
		}

		bool Rename(Reading& r)
		{
			VMValue::ArrayData* const list =
				r.manager ? ArrayOf(Variable(r.manager.Get(), "_modNames")) : nullptr;
			if (!list || r.state.configId < 0 || static_cast<UInt32>(r.state.configId) >= list->len)
			{
				return false;
			}
			list->GetData()[r.state.configId].SetString(r.state.title.c_str());
			return true;
		}

		// bMcmRepairSelfTest: put a WORKING menu into the state the modlist authors saw - no pages, the internal name as
		// its title and in SkyUI's list - so the repair can be watched on any save.
		bool BlankForSelfTest(Reading& r)
		{
			VMValue* const pages = r.menu ? Variable(r.menu.Get(), "::Pages_var") : nullptr;
			VMValue* const title = r.menu ? Variable(r.menu.Get(), "::ModName_var") : nullptr;
			if (!pages || !title || !pages->IsLiteralArray())
			{
				return false;
			}
			// What the engine's VMValue::SetArray (0x14126E410) does with no array: destroy the old value, store null,
			// keep the array type. A string[] holding no array reads as 0 pages, like the empty one CommonLib builds.
			const UInt64 arrayType = pages->type;
			CALL_MEMBER_FN(pages, Destroy)();
			pages->type = arrayType;
			pages->data.arr = nullptr;
			title->SetString(kInternalName);
			r.state.title = kInternalName;
			Rename(r);
			return true;
		}

		struct Watch
		{
			bool armed = false;
			std::chrono::steady_clock::time_point since{};
			std::chrono::steady_clock::time_point lastCheck{};
			std::string lastSaid;
			bool blankedForSelfTest = false;
			bool saidOff = false;
			bool saidGaveUp = false;
			mcm_selfrepair::Repairer repairer;
		};

		Watch s_watch;

		void Tick()
		{
			using mcm_selfrepair::Action;
			using mcm_selfrepair::Verdict;

			Watch& w = s_watch;
			if (!w.armed)
			{
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			if (now - w.lastCheck < std::chrono::seconds(2))
			{
				return;
			}
			w.lastCheck = now;
			const double t = std::chrono::duration<double>(now - w.since).count();

			VMClassRegistry* const registry = Registry();
			if (!registry)
			{
				return;
			}

			Reading r = Read(registry);
			const Verdict verdict = mcm_selfrepair::Judge(r.state);
			if (verdict == Verdict::kNoMenu)
			{
				LOG_INFO("[mcmfix] +%.0fs no menu plugin loaded -- nothing to watch (expected without the menu)", t);
				w.armed = false;
				return;
			}

			std::string said = std::string(mcm_selfrepair::VerdictText(verdict)) + ": " + mcm_selfrepair::Describe(r.state);
			if (said != w.lastSaid)
			{
				LOG_INFO("[mcmfix] +%.0fs %s", t, said.c_str());
				w.lastSaid = std::move(said);
			}
			if (verdict == Verdict::kHealthy)
			{
				w.saidOff = false;
				w.saidGaveUp = false;
			}

			if (mcm_selfrepair::ShouldBlankForSelfTest(mcmRepairSelfTest, w.blankedForSelfTest, verdict))
			{
				w.blankedForSelfTest = true;
				const bool blanked = BlankForSelfTest(r);
				LOG_INFO(
					"[mcmfix] +%.0fs SELF-TEST (bMcmRepairSelfTest = 1): %s",
					t,
					blanked
						? "blanked the menu on purpose -- no pages, the internal name as its title and in SkyUI's list. "
						  "The next check should find it BLANK and repair it."
						: "could NOT blank the menu -- no test this load");
				return; // judged again on the next check, from a fresh reading
			}

			switch (w.repairer.Next(verdict, t, mcmSelfRepair))
			{
			case Action::kRefill:
			{
				const std::string how = Refill(registry, r);
				LOG_INFO(
					"[mcmfix] +%.0fs REPAIR %d of %d: SkyUI lists the menu but its page list is empty, so MCM Helper is "
					"asked to read config.json again -- %s",
					t,
					w.repairer.refills,
					mcm_selfrepair::Repairer::kMaxRefillsPerEpisode,
					how.c_str());
				break;
			}
			case Action::kRename:
			{
				const std::string was = r.state.listName;
				const bool renamed = Rename(r);
				LOG_INFO(
					"[mcmfix] +%.0fs SkyUI's mod list said \"%s\" -- %s%s%s",
					t,
					was.c_str(),
					renamed ? "changed to the title, \"" : "could NOT be changed",
					renamed ? r.state.title.c_str() : "",
					renamed ? "\"" : "");
				break;
			}
			case Action::kNone:
				if (verdict != Verdict::kEmptyPages)
				{
					break;
				}
				if (!mcmSelfRepair && !w.saidOff)
				{
					w.saidOff = true;
					LOG_INFO(
						"[mcmfix] +%.0fs the menu is BLANK and would be repaired, but bMcmSelfRepair = 0 -- save and "
						"reload once to fill it in",
						t);
				}
				else if (w.repairer.GaveUp() && !w.saidGaveUp)
				{
					w.saidGaveUp = true;
					LOG(
						"[mcmfix] +%.0fs the menu is still BLANK after %d repair(s) -- giving up for now. Save and "
						"reload once to fill it in.",
						t,
						w.repairer.refills);
				}
				break;
			}
		}

		void OnMcmWatchStep(void* /*world*/)
		{
			Tick();
		}
	}

	void RegisterMcmWatch()
	{
		if (!higgsInterface)
		{
			LOG_ERR("[mcmfix] HIGGS is not available, so the in-game menu will not be watched or repaired.");
			return;
		}
		higgsInterface->AddPrePhysicsStepCallback(OnMcmWatchStep);
	}

	void McmWatchArm(const char* const why)
	{
		const int total = s_watch.repairer.totalRefills;
		s_watch = Watch{};
		s_watch.repairer.totalRefills = total;
		s_watch.armed = true;
		s_watch.since = std::chrono::steady_clock::now();
		LOG_INFO(
			"[mcmfix] %s: watching the in-game menu every 2 s (bMcmSelfRepair = %d, bMcmRepairSelfTest = %d)",
			why,
			mcmSelfRepair ? 1 : 0,
			mcmRepairSelfTest ? 1 : 0);
	}

	void McmWatchDisarm()
	{
		s_watch.armed = false;
	}
}
