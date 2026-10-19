#include "WeaponThrowButtons.h"

#include "GripBlinkRules.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameForms.h>

#include <cstdint>
#include <cstring>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		// TESGlobal on SkyrimVR 1.4.15, read from the game's own code: TESGlobal::GetFormEditorID
		// (RVA 0x2F5DA0: mov rax,[rcx+20h]) returns the char* at +0x20, and the loader
		// (RVA 0x2F5A40) reads FLTV into the float at +0x34. The classic SDK calls these unk20/unk34.
		constexpr std::uintptr_t kEditorIdOffset = 0x20;
		constexpr std::uintptr_t kValueOffset = 0x34;

		// Weapon Throw VR's log calls the first "HoldButton" and the second "HoldButtonSecondary". The
		// first is taken as the main (game right) hand and the second as the other. For a left-handed
		// player that pairing is assumed, not read from Weapon Throw VR.
		const char* const kMainName = "WT_HoldButton";
		const char* const kOtherName = "WT_HoldButtonSecondary";

		TESForm* s_main = nullptr;
		TESForm* s_other = nullptr;

		const char* EditorId(TESForm* form)
		{
			const char* id = *reinterpret_cast<const char* const*>(reinterpret_cast<std::uintptr_t>(form) + kEditorIdOffset);
			return id ? id : "";
		}

		float Value(TESForm* form)
		{
			return *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(form) + kValueOffset);
		}
	}

	void WeaponThrowButtonsFind()
	{
		s_main = nullptr;
		s_other = nullptr;

		DataHandler* data = DataHandler::GetSingleton();
		if (!data || !data->arrGLOB.entries)
		{
			LOG_INFO("Weapon Throw VR hold buttons: no global list to look in - Grip Hold's blink behaves as in 2.2.0.");
			return;
		}

		UInt32 looked = 0;
		for (UInt32 i = 0; i < data->arrGLOB.count; ++i)
		{
			TESForm* form = data->arrGLOB.entries[i];
			if (!form || form->formType != kFormType_Global)
			{
				continue;
			}
			++looked;
			const char* id = EditorId(form);
			if (_stricmp(id, kMainName) == 0)
			{
				s_main = form;
			}
			else if (_stricmp(id, kOtherName) == 0)
			{
				s_other = form;
			}
		}

		LOG_INFO(
			"Weapon Throw VR hold buttons: %s %s, %s %s (%u globals looked at). Grip Hold shows a hand's grip "
			"released after a pickup only when that hand's button is Grip (or could not be found); the buttons are "
			"read again at every pickup because they live in the save.",
			kMainName, s_main ? "found" : "NOT FOUND",
			kOtherName, s_other ? "found" : "NOT FOUND",
			looked);
	}

	int WeaponThrowButtonFor(const bool isLeftGameHand)
	{
		TESForm* form = isLeftGameHand ? s_other : s_main;
		return GripBlink::ThrowButtonFromGlobal(form != nullptr, form ? Value(form) : 0.0f);
	}
}
