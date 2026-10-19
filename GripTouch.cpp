#include "GripTouch.h"

#include "GripTouchRules.h"
#include "config.h"

#include <skse64/GameVR.h>

#include <chrono>
#include <cstring>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct KnownController
		{
			vr_1_0_12::TrackedDeviceIndex_t device = vr_1_0_12::k_unTrackedDeviceIndexInvalid;
			char type[64] = "";
			int setting = GripTouch::kAuto;
			bool decided = false;
			bool countsTouch = true;
			bool saidUnknown = false;
			Clock::time_point nextAsk{};
		};

		KnownController s_known[2];

		const char* HandName(const bool isLeftVRController)
		{
			return isLeftVRController ? "left" : "right";
		}

		void Announce(const bool isLeftVRController, const KnownController& known)
		{
			const int setting = GripTouch::Setting(known.setting);
			if (setting != GripTouch::kAuto)
			{
				LOG_INFO(
					"Controllers: the %s controller is '%s'. iDoubleTapCountsTouch = %d in the ini, so Simple "
					"mode's double tap counts %s on it.",
					HandName(isLeftVRController),
					known.type,
					setting,
					known.countsTouch ? "a grip press or touch" : "grip PRESSES only");
				return;
			}

			if (!known.countsTouch)
			{
				LOG_INFO(
					"Controllers: the %s controller is '%s' (Valve Index). Its grip touch sensor stays on while "
					"fingers rest on the grip, so Simple mode's double tap counts grip PRESSES only on it - "
					"squeeze twice. Holding still reads touch. (iDoubleTapCountsTouch = -1, auto)",
					HandName(isLeftVRController),
					known.type);
				return;
			}

			LOG_INFO(
				"Controllers: the %s controller is '%s'. Simple mode's double tap counts a grip press or touch "
				"on it, as before. (iDoubleTapCountsTouch = -1, auto)",
				HandName(isLeftVRController),
				known.type);
		}
	}

	bool GripTouchTapsCountTouch(const bool isLeftVRController)
	{
		KnownController& known = s_known[isLeftVRController ? 1 : 0];

		BSOpenVR* const openVR = g_openVR ? *g_openVR : nullptr;
		vr_1_0_12::IVRSystem* const vrSystem = openVR ? openVR->vrSystem : nullptr;
		if (!vrSystem)
		{
			return GripTouch::TapsCountTouch(doubleTapCountsTouch, known.type);
		}

		const vr_1_0_12::TrackedDeviceIndex_t device = vrSystem->GetTrackedDeviceIndexForControllerRole(
			isLeftVRController
				? vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand
				: vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);

		// A different device behind this hand, or a different ini setting: decide again.
		if (device != known.device || doubleTapCountsTouch != known.setting)
		{
			const bool sameDevice = device == known.device;
			known.device = device;
			known.setting = doubleTapCountsTouch;
			known.decided = false;
			known.nextAsk = Clock::time_point{};
			if (!sameDevice)
			{
				known.type[0] = 0;
				known.saidUnknown = false;
			}
		}

		if (known.decided)
		{
			return known.countsTouch;
		}

		// Not decided yet: the controller may be asleep or not paired. Ask once a second, and until it
		// answers give it the reading everyone has always had.
		const Clock::time_point now = Clock::now();
		if (now < known.nextAsk)
		{
			return GripTouch::TapsCountTouch(doubleTapCountsTouch, known.type);
		}
		known.nextAsk = now + std::chrono::seconds(1);

		if (device == vr_1_0_12::k_unTrackedDeviceIndexInvalid)
		{
			return GripTouch::TapsCountTouch(doubleTapCountsTouch, known.type);
		}

		char type[sizeof(known.type)] = "";
		vr_1_0_12::ETrackedPropertyError error = vr_1_0_12::TrackedProp_Success;
		vrSystem->GetStringTrackedDeviceProperty(
			device,
			vr_1_0_12::Prop_ControllerType_String,
			type,
			sizeof(type),
			&error);
		type[sizeof(type) - 1] = 0;

		if (error != vr_1_0_12::TrackedProp_Success || type[0] == 0)
		{
			if (!known.saidUnknown)
			{
				known.saidUnknown = true;
				LOG_INFO(
					"Controllers: the %s controller (device %u) has not said what it is (property error %d). "
					"Simple mode's double tap counts a grip press or touch until it does.",
					HandName(isLeftVRController),
					static_cast<unsigned>(device),
					static_cast<int>(error));
			}
			return GripTouch::TapsCountTouch(doubleTapCountsTouch, "");
		}

		strcpy_s(known.type, type);
		known.countsTouch = GripTouch::TapsCountTouch(doubleTapCountsTouch, known.type);
		known.decided = true;
		Announce(isLeftVRController, known);
		return known.countsTouch;
	}
}
