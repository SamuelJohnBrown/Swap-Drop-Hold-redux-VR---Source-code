# Hand switch and per-mode grip toggles (3.0.0 test 14)

**Date:** 2026-09-25. **Status:** design approved by the user in chat; not built.
**Mods touched:** Swap Drop & Hold Redux (SDH), TrueWield VR (TWV), Physical Collision VR (PCVR).
**Plan:** `docs/PLAN-hand-switch-and-grip-toggles.md`.

## What the player gets

1. **Menu toggles per mode.** The Simple mode page and the Grip Hold mode page each get a section
   "Holding a weapon you pick up" with three switches:
   - **Hold where you grab** (on by default): a weapon is held where the hand took hold of it.
   - **Reverse grip** (on by default): a weapon taken hold of backwards is held reversed.
   - **Reverse two-handed weapons too** (off by default, experimental): greatswords, battleaxes and
     warhammers may be reversed as well.

   Each page's switches apply only in that mode, like every other setting on those pages. Legacy has
   no toggles and keeps today's ini-only `bGrabGrip` / `bGrabGripReversed`; it never reverses a
   two-hander.
2. **Reverse without the grab position.** With "Hold where you grab" off and "Reverse grip" on, a
   weapon taken hold of backwards is held reversed at its normal grip. Today turning the position off
   also turns the reverse off, because SDH sends nothing at all.
3. **The hand switch, in Simple and Grip hold.** Two-handing a one-handed weapon, the hand holding it
   lets go. The weapon moves to the other hand as fast as possible, held where that hand was holding
   it (for example high on the blade), and reversed if that hand held it backwards. This follows that
   mode's toggles.
   - Grip hold: opening the hand, as today, through the existing hand-off.
   - Simple: the first opening of the hand (no double tap needed). A double tap while two-handing now
     switches instead of dropping (the user's choice).
   - A real two-hander stays in the main hand while either hand grips it. The engine cannot hold one
     left-handed (`docs/DESIGN-handswap.md`, abandoned 2026-08-28).
4. **Reversed two-handers** behind the off-by-default toggle above.

## User decisions (2026-09-25)

- Separate switches per mode (Simple, Grip hold); none for Legacy.
- The hand switch runs in both Simple and Grip hold.
- "Two handed weapon functionality ... off by default for reversed weapons" = reversed two-handers.

## How it works

### The chain (unchanged)
SDH senses the grab and publishes a `GrabGripEvent` (`SdhGrabGripInterface.h`) just before it equips.
TWV stores it and applies it on the frame PCVR's weapon twin goes live: `SetGripSlide` (position) and
`SetGripFlip` (reversed). PCVR places the weapon. PCVR forgets any reversal whenever a twin is torn down
(`DestroyProxy`), so a weapon sent nothing is held the natural way.

### SDH settings
New `[Settings]` keys, defaults in brackets:
`bGrabGripSimple` [1], `bGrabGripReversedSimple` [1], `bGrabGripReversedTwoHandedSimple` [0],
`bGrabGripGripHold` [1], `bGrabGripReversedGripHold` [1], `bGrabGripReversedTwoHandedGripHold` [0].
`GrabGrip::ChooseForMode` picks the three for the mode in force; Legacy uses `bGrabGrip` /
`bGrabGripReversed` with two-handers forced off.

### The wire contract (additive)
- A `kReversed` event **without** `kHasAlong` means: turn it over, keep the natural grip. SDH sends
  it when the position is off and the grab was backwards. When neither applies, SDH sends nothing.
- `source = kHandOff` and `weaponRefID = 0` mark a hand switch. The header reserved both already.
- An older TWV ignores an event with no `kHasAlong`, so the weapon is held normally. That is safe.

### SDH: sensing a hand switch
At the moment the switch is decided (the frame the hand opens), SDH measures the receiving hand
against the weapon the other hand holds:
- The weapon's shape and pose: HIGGS's weapon collision body for the hand holding it
  (`GetWeaponRigidBody`), read with the pickup sensor's `ReadBody` / `ReadLocalBox`.
- Where: the receiving hand's palm (HIGGS hand bone + `PalmInHand`) in that body's frame. That gives
  `alongU`, `hiltU`, the tip and the off-axis distance, the same maths as a pickup.
- Which way round: the weapon hangs off the holding hand's attach node (`WEAPON` / `SHIELD`, first
  person). PCVR writes a reversed hold onto that node. Held the natural way in the receiving hand it
  would hang off that hand's attach node. So `bladeNatural = receivingAttach.rot * holdingAttach.rotᵀ *
  bladeActual`, judged with the pickup's `JudgeReversed` and cone.
- The record is kept for the receiving hand and published by the transfer task just before the equip.
- With HIGGS's weapon collision off (`EnableWeaponCollision = 0`) there is no body to measure. The
  switch still happens, and the weapon lands at its natural grip, with a log line saying why. The
  user's own setup has it on, and the throw fixups' "(pose corrected)" proves the body reads.

### SDH: triggering and speed
- `HandSwitch::Decide`: a one-handed weapon, the other hand on it (HIGGS two-handing latch), its grip
  closed, and that hand holding nothing of its own.
- Grip hold: the existing hand-off branch, now also ahead of the older-VRIK holster check. Holster
  design decision 6 says a second hand holding the weapon wins over holstering.
- Simple: a single debounced release (`DoubleTapDetector::Released()`, which is
  max(flicker floor, release debounce)) after the minimum hold time.
- The transfer skips the 8 settle frames Machina's swap pull waits. The Legacy swap pull keeps them.
  A log line reports the time from release to the transfer.

### TWV
`twv_grabgrip::Touch::hasAlong`: false means slide 0 (the natural grip), with the flip as sent.
`OnGrabGripEvent` accepts an event with `kHasAlong` or `kReversed`.

### PCVR
`pcvr_flip::FlipAllowedForWeaponType(type, allowTwoHanded)` replaces the one-handed-only check.
New `[Beta] bGripFlipTwoHanded = 1` is a kill switch, left on so that SDH's menu toggle is the only
switch a player needs. PCVR still only turns a weapon over when asked. The reversed two-hand fix
(revgrip4) has no weapon-type limit and covers a reversed greatsword held in both hands. This must be
proven in game.

## Evidence the log must show
- SDH: `Hand switch: the left hand holds Iron Sword 64% of the way up from the hilt - REVERSED`, then
  `Hand switch: moved Iron Sword to the left hand 0.0Xs after the release`, then `Grab grip: sent
  before equipping ... - it answered: will slide and turn it over`.
- SDH, position off: `Grab grip: sent before equipping Iron Sword ... REVERSED at its natural grip`.
- TWV: `[grabgrip] L pending: ... taken hold of BACKWARDS ... at its natural grip`, then `applied`.
- PCVR: the usual `[flip]` lines, and for a greatsword `[flip 2h]`.

## Out of scope
Moving a real two-hander to the other hand. A hand-switch on/off toggle (it follows the grip toggles).
Legacy menu toggles.
