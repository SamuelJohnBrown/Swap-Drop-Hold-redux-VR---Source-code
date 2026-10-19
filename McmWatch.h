#pragma once

// The in-game menu fills itself in during its first session: MCM-GUIDE.md link 5, [mcmfix].
//
// A modlist's authors reported this mod, with three others, listed in the in-game menu with every page blank and
// its internal name as the title, on the first session after installing; one save and reload fixed it. MCM Helper
// fills a menu's title and page list in exactly one place, and once SkyUI lists the menu nothing refills them for
// the rest of the session. So every 2 s in game the plugin reads the menu's state through the Papyrus VM, and when
// SkyUI lists it with no pages it has MCM Helper read config.json again. McmSelfRepair.h decides WHEN; this does
// the reading and the writing.
//
// The first port of the repair to the classic sksevr SDK: every other mod carrying it uses CommonLib. McmWatch.cpp
// says, call by call, what was read in the unpacked SkyrimVR.exe before trusting it.

namespace SwapDropAndHoldRedux
{
	// A HIGGS pre-physics callback of its own, because Grip Hold's step returns at once in Legacy mode and the menu
	// can go blank in any mode. Call once HIGGS's interface exists.
	void RegisterMcmWatch();

	// After a load that succeeded, and at a new game. The session's refill count carries over; the rest restarts.
	void McmWatchArm(const char* why);

	// Before a load: it throws away the script objects the watch reads.
	void McmWatchDisarm();
}
