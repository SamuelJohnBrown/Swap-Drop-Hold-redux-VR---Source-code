#include "HiggsHandWatch.h"

#include "Engine.h"
#include "GripHold.h"
#include "HandWatchRules.h"
#include "Helper.h"
#include "TrackedWeapons.h"
#include "config.h"

#include <skse64/GameForms.h>
#include <skse64/GameReferences.h>

#include <cstdio>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		struct WatchedHand
		{
			HandWatch::Window window;
			char why[96] = {};
		};

		WatchedHand s_hands[2]; // [0] = right VR controller, [1] = left
		const HandWatch::Config s_config{};

		const char* HandName(const bool isLeftVRController)
		{
			return isLeftVRController ? "left" : "right";
		}

		HandWatch::Snapshot ReadHand(const bool isLeftVRController)
		{
			HandWatch::Snapshot s;
			if (higgsInterface)
			{
				s.canGrab = higgsInterface->CanGrabObject(isLeftVRController);
				s.grabbable = higgsInterface->IsHandInGrabbableState(isLeftVRController);
				s.holding = higgsInterface->IsHoldingObject(isLeftVRController);
				s.disabled = higgsInterface->IsDisabled(isLeftVRController);
				s.twoHanding = higgsInterface->IsTwoHanding();
				const TESObjectREFR* held = higgsInterface->GetGrabbedObject(isLeftVRController);
				s.heldRefID = held ? held->formID : 0;
			}
			s.gripClosed = GripHoldIsGripPressed(isLeftVRController);
			s.triggerClosed = GripHoldIsTriggerPressed(isLeftVRController);
			return s;
		}

		// What HIGGS says the hand holds, by name and reference, flagging a reference the game
		// has already deleted - the state a pickup from under HIGGS would leave behind.
		void DescribeHeld(const bool isLeftVRController, char* out, const size_t size)
		{
			const TESObjectREFR* held = higgsInterface ? higgsInterface->GetGrabbedObject(isLeftVRController) : nullptr;
			if (!held)
			{
				std::snprintf(out, size, "nothing");
				return;
			}

			const bool deleted = (held->flags & TESForm::kFlagIsDeleted) != 0;
			std::snprintf(
				out,
				size,
				"%s refId=%08X%s",
				held->baseForm ? GetSafeFormName(held->baseForm) : "<no base form>",
				held->formID,
				deleted ? " (DELETED)" : "");
		}

		// Average finger curl as HIGGS sees it: 1 fully open, 0 fully curled.
		float FingerCurl(const bool isLeftVRController)
		{
			if (!higgsInterface)
			{
				return -1.0f;
			}

			float values[5] = {};
			higgsInterface->GetFingerValues(isLeftVRController, values);
			float sum = 0.0f;
			for (const float value : values)
			{
				sum += value;
			}
			return sum / 5.0f;
		}

		void LogState(const bool isLeftVRController, const char* what, const HandWatch::Snapshot& s)
		{
			const WatchedHand& hand = s_hands[isLeftVRController ? 1 : 0];
			char held[128];
			DescribeHeld(isLeftVRController, held, sizeof(held));
			LOG_INFO(
				"HIGGS hand [%s] %s +%.2fs (%s): canGrab=%s idle=%s holding=%s (%s) disabled=%s twoHanding=%s | grip=%s trigger=%s fingers=%.2f",
				HandName(isLeftVRController),
				what,
				hand.window.Seconds(),
				hand.why,
				s.canGrab ? "yes" : "NO",
				s.grabbable ? "yes" : "NO",
				s.holding ? "YES" : "no",
				held,
				s.disabled ? "YES" : "no",
				s.twoHanding ? "yes" : "no",
				s.gripClosed ? "closed" : "open",
				s.triggerClosed ? "closed" : "open",
				FingerCurl(isLeftVRController));
		}

		void OnReferenceEvent(const char* verb, const bool isLeft, TESObjectREFR* refr)
		{
			const TESForm* base = refr ? refr->baseForm : nullptr;
			LOG_INFO(
				"HIGGS event: the %s hand %s %s refId=%08X.",
				HandName(isLeft),
				verb,
				base ? GetSafeFormName(refr->baseForm) : "<no base form>",
				refr ? refr->formID : 0);

			char why[64];
			std::snprintf(why, sizeof(why), "after HIGGS %s", verb);
			HiggsHandWatchBegin(isLeft, why, refr ? refr->baseForm : nullptr);
		}

		void OnFormEvent(const char* verb, const bool isLeft, TESForm* form)
		{
			LOG_INFO(
				"HIGGS event: the %s hand %s %s formId=%08X.",
				HandName(isLeft),
				verb,
				form ? GetSafeFormName(form) : "<no form>",
				form ? form->formID : 0);

			char why[64];
			std::snprintf(why, sizeof(why), "after HIGGS %s", verb);
			HiggsHandWatchBegin(isLeft, why, form);
		}

		void OnHiggsGrabbed(const bool isLeft, TESObjectREFR* refr)
		{
			OnReferenceEvent("grabbed", isLeft, refr);
		}

		void OnHiggsDropped(const bool isLeft, TESObjectREFR* refr)
		{
			OnReferenceEvent("dropped", isLeft, refr);
		}

		void OnHiggsPulled(const bool isLeft, TESObjectREFR* refr)
		{
			OnReferenceEvent("pulled", isLeft, refr);
		}

		void OnHiggsStashed(const bool isLeft, TESForm* form)
		{
			OnFormEvent("stashed", isLeft, form);
		}

		void OnHiggsConsumed(const bool isLeft, TESForm* form)
		{
			OnFormEvent("consumed", isLeft, form);
		}
	}

	void HiggsHandWatchRegister()
	{
		static bool s_registered = false;
		if (s_registered || !higgsInterface)
		{
			return;
		}
		s_registered = true;

		higgsInterface->AddGrabbedCallback(OnHiggsGrabbed);
		higgsInterface->AddDroppedCallback(OnHiggsDropped);
		higgsInterface->AddPulledCallback(OnHiggsPulled);
		higgsInterface->AddStashedCallback(OnHiggsStashed);
		higgsInterface->AddConsumedCallback(OnHiggsConsumed);
		LOG_INFO(
			"HIGGS hand watch: logging every HIGGS grab, drop, pull, stash and consume, and each hand's HIGGS state on every change for %.0fs after one of those, a drop, or a pickup of what HIGGS held.",
			s_config.windowSeconds);
	}

	void HiggsHandWatchBegin(const bool isLeftVRController, const char* why, TESForm* item)
	{
		WatchedHand& hand = s_hands[isLeftVRController ? 1 : 0];
		std::snprintf(
			hand.why,
			sizeof(hand.why),
			"%s%s%s",
			why ? why : "",
			item ? " " : "",
			item ? GetSafeFormName(item) : "");
		hand.window.Open(s_config);
	}

	void HiggsHandWatchNotePickup(const bool isLeftVRController, TESObjectREFR* pickedRef)
	{
		if (!pickedRef)
		{
			return;
		}

		const TESObjectREFR* heldRef = higgsInterface ? higgsInterface->GetGrabbedObject(isLeftVRController) : nullptr;
		char held[128];
		DescribeHeld(isLeftVRController, held, sizeof(held));
		LOG_INFO(
			"HIGGS hand [%s]: picking up %s refId=%08X into the inventory while HIGGS says that hand holds %s%s.",
			HandName(isLeftVRController),
			pickedRef->baseForm ? GetSafeFormName(pickedRef->baseForm) : "<no base form>",
			pickedRef->formID,
			held,
			heldRef == pickedRef ? " - the same reference" : "");

		HiggsHandWatchBegin(isLeftVRController, "after picking up what HIGGS held:", pickedRef->baseForm);
	}

	void HiggsHandWatchTick(const float deltaTime)
	{
		for (int i = 0; i < 2; ++i)
		{
			WatchedHand& hand = s_hands[i];
			if (!hand.window.IsOpen())
			{
				continue;
			}

			const bool isLeftVRController = i == 1;
			const HandWatch::Snapshot now = ReadHand(isLeftVRController);
			const HandWatch::Output out = hand.window.Update(now, deltaTime, s_config);

			if (out.suppressed > 0)
			{
				LOG_INFO(
					"HIGGS hand [%s]: %d more state change(s) came too fast to log.",
					HandName(isLeftVRController),
					out.suppressed);
			}

			if (out.started)
			{
				LogState(isLeftVRController, "watching", now);
			}
			else if (out.changed)
			{
				LogState(isLeftVRController, "changed", now);
			}

			if (out.ended)
			{
				LogState(isLeftVRController, "watch ended", now);
			}
		}
	}
}
