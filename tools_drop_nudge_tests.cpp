// Console tests for DropNudgeRules.h: where a weapon you let go of is spawned - its pose in your hand, moved clear
// of that hand - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_drop_nudge_tests.cpp /Fe:drop_nudge_tests.exe
//   drop_nudge_tests.exe
// Exit code 0 means every check passed.
//
// The user, 2026-10-02: dropping a weapon downwards "often it spawns in my hands ... it just drops downwards starting
// from my hand", then flies away or wobbles "fighting my hand collision due to it being spawned directly inside of
// it". Raising the sideways nudge (fDropLateralOffset) fixed sideways drops only: a weapon lying across under a
// palm-down hand slides along its own length when nudged sideways and stays in the fist. "I just want it to spawn a
// bit below my palm like the sideways setting."

#include "DropNudgeRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux;

namespace
{
	int s_checks = 0;
	int s_failures = 0;

	const float kToSkyrim = 69.99125f;   // RawHavok::kHavokToSkyrim

	void Check(const bool ok, const char* what)
	{
		++s_checks;
		if (!ok)
		{
			++s_failures;
			std::printf("FAIL: %s\n", what);
		}
	}

	bool Near(const float a, const float b)
	{
		return std::fabs(a - b) < 1e-5f;
	}

	DropNudge::Pos Held()
	{
		DropNudge::Pos p;
		p.x = 10.0f;
		p.y = -4.0f;
		p.z = 1.2f;
		return p;
	}
}

int main()
{
	const DropNudge::Pos at = Held();

	{
		const DropNudge::Pos p = DropNudge::Nudged(at, 1.0f, 0.0f, false, 0.0f, 0.0f, kToSkyrim);
		Check(Near(p.x, at.x) && Near(p.y, at.y) && Near(p.z, at.z), "no nudge: spawned exactly where it was held");
	}

	// The sideways nudge, exactly as before: toward the body's midline.
	{
		const DropNudge::Pos p = DropNudge::Nudged(at, 1.0f, 0.0f, false, 3.0f, 0.0f, kToSkyrim);
		Check(Near(p.x, at.x - 3.0f / kToSkyrim) && Near(p.y, at.y),
			"the right hand's weapon goes 3 units LEFT of it (minus the right vector)");
		Check(Near(p.z, at.z), "and the sideways nudge never moves it up or down");
	}
	{
		const DropNudge::Pos p = DropNudge::Nudged(at, 0.6f, 0.8f, true, 3.0f, 0.0f, kToSkyrim);
		Check(Near(p.x, at.x + 0.6f * 3.0f / kToSkyrim) && Near(p.y, at.y + 0.8f * 3.0f / kToSkyrim),
			"the left hand's weapon goes 3 units RIGHT of it, along the body's right vector");
	}

	// The downward nudge: straight down, below the palm.
	{
		const DropNudge::Pos p = DropNudge::Nudged(at, 1.0f, 0.0f, false, 0.0f, 5.0f, kToSkyrim);
		Check(Near(p.z, at.z - 5.0f / kToSkyrim), "a 5 unit downward nudge spawns it 5 units lower");
		Check(Near(p.x, at.x) && Near(p.y, at.y), "and nowhere else");
	}
	{
		const DropNudge::Pos p = DropNudge::Nudged(at, 0.6f, 0.8f, true, 0.0f, 6.999125f, kToSkyrim);
		Check(Near(p.z, at.z - 0.1f), "7 Skyrim units is a tenth of a metre: Havok is in metres");
		Check(Near(p.x, at.x) && Near(p.y, at.y), "down is world down whichever way the body faces");
	}
	{
		const DropNudge::Pos r = DropNudge::Nudged(at, 1.0f, 0.0f, false, 3.0f, 5.0f, kToSkyrim);
		const DropNudge::Pos l = DropNudge::Nudged(at, 1.0f, 0.0f, true, 3.0f, 5.0f, kToSkyrim);
		Check(Near(r.x, at.x - 3.0f / kToSkyrim) && Near(r.z, at.z - 5.0f / kToSkyrim),
			"both at once, right hand: toward the midline AND below the palm");
		Check(Near(l.x, at.x + 3.0f / kToSkyrim) && Near(l.z, at.z - 5.0f / kToSkyrim),
			"both at once, left hand: toward the midline AND below the palm");
	}
	{
		const DropNudge::Pos p = DropNudge::Nudged(at, 1.0f, 0.0f, false, -2.0f, 0.0f, kToSkyrim);
		Check(Near(p.x, at.x + 2.0f / kToSkyrim), "a negative sideways nudge still pushes it outward, as before");
	}

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
