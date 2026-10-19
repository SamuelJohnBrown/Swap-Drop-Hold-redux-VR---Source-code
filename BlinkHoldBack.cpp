#include "BlinkHoldBack.h"

#include "Engine.h"
#include "GripBlinkRules.h"
#include "MenuGrip.h"
#include "SurfaceProbe.h"
#include "config.h"

namespace SwapDropAndHoldRedux
{
	namespace
	{
		constexpr long long kTicksPerSecond = 1000000000LL;

		// One per VR controller, for the blink it last saw: what has been said about it, so each thing
		// is logged once per pickup.
		struct HandState
		{
			long long blinkStart = 0;
			bool heldBack = false;
			bool saidClear = false;
			bool gaveUp = false;
			bool saidNoProbe = false;
		};
		HandState s_hands[2];

		const char* SurfaceName(const int layer)
		{
			switch (layer)
			{
			case 1: return "a static surface (a wall, a table, most of the world)";
			case 2: return "an animated static";
			case 9: return "a tree";
			case 10: return "a prop";
			case 13: return "terrain";
			case 17: return "the ground";
			default: return "a climbable surface";
			}
		}

		void OnBlinkWorldStep(void* world)
		{
			SurfaceProbeNoteWorld(world);
		}

		// Every frame, on the frame thread. Does nothing unless a blink is showing a grip released.
		void OnBlinkHoldBackFrame()
		{
			for (int i = 0; i < 2; ++i)
			{
				const bool isLeft = i == 1;
				const char* handName = isLeft ? "left" : "right";
				HandState& state = s_hands[i];

				long long start = 0;
				long long end = 0;
				if (!MenuGripBlinkWindow(isLeft, start, end))
				{
					state = HandState{};
					continue;
				}
				if (state.blinkStart != start)
				{
					state = HandState{};
					state.blinkStart = start;
				}

				const long long now = MenuGripNowTicks();
				if (!GripBlink::Active(now, end))
				{
					continue;
				}

				GripBlink::HoldBackFacts facts;
				facts.enabled = blinkWaitsForClearHand;
				facts.showing = true;
				facts.gripClosed = MenuGripRealGripWhileShowing(isLeft);
				GripBlink::ProbeRay rays[6];
				facts.probeRan = facts.enabled && facts.gripClosed &&
					SurfaceProbeHand(isLeft, GripBlink::kHoldBackReachUnits, rays);
				int layer = -1;
				float distUnits = 0.0f;
				facts.nearClimbable = facts.probeRan &&
					GripBlink::NearClimbable(rays, 6, GripBlink::kHoldBackReachUnits, layer, distUnits);
				facts.secondsSinceBlink = static_cast<float>(static_cast<double>(now - start) / static_cast<double>(kTicksPerSecond));

				switch (GripBlink::DecideHoldBack(facts))
				{
				case GripBlink::HoldBack::kExtend:
					MenuGripHoldBlinkUntil(isLeft, GripBlink::HoldBackEndTicks(now, end, kTicksPerSecond));
					if (!state.heldBack)
					{
						state.heldBack = true;
						LOG_INFO(
							"Grip blink: the %s hand is %.1f units from %s - VR Climbing would grab it if the grip "
							"came back now, so other mods keep seeing that grip released until the hand is clear.",
							handName,
							distUnits,
							SurfaceName(layer));
					}
					break;
				case GripBlink::HoldBack::kGiveUp:
					if (!state.gaveUp)
					{
						state.gaveUp = true;
						LOG_INFO(
							"Grip blink: the %s hand stayed within reach of %s for %.1fs - other mods are shown its "
							"grip pressed again anyway (the limit is %.0fs).",
							handName,
							SurfaceName(layer),
							facts.secondsSinceBlink,
							GripBlink::kHoldBackMaxSeconds);
					}
					break;
				default:
					if (state.heldBack && !state.saidClear && !state.gaveUp && facts.probeRan)
					{
						state.saidClear = true;
						LOG_INFO(
							"Grip blink: the %s hand is clear of anything VR Climbing climbs %.2fs after the grip was "
							"first shown released - other mods see it pressed again.",
							handName,
							facts.secondsSinceBlink);
					}
					if (facts.enabled && facts.gripClosed && !facts.probeRan && !state.saidNoProbe)
					{
						state.saidNoProbe = true;
						LOG_INFO(
							"Grip blink: the %s hand's surface check could not run (no fresh physics world, no "
							"controller node, or a loading screen) - its grip comes back after the usual %.2fs.",
							handName,
							GripBlink::kSeconds);
					}
					break;
				}
			}
		}
	}

	void RegisterBlinkHoldBack()
	{
		if (!higgsInterface)
		{
			return;
		}
		higgsInterface->AddPrePhysicsStepCallback(OnBlinkWorldStep);
		higgsInterface->AddPostVrikPostHiggsCallback(OnBlinkHoldBackFrame);
		LOG_INFO(
			"Grip blink: surface check registered with HIGGS - whether the re-press waits for a clear hand is "
			"reported when the ini is read (bBlinkWaitsForClearHand).");
	}
}
