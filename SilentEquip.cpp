#include "SilentEquip.h"

#include "Engine.h"
#include "config.h"

#include <skse64/GameObjects.h>
#include <skse64/GameRTTI.h>

#include <chrono>

// Muting a weapon means writing null over sound descriptors that live on the
// SHARED BASE FORM. Every Iron Sword in the game shares one. If a restore is ever
// missed, the failure is silent and permanent-looking — the player just quietly
// loses a sound forever and has no idea why. The safeguards below all exist for
// that one reason, and they are deliberately belt-and-braces:
//
//   1. Nothing is muted unless the restore service is proven to be running. If the
//      callback could not be registered, muting is refused outright.
//   2. Deadlines are WALL CLOCK, not accumulated frame time, so a stalled, slow or
//      never-called tick cannot stretch a window indefinitely.
//   3. The service runs on its own unconditional callback — not inside GripHold's,
//      which returns early when the mod is switched off or a menu is open. That was
//      a real hole: legacy mode still mutes, but never ticked.
//   4. Everything is restored on save, on load, and on a new game.
//   5. A hard ceiling caps any single window regardless of the ini.
//   6. An overdue entry is restored AND logged as an error, so a stalled service
//      leaves evidence instead of silence.
//
// None of this survives being wrong about ownership, so it is worth stating: these
// writes are runtime-only. Skyrim does not serialise base-form sound pointers into
// a save, so even a hard crash mid-mute is repaired by restarting the game.

namespace SwapDropAndHoldRedux
{
	namespace
	{
		static const int kMaxSilenced = 8;
		// Hard ceiling regardless of the ini value.
		static const float kMaxSilenceSeconds = 5.0f;
		// Past this, the service has clearly not been running; restore and complain.
		static const float kOverdueSeconds = 10.0f;

		typedef std::chrono::steady_clock Clock;

		struct SilencedWeapon
		{
			TESObjectWEAP* weapon = nullptr;
			BGSSoundDescriptorForm* pickUp = nullptr;
			BGSSoundDescriptorForm* putDown = nullptr;
			BGSSoundDescriptorForm* equipSound = nullptr;
			BGSSoundDescriptorForm* unequipSound = nullptr;
			Clock::time_point deadline;
		};

		static SilencedWeapon s_silenced[kMaxSilenced];
		static bool s_serviceRegistered = false;

		void Restore(SilencedWeapon& entry)
		{
			if (!entry.weapon)
			{
				return;
			}

			entry.weapon->pickupSounds.pickUp = entry.pickUp;
			entry.weapon->pickupSounds.putDown = entry.putDown;
			entry.weapon->equipSound = entry.equipSound;
			entry.weapon->unequipSound = entry.unequipSound;
			entry = SilencedWeapon{};
		}

		// Unconditional: no mode check, no lock check, no menu check. The only job
		// here is making sure nothing stays muted.
		void OnSoundSilenceStep(void* /*world*/)
		{
			const Clock::time_point now = Clock::now();

			for (int i = 0; i < kMaxSilenced; ++i)
			{
				SilencedWeapon& entry = s_silenced[i];
				if (!entry.weapon)
				{
					continue;
				}

				if (now < entry.deadline)
				{
					continue;
				}

				const float overdue =
					std::chrono::duration<float>(now - entry.deadline).count();
				if (overdue > kOverdueSeconds)
				{
					LOG_ERR(
						"Weapon sound silence ran %.1fs past its deadline — the restore service "
						"was not running. Restoring now; please report this.",
						overdue);
				}

				Restore(entry);
			}
		}
	}

	void RegisterWeaponSoundSilence()
	{
		if (s_serviceRegistered || !higgsInterface)
		{
			return;
		}

		higgsInterface->AddPrePhysicsStepCallback(OnSoundSilenceStep);
		s_serviceRegistered = true;
	}

	void SilenceWeaponSoundsFor(TESForm* form)
	{
		// Refuse to mute anything if the thing that un-mutes it is not running.
		// Better to hear an equip chime than to mute a weapon forever.
		if (equipSounds || !s_serviceRegistered || !form)
		{
			return;
		}

		TESObjectWEAP* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
		if (!weapon)
		{
			return;
		}

		float seconds = equipSilenceSeconds;
		if (seconds <= 0.0f)
		{
			return;
		}
		if (seconds > kMaxSilenceSeconds)
		{
			seconds = kMaxSilenceSeconds;
		}

		const Clock::time_point deadline =
			Clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0f));

		// Already muted: extend the window only. Re-saving here would capture the
		// nulls we wrote and make the silence permanent.
		for (int i = 0; i < kMaxSilenced; ++i)
		{
			if (s_silenced[i].weapon == weapon)
			{
				s_silenced[i].deadline = deadline;
				return;
			}
		}

		for (int i = 0; i < kMaxSilenced; ++i)
		{
			if (s_silenced[i].weapon)
			{
				continue;
			}

			s_silenced[i].weapon = weapon;
			s_silenced[i].pickUp = weapon->pickupSounds.pickUp;
			s_silenced[i].putDown = weapon->pickupSounds.putDown;
			s_silenced[i].equipSound = weapon->equipSound;
			s_silenced[i].unequipSound = weapon->unequipSound;
			s_silenced[i].deadline = deadline;

			weapon->pickupSounds.pickUp = nullptr;
			weapon->pickupSounds.putDown = nullptr;
			weapon->equipSound = nullptr;
			weapon->unequipSound = nullptr;
			return;
		}

		// Table full: free the oldest entry and take its slot, so a busy moment
		// cannot leave an unmuted grab AND cannot leak an entry either.
		int oldest = 0;
		for (int i = 1; i < kMaxSilenced; ++i)
		{
			if (s_silenced[i].deadline < s_silenced[oldest].deadline)
			{
				oldest = i;
			}
		}
		Restore(s_silenced[oldest]);

		s_silenced[oldest].weapon = weapon;
		s_silenced[oldest].pickUp = weapon->pickupSounds.pickUp;
		s_silenced[oldest].putDown = weapon->pickupSounds.putDown;
		s_silenced[oldest].equipSound = weapon->equipSound;
		s_silenced[oldest].unequipSound = weapon->unequipSound;
		s_silenced[oldest].deadline = deadline;

		weapon->pickupSounds.pickUp = nullptr;
		weapon->pickupSounds.putDown = nullptr;
		weapon->equipSound = nullptr;
		weapon->unequipSound = nullptr;
	}

	void RestoreAllWeaponSounds()
	{
		for (int i = 0; i < kMaxSilenced; ++i)
		{
			Restore(s_silenced[i]);
		}
	}
}
