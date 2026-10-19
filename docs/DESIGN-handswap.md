# Either-hand bows and two-handers (HandSwap) — ABANDONED

**Status: ABANDONED 2026-08-28. The code was removed; this document is kept only so
nobody spends the same days rediscovering why.**

## Why it was abandoned

Two mechanisms were tried against the live game:

1. **Cached pair only** (`player+0x6D4/+0x6D8`). Safe — never crashed — and it does move
   the draw/fire LOGIC: with a bow equipped the arrow changed hands and the weapon
   "acted as if it wanted to be on the other side". But the bow MODEL stayed attached to
   its original hand, so the result was half a feature.
2. **Pair plus the global `bLeftHandedMode` byte** (RVA 0x01E71778). This moves the model
   too — and crashes the game. Grabbing a bow right-handed died instantly with an access
   violation writing through null at +0x6C, inside a function whose first act is

   ```c
   v4 = NiAVObject::LookupBoneNodeByName(...);
   v8 = v4[1].vftable;      // dereferenced with NO null check
   ```

   Changing handedness makes the engine re-derive that hand's bone node, find none for the
   newly assigned side, and write through the null result. The write itself succeeds; the
   engine's reaction to it is what dies. Flipping *before* the equip was designed as a
   third attempt but the feature was cancelled before it was tested.

**Verdict:** the engine treats handedness as a start-up-time global, not a per-weapon
property. Two crashes on the same mechanism is an architecture answer, not a bug to chase.

## If it is ever revisited

Do not flip engine handedness. The only route that cannot crash this way is to leave the
engine's state alone entirely and re-draw the weapon somewhere else: drive the weapon's
node from HIGGS's post-VRIK/post-HIGGS callback each frame, paired with the (safe) cached
pair flip for the logic half. That is cosmetic-plus-logic rather than a real engine swap,
it needs the physics body moved as well for melee hit detection, and it is a per-frame
fight with anything else that writes transforms. Costed but never built.

---

*Original design below, retained for reference.*

**Date:** 2026-08-28
**Status:** designed, NOT built. Approved shape: held-only swap with automatic revert;
mirrored inputs acceptable while active.

## 1. What the player gets

Grab a bow with the right hand and it behaves as your bow hand — you draw with the left.
Grab a greatsword with the left hand and it swings from the left. Let go, and everything
returns to normal instantly. No settings screen, no mode change, no re-binding.

## 2. The engine lever (verified in the 1.4.15 decompilation)

Skyrim VR has no per-weapon handedness. It has one global:

- `bLeftHandedMode` is cached as a byte at **RVA 0x01E71778**, read by 23 engine functions.
- `sub_1406AF160` expands that byte into a **complementary pair of hand indices** on the
  player: `[player+0x6D4] = (flag == 0)` and `[player+0x6D8] = (flag != 0)`.
- **38 functions read +0x6D4**, including `TESObjectWEAP::Fire` and the activate/rollover
  path. So the pair does not merely pick a render node — it routes attacks and activation.

That is the whole reason left-handed mode swaps your controls: it is the same switch.
Flipping it per weapon is therefore possible, but it is player-global state that must be
put back exactly, every time, on every exit path.

## 3. Approach: held-only flip, aggressive revert

While the player holds a both-hands weapon (bow, crossbow, real two-hander) in the hand
opposite to the engine's assignment, write the flipped pair. Restore it the moment that
stops being true.

The revert is not a single call site — it is a **per-frame invariant**:

```
every frame:
    want = (a both-hands weapon is equipped)
        AND (GripHold bound it to the "wrong" hand)
        AND (mod not globally locked, GripHoldMode on)
        AND (no menu open, game not paused)
    if want != swapped: apply the difference now
```

Anything that goes wrong — weapon vanishes, menu opens, save loads, the player dies —
fails the predicate on the next frame and the pair goes back. There is no path that
leaves it flipped because there is no path that stops the tick.

## 4. Safety rules

1. **Never stack.** Track our own `swapped` flag; only ever write the pair to a value we
   computed from the ORIGINAL global byte, never from the current pair.
2. **Verify before writing.** Read the pair first; if it is not the value we expect (some
   other mod moved it), do nothing and log once. Do not fight another mod for it.
3. **Restore before anything persistent.** Force the restore on `kPreLoadGame`, `kNewGame`,
   and menu open, so a save can never be written while flipped.
4. **Off by default.** `HandSwap = 0` in the ini until it has been tested; it is a
   separate switch from `GripHoldMode` so it can be disabled without losing grip-hold.
5. **One hand at a time.** If both hands somehow qualify, do nothing.

## 5. Milestones

- **A — Probe (must come first).** A debug-only chord that flips the pair and logs the
  before/after values, with the same auto-revert. This answers empirically whether writing
  +0x6D4/+0x6D8 actually moves the bow to the other hand on this runtime, BEFORE any real
  logic is built on the assumption. Cheap, and it either validates the whole design or
  kills it in one test.
- **B — Bow only, automatic.** Wire the predicate to bows grabbed with the main hand.
  Bows are the common case and the least entangled with attack animations.
- **C — Two-handers.** Same predicate, real 2H weapons. Vanilla two-handers use the
  both-hands equip slot; verify the swap moves them rather than producing a broken equip.
  (2H Weapons Unlocked VR's 1H proxies do not need this at all and must be excluded.)
- **D — Compatibility pass.** VRIK and HIGGS read handedness themselves. Check the arms,
  the bow-aim, and HIGGS grabbing while swapped; if VRIK desyncs, the feature stays
  bow-only or gains a VRIK-specific step.

## 6. What would kill it

If milestone A shows the pair is read once at equip time rather than continuously, then
flipping it mid-hold does nothing and the only remaining route is hooking the specific
read in the weapon-attach path — a much deeper change that I would want to scope
separately rather than fold into this mod.

## 7. Open questions for A

- Is `a1` in `sub_1406AF160` really the PlayerCharacter, or a VR-data sub-object? The
  probe must resolve the base pointer the same way the engine does before writing.
- Does the engine rewrite the pair itself each frame (which would fight our write)?
- Does anything cache the hand assignment at draw time, so the swap only takes effect on
  the next equip/draw cycle?
