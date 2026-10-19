#include "HapticCensus.h"

#include "Engine.h"
#include "GripHold.h"
#include "HapticCensusRules.h"
#include "VrikHolsters.h"
#include "VrikPulseRules.h"
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

// WHAT WAS CHECKED BEFORE THIS HOOK WAS WRITTEN (2026-09-17).
//
// - sksevr 2.0.12's Hooks_VR.cpp patches the GAME's import of VR_GetGenericInterface and answers
//   "IVRSystem_019" with a static IVRSystemHooked, which is an IVRSystemProxy, which is a
//   vr_src::IVRSystem - that is vr_1_0_12::IVRSystem. It forwards every call to the real
//   "IVRSystem_020" object. So g_openVR->vrSystem is that proxy, its vtable is laid out exactly as
//   vr_1_0_12::IVRSystem declares, and TriggerHapticPulse is virtual number 36 in both the base
//   (skse64/openvr_1_0_12.h) and the proxy (skse64/InternalVR.h), counted from zero.
// - VRIK.dll has no import from openvr_api.dll and no IVRSystem version string in it, so it reaches
//   OpenVR through the game's pointer - the proxy - like this mod and HIGGS do. Mods that wrap the
//   same import after SKSE (SkyrimVRTools) wrap the proxy and end up calling it too.
// - This is a vtable-slot swap of one aligned pointer, not a code patch: no prologue is overwritten,
//   nothing needs a trampoline, and a caller mid-flight sees either the old function or the new
//   one, both valid. SafeWrite64 makes the read-only page writable for the write.
// - The arming check reads the slot's ORIGINAL value first and requires it to live in the same
//   module as the proxy's GetControllerState (SKSE's) or in an OpenVR module; anything else is a
//   layout this mod does not recognise and the hook is not installed. After the write the slot is
//   read back, and this mod's own first pulse - flagged just before it is sent - has to pass
//   through the hook, or the log says the hook is on the wrong slot.
// - The hook body only records: an atomic flag exchange, a mutex-guarded push into a fixed ring, and
//   the forward. No engine calls, no logging, no controller reads - it runs on whatever thread sent
//   the pulse. The physics step does the reading.
// - The hand is resolved on the physics step from the device index, through the same
//   GetTrackedDeviceIndexForControllerRole calls PulseController already makes there. Hand position
//   is read then too, so a logged position is at most one physics step (~11 ms) after the pulse.

namespace SwapDropAndHoldRedux
{
	namespace
	{
		constexpr int kSlotGetControllerState = 34;
		constexpr int kSlotTriggerHapticPulse = 36;

		using PulseFn = void (*)(vr_1_0_12::IVRSystem*, vr_1_0_12::TrackedDeviceIndex_t, uint32_t, unsigned short);

		PulseFn s_original = nullptr;
		bool s_installed = false;
		bool s_gaveUp = false;

		std::atomic<bool> s_ownPulse{ false };
		std::mutex s_lock;
		HapticCensus::Ring s_ring;

		HapticCensus::Summary s_summary;
		VrikPulse::RunDetector s_detector; // VRIK's holster buzz, out of everything else
		int s_logged = 0;
		int s_ownSent = 0;
		int s_ownSeen = 0;
		bool s_saidCap = false;
		bool s_saidHookConfirmed = false;
		bool s_saidHookMissing = false;

		long long NowTicks()
		{
			return std::chrono::steady_clock::now().time_since_epoch().count();
		}

		void Hooked_TriggerHapticPulse(
			vr_1_0_12::IVRSystem* self,
			vr_1_0_12::TrackedDeviceIndex_t device,
			uint32_t axis,
			unsigned short micros)
		{
			const bool ours = s_ownPulse.exchange(false, std::memory_order_acq_rel);
			{
				std::lock_guard<std::mutex> lock(s_lock);
				HapticCensus::Pulse pulse;
				pulse.device = device;
				pulse.axis = axis;
				pulse.micros = micros;
				pulse.ticks = NowTicks();
				pulse.source = ours ? HapticCensus::Source::kOurs : HapticCensus::Source::kOther;
				pulse.caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
				s_ring.Push(pulse);
			}
			if (s_original)
			{
				s_original(self, device, axis, micros);
			}
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
				s_gaveUp = true;
				LOG_ERR("Haptic census: the VR system has no vtable - the haptic hook is NOT installed.");
				return;
			}

			void* const original = vtable[kSlotTriggerHapticPulse];
			void* const controllerState = vtable[kSlotGetControllerState];
			char originalModule[64];
			char stateModule[64];
			ModuleOf(original, originalModule, sizeof(originalModule));
			ModuleOf(controllerState, stateModule, sizeof(stateModule));

			const bool sameModuleAsProxy = std::strcmp(originalModule, stateModule) == 0;
			const bool inOpenVR = _stricmp(originalModule, "vrclient_x64.dll") == 0 ||
				_stricmp(originalModule, "openvr_api.dll") == 0;
			if (!original || (!sameModuleAsProxy && !inOpenVR))
			{
				s_gaveUp = true;
				LOG_ERR(
					"Haptic census: slot %d of the VR system's vtable (%p) points into %s and GetControllerState's "
					"into %s - not a layout this mod recognises, so the haptic hook is NOT installed. Nothing "
					"else changes.",
					kSlotTriggerHapticPulse,
					original,
					originalModule,
					stateModule);
				return;
			}

			s_original = reinterpret_cast<PulseFn>(original);
			SafeWrite64(
				reinterpret_cast<uintptr_t>(&vtable[kSlotTriggerHapticPulse]),
				reinterpret_cast<UInt64>(&Hooked_TriggerHapticPulse));

			if (vtable[kSlotTriggerHapticPulse] != reinterpret_cast<void*>(&Hooked_TriggerHapticPulse))
			{
				s_original = nullptr;
				s_gaveUp = true;
				LOG_ERR("Haptic census: the vtable write did not land - the haptic hook is NOT installed.");
				return;
			}

			s_installed = true;
			LOG_INFO(
				"Haptic census: hooked TriggerHapticPulse - slot %d of the IVRSystem_019 vtable at %p. The original "
				"lives in %s at %p (GetControllerState's in %s). Every haptic pulse any mod sends is logged from "
				"here on, %d per session with summaries every %d. This mod's own pulses are flagged, and its "
				"first one proves the hook is on the right slot.",
				kSlotTriggerHapticPulse,
				vtable,
				originalModule,
				original,
				stateModule,
				HapticCensus::kMaxLoggedPerSession,
				HapticCensus::kSummaryEvery);
		}

		const char* SourceName(const HapticCensus::Source source)
		{
			return source == HapticCensus::Source::kOurs ? "OURS (this mod's holster buzz)" : "another mod";
		}

		// test12: the same, naming the module that sent it (from the return address the hook saw).
		const char* SenderName(const HapticCensus::Pulse& pulse, char* const out, const size_t size)
		{
			if (pulse.source == HapticCensus::Source::kOurs || pulse.caller == 0)
			{
				strncpy_s(out, size, SourceName(pulse.source), _TRUNCATE);
				return out;
			}
			char module[64];
			ModuleOf(reinterpret_cast<const void*>(pulse.caller), module, sizeof(module));
			sprintf_s(out, size, "another mod (%s)", module);
			return out;
		}

		const char* HandName(const int hand)
		{
			return hand == 1 ? "left" : (hand == 0 ? "right" : "not a controller");
		}

		void LogSummary()
		{
			LOG_INFO(
				"Haptic census summary: %d pulse(s) so far - %d of this mod's own sent, %d of them seen by the hook. "
				"Rows are hand / duration / source / count:",
				s_summary.Total(),
				s_ownSent,
				s_ownSeen);
			for (int i = 0; i < s_summary.RowCount(); ++i)
			{
				const HapticCensus::Row* const row = s_summary.RowAt(i);
				LOG_INFO(
					"Haptic census summary:   %-16s %5u us   %-32s %d",
					HandName(row->hand),
					static_cast<unsigned int>(row->micros),
					SourceName(row->source),
					row->count);
			}
			if (s_summary.Overflow() > 0)
			{
				LOG_INFO(
					"Haptic census summary:   ...and %d pulse(s) in combinations beyond the %d rows kept.",
					s_summary.Overflow(),
					HapticCensus::kSummaryRows);
			}
		}

		void OnHapticCensusStep(void* /*world*/)
		{
			TryInstall();
			if (!s_installed)
			{
				return;
			}

			// The hook is proven by this mod's own pulses. If they keep being sent and none is ever seen,
			// the slot is wrong and every other line here is meaningless.
			if (!s_saidHookMissing && !s_saidHookConfirmed && s_ownSent >= 3 && s_ownSeen == 0)
			{
				s_saidHookMissing = true;
				LOG_ERR(
					"Haptic census: this mod has sent %d holster buzzes and the hook saw none of them - the hook "
					"is NOT on the haptic slot. Treat every census line as unreliable.",
					s_ownSent);
			}

			HapticCensus::Pulse batch[HapticCensus::kRingCapacity];
			int count = 0;
			int dropped = 0;
			{
				std::lock_guard<std::mutex> lock(s_lock);
				count = s_ring.Drain(batch, HapticCensus::kRingCapacity);
				dropped = s_ring.TakeDropped();
			}
			if (dropped > 0)
			{
				LOG_INFO("Haptic census: %d pulse(s) arrived faster than a physics step and were dropped unread.", dropped);
			}
			if (count == 0)
			{
				return;
			}

			BSOpenVR* const openVR = g_openVR ? *g_openVR : nullptr;
			vr_1_0_12::IVRSystem* const vrSystem = openVR ? openVR->vrSystem : nullptr;
			const unsigned int leftDevice = vrSystem
				? vrSystem->GetTrackedDeviceIndexForControllerRole(vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand)
				: HapticCensus::kInvalidDevice;
			const unsigned int rightDevice = vrSystem
				? vrSystem->GetTrackedDeviceIndexForControllerRole(vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand)
				: HapticCensus::kInvalidDevice;
			const long long now = NowTicks();
			const double ticksPerMs =
				static_cast<double>(std::chrono::steady_clock::period::den) / std::chrono::steady_clock::period::num / 1000.0;

			for (int i = 0; i < count; ++i)
			{
				const HapticCensus::Pulse& pulse = batch[i];
				const int hand = HapticCensus::HandFromDevice(pulse.device, leftDevice, rightDevice);
				s_summary.Add(hand, pulse.micros, pulse.source);

				// VRIK's holster buzz: a run of same-duration pulses one frame apart, not ours. It says
				// this hand is at a slot that will take its weapon - the answer Grip hold stands aside on.
				{
					static const double kTicksToNs =
						1.0e9 * std::chrono::steady_clock::period::num / std::chrono::steady_clock::period::den;
					const long long ticksNs = static_cast<long long>(static_cast<double>(pulse.ticks) * kTicksToNs);
					if (s_detector.Feed(hand, pulse.source == HapticCensus::Source::kOurs, pulse.micros, ticksNs))
					{
						LOG_INFO(
							"VRIK holster buzz: the %s hand - a run of %d one-frame pulses of %u us, the pattern VRIK's "
							"holster haptics make when a slot will take the weapon.",
							HandName(hand),
							VrikPulse::kMinRun,
							static_cast<unsigned int>(pulse.micros));
						GripHoldOnVrikHolsterBuzz(hand == 1);
					}
				}

				if (pulse.source == HapticCensus::Source::kOurs)
				{
					++s_ownSeen;
					if (!s_saidHookConfirmed)
					{
						s_saidHookConfirmed = true;
						LOG_INFO("Haptic census: this mod's own holster buzz passed through the hook - the hook is confirmed on the haptic slot.");
					}
				}

				if (HapticCensus::ShouldLog(s_logged))
				{
					++s_logged;
					char sender[96];
					SenderName(pulse, sender, sizeof(sender));
					const double ageMs = static_cast<double>(now - pulse.ticks) / ticksPerMs;
					if (hand < 0)
					{
						LOG_INFO(
							"Haptic census: device %u (not a controller), %u us on axis %u, from %s, read %.0f ms after it was sent.",
							pulse.device,
							static_cast<unsigned int>(pulse.micros),
							pulse.axis,
							sender,
							ageMs);
						continue;
					}

					const bool isLeftVRController = hand == 1;
					char doing[160];
					GripHoldDescribeHand(isLeftVRController, doing, sizeof(doing));

					int firstSlot = 0;
					int bodySlot = 0;
					float firstDistance = 0.0f;
					float bodyDistance = 0.0f;
					const bool measured = HolsterNearestRawSlotOnBothBodies(
						isLeftVRController, firstSlot, firstDistance, bodySlot, bodyDistance);
					if (measured)
					{
						LOG_INFO(
							"Haptic census: %s hand, %u us, from %s, read %.0f ms after it was sent - %s. Nearest slot by "
							"VRIK's own positions: %s at %.1f on the first-person skeleton, %s at %.1f on the third-person "
							"body.",
							HandName(hand),
							static_cast<unsigned int>(pulse.micros),
							sender,
							ageMs,
							doing,
							HolsterSlotName(firstSlot),
							firstDistance,
							HolsterSlotName(bodySlot),
							bodyDistance);
					}
					else
					{
						LOG_INFO(
							"Haptic census: %s hand, %u us, from %s, read %.0f ms after it was sent - %s. (No slot distances: "
							"the holster settings or the hand position were not available.)",
							HandName(hand),
							static_cast<unsigned int>(pulse.micros),
							sender,
							ageMs,
							doing);
					}
				}
				else if (!s_saidCap)
				{
					s_saidCap = true;
					LOG_INFO(
						"Haptic census: %d pulses logged - the per-pulse lines stop here; summaries continue every %d.",
						HapticCensus::kMaxLoggedPerSession,
						HapticCensus::kSummaryEvery);
				}

				if (HapticCensus::ShouldSummarize(s_summary.Total()))
				{
					LogSummary();
				}
			}
		}
	}

	void RegisterHapticCensus()
	{
		if (!higgsInterface)
		{
			LOG_ERR("Haptic census: HIGGS is not available, so haptic pulses will not be watched.");
			return;
		}
		higgsInterface->AddPrePhysicsStepCallback(OnHapticCensusStep);
		LOG_INFO("Haptic census: armed - the haptic hook goes in on the first physics step that finds the VR system.");
	}

	void HapticCensusNoteOwnPulse()
	{
		++s_ownSent;
		s_ownPulse.store(true, std::memory_order_release);
	}

	void HapticCensusOwnPulseDone()
	{
		// If the hook consumed the flag this is a no-op; if the hook is not installed yet, the flag must
		// not linger and claim the next foreign pulse as ours.
		s_ownPulse.store(false, std::memory_order_release);
	}
}
