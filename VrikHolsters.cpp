#include "VrikHolsters.h"

#include "Engine.h"
#include "HolsterBodyRules.h"
#include "HolsterLearnRules.h"
#include "RawHavok.h"
#include "TrackedWeapons.h"
#include "VrikHolsterApi.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/PluginAPI.h>
#include <skse64/GameReferences.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		// vrikslots.ini numbers its slots in this order. The anchor bone for each is
		// documented nowhere, but the configured offsets say a great deal about it:
		// the limb slots MIRROR in x between left and right (left thigh +7.5, right
		// thigh -7.3), which is the signature of each hanging off its own mirrored
		// limb bone, while hips and shoulders do NOT mirror (left hip -11.8, right
		// hip +11.9), which is the signature of one shared central anchor.
		//
		// The mapping below is the best reading of that. Every entry is re-measured
		// by the trace, so a wrong guess shows up plainly as a distance that never
		// collapses while the player is demonstrably over that holster.
		struct SlotDef
		{
			const char* name;
			const char* anchorBone;
		};

		static const SlotDef kSlots[14] = {
			{ "Left Hip",       "NPC Pelvis [Pelv]" },
			{ "Right Hip",      "NPC Pelvis [Pelv]" },
			{ "Left Thigh",     "NPC L Thigh [LThg]" },
			{ "Right Thigh",    "NPC R Thigh [RThg]" },
			{ "Left Calf",      "NPC L Calf [LClf]" },
			{ "Right Calf",     "NPC R Calf [RClf]" },
			{ "Left UpperArm",  "NPC L UpperArm [LUar]" },
			{ "Right UpperArm", "NPC R UpperArm [RUar]" },
			{ "Left Forearm",   "NPC L Forearm [LLar]" },
			{ "Right Forearm",  "NPC R Forearm [RLar]" },
			{ "Left Shoulder",  "NPC Spine2 [Spn2]" },
			{ "Right Shoulder", "NPC Spine2 [Spn2]" },
			{ "Stomach",        "NPC Spine [Spn0]" },
			{ "Chest",          "NPC Spine2 [Spn2]" },
		};

		struct SlotState
		{
			NiPoint3 offset;
			int handRule = 0; // 0 = either hand, 1 = left only, 2 = right only
			bool visible = true;
			// The slot position measured off real VRIK interactions, in the anchor
			// bone frame. When present it replaces the ini-derived estimate above.
			//
			// AVERAGED, not overwritten. A draw or a holster happens with the hand
			// NEAR the slot, not dead on its centre, so single samples scatter by
			// several units - slot 10 spread from x -9.8 to +1.9 across one session.
			// The mean of those converges on the centre; one sample does not.
			bool learned = false;
			NiPoint3 learnedOffset;
			NiPoint3 sampleSum;
			int sampleCount = 0;

			// An estimate borrowed from a LEARNED slot on the same bone. See
			// RebuildSlotSeeds - this is what lets a slot work before anyone has
			// managed to use it once.
			bool seeded = false;
			NiPoint3 seededOffset;

			// The current run of samples the outlier filter turned away. Rejections that agree
			// with each other are the slot rather than noise, so they replace a mean that was
			// built on bad samples - see HolsterLearnRules.h. Not saved: a run means nothing
			// once the session ends.
			NiPoint3 rejectedFirst;
			NiPoint3 rejectedSum;
			int rejectedCount = 0;
		};

		static SlotState s_slots[14];
		static float s_activationDistance = 8.0f;
		static bool s_holstersEnabled = false;
		static bool s_haveSettings = false;
		static int s_settingsAttempts = 0;
		static const char* s_lastPositionSource = nullptr;

		// Where the palm sits relative to the hand BONE.
		//
		// VRIK measures its holsters from the palm. We were measuring from the WEAPON
		// rigid body, which sits wherever that particular weapon's origin happens to
		// be - so the same reach measured differently for a dagger and a greatsword,
		// and worst of all at the shoulders, where reaching over puts the weapon far
		// behind the hand that is doing the reaching.
		//
		// The hand bone is honest: never parked, never moves with the weapon. It is
		// just at the wrist rather than the palm. So measure that difference ONCE,
		// while HIGGS's hand body is live - which it is whenever that hand holds
		// nothing - and keep it as an offset in the bone's own frame. After that the
		// palm is available with a weapon in hand, at any size, forever.
		struct PalmCalibration
		{
			bool have = false;
			NiPoint3 local;
			NiPoint3 sum;
			int samples = 0;
		};
		static PalmCalibration s_palm[2];

		void RebuildSlotSeeds();
		NiPoint3 BoneLocalToWorld(NiAVObject* bone, const NiPoint3& local);

		double VrikSetting(const char* key, const double fallback)
		{
			if (!vrikInterface)
			{
				return fallback;
			}
			return vrikInterface->getSettingDouble(key);
		}

		NiAVObject* FindBone(PlayerCharacter* player, const char* boneName)
		{
			if (!player || !boneName)
			{
				return nullptr;
			}

			NiNode* root = player->GetNiNode();
			if (!root)
			{
				return nullptr;
			}

			BSFixedString name(boneName);
			return root->GetObjectByName(&name.data);
		}

		// The bone a holster slot hangs off. FindBone above has always searched player->GetNiNode(),
		// which on this runtime is the same node as GetNiRootNode(1). The M2.2.3-test2 probe found
		// GetNiRootNode(0) to be a different skeleton, every body bone 8 to 13.5 units away and both
		// hands identical. bHolsterThirdPersonBody moves the slot anchors onto it; the hands stay on
		// FindBone, where it makes no difference. HolsterBodyRules.h.
		NiAVObject* FindAnchorBone(PlayerCharacter* player, const char* boneName)
		{
			if (!holsterThirdPersonBody)
			{
				return FindBone(player, boneName);
			}

			if (!player || !boneName)
			{
				return nullptr;
			}

			NiNode* const body = player->GetNiRootNode(0);
			if (!body)
			{
				return nullptr;
			}

			BSFixedString name(boneName);
			return body->GetObjectByName(&name.data);
		}

		NiAVObject* FindHandNode(PlayerCharacter* player, const bool isLeftVRController)
		{
			const bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);
			return FindBone(player, isLeftGameHand ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]");
		}

		// Where the hand actually is, for holster purposes.
		//
		// The hand BONE sits at the wrist, and VRIK measures its holsters from the palm.
		// That difference is several units and it is not symmetric, because it shifts
		// with how the hand is turned: reaching for one hip landed 3 to 5 units from
		// the slot while the other hip sat at 15 to 18 and so never came inside the
		// 8-unit activation range at all. HIGGS's hand body is at the palm, which is
		// the same place VRIK is measuring from, so prefer it and keep the bone as a
		// fallback for when HIGGS is not up yet.
		// Where the hand-with-weapon actually is. THE SOURCE MATTERS MORE THAN THE
		// MATH: the WEAPON body is the thing HIGGS actively simulates while a weapon
		// is drawn - it is what every throw is measured from, proven fresh at release.
		// The HAND body is parked by HIGGS while a weapon is drawn, and a parked body
		// reports wherever it was last active. Reading it made every release measure
		// "near" the last holster the player used: sixteen of nineteen matches in one
		// session said Left Hip, including releases made at the top of the head.
		const char* GetHandPosition(PlayerCharacter* player, const bool isLeftVRController, NiPoint3& outPos)
		{
			// The calibrated palm first: it is the same point VRIK measures from, it is
			// the same point whatever weapon is in the hand, and the bone it rides on is
			// never parked. Everything below it is a fallback from before this existed.
			const PalmCalibration& palm = s_palm[isLeftVRController ? 1 : 0];
			if (palm.have)
			{
				NiAVObject* handBone = FindHandNode(player, isLeftVRController);
				if (handBone)
				{
					outPos = BoneLocalToWorld(handBone, palm.local);
					return "palm";
				}
			}

			if (higgsInterface)
			{
				NiMatrix33 rot;
				NiPoint3 posMeters;

				if (RawHavok::ReadHkTransform(
						higgsInterface->GetWeaponRigidBody(isLeftVRController), rot, posMeters))
				{
					outPos.x = posMeters.x * RawHavok::kHavokToSkyrim;
					outPos.y = posMeters.y * RawHavok::kHavokToSkyrim;
					outPos.z = posMeters.z * RawHavok::kHavokToSkyrim;
					return "weapon-body";
				}

				if (RawHavok::ReadHkTransform(
						higgsInterface->GetHandRigidBody(isLeftVRController), rot, posMeters))
				{
					outPos.x = posMeters.x * RawHavok::kHavokToSkyrim;
					outPos.y = posMeters.y * RawHavok::kHavokToSkyrim;
					outPos.z = posMeters.z * RawHavok::kHavokToSkyrim;
					return "hand-body";
				}
			}

			NiAVObject* hand = FindHandNode(player, isLeftVRController);
			if (!hand)
			{
				return nullptr;
			}

			outPos = hand->m_worldTransform.pos;
			return "hand-bone";
		}
		// A world point expressed in the anchor bone's local space. The slot offsets
		// are authored in that frame, which is what makes the two comparable.
		bool WorldToBoneLocal(NiAVObject* bone, const NiPoint3& world, NiPoint3& outLocal)
		{
			if (!bone)
			{
				return false;
			}

			const NiPoint3 d(
				world.x - bone->m_worldTransform.pos.x,
				world.y - bone->m_worldTransform.pos.y,
				world.z - bone->m_worldTransform.pos.z);

			const NiMatrix33& r = bone->m_worldTransform.rot;
			const float scale = bone->m_worldTransform.scale != 0.0f
				? bone->m_worldTransform.scale
				: 1.0f;

			// The rotation is orthonormal, so its inverse is its transpose.
			outLocal.x = (r.data[0][0] * d.x + r.data[1][0] * d.y + r.data[2][0] * d.z) / scale;
			outLocal.y = (r.data[0][1] * d.x + r.data[1][1] * d.y + r.data[2][1] * d.z) / scale;
			outLocal.z = (r.data[0][2] * d.x + r.data[1][2] * d.y + r.data[2][2] * d.z) / scale;
			return true;
		}

		// The inverse of WorldToBoneLocal.
		NiPoint3 BoneLocalToWorld(NiAVObject* bone, const NiPoint3& local)
		{
			const NiMatrix33& r = bone->m_worldTransform.rot;
			const float scale = bone->m_worldTransform.scale != 0.0f
				? bone->m_worldTransform.scale
				: 1.0f;

			return NiPoint3(
				bone->m_worldTransform.pos.x +
					scale * (r.data[0][0] * local.x + r.data[0][1] * local.y + r.data[0][2] * local.z),
				bone->m_worldTransform.pos.y +
					scale * (r.data[1][0] * local.x + r.data[1][1] * local.y + r.data[1][2] * local.z),
				bone->m_worldTransform.pos.z +
					scale * (r.data[2][0] * local.x + r.data[2][1] * local.y + r.data[2][2] * local.z));
		}

		// Is the hand behind the player's body?
		//
		// Read forward exactly the way GetPlayerRightVector reads right - off the root
		// node's world rotation - and flatten it, so looking up or down cannot turn "in
		// front of me" into "behind me".
		bool IsHandBehindPlayer(PlayerCharacter* player, const NiPoint3& handWorld)
		{
			if (!player)
			{
				return false;
			}

			NiNode* rootNode = player->GetNiRootNode(0);
			if (!rootNode)
			{
				rootNode = player->GetNiRootNode(1);
			}
			if (!rootNode)
			{
				return false;
			}

			NiPoint3 forward(
				rootNode->m_worldTransform.rot.data[0][1],
				rootNode->m_worldTransform.rot.data[1][1],
				0.0f);
			const float len = std::sqrt(forward.x * forward.x + forward.y * forward.y);
			if (len < 0.0001f)
			{
				return false;
			}
			forward.x /= len;
			forward.y /= len;

			const NiPoint3& body = rootNode->m_worldTransform.pos;
			const float ahead =
				(handWorld.x - body.x) * forward.x + (handWorld.y - body.y) * forward.y;
			return ahead < 0.0f;
		}

		// How far from a slot still counts as being at it, for a hand in this position.
		//
		// Tight in front, generous behind. In front of you is where you deliberately
		// drop things, so a wide catch there steals drops and turns them into holsters.
		// Behind your back nobody ever means to drop a weapon, so there is nothing to
		// steal and the only thing width costs is nothing at all - while it buys the
		// back slots, which are the hardest to reach precisely and the ones VRIK's own
		// ini places worst. Both shoulders were measured 10 to 11 units from where the
		// ini put them.
		float CatchRangeForHand(
			PlayerCharacter* player,
			const NiPoint3& handWorld,
			const HolsterRange::Reach& reach = HolsterRange::Reach{})
		{
			return HolsterRange::CatchRange(
				s_activationDistance,
				holsterExtraRange,
				holsterExtraRangeBehind,
				IsHandBehindPlayer(player, handWorld),
				reach);
		}

		float Distance(const NiPoint3& a, const NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		void ScanForVrikNodes(NiAVObject* node, const int depth, int& logged)
		{
			if (!node || depth > 6 || logged > 40)
			{
				return;
			}

			const char* name = node->m_name;
			if (name && name[0])
			{
				std::string lowered(name);
				for (char& c : lowered)
				{
					c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
				}

				if (lowered.find("holster") != std::string::npos ||
					lowered.find("vrik") != std::string::npos ||
					lowered.find("slot") != std::string::npos ||
					lowered.find("sphere") != std::string::npos)
				{
					++logged;
					LOG_INFO(
						"Holster nodes: depth %d \"%s\" world=[%7.2f %7.2f %7.2f]",
						depth,
						name,
						node->m_worldTransform.pos.x,
						node->m_worldTransform.pos.y,
						node->m_worldTransform.pos.z);
				}
			}

			NiNode* asNode = node->GetAsNiNode();
			if (!asNode)
			{
				return;
			}

			for (UInt32 i = 0; i < asNode->m_children.m_arrayBufLen; ++i)
			{
				ScanForVrikNodes(asNode->m_children.m_data[i], depth + 1, logged);
			}
		}

		std::string HolsterStateFilePath()
		{
			const std::string runtimeDirectory = GetRuntimeDirectory();
			if (runtimeDirectory.empty())
			{
				return std::string();
			}

			return runtimeDirectory + "Data\\SKSE\\Plugins\\" + PLUGIN_FILE_NAME + "_holsters.ini";
		}

		void SaveLearnedSlots()
		{
			const std::string path = HolsterStateFilePath();
			if (path.empty())
			{
				return;
			}

			std::ofstream file(path, std::ios::trunc);
			if (!file.is_open())
			{
				LOG_ERR("Holster learning: could not write %s", path.c_str());
				return;
			}

			file << "; Written by Swap Drop & Hold redux - VRIK slot positions learned\n";
			file << "; from real draws, holsters and VRIK's own holster spheres.\n";
			file << "; Delete this file to forget them.\n";
			file << "Format = 5\n";
			for (int i = 0; i < 14; ++i)
			{
				if (s_slots[i].learned)
				{
					file << "Slot" << (i + 1) << " = "
						<< s_slots[i].learnedOffset.x << ", "
						<< s_slots[i].learnedOffset.y << ", "
						<< s_slots[i].learnedOffset.z << ", "
						<< s_slots[i].sampleCount << "\n";
				}
			}
		}

		void LoadLearnedSlots()
		{
			const std::string path = HolsterStateFilePath();
			if (path.empty())
			{
				return;
			}

			std::ifstream file(path);
			if (!file.is_open())
			{
				return;
			}

			int loaded = 0;
			int format = 0;
			std::string line;
			while (std::getline(file, line))
			{
				if (line.empty() || line[0] == ';')
				{
					continue;
				}

				if (sscanf_s(line.c_str(), "Format = %d", &format) == 1)
				{
					continue;
				}

				// Format 1 files predate the junk filter; format 2 files were still
				// being written by the sphere scan, which turned out to be pinning two
				// decoy nodes onto a different slot every couple of seconds. Format 3
				// was measured against the WEAPON rigid body, so every position in it
				// carries that weapon's offset baked in and means nothing now that the
				// palm is what gets measured. Format 4 learned from any hand at any slot, and
				// a hand is always close to its own forearm slot - it is attached to that
				// arm - so every grab taught the forearm holsters where the wrists were:
				// one install had slot 10 averaged over 302 grabs onto the right wrist.
				// None of the four is worth keeping.
				if (format < 5)
				{
					LOG_INFO(
						"Holster learning: ignoring learned positions from before 2.1.0 - grabbing "
						"weapons was being learned as holstering, which dragged the forearm slots "
						"onto your wrists. They are learned again from real draws.");
					return;
				}

				int slot = 0;
				float x = 0.0f;
				float y = 0.0f;
				float z = 0.0f;
				int count = 1;
				if (sscanf_s(line.c_str(), "Slot%d = %f, %f, %f, %d", &slot, &x, &y, &z, &count) >= 4 &&
					slot >= 1 && slot <= 14)
				{
					if (count < 1)
					{
						count = 1;
					}

					s_slots[slot - 1].learned = true;
					s_slots[slot - 1].learnedOffset = NiPoint3(x, y, z);
					s_slots[slot - 1].sampleCount = count;
					s_slots[slot - 1].sampleSum =
						NiPoint3(x * count, y * count, z * count);
					++loaded;
				}
			}

			if (loaded > 0)
			{
				LOG_INFO("Holster learning: %d learned slot position(s) restored.", loaded);
			}

			// Slots that were never learned inherit from those that were.
			RebuildSlotSeeds();
		}

		const NiPoint3& SlotTarget(const int slotIndex)
		{
			const SlotState& slot = s_slots[slotIndex];
			switch (HolsterBody::SourceFor(holsterThirdPersonBody, slot.learned, slot.seeded))
			{
			case HolsterBody::Centre::kLearned:
				return slot.learnedOffset;
			case HolsterBody::Centre::kSeeded:
				return slot.seededOffset;
			default:
				return slot.offset;
			}
		}

		// Give a slot nobody has managed to use the correction from one that works.
		//
		// The error in these estimates is not random: it is our reconstruction of a
		// BONE's frame being off, so every slot hanging off that bone is wrong in the
		// same direction by roughly the same amount. Chest and both Shoulders all hang
		// off Spine2, and on this machine the chest was measured 17 units from its ini
		// estimate while a shoulder had never been learned at all.
		//
		// That mattered because the two facts made a circle: a slot outside the catch
		// radius can never be used, and a slot that is never used can never be learned.
		// Borrowing the correction breaks it - the shoulder starts life roughly right,
		// gets reached, and then learns its own true position and stops borrowing.
		void RebuildSlotSeeds()
		{
			for (int i = 0; i < 14; ++i)
			{
				s_slots[i].seeded = false;
			}

			for (int i = 0; i < 14; ++i)
			{
				if (s_slots[i].learned)
				{
					continue;
				}

				NiPoint3 sum(0.0f, 0.0f, 0.0f);
				int donors = 0;
				for (int j = 0; j < 14; ++j)
				{
					// Three samples is where a mean stops being one gesture's noise.
					if (j == i || !s_slots[j].learned || s_slots[j].sampleCount < 3)
					{
						continue;
					}
					if (strcmp(kSlots[i].anchorBone, kSlots[j].anchorBone) != 0)
					{
						continue;
					}

					sum.x += s_slots[j].learnedOffset.x - s_slots[j].offset.x;
					sum.y += s_slots[j].learnedOffset.y - s_slots[j].offset.y;
					sum.z += s_slots[j].learnedOffset.z - s_slots[j].offset.z;
					++donors;
				}

				if (donors == 0)
				{
					continue;
				}

				s_slots[i].seededOffset.x = s_slots[i].offset.x + sum.x / donors;
				s_slots[i].seededOffset.y = s_slots[i].offset.y + sum.y / donors;
				s_slots[i].seededOffset.z = s_slots[i].offset.z + sum.z / donors;
				s_slots[i].seeded = true;

				LOG_INFO(
					"Holster learning: slot %d %s has never been used, so it borrows the "
					"correction from %d learned slot(s) on %s - estimate moved %.1f units.",
					i + 1,
					kSlots[i].name,
					donors,
					kSlots[i].anchorBone,
					Distance(s_slots[i].seededOffset, s_slots[i].offset));
			}
		}

		// VRIK's own rule for which hand may use a slot (handSlotN): 0 = either hand,
		// 1 = left only, 2 = right only, in game hands.
		bool SlotAcceptsHand(const int slotIndex, const bool isLeftVRController)
		{
			const int rule = s_slots[slotIndex].handRule;
			if (rule == 0)
			{
				return true;
			}

			const bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);
			return (rule == 1) ? isLeftGameHand : !isLeftGameHand;
		}

		// A hand cannot draw from, or holster onto, its own arm. It is also never far from
		// that arm's slots - it is attached to the arm - so without this the nearest slot
		// to anything a hand did was its own forearm, whatever VRIK's hand rule said.
		bool SlotOnOwnArm(const int slotIndex, const bool isLeftVRController)
		{
			const std::string bone(kSlots[slotIndex].anchorBone);
			if (bone.find("Forearm") == std::string::npos && bone.find("UpperArm") == std::string::npos)
			{
				return false;
			}

			const bool leftArm = bone.find("NPC L ") != std::string::npos;
			return leftArm == isLeftVRController;
		}

		// THE A/B. Called at every real VRIK interaction - a press-holster VRIK completed, or a draw -
		// with the hand where VRIK activated a slot. VRIK only activates a slot with the hand inside
		// its activation distance of that slot ON ITS OWN BODY, so the skeleton VRIK uses has to show
		// the hand inside that reach. Both skeletons are measured with VRIK's own ini positions, no
		// learned corrections, whatever bHolsterThirdPersonBody says, so every session collects
		// evidence either way. Also logs the Spine2 heading on both: both shoulders hang off that one
		// bone, so only a ROTATION between the two bodies could make one shoulder worse than the other.
		// The nearest slot to a hand by VRIK's OWN positions, on each skeleton, 0-based. Shared by the
		// A/B below and HolsterNearestRawSlotOnBothBodies.
		bool NearestRawSlotOnBothBodies(
			NiNode* const firstPerson,
			NiNode* const body,
			const bool isLeftVRController,
			const NiPoint3& handWorld,
			int& outFirstSlot,
			float& outFirstDistance,
			int& outBodySlot,
			float& outBodyDistance)
		{
			outFirstSlot = -1;
			outBodySlot = -1;
			outFirstDistance = 1.0e9f;
			outBodyDistance = 1.0e9f;
			if (!firstPerson || !body)
			{
				return false;
			}

			for (int i = 0; i < 14; ++i)
			{
				if (!SlotAcceptsHand(i, isLeftVRController) || SlotOnOwnArm(i, isLeftVRController))
				{
					continue;
				}

				BSFixedString boneName(kSlots[i].anchorBone);
				NiPoint3 local;
				if (WorldToBoneLocal(firstPerson->GetObjectByName(&boneName.data), handWorld, local))
				{
					const float d = Distance(local, s_slots[i].offset);
					if (d < outFirstDistance)
					{
						outFirstDistance = d;
						outFirstSlot = i;
					}
				}
				if (WorldToBoneLocal(body->GetObjectByName(&boneName.data), handWorld, local))
				{
					const float d = Distance(local, s_slots[i].offset);
					if (d < outBodyDistance)
					{
						outBodyDistance = d;
						outBodySlot = i;
					}
				}
			}

			return outFirstSlot >= 0 && outBodySlot >= 0;
		}

		void LogBodyComparison(
			PlayerCharacter* player,
			const bool isLeftVRController,
			const NiPoint3& handWorld,
			const char* const how)
		{
			static int s_comparisonsLeft = 60;
			if (s_comparisonsLeft <= 0 || !player || !s_haveSettings)
			{
				return;
			}

			// The two ROOTS, by name. Never GetNiNode(): it moves between them within a session, and
			// M2.2.5-test1 compared it against GetNiRootNode(0) - measuring one skeleton twice once
			// they coincided, which lost all seven of that session's comparisons.
			NiNode* const firstPerson = player->GetNiRootNode(1);
			NiNode* const body = player->GetNiRootNode(0);
			if (!firstPerson || !body)
			{
				return;
			}
			--s_comparisonsLeft;

			const HolsterBody::Root nodeIs = HolsterBody::WhichRoot(player->GetNiNode(), body, firstPerson);

			if (firstPerson == body)
			{
				LOG_INFO(
					"Holster body A/B (%s): GetNiRootNode(0) IS GetNiRootNode(1) here - one skeleton, nothing to compare.",
					how);
				return;
			}

			int firstSlot = -1;
			int bodySlot = -1;
			float firstDistance = 1.0e9f;
			float bodyDistance = 1.0e9f;
			if (!NearestRawSlotOnBothBodies(
					firstPerson, body, isLeftVRController, handWorld, firstSlot, firstDistance, bodySlot, bodyDistance))
			{
				return;
			}

			const char* verdict = "";
			switch (HolsterBody::Judge(firstDistance, bodyDistance, s_activationDistance, HolsterBody::kPalmTolerance))
			{
			case HolsterBody::Verdict::kOnlyThirdPersonBody:
				verdict = "ONLY THE THIRD-PERSON BODY puts the hand in VRIK's reach - evidence FOR it being VRIK's body.";
				break;
			case HolsterBody::Verdict::kOnlyFirstPerson:
				verdict = "ONLY THE FIRST-PERSON SKELETON puts the hand in VRIK's reach - evidence AGAINST the switch.";
				break;
			case HolsterBody::Verdict::kBothInside:
				verdict = "Both put the hand in VRIK's reach - this one does not decide it.";
				break;
			default:
				verdict = "Neither puts the hand in VRIK's reach - the slot-to-bone mapping or the hand position is off too.";
				break;
			}

			LOG_INFO(
				"Holster body A/B (%s, %s hand): with VRIK's own slot positions the hand is %.1f from %s on the "
				"first-person skeleton and %.1f from %s on the third-person body (VRIK activates within %.0f, "
				"+%.0f palm tolerance). %s GetNiNode() is %s.",
				how,
				isLeftVRController ? "left" : "right",
				firstDistance,
				kSlots[firstSlot].name,
				bodyDistance,
				kSlots[bodySlot].name,
				s_activationDistance,
				HolsterBody::kPalmTolerance,
				verdict,
				HolsterBody::RootName(nodeIs));

			BSFixedString spine2("NPC Spine2 [Spn2]");
			NiAVObject* const firstSpine = firstPerson->GetObjectByName(&spine2.data);
			NiAVObject* const bodySpine = body->GetObjectByName(&spine2.data);
			if (firstSpine && bodySpine)
			{
				const NiMatrix33& a = firstSpine->m_worldTransform.rot;
				const NiMatrix33& b = bodySpine->m_worldTransform.rot;
				const float headingFirst = HolsterBody::HeadingDegrees(a.data[0][0], a.data[1][0]);
				const float headingBody = HolsterBody::HeadingDegrees(b.data[0][0], b.data[1][0]);
				LOG_INFO(
					"Holster body A/B: Spine2 heading %.1f on the first-person skeleton, %.1f on the third-person "
					"body - turned %+.1f degrees. Both shoulders hang off this bone, so a turn here moves one "
					"shoulder slot forward and the other back.",
					headingFirst,
					headingBody,
					HolsterBody::HeadingDifference(headingFirst, headingBody));
			}
		}

		// A world-space hand position known to be exactly ON a VRIK slot. Find which
		// slot, and record the measured position as that slot's target from now on.
		//
		// Only slots this hand can use are candidates. Before 2.1.0 every slot was, and
		// every event lands within 20 units of the hand's own forearm slot, so grabbing
		// weapons kept teaching the forearm holsters where the wrists were.
		void LearnSlotFromWorldPosition(const bool isLeftVRController, const NiPoint3& handWorld, const char* how)
		{
			PlayerCharacter* player = *g_thePlayer;

			// The A/B runs first, on every real interaction, whatever the settings below decide.
			LogBodyComparison(player, isLeftVRController, handWorld, how);

			if (!player || !s_haveSettings || !holsterLearning)
			{
				return;
			}

			if (!HolsterBody::MayLearn(holsterThirdPersonBody))
			{
				return;
			}

			int bestSlot = -1;
			float bestDist = 20.0f; // beyond this it was not a slot interaction at all
			NiPoint3 bestLocal;
			for (int i = 0; i < 14; ++i)
			{
				if (!SlotAcceptsHand(i, isLeftVRController) || SlotOnOwnArm(i, isLeftVRController))
				{
					continue;
				}

				NiPoint3 local;
				if (!WorldToBoneLocal(FindAnchorBone(player, kSlots[i].anchorBone), handWorld, local))
				{
					continue;
				}

				const float d = Distance(local, SlotTarget(i));
				if (d < bestDist)
				{
					bestDist = d;
					bestSlot = i;
					bestLocal = local;
				}
			}

			if (bestSlot < 0)
			{
				LOG_INFO(
					"Holster learning: %s by the %s hand happened nowhere near a slot that hand can use - not learned.",
					how,
					isLeftVRController ? "left" : "right");
				return;
			}

			SlotState& slot = s_slots[bestSlot];

			// Once a mean exists, a sample far from it is a different gesture that
			// happened to be nearest this slot - not evidence about where it is.
			const float fromMean = Distance(bestLocal, slot.learnedOffset);
			if (HolsterLearn::IsOutlier(slot.sampleCount, fromMean))
			{
				// ...unless the turned-away samples agree with EACH OTHER, which noise does not
				// do. Then they are the slot and the stored mean is the mistake. Without this a
				// slot whose first three samples were junk turns away every correct sample after
				// them for good: the 2026-09-15 log caught the Right Hip slot doing it four times
				// in a row at 16.7 to 19.9 units.
				const float fromRun = Distance(bestLocal, slot.rejectedFirst);
				if (slot.rejectedCount > 0 && HolsterLearn::RejectionJoinsCluster(fromRun))
				{
					++slot.rejectedCount;
					slot.rejectedSum.x += bestLocal.x;
					slot.rejectedSum.y += bestLocal.y;
					slot.rejectedSum.z += bestLocal.z;
				}
				else
				{
					slot.rejectedCount = 1;
					slot.rejectedFirst = bestLocal;
					slot.rejectedSum = bestLocal;
				}

				if (!HolsterLearn::ClusterReplacesSlot(slot.rejectedCount))
				{
					LOG_INFO(
						"Holster learning: %s was %.1f units off slot %d %s's average - ignored as an "
						"outlier (%d in a row that agree; %d replaces the slot).",
						how,
						fromMean,
						bestSlot + 1,
						kSlots[bestSlot].name,
						slot.rejectedCount,
						HolsterLearn::kRejectionsBeforeReset);
					return;
				}

				slot.sampleCount = slot.rejectedCount;
				slot.sampleSum = slot.rejectedSum;
				slot.learnedOffset.x = slot.sampleSum.x / slot.sampleCount;
				slot.learnedOffset.y = slot.sampleSum.y / slot.sampleCount;
				slot.learnedOffset.z = slot.sampleSum.z / slot.sampleCount;
				slot.learned = true;
				slot.rejectedCount = 0;
				RebuildSlotSeeds();
				SaveLearnedSlots();
				LOG_INFO(
					"Holster learning: slot %d %s turned away %d interactions in a row that all landed "
					"within %.0f units of each other - its stored centre was wrong, so it starts again "
					"at [%.2f %.2f %.2f].",
					bestSlot + 1,
					kSlots[bestSlot].name,
					slot.sampleCount,
					HolsterLearn::kAgreementRadius,
					slot.learnedOffset.x,
					slot.learnedOffset.y,
					slot.learnedOffset.z);
				return;
			}

			// A sample the mean accepts ends any run of rejections.
			slot.rejectedCount = 0;

			slot.sampleSum.x += bestLocal.x;
			slot.sampleSum.y += bestLocal.y;
			slot.sampleSum.z += bestLocal.z;
			++slot.sampleCount;
			slot.learnedOffset.x = slot.sampleSum.x / slot.sampleCount;
			slot.learnedOffset.y = slot.sampleSum.y / slot.sampleCount;
			slot.learnedOffset.z = slot.sampleSum.z / slot.sampleCount;
			slot.learned = true;
			RebuildSlotSeeds();

			const float shift = Distance(slot.learnedOffset, slot.offset);
			SaveLearnedSlots();
			LOG_INFO(
				"Holster learning: %s refined slot %d %s to [%.2f %.2f %.2f] over %d samples "
				"- %.1f units from the ini estimate.",
				how,
				bestSlot + 1,
				kSlots[bestSlot].name,
				slot.learnedOffset.x,
				slot.learnedOffset.y,
				slot.learnedOffset.z,
				slot.sampleCount,
				shift);
		}

		// A press-holster candidate: position captured at the grip press, and the hand
		// that pressed, learned only if VRIK then actually holsters.
		static NiPoint3 s_pressCandidate;
		static bool s_pressCandidateIsLeft = false;
		static bool s_havePressCandidate = false;
	}

	void HolsterRefreshSettings()
	{
		s_haveSettings = false;
		if (!vrikInterface)
		{
			LOG_INFO("Holsters: VRIK is not present, so holster-aware release stays off.");
			return;
		}

		s_holstersEnabled = VrikSetting("enableHolsters", 0.0) != 0.0;
		s_activationDistance = static_cast<float>(VrikSetting("slotActivationDistance", 8.0));
		if (s_activationDistance <= 0.0f)
		{
			s_activationDistance = 8.0f;
		}

		char key[32];
		for (int i = 0; i < 14; ++i)
		{
			const int n = i + 1;
			sprintf_s(key, sizeof(key), "posX%d", n);
			s_slots[i].offset.x = static_cast<float>(VrikSetting(key, 0.0));
			sprintf_s(key, sizeof(key), "posY%d", n);
			s_slots[i].offset.y = static_cast<float>(VrikSetting(key, 0.0));
			sprintf_s(key, sizeof(key), "posZ%d", n);
			s_slots[i].offset.z = static_cast<float>(VrikSetting(key, 0.0));
			sprintf_s(key, sizeof(key), "handSlot%d", n);
			s_slots[i].handRule = static_cast<int>(VrikSetting(key, 0.0));
			sprintf_s(key, sizeof(key), "visibleSlot%d", n);
			s_slots[i].visible = VrikSetting(key, 1.0) != 0.0;
		}

		s_haveSettings = true;
		LoadLearnedSlots();
		LOG_INFO(
			"Holsters: read VRIK's slot layout (enableHolsters=%s, activationDistance=%.1f, "
			"left hip offset [%.1f %.1f %.1f]).",
			s_holstersEnabled ? "yes" : "no",
			s_activationDistance,
			s_slots[0].offset.x,
			s_slots[0].offset.y,
			s_slots[0].offset.z);
	}

	namespace
	{
		// VRIK reads its own ini during its startup, so asking too early gets nothing.
		// Try again on the first few releases rather than once at load.
		void EnsureSettings()
		{
			if (s_haveSettings || !vrikInterface || s_settingsAttempts >= 5)
			{
				return;
			}
			++s_settingsAttempts;
			HolsterRefreshSettings();
		}
	}

	// Learns where the palm is, from a hand that is holding nothing.
	//
	// HIGGS parks its hand body while a weapon is drawn, and a parked body reports
	// wherever it was last active - which is what made an earlier attempt at this
	// read "near the last holster you used" for every release, including ones made
	// above the player's head. An empty hand is the one moment the body is honest,
	// so that is the only moment this samples.
	void HolsterCalibratePalms()
	{
		// VRIK 0.8.7 and newer answer holster questions themselves: nothing to calibrate.
		if (VrikHolsterApiAvailable())
		{
			return;
		}

		PlayerCharacter* player = *g_thePlayer;
		if (!player || !higgsInterface)
		{
			return;
		}

		for (int hand = 0; hand < 2; ++hand)
		{
			const bool isLeftVRController = (hand == 1);
			PalmCalibration& palm = s_palm[hand];
			if (palm.samples >= 90)
			{
				continue;
			}

			// A weapon in this hand means the body is parked. Nothing to learn from.
			if (player->GetEquippedObject(VRControllerToGameHand(isLeftVRController)))
			{
				continue;
			}

			NiMatrix33 rot;
			NiPoint3 posMeters;
			if (!RawHavok::ReadHkTransform(
					higgsInterface->GetHandRigidBody(isLeftVRController), rot, posMeters))
			{
				continue;
			}

			NiAVObject* bone = FindHandNode(player, isLeftVRController);
			if (!bone)
			{
				continue;
			}

			const NiPoint3 world(
				posMeters.x * RawHavok::kHavokToSkyrim,
				posMeters.y * RawHavok::kHavokToSkyrim,
				posMeters.z * RawHavok::kHavokToSkyrim);

			NiPoint3 local;
			if (!WorldToBoneLocal(bone, world, local))
			{
				continue;
			}

			// A palm is a few units from a wrist. Anything further is a parked body or
			// a skeleton we do not understand, and averaging it in would poison the
			// number this whole feature now rests on.
			const NiPoint3 origin(0.0f, 0.0f, 0.0f);
			if (Distance(local, origin) > 25.0f)
			{
				continue;
			}

			palm.sum.x += local.x;
			palm.sum.y += local.y;
			palm.sum.z += local.z;
			++palm.samples;
			palm.local.x = palm.sum.x / palm.samples;
			palm.local.y = palm.sum.y / palm.samples;
			palm.local.z = palm.sum.z / palm.samples;

			if (!palm.have && palm.samples >= 10)
			{
				palm.have = true;
				LOG_INFO(
					"Holsters: %s palm calibrated to [%.2f %.2f %.2f] from the hand bone - "
					"holster distances now measure the same point VRIK does.",
					isLeftVRController ? "left" : "right",
					palm.local.x, palm.local.y, palm.local.z);
			}
		}
	}

	namespace
	{
		// The estimate-based scan, shared by the stand-aside test and the buzz. trustedOnly
		// keeps to slots measured off enough real draws and holsters to be worth a tight zone -
		// see HolsterRange::ForBuzz for why the buzz wants that and the stand-aside does not.
		bool NearHandFromOwnEstimate(
			const bool isLeftVRController,
			int& outSlot,
			float& outDistance,
			const HolsterRange::Reach& reach,
			const bool trustedOnly)
		{
			EnsureSettings();

			if (!holsterAware || !s_haveSettings || !s_holstersEnabled)
			{
				return false;
			}

			PlayerCharacter* player = *g_thePlayer;
			NiPoint3 handWorld;
			s_lastPositionSource = GetHandPosition(player, isLeftVRController, handWorld);
			if (!s_lastPositionSource)
			{
				return false;
			}

			float best = CatchRangeForHand(player, handWorld, reach);

			for (int i = 0; i < 14; ++i)
			{
				if (!SlotAcceptsHand(i, isLeftVRController))
				{
					continue;
				}

				if (trustedOnly &&
					!HolsterBody::CentreTrusted(
						holsterThirdPersonBody, HolsterLearn::CentreIsTrusted(s_slots[i].sampleCount)))
				{
					continue;
				}

				NiPoint3 local;
				if (!WorldToBoneLocal(FindAnchorBone(player, kSlots[i].anchorBone), handWorld, local))
				{
					continue;
				}

				const float d = Distance(local, SlotTarget(i));
				if (d < best)
				{
					best = d;
					outSlot = i + 1;
					outDistance = d;
				}
			}

			return outSlot != 0;
		}
	}

	bool HolsterNearHand(
		const bool isLeftVRController,
		int& outSlot,
		float& outDistance,
		const HolsterRange::Reach& reach)
	{
		outSlot = 0;
		outDistance = 0.0f;

		// VRIK 0.8.7 and newer say which slot is in reach themselves. That answer IS the range, so
		// reach is not applied, and there is no distance to report.
		if (VrikHolsterApiAvailable())
		{
			if (!holsterAware)
			{
				return false;
			}
			s_lastPositionSource = "VRIK";
			outSlot = VrikHolsterSlotInReach(VRControllerToGameHand(isLeftVRController));
			return outSlot != 0;
		}

		return NearHandFromOwnEstimate(isLeftVRController, outSlot, outDistance, reach, false);
	}

	bool HolsterBuzzNearHand(const bool isLeftVRController, int& outSlot, float& outDistance)
	{
		outSlot = 0;
		outDistance = 0.0f;

		// VRIK 0.8.7 and newer answer the buzz exactly, in GripHold. Nothing here competes with
		// that: this is the older-VRIK path only.
		if (VrikHolsterApiAvailable())
		{
			return false;
		}

		return NearHandFromOwnEstimate(
			isLeftVRController, outSlot, outDistance, HolsterRange::ForBuzz(), true);
	}

	bool HolsterSlotCentreIsTrusted(const int slot)
	{
		return slot >= 1 && slot <= 14 &&
			HolsterBody::CentreTrusted(
				holsterThirdPersonBody, HolsterLearn::CentreIsTrusted(s_slots[slot - 1].sampleCount));
	}

	namespace
	{
		// Collects candidate sphere nodes anywhere under a subtree. VRIK's own naming
		// is not documented, so this matches loosely and the dump option below exists
		// for the case where it matches nothing.
		struct ScanStats
		{
			int visited = 0;
			int named = 0;
			int rejectedFar = 0;
			int rejectedJunk = 0;
			int budget = 20000;
		};

		// The scene is full of template geometry that matches any loose name test -
		// QuestMarker, EditorMarker, CritterMarker01, MapMarkers3D, and a pile of
		// unparented Sphere01 nodes all sitting at the world origin. Letting those in
		// filled the candidate list forty deep before the walk had gone 500 nodes, so
		// it never reached VRIK's spheres at all, and two of them got pinned onto real
		// slots. A holster sphere is ON THE PLAYER, so distance settles it.
		bool LooksLikeSceneJunk(const std::string& lowered)
		{
			static const char* kJunk[] = {
				"questmarker", "editormarker", "crittermarker", "mapmarker",
				"markerback", "markerdoor", "quest markers", "markerx",
			};

			for (const char* junk : kJunk)
			{
				if (lowered.find(junk) != std::string::npos)
				{
					return true;
				}
			}
			return false;
		}

		// ---- the sphere census ---------------------------------------------------------------
		//
		// Everything beside the hand, NAMED OR NOT. The 2.1.x scan matched on names containing
		// "sphere", "holster", "hover", "slot", "marker" or "vrik", which can only ever find what
		// it already expects - and what it actually found were two decoys that corrupted eleven
		// slots. A census makes no guess: it reports what is there and a human decides.
		constexpr float kCensusRadius = 60.0f;
		constexpr int kCensusReport = 30;

		struct CensusHit
		{
			std::string name;
			NiPoint3 world;
			float fromHand = 0.0f;
			float fromPlayer = 0.0f;
			bool geometry = false;
		};

		struct CensusStats
		{
			int visited = 0;
			int budget = 60000;
		};

		// Whether this node hangs off the player's own body. The body is not what we are looking
		// for and it is most of what is nearby.
		bool UnderPlayer(NiAVObject* node, NiNode* playerRoot)
		{
			int climbed = 0;
			for (NiAVObject* walk = node; walk && climbed < 64; walk = walk->m_parent, ++climbed)
			{
				if (walk == playerRoot)
				{
					return true;
				}
			}
			return false;
		}

		void CollectCensus(
			NiAVObject* node,
			const int depth,
			NiNode* playerRoot,
			const NiPoint3& handWorld,
			std::vector<CensusHit>& out,
			CensusStats& stats)
		{
			if (!node || depth > 24 || stats.budget <= 0 || out.size() >= 400)
			{
				return;
			}

			--stats.budget;
			++stats.visited;

			const NiPoint3& world = node->m_worldTransform.pos;
			const float fromHand = Distance(world, handWorld);

			// An unparented template sits at the origin and is not in the world at all.
			const bool atOrigin = std::fabs(world.x) < 0.01f &&
				std::fabs(world.y) < 0.01f &&
				std::fabs(world.z) < 0.01f;

			if (fromHand <= kCensusRadius && !atOrigin && !UnderPlayer(node, playerRoot))
			{
				CensusHit hit;
				const char* const name = node->m_name;
				hit.name = (name && name[0]) ? name : "(unnamed)";
				hit.world = world;
				hit.fromHand = fromHand;
				hit.fromPlayer = Distance(world, playerRoot->m_worldTransform.pos);
				hit.geometry = node->GetAsNiNode() == nullptr;
				out.push_back(hit);
			}

			NiNode* asNode = node->GetAsNiNode();
			if (!asNode)
			{
				return;
			}

			for (UInt32 i = 0; i < asNode->m_children.m_arrayBufLen; ++i)
			{
				CollectCensus(asNode->m_children.m_data[i], depth + 1, playerRoot, handWorld, out, stats);
			}
		}

		void CollectSpheres(
			NiAVObject* node,
			const int depth,
			const NiPoint3& playerPos,
			std::vector<std::pair<std::string, NiPoint3>>& out,
			ScanStats& stats)
		{
			if (!node || depth > 12 || out.size() >= 200 || stats.budget <= 0)
			{
				return;
			}

			--stats.budget;
			++stats.visited;

			const char* name = node->m_name;
			if (name && name[0])
			{
				++stats.named;

				std::string lowered(name);
				for (char& c : lowered)
				{
					c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
				}

				if (lowered.find("sphere") != std::string::npos ||
					lowered.find("holster") != std::string::npos ||
					lowered.find("hover") != std::string::npos ||
					lowered.find("slot") != std::string::npos ||
					lowered.find("marker") != std::string::npos ||
					lowered.find("vrik") != std::string::npos)
				{
					const NiPoint3& world = node->m_worldTransform.pos;
					if (LooksLikeSceneJunk(lowered))
					{
						++stats.rejectedJunk;
					}
					else if (Distance(world, playerPos) > 250.0f ||
						(std::fabs(world.x) < 0.01f &&
							std::fabs(world.y) < 0.01f &&
							std::fabs(world.z) < 0.01f))
					{
						// Unparented templates sit at the origin; world scenery is far.
						++stats.rejectedFar;
					}
					else
					{
						out.push_back(std::make_pair(std::string(name), world));
					}
				}
			}

			NiNode* asNode = node->GetAsNiNode();
			if (!asNode)
			{
				return;
			}

			for (UInt32 i = 0; i < asNode->m_children.m_arrayBufLen; ++i)
			{
				CollectSpheres(asNode->m_children.m_data[i], depth + 1, playerPos, out, stats);
			}
		}

		void DumpEveryNode(NiAVObject* node, const int depth, int& budget)
		{
			if (!node || depth > 3 || budget <= 0)
			{
				return;
			}

			--budget;
			const char* name = node->m_name;
			LOG_INFO(
				"Node dump: %*s%s%s",
				depth * 2,
				"",
				(name && name[0]) ? name : "(unnamed)",
				node->GetAsNiNode() ? "" : " [geometry]");

			NiNode* asNode = node->GetAsNiNode();
			if (!asNode)
			{
				return;
			}

			for (UInt32 i = 0; i < asNode->m_children.m_arrayBufLen; ++i)
			{
				DumpEveryNode(asNode->m_children.m_data[i], depth + 1, budget);
			}
		}

		static float s_sphereScanTimer = 0.0f;
		static int s_spheresSeenLast = 0;
	}

	bool HolsterNearestRawSlotOnBothBodies(
		const bool isLeftVRController,
		int& outFirstPersonSlot,
		float& outFirstPersonDistance,
		int& outThirdPersonSlot,
		float& outThirdPersonDistance)
	{
		outFirstPersonSlot = 0;
		outThirdPersonSlot = 0;
		outFirstPersonDistance = 0.0f;
		outThirdPersonDistance = 0.0f;

		EnsureSettings();
		PlayerCharacter* const player = *g_thePlayer;
		if (!player || !s_haveSettings)
		{
			return false;
		}

		NiNode* const firstPerson = player->GetNiRootNode(1);
		NiNode* const body = player->GetNiRootNode(0);
		NiPoint3 handWorld;
		if (!firstPerson || !body || !GetHandPosition(player, isLeftVRController, handWorld))
		{
			return false;
		}

		int firstSlot = -1;
		int bodySlot = -1;
		float firstDistance = 0.0f;
		float bodyDistance = 0.0f;
		if (!NearestRawSlotOnBothBodies(
				firstPerson, body, isLeftVRController, handWorld, firstSlot, firstDistance, bodySlot, bodyDistance))
		{
			return false;
		}

		outFirstPersonSlot = firstSlot + 1;
		outFirstPersonDistance = firstDistance;
		outThirdPersonSlot = bodySlot + 1;
		outThirdPersonDistance = bodyDistance;
		return true;
	}

	void HolsterSkeletonProbeTick(const float deltaTime)
	{
		// THE SKELETON WATCH. GetNiNode() is what every holster calculation has always looked its
		// bones up under, and it is not fixed: M2.2.5-test1 caught it on GetNiRootNode(1) at 254.8s and
		// on a replaced GetNiRootNode(0) from 401.7s on. A holster sample learned on one skeleton and
		// used on the other is 10 to 39 units out. Every change is logged, with what it changed to,
		// so a session shows exactly when the holster maths moved body.
		{
			static void* s_watchNode = nullptr;
			static void* s_watchRoot0 = nullptr;
			static void* s_watchRoot1 = nullptr;
			static int s_watchLinesLeft = 40;

			PlayerCharacter* const watched = *g_thePlayer;
			if (watched && s_watchLinesLeft > 0)
			{
				NiNode* const node = watched->GetNiNode();
				NiNode* const root0 = watched->GetNiRootNode(0);
				NiNode* const root1 = watched->GetNiRootNode(1);
				if (node != s_watchNode || root0 != s_watchRoot0 || root1 != s_watchRoot1)
				{
					--s_watchLinesLeft;
					LOG_INFO(
						"Skeleton watch: GetNiNode()=%p is %s. GetNiRootNode(0)=%p%s, GetNiRootNode(1)=%p%s. "
						"%d change line(s) left this session.",
						node,
						HolsterBody::RootName(HolsterBody::WhichRoot(node, root0, root1)),
						root0,
						(s_watchRoot0 && root0 != s_watchRoot0) ? " (REPLACED)" : "",
						root1,
						(s_watchRoot1 && root1 != s_watchRoot1) ? " (REPLACED)" : "",
						s_watchLinesLeft);
					s_watchNode = node;
					s_watchRoot0 = root0;
					s_watchRoot1 = root1;
				}
			}
		}

		static float s_next = 8.0f;
		static int s_fired = 0;

		if (s_fired >= 3)
		{
			return;
		}

		s_next -= deltaTime;
		if (s_next > 0.0f)
		{
			return;
		}

		PlayerCharacter* player = *g_thePlayer;
		if (!player || !player->GetNiNode())
		{
			s_next = 2.0f; // body is not up yet - ask again shortly
			return;
		}

		++s_fired;
		s_next = 20.0f;
		HolsterSkeletonProbe(s_fired == 1 ? "first look after load" : "a later look, in case the body was still settling");
	}

	void HolsterSkeletonProbe(const char* const why)
	{
		// Bounded hard: this answers one question and then stops.
		static int s_probesLeft = 3;
		if (s_probesLeft <= 0)
		{
			return;
		}

		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			return;
		}

		--s_probesLeft;

		NiNode* const roots[3] = {
			player->GetNiNode(),
			player->GetNiRootNode(0),
			player->GetNiRootNode(1),
		};
		static const char* const kRootName[3] = {
			"GetNiNode()", "GetNiRootNode(0)", "GetNiRootNode(1)"
		};

		LOG_INFO(
			"Holster skeleton probe (%s): %s=%p  %s=%p  %s=%p. FindBone uses the first of these. "
			"%d probe(s) left this session.",
			why,
			kRootName[0], roots[0],
			kRootName[1], roots[1],
			kRootName[2], roots[2],
			s_probesLeft);

		if (roots[0] == roots[1] && roots[1] == roots[2])
		{
			LOG_INFO(
				"Holster skeleton probe: all three roots are the SAME node, so there is only one "
				"skeleton and the holster maths is on it. Nothing to fix here.");
		}

		// Every anchor bone once, plus both hands - GetHandPosition looks those up the same way.
		const char* names[16];
		int count = 0;
		for (int i = 0; i < 14 && count < 14; ++i)
		{
			bool seen = false;
			for (int j = 0; j < count; ++j)
			{
				if (std::strcmp(names[j], kSlots[i].anchorBone) == 0)
				{
					seen = true;
					break;
				}
			}
			if (!seen)
			{
				names[count++] = kSlots[i].anchorBone;
			}
		}
		names[count++] = "NPC L Hand [LHnd]";
		names[count++] = "NPC R Hand [RHnd]";

		for (int n = 0; n < count; ++n)
		{
			BSFixedString boneName(names[n]);
			NiPoint3 found[3];
			bool have[3] = { false, false, false };

			for (int r = 0; r < 3; ++r)
			{
				if (!roots[r])
				{
					continue;
				}
				NiAVObject* const node = roots[r]->GetObjectByName(&boneName.data);
				if (node)
				{
					have[r] = true;
					found[r] = node->m_worldTransform.pos;
				}
			}

			// The number that matters: how far apart the same bone is under different roots.
			float worst = 0.0f;
			for (int a = 0; a < 3; ++a)
			{
				for (int b = a + 1; b < 3; ++b)
				{
					if (have[a] && have[b])
					{
						const float d = Distance(found[a], found[b]);
						if (d > worst)
						{
							worst = d;
						}
					}
				}
			}

			LOG_INFO(
				"Holster skeleton probe: %-24s NiNode=%s  Root0=%s  Root1=%s  worst separation %.2f%s",
				names[n],
				have[0] ? "yes" : "NO ",
				have[1] ? "yes" : "NO ",
				have[2] ? "yes" : "NO ",
				worst,
				worst > 1.0f ? "   <== THE SAME BONE IN TWO PLACES" : "");

			if (worst > 1.0f)
			{
				for (int r = 0; r < 3; ++r)
				{
					if (have[r])
					{
						LOG_INFO(
							"Holster skeleton probe:     %-18s [%.1f %.1f %.1f]",
							kRootName[r],
							found[r].x,
							found[r].y,
							found[r].z);
					}
				}
			}
		}
	}

	void HolsterCensusAtHand(const bool isLeftVRController, const char* const why)
	{
		// ANSWERED, 2026-09-15: six censuses over a full walk of the scene graph (12635 nodes)
		// found no node anywhere whose name contains "sphere", "hover" or "holster". VRIK's hover
		// spheres are not reachable this way. Kept behind bHolsterScanSpheres (default off) in
		// case a future VRIK renders them differently; there is no reason to run it otherwise.
		if (!holsterScanSpheres || VrikHolsterApiAvailable())
		{
			return;
		}

		// Bounded hard. This walks a large part of the scene graph, and it exists to answer one
		// question, not to run forever.
		static int s_censusesLeft = 6;
		if (s_censusesLeft <= 0)
		{
			return;
		}

		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			return;
		}

		NiNode* playerRoot = player->GetNiNode();
		NiPoint3 handWorld;
		if (!playerRoot || !GetHandPosition(player, isLeftVRController, handWorld))
		{
			return;
		}

		--s_censusesLeft;

		// The TRUE root. The 2.1.x scan climbed three parents and reported zero; if VRIK's
		// spheres are references rather than parts of the body, three parents may never have
		// reached them.
		NiAVObject* root = playerRoot;
		int climbed = 0;
		while (root->m_parent)
		{
			root = root->m_parent;
			++climbed;
		}

		std::vector<CensusHit> hits;
		CensusStats stats;
		CollectCensus(root, 0, playerRoot, handWorld, hits, stats);

		std::sort(hits.begin(), hits.end(), [](const CensusHit& a, const CensusHit& b) {
			return a.fromHand < b.fromHand;
		});

		LOG_INFO(
			"Holster census (%s, %s hand): climbed %d parents to the scene root, visited %d nodes, "
			"%d within %.0f units of the hand and outside the player's own body. %d census(es) left "
			"this session.",
			why,
			isLeftVRController ? "left" : "right",
			climbed,
			stats.visited,
			static_cast<int>(hits.size()),
			kCensusRadius,
			s_censusesLeft);

		if (hits.empty())
		{
			LOG_INFO(
				"Holster census: nothing at all beside the hand. VRIK's hover spheres are not nodes "
				"in this scene graph, so they cannot be read this way - see vrikslots.ini "
				"showHolsterHoverSphere to confirm they are switched on.");
			return;
		}

		const int report = static_cast<int>(hits.size()) < kCensusReport
			? static_cast<int>(hits.size())
			: kCensusReport;
		for (int i = 0; i < report; ++i)
		{
			const CensusHit& hit = hits[i];
			LOG_INFO(
				"Holster census: %5.1f from hand, %5.1f from body  %-34s %s  world=[%.1f %.1f %.1f]",
				hit.fromHand,
				hit.fromPlayer,
				hit.name.c_str(),
				hit.geometry ? "[geometry]" : "[node]    ",
				hit.world.x,
				hit.world.y,
				hit.world.z);
		}
	}

	void HolsterScanSpheresTick(const float deltaTime)
	{
		if (!holsterScanSpheres || VrikHolsterApiAvailable())
		{
			return;
		}

		s_sphereScanTimer -= deltaTime;
		if (s_sphereScanTimer > 0.0f)
		{
			return;
		}
		s_sphereScanTimer = 2.0f;

		EnsureSettings();
		PlayerCharacter* player = *g_thePlayer;
		if (!player || !s_haveSettings)
		{
			return;
		}

		NiNode* playerRoot = player->GetNiNode();
		if (!playerRoot)
		{
			return;
		}

		// Do NOT assume the spheres hang off the player. VRIK builds its own body and
		// may attach debug geometry beside the player rather than under it, and the
		// first scan looked only at the player's own subtree - which is why it could
		// report zero while the spheres were plainly on screen. Climb a few parents
		// first so siblings are in reach, with a node budget so this stays cheap.
		NiAVObject* scanRoot = playerRoot;
		int climbed = 0;
		while (scanRoot->m_parent && climbed < 3)
		{
			scanRoot = scanRoot->m_parent;
			++climbed;
		}

		const NiPoint3 playerPos = playerRoot->m_worldTransform.pos;

		std::vector<std::pair<std::string, NiPoint3>> spheres;
		ScanStats stats;
		CollectSpheres(scanRoot, 0, playerPos, spheres, stats);

		const int seen = static_cast<int>(spheres.size());

		// Report every scan, not just the interesting ones. A silent scan taught us
		// nothing last time: zero lines could equally mean it never ran, found nothing,
		// or never reached the right part of the tree.
		static float reportTimer = 0.0f;
		reportTimer -= 2.0f;
		if (seen != s_spheresSeenLast || reportTimer <= 0.0f)
		{
			reportTimer = 10.0f;
			LOG_INFO(
				"Holster spheres: scanned %d nodes (%d named, climbed %d parents) - %d on the "
				"player, %d scene junk and %d too far ignored.",
				stats.visited,
				stats.named,
				climbed,
				seen,
				stats.rejectedJunk,
				stats.rejectedFar);
		}

		if (spheres.empty())
		{
			s_spheresSeenLast = 0;

			// One dump of what IS there, so the sphere nodes can be named exactly.
			static bool dumped = false;
			if (holsterDumpNodes && !dumped)
			{
				dumped = true;
				int budget = 400;
				LOG_INFO("Node dump: no candidates matched. Listing what is actually there:");
				DumpEveryNode(scanRoot, 0, budget);
			}
			return;
		}

		if (seen == s_spheresSeenLast)
		{
			return; // already pinned from this set
		}
		s_spheresSeenLast = seen;

		// LOG ONLY. This scan never writes a slot any more.
		//
		// It was built to read VRIK's holster spheres, and it never found them. What
		// it did find were two decoy nodes - "BlackSphere" and "Sphere01" - that
		// follow the player around 70 to 115 units out, and because the nearest-slot
		// match always lands on something, they pinned a DIFFERENT slot every two
		// seconds: eleven of the fourteen slots were corrupted this way in one
		// session. The draws and press-holsters are the trustworthy source.
		for (size_t sphere = 0; sphere < spheres.size(); ++sphere)
		{
			const NiPoint3& world = spheres[sphere].second;
			LOG_INFO(
				"Holster spheres: \"%s\" world=[%.1f %.1f %.1f] (%.0f from player) - noted only.",
				spheres[sphere].first.c_str(),
				world.x,
				world.y,
				world.z,
				Distance(world, playerPos));
		}
	}


	void HolsterLearnFromDraw(const bool isLeftVRController)
	{
		// RETIRED in M2.2.8-test1. A draw is inferred when the weapon binds after its equip, and by
		// then the hand has left the slot: M2.2.5-test2's log put six of eight inferred draws 20 to 29
		// units from every slot on both skeletons, where VRIK's reach is 10, and one of them handed the
		// Stomach slot a second bad sample. Press-holsters learn at the grip press, with the hand still
		// on the slot, and stay. This is kept as a no-op so the call sites read as before.
		(void)isLeftVRController;
		static bool s_said = false;
		if (!s_said && !VrikHolsterApiAvailable())
		{
			s_said = true;
			LOG_INFO(
				"Holster learning: draws are no longer learned from - the hand has left the slot by the time a "
				"draw can be recognised. Press-holsters still teach slot positions.");
		}
	}

	bool HolsterHandWorldPosition(const bool isLeftVRController, NiPoint3& outWorld)
	{
		PlayerCharacter* const player = *g_thePlayer;
		return player && GetHandPosition(player, isLeftVRController, outWorld) != nullptr;
	}

	float HolsterActivationDistance()
	{
		EnsureSettings();
		return s_activationDistance;
	}

	void HolsterCapturePressCandidate(const bool isLeftVRController)
	{
		// Nothing to learn from VRIK's own press-holster when VRIK can be asked directly.
		if (VrikHolsterApiAvailable())
		{
			s_havePressCandidate = false;
			return;
		}

		EnsureSettings();
		PlayerCharacter* player = *g_thePlayer;
		s_havePressCandidate =
			player && GetHandPosition(player, isLeftVRController, s_pressCandidate) != nullptr;
		s_pressCandidateIsLeft = isLeftVRController;
	}

	void HolsterLearnFromCapture()
	{
		if (!s_havePressCandidate)
		{
			return;
		}

		s_havePressCandidate = false;
		LearnSlotFromWorldPosition(s_pressCandidateIsLeft, s_pressCandidate, "a VRIK press-holster");
	}

	const char* HolsterLastPositionSource()
	{
		return s_lastPositionSource ? s_lastPositionSource : "none";
	}

	const char* HolsterSlotName(const int slot)
	{
		if (slot < 1 || slot > 14)
		{
			return "unknown";
		}
		return kSlots[slot - 1].name;
	}

	void HolsterTraceRelease(const bool isLeftVRController)
	{
		// With VRIK 0.8.7 and newer, one line with VRIK's own answer replaces the slot-by-slot trace.
		if (VrikHolsterApiAvailable())
		{
			if (!holsterTrace)
			{
				return;
			}

			const bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);
			const int slot = VrikHolsterSlotInReach(isLeftGameHand);
			if (slot == 0)
			{
				LOG_INFO(
					"Holster trace: VRIK says no holster is in reach of the %s hand.",
					isLeftVRController ? "left" : "right");
				return;
			}

			PlayerCharacter* player = *g_thePlayer;
			TESForm* held = player ? player->GetEquippedObject(isLeftGameHand) : nullptr;
			LOG_INFO(
				"Holster trace: VRIK says slot %d %s is in reach of the %s hand; takes %s: %s",
				slot,
				HolsterSlotName(slot),
				isLeftVRController ? "left" : "right",
				GetSafeFormName(held),
				VrikCanHolster(isLeftGameHand) ? "yes" : "no");
			return;
		}

		EnsureSettings();
		if (!holsterTrace || !s_haveSettings)
		{
			return;
		}

		PlayerCharacter* player = *g_thePlayer;
		NiPoint3 handWorld;
		const char* source = GetHandPosition(player, isLeftVRController, handWorld);
		if (!source)
		{
			LOG_INFO(
				"Holster trace: no hand position for the %s controller.",
				isLeftVRController ? "left" : "right");
			return;
		}
		// The radius a hand arriving at a slot is caught by in this mode, so the log shows
		// the one that actually decided - Simple mode's is VRIK's own reach.
		const float range = CatchRangeForHand(
			player,
			handWorld,
			HolsterRange::ForStandDown(simpleMode, false, holsterReleaseMargin));
		LOG_INFO(
			"Holster trace: measuring from %s, hand is %s the body, catch range %.1f%s",
			source,
			IsHandBehindPlayer(player, handWorld) ? "BEHIND" : "in front of",
			range,
			simpleMode ? " (Simple mode: VRIK's own reach)" : "");

		// One pass over the player's scene graph looking for nodes VRIK may have made
		// for its holsters. If any exist their world position IS the slot, and none of
		// the anchor guessing below is needed any more.
		static bool scannedNodes = false;
		if (!scannedNodes)
		{
			scannedNodes = true;
			int logged = 0;
			ScanForVrikNodes(player->GetNiNode(), 0, logged);
			LOG_INFO("Holster nodes: scan finished, %d candidate node(s) found.", logged);
		}

		// Each slot is measured in TWO candidate frames: its mapped anchor bone, and
		// the body root. The offsets say limb slots hang off their own bones while the
		// body slots share a central one, so whichever column collapses toward zero
		// while the hand is genuinely on a holster names the correct frame.
		NiAVObject* rootBone = FindAnchorBone(player, "NPC Root [Root]");

		for (int i = 0; i < 14; ++i)
		{
			NiAVObject* anchor = FindAnchorBone(player, kSlots[i].anchorBone);
			if (!anchor)
			{
				LOG_INFO(
					"Holster trace: slot %2d %-14s anchor bone \"%s\" NOT FOUND",
					i + 1,
					kSlots[i].name,
					kSlots[i].anchorBone);
				continue;
			}

			NiPoint3 local;
			WorldToBoneLocal(anchor, handWorld, local);
			const float boneDist = Distance(local, SlotTarget(i));

			NiPoint3 rootLocal;
			float rootDist = -1.0f;
			if (WorldToBoneLocal(rootBone, handWorld, rootLocal))
			{
				rootDist = Distance(rootLocal, s_slots[i].offset);
			}

			// The shoulders are the last slots that never catch, and they are the only
			// ones whose anchor is a guess we have never been able to test: both are
			// mapped to Spine2, the same bone as Chest, while VRIK's own binary carries
			// clavicle bones we do not use anywhere. Every other slot now lands within
			// a few units of its anchor once the palm is measured properly, so if the
			// shoulders are still tens of units out, the frame is wrong rather than the
			// offset. Measuring both frames side by side settles it in one release: the
			// column that collapses names the right bone.
			float clavDist = -1.0f;
			if (i == 10 || i == 11)
			{
				NiAVObject* clav = FindAnchorBone(
					player, (i == 10) ? "NPC L Clavicle [LClv]" : "NPC R Clavicle [RClv]");
				NiPoint3 clavLocal;
				if (clav && WorldToBoneLocal(clav, handWorld, clavLocal))
				{
					clavDist = Distance(clavLocal, SlotTarget(i));
				}
				else
				{
					LOG_INFO(
						"Holster trace: slot %2d %s - clavicle bone not found on this skeleton.",
						i + 1,
						kSlots[i].name);
				}
			}

			LOG_INFO(
				"Holster trace: slot %2d %-14s inBone=[%7.2f %7.2f %7.2f] "
				"slot=[%7.2f %7.2f %7.2f] boneDist=%7.2f rootDist=%7.2f clavDist=%7.2f%s",
				i + 1,
				kSlots[i].name,
				local.x,
				local.y,
				local.z,
				s_slots[i].offset.x,
				s_slots[i].offset.y,
				s_slots[i].offset.z,
				boneDist,
				rootDist,
				clavDist,
				boneDist <= range ? "   <== IN RANGE" : "");
		}
	}
}
