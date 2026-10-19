#pragma once

// The hand switch (M3.0.0-test14): two-handing a one-handed weapon, the hand holding it lets go - and the weapon
// moves to the other hand, held where that hand was holding it, and reversed if it held it backwards. Grip hold
// does it when the hand opens (its old hand-off); Simple does it on the first opening, with no double tap.
// GripHold.cpp decides with Decide; GrabPose.cpp measures the receiving hand with JudgeReversed.
//
// No engine types in here, so tools_hand_switch_tests.cpp can drive it outside the game.

#include "GrabPoseRules.h"

namespace SwapDropAndHoldRedux
{
	namespace HandSwitch
	{
		struct Facts
		{
			bool twoHandedWeapon = false;   // it stays while either hand grips it: the engine will not hold it left-handed
			bool otherHandOnWeapon = false; // the HIGGS two-handing latch (IsOppositeHandEngaged)
			bool otherGripClosed = false;   // that hand is still gripping
			bool otherHandFree = false;     // and holds nothing of its own: dual wielding never moves a weapon
		};

		enum class Verdict
		{
			kSwitch,
			kTwoHanderStays,
			kOtherHandNotOnIt,
			kOtherGripOpen,
			kOtherHandBusy,
		};

		// The most basic reason wins, so a log says the thing that matters.
		inline Verdict Decide(const Facts& f)
		{
			if (f.twoHandedWeapon)
			{
				return Verdict::kTwoHanderStays;
			}
			if (!f.otherHandOnWeapon)
			{
				return Verdict::kOtherHandNotOnIt;
			}
			if (!f.otherGripClosed)
			{
				return Verdict::kOtherGripOpen;
			}
			if (!f.otherHandFree)
			{
				return Verdict::kOtherHandBusy;
			}
			return Verdict::kSwitch;
		}

		// Which way round the receiving hand has it. The weapon hangs off the attach node of the hand holding it
		// (first-person WEAPON or SHIELD - PCVR writes a reversed hold onto that node), and held the natural way in
		// the receiving hand it would hang off THAT hand's attach node. So the natural blade is the real one carried
		// from one node's frame to the other's: the pickup's test (GrabPose::JudgeReversed), with the receiving
		// hand's node standing in for HIGGS's as-if-equipped pose.
		inline GrabPose::Reverse JudgeReversed(
			const GrabPose::Mat3& holdingAttachRot,
			const GrabPose::Mat3& receivingAttachRot,
			const GrabPose::Vec3& bladeWorld,
			const float coneDeg)
		{
			const GrabPose::Mat3 toNatural = GrabPose::Mul(receivingAttachRot, GrabPose::Transpose(holdingAttachRot));
			return GrabPose::JudgeReversed(bladeWorld, GrabPose::Mul(toNatural, bladeWorld), coneDeg);
		}
	}
}
