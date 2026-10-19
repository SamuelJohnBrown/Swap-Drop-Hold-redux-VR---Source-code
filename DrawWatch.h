#pragma once

#include "skse64/GameForms.h"

namespace SwapDropAndHoldRedux
{
	// Watches a weapon that was just grab-equipped until it is actually out of its sheath, and
	// asks again for the draw while it is not.
	//
	// The draw requested at the end of a grab-equip is fire and forget: DrawSheatheWeapon(true)
	// is a request, and this mod never checked that the engine honoured it. When it is swallowed
	// the weapon is equipped and SHEATHED - in VR, simply not in your hand - until something
	// unrelated draws it later. That is the "I hear it equip but the weapon is gone, and it turns
	// up eventually" report. The same thing was already known about crossbows, which is why they
	// alone got a retrying draw; everything else kept the single shot.
	//
	// This is a watch, not upkeep. It stops the instant the weapon is out, it never asks for a
	// draw that is already done, and it never touches a weapon that was put away on purpose -
	// which is what separates it from the continuous draw/sheathe upkeep that used to block
	// sneak and jump and spam equip audio when dual wielding. DrawWatchRules.h has the rules and
	// the reasoning; tools_draw_watch_tests.cpp drives them outside the game.
	//
	// With bGrabDrawVerify = 0 it only measures, and the log still says what happened.

	// Call right after a grab-equip has asked for its draw.
	void DrawWatchBegin(bool isLeftGameHand, TESForm* weaponForm);

	// Call once per frame.
	void DrawWatchTick(float deltaTime);

	// Logs how the watch is configured, once, when it is armed.
	void DrawWatchReportArming();
}
