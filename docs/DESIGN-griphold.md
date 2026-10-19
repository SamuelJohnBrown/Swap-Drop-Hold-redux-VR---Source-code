# Swap Drop & Hold Redux — GripHold merge design

**Date:** 2026-08-27
**Branch:** `swap-griphold`
**Authors:** Jashua (with Claude), building on Swap Drop & Hold Redux 1.3.4 by SamuelJohnBrown
and Weapon Pick Up VR 0.8.0.

## 1. Goal

Merge the proven mechanics of Weapon Pick Up VR (WPVR) into Swap Drop & Hold Redux (SDH),
shipping as SDH under Sam's mod name. SDH keeps its identity and all existing behavior behind a
master switch; a new **GripHold** mode replaces the "sticky" hold model with a Blade & Sorcery
style grip-held model, plus WPVR's throw momentum and Immersive Weapon Penetration (IWP)
stab-and-stick integration.

WPVR's grab-to-equip half is dropped entirely — SDH's pickup/equip pipeline
(`Activate` → inventory poll → `EquipManager::EquipItem`) is the one that ships.

## 2. The GripHold model (`bGripHoldMode=1`)

A weapon in your hand belongs to your physical grip.

### 2.1 Pick up (unchanged)
Grip a world weapon → HIGGS grab → SDH's existing pickup path equips it to that hand.
All of SDH's per-type rules (bow off-hand, crossbow main-hand, torches, staves, shields,
2H toggles, exclusion lists) apply exactly as today.

### 2.2 Release grip
Releasing the grip of a hand whose weapon is **armed** (see 2.4) and while the mod is not
**locked** (see 2.5) does, in priority order:

1. **Hand-off** — if the other hand is currently two-hand-gripping that same weapon
   (HIGGS grab of the equipped weapon, detected by the existing opposite-hand plumbing),
   the equip transfers to that hand via the existing `TransferWeaponToOppositeHand`.
   Works in both directions, including 2H weapons.
2. **Stick** — if IWP reports the weapon embedded in a body, the weapon stays in the wound:
   unequip + spawn world ref at the wound, pin it to the victim's nearest bone, hand the
   wound state to IWP (`GetEmbedInfo`). Re-gripping within reach starts a progressive
   draw-out (hand travel projected on the blade axis); once free the weapon re-equips and
   IWP resumes the embed (`ResumeEmbed` with explicit axis + blade length) so extraction has
   real resistance.
3. **Drop with momentum** — otherwise the weapon drops from the hand carrying the hand's
   real swing velocity (pose-history sampling). A fling-and-release throws it in one motion.
   No HIGGS middle step — the hand is open.

### 2.3 Menu / inventory equips are grip-bound too
A weapon equipped from a menu, favorites, or another mod obeys grip-release like a grabbed
one — after **arming** (2.4).

### 2.4 Arming
A freshly equipped weapon is NOT dropped by an open hand until the player has pressed grip
once while holding it. Grab-equips arm immediately (the grab *is* the grip press);
menu-equips arm on the first grip press afterward. This prevents a menu equip from falling
on the floor the instant the menu closes.

### 2.5 Global perma-lock
**Hold grip + double-click trigger** on the main-hand controller (right unless the game's
left-handed mode; buttons and double-click window INI-configurable) toggles a global lock:

- Locked → the entire GripHold layer is inert. Weapons are vanilla-sticky, grip releases do
  nothing, stick/momentum/hand-off are all off. SDH legacy features stay off too (they are
  mode-switched, not lock-switched). Vanilla game + HIGGS, effectively.
- The lock state is written to disk immediately on toggle
  (`Data\SKSE\Plugins\SwapDropAndHoldRedux_state.ini`) and survives sessions, saves, and
  characters. Perma until chorded again.
- The chord is ignored while any menu is open; a short cooldown prevents double-firing.
- Toggling shows a brief notification so the player knows which state they're in.

### 2.6 Legacy mode (`bGripHoldMode=0`)
Stock SDH 1.3.4 behavior: sticky hold, trigger-hold drop, swap-pull. The legacy code paths
are disabled by the mode switch, never removed. Default for Sam's existing users is his call
at upstream time; our test builds default to `1`.

## 3. Architecture

New files, additive; edits to existing SDH files kept small and behind `bGripHoldMode`.

| Unit | New/Edit | Purpose |
|---|---|---|
| `GripHold.cpp/h` | new | Grip state per hand, arming, release dispatch (hand-off → stick → drop), global lock chord + persisted state |
| `ThrowMomentum.cpp/h` | new | Per-hand pose-history ring buffer, fastest-window velocity, multi-frame velocity re-assert + island activation |
| `EmbedPins.cpp/h` | new | Pin table (dynamic body + no-collision + zeroed velocities per frame, bone-tracked, damped), draw-out, IWP handoff |
| `IwpInterface.h` | new (copied from IWP) | `IIwpInterface001`: IsEmbedded / GetEmbedInfo / StartEmbed / EndEmbed / ResumeEmbed |
| `FrameTick.cpp/h` | new | Per-frame update hook driving pose sampling, pin servicing, chord detection |
| `TriggerHoldLogger.cpp` | edit | Skip trigger-hold drop when GripHold mode on |
| `OppositeHandGrabLogger.cpp` | edit | Skip swap-pull when GripHold mode on; expose two-hand-grip state to GripHold |
| `WeaponGrabActivator.cpp` | edit | Notify GripHold on grab-equip (arms the hand) |
| `config.cpp/h` | edit | `[GripHold]`, `[Throw]`, `[Embed]` INI sections |
| `main.cpp` | edit | Module registration, IWP interface acquisition |

### 3.1 Input
Grip/trigger state comes from the same input path SDH already uses for trigger-hold
detection, extended to grip buttons; button ids INI-rebindable (headsets differ; WPVR's
`2,34` grip default carries over). Left-handed mode goes through SDH's existing
`GrabHandToGameHand` — one convention everywhere.

### 3.2 Per-frame tick
SDH already has a per-frame tick: `higgsInterface->AddPrePhysicsStepCallback` (its
trigger-hold state machine runs on it). GripHold registers its own callback on the same
HIGGS hook — no new engine hook needed. `AddTask` recursion is NOT a frame tick and will
not be used for one.

### 3.3 IWP interface
IWP ≥ 1.7.2 broadcasts its interface at load (SKSE message `0x49575001` from
"ImmersiveWeaponPenetrationVR"); SDH registers a listener the same way WPVR did.
IWP absent → embed features silently off (same pattern as SDH's HIGGS guard). All three
WPVR resume fixes are preserved: wait for `GetWeaponState() == kDrawn` **exactly**, force the
draw, hold it for `fResumeDrawGuard` seconds.

### 3.4 Havok in the classic SDK
WPVR's pin/velocity code used CommonLibVR wrappers; SDH is classic SKSE64 SDK. The
underlying Havok memory layouts are identical — the port translates WPVR's working code onto
the classic SDK's thinner types plus raw offsets, each verified against the decompilation.
Hard-won rules that carry over verbatim:

- Never keyframe a pinned/driven dropped-ref body (node stops publishing); keep it dynamic +
  `kNoCollision` + zeroed velocities + activate every frame.
- Search the dropped ref's whole 3D subtree for the collision body.
- Re-assert throw velocity for several frames; never `SetPosition`/`SetAngle` on it.
- Pinned bodies are uncollidable → re-grab is grip-press-within-reach, not HIGGS grab.

## 4. Config (new INI keys, SDH style)

His parser is flat `key=value` — `[section]` lines are ignored — so keys are flat and
globally unique, matching his existing names (`EnableTwoHandedWeapons` etc.):

```ini
GripHoldMode=1            ; master: 1 = grip-held model, 0 = stock SDH behavior
ThrowMomentum=1
ThrowVelocityScale=1.0
ThrowWindow=0.15
ThrowStampFrames=6
ThrowMaxSpeed=1200.0
StickOnRelease=1
EmbedMaxSeconds=30.0
EmbedSmoothing=0.5
EmbedRadius=12.0
PullReach=25.0
PullOutDistance=45.0
PullOut=1
ResumeIwpEmbed=1
ResumeDrawGuard=0.6
MinHoldTime=0.35
RegrabCooldown=0.4
LockDoubleClickWindow=0.35
LockChordCooldown=1.0
LockNotifications=1
GripReleaseGraceSeconds=0.10
```

Grip/trigger button reads reuse SDH's existing masks (grip `1<<2`, trigger `1<<33` +
`rAxis[1].x`), same as its trigger-hold gesture. Every feature has an off switch. Exact
defaults tuned during testing.

## 5. Compatibility

- **HIGGS** required (as SDH already requires for grabbing). **VRIK / Spell Wheel /
  SkyrimVRESL / Biting Axes** integrations untouched. SDH's own mod API events keep firing
  from the same places; the API surface does not change.
- **IWP** optional; enables stick/draw-out.
- Weapons excluded via SDH's `ExcludedForms` are invisible to GripHold too.

## 6. Out of scope (this build)

- Per-weapon lock (replaced by the global lock).
- WPVR's fingerprint matching (SDH's Activate pickup preserves tempering natively).
- Any change to SDH's bow/crossbow/shield/2H type rules.
- Weight/lag feel (TrueWieldVR's territory).

## 7. Testing & release

- Baseline: build unmodified 1.3.4 in the SKSEVR tree first — proves toolchain before any
  edit.
- Each milestone ships as a zip to Downloads (MO2/Vortex layout), M-prefixed version string
  (`M1.4.x`), user tests in-game. Numeric/public version stays Sam's to burn.
- Milestones: **A** baseline build → **B** GripHold core (grip release = plain drop, arming,
  mode switch) → **C** momentum + hand-off → **D** global lock + persistence → **E** IWP
  stick/draw-out.
- Upstream: when the user says a build is ready and Sam agrees, the `swap-griphold` branch
  goes to his repo as a PR — his toolchain, his file style, no framework change.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Classic-SDK Havok port (no CommonLib wrappers) | Translate WPVR's working code; verify every offset against the 1.4.15 decompilation; milestone E lands last |
| Frame-tick hook in classic SDK | Port WPVR's hook; decomp-verify; trampoline already allocated |
| Grip release mid-menu/loading edge cases | Lock chord state machine ignores menus; arming prevents menu-equip drops |
| Double-click trigger misfires in combat | Chord requires grip held + tunable window + cooldown; fully rebindable |
| SKSEVR SDK build environment (v142/v143) | Baseline milestone proves it before feature work |
