// Console tests for HolsterLearnRules.h: when a measured VRIK slot position is believed, and
// how a slot recovers from an average built on bad samples - without the game.
//
// Not part of the plugin - SwapDropAndHoldRedux.vcxproj does not compile this file.
// From a developer prompt (vcvars64.bat):
//   cl /nologo /EHsc /W4 /std:c++17 tools_holster_learn_tests.cpp /Fe:holster_learn_tests.exe
//   holster_learn_tests.exe
// Exit code 0 means every check passed.

#include "HolsterLearnRules.h"

#include <cstdio>

using namespace SwapDropAndHoldRedux::HolsterLearn;

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

	// The learner as VrikHolsters.cpp runs it, with distances handed in rather than measured.
	struct Slot
	{
		int sampleCount = 0;
		int rejections = 0;
	};

	enum class Step
	{
		kTaken,    // the sample joined the slot's average
		kRejected, // an outlier, and not yet enough agreeing ones to mean anything
		kReset,    // the agreeing rejections replaced the slot
	};

	// distanceFromMean: this sample against the slot's stored average.
	// distanceFromFirstRejection: against the first of the current run of rejections.
	Step Offer(Slot& slot, const float distanceFromMean, const float distanceFromFirstRejection)
	{
		if (IsOutlier(slot.sampleCount, distanceFromMean))
		{
			if (slot.rejections > 0 && RejectionJoinsCluster(distanceFromFirstRejection))
			{
				++slot.rejections;
			}
			else
			{
				slot.rejections = 1;
			}

			if (!ClusterReplacesSlot(slot.rejections))
			{
				return Step::kRejected;
			}

			slot.sampleCount = slot.rejections;
			slot.rejections = 0;
			return Step::kReset;
		}

		++slot.sampleCount;
		slot.rejections = 0;
		return Step::kTaken;
	}
}

int main()
{
	// The filter only switches on once a mean means something.
	Check(!IsOutlier(0, 40.0f), "no mean yet: nothing is an outlier");
	Check(!IsOutlier(1, 40.0f), "one sample: nothing is an outlier");
	Check(!IsOutlier(2, 40.0f), "two samples: nothing is an outlier");
	Check(IsOutlier(3, 15.1f), "three samples: just past 15 units is an outlier");
	Check(!IsOutlier(3, 15.0f), "three samples: exactly 15 units is still taken");
	Check(!IsOutlier(9, 2.0f), "a near sample is never an outlier");

	// The four real Right Hip interactions from the 2026-09-15 log.
	const float rightHipRejects[] = { 16.7f, 17.8f, 19.6f, 19.9f };
	for (const float d : rightHipRejects)
	{
		Check(IsOutlier(3, d), "the logged Right Hip interaction is rejected by the old filter");
	}

	// Agreement: what separates a real slot from noise.
	Check(RejectionJoinsCluster(0.0f), "a rejection agrees with itself");
	Check(RejectionJoinsCluster(8.0f), "exactly 8 units apart still agrees");
	Check(!RejectionJoinsCluster(8.1f), "past 8 units apart is scatter, not a slot");
	Check(!ClusterReplacesSlot(1), "one rejection proves nothing");
	Check(!ClusterReplacesSlot(2), "two rejections prove nothing");
	Check(ClusterReplacesSlot(3), "three agreeing rejections replace the slot");

	// THE LATCH, as the log shows it: a slot on three junk samples turning away correct ones.
	{
		Slot slot;
		Check(Offer(slot, 0.0f, 0.0f) == Step::kTaken, "junk sample 1 is taken");
		Check(Offer(slot, 1.0f, 0.0f) == Step::kTaken, "junk sample 2 is taken");
		Check(Offer(slot, 1.0f, 0.0f) == Step::kTaken, "junk sample 3 is taken - the filter is now on");
		Check(slot.sampleCount == 3, "the slot is latched on three samples");

		// The player now uses the real holster. Every sample is ~18 units from the junk mean and
		// within a couple of units of the others.
		Check(Offer(slot, 18.0f, 0.0f) == Step::kRejected, "real interaction 1 is rejected");
		Check(Offer(slot, 17.5f, 1.5f) == Step::kRejected, "real interaction 2 is rejected");
		Check(Offer(slot, 18.4f, 2.0f) == Step::kReset, "real interaction 3 unsticks the slot");
		Check(slot.sampleCount == 3, "the slot restarts on the three agreeing samples");
		Check(slot.rejections == 0, "the run is cleared by the reset");
	}

	// Scatter must NOT reset a good slot: three outliers that disagree with each other.
	{
		Slot slot;
		Offer(slot, 0.0f, 0.0f);
		Offer(slot, 0.0f, 0.0f);
		Offer(slot, 0.0f, 0.0f);
		Check(Offer(slot, 30.0f, 0.0f) == Step::kRejected, "scattered outlier 1 is rejected");
		Check(Offer(slot, 30.0f, 25.0f) == Step::kRejected, "scattered outlier 2 restarts the run");
		Check(Offer(slot, 30.0f, 25.0f) == Step::kRejected, "scattered outlier 3 restarts it again");
		Check(slot.sampleCount == 3, "a slot is never reset by samples that disagree");
	}

	// A good sample in the middle of a run ends it.
	{
		Slot slot;
		Offer(slot, 0.0f, 0.0f);
		Offer(slot, 0.0f, 0.0f);
		Offer(slot, 0.0f, 0.0f);
		Check(Offer(slot, 18.0f, 0.0f) == Step::kRejected, "outlier starts a run");
		Check(Offer(slot, 18.0f, 1.0f) == Step::kRejected, "a second agrees");
		Check(Offer(slot, 2.0f, 0.0f) == Step::kTaken, "then the hand is back at the stored centre");
		Check(slot.rejections == 0, "that ends the run");
		Check(Offer(slot, 18.0f, 0.0f) == Step::kRejected, "so the next outlier starts over");
		Check(Offer(slot, 18.0f, 1.0f) == Step::kRejected, "two again, not three");
	}

	// A slot can always recover, however long it has been wrong.
	{
		Slot slot;
		slot.sampleCount = 40;
		Check(Offer(slot, 25.0f, 0.0f) == Step::kRejected, "a 40-sample slot still rejects");
		Check(Offer(slot, 25.0f, 3.0f) == Step::kRejected, "and again");
		Check(Offer(slot, 25.0f, 4.0f) == Step::kReset, "three agreeing samples still unstick it");
		Check(slot.sampleCount == 3, "and it restarts small, so the next samples carry weight");
	}

	// How much evidence the tight buzz wants.
	Check(!CentreIsTrusted(1), "one sample does not earn the tight buzz");
	Check(!CentreIsTrusted(3), "three samples - the outlier threshold - still do not");
	Check(!CentreIsTrusted(4), "four do not");
	Check(CentreIsTrusted(5), "five do");
	Check(CentreIsTrusted(9), "and anything above");
	Check(kBuzzMinSamples > kSamplesBeforeOutlierFilter,
		"the buzz must want MORE evidence than the outlier filter, or a latched slot buzzes tight");

	// A slot just unstuck by a reset restarts at kRejectionsBeforeReset samples, and that must
	// not be enough for the tight buzz - the new centre has had no chance to average yet.
	Check(!CentreIsTrusted(kRejectionsBeforeReset),
		"a freshly unstuck slot keeps the wide zone until it has averaged");

	std::printf("%d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
