#include "GameGripGuard.h"

#include "Engine.h"
#include "GameGripRules.h"
#include "MenuGrip.h"
#include "config.h"

#include <skse64/GameVR.h>
#include <skse64_common/SafeWrite.h>

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

// WHAT WAS CHECKED BEFORE THIS HOOK WAS WRITTEN (2026-09-22).
//
// - The slot: GetControllerState is virtual number 34 of vr_1_0_12::IVRSystem (counted from
//   skse64/openvr_1_0_12.h: 33 GetHiddenAreaMesh, 34 GetControllerState, 35 ...WithPose, 36
//   TriggerHapticPulse), and HapticCensus.cpp's hook on slot 36 of the SAME vtable is proven in game.
// - The game's reads: BSOpenVRControllerDevice (0x140C5F3E0) fetches the object at BSOpenVR+0x208 on
//   every poll and calls its slot 34 through the vtable - vtable+272 - with a 64-byte state, IGNORES
//   the return value, and turns changes of ulButtonPressed into button events. Read from the
//   decompilation, not assumed.
// - That object is SkyrimVRTools' own IVRSystem (its vtable lives in skyrimvrtools.dll - the test9 install
//   line: "taken from skyrimvrtools.dll"). Its GetControllerState reaches SKSE's proxy - real OpenVR, then
//   the SKSE callbacks, MenuGrip's blink among them - and runs its own callbacks, Weapon Throw VR's, on
//   the result. That is why Weapon Throw VR sees the blink.
// - This hook takes the slot from SkyrimVRTools and calls it, so it is outermost. It only arms while the
//   slot belongs to skyrimvrtools.dll (or, without SkyrimVRTools, to SKSE's proxy or OpenVR): arming
//   INSIDE SkyrimVRTools would hand Weapon Throw VR the real grip and break throwing again.
// - The return address says who is asking: SkyrimVR.exe's own reads come from its image. Mods that read
//   through the vtable themselves get exactly what they got before.
// - A vtable-slot swap of one aligned pointer, as HapticCensus.cpp: no code patched, no trampoline, a caller
//   mid-flight sees either function. The hook body is atomics and a clock read: no engine calls, no logging,
//   no controller reads - it runs on whatever thread polled.
//
// test9 installed exactly here and handed back 0 reads all session, because it only ever used a record made
// inside the same call, and only when that call answered true. GameGripRules.h has why neither was the
// game's condition, and what test10 does instead; each blink's reads are counted and logged, so the next log
// says which reads the blink reached and how many of those the game was handed back.

namespace SwapDropAndHoldRedux
{
	namespace
	{
		constexpr int kSlotGetControllerState = 34;
		constexpr int kSlotGetTrackedDeviceIndexForControllerRole = 17; // one of SKSE's own proxy functions
		constexpr int kArmingStepsMax = 90 * 30;                        // about half a minute of physics steps
		constexpr int kSqueezeAxis = 2;
		constexpr long long kTicksPerSecond = 1000000000LL;
		constexpr long long kGraceTicks = static_cast<long long>(GameGrip::kGraceSeconds * kTicksPerSecond);
		constexpr long long kFreshTicks = static_cast<long long>(GameGrip::kFreshSeconds * kTicksPerSecond);

		using GetStateFn = bool (*)(vr_1_0_12::IVRSystem*, vr_1_0_12::TrackedDeviceIndex_t,
			vr_1_0_12::VRControllerState_t*, uint32_t);

		GetStateFn s_original = nullptr;
		void** s_vtable = nullptr;
		bool s_installed = false;
		bool s_gaveUp = false;
		bool s_saidNotOutermost = false;
		bool s_saidObjectChanged = false;
		int s_armingSteps = 0;
		uintptr_t s_gameBase = 0;
		uintptr_t s_gameEnd = 0;
		std::atomic<bool> s_on{ false };

		// test11: the function that has taken over the game's GetControllerState call (VRIK's, on this setup) -
		// reads made from inside it are the game's. 0/0 while nobody has. Written on the physics step, read by
		// the hook on any thread.
		std::atomic<uintptr_t> s_proxyBegin{ 0 };
		std::atomic<uintptr_t> s_proxyEnd{ 0 };
		uint8_t s_siteSeen[6] = {};
		bool s_haveSiteSeen = false;

		// What MenuGrip's callback last saw of each hand's grip. [0] = right VR controller, [1] = left.
		std::atomic<GameGrip::Record> s_record[2];

		// Counted in the hook, reported from the physics step.
		std::atomic<long long> s_gameReads{ 0 };      // every read made for the game through the guard
		std::atomic<long long> s_gameReadsNotOk{ 0 }; // ...that the layer under it answered false to
		std::atomic<uintptr_t> s_firstGameCaller{ 0 };
		std::atomic<long long> s_blinkReads[2]{ { 0 }, { 0 } };      // game reads of that hand in a blink window
		std::atomic<long long> s_blinkReached[2]{ { 0 }, { 0 } };    // ...that arrived with the real grip hidden
		std::atomic<long long> s_blinkHandedBack[2]{ { 0 }, { 0 } }; // ...and were handed it back
		std::atomic<long long> s_blinkKept[2]{ { 0 }, { 0 } };       // ...and were shown the view kept from before (test13)

		// test13: what the game was shown of each grip, kept from before a pickup until the hand opens
		// (GameGripRules.h GameViewRead). Only the game's reads touch it.
		constexpr uint64_t kHiddenBits = (1ull << 2) | (1ull << 34); // the bits MenuGrip's blink hides
		std::mutex s_viewLock;
		GameGrip::GameView s_view[2];
		std::atomic<bool> s_keptHeld[2]{ { false }, { false } };    // the kept view shows the grip held
		std::atomic<long long> s_keptSince[2]{ { 0 }, { 0 } };
		std::atomic<long long> s_keptEnds[2]{ { 0 }, { 0 } };       // kept views that ended with the hand opening
		std::atomic<long long> s_keptLastTicks[2]{ { 0 }, { 0 } };  // ...how long the last one lasted
		long long s_keptEndsSaid[2] = { 0, 0 };

		bool s_saidFirstRead = false;
		bool s_saidNotOk = false;

		struct BlinkReport
		{
			long long start = 0;
			bool pending = false;
			long long reads0 = 0;
			long long reached0 = 0;
			long long handedBack0 = 0;
			long long kept0 = 0;
		};
		BlinkReport s_report[2];

		// A clock read, not an engine call, so the hook may use it. The same clock as MenuGrip's window.
		long long NowTicks()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		bool Hooked_GetControllerState(
			vr_1_0_12::IVRSystem* self,
			vr_1_0_12::TrackedDeviceIndex_t device,
			vr_1_0_12::VRControllerState_t* state,
			uint32_t size)
		{
			const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
			const bool ok = s_original ? s_original(self, device, state, size) : false;
			if (!state || size < sizeof(vr_1_0_12::VRControllerState001_t) || !s_on.load(std::memory_order_relaxed) ||
				!GameGrip::IsGameRead(caller, s_gameBase, s_gameEnd, s_proxyBegin.load(std::memory_order_relaxed),
					s_proxyEnd.load(std::memory_order_relaxed)))
			{
				return ok;
			}

			// The game uses the state whatever the answer, so the guard does too.
			s_gameReads.fetch_add(1, std::memory_order_relaxed);
			if (!ok)
			{
				s_gameReadsNotOk.fetch_add(1, std::memory_order_relaxed);
			}
			uintptr_t none = 0;
			s_firstGameCaller.compare_exchange_strong(none, caller, std::memory_order_relaxed);

			const long long now = NowTicks();
			for (int hand = 0; hand < 2; ++hand)
			{
				if (device != MenuGripDevice(hand == 1))
				{
					continue;
				}
				long long start = 0;
				long long end = 0;
				MenuGripBlinkWindow(hand == 1, start, end); // 0/0 until this hand's first blink
				const GameGrip::Record rec = s_record[hand].load(std::memory_order_relaxed);
				const bool inWindow = end != 0 && GameGrip::InWindow(now, start, end, kGraceTicks);
				const bool reached = inWindow && rec.device == device && rec.ticks >= start &&
					GameGrip::ReachedGame(rec, state->ulButtonPressed, state->ulButtonTouched);

				// test13: the game is shown this grip as it was before the pickup until the hand opens
				// (GameGripRules.h) - never a press the pickup made.
				GameGrip::ViewStep step = GameGrip::ViewStep::kPassThrough;
				bool wasKept = false;
				bool isKept = false;
				uint64_t keptPressed = 0;
				{
					std::lock_guard<std::mutex> lock(s_viewLock);
					wasKept = s_view[hand].latched;
					step = GameGrip::GameViewRead(s_view[hand], device, now, start, end, kGraceTicks, kFreshTicks, rec,
						kHiddenBits, state->ulButtonPressed, state->ulButtonTouched, state->rAxis[kSqueezeAxis].x,
						state->rAxis[kSqueezeAxis].y);
					isKept = s_view[hand].latched;
					keptPressed = s_view[hand].latchPressed;
				}
				if (!wasKept && isKept)
				{
					s_keptSince[hand].store(now, std::memory_order_relaxed);
					s_keptHeld[hand].store((keptPressed & (1ull << 2)) != 0, std::memory_order_relaxed);
				}
				else if (wasKept && !isKept)
				{
					s_keptLastTicks[hand].store(now - s_keptSince[hand].load(std::memory_order_relaxed), std::memory_order_relaxed);
					s_keptEnds[hand].fetch_add(1, std::memory_order_relaxed);
				}

				if (inWindow)
				{
					s_blinkReads[hand].fetch_add(1, std::memory_order_relaxed);
					if (reached)
					{
						s_blinkReached[hand].fetch_add(1, std::memory_order_relaxed);
					}
					if (step == GameGrip::ViewStep::kKept)
					{
						s_blinkKept[hand].fetch_add(1, std::memory_order_relaxed);
					}
					else if (step == GameGrip::ViewStep::kHandedBack && reached)
					{
						s_blinkHandedBack[hand].fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
			return ok;
		}

		const char* ModuleOf(const void* const address, char* const out, const size_t size)
		{
			HMODULE module = nullptr;
			if (GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCSTR>(address),
					&module) &&
				module)
			{
				char path[MAX_PATH];
				if (GetModuleFileNameA(module, path, MAX_PATH))
				{
					const char* const slash = std::strrchr(path, '\\');
					strncpy_s(out, size, slash ? slash + 1 : path, _TRUNCATE);
					return out;
				}
			}
			strncpy_s(out, size, "(no module)", _TRUNCATE);
			return out;
		}

		bool ReadGameImage()
		{
			const HMODULE exe = GetModuleHandleW(nullptr);
			if (!exe)
			{
				return false;
			}
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			{
				return false;
			}
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(exe) + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0)
			{
				return false;
			}
			s_gameBase = reinterpret_cast<uintptr_t>(exe);
			s_gameEnd = s_gameBase + nt->OptionalHeader.SizeOfImage;
			return true;
		}

		bool IsReadable(const uintptr_t address, const size_t size)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
				info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			{
				return false;
			}
			const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
			return address <= regionEnd && size <= regionEnd - address;
		}

		bool ReadBytes(const uintptr_t address, uint8_t* const out, const size_t size)
		{
			if (!IsReadable(address, size))
			{
				return false;
			}
			std::memcpy(out, reinterpret_cast<const void*>(address), size);
			return true;
		}

		bool ReadPointer(const uintptr_t address, uintptr_t& out)
		{
			uint8_t raw[8];
			if (!ReadBytes(address, raw, sizeof(raw)))
			{
				return false;
			}
			std::memcpy(&out, raw, sizeof(out));
			return true;
		}

		// test11: who makes the read the game acts on. Decodes the game's GetControllerState call site and, if a
		// mod has taken it over, follows it through any jump stubs to the function that does the reading, whose
		// code range then counts as the game's (GameGripRules.h). Re-run whenever the site's bytes change.
		void ResolveGameReadProxy()
		{
			const uintptr_t site = s_gameBase + GameGrip::kPollCallSiteRva;
			uint8_t bytes[6];
			if (!ReadBytes(site, bytes, sizeof(bytes)))
			{
				return;
			}
			if (s_haveSiteSeen && std::memcmp(bytes, s_siteSeen, sizeof(bytes)) == 0)
			{
				return; // unchanged since the last look
			}
			std::memcpy(s_siteSeen, bytes, sizeof(bytes));
			s_haveSiteSeen = true;

			const GameGrip::Site decoded = GameGrip::DecodeCallSite(bytes, site);
			uintptr_t destination = 0;
			if (decoded.kind == GameGrip::SiteKind::kOriginal)
			{
				s_proxyBegin.store(0, std::memory_order_relaxed);
				s_proxyEnd.store(0, std::memory_order_relaxed);
				LOG_INFO(
					"Game grip guard: the game reads its own controller (SkyrimVR.exe+0x%X is the game's own call) - "
					"reads from SkyrimVR.exe are the game's.",
					static_cast<unsigned>(GameGrip::kPollCallSiteRva));
				return;
			}
			if (decoded.kind == GameGrip::SiteKind::kIndirectCall)
			{
				if (!ReadPointer(decoded.slot, destination))
				{
					destination = 0;
				}
			}
			else if (decoded.kind == GameGrip::SiteKind::kRelativeCall)
			{
				destination = decoded.target;
			}
			for (int hop = 0; destination != 0 && hop < 4; ++hop)
			{
				uint8_t head[6];
				if (!ReadBytes(destination, head, sizeof(head)))
				{
					destination = 0;
					break;
				}
				const GameGrip::Stub stub = GameGrip::DecodeJumpStub(head, destination);
				if (!stub.isStub)
				{
					break;
				}
				uintptr_t next = stub.target;
				if (stub.viaSlot && !ReadPointer(stub.slot, next))
				{
					next = 0;
				}
				destination = next;
			}

			DWORD64 imageBase = 0;
			const RUNTIME_FUNCTION* const function =
				destination ? RtlLookupFunctionEntry(static_cast<DWORD64>(destination), &imageBase, nullptr) : nullptr;
			if (!function)
			{
				s_proxyBegin.store(0, std::memory_order_relaxed);
				s_proxyEnd.store(0, std::memory_order_relaxed);
				char owner[64];
				ModuleOf(reinterpret_cast<const void*>(destination), owner, sizeof(owner));
				LOG_ERR(
					"Game grip guard: the game's controller read at SkyrimVR.exe+0x%X has been replaced (%02X %02X %02X %02X "
					"%02X %02X, leading to %p in %s) and the function doing the read could not be found - the game's reads "
					"will not be recognised, and a left-hand weapon can still be put away after a pickup.",
					static_cast<unsigned>(GameGrip::kPollCallSiteRva), bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
					bytes[5], reinterpret_cast<void*>(destination), owner);
				return;
			}

			const uintptr_t begin = static_cast<uintptr_t>(imageBase) + function->BeginAddress;
			const uintptr_t end = static_cast<uintptr_t>(imageBase) + function->EndAddress;
			s_proxyBegin.store(begin, std::memory_order_relaxed);
			s_proxyEnd.store(end, std::memory_order_relaxed);
			char owner[64];
			ModuleOf(reinterpret_cast<const void*>(destination), owner, sizeof(owner));
			LOG_INFO(
				"Game grip guard: the game's controller read at SkyrimVR.exe+0x%X is made by %s for it (%s, its function at "
				"+0x%llX-0x%llX) - the reads made from there are the game's, and are handed the real grip during a blink.",
				static_cast<unsigned>(GameGrip::kPollCallSiteRva), owner,
				decoded.kind == GameGrip::SiteKind::kIndirectCall ? "an indirect call through a trampoline slot"
																   : "a relative call",
				static_cast<unsigned long long>(begin - static_cast<uintptr_t>(imageBase)),
				static_cast<unsigned long long>(end - static_cast<uintptr_t>(imageBase)));
		}

		void GiveUp(const char* why)
		{
			s_gaveUp = true;
			LOG_ERR(
				"Game grip guard: %s - NOT installed. The game sees the Weapon Throw VR blink as in 2.2.0, "
				"so on Oculus controllers a LEFT-hand weapon can be put away right after a Grip Hold pickup.",
				why);
		}

		void TryInstall()
		{
			if (s_installed || s_gaveUp)
			{
				return;
			}

			BSOpenVR* const openVR = g_openVR ? *g_openVR : nullptr;
			if (!openVR || !openVR->vrSystem)
			{
				return; // not up yet - the next step asks again
			}
			void** const vtable = *reinterpret_cast<void***>(openVR->vrSystem);
			if (!vtable)
			{
				GiveUp("the VR system has no vtable");
				return;
			}

			void* const current = vtable[kSlotGetControllerState];
			char currentModule[64];
			char proxyModule[64];
			ModuleOf(current, currentModule, sizeof(currentModule));
			ModuleOf(vtable[kSlotGetTrackedDeviceIndexForControllerRole], proxyModule, sizeof(proxyModule));

			const bool toolsLoaded = GetModuleHandleA("skyrimvrtools.dll") != nullptr;
			const bool ownedByTools = _stricmp(currentModule, "skyrimvrtools.dll") == 0;
			const bool ownedByProxyOrOpenVR = std::strcmp(currentModule, proxyModule) == 0 ||
				_stricmp(currentModule, "vrclient_x64.dll") == 0 || _stricmp(currentModule, "openvr_api.dll") == 0;

			if (toolsLoaded && !ownedByTools)
			{
				// SkyrimVRTools is loaded but not in the slot yet. Arming now would put this hook INSIDE it,
				// where Weapon Throw VR would be handed the real grip - so wait for it.
				if (++s_armingSteps < kArmingStepsMax)
				{
					return;
				}
				char why[256];
				sprintf_s(why, "SkyrimVRTools is loaded but the controller-state slot still belongs to %s after half a "
					"minute, so this mod cannot tell where Weapon Throw VR reads", currentModule);
				GiveUp(why);
				return;
			}
			if (!toolsLoaded && !ownedByProxyOrOpenVR)
			{
				char why[256];
				sprintf_s(why, "the controller-state slot belongs to %s, not a layout this mod recognises", currentModule);
				GiveUp(why);
				return;
			}
			if (!ReadGameImage())
			{
				GiveUp("SkyrimVR.exe's own image could not be read, so the game's reads cannot be told apart");
				return;
			}

			s_original = reinterpret_cast<GetStateFn>(current);
			SafeWrite64(
				reinterpret_cast<uintptr_t>(&vtable[kSlotGetControllerState]),
				reinterpret_cast<UInt64>(&Hooked_GetControllerState));
			if (vtable[kSlotGetControllerState] != reinterpret_cast<void*>(&Hooked_GetControllerState))
			{
				s_original = nullptr;
				GiveUp("the vtable write did not land");
				return;
			}

			s_vtable = vtable;
			s_installed = true;
			s_on.store(true, std::memory_order_relaxed);
			LOG_INFO(
				"Game grip guard: hooked GetControllerState - slot %d of the IVRSystem_019 vtable at %p, taken from %s at "
				"%p%s. From here on, while Grip Hold shows other mods a grip released after a pickup, SkyrimVR.exe itself "
				"(%p-%p) is handed the real grip - it cannot read the re-press as Ready Weapon and put a left-hand weapon "
				"away - and every mod still sees the blink. Each blink ends with a line saying how many of the game's "
				"reads it reached and how many were handed back.",
				kSlotGetControllerState,
				vtable,
				currentModule,
				current,
				ownedByTools ? " (SkyrimVRTools, the layer Weapon Throw VR reads in, now runs inside this hook)" : "",
				reinterpret_cast<void*>(s_gameBase),
				reinterpret_cast<void*>(s_gameEnd));
			ResolveGameReadProxy();
		}

		// One line per blink, once its grace is over: the proof, or the reason there is none.
		void ReportBlinks()
		{
			const long long now = NowTicks();
			for (int hand = 0; hand < 2; ++hand)
			{
				long long start = 0;
				long long end = 0;
				if (!MenuGripBlinkWindow(hand == 1, start, end))
				{
					continue;
				}
				BlinkReport& report = s_report[hand];
				if (start != report.start)
				{
					report.start = start;
					report.pending = true;
					report.reads0 = s_blinkReads[hand].load(std::memory_order_relaxed);
					report.reached0 = s_blinkReached[hand].load(std::memory_order_relaxed);
					report.handedBack0 = s_blinkHandedBack[hand].load(std::memory_order_relaxed);
					report.kept0 = s_blinkKept[hand].load(std::memory_order_relaxed);
				}
				if (!report.pending || now <= end + kGraceTicks)
				{
					continue;
				}
				report.pending = false;
				const long long reads = s_blinkReads[hand].load(std::memory_order_relaxed) - report.reads0;
				const long long reached = s_blinkReached[hand].load(std::memory_order_relaxed) - report.reached0;
				const long long handedBack = s_blinkHandedBack[hand].load(std::memory_order_relaxed) - report.handedBack0;
				const long long kept = s_blinkKept[hand].load(std::memory_order_relaxed) - report.kept0;
				const char* const side = hand == 1 ? "left" : "right";
				const double seconds = static_cast<double>(end - start) / static_cast<double>(kTicksPerSecond);
				if (kept > 0)
				{
					// test13's proof line.
					LOG_INFO(
						"Game grip guard: %s-hand blink (%.2fs): the game read that controller %lld time(s) and %lld of them "
						"showed it the grip exactly as before the pickup (%s) - kept so until the hand opens, so the pickup "
						"gives the game no press and no Ready Weapon.",
						side, seconds, reads, kept,
						s_keptHeld[hand].load(std::memory_order_relaxed) ? "held" : "not held - HIGGS was hiding it");
					continue;
				}
				switch (GameGrip::Judge(reads, reached, handedBack))
				{
				case GameGrip::Verdict::kNoReads:
					LOG_ERR(
						"Game grip guard: %s-hand blink (%.2fs): the game did not read that controller through the guard "
						"once, so there was nothing it could hand back - the game's reads are not passing through it.",
						side, seconds);
					break;
				case GameGrip::Verdict::kNeverReached:
					LOG_INFO(
						"Game grip guard: %s-hand blink (%.2fs): the game read that controller %lld time(s) and never with "
						"the grip hidden - it saw no release and no re-press, and nothing needed handing back.",
						side, seconds, reads);
					break;
				case GameGrip::Verdict::kAllHandedBack:
					LOG_INFO(
						"Game grip guard: %s-hand blink (%.2fs): the game read that controller %lld time(s); the blink "
						"reached %lld of them and EVERY one was handed the real grip - the game saw the grip held throughout, "
						"so no Ready Weapon press.",
						side, seconds, reads, reached);
					break;
				case GameGrip::Verdict::kSomeMissed:
					LOG_ERR(
						"Game grip guard: %s-hand blink (%.2fs): the game read that controller %lld time(s); the blink "
						"reached %lld of them and only %lld were handed back - the game saw a release on the rest, and a "
						"left-hand weapon may have been put away.",
						side, seconds, reads, reached, handedBack);
					break;
				}
			}
		}

		void OnGameGripGuardStep(void* /*world*/)
		{
			TryInstall();
			if (!s_installed)
			{
				return;
			}

			// Still outermost? Anything that takes the slot after us would put the game's reads behind it.
			if (!s_saidNotOutermost && s_vtable[kSlotGetControllerState] != reinterpret_cast<void*>(&Hooked_GetControllerState))
			{
				s_saidNotOutermost = true;
				char owner[64];
				ModuleOf(s_vtable[kSlotGetControllerState], owner, sizeof(owner));
				LOG_ERR(
					"Game grip guard: the controller-state slot now belongs to %s - something wrapped it after this mod, "
					"so the game may see the blink again (a left-hand weapon put away after a pickup).",
					owner);
			}

			// And still the object the game polls? A replaced VR system would leave the guard out of its path.
			BSOpenVR* const openVR = g_openVR ? *g_openVR : nullptr;
			if (!s_saidObjectChanged && openVR && openVR->vrSystem &&
				*reinterpret_cast<void***>(openVR->vrSystem) != s_vtable)
			{
				s_saidObjectChanged = true;
				LOG_ERR(
					"Game grip guard: the VR system the game polls has a different vtable now (%p, the guard is in %p) - "
					"the game's reads no longer pass through the guard.",
					*reinterpret_cast<void***>(openVR->vrSystem),
					s_vtable);
			}

			// test13: a kept view ended - the hand opened.
			for (int hand = 0; hand < 2; ++hand)
			{
				const long long ends = s_keptEnds[hand].load(std::memory_order_relaxed);
				if (ends != s_keptEndsSaid[hand])
				{
					s_keptEndsSaid[hand] = ends;
					LOG_INFO(
						"Game grip guard: the %s hand opened - for %.2fs after the pickup the game was shown that grip as "
						"before it (%s); it sees the grip as it really is again.",
						hand == 1 ? "left" : "right",
						static_cast<double>(s_keptLastTicks[hand].load(std::memory_order_relaxed)) /
							static_cast<double>(kTicksPerSecond),
						s_keptHeld[hand].load(std::memory_order_relaxed) ? "held" : "not held");
				}
			}

			// A mod that takes over the game's read after this went in is picked up here, about once a second.
			static int s_resolveSteps = 0;
			if (++s_resolveSteps >= 90)
			{
				s_resolveSteps = 0;
				ResolveGameReadProxy();
			}

			if (!s_saidFirstRead)
			{
				const uintptr_t first = s_firstGameCaller.load(std::memory_order_relaxed);
				if (first != 0)
				{
					s_saidFirstRead = true;
					char owner[64];
					ModuleOf(reinterpret_cast<const void*>(first), owner, sizeof(owner));
					HMODULE module = nullptr;
					GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCSTR>(first), &module);
					LOG_INFO(
						"Game grip guard: the game's controller reads pass through the guard (the first returned into "
						"%s+0x%llX).",
						owner,
						static_cast<unsigned long long>(first - reinterpret_cast<uintptr_t>(module)));
				}
			}
			if (!s_saidNotOk)
			{
				const long long notOk = s_gameReadsNotOk.load(std::memory_order_relaxed);
				if (notOk > 0)
				{
					s_saidNotOk = true;
					LOG_INFO(
						"Game grip guard: the layer under the guard answered false to %lld of the game's %lld reads so far - "
						"the game uses the state whatever the answer, and so does the guard (test9 did not).",
						notOk,
						s_gameReads.load(std::memory_order_relaxed));
				}
			}

			ReportBlinks();
		}
	}

	void RegisterGameGripGuard()
	{
		if (!blinkHiddenFromGame)
		{
			LOG_INFO("Game grip guard: off (bBlinkHiddenFromGame = 0) - the game sees the Weapon Throw VR blink too.");
			return;
		}
		if (!GetModuleHandleA("WeaponThrowVR.dll"))
		{
			LOG_INFO("Game grip guard: Weapon Throw VR is not loaded, so there is no blink to hide from the game - not armed.");
			return;
		}
		if (!higgsInterface)
		{
			LOG_ERR("Game grip guard: HIGGS is not available, so the guard cannot be armed.");
			return;
		}
		higgsInterface->AddPrePhysicsStepCallback(OnGameGripGuardStep);
		LOG_INFO(
			"Game grip guard: armed - it goes in on the first physics step that finds SkyrimVRTools in the "
			"controller-state slot.");
	}

	bool GameGripGuardActive()
	{
		return s_installed && s_on.load(std::memory_order_relaxed);
	}

	void GameGripGuardRecord(const bool isLeftVRController, const GameGrip::Record& record)
	{
		s_record[isLeftVRController ? 1 : 0].store(record, std::memory_order_relaxed);
	}
}
