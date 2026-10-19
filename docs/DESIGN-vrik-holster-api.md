# Swap Drop & Hold Redux — Blade & Sorcery holsters through VRIK's holster API

**Date:** 2026-09-14
**Branch:** `swap-griphold`
**Authors:** Jashua (with Claude), for Swap Drop & Hold Redux by SamuelJohnBrown, using the holster API
Shizof added to VRIK build 80700 (VRIK 0.8.7).
**Status:** implemented as test build M2.2.0-test1, not yet tested in game. Five questions about VRIK's API are with Shizof;
section 6 lists what each answer can change.

## 1. Goal

In Grip hold mode, opening your hand at a VRIK holster should put the weapon in the holster, the way Blade
& Sorcery snaps an item into a holster. Today Grip hold stands aside and waits for a second squeeze.

VRIK build 80700 lets a plugin ask VRIK itself three things: is a holster in reach, can it take the
weapon, and please holster it. Both weapon modes use those answers instead of this mod's estimates of
where VRIK's slots are. Players on older VRIK keep exactly what 2.1.1 does.

## 2. Decisions made with the user

1. Only Grip hold holsters on release. Simple and Legacy keep their behaviour.
2. The range is VRIK's own. There is no catch radius on top of it.
3. A holster in range that cannot take the weapon (full, too big, not for that hand) lets the weapon drop.
   The controller buzz means "this holster will take it".
4. Both modes ask VRIK where the holsters are. Simple mode's stand-aside range becomes VRIK's exact answer.
5. If VRIK agreed to holster but the weapon is still in the hand 3 seconds later, the weapon stays in the
   hand. It is never dropped after VRIK agreed.
6. A fast release inside a holster's range holsters rather than throws. A second hand still holding the
   weapon (a two-hander's other grip, or a hand-off) wins over holstering.
7. Grip hold gets its own toggle, "Holster when you let go", on by default.

## 3. Background

### 3.1 Why the mod has been estimating

Before build 80700, VRIK's plugin interface had no holster query. The mod reads VRIK's slot layout through
`getSettingDouble` and reconstructs where each slot is, in `VrikHolsters.cpp`. It relies on guessed anchor
bones, a calibrated palm, and positions learned from real draws. Those estimates were measured 7 to 12
units off, against an activation distance of 8 to 10 units.

Grip hold keeps the grip closed while a weapon is carried, so the grip press VRIK holsters on never
happens. Since 2.0.x Grip hold has worked around that with a stand-aside zone:
- The zone is VRIK's range, plus `fHolsterExtraRange` (6) in front or `fHolsterExtraRangeBehind` (14)
  behind, plus `fHolsterReleaseMargin` (4) to leave it.
- Opening the hand inside it does nothing, and the next squeeze lets VRIK holster.

2.1.1 shrank Simple mode's zone to VRIK's reach, still estimated. It shipped a known risk: a double tap
right on a holster might drop the weapon.

Holstering the weapon ourselves (`iHolsterAction` 1 and 2) never worked, because a VRIK holster is not a
sheathe (3.3).

### 3.2 What build 80700 adds

`vrikinterface001.h` gains four virtual functions after `getFinalSmoothingOffsettingAmount`. The mod's
copy of the header matches VRIK's line for line up to there, so appending them keeps the vtable order:

```cpp
virtual int  GetHolsterSlotInReach(bool secondaryHand) = 0;
virtual bool CanBeHolstered(bool secondaryHand) = 0;
virtual bool HolsterWeapon(bool secondaryHand) = 0;
virtual bool UnHolsterWeapon(bool secondaryHand) = 0;
```

Shizof's rule: check the interface for null, and call these only when `getBuildNumber() >= 80700`,
because an older VRIK's vtable has no such slots. The mod already acquires the interface at PostPostLoad,
where he says to.

VRIK 0.8.7 Beta2's `_vrik_qust_system.psc` also sends two mod events. `SendHolsterModEvent` fires them at
the end of `OnActivateSlot`, after VRIK's own equip or unequip:

| Event | strArg | numArg |
|---|---|---|
| `VRIK_Holstered` | `Primary\|0xFE123ABC` or `Secondary\|0xFE123ABC` (the base form) | slot number |
| `VRIK_Unholstered` | the same | slot number |

- **Hands:** "Primary" is VRIK's Papyrus `abIsRightHand`, which is the game's right hand.
- **What changed:** compared with VRIK 0.8.5's script, these events are Beta2's only Papyrus change.
- **Dependency:** they need VRIK's own `_vrik_qust_system.pex` deployed. Shizof warns that Arctal's VRIK
  Tweaks overwrites it. On the development install it does not, because VRIK Tweaks 2.0 ships only
  `vriktw_*.pex`.
- **Thread:** SKSE flags `Form.SendModEvent` as NoWait (`skse64/PapyrusForm.cpp`), so a listener can be
  called on a Papyrus thread.

### 3.3 How VRIK holsters

VRIK.dll calls the Papyrus event `OnActivateSlot(slot, isRightHand)` when a slot is activated:

- **Empty hand:** the slot's weapon is equipped into the hand. This is a draw.
- **Weapon in hand, empty slot:** the slot takes the weapon's model and the weapon is UNEQUIPPED. This is
  a holster.
- **Weapon in hand, filled slot:** the weapons swap, so the slot's weapon comes into the hand.

A VRIK holster is an unequip plus a slot model, not a sheathe.

## 4. Behaviour

### 4.1 Grip hold, VRIK 0.8.7 or newer, "Holster when you let go" on (the default)

- Open your hand in range of a holster that VRIK says can take the weapon, and it is holstered. The
  decision is made the moment the release is recognised.
- Your other hand comes first. A two-hander your other grip still holds stays in hand, and a one-hander
  your other hand grips is handed over. Holstering is considered only after those.
- Anywhere else the release does what it does today: a lodged blade sticks, and anything else drops.
- The controller buzzes when your hand enters range of a holster that will take the weapon it holds. It also buzzes
  when a weapon is drawn at a holster, because letting go there would put it back.
- There is no stand-aside zone.
- A fast release inside the range holsters. There is no speed check.
- If the grip sensor blinks open, the weapon goes into the holster the hand is at. A squeeze there draws
  it back.

### 4.2 Grip hold, VRIK 0.8.7 or newer, toggle off

Grip hold stands aside at holsters as in 2.1.1: opening the hand there does nothing, and a squeeze lets
VRIK holster. The difference is that the range is VRIK's own answer instead of the estimated catch zone.

### 4.3 Simple mode, VRIK 0.8.7 or newer

It plays the same as before. The mod steps aside for a double tap only where VRIK reports a holster in
reach, and the buzz marks entering that range. The answer now comes from VRIK instead of the estimate.

### 4.4 Both modes, VRIK 0.8.7 or newer

These are not used:
- `fHolsterExtraRange`, `fHolsterExtraRangeBehind` and `fHolsterReleaseMargin`
- `bHolsterLearning`. The learned-positions file is left alone.
- `iHolsterAction`, which counts as 0
- palm calibration and the sphere scan
- the 14-slot trace, which becomes one line (5.3)

### 4.5 VRIK older than 0.8.7, no VRIK, and Legacy mode

Exactly 2.1.1.

## 5. Design

### 5.1 Components

| File | Change |
|---|---|
| `vrikinterface001.h` | Append the four functions. Nothing else in the header, or in `vrikinterface001.cpp`, changes. |
| `VrikHolsterApi.cpp/.h` (new) | The only code that calls VRIK's holster functions: availability, the three calls by game hand, the hand mapping, the mod-event listener, and the warnings in 5.6. |
| `VrikHolsterRules.h` (new) | Engine-free rules, tested by `tools_vrik_holster_tests.cpp`: build gate, slot validity, event parsing, the release decision, the wait, the buzz edge, and the effective holster action. |
| `VrikHolsters.cpp/.h` | `HolsterNearHand` asks VRIK first. Learning, palm calibration and the sphere scan stand down when VRIK answers, and the trace becomes one line. |
| `GripHold.cpp` | Grip hold with the API and the toggle: no stand-down, the buzz edge, the new release order, and the per-hand wait. |
| `TriggerHoldLogger.cpp` | `OnPrePhysicsStep`, which already ticks in every mode, drains the event queue (5.6). |
| `main.cpp` | Register the listener in `SetupReceptors`. Log availability after the interface is acquired at PostPostLoad. |
| `config.h/.cpp` | `holsterOnRelease`, ini key `bHolsterOnRelease`, default 1. |
| Shipped ini, MCM `config.json`, both MCM `settings.ini` copies | The toggle, and help text marking the unused settings "older VRIK only". |
| `SwapDropAndHoldRedux.vcxproj` and `.filters` | Add `VrikHolsterApi.cpp/.h`. The rules header and its test stay out, like the others. |

### 5.2 Asking VRIK

- **Available** means `vrikInterface != nullptr` and `getBuildNumber() >= 80700`. It is decided once at
  PostPostLoad and logged as either `VRIK holster API: build N - available` or
  `VRIK holster API: build N - not available (needs 80700), holsters work as in 2.1.1`.
- **Hand mapping:** one function turns a game hand into VRIK's `secondaryHand`. The working assumption is
  `secondaryHand == isLeftGameHand`, because VRIK's own event calls the game's right hand Primary
  (contingency C1).
- **Slots** are 1 to 14. Any other value from `GetHolsterSlotInReach` means none.
- **Where the calls run:** from GripHold's HIGGS pre-physics step, where the mod already reads equip state
  and issues sheathes (contingency C5).

### 5.3 `HolsterNearHand` with VRIK

When the API is available, `HolsterNearHand`:
- returns VRIK's slot for the controller's game hand;
- reports no distance (0), with the position source `VRIK`;
- ignores its `reach` argument, because VRIK's answer is the range.

Its consumers pick this up without further change:
- `ShouldStandDownForHolster` (Simple, and Grip hold with the toggle off)
- `DispatchGripRelease`'s existing holster check (the same cases)
- `HolsterTraceRelease`

When the API is available, these return at once:
- `HolsterLearnFromDraw`
- `HolsterCapturePressCandidate` and `HolsterLearnFromCapture`
- `HolsterCalibratePalms`
- `HolsterScanSpheresTick`

`HolsterTraceRelease` logs one line instead of fourteen, still behind `HolsterTrace`:
`Holster trace: VRIK says slot 2 Right Hip is in reach of the right hand; takes Iron Sword: yes`.

### 5.4 Grip hold release

The new path applies when all four hold: the API is available, `bHolsterAware` is on, `bHolsterOnRelease`
is on, and the mode is Grip hold.

- **No stand-down:** `ShouldStandDownForHolster` returns false, so a hand holding a weapon stays armed at a
  holster.
- **Buzz:** each frame a hand holds a bound weapon, "VRIK has a slot in reach and can holster" feeds the
  buzz edge. `PulseController` fires on entering, and does not fire again until the answer has been "no"
  for 0.15 s.
- **Release order:** `DispatchGripRelease` runs these steps in order:
  1. Two-hander still gripped by the other hand: keep it (unchanged).
  2. One-handed hand-off: hand it off (unchanged).
  3. **Holster through VRIK:**
     - No slot in reach: go on to 4.
     - Slot in reach, `CanBeHolstered` false: log
       `VRIK says Right Hip can't take Daedric Greatsword - dropping it.`, then go on to 4.
     - Slot in reach, `CanBeHolstered` true: call `HolsterWeapon`.
       - If it returns true: start the wait (5.5), log
         `GripHold: right controller let go of Iron Sword at the Right Hip holster - VRIK is holstering it.`,
         and stop.
       - If it returns false: log `VRIK refused to holster Iron Sword at Right Hip - dropping it.`, then go
         on to 4 (contingency C4).
  4. Stick, if the blade is lodged (unchanged).
  5. Drop, with the existing yield (unchanged).
- On this path, the existing first-in-line holster check in `DispatchGripRelease` is skipped.
  `HolsterTraceRelease` still runs at the top of every release.

The release and stand-down code runs as it does today everywhere else. That covers Simple mode, Grip hold
with the toggle or `bHolsterAware` off, and VRIK older than 80700. When the API is available, 5.3 supplies
VRIK's answer to that code.

`iHolsterAction` is read through one rule: when the API is available it counts as 0.

### 5.5 Waiting for VRIK

`GripHand` gains `holsterWaitSeconds`, `holsterFormID`, `holsterIsLeftGameHand` and `holsterSlot`. They are
all default-initialised, so loading a save, starting a new game and switching mode clear them, just as
they already clear the hand.

`ServicePendingHolster` runs at the top of `UpdateGripHand`, beside `ServicePendingDrop`. While it is
waiting, nothing else in that hand's update runs. Each frame it checks:

- **The weapon is no longer equipped in that game hand:** done. Log
  `VRIK holstered Iron Sword into Right Hip 0.05s after the release.` and reset the hand.
- **A different weapon is in that hand:** VRIK swapped. Log it, end the wait, and let the normal bind path
  take the new weapon (contingency C3).
- **3.0 seconds have passed with the weapon still there:** keep it. The hand is armed if the grip is
  closed; otherwise it waits for a fresh squeeze. Log at error level:
  `VRIK agreed to holster Iron Sword but it is still in the right hand after 3s - keeping it. VRIK may have refused quietly, its scripts may be busy, or another mod may be overwriting _vrik_qust_system.pex.`
- **The grip closes again:** nothing changes, because VRIK's holster cannot be cancelled.

The wait only advances in Grip hold's tick, so an open menu or a spell wheel pauses it. Turning on the global lock
clears it, as a load or a mode switch does. A weapon VRIK has not taken yet is then treated like any equipped weapon.

Never drop after VRIK agreed. A late holster on top of a drop could leave the weapon both in the holster
and on the floor.

### 5.6 The event listener

- **Registration:** a `BSTEventSink<SKSEModCallbackEvent>` on SKSE's own dispatcher, obtained through
  `messaging->GetEventDispatcher(kDispatcher_ModEvent)`. It is registered in `SetupReceptors` beside
  `RegisterExclusions`. Never use `g_modCallbackEventDispatcher`, which is a private copy inside this DLL.
- **Threading:** the listener can run on a Papyrus thread, so it only parses `VRIK_Holstered` and
  `VRIK_Unholstered`. It records {holstered, primary, formID, slot} in a small mutex-guarded queue of eight
  entries, dropping the oldest. `TriggerHoldLogger`'s `OnPrePhysicsStep` drains the queue in every mode and
  logs `VRIK event: holstered Iron Sword (Primary hand) into slot 2 Right Hip.`
- **Coverage:** it listens in every mode, so VRIK's own grip-press holsters and draws show in the log too.
- **Missing event:** VRIK sends the event right after its own unequip, so it can arrive before or after
  the mod sees the weapon leave the hand. If a holster from 5.5 completes and no matching `VRIK_Holstered`
  has arrived within 3 seconds of the weapon leaving the hand, warn once per session that another mod may
  be overwriting VRIK's `_vrik_qust_system.pex` (for example Arctal's VRIK Tweaks). Loading a save or
  starting a new game forgets pending requests. Nothing else depends on the event.
- **Wrong hand:** if the matching event names a different hand from the one asked, warn once per session
  that the hand mapping in 5.2 is wrong (contingency C1).

### 5.7 Settings and text

- **New setting:** `bHolsterOnRelease`, "Holster when you let go", on the MCM Holsters page and in the
  shipped ini, default 1. The help text says, in substance:
  - it applies to Grip hold with VRIK 0.8.7 or newer;
  - open your hand at a holster that buzzes and VRIK holsters the weapon;
  - off means the mod steps aside at holsters and you squeeze again.
- **Master switch, unchanged:** `bHolsterAware` ("Get out of VRIKs way").
- **Marked "older VRIK only"** in MCM help and ini comments: `fHolsterExtraRange`,
  `fHolsterExtraRangeBehind`, `fHolsterReleaseMargin`, `bHolsterLearning`, `iHolsterAction`, and the
  14-slot `HolsterTrace`.
- **Menu text rules:** ASCII only, no `<`, and "VRIKs" without an apostrophe. The audit must report 46
  controls, 46 ini-backed, 0 fatal.
- **Nexus description:** changes only at the public release. It gets Grip hold's holster bullets, its
  known-behaviour line about letting go near the body, and a VRIK 0.8.7 note.

## 6. Contingencies on Shizof's answers

| # | If Shizof says | Then |
|---|---|---|
| C1 | `secondaryHand` is not the game's left hand | Change the mapping function in 5.2 and its tests. The wrong-hand warning (5.6) catches this in game. |
| C2 | `GetHolsterSlotInReach` ignores VRIK's hand or size rules | Simple mode's stand-aside would swallow double taps where a VRIK press does nothing, so Simple also asks `CanBeHolstered`. Grip hold already does. |
| C3 | `CanBeHolstered` is true at a full holster and `HolsterWeapon` swaps | Add a "holster is full" check before calling: ask Shizof for one, or track slot contents from the two events. Until then, the swap branch of the wait (5.5) keeps the result sane. |
| C4 | `HolsterWeapon` returning false means "queued", not "refused" | Treat false like true: wait, then keep. The drop decision rests on `CanBeHolstered` alone. |
| C5 | The calls are not safe from HIGGS's pre-physics callback | Call `HolsterWeapon` from an SKSE task queued at the release, like `GripReleaseDropTask`. Move the queries too if needed, with the buzz using the last answer. |
| C6 | The interface changes again before 0.8.7 is public | Copy the final header. The build gate protects only against VRIK older than 80700, not against a reordered vtable. |

## 7. Testing

### 7.1 Before the headset

- **Console tests:** `tools_vrik_holster_tests.cpp` tests `VrikHolsterRules.h`. It must fail against a
  stub first, then pass. It checks:
  - the build gate (80699 no, 80700 yes) and slot validity (0, -1 and 15 are none; 1 to 14 are valid);
  - event parsing: Primary and Secondary, upper- and lower-case hex, missing separator or prefix, empty and
    garbage text, and slot values;
  - the release decision for every combination of mode, API available, `bHolsterAware`,
    `bHolsterOnRelease`, slot in reach and can holster. The other hand's branches come first in
    `DispatchGripRelease` and return before the decision is asked;
  - the wait: the weapon leaves, a different weapon arrives, the 3-second timeout with the grip open and
    with it closed, and the grip re-closing;
  - the buzz edge: one pulse on entering, none while the answer flickers for under 0.15 s, and a new pulse
    after a real exit;
  - the effective holster action: `iHolsterAction` 1 and 2 count as 0 with the API.
- **Existing tests:** every existing console test still passes.
- **Release gates:** build, stage, FOMOD verifier on all 6 installer paths, menu audit 46/46, zips checked
  against the previous test build, and a commit on `swap-griphold`.

### 7.2 In the headset (VRIK 0.8.7 Beta2 installed)

Grip hold:
1. Bring a one-handed weapon to a hip until it buzzes, then open your hand. It is on the hip.
2. Squeeze there to draw it, then open your hand away from your body. It drops.
3. With that holster full, bring a second weapon there and open your hand. There is no buzz, and it drops.
   If contingency C3 applies, the same result comes from the full-holster check instead.
4. Hold a greatsword over a shoulder and open the main hand. It goes on your back.

Simple:

5. Double tap right at a holster: VRIK holsters it and nothing drops. A double tap a little further out
   drops it.

The log proves each step:
- startup: `VRIK holster API: build 807xx - available`
- steps 1 and 4: `VRIK holstered <weapon> into <slot>`, then `VRIK event: holstered ...`
- step 3: `VRIK says <slot> can't take <weapon> - dropping it.`
- step 5: a `VRIK event: holstered` line, and no drop line for that weapon

If there are no event lines at all, VRIK's `_vrik_qust_system.pex` is being overwritten.

## 8. Out of scope

- Holstering on release in Simple or Legacy mode.
- Any range wider than VRIK's own.
- Drawing through `UnHolsterWeapon`. VRIK's own grip press already draws.
- Holstering a weapon HIGGS holds without it being equipped.
- Removing the estimate and learning code, which older VRIK still needs.
- Changing the older-VRIK release order, where the holster check comes before the hand-off.

## 9. Versions and release

- Test builds carry the M prefix in string fields only. The number is chosen with the user when building.
- Players benefit once VRIK 0.8.7 is public. Until then, a build containing this behaves as 2.1.1.
- Before testing: install VRIK 0.8.7 Beta2, and check that its `_vrik_qust_system.pex` is the deployed
  copy.
