# Build environment notes (for Sam / upstreaming)

## Heads-up: `NiRTTI_*` comparisons in this SDK are a trap

The SDK's `NiRTTI_bhkRigidBody` and friends (`skse64/NiRTTI.cpp`) are stored as
**unrelocated RVAs** — `(NiRTTI*)0x03187F58`. They only mean anything once
`RelocationManager::s_baseAddr` is added, which is what `DoNiRTTICast()` and `IsType()`
do internally. Comparing a live `obj->GetRTTI()` against them directly is **always false**.

This cost a whole test build here: our thrown weapons never received their velocity
because the rigid-body lookup used a direct compare (36 drops, 0 successes in the log).

`OppositeHandGrabLogger.cpp::TryGetHiggsRigidBodyPosition` has the same direct compare:

```cpp
NiRTTI* rtti = obj->GetRTTI();
if (rtti != NiRTTI_bhkRigidBody && rtti != NiRTTI_bhkRigidBodyT) return false;
```

It fails silently rather than visibly — the caller falls back to hand-node positions, so
swap-pull still works but never uses the real weapon/hand rigid-body positions it was
written to use. Deliberately left as-is here (changing it would alter tuned swap-pull
behaviour); flagging it so you can decide. The fix would be
`DoNiRTTICast(obj, NiRTTI_bhkRigidBody)`, which also matches `bhkRigidBodyT` through the
parent chain.

How this repo was built on this machine, and every deviation from a stock setup.
None of these changes live in the repo itself — they are local SDK/toolchain fixups.

## Layout

The repo is cloned into the SKSEVR 2.0.12 source tree, which is what the project's
relative references expect:

```
<workspace>\sksevr_2_00_12\src\sksevr\SwapDropAndHoldRedux\   <- this repo
<workspace>\sksevr_2_00_12\src\sksevr\skse64\                 <- SDK (project refs ..\skse64\)
<workspace>\sksevr_2_00_12\src\sksevr\skse64_common\
<workspace>\sksevr_2_00_12\src\common\                        <- project refs ..\..\common\
<workspace>\sksevr_2_00_12\src\sksevr\xbyak\
```

SKSEVR source: `https://skse.silverlock.org/beta/sksevr_2_00_12.7z`.

## Build command (VS 2022 BuildTools)

```
MSBuild.exe SwapDropAndHoldRedux.vcxproj -p:Configuration=Release -p:Platform=x64
  -p:SolutionDir="<workspace>\sksevr_2_00_12\src\sksevr\\"
  -p:PlatformToolset=v143 -p:WindowsTargetPlatformVersion=10.0 -m
```

`SolutionDir` must point at `src\sksevr\` (where `sksevr.sln` lives) — the project's
include paths are `$(SolutionDir);$(SolutionDir)\..`, which is how `skse64/...`,
`xbyak/...` and `common/...` headers all resolve.

## Local SDK fixups (stock 2.0.12 → what this repo's code expects)

1. **Workspace path must have no spaces.** The 2.0.12 project files interpolate
   `$(SolutionDir)` into cl.exe command lines unquoted; a path with spaces shreds the
   compiler arguments (`warning D9024: unrecognized source file type 'Drop'`).
2. **`GetGameSetting` added to the SDK** (`skse64/GameSettings.h/.cpp`) — `Utility.hpp`
   calls it; newer skse64 SDKs have it, 2.0.12 does not. Implementation is the standard
   one over `g_gameSettingCollection` (VR address 0x02F8A780, already present in 2.0.12):
   ```cpp
   Setting * GetGameSetting(const char * name)
   {
       SettingCollectionMap * settings = *g_gameSettingCollection;
       return settings ? settings->Get(name) : nullptr;
   }
   ```
3. **`skse64.vcxproj` Release|x64 retargeted `DynamicLibrary` → `StaticLibrary`** — this
   repo links `sksevr_1_4_15.lib` (the whole skse64 project statically), and the stock
   DLL configuration produces no import lib (no exports). Static is also what the shipped
   1.3.4 DLL's behavior implies.
4. **Toolset/SDK retarget on the command line only** (`v143`, Windows SDK 10.x) — the
   stock projects name v140/v141 and Windows SDK 8.1, which are not installed here. No
   project files were edited for this; the two `-p:` switches cover every project in the
   reference chain.

## Baseline verification (2026-08-27)

Unmodified 1.3.4 source builds clean with the above; output DLL is x64 and exports
`SKSEPlugin_Query` / `SKSEPlugin_Load` (dumpbin-verified).

## Engine addresses the GripHold merge added (SkyrimVR 1.4.15 only)

| What | Where | Evidence |
|---|---|---|
| `hkpEntity::activate` | module +0xAA7130 | shipped + in-game proven by Weapon Pick Up VR 0.8.0 |
| `hkpRigidBody::setPositionAndRotation` | module +0xAA9030 | same |
| HUD notification (`DebugNotification`) | module +0x908170 | address-library id 52050; verified against the local IDA decompilation and the installed version-1-4-15-0.csv |
| `Actor::DrawWeaponMagicHands(bool)` | Actor vtable slot 0xA6 | CommonLibSSE-NG SE slot; in-game proven by WPVR's forced draws |
| `hkpShape::GetAabbImpl` | shape vtable slot 7 | CommonLib absolute index; in-game proven by WPVR's blade probes |
| Weapon state | `(Actor::actorState.flags08 >> 5) & 7`, drawn == 3 exactly | classic SDK's own `IsWeaponDrawn()` uses the loose `>= 3` test, which is precisely what broke IWP resumes in WPVR 0.8.0 — do not use it here |
| `bhkNiCollisionObject::body` | +0x20 (collision object at NiAVObject+0x40) | CommonLibSSE-NG header with `static_assert(sizeof == 0x28)` |
| Havok struct offsets | hkp body = bhk+0x10; motion +0x150; transform +0x170; lin/ang velocity +0x230/+0x240; collidable +0x20; filter word +0x4C; `kNoCollision` = 1<<14; NiAVObject collision slot +0x40; `bhkNiCollisionObject::body` +0x20 | lifted from CommonLibSSE-NG headers and in-game proven by WPVR 0.6–0.8 |

## Heads-up 2: `Log()` passes formatted text as a format string

`config.cpp`'s `Log()` did this:

```cpp
vsprintf_s(logBuffer, sizeof(logBuffer), fmt, args);
_MESSAGE(logBuffer);
```

`_MESSAGE` is printf-style, so the already-formatted text becomes the FORMAT STRING.
Any `%` that survives formatting is then re-parsed as a conversion specifier against an
empty argument list. A log line that formatted to

    Embed: IWP reports a lodged blade: bone='NPC Spine2' depth=100% bladeLen=45

killed the process on the `% b`. It is an invalid-parameter abort rather than an access
violation, so **CrashLogger never produces a crash log** — the game simply vanishes,
which is what made this take several rounds to find.

Fixed here as `_MESSAGE("%s", logBuffer);`. Worth taking upstream: it is latent in every
log line, and any future message of yours containing a percentage would hit it. The same
shape exists at `main.cpp`'s `_MESSAGE(logMsg.c_str())` — harmless today because the
version string has no `%`, but the same trap.
