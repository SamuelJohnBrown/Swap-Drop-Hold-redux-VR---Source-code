#include "DropModelWatch.h"

#include "ModelWatchRules.h"
#include "TrackedWeapons.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameForms.h>
#include <skse64/GameRTTI.h>
#include <skse64/GameReferences.h>
#include <skse64/NiNodes.h>
#include <skse64/PluginAPI.h>
#include <skse64/gamethreads.h>
#include <skse64_common/Relocation.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace SwapDropAndHoldRedux
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		// Everything below is SkyrimVR 1.4.15, read from the decompilation and checked
		// against the bytes of the unpacked executable.

		// The model loader's queue function, address library id 12910. For a reference that
		// already has a job it only changes the job's priority: a job still waiting in the
		// IOManager is taken out and put back in at the new priority, and a job already
		// loading is left alone. The IOManager keeps one queue per priority, and the engine
		// hands out 0 for attaching finished models and for SetRace, 1 for references in or
		// next to the player's cell, 3 for far ones - 0 is the front of the line. The last
		// argument must be false: true parks a new job on a main-thread list instead.
		const uintptr_t kQueueRefOffset = 0x001585E0;
		const unsigned char kQueueRefStart[] = {
			0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
			0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
		const UInt32 kFrontOfTheLine = 0;
		typedef void* (*QueueRef_t)(void* loader, TESObjectREFR* ref, UInt32 priority, bool mainThreadList);

		// The loader itself has no address library id. TESObjectREFR::Set3D (id 19302) reads
		// it with mov rcx, [rip+disp32] at +0xA3, so the address comes from that instruction -
		// and only when the instruction is exactly the one in the executable.
		const uintptr_t kSet3DOffset = 0x002A2010;
		const unsigned char kSet3DStart[] = {
			0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
			0x41, 0x57, 0x48, 0x8D, 0x68, 0xB8 };
		const uintptr_t kSet3DLoaderRead = 0xA3;
		const unsigned char kMovRcxRipRelative[] = { 0x48, 0x8B, 0x0D };
		const uintptr_t kLoaderGlobalOffset = 0x01F83228;

		// The loader's table of reference jobs is a BSTHashMap at +0x10: capacity at +0x14,
		// free entries at +0x18. The difference is how many references have a job right now.
		const uintptr_t kJobTableCapacity = 0x14;
		const uintptr_t kJobTableFree = 0x18;

		// Flags on a reference the engine will never give a model.
		const UInt32 kFlagDeleted = 0x20;
		const UInt32 kFlagDisabled = 0x800;

		// How long after its model comes a dropped weapon is looked at again, to say where it
		// ended up: long enough to have fallen and settled, short enough to beat most pickups.
		const float kLandingReportSeconds = 2.0f;

		bool s_checked = false;
		const char* s_mismatch = nullptr; // what did not match; null once checked means armed
		void** s_loaderGlobal = nullptr;
		QueueRef_t s_queueRef = nullptr;

		bool CodeMatches(const uintptr_t address, const unsigned char* expected, const size_t count)
		{
			return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0;
		}

		void CheckEngineCode()
		{
			if (s_checked)
			{
				return;
			}
			s_checked = true;

			const uintptr_t base = RelocationManager::s_baseAddr;
			if (base == 0)
			{
				s_mismatch = "the executable's base address";
				return;
			}

			const uintptr_t queueRef = base + kQueueRefOffset;
			if (!CodeMatches(queueRef, kQueueRefStart, sizeof(kQueueRefStart)))
			{
				s_mismatch = "the model loader's queue function";
				return;
			}

			const uintptr_t set3D = base + kSet3DOffset;
			if (!CodeMatches(set3D, kSet3DStart, sizeof(kSet3DStart)))
			{
				s_mismatch = "TESObjectREFR::Set3D";
				return;
			}

			const uintptr_t loaderRead = set3D + kSet3DLoaderRead;
			if (!CodeMatches(loaderRead, kMovRcxRipRelative, sizeof(kMovRcxRipRelative)))
			{
				s_mismatch = "the loader read inside TESObjectREFR::Set3D";
				return;
			}

			SInt32 displacement = 0;
			std::memcpy(
				&displacement,
				reinterpret_cast<const void*>(loaderRead + sizeof(kMovRcxRipRelative)),
				sizeof(displacement));
			const uintptr_t nextInstruction = loaderRead + sizeof(kMovRcxRipRelative) + sizeof(displacement);
			const uintptr_t loaderGlobal = nextInstruction + static_cast<uintptr_t>(static_cast<intptr_t>(displacement));
			if (loaderGlobal != base + kLoaderGlobalOffset)
			{
				s_mismatch = "the loader address inside TESObjectREFR::Set3D";
				return;
			}

			s_loaderGlobal = reinterpret_cast<void**>(loaderGlobal);
			s_queueRef = reinterpret_cast<QueueRef_t>(queueRef);
		}

		bool Armed()
		{
			CheckEngineCode();
			return s_mismatch == nullptr;
		}

		void* Loader()
		{
			return Armed() ? *s_loaderGlobal : nullptr;
		}

		// How many references have a job in the loader, or -1 when that cannot be read. Read
		// without the loader's lock: it is a number for the log, never used to decide anything.
		int ReferenceJobs()
		{
			const void* loader = Loader();
			if (!loader)
			{
				return -1;
			}

			const UInt32 capacity = *reinterpret_cast<const UInt32*>(static_cast<const char*>(loader) + kJobTableCapacity);
			const UInt32 freeEntries = *reinterpret_cast<const UInt32*>(static_cast<const char*>(loader) + kJobTableFree);
			return capacity >= freeEntries ? static_cast<int>(capacity - freeEntries) : -1;
		}

		void FormatJobs(const int jobs, char* out, const size_t size)
		{
			if (jobs < 0)
			{
				std::snprintf(out, size, "?");
			}
			else
			{
				std::snprintf(out, size, "%d", jobs);
			}
		}

		struct WatchedDrop
		{
			UInt32 refID = 0;
			UInt32 baseFormID = 0;
			char name[64] = {};
			int jobsAtDrop = -1;
			bool hurryCalled = false;
			float hurryCalledSeconds = 0.0f;
			float landingSeconds = -1.0f; // counting down to the landing report; negative when none is due
			NiPoint3 cameAt;              // where the model was on the frame it came
			ModelWatch::Watch watch;

			bool Busy() const
			{
				return watch.Active() || landingSeconds > 0.0f;
			}
		};

		const int kMaxWatchedDrops = 8;
		WatchedDrop s_drops[kMaxWatchedDrops];
		const ModelWatch::Config s_config{};

		// The dropped reference, while it is still the one that was dropped and the engine
		// could still give it a model. Reference ids get reused, hence the base form check.
		TESObjectREFR* ResolveDrop(const WatchedDrop& drop)
		{
			TESForm* form = LookupFormByID(drop.refID);
			TESObjectREFR* refr = DYNAMIC_CAST(form, TESForm, TESObjectREFR);
			if (!refr || !refr->baseForm || refr->baseForm->formID != drop.baseFormID)
			{
				return nullptr;
			}

			if ((refr->flags & (kFlagDeleted | kFlagDisabled)) != 0)
			{
				return nullptr;
			}

			return refr;
		}

		// Starts the landing report's clock from where the model appeared.
		void NoteModelCame(WatchedDrop& drop, TESObjectREFR* refr)
		{
			NiNode* node = refr->GetNiNode();
			if (!node)
			{
				return;
			}

			drop.cameAt = node->m_worldTransform.pos;
			drop.landingSeconds = kLandingReportSeconds;
		}

		// Where a dropped weapon ended up, against where the player stands. A weapon that fell
		// through the floor reads as far below the feet; one knocked away reads as far off.
		void ReportLanding(const WatchedDrop& drop)
		{
			TESObjectREFR* refr = ResolveDrop(drop);
			PlayerCharacter* player = *g_thePlayer;
			if (!refr || !player)
			{
				return; // picked up, or otherwise gone, before there was anything to say
			}

			NiNode* node = refr->GetNiNode();
			if (!node)
			{
				LOG_INFO(
					"Dropped model: %s refId=%08X had a model %.1fs ago and has none now.",
					drop.name,
					drop.refID,
					kLandingReportSeconds);
				return;
			}

			const NiPoint3& at = node->m_worldTransform.pos;
			const float dx = at.x - player->pos.x;
			const float dy = at.y - player->pos.y;
			const float dz = at.z - player->pos.z;
			const float mx = at.x - drop.cameAt.x;
			const float my = at.y - drop.cameAt.y;
			const float mz = at.z - drop.cameAt.z;
			LOG_INFO(
				"Dropped model: %s refId=%08X, %.1fs after its model came, lies %.0f units from you and %.0f units %s your feet (it moved %.0f units after it came).",
				drop.name,
				drop.refID,
				kLandingReportSeconds,
				std::sqrt(dx * dx + dy * dy),
				std::fabs(dz),
				dz >= 0.0f ? "above" : "below",
				std::sqrt(mx * mx + my * my + mz * mz));
		}

		// Runs from the task queue rather than the physics step it was decided in: a job the
		// loader can finish on the spot attaches the model, collision and all, inside this call.
		class HurryDropModelTask : public TaskDelegate
		{
		public:
			HurryDropModelTask(const int slot, const UInt32 refID)
				: m_slot(slot)
				, m_refID(refID)
			{
			}

			virtual void Run() override
			{
				WatchedDrop& drop = s_drops[m_slot];
				if (drop.refID != m_refID || !drop.watch.Active())
				{
					return; // the model came first, or the slot went to a newer drop
				}

				TESObjectREFR* refr = ResolveDrop(drop);
				void* loader = Loader();
				if (!refr || refr->GetNiNode() || !loader)
				{
					return;
				}

				s_queueRef(loader, refr, kFrontOfTheLine, false);
				drop.hurryCalled = true;
				drop.hurryCalledSeconds = drop.watch.Seconds();

				char jobs[16];
				FormatJobs(ReferenceJobs(), jobs, sizeof(jobs));
				LOG_INFO(
					"Dropped model: %s refId=%08X still had no model %.2fs after the drop - moved it to the front of the engine's model loader (loader jobs: %s).",
					drop.name,
					drop.refID,
					drop.watch.Seconds(),
					jobs);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			int m_slot;
			UInt32 m_refID;
		};
	}

	void DropModelWatchBegin(TESObjectREFR* droppedRef, TESForm* item)
	{
		if (!droppedRef || !droppedRef->baseForm)
		{
			return;
		}

		// A reference id the engine hands out again belongs to the newest drop: an older slot
		// still waiting to say where that id lies would describe this drop's weapon, not its own.
		int slot = -1;
		for (int i = 0; i < kMaxWatchedDrops && slot < 0; ++i)
		{
			if (s_drops[i].Busy() && s_drops[i].refID == droppedRef->formID)
			{
				slot = i;
			}
		}

		// Otherwise a free slot, or else the one whose watch has been waiting longest.
		for (int i = 0; i < kMaxWatchedDrops && slot < 0; ++i)
		{
			if (!s_drops[i].Busy())
			{
				slot = i;
			}
		}
		if (slot < 0)
		{
			slot = 0;
			for (int i = 1; i < kMaxWatchedDrops; ++i)
			{
				if (s_drops[i].watch.Seconds() > s_drops[slot].watch.Seconds())
				{
					slot = i;
				}
			}
		}

		WatchedDrop& drop = s_drops[slot];
		drop = WatchedDrop{};
		drop.refID = droppedRef->formID;
		drop.baseFormID = droppedRef->baseForm->formID;
		std::snprintf(drop.name, sizeof(drop.name), "%s", GetSafeFormName(item ? item : droppedRef->baseForm));
		drop.jobsAtDrop = ReferenceJobs();
		drop.watch.Begin();
	}

	void DropModelWatchTick(const float deltaTime)
	{
		for (int slot = 0; slot < kMaxWatchedDrops; ++slot)
		{
			WatchedDrop& drop = s_drops[slot];

			if (drop.landingSeconds > 0.0f)
			{
				drop.landingSeconds -= deltaTime;
				if (drop.landingSeconds <= 0.0f)
				{
					drop.landingSeconds = -1.0f;
					ReportLanding(drop);
				}
				continue;
			}

			if (!drop.watch.Active())
			{
				continue;
			}

			TESObjectREFR* refr = ResolveDrop(drop);
			if (!refr)
			{
				LOG_INFO(
					"Dropped model: %s refId=%08X was gone %.2fs after the drop, before its model came.",
					drop.name,
					drop.refID,
					drop.watch.Seconds());
				drop.watch = ModelWatch::Watch{};
				continue;
			}

			char jobsAtDrop[16];
			FormatJobs(drop.jobsAtDrop, jobsAtDrop, sizeof(jobsAtDrop));

			switch (drop.watch.Update(refr->GetNiNode() != nullptr, deltaTime, s_config))
			{
			case ModelWatch::Step::kAppeared:
				if (drop.watch.Seconds() < s_config.hurryAfterSeconds)
				{
					LOG_INFO(
						"Dropped model: %s refId=%08X came %.3fs after the drop (%d frames, loader jobs at the drop: %s).",
						drop.name,
						drop.refID,
						drop.watch.Seconds(),
						drop.watch.Frames(),
						jobsAtDrop);
				}
				else if (drop.hurryCalled)
				{
					LOG_INFO(
						"Dropped model: %s refId=%08X came %.2fs after the drop - LATE (%d frames). It was hurried %.2fs after the drop and came %.2fs after that; loader jobs at the drop: %s.",
						drop.name,
						drop.refID,
						drop.watch.Seconds(),
						drop.watch.Frames(),
						drop.hurryCalledSeconds,
						drop.watch.Seconds() - drop.hurryCalledSeconds,
						jobsAtDrop);
				}
				else
				{
					LOG_INFO(
						"Dropped model: %s refId=%08X came %.2fs after the drop - LATE (%d frames), and it was not hurried; loader jobs at the drop: %s.",
						drop.name,
						drop.refID,
						drop.watch.Seconds(),
						drop.watch.Frames(),
						jobsAtDrop);
				}
				NoteModelCame(drop, refr);
				break;

			case ModelWatch::Step::kHurry:
				if (!Armed())
				{
					LOG_INFO(
						"Dropped model: %s refId=%08X still has no model %.2fs after the drop - not hurried, because %s did not match this build's check.",
						drop.name,
						drop.refID,
						drop.watch.Seconds(),
						s_mismatch);
				}
				else if (g_task)
				{
					g_task->AddTask(new HurryDropModelTask(slot, drop.refID));
				}
				break;

			case ModelWatch::Step::kGaveUp:
				LOG_INFO(
					"Dropped model: %s refId=%08X still had no model %.0fs after the drop - stopped watching (%s; loader jobs at the drop: %s).",
					drop.name,
					drop.refID,
					drop.watch.Seconds(),
					drop.hurryCalled ? "it was hurried" : "it was not hurried",
					jobsAtDrop);
				break;

			default:
				break;
			}
		}
	}

	int DropModelWatchLoaderJobs()
	{
		return ReferenceJobs();
	}

	void DropModelWatchReportArming()
	{
		if (Armed())
		{
			LOG_INFO(
				"Dropped model watch: armed. The engine's model loader matched this build's check, so a dropped weapon still without a model %.2fs after the drop is moved to the front of the loader. Every drop logs how long its model took, and where the weapon lies %.0fs later.",
				s_config.hurryAfterSeconds,
				kLandingReportSeconds);
		}
		else
		{
			LOG_ERR(
				"Dropped model watch: NOT armed - %s did not match this build's check, so nothing is hurried. Every drop still logs how long its model took.",
				s_mismatch);
		}
	}
}
