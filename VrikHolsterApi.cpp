#include "VrikHolsterApi.h"

#include "Engine.h"
#include "TrackedWeapons.h"
#include "VrikHolsterRules.h"
#include "VrikHolsters.h"
#include "config.h"

#include <skse64/GameForms.h>
#include <skse64/PapyrusEvents.h>

#include <mutex>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		bool s_available = false;

		// VRIK's holster events as the listener recorded them. The listener can be called on a Papyrus
		// thread (SKSE flags SendModEvent NoWait), so it only parses and records; the physics step logs
		// and matches them.
		constexpr int kMaxRecordedEvents = 8;
		std::mutex s_eventLock;
		VrikHolster::Event s_recorded[kMaxRecordedEvents];
		int s_recordedCount = 0;

		// This mod's last HolsterWeapon call, so VRIK's event for it can be checked. Only the physics
		// step and GripHold's update touch this, and both run on the same thread.
		struct Request
		{
			UInt32 formID = 0;
			bool secondaryHand = false;
			float secondsSinceCall = 0.0f;
			bool completed = false;
			float secondsSinceCompleted = 0.0f;
		};
		Request s_requests[2]; // [0] = VRIK's primary hand, [1] = its secondary hand
		bool s_warnedMissingEvent = false;
		bool s_warnedWrongHand = false;

		class HolsterEventSink : public BSTEventSink<SKSEModCallbackEvent>
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

				VrikHolster::Event parsed;
				if (!VrikHolster::ParseEvent(evn->eventName.data, evn->strArg.data, evn->numArg, parsed))
				{
					return kEvent_Continue;
				}

				std::lock_guard<std::mutex> lock(s_eventLock);
				if (s_recordedCount == kMaxRecordedEvents)
				{
					// Full: the oldest goes.
					for (int i = 1; i < kMaxRecordedEvents; ++i)
					{
						s_recorded[i - 1] = s_recorded[i];
					}
					--s_recordedCount;
				}
				s_recorded[s_recordedCount++] = parsed;
				return kEvent_Continue;
			}
		};

		HolsterEventSink s_sink;

		const char* HandWord(const bool primary)
		{
			return primary ? "Primary" : "Secondary";
		}
	}

	void VrikHolsterApiInit()
	{
		const bool haveInterface = vrikInterface != nullptr;
		const unsigned int build = haveInterface ? vrikInterface->getBuildNumber() : 0;
		s_available = VrikHolster::HasApi(haveInterface, build);

		if (s_available)
		{
			LOG_INFO(
				"VRIK holster API: build %u - available. What the mod does at holsters is logged once the "
				"settings are read.",
				build);
		}
		else if (haveInterface)
		{
			LOG_INFO(
				"VRIK holster API: build %u - not available (needs %u), holsters work as in 2.1.1.",
				build,
				VrikHolster::kFirstApiBuild);
		}
		else
		{
			LOG_INFO("VRIK holster API: VRIK is not present, holsters work as in 2.1.1.");
		}
	}

	bool VrikHolsterApiAvailable()
	{
		return s_available;
	}

	int VrikHolsterSlotInReach(const bool isLeftGameHand)
	{
		if (!s_available || !vrikInterface)
		{
			return 0;
		}

		const int slot = vrikInterface->GetHolsterSlotInReach(VrikHolster::SecondaryHandFor(isLeftGameHand));
		return VrikHolster::IsValidSlot(slot) ? slot : 0;
	}

	bool VrikCanHolster(const bool isLeftGameHand)
	{
		return s_available && vrikInterface &&
			vrikInterface->CanBeHolstered(VrikHolster::SecondaryHandFor(isLeftGameHand));
	}

	bool VrikHolsterWeapon(const bool isLeftGameHand, const UInt32 weaponFormID)
	{
		if (!s_available || !vrikInterface)
		{
			return false;
		}

		const bool secondaryHand = VrikHolster::SecondaryHandFor(isLeftGameHand);
		if (!vrikInterface->HolsterWeapon(secondaryHand))
		{
			return false;
		}

		Request& request = s_requests[secondaryHand ? 1 : 0];
		request = Request{};
		request.formID = weaponFormID;
		request.secondaryHand = secondaryHand;
		return true;
	}

	void VrikHolsterNoteCompleted(const bool isLeftGameHand, const UInt32 weaponFormID)
	{
		Request& request = s_requests[VrikHolster::SecondaryHandFor(isLeftGameHand) ? 1 : 0];
		if (request.formID != 0 && request.formID == weaponFormID)
		{
			request.completed = true;
		}
	}

	void VrikHolsterOnPreLoadGame()
	{
		s_requests[0] = Request{};
		s_requests[1] = Request{};

		std::lock_guard<std::mutex> lock(s_eventLock);
		s_recordedCount = 0;
	}

	void RegisterVrikHolsterEvents(SKSEMessagingInterface* const messaging)
	{
		// SKSE's own dispatcher, never g_modCallbackEventDispatcher: that one is a private copy inside
		// this DLL that nothing fires. RegisterExclusions in Exclusions.cpp has the story.
		if (!messaging)
		{
			LOG_INFO("VRIK holster API: no messaging interface - VRIK's holster events will not be logged.");
			return;
		}

		EventDispatcher<SKSEModCallbackEvent>* const dispatcher =
			static_cast<EventDispatcher<SKSEModCallbackEvent>*>(
				messaging->GetEventDispatcher(SKSEMessagingInterface::kDispatcher_ModEvent));
		if (!dispatcher)
		{
			LOG_INFO("VRIK holster API: SKSE returned no mod event dispatcher - VRIK's holster events will not be logged.");
			return;
		}

		dispatcher->AddEventSink(&s_sink);
		LOG_INFO(
			"VRIK holster API: listening for VRIK_Holstered and VRIK_Unholstered on SKSE's dispatcher %p.",
			dispatcher);
	}

	void VrikHolsterEventsTick(const float deltaTime)
	{
		VrikHolster::Event events[kMaxRecordedEvents];
		int count = 0;
		{
			std::lock_guard<std::mutex> lock(s_eventLock);
			count = s_recordedCount;
			for (int i = 0; i < count; ++i)
			{
				events[i] = s_recorded[i];
			}
			s_recordedCount = 0;
		}

		for (int i = 0; i < count; ++i)
		{
			const VrikHolster::Event& evn = events[i];
			LOG_INFO(
				"VRIK event: %s %s (%s hand) %s slot %d %s.",
				evn.holstered ? "holstered" : "drew",
				GetSafeFormName(LookupFormByID(evn.formID)),
				HandWord(evn.primary),
				evn.holstered ? "into" : "from",
				evn.slot,
				HolsterSlotName(evn.slot));

			int matchedHand = -1;
			for (int hand = 0; hand < 2; ++hand)
			{
				const Request& request = s_requests[hand];
				if (request.formID == 0 || !VrikHolster::EventIsFor(evn, request.formID) ||
					!VrikHolster::EventHandMatches(evn, request.secondaryHand))
				{
					continue;
				}

				matchedHand = hand;
				break;
			}

			if (matchedHand >= 0)
			{
				// Heard: nothing more to check for this request.
				s_requests[matchedHand] = Request{};
				continue;
			}

			for (int hand = 0; hand < 2; ++hand)
			{
				Request& request = s_requests[hand];
				if (request.formID == 0 || !VrikHolster::EventIsFor(evn, request.formID))
				{
					continue;
				}

				if (!s_warnedWrongHand)
				{
					s_warnedWrongHand = true;
					LOG_ERR(
						"VRIK holster API: this mod asked VRIK to holster %s from its %s hand, but VRIK's event "
						"names its %s hand. The mod's left/right mapping for VRIK is wrong - please report this.",
						GetSafeFormName(LookupFormByID(evn.formID)),
						HandWord(!request.secondaryHand),
						HandWord(evn.primary));
				}
				request = Request{};
				break;
			}
		}

		for (int hand = 0; hand < 2; ++hand)
		{
			Request& request = s_requests[hand];
			if (request.formID == 0)
			{
				continue;
			}

			request.secondsSinceCall += deltaTime;
			if (request.completed)
			{
				request.secondsSinceCompleted += deltaTime;
			}

			if (request.completed && request.secondsSinceCompleted >= VrikHolster::kEventGraceSeconds)
			{
				if (!s_warnedMissingEvent)
				{
					s_warnedMissingEvent = true;
					LOG_ERR(
						"VRIK holster API: VRIK holstered %s but never sent VRIK_Holstered. Another mod may be "
						"overwriting VRIK's _vrik_qust_system.pex (Arctal's VRIK Tweaks is known to). Holstering "
						"still works; only this log misses the event.",
						GetSafeFormName(LookupFormByID(request.formID)));
				}
				request = Request{};
			}
			else if (!request.completed &&
				request.secondsSinceCall >= VrikHolster::kWaitSeconds + VrikHolster::kEventGraceSeconds)
			{
				// The holster never completed, and GripHold has already said so. Stop watching.
				request = Request{};
			}
		}
	}
}
