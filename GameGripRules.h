#pragma once

// Handing the GAME the real grip while Grip Hold's blink shows it released to the other mods
// (M3.0.0-test9, rebuilt in test10). GripBlinkRules.h has what the blink is for.
//
// WHY. The blink's re-press is a real press to everything that reads the controller, the game
// included, and Skyrim VR binds Ready Weapon to the Oculus LEFT grip (Skyrim_VR - Main.bsa,
// controlmapvr.txt: Ready Weapon, Oculus-left column = 0x02, the grip; the right column is 0xff).
// So every left-hand blink put the weapon away - 11 of 11 in the 2026-09-22 log, 0 of 66 on the right.
// test8 answered that by not blinking, which stopped Weapon Throw VR throwing anything at all.
//
// HOW. The game polls the object at BSOpenVR+0x208 through its vtable slot 34 on every frame
// (BSOpenVRControllerDevice, 0x140C5F3E0) and turns changes of ulButtonPressed into button events.
// That object is SkyrimVRTools'; its GetControllerState reaches SKSE's (where MenuGrip's blink hides
// the grip) and then runs its own callbacks - Weapon Throw VR's - on the result. This mod takes slot 34
// once more, outermost, and for a read that returns into SkyrimVR.exe puts back the grip the blink hid.
// Every mod still sees what it saw in test7.
//
// WHAT test9 GOT WRONG. It carried the record of what was hidden from SKSE's callback to the outer hook
// inside ONE call, on one thread, and only when the call returned true. The whole session handed back
// 0 reads. The game's read need not be the call that ran the callback - a layer that answers from state
// it polled earlier hands the game a grip hidden in ANOTHER call - and the game ignores the return value
// (it uses the state whatever the answer), so neither condition was the game's. test10 keeps the record
// per HAND, for the blink and a grace after it, and hands a game read back whenever that record says the
// grip is really closed. Each blink's reads are counted, so the log proves which of those it was.
//
// WHAT test10's LOG PROVED (2026-09-23): "the game did not read that controller through the guard once",
// on every blink, and the game's first read was never seen at all. The reason is in VRIK.dll: it takes over
// the game's one GetControllerState call - `call qword ptr [rax+110h]` at SkyrimVR.exe+0xC5F48E - with
// SKSE's Write6Call (FF 15 -> a trampoline slot holding handleGetControllerStateHook), makes the read
// ITSELF through the same vtable, hands the state to its own input code (vin::updateInputs) and returns it
// to the game. So the read the game acts on returns into VRIK.dll, and test10 only counted SkyrimVR.exe.
// test11 decodes whatever now sits at that call site, follows it to the function that makes the read, and
// counts reads from inside that function as the game's. VRIK's own input code then sees the real grip too,
// as it would with no blink at all; Weapon Throw VR, inside SkyrimVRTools, still sees the blink.
//
// No engine types in here, so tools_game_grip_tests.cpp can drive it outside the game.

#include <cstdint>

namespace SwapDropAndHoldRedux
{
	namespace GameGrip
	{
		// After the blink ends, a read can still arrive with state taken inside it.
		constexpr double kGraceSeconds = 0.15;
		// A record older than this says nothing about the grip now.
		constexpr double kFreshSeconds = 0.25;

		// What MenuGrip's controller callback last saw of ONE hand's controller, before hiding anything.
		struct Record
		{
			long long ticks = 0; // steady-clock ns; 0 = never
			uint32_t device = 0xFFFFFFFFu;
			bool closed = false; // the grip, really
			bool menu = false;   // a MENU was hiding it: that stays hidden from the game as well
			uint64_t pressed = 0; // the real pressed bits among the hidden ones
			uint64_t touched = 0; // the real touched bits among the hidden ones
			float axisX = 0.0f;   // the squeeze axis, really
			float axisY = 0.0f;
		};

		inline Record Take(const long long now, const uint32_t device, const bool closed, const bool menu,
			const uint64_t pressedBefore, const uint64_t touchedBefore, const uint64_t hiddenBits, const float axisX,
			const float axisY)
		{
			Record r;
			r.ticks = now;
			r.device = device;
			r.closed = closed;
			r.menu = menu;
			r.pressed = pressedBefore & hiddenBits;
			r.touched = touchedBefore & hiddenBits;
			r.axisX = axisX;
			r.axisY = axisY;
			return r;
		}

		// Only the game itself is handed the real grip: a call returning into SkyrimVR.exe's image.
		inline bool CallerIsGame(const uintptr_t returnAddress, const uintptr_t imageBase, const uintptr_t imageEnd)
		{
			return imageBase != 0 && returnAddress >= imageBase && returnAddress < imageEnd;
		}

		// ---- test11: the read the game acts on, when a mod has taken over the game's call ----------------

		// The game's own GetControllerState call in the BSOpenVRControllerDevice poll, SkyrimVR.exe 1.4.15:
		// `call qword ptr [rax+110h]` - read from the unpacked exe.
		constexpr uintptr_t kPollCallSiteRva = 0xC5F48E;
		constexpr uint8_t kPollCallOriginal[6] = { 0xFF, 0x90, 0x10, 0x01, 0x00, 0x00 };

		enum class SiteKind
		{
			kOriginal,     // the game's own call: nobody in between
			kIndirectCall, // FF 15 disp32 - call through a slot (SKSE's Write6Call; VRIK)
			kRelativeCall, // E8 rel32 - (SKSE's Write5Call)
			kUnknown,
		};

		struct Site
		{
			SiteKind kind = SiteKind::kUnknown;
			uintptr_t slot = 0;   // kIndirectCall: where the absolute destination is stored
			uintptr_t target = 0; // kRelativeCall: where the call lands (a function or a jump stub)
		};

		inline int32_t Rel32(const uint8_t* at)
		{
			return static_cast<int32_t>(static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
				(static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24));
		}

		// The six bytes at the game's call site, at address `site`.
		inline Site DecodeCallSite(const uint8_t b[6], const uintptr_t site)
		{
			Site s;
			bool original = true;
			for (int i = 0; i < 6; ++i)
			{
				original = original && b[i] == kPollCallOriginal[i];
			}
			if (original)
			{
				s.kind = SiteKind::kOriginal;
			}
			else if (b[0] == 0xFF && b[1] == 0x15)
			{
				s.kind = SiteKind::kIndirectCall;
				s.slot = site + 6 + static_cast<intptr_t>(Rel32(b + 2));
			}
			else if (b[0] == 0xE8)
			{
				s.kind = SiteKind::kRelativeCall;
				s.target = site + 5 + static_cast<intptr_t>(Rel32(b + 1));
			}
			return s;
		}

		struct Stub
		{
			bool isStub = false;
			bool viaSlot = false; // jump through the 8-byte slot at `slot`, else straight to `target`
			uintptr_t slot = 0;
			uintptr_t target = 0;
		};

		// Where a call can land before the real function: SKSE's Write5Call stub (FF 25 00000000 + an
		// absolute address), a jump through a slot (FF 25 disp32), or a relative jump (E9 rel32).
		inline Stub DecodeJumpStub(const uint8_t b[6], const uintptr_t at)
		{
			Stub st;
			if (b[0] == 0xFF && b[1] == 0x25)
			{
				st.isStub = true;
				st.viaSlot = true;
				st.slot = at + 6 + static_cast<intptr_t>(Rel32(b + 2));
			}
			else if (b[0] == 0xE9)
			{
				st.isStub = true;
				st.target = at + 5 + static_cast<intptr_t>(Rel32(b + 1));
			}
			return st;
		}

		// A read is the game's when it returns into SkyrimVR.exe, or into the function that has taken over the
		// game's call ([proxyBegin, proxyEnd), 0 when nobody has).
		inline bool IsGameRead(const uintptr_t returnAddress, const uintptr_t imageBase, const uintptr_t imageEnd,
			const uintptr_t proxyBegin, const uintptr_t proxyEnd)
		{
			return CallerIsGame(returnAddress, imageBase, imageEnd) ||
				(proxyBegin != 0 && returnAddress >= proxyBegin && returnAddress < proxyEnd);
		}

		// The blink, and its grace.
		inline bool InWindow(const long long now, const long long blinkStart, const long long blinkEnd,
			const long long graceTicks)
		{
			return blinkEnd != 0 && now >= blinkStart && now <= blinkEnd + graceTicks;
		}

		// Whether this read gets the real grip put back.
		inline bool HandsBack(const bool callerIsGame, const uint32_t device, const long long now,
			const long long blinkStart, const long long blinkEnd, const long long graceTicks, const long long freshTicks,
			const Record& rec)
		{
			if (!callerIsGame || !InWindow(now, blinkStart, blinkEnd, graceTicks))
			{
				return false;
			}
			if (rec.ticks == 0 || rec.device != device || rec.ticks < blinkStart || now - rec.ticks > freshTicks)
			{
				return false; // not this controller, from before this blink, or too old to say
			}
			return rec.closed && !rec.menu;
		}

		// Puts back the real grip: it only ever SETS the hidden bits, so a bit somebody else cleared
		// elsewhere stays cleared.
		inline void Apply(const Record& rec, uint64_t& pressed, uint64_t& touched, float& axisX, float& axisY)
		{
			pressed |= rec.pressed;
			touched |= rec.touched;
			axisX = rec.axisX;
			axisY = rec.axisY;
		}

		// For the log: this read arrived with the real grip hidden - the blink reached the game.
		inline bool ReachedGame(const Record& rec, const uint64_t pressedNow, const uint64_t touchedNow)
		{
			return rec.closed && !rec.menu &&
				((pressedNow & rec.pressed) != rec.pressed || (touchedNow & rec.touched) != rec.touched);
		}

		enum class Verdict
		{
			kNoReads,       // the game did not read that controller through the hook during the blink
			kNeverReached,  // it did, and the blink never reached those reads
			kAllHandedBack, // every read the blink reached was handed the real grip
			kSomeMissed,    // some were not
		};

		inline Verdict Judge(const long long reads, const long long reached, const long long handedBack)
		{
			if (reads <= 0)
			{
				return Verdict::kNoReads;
			}
			if (reached <= 0)
			{
				return Verdict::kNeverReached;
			}
			return handedBack >= reached ? Verdict::kAllHandedBack : Verdict::kSomeMissed;
		}

		// ---- test13: the game is shown the grip exactly as it was before the pickup, until the hand opens ----
		//
		// WHAT test12's LOG SHOWED. 5 LEFT pickups (of 30) put the weapon away 0.27-0.30s later with the grip
		// still closed - all 4 whose weapon was already drawn within 0.10s, none of the 25 still drawing. The
		// game's VR Ready Weapon fires once its button has been HELD 0.25s after a press (ReadyWeaponHandler,
		// 0x1407316C0; the threshold at 0x141EB3540 is 0.25f); a draw in progress absorbs it. HIGGS hides a
		// grip it grabs with from everyone after it - the game included (Block, hand.cpp 4240-4260) - and SKSE
		// runs its callbacks by sort order, so MenuGrip's blink (0) runs BEFORE HIGGS (66): HIGGS reads the
		// blink as a release and stops hiding the grip. test11/12 then gave the game the REAL grip at the
		// pickup - a press it had never seen - and 0.25s later Ready Weapon put a drawn weapon away.
		//
		// So for the game the pickup must change NOTHING: from the blink on, its reads of that grip are what it
		// was shown on its last read BEFORE the blink - hidden, if HIGGS was hiding it; held, if it was seeing it
		// held - until the hand really opens. That is exactly what the game would see with no blink at all (HIGGS
		// would still be hiding it). Every mod still sees what it saw before: this changes the game's reads only.
		struct GameView
		{
			bool haveLast = false;     // the game has read this controller before
			long long lastReadTicks = 0;
			uint64_t lastPressed = 0;  // the hidden bits the game was shown on its last read
			uint64_t lastTouched = 0;
			bool latched = false;      // showing the game the kept view
			long long latchBlinkStart = 0;
			uint64_t latchPressed = 0;
			uint64_t latchTouched = 0;
		};

		enum class ViewStep
		{
			kPassThrough, // nothing changed
			kKept,        // shown the view kept from before the pickup
			kHandedBack,  // no earlier read to keep: shown the real grip, as test11 did
		};

		// One game read of this hand's controller: `rec` is the REAL grip (MenuGrip's record, before anything
		// hid it), pressed/touched/axis the state the game is about to be given.
		inline ViewStep GameViewRead(GameView& view, const uint32_t device, const long long now, const long long blinkStart,
			const long long blinkEnd, const long long graceTicks, const long long freshTicks, const Record& rec,
			const uint64_t hiddenBits, uint64_t& pressed, uint64_t& touched, float& axisX, float& axisY)
		{
			// A blink this view has not seen yet: keep what the game was shown on its last read before it began -
			// however late this first read after it is (a hitch can skip the whole window).
			if (blinkEnd != 0 && blinkStart != view.latchBlinkStart && now >= blinkStart &&
				(!view.haveLast || view.lastReadTicks < blinkStart))
			{
				view.latchBlinkStart = blinkStart;
				if (view.haveLast)
				{
					view.latched = true;
					view.latchPressed = view.lastPressed;
					view.latchTouched = view.lastTouched;
				}
			}

			ViewStep step = ViewStep::kPassThrough;
			if (view.latched)
			{
				const bool fresh = rec.ticks != 0 && rec.device == device && now - rec.ticks <= freshTicks;
				if (!fresh || !rec.closed)
				{
					view.latched = false; // the hand opened, or nobody can say it is still closed: the game sees it as it is
				}
				else if (!rec.menu)       // a MENU hiding it stays hidden - the kept view waits for the menu to finish
				{
					pressed = (pressed & ~hiddenBits) | view.latchPressed;
					touched = (touched & ~hiddenBits) | view.latchTouched;
					axisX = rec.axisX;
					axisY = rec.axisY;
					step = ViewStep::kKept;
				}
			}
			else if (HandsBack(true, device, now, blinkStart, blinkEnd, graceTicks, freshTicks, rec))
			{
				Apply(rec, pressed, touched, axisX, axisY);
				step = ViewStep::kHandedBack;
			}

			view.haveLast = true;
			view.lastReadTicks = now;
			view.lastPressed = pressed & hiddenBits;
			view.lastTouched = touched & hiddenBits;
			return step;
		}
	}
}
