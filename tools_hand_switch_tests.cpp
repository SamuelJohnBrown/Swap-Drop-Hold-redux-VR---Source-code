// Console tests for HandSwitchRules.h: when letting go with one hand moves the weapon to the other, and
// which way round the other hand has it - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_hand_switch_tests.cpp /Fe:hand_switch_tests.exe
//   hand_switch_tests.exe
// Exit code 0 means every check passed.

#include "HandSwitchRules.h"

#include <cmath>
#include <cstdio>

using namespace SwapDropAndHoldRedux;
namespace GP = SwapDropAndHoldRedux::GrabPose;

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

	HandSwitch::Facts Ready()
	{
		HandSwitch::Facts f;
		f.twoHandedWeapon = false;
		f.otherHandOnWeapon = true;
		f.otherGripClosed = true;
		f.otherHandFree = true;
		return f;
	}

	// A turn of `degrees` about world axis 0 (X), 1 (Y) or 2 (Z).
	GP::Mat3 About(const int axis, const float degrees)
	{
		const float r = degrees * 3.14159265f / 180.0f;
		const float c = std::cos(r);
		const float s = std::sin(r);
		GP::Mat3 m = GP::Identity();
		const int a = (axis + 1) % 3;
		const int b = (axis + 2) % 3;
		m.m[a][a] = c;
		m.m[a][b] = -s;
		m.m[b][a] = s;
		m.m[b][b] = c;
		return m;
	}
}

int main()
{
	// ---- when --------------------------------------------------------------------------------
	Check(HandSwitch::Decide(Ready()) == HandSwitch::Verdict::kSwitch, "the other hand on it, gripping, empty: it switches");
	{
		HandSwitch::Facts f = Ready();
		f.twoHandedWeapon = true;
		Check(HandSwitch::Decide(f) == HandSwitch::Verdict::kTwoHanderStays, "a two-hander stays: the engine will not hold it left-handed");
	}
	{
		HandSwitch::Facts f = Ready();
		f.otherHandOnWeapon = false;
		Check(HandSwitch::Decide(f) == HandSwitch::Verdict::kOtherHandNotOnIt, "the other hand is not on it: nothing happens");
	}
	{
		HandSwitch::Facts f = Ready();
		f.otherGripClosed = false;
		Check(HandSwitch::Decide(f) == HandSwitch::Verdict::kOtherGripOpen, "on it but not gripping: nothing happens");
	}
	{
		HandSwitch::Facts f = Ready();
		f.otherHandFree = false;
		Check(HandSwitch::Decide(f) == HandSwitch::Verdict::kOtherHandBusy, "the other hand holds its own weapon: dual wielding never moves one");
	}
	Check(HandSwitch::Decide(HandSwitch::Facts{}) == HandSwitch::Verdict::kOtherHandNotOnIt, "nothing true: nothing happens");

	// ---- which way round ---------------------------------------------------------------------
	const GP::Vec3 blade = GP::Make(0.0f, 1.0f, 0.0f);
	{
		const GP::Reverse r = HandSwitch::JudgeReversed(GP::Identity(), GP::Identity(), blade, 90.0f);
		Check(!r.reversed && r.angleDeg < 0.5f, "both hands' nodes the same way round: the natural way");
	}
	{
		const GP::Reverse r = HandSwitch::JudgeReversed(GP::Identity(), About(0, 180.0f), blade, 90.0f);
		Check(r.reversed && r.angleDeg > 179.0f, "the receiving hand's node turned over: reversed");
	}
	Check(!HandSwitch::JudgeReversed(GP::Identity(), About(1, 180.0f), blade, 90.0f).reversed,
		"turned about the blade itself: the tip still points the same way, not reversed");
	Check(HandSwitch::JudgeReversed(GP::Identity(), About(0, 100.0f), blade, 90.0f).reversed,
		"100 degrees apart is nearer backwards: reversed");
	Check(!HandSwitch::JudgeReversed(GP::Identity(), About(0, 80.0f), blade, 90.0f).reversed,
		"80 degrees apart is nearer forwards: the natural way");
	Check(!HandSwitch::JudgeReversed(GP::Identity(), About(0, 150.0f), blade, 20.0f).reversed,
		"a 20 degree cone wants it within 20 of dead backwards");
	Check(HandSwitch::JudgeReversed(GP::Identity(), About(0, 170.0f), blade, 20.0f).reversed,
		"170 is inside a 20 degree cone");
	{
		// Only the two nodes' relation matters, wherever the hands are.
		const GP::Mat3 holding = About(2, 30.0f);
		Check(!HandSwitch::JudgeReversed(holding, holding, blade, 90.0f).reversed, "the same node turned anywhere: natural");
		Check(HandSwitch::JudgeReversed(holding, GP::Mul(holding, About(0, 180.0f)), blade, 90.0f).reversed,
			"the receiving node half a turn from the holding one: reversed");
	}
	Check(!HandSwitch::JudgeReversed(GP::Identity(), About(0, 180.0f), GP::Make(0.0f, 0.0f, 0.0f), 90.0f).reversed,
		"no blade direction: never reversed");

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
