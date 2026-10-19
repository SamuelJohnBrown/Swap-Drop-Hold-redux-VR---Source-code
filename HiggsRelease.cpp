#include "HiggsRelease.h"

#include "Engine.h"
#include "HiggsReleaseRules.h"
#include "Helper.h"
#include "MenuGrip.h"
#include "TrackedWeapons.h"
#include "config.h"

#include <skse64/GameForms.h>
#include <skse64/GameReferences.h>

#include <chrono>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		HiggsRelease::Guard s_guards[2]; // [0] = right VR controller, [1] = left
		const HiggsRelease::Config s_config{};

		// The last reference this mod picked up out of a HIGGS hand, so a later drop can say
		// whether it is that reference again.
		UInt32 s_lastPickupHandle = 0;
		UInt32 s_lastPickupRefID = 0;
		Clock::time_point s_lastPickupTime;

		const char* HandName(const bool isLeftVRController)
		{
			return isLeftVRController ? "left" : "right";
		}

		const char* NameOf(const TESObjectREFR* refr, TESForm* item = nullptr)
		{
			if (item)
			{
				return GetSafeFormName(item);
			}
			return refr && refr->baseForm ? GetSafeFormName(refr->baseForm) : "<no base form>";
		}
	}

	void HiggsReleaseBeforePickup(TESObjectREFR* pickedRef)
	{
		if (!pickedRef || !higgsInterface)
		{
			return;
		}

		// Both hands are asked rather than the one the grab came from, so a mix-up between game
		// hands and controllers cannot leave the hand that really holds it untouched.
		for (int i = 0; i < 2; ++i)
		{
			const bool isLeftVRController = i == 1;
			if (higgsInterface->GetGrabbedObject(isLeftVRController) != pickedRef)
			{
				continue;
			}

			const UInt32 handle = pickedRef->CreateRefHandle();
			s_lastPickupHandle = handle;
			s_lastPickupRefID = pickedRef->formID;
			s_lastPickupTime = Clock::now();

			HiggsRelease::Guard& guard = s_guards[i];
			const bool alreadyOurs = guard.Active();
			if (guard.Begin(true, higgsInterface->IsDisabled(isLeftVRController)))
			{
				higgsInterface->DisableHand(isLeftVRController);
				LOG_INFO(
					"HIGGS release: the %s hand holds %s refId=%08X (handle %08X) - switched that hand off for the pickup, so HIGGS lets go the normal way and clears its selection.",
					HandName(isLeftVRController),
					NameOf(pickedRef),
					pickedRef->formID,
					handle);
			}
			else if (alreadyOurs)
			{
				LOG_INFO(
					"HIGGS release: the %s hand holds %s refId=%08X (handle %08X) and is still switched off from the last pickup - waiting for HIGGS to let go again.",
					HandName(isLeftVRController),
					NameOf(pickedRef),
					pickedRef->formID,
					handle);
			}
			else
			{
				LOG_INFO(
					"HIGGS release: the %s hand holds %s refId=%08X (handle %08X), but something else has switched that hand off - leaving it alone, so HIGGS may keep this selection.",
					HandName(isLeftVRController),
					NameOf(pickedRef),
					pickedRef->formID,
					handle);
			}
		}
	}

	void HiggsReleaseNoteDrop(TESObjectREFR* droppedRef, TESForm* item)
	{
		if (!droppedRef)
		{
			return;
		}

		const UInt32 handle = droppedRef->CreateRefHandle();
		if (s_lastPickupHandle == 0 || handle == *g_invalidRefHandle)
		{
			LOG_INFO(
				"HIGGS release: dropped %s refId=%08X has handle %08X.",
				NameOf(droppedRef, item),
				droppedRef->formID,
				handle);
			return;
		}

		const float since = std::chrono::duration<float>(Clock::now() - s_lastPickupTime).count();
		if (handle == s_lastPickupHandle)
		{
			LOG_INFO(
				"HIGGS release: dropped %s refId=%08X has handle %08X - the SAME reference this mod picked up out of a HIGGS hand %.1fs ago, the case where a hand that kept its selection would not select this weapon again.",
				NameOf(droppedRef, item),
				droppedRef->formID,
				handle,
				since);
		}
		else
		{
			LOG_INFO(
				"HIGGS release: dropped %s refId=%08X has handle %08X - not the reference last picked up out of a HIGGS hand (refId=%08X, handle %08X, %.1fs ago).",
				NameOf(droppedRef, item),
				droppedRef->formID,
				handle,
				s_lastPickupRefID,
				s_lastPickupHandle,
				since);
		}
	}

	void HiggsReleaseTick(const float deltaTime)
	{
		if (!higgsInterface)
		{
			return;
		}

		for (int i = 0; i < 2; ++i)
		{
			HiggsRelease::Guard& guard = s_guards[i];
			if (!guard.Active())
			{
				continue;
			}

			const bool isLeftVRController = i == 1;
			// While Grip Hold shows other mods this grip released after the pickup, the hand stays
			// off, so HIGGS cannot act on the grip coming back (GripBlinkRules.h). test12: and until
			// the game has the weapon put in this hand DRAWN - HIGGS's own test for a weapon hand
			// (IsWeaponDrawn, hand.cpp:4310), so the hand comes back as one and never as an empty hand
			// holding a weapon (HiggsReleaseRules.h).
			PlayerCharacter* const player = *g_thePlayer;
			const bool weaponInThisHand =
				player && GetTrackedWeaponForVRController(player, isLeftVRController).weapon != nullptr;
			const bool weaponDrawn = player && player->actorState.IsWeaponDrawn();
			const HiggsRelease::Step step = guard.Update(
				higgsInterface->IsHoldingObject(isLeftVRController),
				HiggsRelease::WhyKeepOff(MenuGripShowingReleased(isLeftVRController), weaponInThisHand, weaponDrawn),
				deltaTime,
				s_config);
			if (step == HiggsRelease::Step::kEnable && guard.KeptOffForDraw())
			{
				higgsInterface->EnableHand(isLeftVRController);
				LOG_INFO(
					"HIGGS release: the %s hand stayed switched off until the game had the weapon drawn - switched back on %d physics step(s) (%.3fs) after the pickup, straight into a weapon hand (%s).",
					HandName(isLeftVRController),
					guard.Steps(),
					guard.Seconds(),
					weaponDrawn ? "drawn" : "the limit came first");
			}
			else if (step == HiggsRelease::Step::kEnable && guard.KeptOff())
			{
				higgsInterface->EnableHand(isLeftVRController);
				LOG_INFO(
					"HIGGS release: the %s hand let go and stayed switched off while other mods were shown its grip released - switched back on %d physics step(s) (%.3fs) after the pickup.",
					HandName(isLeftVRController),
					guard.Steps(),
					guard.Seconds());
			}
			else if (step == HiggsRelease::Step::kEnable)
			{
				higgsInterface->EnableHand(isLeftVRController);
				LOG_INFO(
					"HIGGS release: the %s hand let go %d physics step(s) (%.3fs) after the pickup - switched back on.",
					HandName(isLeftVRController),
					guard.Steps(),
					guard.Seconds());
			}
			else if (step == HiggsRelease::Step::kEnableLate)
			{
				higgsInterface->EnableHand(isLeftVRController);
				LOG_INFO(
					"HIGGS release: the %s hand still said it held something %d physics step(s) (%.2fs) after the pickup - switched back on anyway, so HIGGS may keep that selection.",
					HandName(isLeftVRController),
					guard.Steps(),
					guard.Seconds());
			}
		}
	}

	bool HiggsReleaseHandIsOff(const bool isLeftVRController)
	{
		return s_guards[isLeftVRController ? 1 : 0].Active();
	}

	void HiggsReleaseOnPreLoadGame()
	{
		for (int i = 0; i < 2; ++i)
		{
			if (s_guards[i].Restore() && higgsInterface)
			{
				const bool isLeftVRController = i == 1;
				higgsInterface->EnableHand(isLeftVRController);
				LOG_INFO(
					"HIGGS release: switched the %s hand back on before the game loads.",
					HandName(isLeftVRController));
			}
		}

		s_lastPickupHandle = 0;
		s_lastPickupRefID = 0;
	}
}
