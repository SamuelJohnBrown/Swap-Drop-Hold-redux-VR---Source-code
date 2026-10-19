#include "GrabPose.h"

#include "Engine.h"
#include "GrabGripProvider.h"
#include "GrabGripRules.h"
#include "GrabPoseRules.h"
#include "HandSwitchRules.h"
#include "Helper.h"
#include "RawHavok.h"
#include "TrackedWeapons.h"
#include "config.h"

#include <skse64/GameObjects.h>
#include <skse64/GameReferences.h>
#include <skse64/GameRTTI.h>
#include <skse64/NiNodes.h>
#include <skse64_common/Utilities.h>

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		namespace GP = GrabPose;
		using Clock = std::chrono::steady_clock;

		constexpr float kUnitsPerMetre = 69.99125f;
		// A weapon's collision body and its node normally share a frame to a fraction of a degree. Past
		// this the body's long axis is not a node cardinal and HoldPose cannot be expressed; say so, pose nothing.
		constexpr float kMaxBodyNodeAngleDeg = 5.0f;
		constexpr int kFollowSteps = 30;

		// ---- engine types to the rules' own -----------------------------------------------------------
		GP::Vec3 ToVec(const NiPoint3& p)
		{
			return GP::Make(p.x, p.y, p.z);
		}

		GP::Mat3 ToMat(const NiMatrix33& m)
		{
			GP::Mat3 r;
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
				{
					r.m[i][j] = m.data[i][j];
				}
			}
			return r;
		}

		GP::Xform ToXform(const NiTransform& t)
		{
			GP::Xform x;
			x.rot = ToMat(t.rot);
			x.pos = ToVec(t.pos);
			x.scale = t.scale;
			return x;
		}

		NiTransform FromXform(const GP::Xform& x)
		{
			NiTransform t;
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
				{
					t.rot.data[i][j] = x.rot.m[i][j];
				}
			}
			t.pos = NiPoint3(x.pos.x, x.pos.y, x.pos.z);
			t.scale = x.scale;
			return t;
		}

		bool Finite(const GP::Xform& x)
		{
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
				{
					if (!std::isfinite(x.rot.m[i][j]))
					{
						return false;
					}
				}
			}
			return std::isfinite(x.pos.x) && std::isfinite(x.pos.y) && std::isfinite(x.pos.z) && std::isfinite(x.scale);
		}

		// ---- the weapon's collision body --------------------------------------------------------------
		// The body's world pose in Havok metres. The motion state keeps its rotation as three columns
		// (RawHavok.h, ReadHkTransform); read straight into the rules' matrix.
		bool ReadBody(NiObject* bhkBody, GP::Mat3& outRot, GP::Vec3& outPosMetres)
		{
			void* hk = RawHavok::GetHkpRigidBody(bhkBody);
			if (!hk)
			{
				return false;
			}
			const float* t = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(hk) + 0x170);
			for (int r = 0; r < 3; ++r)
			{
				outRot.m[r][0] = t[0 + r];
				outRot.m[r][1] = t[4 + r];
				outRot.m[r][2] = t[8 + r];
			}
			outPosMetres = GP::Make(t[12], t[13], t[14]);
			return true;
		}

		// The collision shape's box in the shape's OWN frame: hkpShape::getAabb (vtable slot 7, the call
		// EmbedPins.cpp makes with the body's transform) given the identity instead.
		bool ReadLocalBox(NiObject* bhkBody, GP::Box& outMetres)
		{
			void* hk = RawHavok::GetHkpRigidBody(bhkBody);
			if (!hk)
			{
				return false;
			}
			void* shape = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hk) + 0x20);
			if (!shape)
			{
				return false;
			}

			__declspec(align(16)) float identity[16] = {
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 0.0f };
			__declspec(align(16)) float aabb[8] = {};
			typedef void (*GetAabbImpl_t)(void* shape, const void* localToWorld, float tolerance, void* outAabb);
			void** vtbl = *reinterpret_cast<void***>(shape);
			reinterpret_cast<GetAabbImpl_t>(vtbl[7])(shape, identity, 0.0f, aabb);

			outMetres.lo = GP::Make(aabb[0], aabb[1], aabb[2]);
			outMetres.hi = GP::Make(aabb[4], aabb[5], aabb[6]);
			return std::isfinite(aabb[0]) && std::isfinite(aabb[4]);
		}

		// ---- the hand ---------------------------------------------------------------------------------
		// HIGGS's palm, in the hand bone's frame and Skyrim units: higgs_vr.ini [Settings] PalmPositionX/Y/Z.
		// HIGGS does not offer these through GetSettingDouble, so the ini is read once; its shipped values
		// are the fallback. X is mirrored for the left hand, as HIGGS mirrors its palm vector (hand.cpp:2277).
		GP::Vec3 PalmInHand(const bool isLeftVRController)
		{
			static bool s_read = false;
			static GP::Vec3 s_palm = GP::Make(0.0f, -2.4f, 6.0f);
			if (!s_read)
			{
				s_read = true;
				const std::string path = GetRuntimeDirectory() + "Data\\SKSE\\Plugins\\higgs_vr.ini";
				const char* keys[3] = { "PalmPositionX", "PalmPositionY", "PalmPositionZ" };
				float values[3] = { s_palm.x, s_palm.y, s_palm.z };
				int found = 0;
				for (int i = 0; i < 3; ++i)
				{
					char buffer[64] = "";
					if (GetPrivateProfileStringA("Settings", keys[i], "", buffer, sizeof(buffer), path.c_str()) > 0)
					{
						values[i] = static_cast<float>(std::atof(buffer));
						++found;
					}
				}
				s_palm = GP::Make(values[0], values[1], values[2]);
				LOG_INFO(
					"Grab probe: HIGGS palm position in the hand bone's frame = (%.2f, %.2f, %.2f) - %s.",
					s_palm.x, s_palm.y, s_palm.z,
					found == 3 ? "read from higgs_vr.ini" : "HIGGS's shipped values (higgs_vr.ini not readable)");
			}
			return GP::Make(isLeftVRController ? -s_palm.x : s_palm.x, s_palm.y, s_palm.z);
		}

		// The hand bone HIGGS itself measures from (hand.cpp:2073, GetFirstPersonHandNode): the player's
		// own VR hand bone for that controller. The grab transform is expressed in THIS bone's frame.
		NiAVObject* HiggsHandBone(const bool isLeftVRController)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !player->GetNiRootNode(1))
			{
				return nullptr;
			}
			return player->unk3F0[isLeftVRController ? PlayerCharacter::kNode_LeftHandBone : PlayerCharacter::kNode_RightHandBone];
		}

		NiAVObject* HandBoneByName(const int rootIndex, const bool isLeftVRController)
		{
			PlayerCharacter* player = *g_thePlayer;
			NiNode* root = player ? player->GetNiRootNode(rootIndex) : nullptr;
			if (!root)
			{
				return nullptr;
			}
			BSFixedString name(isLeftVRController ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]");
			return root->GetObjectByName(&name.data);
		}

		// ---- pulls ------------------------------------------------------------------------------------
		struct Pull
		{
			UInt32 refID = 0;
			Clock::time_point when{};
		};
		Pull s_pulls[2];

		void OnGrabPosePulled(const bool isLeft, TESObjectREFR* pulledRefr)
		{
			if (!pulledRefr)
			{
				return;
			}
			Pull& pull = s_pulls[isLeft ? 1 : 0];
			pull.refID = pulledRefr->formID;
			pull.when = Clock::now();
		}

		float SecondsSincePull(const UInt32 refID)
		{
			float best = -1.0f;
			const Clock::time_point now = Clock::now();
			for (int i = 0; i < 2; ++i)
			{
				if (s_pulls[i].refID == refID && refID != 0)
				{
					const float age = std::chrono::duration<float>(now - s_pulls[i].when).count();
					if (best < 0.0f || age < best)
					{
						best = age;
					}
				}
			}
			return best;
		}

		// ---- what happens to the weapon after the callback --------------------------------------------
		struct Follow
		{
			bool active = false;
			UInt32 refID = 0;
			int step = 0;
			GP::Xform nodeAtGrab;
			GP::Xform targetInHand; // what HIGGS has been told to hold it at: its own natural pose, or ours
			bool targetIsOurs = false;
			char name[96] = "";
		};
		Follow s_follow[2];

		void OnGrabPoseStep(void* /*world*/)
		{
			if (!higgsInterface)
			{
				return;
			}
			for (int i = 0; i < 2; ++i)
			{
				Follow& follow = s_follow[i];
				if (!follow.active)
				{
					continue;
				}
				const bool isLeft = i == 1;
				++follow.step;

				TESObjectREFR* held = higgsInterface->GetGrabbedObject(isLeft);
				NiNode* node = (held && held->formID == follow.refID) ? held->GetNiNode() : nullptr;
				NiAVObject* hand = HiggsHandBone(isLeft);
				if (!node || !hand)
				{
					LOG_INFO(
						"Grab probe [%s] %s: no longer held by HIGGS at step %d (equipped, dropped, or its 3D is gone).",
						isLeft ? "left" : "right", follow.name, follow.step);
					follow.active = false;
					continue;
				}

				const bool report = follow.step == 1 || follow.step % 5 == 0;
				if (report)
				{
					const GP::Xform now = ToXform(node->m_worldTransform);
					const GP::Xform predicted = GP::Compose(ToXform(hand->m_worldTransform), follow.targetInHand);
					LOG_INFO(
						"Grab probe [%s] %s step %2d: moved %.1fu / %.1fdeg since the grab | %.1fu / %.1fdeg from %s.",
						isLeft ? "left" : "right",
						follow.name,
						follow.step,
						GP::Length(GP::Sub(now.pos, follow.nodeAtGrab.pos)),
						GP::AngleBetweenDeg(now.rot, follow.nodeAtGrab.rot),
						GP::Length(GP::Sub(now.pos, predicted.pos)),
						GP::AngleBetweenDeg(now.rot, predicted.rot),
						follow.targetIsOurs ? "the hold THIS MOD asked for" : "HIGGS's own natural hold");
				}

				if (follow.step >= kFollowSteps)
				{
					follow.active = false;
				}
			}
		}

		const char* kAxisNames[3] = { "X", "Y", "Z" };

		// What the hand that grabbed left behind, until the equip task asks for it.
		struct Kept
		{
			GrabGrip::Touch touch;
			Clock::time_point when{};
		};
		Kept s_kept[2];

		const char* WhyText(const GrabGrip::Why why)
		{
			switch (why)
			{
			case GrabGrip::Why::kNoRecord: return "nothing was sensed when it was grabbed";
			case GrabGrip::Why::kOtherWeapon: return "the hand's last touch was of a different weapon";
			case GrabGrip::Why::kStale: return "the touch is too old to be this pickup";
			case GrabGrip::Why::kNotMelee: return "not a melee weapon";
			case GrabGrip::Why::kPulled: return "it was pulled through the air, so where it landed in the hand is chance";
			case GrabGrip::Why::kWarped: return "HIGGS had already moved it to the hand, so there was no touch to measure";
			case GrabGrip::Why::kNoAxis: return "its collision shape is too small to measure";
			case GrabGrip::Why::kAmbiguousAxis: return "its shape has no clear long axis";
			case GrabGrip::Why::kOffTheWeapon: return "the palm read as nowhere near the weapon";
			default: return "";
			}
		}

		// The weapon mode's "Holding a weapon you pick up" switches (M3.0.0-test14). Legacy keeps the ini pair.
		GrabGrip::Choice CurrentGrabGripChoice()
		{
			GrabGrip::Choice simple;
			simple.position = grabGripSimple;
			simple.reversed = grabGripReversedSimple;
			simple.reversedTwoHanded = grabGripReversedTwoHandedSimple;
			simple.anywhere = grabGripAnywhereSimple;
			GrabGrip::Choice gripHold;
			gripHold.position = grabGripGripHold;
			gripHold.reversed = grabGripReversedGripHold;
			gripHold.reversedTwoHanded = grabGripReversedTwoHandedGripHold;
			gripHold.anywhere = grabGripAnywhereGripHold;
			GrabGrip::Choice legacy;
			legacy.position = grabGrip;
			legacy.reversed = grabGripReversed;
			legacy.reversedTwoHanded = false;
			return GrabGrip::ChooseForMode(weaponMode, simple, gripHold, legacy);
		}

		// A toss-up between the two flip axes: turn about the WIDER of the two extents across the weapon. That is
		// the axis the edge lies along, so the edge keeps facing the way it did.
		int WiderAcross(const GP::Box& box, const int longAxis)
		{
			int wider = -1;
			float widest = -1.0f;
			for (int i = 0; i < 3; ++i)
			{
				const float extent = GP::Get(box.hi, i) - GP::Get(box.lo, i);
				if (i != longAxis && extent > widest)
				{
					widest = extent;
					wider = i;
				}
			}
			return wider;
		}

		// The first-person node an equipped weapon hangs off, by GAME hand: WEAPON for the right, SHIELD for the
		// left - the engine's attach points, and the nodes PCVR writes a reversed hold onto.
		NiAVObject* WeaponAttachNode(const bool isLeftGameHand)
		{
			PlayerCharacter* player = *g_thePlayer;
			NiNode* root = player ? player->GetNiRootNode(1) : nullptr;
			if (!root)
			{
				return nullptr;
			}
			BSFixedString name(isLeftGameHand ? "SHIELD" : "WEAPON");
			return root->GetObjectByName(&name.data);
		}
	}

	void RegisterGrabPose()
	{
		if (!higgsInterface)
		{
			return;
		}
		higgsInterface->AddPulledCallback(OnGrabPosePulled);
		higgsInterface->AddPrePhysicsStepCallback(OnGrabPoseStep);
		// The ini is read later, at DataLoaded: config.cpp reports what the probe's settings came to.
		LOG_INFO("Grab probe (3.0.0 test) registered with HIGGS.");
	}

	void GrabPoseOnGrabbed(const bool isLeftVRController, TESObjectREFR* grabbedRefr)
	{
		const GrabGrip::Choice choice = CurrentGrabGripChoice();
		if ((!choice.position && !choice.reversed && !grabGripTrace) || !higgsInterface || !grabbedRefr ||
			!grabbedRefr->baseForm)
		{
			return;
		}

		auto* weapon = DYNAMIC_CAST(grabbedRefr->baseForm, TESForm, TESObjectWEAP);
		if (!weapon)
		{
			return;
		}
		const int weaponType = weapon->type();
		if (!GP::IsGripMelee(weaponType))
		{
			return;
		}

		const char* hand = isLeftVRController ? "left" : "right";
		const char* name = GetSafeFormName(grabbedRefr->baseForm);

		// A fresh grab replaces whatever this hand kept.
		Kept& kept = s_kept[isLeftVRController ? 1 : 0];
		kept = Kept{};

		NiNode* node = grabbedRefr->GetNiNode();
		NiObject* body = node ? RawHavok::FindRigidBodyInSubtree(node) : nullptr;
		GP::Mat3 bodyRot;
		GP::Vec3 bodyPosMetres;
		GP::Box box;
		if (!node || !body || !ReadBody(body, bodyRot, bodyPosMetres) || !ReadLocalBox(body, box))
		{
			LOG_INFO("Grab grip [%s] %s ref=%08X: no 3D or no collision body to measure - nothing recorded.", hand, name, grabbedRefr->formID);
			return;
		}

		const GP::LongAxis axis = GP::FindLongAxis(box, 0.05f);
		const GP::Xform nodeWorld = ToXform(node->m_worldTransform);
		const GP::Vec3 bodyPosUnits = GP::Scale(bodyPosMetres, kUnitsPerMetre);
		const float nodeBodyDistance = GP::Length(GP::Sub(bodyPosUnits, nodeWorld.pos));
		const float nodeBodyAngle = GP::AngleBetweenDeg(nodeWorld.rot, bodyRot);
		const bool warped = GP::Warped(nodeBodyDistance, nodeBodyAngle);

		const float hiltU = GP::HiltU(axis);
		const int tipSign = GP::TipSign(hiltU);

		// The palm, three ways, so the log says which source to trust: HIGGS's own hand bone (the frame
		// its grab transform is in), the same bone looked up by name under the other skeleton root, and
		// the origin of HIGGS's hand collision body.
		NiAVObject* handBone = HiggsHandBone(isLeftVRController);
		NiAVObject* handBoneRoot0 = HandBoneByName(0, isLeftVRController);
		const GP::Vec3 palmInHand = PalmInHand(isLeftVRController);

		float alongBone = 0.0f, offAxisBoneCm = 0.0f, alongRoot0 = 0.0f, alongHandBody = 0.0f;
		bool haveBone = false, haveRoot0 = false, haveHandBody = false;
		GP::Vec3 palmLocalMetres;
		if (handBone)
		{
			const GP::Vec3 palmWorld = GP::Apply(ToXform(handBone->m_worldTransform), palmInHand);
			palmLocalMetres = GP::ToLocal(bodyRot, bodyPosMetres, GP::Scale(palmWorld, 1.0f / kUnitsPerMetre));
			alongBone = GP::AlongU(axis, palmLocalMetres);
			offAxisBoneCm = GP::OffAxis(axis, box, palmLocalMetres) * 100.0f;
			haveBone = true;
		}
		if (handBoneRoot0)
		{
			const GP::Vec3 palmWorld = GP::Apply(ToXform(handBoneRoot0->m_worldTransform), palmInHand);
			alongRoot0 = GP::AlongU(axis, GP::ToLocal(bodyRot, bodyPosMetres, GP::Scale(palmWorld, 1.0f / kUnitsPerMetre)));
			haveRoot0 = true;
		}
		{
			GP::Mat3 handBodyRot;
			GP::Vec3 handBodyPosMetres;
			if (ReadBody(higgsInterface->GetHandRigidBody(isLeftVRController), handBodyRot, handBodyPosMetres))
			{
				alongHandBody = GP::AlongU(axis, GP::ToLocal(bodyRot, bodyPosMetres, handBodyPosMetres));
				haveHandBody = true;
			}
		}

		const float pulledAge = SecondsSincePull(grabbedRefr->formID);
		const bool pulled = GP::WasPulled(pulledAge, GP::kPulledWindowSeconds);

		if (grabGripTrace) LOG_INFO(
			"Grab probe [%s] %s (type %d) ref=%08X: node-vs-body %.1fu / %.1fdeg (warped: %s) | long axis %s, "
			"length %.2fm, %.1fx its next extent%s%s | hilt at %+.2f, tip is the %s end | palm along it: %+.2f = %.0f%% up "
			"from the hilt, %.1fcm off the centre line [HIGGS hand bone%s]; %+.2f [3rd-person hand bone%s]; %+.2f "
			"[HIGGS hand body%s] | pulled: %s",
			hand, name, weaponType, grabbedRefr->formID,
			nodeBodyDistance, nodeBodyAngle, warped ? "YES" : "no",
			kAxisNames[axis.index], axis.length, axis.ratio,
			axis.usable ? "" : " (TOO SHORT TO MEASURE)",
			axis.ambiguous ? " (AMBIGUOUS - no clear long axis)" : "",
			hiltU, tipSign > 0 ? "HIGH" : (tipSign < 0 ? "LOW" : "UNKNOWN"),
			alongBone, GP::PercentFromHilt(alongBone, hiltU, tipSign), offAxisBoneCm, haveBone ? "" : " MISSING",
			alongRoot0, haveRoot0 ? "" : " MISSING",
			alongHandBody, haveHandBody ? "" : " MISSING",
			pulled ? "YES" : "no");

		if (!handBone)
		{
			LOG_INFO("Grab grip [%s] %s: HIGGS's hand bone is missing - nothing recorded.", hand, name);
			return;
		}

		// Kept from here on, even when it cannot be used: the equip task's log then says why.
		kept.when = Clock::now();
		kept.touch.valid = true;
		kept.touch.refID = grabbedRefr->formID;
		kept.touch.formID = grabbedRefr->baseForm->formID;
		kept.touch.isLeftVRController = isLeftVRController;
		kept.touch.weaponType = weaponType;
		kept.touch.longAxis = axis.index;
		kept.touch.axisUsable = axis.usable;
		kept.touch.axisAmbiguous = axis.ambiguous;
		kept.touch.alongU = alongBone;
		kept.touch.hiltU = hiltU;
		kept.touch.lengthM = axis.length;
		kept.touch.offAxisM = offAxisBoneCm * 0.01f;
		kept.touch.tipSign = tipSign;
		kept.touch.pulled = pulled;
		kept.touch.warped = warped;

		if (!axis.usable)
		{
			LOG_INFO("Grab grip: the %s hand took %s, but %s - it will be held the natural way.", hand, name, WhyText(GrabGrip::Why::kNoAxis));
			return;
		}

		// HIGGS's grab transform right now is its as-if-equipped pose in the hand bone's frame. Put it in
		// the world and compare it with how the weapon really sits: that is how far from natural the grab was.
		const GP::Xform natural = ToXform(higgsInterface->GetGrabTransform(isLeftVRController));
		const GP::Xform naturalWorld = GP::Compose(ToXform(handBone->m_worldTransform), natural);
		const float attachAngle = GP::AngleBetweenDeg(naturalWorld.rot, nodeWorld.rot);
		const bool attachUsed = attachAngle > 1.0f || GP::Length(GP::Sub(naturalWorld.pos, nodeWorld.pos)) > 1.0f;

		const GP::Vec3 bladeLocal = GP::AxisUnit(axis.index, tipSign == 0 ? 1 : tipSign);
		const GP::Vec3 bladeActual = GP::Mul(bodyRot, bladeLocal);
		const GP::Mat3 actualToNatural = GP::Mul(naturalWorld.rot, GP::Transpose(nodeWorld.rot));
		const GP::Vec3 bladeNatural = GP::Mul(actualToNatural, bladeActual);
		const float cone = GP::ClampCone(grabGripReversedCone);
		const GP::Reverse reverse = GP::JudgeReversed(bladeActual, bladeNatural, cone);
		const bool mayReverse = GrabGrip::MayReverse(
			weaponType, axis.ambiguous, tipSign, pulled, warped, grabGripReversePulled, choice.reversedTwoHanded);
		const bool reversed = reverse.reversed && mayReverse;

		GP::FlipChoice flip = GP::ChooseFlipAxis(naturalWorld.rot, nodeWorld.rot, axis.index);
		if (!flip.decided)
		{
			flip.axis = WiderAcross(box, axis.index);
		}

		kept.touch.reversedAngleDeg = reverse.angleDeg;
		kept.touch.reversedVerdict = reversed;
		kept.touch.flipDecided = flip.decided;
		kept.touch.flipAxis = flip.axis;

		{
			const GrabGrip::Why why = GrabGrip::Judge(kept.touch, kept.touch.formID, 0.0f, grabGripPulled);
			if (why == GrabGrip::Why::kSend)
			{
				char where[96];
				if (tipSign != 0)
				{
					sprintf_s(where, "%.0f%% of the way up from the hilt", GP::PercentFromHilt(alongBone, hiltU, tipSign));
				}
				else
				{
					sprintf_s(where, "at %+.2f along it (which end is the tip is unclear)", alongBone);
				}
				LOG_INFO("Grab grip: the %s hand took %s %s - %s.", hand, name, where, reversed ? "REVERSED" : "normal grip");
			}
			else
			{
				LOG_INFO("Grab grip: the %s hand took %s, but %s - it will be held the natural way.", hand, name, WhyText(why));
			}
		}

		if (!grabGripTrace)
		{
			return;
		}

		LOG_INFO(
			"Grab probe [%s] %s: blade is %.0f degrees from the natural way round -> %s (cone %.0f, attach pose used by HIGGS: %s, "
			"%.0f degrees from how it lay) | would turn over about %s (%s, margin %.2f)%s%s%s",
			hand, name, reverse.angleDeg,
			reversed ? "REVERSED" : (reverse.reversed ? "reversed, but not allowed for this grab" : "normal grip"),
			cone, attachUsed ? "yes" : "NO", attachAngle,
			kAxisNames[flip.axis], flip.decided ? "decided by how it was held" : "a toss-up, so the wider side", flip.margin,
			mayReverse ? "" : " | not allowed to reverse: ",
			GrabGrip::WhyNotReverse(
				weaponType, axis.ambiguous, tipSign, pulled, warped, grabGripReversePulled, choice.reversedTwoHanded),
			pulled && mayReverse ? " | pulled: judged against the hand as it caught it" : "");

		// What happens next: HIGGS moves the weapon to its hold. Watch it get there.
		Follow& follow = s_follow[isLeftVRController ? 1 : 0];
		follow = Follow{};
		follow.active = true;
		follow.refID = grabbedRefr->formID;
		follow.nodeAtGrab = nodeWorld;
		follow.targetInHand = natural;
		sprintf_s(follow.name, "%s", name);

		if (!grabProbeSetTransform)
		{
			return;
		}

		// S2: make HIGGS hold it where it was taken hold of. The long axis is the BODY's; HoldPose needs it
		// as a cardinal of the NODE HIGGS drives, which it is only while the two frames agree.
		if (pulled || warped || axis.ambiguous || nodeBodyAngle > kMaxBodyNodeAngleDeg)
		{
			LOG_INFO(
				"Grab probe [%s] %s: HIGGS's hold left alone (%s).",
				hand, name,
				pulled ? "pulled" : (warped ? "warped" : (axis.ambiguous ? "ambiguous axis" : "body and node frames differ")));
			return;
		}

		const GP::Xform bodyWorld = [&]() { GP::Xform b; b.rot = bodyRot; b.pos = bodyPosUnits; b.scale = 1.0f; return b; }();
		const GP::Xform bodyInNode = GP::Compose(GP::Inverse(nodeWorld), bodyWorld);
		// A palm past either end holds the end, not thin air.
		const float clampedU = alongBone < -1.0f ? -1.0f : (alongBone > 1.0f ? 1.0f : alongBone);
		const float touchedMetres = axis.centre + clampedU * axis.half;
		const float nodeScale = nodeWorld.scale != 0.0f ? nodeWorld.scale : 1.0f;
		const float touchedLocal = GP::Get(bodyInNode.pos, axis.index) + touchedMetres * kUnitsPerMetre / nodeScale;

		const GP::Xform hold = GP::HoldPose(natural, palmInHand, axis.index, touchedLocal, reversed, flip.axis);
		if (!Finite(hold))
		{
			LOG_ERR("Grab probe [%s] %s: the computed hold is not a number - HIGGS's hold left alone.", hand, name);
			return;
		}

		higgsInterface->SetGrabTransform(isLeftVRController, FromXform(hold));
		follow.targetInHand = hold;
		follow.targetIsOurs = true;
		const GP::Vec3 palmNatural = GP::Apply(GP::Inverse(natural), palmInHand);
		LOG_INFO(
			"Grab probe [%s] %s: HIGGS told to hold it with the palm %.1fu further along the weapon than its natural "
			"grip%s (weapon position in the hand (%.1f, %.1f, %.1f), was (%.1f, %.1f, %.1f)).",
			hand, name,
			touchedLocal - GP::Get(palmNatural, axis.index),
			reversed ? ", TURNED OVER" : "",
			hold.pos.x, hold.pos.y, hold.pos.z,
			natural.pos.x, natural.pos.y, natural.pos.z);
	}

	void GrabPosePublishForEquip(const bool isLeftGameHand, const UInt32 weaponFormID)
	{
		const GrabGrip::Choice choice = CurrentGrabGripChoice();
		if (!choice.position && !choice.reversed)
		{
			return;
		}

		// The record belongs to the CONTROLLER that grabbed (or, for a hand switch, that took it over); the equip
		// task knows the game hand.
		const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
		Kept& kept = s_kept[isLeftVRController ? 1 : 0];
		const GrabGrip::Touch touch = kept.touch;
		const float age = touch.valid ? std::chrono::duration<float>(Clock::now() - kept.when).count() : 0.0f;
		kept = Kept{}; // used up, whatever happens next

		const char* name = GetSafeFormName(LookupFormByID(weaponFormID));
		const GrabGrip::Why why = GrabGrip::Judge(touch, weaponFormID, age, grabGripPulled);
		if (why != GrabGrip::Why::kSend)
		{
			LOG_INFO("Grab grip: nothing sent for %s - %s.", name, WhyText(why));
			return;
		}

		if (!GrabGripHasListener())
		{
			LOG_INFO(
				"Grab grip: %s is about to be equipped, but no grip driver is listening (it needs a TrueWield VR with "
				"grab-grip support) - held the natural way.",
				name);
			return;
		}

		const SdhGrabGripAPI::GrabGripEvent event = GrabGrip::BuildEvent(touch, isLeftGameHand, choice.reversed,
			grabGripReversePulled, choice.position, choice.reversedTwoHanded, choice.anywhere);
		if (!GrabGrip::WorthSending(event))
		{
			LOG_INFO(
				"Grab grip: nothing sent for %s - \"Hold where you grab\" is off in this mode and it was not taken hold "
				"of backwards, so it is held the natural way.",
				name);
			return;
		}

		const std::uint32_t answer = GrabGripPublish(event);
		// Room for two floats at their longest: sprintf_s does not truncate, it ends the game.
		char where[192];
		if (event.flags & SdhGrabGripAPI::kHasAlong)
		{
			sprintf_s(where, "along %+.2f, hilt %+.2f%s", event.alongU, event.hiltU,
				(event.flags & SdhGrabGripAPI::kAnywhere) ? ", anywhere - above the guard too, never through it" : "");
		}
		else
		{
			sprintf_s(where, "at its natural grip");
		}
		const char* answerText = (answer & SdhGrabGripAPI::kWillFlip)
			? ((answer & SdhGrabGripAPI::kWillSlide) ? "will slide and turn it over" : "will turn it over")
			: ((answer & SdhGrabGripAPI::kWillSlide) ? "will slide" : "ignored");
		LOG_INFO(
			"Grab grip: sent before equipping %s, %.2fs after the %s (%s%s) - it answered: %s.",
			name, age, touch.handOff ? "hand switch" : "grab", where,
			(event.flags & SdhGrabGripAPI::kReversed) ? ", REVERSED" : "",
			answerText);
	}

	void GrabPoseOnHandOff(const bool isLeftHoldingGameHand, const UInt32 weaponFormID)
	{
		const bool isLeftReceivingGameHand = !isLeftHoldingGameHand;
		const bool holdingVR = GameHandToVRController(isLeftHoldingGameHand);
		const bool receivingVR = GameHandToVRController(isLeftReceivingGameHand);
		const char* hand = receivingVR ? "left" : "right";

		// A hand switch replaces whatever the receiving hand kept.
		Kept& kept = s_kept[receivingVR ? 1 : 0];
		kept = Kept{};

		const GrabGrip::Choice choice = CurrentGrabGripChoice();
		if ((!choice.position && !choice.reversed && !grabGripTrace) || !higgsInterface)
		{
			return;
		}

		TESForm* form = LookupFormByID(weaponFormID);
		auto* weapon = form ? DYNAMIC_CAST(form, TESForm, TESObjectWEAP) : nullptr;
		if (!weapon || !GP::IsGripMelee(weapon->type()))
		{
			return;
		}
		const int weaponType = weapon->type();
		const char* name = GetSafeFormName(form);

		// The weapon as the hand holding it holds it: HIGGS's weapon body follows the drawn weapon, and PCVR's
		// reversal is on that hand's attach node.
		NiObject* body = higgsInterface->GetWeaponRigidBody(holdingVR);
		GP::Mat3 bodyRot;
		GP::Vec3 bodyPosMetres;
		GP::Box box;
		const bool haveBody = body && ReadBody(body, bodyRot, bodyPosMetres) && ReadLocalBox(body, box);
		NiAVObject* handBone = HiggsHandBone(receivingVR);
		NiAVObject* holdingAttach = WeaponAttachNode(isLeftHoldingGameHand);
		NiAVObject* receivingAttach = WeaponAttachNode(isLeftReceivingGameHand);
		if (!haveBody || !handBone || !holdingAttach || !receivingAttach)
		{
			LOG_INFO(
				"Hand switch: the %s hand takes %s over, but %s could not be read - held the natural way.",
				hand, name,
				!haveBody ? "HIGGS's weapon body" : (!handBone ? "that hand's bone" : "a weapon attach node"));
			return;
		}

		const GP::LongAxis axis = GP::FindLongAxis(box, 0.05f);
		const float hiltU = GP::HiltU(axis);
		const int tipSign = GP::TipSign(hiltU);
		const GP::Vec3 palmWorld = GP::Apply(ToXform(handBone->m_worldTransform), PalmInHand(receivingVR));
		const GP::Vec3 palmLocalMetres =
			GP::ToLocal(bodyRot, bodyPosMetres, GP::Scale(palmWorld, 1.0f / kUnitsPerMetre));
		const float alongU = GP::AlongU(axis, palmLocalMetres);
		const float offAxisM = GP::OffAxis(axis, box, palmLocalMetres);

		const GP::Vec3 bladeWorld = GP::Mul(bodyRot, GP::AxisUnit(axis.index, tipSign == 0 ? 1 : tipSign));
		const GP::Mat3 holdingRot = ToMat(holdingAttach->m_worldTransform.rot);
		const GP::Mat3 receivingRot = ToMat(receivingAttach->m_worldTransform.rot);
		const float cone = GP::ClampCone(grabGripReversedCone);
		const GP::Reverse reverse = HandSwitch::JudgeReversed(holdingRot, receivingRot, bladeWorld, cone);
		const bool mayReverse =
			GrabGrip::MayReverse(weaponType, axis.ambiguous, tipSign, false, false, false, choice.reversedTwoHanded);

		GP::FlipChoice flip = GP::ChooseFlipAxis(receivingRot, holdingRot, axis.index);
		if (!flip.decided)
		{
			flip.axis = WiderAcross(box, axis.index);
		}

		kept.when = Clock::now();
		kept.touch.valid = true;
		kept.touch.refID = 0;
		kept.touch.formID = weaponFormID;
		kept.touch.isLeftVRController = receivingVR;
		kept.touch.weaponType = weaponType;
		kept.touch.longAxis = axis.index;
		kept.touch.axisUsable = axis.usable;
		kept.touch.axisAmbiguous = axis.ambiguous;
		kept.touch.alongU = alongU;
		kept.touch.hiltU = hiltU;
		kept.touch.lengthM = axis.length;
		kept.touch.offAxisM = offAxisM;
		kept.touch.tipSign = tipSign;
		kept.touch.pulled = false;
		kept.touch.warped = false;
		kept.touch.reversedAngleDeg = reverse.angleDeg;
		kept.touch.reversedVerdict = reverse.reversed && mayReverse;
		kept.touch.flipDecided = flip.decided;
		kept.touch.flipAxis = flip.axis;
		kept.touch.handOff = true;

		const GrabGrip::Why why = GrabGrip::Judge(kept.touch, weaponFormID, 0.0f, grabGripPulled);
		if (why != GrabGrip::Why::kSend)
		{
			LOG_INFO("Hand switch: the %s hand takes %s over, but %s - it will be held the natural way.", hand, name, WhyText(why));
			return;
		}

		char where[96];
		if (tipSign != 0)
		{
			sprintf_s(where, "%.0f%% of the way up from the hilt", GP::PercentFromHilt(alongU, hiltU, tipSign));
		}
		else
		{
			sprintf_s(where, "at %+.2f along it (which end is the tip is unclear)", alongU);
		}
		LOG_INFO(
			"Hand switch: the %s hand holds %s %s, %.1fcm off its centre line - %s (%.0f degrees from the natural way round%s).",
			hand, name, where, offAxisM * 100.0f,
			kept.touch.reversedVerdict ? "REVERSED" : "normal grip",
			reverse.angleDeg,
			reverse.reversed && !mayReverse ? ", but not allowed to reverse" : "");
	}
}
