#include "ThrowMomentum.h"

#include "DropNudgeRules.h"

#include "Engine.h"
#include "Helper.h"
#include "RawHavok.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameRTTI.h>
#include <skse64/GameReferences.h>

#include <chrono>
#include <cmath>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		using RawHavok::Mat3Mul;
		using RawHavok::Mat3Transpose;

		// Angular velocity (rad/s) carrying rotation `from` to `to` over dt.
		NiPoint3 AngularVelocity(const NiMatrix33& from, const NiMatrix33& to, const float dt)
		{
			if (dt <= 0.0f)
			{
				return NiPoint3(0.0f, 0.0f, 0.0f);
			}

			const NiMatrix33 d = Mat3Mul(to, Mat3Transpose(from));
			float cosA = (d.data[0][0] + d.data[1][1] + d.data[2][2] - 1.0f) * 0.5f;
			if (cosA > 1.0f)
			{
				cosA = 1.0f;
			}
			else if (cosA < -1.0f)
			{
				cosA = -1.0f;
			}
			const float angle = std::acos(cosA);
			const float s = std::sin(angle);
			if (angle < 1e-4f || std::fabs(s) < 1e-6f)
			{
				return NiPoint3(0.0f, 0.0f, 0.0f);
			}

			NiPoint3 axis(
				d.data[2][1] - d.data[1][2],
				d.data[0][2] - d.data[2][0],
				d.data[1][0] - d.data[0][1]);
			const float invTwoS = 1.0f / (2.0f * s);
			axis.x *= invTwoS;
			axis.y *= invTwoS;
			axis.z *= invTwoS;
			const float scale = angle / dt;
			return NiPoint3(axis.x * scale, axis.y * scale, axis.z * scale);
		}

		// Player's horizontal right vector, for nudging a dropped weapon sideways so it
		// does not spawn inside the hand that let go of it.
		NiPoint3 GetPlayerRightVector(PlayerCharacter* player)
		{
			if (!player)
			{
				return NiPoint3(1.0f, 0.0f, 0.0f);
			}

			NiNode* rootNode = player->GetNiRootNode(0);
			if (!rootNode)
			{
				rootNode = player->GetNiRootNode(1);
			}

			if (!rootNode)
			{
				return NiPoint3(1.0f, 0.0f, 0.0f);
			}

			NiPoint3 right(
				rootNode->m_worldTransform.rot.data[0][0],
				rootNode->m_worldTransform.rot.data[1][0],
				0.0f);
			const float len = std::sqrt(right.x * right.x + right.y * right.y);
			if (len < 0.0001f)
			{
				return NiPoint3(1.0f, 0.0f, 0.0f);
			}

			right.x /= len;
			right.y /= len;
			return right;
		}

		NiObject* GetHiggsBody(const bool isLeftVRController, const bool weapon)
		{
			if (!higgsInterface)
			{
				return nullptr;
			}

			return weapon
				? higgsInterface->GetWeaponRigidBody(isLeftVRController)
				: higgsInterface->GetHandRigidBody(isLeftVRController);
		}

		struct PoseSample
		{
			float t = 0.0f;
			NiPoint3 pos;
			NiMatrix33 rot;
		};

		static const int kHistory = 24;

		struct ThrowHand
		{
			PoseSample history[kHistory];
			int historyCount = 0;
			int historyHead = 0;

			// Captured at release, applied to the dropped ref once its body exists.
			NiPoint3 dropVel;
			NiPoint3 dropAngVel;
			// The weapon's real pose at the moment of release (Havok metres). The
			// spawned world model is otherwise placed at the HAND node with no
			// rotation at all, which is why a dropped weapon appeared on top of the
			// hand at the wrong angle.
			NiPoint3 dropPosMeters;
			NiMatrix33 dropRot;
			bool haveDropPose = false;
			UInt32 droppedRefID = 0;
			int fixupFrames = 0;
			int stampsLeft = 0;
		};

		static ThrowHand s_throwHands[2]; // [0] = right VR controller, [1] = left

		ThrowHand& ThrowHandFor(const bool isLeftVRController)
		{
			return s_throwHands[isLeftVRController ? 1 : 0];
		}

		// Samples are stamped with REAL elapsed time, not accumulated frame deltas.
		// Sampling only runs while a weapon is held, so a drop stops the clock: with
		// frame deltas the last sample before a drop and the first after the re-equip
		// looked one frame apart no matter how long the weapon had been gone, and the
		// metres it travelled in between read back as a throw of hundreds of m/s.
		typedef std::chrono::steady_clock ThrowClock;

		float ThrowNowSeconds()
		{
			static const ThrowClock::time_point epoch = ThrowClock::now();
			return std::chrono::duration<float>(ThrowClock::now() - epoch).count();
		}

		// Wipes the pose history WITHOUT touching a throw already in flight — the
		// fixup fields keep stamping the weapon that was just released.
		void ClearHistory(ThrowHand& hand)
		{
			hand.historyCount = 0;
			hand.historyHead = 0;
		}

		// Release velocity: the FASTEST motion over the recent window. A real throw
		// decelerates as the hand opens, so the last frame alone reads far too slow.
		void ReleaseVelocity(const ThrowHand& hand, NiPoint3& outLinear, NiPoint3& outAngular)
		{
			outLinear = NiPoint3(0.0f, 0.0f, 0.0f);
			outAngular = NiPoint3(0.0f, 0.0f, 0.0f);
			if (hand.historyCount < 2)
			{
				return;
			}

			const int newest = (hand.historyHead - 1 + kHistory) % kHistory;
			const PoseSample& s0 = hand.history[newest];
			float bestSpeed = 0.0f;
			for (int i = 1; i < hand.historyCount; ++i)
			{
				const PoseSample& si = hand.history[(newest - i + kHistory) % kHistory];
				const float dt = s0.t - si.t;
				if (dt < 0.008f)
				{
					continue; // too short a baseline: mostly sampling noise
				}
				if (dt > throwWindow)
				{
					break;
				}

				const NiPoint3 v(
					(s0.pos.x - si.pos.x) / dt,
					(s0.pos.y - si.pos.y) / dt,
					(s0.pos.z - si.pos.z) / dt);
				const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
				if (speed > bestSpeed)
				{
					bestSpeed = speed;
					outLinear = v;
					outAngular = AngularVelocity(si.rot, s0.rot, dt);
				}
			}

			if (bestSpeed > throwMaxSpeed && bestSpeed > 0.0f)
			{
				const float scale = throwMaxSpeed / bestSpeed;
				outLinear.x *= scale;
				outLinear.y *= scale;
				outLinear.z *= scale;
				outAngular.x *= scale;
				outAngular.y *= scale;
				outAngular.z *= scale;
			}
		}

		TESObjectREFR* ResolveRef(const UInt32 refID)
		{
			if (refID == 0)
			{
				return nullptr;
			}

			TESForm* refForm = LookupFormByID(refID);
			return DYNAMIC_CAST(refForm, TESForm, TESObjectREFR);
		}

		// Hand the freshly dropped reference its throw velocity. The reference has no
		// 3D for a frame or two after the drop, and the engine wipes a single write
		// while it settles the object — so the body is searched over a window and the
		// velocity is re-asserted for several frames.
		void ServiceFixup(ThrowHand& hand, const bool isLeftVRController)
		{
			if (hand.fixupFrames <= 0)
			{
				return;
			}

			--hand.fixupFrames;
			TESObjectREFR* refr = ResolveRef(hand.droppedRefID);
			if (!refr)
			{
				hand.fixupFrames = 0;
				hand.droppedRefID = 0;
				return;
			}

			NiNode* root = refr->GetNiNode();
			NiObject* body = RawHavok::FindRigidBodyInSubtree(root);
			void* hk = RawHavok::GetHkpRigidBody(body);
			if (!hk)
			{
				if (hand.fixupFrames == 0)
				{
					LOG_INFO(
						"Throw fixup [%s controller]: no rigid body found for refId=%08X — dropped without momentum",
						isLeftVRController ? "left" : "right",
						hand.droppedRefID);
				}
				return; // not built yet; try again next frame
			}

			// First frame the body exists: put it exactly where the weapon was, at the
			// angle it was held, instead of leaving it at the hand-node placement.
			if (hand.stampsLeft == throwStampFrames)
			{
				if (hand.haveDropPose)
				{
					__declspec(align(16)) float havokPos[4] = {
						hand.dropPosMeters.x, hand.dropPosMeters.y, hand.dropPosMeters.z, 0.0f
					};
					__declspec(align(16)) float quat[4];
					RawHavok::ToQuat(hand.dropRot, quat);
					RawHavok::hkpRigidBody_setPositionAndRotation(hk, havokPos, quat);
				}

				LOG_INFO(
					"Throw fixup [%s controller]: applying v=[%.1f %.1f %.1f] m/s to refId=%08X%s - started %.1f units "
					"toward your middle and %.1f units below the hand",
					isLeftVRController ? "left" : "right",
					hand.dropVel.x,
					hand.dropVel.y,
					hand.dropVel.z,
					hand.droppedRefID,
					hand.haveDropPose ? " (pose corrected)" : "",
					hand.haveDropPose ? dropLateralOffset : 0.0f,
					hand.haveDropPose ? dropDownOffset : 0.0f);
			}

			// Wake the island first: a sleeping body ignores a velocity write.
			RawHavok::hkpEntity_activate(hk);
			RawHavok::WriteHkVelocities(hk, hand.dropVel, hand.dropAngVel);

			if (--hand.stampsLeft <= 0)
			{
				hand.fixupFrames = 0;
				hand.droppedRefID = 0;
			}
		}
	}

	float ThrowLastReleaseSpeedMps(const bool isLeftVRController)
	{
		const ThrowHand& hand = ThrowHandFor(isLeftVRController);
		return std::sqrt(
			hand.dropVel.x * hand.dropVel.x + hand.dropVel.y * hand.dropVel.y + hand.dropVel.z * hand.dropVel.z);
	}

	float ThrowPeakSpeedMps(const bool isLeftVRController, int& outSamples)
	{
		const ThrowHand& hand = ThrowHandFor(isLeftVRController);
		outSamples = hand.historyCount;

		NiPoint3 linear;
		NiPoint3 angular;
		ReleaseVelocity(hand, linear, angular);
		return std::sqrt(
			linear.x * linear.x + linear.y * linear.y + linear.z * linear.z);
	}

	void ThrowSampleHeldPose(const bool isLeftVRController, const float deltaTime)
	{
		(void)deltaTime;
		ThrowHand& hand = ThrowHandFor(isLeftVRController);

		NiMatrix33 rot;
		NiPoint3 posMeters;
		if (!RawHavok::ReadHkTransform(GetHiggsBody(isLeftVRController, true), rot, posMeters) &&
			!RawHavok::ReadHkTransform(GetHiggsBody(isLeftVRController, false), rot, posMeters))
		{
			return;
		}

		PoseSample& slot = hand.history[hand.historyHead];
		slot.t = ThrowNowSeconds();
		slot.pos = posMeters;
		slot.rot = rot;
		hand.historyHead = (hand.historyHead + 1) % kHistory;
		if (hand.historyCount < kHistory)
		{
			++hand.historyCount;
		}
	}

	void ThrowCaptureRelease(const bool isLeftVRController)
	{
		ThrowHand& hand = ThrowHandFor(isLeftVRController);

		// The weapon's own pose, captured BEFORE the unequip destroys it. Applied to
		// the spawned world model so it appears where the weapon actually was.
		hand.haveDropPose = RawHavok::ReadHkTransform(
			GetHiggsBody(isLeftVRController, true), hand.dropRot, hand.dropPosMeters);

		// Move it clear of the hand: a weapon released at its exact held pose spawns inside
		// HIGGS's hand collision and fights it. Sideways toward the midline (positive: left
		// of the right hand, right of the left hand), and - 3.0.0 - straight down below the
		// palm, which is what clears a weapon lying across a palm-down hand. DropNudgeRules.h.
		if (hand.haveDropPose && (dropLateralOffset != 0.0f || dropDownOffset != 0.0f))
		{
			const NiPoint3 right = GetPlayerRightVector(*g_thePlayer);
			DropNudge::Pos at;
			at.x = hand.dropPosMeters.x;
			at.y = hand.dropPosMeters.y;
			at.z = hand.dropPosMeters.z;
			const DropNudge::Pos moved = DropNudge::Nudged(at, right.x, right.y, isLeftVRController,
				dropLateralOffset, dropDownOffset, RawHavok::kHavokToSkyrim);
			hand.dropPosMeters.x = moved.x;
			hand.dropPosMeters.y = moved.y;
			hand.dropPosMeters.z = moved.z;
		}

		if (!throwMomentum)
		{
			hand.dropVel = NiPoint3(0.0f, 0.0f, 0.0f);
			hand.dropAngVel = NiPoint3(0.0f, 0.0f, 0.0f);
			return;
		}

		NiPoint3 linear;
		NiPoint3 angular;
		ReleaseVelocity(hand, linear, angular);
		hand.dropVel = NiPoint3(
			linear.x * throwVelocityScale,
			linear.y * throwVelocityScale,
			linear.z * throwVelocityScale);
		hand.dropAngVel = angular;

		// This hold is over. Anything still in the buffer belongs to a weapon that is
		// no longer in this hand, and must never contribute to the next throw.
		ClearHistory(hand);
	}

	void ThrowBeginFixup(const bool isLeftVRController, const UInt32 droppedRefID)
	{
		ThrowHand& hand = ThrowHandFor(isLeftVRController);
		if (droppedRefID == 0)
		{
			return;
		}

		hand.droppedRefID = droppedRefID;
		hand.fixupFrames = 30;
		hand.stampsLeft = throwStampFrames;
	}

	void ThrowServiceFixups()
	{
		ServiceFixup(s_throwHands[0], false);
		ServiceFixup(s_throwHands[1], true);
	}

	void ThrowClearHistory(const bool isLeftVRController)
	{
		ClearHistory(ThrowHandFor(isLeftVRController));
	}

	void ThrowResetHand(const bool isLeftVRController)
	{
		ThrowHandFor(isLeftVRController) = ThrowHand{};
	}

	void ThrowResetAll()
	{
		s_throwHands[0] = ThrowHand{};
		s_throwHands[1] = ThrowHand{};
	}
}
