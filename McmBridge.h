#pragma once

namespace SwapDropAndHoldRedux
{
	// The esp exists only to carry a start-enabled quest, which is what lets MCM
	// Helper register the menu. Without it an SKSE-only mod has no menu at all.
	constexpr const char* kMcmEspName = "SwapDropAndHoldRedux.esp";
	constexpr unsigned int kMcmQuestFormID = 0x000800;

	// Hooks the journal-close re-read. Safe at kInputLoaded.
	void RegisterMcmBridge();

	// Reports, in our own log, whether the menu can actually register. MUST run at
	// kDataLoaded or later - see the note on the definition.
	void ReportMcmChain();
}
