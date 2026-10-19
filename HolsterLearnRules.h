#pragma once

// When a measured VRIK slot position is believed, and how a slot recovers from an average
// built on bad samples. No engine types in here, so tools_holster_learn_tests.cpp can check
// it outside the game; VrikHolsters.cpp feeds it the live sample counts and distances.

namespace SwapDropAndHoldRedux
{
	namespace HolsterLearn
	{
		// A sample this far from a slot's running mean is a different gesture that happened to
		// be nearest this slot, not evidence about where the slot is.
		constexpr float kOutlierDistance = 15.0f;

		// Under this the mean is still one or two gestures' noise, so nothing counts as an
		// outlier yet and every sample is taken.
		constexpr int kSamplesBeforeOutlierFilter = 3;

		inline bool IsOutlier(const int sampleCount, const float distanceFromMean)
		{
			return sampleCount >= kSamplesBeforeOutlierFilter && distanceFromMean > kOutlierDistance;
		}

		// ---- recovering from an average that was wrong ---------------------------------------
		//
		// The filter above protects a mean and cannot tell a good mean from a bad one. A slot
		// whose first three samples were junk therefore rejects every correct sample after them,
		// forever. The 2026-09-15 log caught exactly that: the Right Hip slot turned away four
		// real interactions in a row at 16.7 to 19.9 units, while the Right Thigh slot sat 18.6
		// units from where the mirrored left-side evidence puts it, on three samples that never
		// moved again.
		//
		// Rejected samples that AGREE WITH EACH OTHER are the tell. Noise scatters; a real slot
		// does not. When this many rejections in a row land within kAgreementRadius of each
		// other, they are the slot and the stored mean is the mistake.
		constexpr float kAgreementRadius = 8.0f;
		constexpr int kRejectionsBeforeReset = 3;

		inline bool RejectionJoinsCluster(const float distanceFromFirstRejection)
		{
			return distanceFromFirstRejection <= kAgreementRadius;
		}

		inline bool ClusterReplacesSlot(const int agreeingRejections)
		{
			return agreeingRejections >= kRejectionsBeforeReset;
		}

		// ---- how much evidence the tight buzz wants ------------------------------------------
		//
		// HolsterRange::ForBuzz drops the padding that a known slot centre no longer needs. One
		// accepted sample is not a known centre: in the same log the Left Thigh slot's one-sample
		// centre was [4.75 12.63 0.79] and its seven-sample centre [7.71 17.21 5.65], 7.3 units
		// apart - nearly the whole width of the tight buzz. Under this a slot keeps the wide,
		// forgiving zone it had in 2.1.1.
		constexpr int kBuzzMinSamples = 5;

		inline bool CentreIsTrusted(const int sampleCount)
		{
			return sampleCount >= kBuzzMinSamples;
		}
	}
}
