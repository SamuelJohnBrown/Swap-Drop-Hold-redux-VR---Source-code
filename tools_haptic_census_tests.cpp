// Console tests for HapticCensusRules.h: the bookkeeping behind the haptic census - the ring that
// carries pulses from whatever thread fired them to the physics step, the per-session log cap, and
// the summary that turns hundreds of pulses into a few rows - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_haptic_census_tests.cpp /Fe:haptic_census_tests.exe
//   haptic_census_tests.exe
// Exit code 0 means every check passed.

#include "HapticCensusRules.h"

#include <cstdio>

using namespace SwapDropAndHoldRedux::HapticCensus;

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

	Pulse Make(const unsigned int device, const unsigned short micros, const Source source, const long long ticks)
	{
		Pulse p;
		p.device = device;
		p.axis = 0;
		p.micros = micros;
		p.ticks = ticks;
		p.source = source;
		return p;
	}
}

int main()
{
	// ---- the ring ---------------------------------------------------------------------------
	{
		Ring ring;
		Check(ring.Count() == 0, "a new ring is empty");
		Check(ring.Dropped() == 0, "and has dropped nothing");

		ring.Push(Make(3, 1800, Source::kOurs, 10));
		ring.Push(Make(4, 400, Source::kOther, 11));
		Check(ring.Count() == 2, "two pushes, two pulses");

		Pulse out[8];
		const int n = ring.Drain(out, 8);
		Check(n == 2, "drain hands both back");
		Check(out[0].device == 3 && out[0].micros == 1800 && out[0].source == Source::kOurs, "in order: the first");
		Check(out[1].device == 4 && out[1].micros == 400 && out[1].source == Source::kOther, "then the second");
		Check(ring.Count() == 0, "and the ring is empty again");
	}

	// Overflow drops the OLDEST, and says how many.
	{
		Ring ring;
		for (int i = 0; i < kRingCapacity + 5; ++i)
		{
			ring.Push(Make(static_cast<unsigned int>(i), 100, Source::kOther, i));
		}
		Check(ring.Count() == kRingCapacity, "a full ring holds exactly its capacity");
		Check(ring.Dropped() == 5, "five were dropped");
		Pulse out[kRingCapacity];
		const int n = ring.Drain(out, kRingCapacity);
		Check(n == kRingCapacity, "drain hands back the whole ring");
		Check(out[0].device == 5, "the oldest five are gone: the first survivor is number 5");
		Check(out[n - 1].device == static_cast<unsigned int>(kRingCapacity + 4), "the newest is last");
		Check(ring.TakeDropped() == 5, "the drop count is handed over once");
		Check(ring.Dropped() == 0, "and cleared");
	}

	// A drain smaller than the ring leaves the rest in order.
	{
		Ring ring;
		for (int i = 0; i < 6; ++i)
		{
			ring.Push(Make(static_cast<unsigned int>(i), 100, Source::kOther, i));
		}
		Pulse out[4];
		Check(ring.Drain(out, 4) == 4, "four of six");
		Check(out[3].device == 3, "in order");
		Check(ring.Count() == 2, "two left");
		Check(ring.Drain(out, 4) == 2 && out[0].device == 4, "and they come next");
	}

	// ---- the caps ---------------------------------------------------------------------------
	Check(ShouldLog(0), "the first pulse is logged");
	Check(ShouldLog(kMaxLoggedPerSession - 1), "so is the last under the cap");
	Check(!ShouldLog(kMaxLoggedPerSession), "the cap stops the per-pulse lines");
	Check(!ShouldSummarize(0), "no summary before any pulse");
	Check(ShouldSummarize(kSummaryEvery), "a summary at the first multiple");
	Check(!ShouldSummarize(kSummaryEvery + 1), "not between");
	Check(ShouldSummarize(kSummaryEvery * 3), "and at every multiple after");

	// ---- the summary ------------------------------------------------------------------------
	{
		Summary s;
		Check(s.RowCount() == 0, "a new summary has no rows");
		s.Add(1, 1800, Source::kOurs);
		s.Add(1, 1800, Source::kOurs);
		s.Add(0, 1800, Source::kOurs);
		s.Add(1, 400, Source::kOther);
		s.Add(1, 400, Source::kOther);
		s.Add(1, 400, Source::kOther);
		s.Add(1, 1800, Source::kOther); // same duration as ours, other source: its own row
		Check(s.RowCount() == 4, "rows are keyed by hand, duration AND source");
		const Row* left1800ours = s.Find(1, 1800, Source::kOurs);
		const Row* left400other = s.Find(1, 400, Source::kOther);
		const Row* left1800other = s.Find(1, 1800, Source::kOther);
		Check(left1800ours && left1800ours->count == 2, "left, 1800us, ours: 2");
		Check(left400other && left400other->count == 3, "left, 400us, other: 3");
		Check(left1800other && left1800other->count == 1, "left, 1800us, other: 1");
		Check(s.Find(0, 400, Source::kOther) == nullptr, "a row that was never added is absent");
		Check(s.Total() == 7, "the total counts every Add");
	}

	// The summary is bounded too: past its rows, pulses are counted but not broken out.
	{
		Summary s;
		for (int i = 0; i < kSummaryRows + 3; ++i)
		{
			s.Add(0, static_cast<unsigned short>(i + 1), Source::kOther);
		}
		Check(s.RowCount() == kSummaryRows, "no more rows than the table holds");
		Check(s.Overflow() == 3, "the three that did not fit are counted");
		Check(s.Total() == kSummaryRows + 3, "and still in the total");
	}

	// ---- hand from device -------------------------------------------------------------------
	Check(HandFromDevice(7, 7, 9) == 1, "the left controller's device is the left hand");
	Check(HandFromDevice(9, 7, 9) == 0, "the right controller's device is the right hand");
	Check(HandFromDevice(3, 7, 9) == -1, "anything else is neither");
	Check(HandFromDevice(7, kInvalidDevice, 9) == -1, "an unknown left controller matches nothing");
	Check(HandFromDevice(kInvalidDevice, kInvalidDevice, kInvalidDevice) == -1, "invalid never matches invalid");

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
