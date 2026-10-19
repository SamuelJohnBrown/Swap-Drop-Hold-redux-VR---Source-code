#pragma once

#include "skse64/GameReferences.h"

namespace SwapDropAndHoldRedux
{
	// Times every dropped weapon's model, and hurries one that is late.
	//
	// The engine never gives a new world reference its model on the spot. Adding the
	// reference to its cell queues the model on the model loader, which runs on ONE
	// background thread, and until that thread gets to it the reference has no 3D: it
	// cannot be seen, and with no collision it cannot be grabbed either. That is three or
	// four frames normally, and it was seconds in a report of a dropped weapon vanishing
	// and reappearing where it fell.
	//
	// A model still missing ModelWatch::Config::hurryAfterSeconds after the drop is handed
	// back to the loader's own queue function at the front-of-the-line priority. A job
	// still waiting in line moves to the front; a job already loading is not affected, and
	// neither is a paused loader - the log says how long every drop took, so it shows
	// which of those happened.

	// Call right after a drop created droppedRef from item.
	void DropModelWatchBegin(TESObjectREFR* droppedRef, TESForm* item);

	// Call once per frame.
	void DropModelWatchTick(float deltaTime);

	// Logs whether the engine code the hurry relies on matched this build's check.
	void DropModelWatchReportArming();

	// How many references have a job in the engine's model loader, or -1 when that cannot be
	// read. A number for the log, never used to decide anything - the draw watch prints it so
	// a wait can be told apart from a loader backlog.
	int DropModelWatchLoaderJobs();
}
