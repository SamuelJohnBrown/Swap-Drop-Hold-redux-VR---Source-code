// Console tests for GameGripRules.h: handing the GAME the real grip while Grip Hold's blink shows
// it released to the other mods - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_game_grip_tests.cpp /Fe:game_grip_tests.exe
//   game_grip_tests.exe
// Exit code 0 means every check passed.

#include "GameGripRules.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace SwapDropAndHoldRedux;

namespace
{
	int s_checks = 0;
	int s_failures = 0;

	void Check(const bool ok, const char* what)
	{
		++s_checks;
		if (!ok)
		{
			++s_failures;
			std::printf("FAIL: %s\n", what);
		}
	}

	constexpr uint64_t kGrip = 1ull << 2;
	constexpr uint64_t kSqueeze = 1ull << 34;
	constexpr uint64_t kTrigger = 1ull << 33;
	constexpr uint64_t kA = 1ull << 7;
	constexpr uint64_t kHidden = kGrip | kSqueeze; // exactly what MenuGrip's callback hides
	constexpr uint32_t kLeft = 3;
	constexpr uint32_t kRight = 4;

	constexpr long long kMs = 1000000LL; // steady-clock nanoseconds
	constexpr long long kStart = 5000 * kMs;
	constexpr long long kEnd = kStart + 100 * kMs; // the 0.10s blink
	constexpr long long kGrace = 150 * kMs;
	constexpr long long kFresh = 250 * kMs;

	GameGrip::Record Closed(const long long at, const uint32_t device = kLeft)
	{
		return GameGrip::Take(at, device, true, false, kGrip | kSqueeze | kTrigger, kGrip, kHidden, 0.93f, 0.0f);
	}

	void TestTakeKeepsOnlyWhatTheBlinkHides()
	{
		const GameGrip::Record r = GameGrip::Take(kStart, kLeft, true, false, kGrip | kSqueeze | kTrigger | kA, kGrip | kTrigger, kHidden, 0.93f, 0.1f);
		Check(r.ticks == kStart && r.device == kLeft, "a record says when and which controller");
		Check(r.closed && !r.menu, "whether the grip was really closed, and whether a menu was hiding it");
		Check(r.pressed == (kGrip | kSqueeze), "the real pressed bits among the hidden ones, nothing else");
		Check(r.touched == kGrip, "and the touched ones");
		Check(r.axisX == 0.93f && r.axisY == 0.1f, "and the squeeze axis as it really was");
	}

	void TestOnlyTheGameIsHandedIt()
	{
		const uintptr_t base = 0x140000000ull;
		const uintptr_t end = 0x143000000ull;
		Check(GameGrip::CallerIsGame(0x140A12345ull, base, end), "a call from inside SkyrimVR.exe is the game");
		Check(GameGrip::CallerIsGame(base, base, end), "its first byte counts");
		Check(!GameGrip::CallerIsGame(end, base, end), "the end is outside");
		Check(!GameGrip::CallerIsGame(0x7FFE10114370ull, base, end), "a call from a mod's DLL is not the game - it still sees the blink");
		Check(!GameGrip::CallerIsGame(0x140A12345ull, 0, 0), "no image known: nobody is the game");
	}

	void TestTheWindowIsTheBlinkAndItsGrace()
	{
		Check(!GameGrip::InWindow(kStart - 1, kStart, kEnd, kGrace), "not before the blink");
		Check(GameGrip::InWindow(kStart, kStart, kEnd, kGrace), "from its first tick");
		Check(GameGrip::InWindow(kEnd + kGrace, kStart, kEnd, kGrace), "through the grace");
		Check(!GameGrip::InWindow(kEnd + kGrace + 1, kStart, kEnd, kGrace), "and not after it");
		Check(!GameGrip::InWindow(kStart, 0, 0, kGrace), "no blink ever started: no window");
	}

	void TestTheGameGetsTheRealGripDuringTheBlink()
	{
		const GameGrip::Record r = Closed(kStart + 2 * kMs);
		Check(GameGrip::HandsBack(true, kLeft, kStart + 3 * kMs, kStart, kEnd, kGrace, kFresh, r),
			"the game's read during the blink, grip really closed: handed back");
		Check(GameGrip::HandsBack(true, kLeft, kEnd + 100 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kEnd + 90 * kMs)),
			"and in the grace after it");
	}

	void TestTheCachedReadV1Missed()
	{
		// M3.0.0-test9 carried the record inside ONE call, on one thread, and handed back 0 times in a whole
		// session. The game's read need not be the call that ran SKSE's callback: a layer that answers from
		// state it polled earlier hands the game a grip the blink hid in ANOTHER call. v2 keeps the record per
		// hand, so the read is handed back whichever call hid it.
		const GameGrip::Record fromAnEarlierCall = Closed(kStart + 1 * kMs);
		Check(GameGrip::HandsBack(true, kLeft, kStart + 9 * kMs, kStart, kEnd, kGrace, kFresh, fromAnEarlierCall),
			"a read served from an earlier call's state is still handed back");
	}

	void TestWhoAndWhatIsNotHandedBack()
	{
		const GameGrip::Record r = Closed(kStart + 2 * kMs);
		const long long now = kStart + 3 * kMs;
		Check(!GameGrip::HandsBack(false, kLeft, now, kStart, kEnd, kGrace, kFresh, r), "a mod's read keeps the blink - Weapon Throw VR needs it");
		Check(!GameGrip::HandsBack(true, kRight, now, kStart, kEnd, kGrace, kFresh, r), "the other controller is not this record's");
		Check(!GameGrip::HandsBack(true, kLeft, kStart - kMs, kStart, kEnd, kGrace, kFresh, r), "not before the blink");
		Check(!GameGrip::HandsBack(true, kLeft, kEnd + kGrace + kMs, kStart, kEnd, kGrace, kFresh, r), "not after its grace");
		Check(!GameGrip::HandsBack(true, kLeft, now, kStart, kEnd, kGrace, kFresh, Closed(kStart - 30 * kMs)),
			"a record from before this blink says nothing about it");
		Check(GameGrip::HandsBack(true, kLeft, kEnd + kGrace, kStart, kEnd, kGrace, kFresh, Closed(kStart + kMs)),
			"a record 0.249s old at the end of the grace is still fresh");
		// A blink held back for longer (VR Climbing), read 0.251s after the last record.
		const long long longEnd = kStart + 60 * kMs + kFresh + kMs;
		Check(!GameGrip::HandsBack(true, kLeft, longEnd, kStart, longEnd, kGrace, kFresh, Closed(kStart + 60 * kMs)),
			"a record older than 0.25s is not trusted");
		const GameGrip::Record open = GameGrip::Take(kStart + 2 * kMs, kLeft, false, false, 0, 0, kHidden, 0.0f, 0.0f);
		Check(!GameGrip::HandsBack(true, kLeft, now, kStart, kEnd, kGrace, kFresh, open), "a hand that really opened is not closed for the game");
		const GameGrip::Record menu = GameGrip::Take(kStart + 2 * kMs, kLeft, true, true, kGrip, kGrip, kHidden, 1.0f, 0.0f);
		Check(!GameGrip::HandsBack(true, kLeft, now, kStart, kEnd, kGrace, kFresh, menu),
			"a grip a MENU is hiding stays hidden from the game too - or the menu would close on it");
		Check(!GameGrip::HandsBack(true, kLeft, now, kStart, kEnd, kGrace, kFresh, GameGrip::Record{}), "no record at all: nothing");
	}

	void TestApplyPutsBackExactlyTheRealGrip()
	{
		const GameGrip::Record r = GameGrip::Take(kStart, kLeft, true, false, kGrip | kSqueeze | kTrigger, kGrip, kHidden, 0.93f, 0.1f);
		// What reaches the outermost layer: the blink cleared the grip, and some other mod happened to clear
		// the trigger too.
		uint64_t pressed = kA;
		uint64_t touched = 0;
		float x = 0.0f, y = 0.0f;
		GameGrip::Apply(r, pressed, touched, x, y);
		Check((pressed & kHidden) == (kGrip | kSqueeze) && (touched & kGrip) != 0, "the grip is pressed again for the game");
		Check((pressed & kTrigger) == 0, "a bit somebody else cleared outside the grip stays cleared");
		Check((pressed & kA) != 0, "and everything else passes through");
		Check(x == 0.93f && y == 0.1f, "the squeeze axis is the real one");
	}

	void TestTheLogCanTellWhatHappened()
	{
		const GameGrip::Record r = Closed(kStart);
		Check(GameGrip::ReachedGame(r, kA, 0), "a read arriving with the real grip hidden: the blink reached the game");
		Check(!GameGrip::ReachedGame(r, kA | kGrip | kSqueeze, kGrip), "one arriving with it pressed: it did not");
		const GameGrip::Record open = GameGrip::Take(kStart, kLeft, false, false, 0, 0, kHidden, 0.0f, 0.0f);
		Check(!GameGrip::ReachedGame(open, 0, 0), "an open hand hides nothing");
		const GameGrip::Record menu = GameGrip::Take(kStart, kLeft, true, true, kGrip, kGrip, kHidden, 1.0f, 0.0f);
		Check(!GameGrip::ReachedGame(menu, 0, 0), "a menu's hiding is not the blink's");
		const GameGrip::Record touchOnly = GameGrip::Take(kStart, kLeft, true, false, 0, kGrip, kHidden, 0.4f, 0.0f);
		Check(GameGrip::ReachedGame(touchOnly, 0, 0), "a grip closed by touch alone counts too (the watched-bits preset)");

		Check(GameGrip::Judge(0, 0, 0) == GameGrip::Verdict::kNoReads, "no game read of that controller during the blink");
		Check(GameGrip::Judge(11, 0, 0) == GameGrip::Verdict::kNeverReached, "read, but never with the grip hidden");
		Check(GameGrip::Judge(11, 10, 10) == GameGrip::Verdict::kAllHandedBack, "every read the blink reached was handed back");
		Check(GameGrip::Judge(11, 10, 7) == GameGrip::Verdict::kSomeMissed, "some were not");
	}

	// The whole left-hand pickup, one game read per 11ms frame: the grip held from before the pickup to
	// after the blink. Counts the press edges the game's poll would turn into Ready Weapon.
	int PressEdgesTheGameSees(const bool guarded)
	{
		bool previous = true; // squeezing to pick it up
		int edges = 0;
		for (long long t = kStart - 50 * kMs; t < kEnd + 300 * kMs; t += 11 * kMs)
		{
			const bool blink = t >= kStart && t < kEnd;
			const GameGrip::Record r = Closed(t);   // SKSE's callback, this frame, before it hides anything
			uint64_t pressed = blink ? kA : (kA | kGrip | kSqueeze);
			uint64_t touched = blink ? 0 : kGrip;
			float x = blink ? 0.0f : 0.93f, y = 0.0f;
			if (guarded && GameGrip::HandsBack(true, kLeft, t + kMs, kStart, kEnd, kGrace, kFresh, r))
			{
				GameGrip::Apply(r, pressed, touched, x, y);
			}
			const bool now = (pressed & kGrip) != 0;
			if (now && !previous)
			{
				++edges;
			}
			previous = now;
		}
		return edges;
	}

	void TestAPickupNoLongerPutsTheLeftWeaponAway()
	{
		Check(PressEdgesTheGameSees(false) == 1, "unguarded: the blink's re-press is one Ready Weapon press (the 11 of 11 sheathes)");
		Check(PressEdgesTheGameSees(true) == 0, "guarded: the game sees the grip held throughout - no press, nothing put away");
	}

	// ---- M3.0.0-test11: the read the game acts on can be made by a mod that took over its call site ----

	// The game's own call, `call qword ptr [rax+110h]` at SkyrimVR.exe+0xC5F48E, as the unpacked exe has it.
	constexpr uint8_t kOriginal[6] = { 0xFF, 0x90, 0x10, 0x01, 0x00, 0x00 };
	constexpr uintptr_t kExe = 0x7FF613980000ull; // the test10 log's image
	constexpr uintptr_t kExeEnd = 0x7FF6172D9000ull;
	constexpr uintptr_t kSite = kExe + 0xC5F48Eull;
	// VRIK.dll's handleGetControllerStateHook in the installed build: 0x2C4D0-0x2C522, and its vtable call
	// returns to +0x2C4FC.
	constexpr uintptr_t kVrik = 0x7FFE60000000ull;
	constexpr uintptr_t kVrikHookBegin = kVrik + 0x2C4D0ull;
	constexpr uintptr_t kVrikHookEnd = kVrik + 0x2C522ull;
	constexpr uintptr_t kVrikReturn = kVrik + 0x2C4FCull;

	void Put32(uint8_t* at, const int32_t v)
	{
		const uint32_t u = static_cast<uint32_t>(v);
		at[0] = static_cast<uint8_t>(u);
		at[1] = static_cast<uint8_t>(u >> 8);
		at[2] = static_cast<uint8_t>(u >> 16);
		at[3] = static_cast<uint8_t>(u >> 24);
	}

	void TestTheGamesOwnCallIsRecognised()
	{
		const GameGrip::Site s = GameGrip::DecodeCallSite(kOriginal, kSite);
		Check(s.kind == GameGrip::SiteKind::kOriginal, "the game's own call: nobody has taken it over");
	}

	void TestVriksTakeOverIsFollowed()
	{
		// SKSE's Write6Call, as VRIK uses it: FF 15 disp32 = call qword ptr [rip+disp32], the slot holding
		// VRIK's function. The trampoline sits within 2 GB either side of the image.
		uint8_t b[6] = { 0xFF, 0x15 };
		Put32(b + 2, 0x0A3B1C40);
		GameGrip::Site s = GameGrip::DecodeCallSite(b, kSite);
		Check(s.kind == GameGrip::SiteKind::kIndirectCall, "FF 15: an indirect call through a slot (SKSE's Write6Call - VRIK)");
		Check(s.slot == kSite + 6 + 0x0A3B1C40ull, "the slot is measured from the NEXT instruction");
		Put32(b + 2, -0x00123456);
		s = GameGrip::DecodeCallSite(b, kSite);
		Check(s.slot == kSite + 6 - 0x00123456ull, "a trampoline below the image: the displacement is signed");

		uint8_t e8[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
		Put32(e8 + 1, 0x01020304);
		s = GameGrip::DecodeCallSite(e8, kSite);
		Check(s.kind == GameGrip::SiteKind::kRelativeCall && s.target == kSite + 5 + 0x01020304ull,
			"E8 rel32 (SKSE's Write5Call): a direct call, measured from the next instruction");

		const uint8_t junk[6] = { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
		Check(GameGrip::DecodeCallSite(junk, kSite).kind == GameGrip::SiteKind::kUnknown, "anything else: not understood, not guessed at");
		const uint8_t otherSlot[6] = { 0xFF, 0x90, 0x18, 0x01, 0x00, 0x00 };
		Check(GameGrip::DecodeCallSite(otherSlot, kSite).kind == GameGrip::SiteKind::kUnknown,
			"a call through a DIFFERENT vtable slot is not the game's GetControllerState");
	}

	void TestJumpStubsAreFollowed()
	{
		const uintptr_t at = 0x7FF613900000ull;
		const uint8_t absolute[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
		GameGrip::Stub st = GameGrip::DecodeJumpStub(absolute, at);
		Check(st.isStub && st.viaSlot && st.slot == at + 6, "FF 25 00000000: jump to the 8 bytes right after it (Write5Call's stub)");
		uint8_t ripSlot[6] = { 0xFF, 0x25 };
		Put32(ripSlot + 2, 0x40);
		st = GameGrip::DecodeJumpStub(ripSlot, at);
		Check(st.isStub && st.viaSlot && st.slot == at + 6 + 0x40, "FF 25 disp32: jump through a slot further on");
		uint8_t e9[6] = { 0xE9 };
		Put32(e9 + 1, -0x20);
		st = GameGrip::DecodeJumpStub(e9, at);
		Check(st.isStub && !st.viaSlot && st.target == at + 5 - 0x20, "E9 rel32: a relative jump (an incremental-link thunk)");
		const uint8_t prologue[6] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57 }; // VRIK's hook starts mov [rsp+8],rbx
		Check(!GameGrip::DecodeJumpStub(prologue, at).isStub, "a real function start is where the chain ends");
	}

	void TestTheReadTest10MissedIsTheGames()
	{
		// test10's log: "the game did not read that controller through the guard once", on every blink - because
		// the read the game acts on returns into VRIK.dll, not SkyrimVR.exe.
		Check(!GameGrip::CallerIsGame(kVrikReturn, kExe, kExeEnd), "test10's rule: VRIK's read is not the game's (why it handed back nothing)");
		Check(GameGrip::IsGameRead(kVrikReturn, kExe, kExeEnd, kVrikHookBegin, kVrikHookEnd),
			"test11's rule: a read from the function that took over the game's call IS the game's read");
		Check(GameGrip::IsGameRead(kSite + 6, kExe, kExeEnd, kVrikHookBegin, kVrikHookEnd),
			"the game's own read still counts, taken over or not");
		Check(GameGrip::IsGameRead(kSite + 6, kExe, kExeEnd, 0, 0), "and with nobody in between");
		Check(!GameGrip::IsGameRead(kVrikHookEnd, kExe, kExeEnd, kVrikHookBegin, kVrikHookEnd), "the function's end is outside it");
		Check(!GameGrip::IsGameRead(kVrik + 0x40000ull, kExe, kExeEnd, kVrikHookBegin, kVrikHookEnd),
			"VRIK's OTHER reads are not the game's");
		Check(!GameGrip::IsGameRead(0x7FFE588872D0ull, kExe, kExeEnd, kVrikHookBegin, kVrikHookEnd),
			"another mod's read is not the game's - Weapon Throw VR keeps its blink");
		Check(!GameGrip::IsGameRead(kVrikReturn, kExe, kExeEnd, 0, 0), "no take-over found: VRIK's read is not counted");
	}

	// ---- M3.0.0-test13: the game is shown the grip exactly as before the pickup, until the hand opens ----
	//
	// The test12 log: 5 LEFT pickups (of 30) were put away 0.27-0.30s later with the grip still closed - every
	// one of the 4 whose weapon was already drawn within 0.10s, none of the 25 that still had to draw. The
	// game's VR Ready Weapon fires once the button has been held 0.25s after a press (0x141EB3540 = 0.25f,
	// ReadyWeaponHandler 0x1407316C0). HIGGS hides a grip it grabs with from the game (Block, hand.cpp
	// 4240-4260); the blink, which SKSE runs BEFORE HIGGS (sort 0 < 66), shows HIGGS a release, so HIGGS stops
	// hiding it - and test11/12 then handed the game the REAL grip: a press it had never seen.

	struct Frame
	{
		uint64_t pressed;
		uint64_t touched;
		GameGrip::Record rec; // what MenuGrip's callback saw, before anything hid it
	};

	// One pickup, frame by frame (11ms): the hand closes at 0.30s (HIGGS hides that press from the game), the
	// pickup and blink at 0.50s, HIGGS no longer hiding it after the blink, the hand opens at 3.00s.
	// `higgsHid`: false = the game already saw the press before the pickup (HIGGS was not hiding it).
	Frame ChainAt(const long long t, const bool higgsHid)
	{
		const long long press = kStart - 200 * kMs;
		const long long release = kStart + 2500 * kMs;
		const bool held = t >= press && t < release;
		const bool blink = t >= kStart && t < kEnd;
		Frame f{};
		f.rec = GameGrip::Take(t, kLeft, held, false, held ? (kGrip | kSqueeze) : 0, held ? kGrip : 0, kHidden, held ? 0.93f : 0.0f, 0.0f);
		if (!held)
		{
			f.pressed = 0;
			f.touched = 0;
		}
		else if (blink)
		{
			f.pressed = 0; // the blink hides it
			f.touched = 0;
		}
		else if (t < kStart && higgsHid)
		{
			f.pressed = 0; // HIGGS hides the grip it grabbed with (pressed only, not touch, on Oculus)
			f.touched = kGrip;
		}
		else
		{
			f.pressed = kGrip | kSqueeze; // after the blink HIGGS no longer hides it
			f.touched = kGrip;
		}
		return f;
	}

	struct GameSaw
	{
		int pressEdges = 0;    // presses the game saw from the pickup on
		int readyWeapon = 0;   // presses held 0.25s - the game's VR Ready Weapon
		bool heldAtEnd = false;
	};

	GameSaw RunGame(const bool guardTest13, const bool higgsHid, const long long until = kStart + 2600 * kMs)
	{
		GameGrip::GameView view;
		GameSaw saw;
		bool previous = false;
		long long pressedSince = -1;
		bool fired = false;
		for (long long t = kStart - 400 * kMs; t < until; t += 11 * kMs)
		{
			Frame f = ChainAt(t, higgsHid);
			float x = 0.0f, y = 0.0f;
			if (guardTest13)
			{
				GameGrip::GameViewRead(view, kLeft, t, kStart, kEnd, kGrace, kFresh, f.rec, kHidden, f.pressed, f.touched, x, y);
			}
			else if (GameGrip::HandsBack(true, kLeft, t, kStart, kEnd, kGrace, kFresh, f.rec))
			{
				GameGrip::Apply(f.rec, f.pressed, f.touched, x, y); // test11/12
			}
			const bool now = (f.pressed & kGrip) != 0;
			if (now && !previous)
			{
				pressedSince = t;
				fired = false;
				if (t >= kStart)
				{
					++saw.pressEdges;
				}
			}
			if (now && !fired && pressedSince >= 0 && t - pressedSince >= 250 * kMs && pressedSince >= kStart)
			{
				fired = true;
				++saw.readyWeapon;
			}
			if (!now)
			{
				pressedSince = -1;
			}
			previous = now;
			saw.heldAtEnd = now;
		}
		return saw;
	}

	void TestTest12GaveTheGameAPressAtThePickup()
	{
		const GameSaw old = RunGame(false, true);
		Check(old.pressEdges == 1, "test12: the game saw a NEW press at the pickup, one HIGGS had hidden from it");
		Check(old.readyWeapon == 1, "held 0.25s: one Ready Weapon - the weapon put away (the 5 in the test12 log)");
	}

	void TestTheGameSeesNoPressFromAPickup()
	{
		const GameSaw kept = RunGame(true, true);
		Check(kept.pressEdges == 0, "test13: the game is shown the grip as before the pickup - no press at all");
		Check(kept.readyWeapon == 0, "so no Ready Weapon, drawn weapon or not");
		const GameSaw heldOn = RunGame(true, true, kStart + 2000 * kMs);
		Check(!heldOn.heldAtEnd, "still held 2s later, the game is still shown it the way HIGGS hid it");
	}

	void TestAGripTheGameAlreadySawStaysSeen()
	{
		const GameSaw kept = RunGame(true, false);
		Check(kept.pressEdges == 0, "HIGGS was not hiding it: the game keeps seeing it held - no new press");
		const GameSaw heldOn = RunGame(true, false, kStart + 2000 * kMs);
		Check(heldOn.heldAtEnd, "and still sees it held");
	}

	void TestTheHandOpeningEndsIt()
	{
		GameGrip::GameView view;
		// Seen before the pickup: HIGGS hiding the press.
		uint64_t p = 0, tch = kGrip;
		float x = 0.0f, y = 0.0f;
		GameGrip::GameViewRead(view, kLeft, kStart - 11 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart - 11 * kMs), kHidden, p, tch, x, y);
		// During the hold, long after the blink: kept.
		p = kGrip | kSqueeze;
		tch = kGrip;
		Check(GameGrip::GameViewRead(view, kLeft, kStart + 900 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart + 900 * kMs), kHidden, p, tch, x, y) ==
				GameGrip::ViewStep::kKept && (p & kGrip) == 0,
			"0.9s into the hold the game is still shown the grip as HIGGS hid it");
		// The hand opens: the game sees the chain as it is, and keeps doing so.
		const GameGrip::Record open = GameGrip::Take(kStart + 1000 * kMs, kLeft, false, false, 0, 0, kHidden, 0.0f, 0.0f);
		p = 0;
		tch = 0;
		Check(GameGrip::GameViewRead(view, kLeft, kStart + 1000 * kMs, kStart, kEnd, kGrace, kFresh, open, kHidden, p, tch, x, y) ==
				GameGrip::ViewStep::kPassThrough && !view.latched,
			"the hand opens: the game sees it as it is again");
		// A fresh press later is the player's own, and the game sees it.
		p = kGrip;
		tch = kGrip;
		GameGrip::GameViewRead(view, kLeft, kStart + 1500 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart + 1500 * kMs), kHidden, p, tch, x, y);
		Check((p & kGrip) != 0, "a new press after that is the player's, untouched");
	}

	void TestAMenuStillHidesIt()
	{
		GameGrip::GameView view;
		uint64_t p = kGrip | kSqueeze, tch = kGrip;
		float x = 0.0f, y = 0.0f;
		GameGrip::GameViewRead(view, kLeft, kStart - 11 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart - 11 * kMs), kHidden, p, tch, x, y);
		const GameGrip::Record menu = GameGrip::Take(kStart + 400 * kMs, kLeft, true, true, kGrip, kGrip, kHidden, 1.0f, 0.0f);
		p = 0;
		tch = 0; // the menu hid it
		GameGrip::GameViewRead(view, kLeft, kStart + 400 * kMs, kStart, kEnd, kGrace, kFresh, menu, kHidden, p, tch, x, y);
		Check(p == 0 && tch == 0, "a MENU hiding the grip stays hidden from the game - or the menu would close on it");
		Check(view.latched, "and the kept view waits for the menu to finish");
	}

	void TestNoEarlierReadFallsBackToTheRealGrip()
	{
		GameGrip::GameView view; // the game has not read this controller before the blink
		uint64_t p = 0, tch = 0;
		float x = 0.0f, y = 0.0f;
		Check(GameGrip::GameViewRead(view, kLeft, kStart + 3 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart + 2 * kMs), kHidden, p, tch, x, y) ==
				GameGrip::ViewStep::kHandedBack && (p & kGrip) != 0,
			"nothing seen before the blink to keep: the real grip, as test11 did");
	}

	void TestAStaleRecordLetsGo()
	{
		GameGrip::GameView view;
		uint64_t p = 0, tch = kGrip;
		float x = 0.0f, y = 0.0f;
		GameGrip::GameViewRead(view, kLeft, kStart - 11 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart - 11 * kMs), kHidden, p, tch, x, y);
		GameGrip::GameViewRead(view, kLeft, kStart + 5 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart + 4 * kMs), kHidden, p, tch, x, y);
		Check(view.latched, "kept from the blink on");
		p = kGrip;
		tch = kGrip;
		GameGrip::GameViewRead(view, kLeft, kStart + 2000 * kMs, kStart, kEnd, kGrace, kFresh, Closed(kStart + 5 * kMs), kHidden, p, tch, x, y);
		Check(!view.latched && (p & kGrip) != 0, "no fresh word on the real grip for 2s: nobody can say it is still held - let go");
	}
}

int main()
{
	TestTakeKeepsOnlyWhatTheBlinkHides();
	TestOnlyTheGameIsHandedIt();
	TestTheWindowIsTheBlinkAndItsGrace();
	TestTheGameGetsTheRealGripDuringTheBlink();
	TestTheCachedReadV1Missed();
	TestWhoAndWhatIsNotHandedBack();
	TestApplyPutsBackExactlyTheRealGrip();
	TestTheLogCanTellWhatHappened();
	TestAPickupNoLongerPutsTheLeftWeaponAway();
	TestTheGamesOwnCallIsRecognised();
	TestVriksTakeOverIsFollowed();
	TestJumpStubsAreFollowed();
	TestTheReadTest10MissedIsTheGames();
	TestTest12GaveTheGameAPressAtThePickup();
	TestTheGameSeesNoPressFromAPickup();
	TestAGripTheGameAlreadySawStaysSeen();
	TestTheHandOpeningEndsIt();
	TestAMenuStillHidesIt();
	TestNoEarlierReadFallsBackToTheRealGrip();
	TestAStaleRecordLetsGo();

	std::printf("game_grip: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
