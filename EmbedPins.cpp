#include "EmbedPins.h"

#include "EmbedBuryRules.h"

#include "Engine.h"
#include "Helper.h"
#include "RawHavok.h"
#include "TrackedWeapons.h"
#include "TriggerHoldLogger.h"
#include "WeaponGrabActivator.h"
#include "config.h"

#include <skse64/GameData.h>
#include <skse64/GameForms.h>
#include <skse64/GameRTTI.h>
#include <skse64/GameReferences.h>
#include <skse64/NiNodes.h>
#include <skse64/gamethreads.h>

#include <cmath>
#include <cstring>

// Port of Weapon Pick Up VR 0.8.0's embed-pin system (see reference/wpvr-plugin-0.8.0.cpp
// in the workspace). Release grip with a blade buried in someone and the weapon stays in
// the wound: the dropped reference is pinned by force — its pose re-asserted from the
// victim's bone every frame, velocities zeroed, collision filtered out. The motion type
// is deliberately LEFT ALONE: a keyframed body stops publishing its transform to its
// node ("sword hangs in the sky"), while a dynamic body with zeroed velocities and a
// re-asserted pose is kinematic in practice anyway.

// Verbose path tracing, off unless EmbedTrace is set in the ini.
#define EMBED_TRACE(...) do { if (embedTrace) { LOG_INFO(__VA_ARGS__); } } while (0)

namespace SwapDropAndHoldRedux
{
	extern SKSETaskInterface* g_task;

	IwpAPI::IIwpInterface001* g_iwpInterface = nullptr;

	namespace
	{
		using RawHavok::kHavokToSkyrim;
		using RawHavok::Mat3Lerp;
		using RawHavok::Mat3Mul;
		using RawHavok::Mat3MulVec;
		using RawHavok::Mat3Transpose;

		// Frames to wait for the weapon-draw animation before giving up on a resume.
		static const int kResumeWaitFrames = 120;
		// Frames to wait for a dropped ref's rigid body before giving up on its pin.
		static const int kPinFixupFrames = 30;

		// Actor::DrawWeaponMagicHands(bool): Actor vtable slot 0xA6 on SkyrimVR
		// (CommonLibSSE-NG SE slot, runtime-proven by WPVR 0.7/0.8's forced draws).
		static const UInt32 kVtbl_DrawWeaponMagicHands = 0xA6;

		void ForcePlayerWeaponDraw(PlayerCharacter* player)
		{
			if (!player)
			{
				return;
			}


			typedef void (*DrawWeaponMagicHands_t)(Actor*, bool);
			void** vtbl = *reinterpret_cast<void***>(static_cast<Actor*>(player));
			reinterpret_cast<DrawWeaponMagicHands_t>(vtbl[kVtbl_DrawWeaponMagicHands])(player, true);
		}

		// Exactly kDrawn (3), NOT the classic IsWeaponDrawn() which also accepts
		// kWantToSheathe/kSheathing — the loose test is what let WPVR's resume fire
		// while the weapon was already on its way back into the sheath.
		UInt32 PlayerWeaponState(PlayerCharacter* player)
		{
			if (!player)
			{
				return 0;
			}

			return (player->actorState.flags08 >> 5) & 7;
		}

		TESObjectREFR* ResolveRefByID(const UInt32 refID)
		{
			if (refID == 0)
			{
				return nullptr;
			}

			TESForm* refForm = LookupFormByID(refID);
			return DYNAMIC_CAST(refForm, TESForm, TESObjectREFR);
		}

		Actor* ResolveActorByHandle(const UInt32 victimHandle)
		{
			if (victimHandle == 0)
			{
				return nullptr;
			}

			UInt32 handle = victimHandle;
			NiPointer<TESObjectREFR> refr;
			LookupREFRByHandle(handle, refr);
			if (!refr)
			{
				return nullptr;
			}

			return DYNAMIC_CAST(refr.get(), TESObjectREFR, Actor);
		}

		using RawHavok::ToQuat;

		// Orthonormalised blend between two rotations (per-frame deltas are tiny).
		NiMatrix33 BlendRot(const NiMatrix33& a, const NiMatrix33& b, const float t)
		{
			NiMatrix33 m = Mat3Lerp(a, b, t);

			NiPoint3 x(m.data[0][0], m.data[1][0], m.data[2][0]);
			float len = std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
			if (len < 1e-4f)
			{
				return b;
			}
			x.x /= len; x.y /= len; x.z /= len;

			NiPoint3 y(m.data[0][1], m.data[1][1], m.data[2][1]);
			const float dot = x.x * y.x + x.y * y.y + x.z * y.z;
			y.x -= x.x * dot; y.y -= x.y * dot; y.z -= x.z * dot;
			len = std::sqrt(y.x * y.x + y.y * y.y + y.z * y.z);
			if (len < 1e-4f)
			{
				return b;
			}
			y.x /= len; y.y /= len; y.z /= len;

			const NiPoint3 z(
				x.y * y.z - x.z * y.y,
				x.z * y.x - x.x * y.z,
				x.x * y.y - x.y * y.x);

			m.data[0][0] = x.x; m.data[1][0] = x.y; m.data[2][0] = x.z;
			m.data[0][1] = y.x; m.data[1][1] = y.y; m.data[2][1] = y.z;
			m.data[0][2] = z.x; m.data[1][2] = z.y; m.data[2][2] = z.z;
			return m;
		}

		NiPoint3 InverseTransformPoint(const NiTransform& t, const NiPoint3& worldPoint)
		{
			const float invScale = t.scale > 0.0f ? 1.0f / t.scale : 1.0f;
			const NiPoint3 rel(
				worldPoint.x - t.pos.x,
				worldPoint.y - t.pos.y,
				worldPoint.z - t.pos.z);
			const NiPoint3 local = Mat3MulVec(Mat3Transpose(t.rot), rel);
			return NiPoint3(local.x * invScale, local.y * invScale, local.z * invScale);
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

		// World AABB centre of the body's collision shape (Havok metres) plus the body
		// origin — how the middle of the blade is found. hkpShape::GetAabbImpl is
		// vtable slot 7 (CommonLib absolute index, runtime-proven by WPVR).
		bool ReadShapeAabb(NiObject* bhkBody, NiPoint3& centreMeters, NiPoint3& originMeters)
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

			const float* transform = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(hk) + 0x170);

			__declspec(align(16)) float aabb[8] = {};
			typedef void (*GetAabbImpl_t)(void* shape, const void* localToWorld, float tolerance, void* outAabb);
			void** vtbl = *reinterpret_cast<void***>(shape);
			reinterpret_cast<GetAabbImpl_t>(vtbl[7])(shape, transform, 0.0f, aabb);

			centreMeters = NiPoint3(
				(aabb[0] + aabb[4]) * 0.5f,
				(aabb[1] + aabb[5]) * 0.5f,
				(aabb[2] + aabb[6]) * 0.5f);
			originMeters = NiPoint3(transform[12], transform[13], transform[14]);
			return true;
		}

		// Closest "NPC ..." skeleton bone of a nearby actor to a world point. Iterates
		// the player's parent cell (the IWP path is the primary one; this geometric
		// fallback only needs the actors around the player).
		NiAVObject* NearestActorBone(
			const NiPoint3& worldPos,
			const float maxDist,
			Actor** outActor,
			float* outDist)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !player->parentCell)
			{
				return nullptr;
			}

			NiAVObject* best = nullptr;
			float bestSq = maxDist * maxDist;
			const float reach = maxDist + 200.0f; // generous: the actor origin is at their feet

			const TESObjectCELL::ReferenceData& refData = player->parentCell->refData;
			if (!refData.refArray)
			{
				return nullptr;
			}

			for (UInt32 i = 0; i < refData.maxSize; ++i)
			{
				const TESObjectCELL::ReferenceData::Reference& entry = refData.refArray[i];
				if (!entry.unk08 || !entry.ref)
				{
					continue;
				}

				Actor* actor = DYNAMIC_CAST(entry.ref, TESObjectREFR, Actor);
				if (!actor || actor == player || (actor->flags & TESForm::kFlagIsDeleted) != 0)
				{
					continue;
				}

				const float adx = actor->pos.x - worldPos.x;
				const float ady = actor->pos.y - worldPos.y;
				const float adz = actor->pos.z - worldPos.z;
				if (adx * adx + ady * ady + adz * adz > reach * reach)
				{
					continue;
				}

				NiNode* root = actor->GetNiNode();
				if (!root)
				{
					continue;
				}

				// Iterative subtree walk over the skeleton, real bones only ("NPC ...")
				// so the nearest node is not a piece of the victim's own gear.
				NiAVObject* stack[128];
				int stackSize = 0;
				stack[stackSize++] = root;
				while (stackSize > 0)
				{
					NiAVObject* obj = stack[--stackSize];
					if (!obj)
					{
						continue;
					}

					const char* name = obj->m_name;
					if (name && std::strncmp(name, "NPC ", 4) == 0)
					{
						const float dx = obj->m_worldTransform.pos.x - worldPos.x;
						const float dy = obj->m_worldTransform.pos.y - worldPos.y;
						const float dz = obj->m_worldTransform.pos.z - worldPos.z;
						const float dsq = dx * dx + dy * dy + dz * dz;
						if (dsq < bestSq)
						{
							bestSq = dsq;
							best = obj;
							if (outActor)
							{
								*outActor = actor;
							}
						}
					}

					NiNode* node = obj->GetAsNiNode();
					if (node)
					{
						for (UInt32 c = 0; c < node->m_children.m_size && stackSize < 128; ++c)
						{
							stack[stackSize++] = node->m_children.m_data[c];
						}
					}
				}
			}

			if (outDist)
			{
				*outDist = best ? std::sqrt(bestSq) : -1.0f;
			}
			return best;
		}

		// ---- is the blade's tip inside the victim? (M3.0.0-test8, EmbedBuryRules.h) -----------------
		//
		// The victim's own collision bodies are the ones the game hangs on its "NPC ..." skeleton bones
		// (the bodies melee hits land on). Each is tested in its OWN frame: the tip is taken into the
		// body's frame and compared with its shape's box there - hkpShape::getAabb (vtable slot 7, as
		// ReadShapeAabb above) given the identity instead of the body's transform. Offsets are the ones
		// RawHavok.h and FindRigidBodyInSubtree already use: collision object at NiAVObject+0x40, its
		// body at +0x20, the hkpRigidBody at +0x10, its shape at +0x20, its transform at +0x170.
		struct TipInBody
		{
			int looked = 0;               // body parts the tip was tested against
			bool inside = false;
			const char* insideName = nullptr;
			float nearestUnits = -1.0f;   // how far outside the nearest body part it was
			const char* nearestName = nullptr;
		};

		bool ReadShapeLocalBox(void* hkpRigidBody, float lo[3], float hi[3])
		{
			void* shape = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hkpRigidBody) + 0x20);
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
			for (int i = 0; i < 3; ++i)
			{
				lo[i] = aabb[i];
				hi[i] = aabb[4 + i];
				if (!(lo[i] <= hi[i]))
				{
					return false; // not a number, or an empty box
				}
			}
			return true;
		}

		TipInBody TestTipInVictimBody(Actor* victim, const NiPoint3& tipUnits)
		{
			TipInBody result;
			NiNode* root = victim ? victim->GetNiNode() : nullptr;
			if (!root)
			{
				return result;
			}

			const float tipMetres[3] = { tipUnits.x / kHavokToSkyrim, tipUnits.y / kHavokToSkyrim, tipUnits.z / kHavokToSkyrim };

			NiAVObject* stack[256];
			int stackSize = 0;
			stack[stackSize++] = root;
			while (stackSize > 0)
			{
				NiAVObject* obj = stack[--stackSize];
				if (!obj)
				{
					continue;
				}

				const char* name = obj->m_name;
				NiAVObject* collisionObject = obj->unk040;
				if (name && std::strncmp(name, "NPC ", 4) == 0 && collisionObject)
				{
					NiObject* body = *reinterpret_cast<NiObject**>(reinterpret_cast<uintptr_t>(collisionObject) + 0x20);
					void* hk = (body && DoNiRTTICast(body, NiRTTI_bhkRigidBody)) ? RawHavok::GetHkpRigidBody(body) : nullptr;
					float lo[3], hi[3];
					if (hk && ReadShapeLocalBox(hk, lo, hi))
					{
						// The motion state's rotation is three COLUMNS; the tip in the body's frame is
						// each column dotted with the tip's offset from the body.
						const float* t = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(hk) + 0x170);
						const float d[3] = { tipMetres[0] - t[12], tipMetres[1] - t[13], tipMetres[2] - t[14] };
						const float local[3] = {
							t[0] * d[0] + t[1] * d[1] + t[2] * d[2],
							t[4] * d[0] + t[5] * d[1] + t[6] * d[2],
							t[8] * d[0] + t[9] * d[1] + t[10] * d[2] };
						++result.looked;
						if (EmbedBury::InsideBox(local, lo, hi))
						{
							result.inside = true;
							result.insideName = name;
							return result;
						}
						const float outside = EmbedBury::DistanceOutsideBox(local, lo, hi);
						if (outside >= 0.0f && (result.nearestUnits < 0.0f || outside * kHavokToSkyrim < result.nearestUnits))
						{
							result.nearestUnits = outside * kHavokToSkyrim;
							result.nearestName = name;
						}
					}
				}

				NiNode* node = obj->GetAsNiNode();
				if (node)
				{
					for (UInt32 c = 0; c < node->m_children.m_size && stackSize < 256; ++c)
					{
						stack[stackSize++] = node->m_children.m_data[c];
					}
				}
			}
			return result;
		}

		// ---- the pin table ----------------------------------------------------------

		struct EmbedPin
		{
			bool active = false;
			UInt32 refID = 0;          // dropped world ref (PlaceAtMe temp ref)
			UInt32 weaponFormID = 0;   // the base weapon, so a lost ref can still be found
			UInt32 victimHandle = 0;
			NiPointer<NiAVObject> bone;
			NiPointer<NiObject> body;  // bhkRigidBody
			NiMatrix33 relRot;         // weapon pose in the bone's frame (Skyrim units)
			NiPoint3 relPos;
			NiPoint3 pullAxisLocal;    // out of the wound (grip-wards), bone frame
			bool fromIwp = false;
			NiPoint3 woundLocal;       // wound entry, bone frame
			NiPoint3 axisLocal;        // channel axis INTO the body, bone frame
			BSFixedString boneName;
			float depthFrac = 0.0f;
			float bladeLen = 0.0f;
			float pullOffset = 0.0f;   // how far it has already been drawn out
			// How far this blade must travel to actually clear the wound. Derived from
			// the real buried length (bladeLen * depthFrac) when IWP gave us the wound,
			// because a fixed distance frees a deep stab while a third of the blade is
			// still inside — which reads in-game as the weapon teleporting to the hand.
			float requiredPull = 0.0f;
			int heldBy = 0;            // 0 = nobody, 1 = right VR controller, 2 = left
			NiPoint3 pullStartHandPos;
			float pullStartOffset = 0.0f;
			UInt32 savedFilter = 0;
			bool filterChanged = false;
			NiPoint3 smoothPos;        // damped pose actually written
			NiMatrix33 smoothRot;
			bool smoothInit = false;
			NiPoint3 lastWorldPos;
			float age = 0.0f;
			bool tracedFirstWrite = false;
			bool tracedFirstDone = false;
		};

		static const int kMaxPins = 8;
		static EmbedPin s_pins[kMaxPins];

		// Filter out the lodged weapon's collision. That alone breaks the feedback loop
		// that makes a stuck blade buzz (weapon shoves ragdoll -> body moves -> pin
		// follows bone -> shove again). Motion type deliberately untouched.
		void ApplyPinPhysics(EmbedPin& pin, const bool lodged)
		{
			void* hk = RawHavok::GetHkpRigidBody(pin.body.get());
			if (!hk)
			{
				return;
			}

			UInt32* filter = RawHavok::GetFilterInfo(hk);
			if (filter)
			{
				if (lodged)
				{
					if (!pin.filterChanged && embedNoCollision)
					{
						EMBED_TRACE("Embed trace: 15 filter was %08X, clearing collision", *filter);
						pin.savedFilter = *filter;
						pin.filterChanged = true;
						*filter |= RawHavok::kFilterNoCollision;
						EMBED_TRACE("Embed trace: 16 filter written");
					}
				}
				else if (pin.filterChanged)
				{
					*filter = pin.savedFilter;
					pin.filterChanged = false;
				}
			}

			EMBED_TRACE("Embed trace: 17 activating the island");
			RawHavok::hkpEntity_activate(hk);
			EMBED_TRACE("Embed trace: 18 island activated");
		}

		void ReleasePin(EmbedPin& pin, const char* reason)
		{
			if (!pin.active)
			{
				return;
			}

			ApplyPinPhysics(pin, false);
			LOG_INFO("Embed: pin released (%s)", reason);
			pin = EmbedPin{};
		}

		EmbedPin* FindFreePin()
		{
			for (int i = 0; i < kMaxPins; ++i)
			{
				if (!s_pins[i].active)
				{
					return &s_pins[i];
				}
			}
			return nullptr;
		}

		// ---- pending pins (between the drop task and the ref's body existing) -------

		struct PendingPin
		{
			bool active = false;
			UInt32 dropRefID = 0;
			UInt32 weaponFormID = 0;
			int fixupFrames = 0;
			UInt32 victimHandle = 0;
			NiPointer<NiAVObject> bone;
			NiMatrix33 relRot;
			NiPoint3 relPos;
			NiPoint3 pullAxisLocal;
			bool fromIwp = false;
			NiPoint3 woundLocal;
			NiPoint3 axisLocal;
			BSFixedString boneName;
			float depthFrac = 0.0f;
			float bladeLen = 0.0f;
		};

		static PendingPin s_pending[2]; // [0] = right VR controller, [1] = left

		PendingPin& PendingFor(const bool isLeftVRController)
		{
			return s_pending[isLeftVRController ? 1 : 0];
		}

		class EmbedStickDropTask : public TaskDelegate
		{
		public:
			EmbedStickDropTask(
				const bool isLeftGameHand,
				const bool isLeftVRController,
				const UInt32 weaponFormID)
				: m_isLeftGameHand(isLeftGameHand)
				, m_isLeftVRController(isLeftVRController)
				, m_weaponFormID(weaponFormID)
			{
			}

			virtual void Run() override
			{
				PendingPin& pending = PendingFor(m_isLeftVRController);

				UInt32 droppedRefID = 0;
				if (!DropEquippedWeaponFromHand(
					m_isLeftGameHand, m_isLeftVRController, m_weaponFormID, false, &droppedRefID) ||
					droppedRefID == 0)
				{
					LOG_ERR("Embed: stick drop failed for formId=%08X — pin abandoned", m_weaponFormID);
					pending = PendingPin{};
					return;
				}

				pending.dropRefID = droppedRefID;
				pending.weaponFormID = m_weaponFormID;
				pending.fixupFrames = kPinFixupFrames;
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftGameHand;
			bool m_isLeftVRController;
			UInt32 m_weaponFormID;
		};

		void ServicePendingPin(PendingPin& pending, const bool isLeftVRController)
		{
			if (!pending.active || pending.dropRefID == 0)
			{
				return;
			}

			if (pending.fixupFrames <= 0)
			{
				LOG_ERR("Embed: dropped ref refId=%08X never grew a rigid body — pin abandoned", pending.dropRefID);
				pending = PendingPin{};
				return;
			}
			--pending.fixupFrames;

			TESObjectREFR* refr = ResolveRefByID(pending.dropRefID);
			if (!refr)
			{
				pending = PendingPin{};
				return;
			}

			NiObject* body = RawHavok::FindRigidBodyInSubtree(refr->GetNiNode());
			if (!RawHavok::GetHkpRigidBody(body))
			{
				return; // not built yet; try again next frame
			}

			EMBED_TRACE("Embed trace: 12 dropped ref has a body, building the pin");

			EmbedPin* pin = FindFreePin();
			if (!pin)
			{
				LOG_ERR("Embed: pin table full — weapon drops normally");
				pending = PendingPin{};
				return;
			}

			EMBED_TRACE("Embed trace: 13 copying pin fields");
			pin->active = true;
			pin->refID = pending.dropRefID;
			pin->weaponFormID = pending.weaponFormID;
			pin->victimHandle = pending.victimHandle;
			pin->bone = pending.bone;
			pin->body = body;
			pin->relRot = pending.relRot;
			pin->relPos = pending.relPos;
			pin->pullAxisLocal = pending.pullAxisLocal;
			pin->fromIwp = pending.fromIwp;
			pin->woundLocal = pending.woundLocal;
			pin->axisLocal = pending.axisLocal;
			pin->boneName = pending.boneName;
			pin->depthFrac = pending.depthFrac;
			pin->bladeLen = pending.bladeLen;
			pin->requiredPull = pullOutDistance;
			if (pin->bladeLen > 1.0f && pin->depthFrac > 0.0f)
			{
				// A little past free, so the blade is clear rather than just level
				// with the skin when it lets go.
				const float buried = pin->bladeLen * pin->depthFrac * 1.1f;
				if (buried > 1.0f)
				{
					pin->requiredPull = buried;
				}
			}
			pin->age = 0.0f;

			// Logged BEFORE the physics is touched: if the log shows the pin and then
			// stops, the collision-filter write is what died, not the bookkeeping.
			LOG_INFO(
				"Embed trace: 14 pin fields set, refId=%08X bone='%s' (%s, depth %.0f%%)",
				pin->refID,
				pin->boneName.data ? pin->boneName.data : "<none>",
				pin->fromIwp ? "IWP wound" : "geometric",
				pin->depthFrac * 100.0f);

			ApplyPinPhysics(*pin, true);

			LOG_INFO(
				"Embed: pinned refId=%08X — tracking the wound now (blade %.0fu, %.0f pct buried, drag %.0fu to free it)",
				pin->refID,
				pin->bladeLen,
				pin->depthFrac * 100.0f,
				pin->requiredPull);
			pending = PendingPin{};
		}

		// ---- resume (hand the extraction back to IWP) -------------------------------

		struct ResumeState
		{
			bool pending = false;
			int framesLeft = 0;
			float drawGuard = 0.0f;
			float redrawCooldown = 0.0f;
			UInt32 victimHandle = 0;
			NiPointer<NiAVObject> bone;
			BSFixedString boneName;
			NiPoint3 woundLocal;
			NiPoint3 axisLocal;
			float depthFrac = 0.0f;
			float bladeLen = 0.0f;
		};

		static ResumeState s_resume[2]; // [0] = right VR controller, [1] = left

		ResumeState& ResumeFor(const bool isLeftVRController)
		{
			return s_resume[isLeftVRController ? 1 : 0];
		}

		bool ReadHandPositionSkyrim(const bool isLeftVRController, NiPoint3& outPos)
		{
			NiMatrix33 rot;
			NiPoint3 posMeters;
			if (!RawHavok::ReadHkTransform(GetHiggsBody(isLeftVRController, false), rot, posMeters))
			{
				return false;
			}

			outPos = NiPoint3(
				posMeters.x * kHavokToSkyrim,
				posMeters.y * kHavokToSkyrim,
				posMeters.z * kHavokToSkyrim);
			return true;
		}
	}

	bool AcceptIwpInterfaceOffer(SKSEMessagingInterface::Message* msg)
	{
		if (!msg || msg->type != IwpAPI::kMessage_Interface || !msg->data ||
			msg->dataLen < sizeof(IwpAPI::IwpApiOffer))
		{
			return false;
		}

		IwpAPI::IwpApiOffer* offer = static_cast<IwpAPI::IwpApiOffer*>(msg->data);
		if (!offer->GetApiFunction)
		{
			return false;
		}

		g_iwpInterface = static_cast<IwpAPI::IIwpInterface001*>(offer->GetApiFunction(1));
		if (g_iwpInterface)
		{
			LOG_INFO(
				"Got IWP interface from broadcast (build %u) — lodged weapons use its extraction feel.",
				g_iwpInterface->GetBuildNumber());
		}
		return g_iwpInterface != nullptr;
	}

	void RequestIwpInterface(const PluginHandle pluginHandle, SKSEMessagingInterface* messaging)
	{
		if (g_iwpInterface || !messaging)
		{
			return;
		}

		IwpAPI::IwpApiOffer msg{};
		messaging->Dispatch(
			pluginHandle,
			IwpAPI::kMessage_GetInterface,
			&msg,
			sizeof(msg),
			"ImmersiveWeaponPenetrationVR");
		if (msg.GetApiFunction)
		{
			g_iwpInterface = static_cast<IwpAPI::IIwpInterface001*>(msg.GetApiFunction(1));
			if (g_iwpInterface)
			{
				LOG_INFO("Got IWP interface via request (build %u).", g_iwpInterface->GetBuildNumber());
			}
		}
	}

	bool EmbedTryCaptureOnRelease(
		const bool isLeftGameHand,
		const bool isLeftVRController,
		const UInt32 weaponFormID)
	{
		if (!stickOnRelease || !g_task || weaponFormID == 0)
		{
			return false;
		}

		// Pose comes from the physical weapon, captured BEFORE the unequip destroys it.
		NiMatrix33 weaponRot;
		NiPoint3 weaponPosMeters;
		bool havePose = RawHavok::ReadHkTransform(GetHiggsBody(isLeftVRController, true), weaponRot, weaponPosMeters);
		if (!havePose)
		{
			havePose = RawHavok::ReadHkTransform(GetHiggsBody(isLeftVRController, false), weaponRot, weaponPosMeters);
		}

		Actor* victim = nullptr;
		NiAVObject* bone = nullptr;
		float boneDist = -1.0f;
		// How far the two ENDS of the blade are from the bone that was chosen. The gap between
		// them is how far the blade actually went in - see EmbedBuryRules.h. Negative means the
		// geometric path never ran, which is the case whenever IWP reported a real wound.
		float geoGripDist = -1.0f;
		float geoTipDist = -1.0f;
		NiPoint3 geoTip;
		NiPoint3 pullAxisWorld(0.0f, 0.0f, 1.0f);

		bool fromIwp = false;
		NiPoint3 woundWorld;
		NiPoint3 channelAxisWorld;
		BSFixedString boneName;
		float depthFrac = 0.0f;
		float bladeLen = 0.0f;

		// IWP knows exactly where the wound is — prefer its answer over the geometric
		// guess, and take the description with us so the embed can be handed back.
		// UseIwpWoundData=0 skips every IWP call on this path entirely and falls back
		// to the built-in geometric stab detection.
		if (g_iwpInterface && useIwpWoundData)
		{
			// IWP's EmbedInfo is mirrored here from its published header, but the
			// installed build can be newer than the one this was written against. Give
			// the call a generously padded, zeroed buffer so a grown struct writes into
			// slack instead of over the stack, and sanity-check what comes back before
			// trusting any of it.
			struct PaddedEmbedInfo
			{
				IwpAPI::EmbedInfo info;
				unsigned char slack[512];
			};
			PaddedEmbedInfo padded{};

			IwpAPI::EmbedInfo& info = padded.info;

			// Step-by-step trace. The stab crash leaves no crash log and no output at
			// all between the query and the first result line, so each step announces
			// itself: whichever trace is LAST in the log is the statement that died.
			EMBED_TRACE("Embed trace: 1 querying IWP for %s hand", isLeftVRController ? "left" : "right");
			const bool iwpReportsEmbed = g_iwpInterface->GetEmbedInfo(isLeftVRController, info);
			EMBED_TRACE("Embed trace: 2 query returned %s", iwpReportsEmbed ? "embedded" : "not embedded");
			const bool infoLooksSane =
				info.depthFrac >= 0.0f && info.depthFrac <= 1.0f &&
				info.bladeLen >= 0.0f && info.bladeLen <= 1000.0f;
			if (iwpReportsEmbed && info.bone && !infoLooksSane)
			{
				LOG_ERR(
					"Embed: IWP returned wound data this build does not understand (depth=%.3f bladeLen=%.1f) — "
					"ignoring it and dropping normally. Check for an IWP interface update.",
					info.depthFrac,
					info.bladeLen);
			}
			else if (iwpReportsEmbed && info.bone)
			{
				fromIwp = true;
				EMBED_TRACE("Embed trace: 3 reading bone pointer");
				bone = info.bone.get();
				EMBED_TRACE("Embed trace: 4 resolving victim handle %08X", info.victimHandle);
				victim = ResolveActorByHandle(info.victimHandle);
				EMBED_TRACE("Embed trace: 5 copying bone name");
				boneName = info.boneName;
				EMBED_TRACE("Embed trace: 6 copying wound values");
				woundWorld = info.woundPos;
				channelAxisWorld = info.axis;
				depthFrac = info.depthFrac;
				bladeLen = info.bladeLen;
				EMBED_TRACE("Embed trace: 7 wound values copied");
				pullAxisWorld = NiPoint3(-info.axis.x, -info.axis.y, -info.axis.z);
				boneDist = 0.0f;

				LOG_INFO(
					"Embed: IWP reports a lodged blade: bone='%s' depth=%.0f%% bladeLen=%.0f",
					boneName.data ? boneName.data : "<none>",
					depthFrac * 100.0f,
					bladeLen);

				// Stop IWP driving the hand for a weapon we are about to unequip.
				EMBED_TRACE("Embed trace: 8 calling EndEmbed");
				g_iwpInterface->EndEmbed(isLeftVRController);
				EMBED_TRACE("Embed trace: 9 EndEmbed returned");
			}
		}

		// Geometric fallback: probe the middle and tip of the blade (from the collision
		// shape's own AABB) for a nearby skeleton bone.
		if (!fromIwp && havePose)
		{
			NiPoint3 midMeters, originMeters;
			if (ReadShapeAabb(GetHiggsBody(isLeftVRController, true), midMeters, originMeters))
			{
				const NiPoint3 mid(
					midMeters.x * kHavokToSkyrim, midMeters.y * kHavokToSkyrim, midMeters.z * kHavokToSkyrim);
				const NiPoint3 grip(
					originMeters.x * kHavokToSkyrim, originMeters.y * kHavokToSkyrim, originMeters.z * kHavokToSkyrim);
				const NiPoint3 tip(
					grip.x + (mid.x - grip.x) * 2.0f,
					grip.y + (mid.y - grip.y) * 2.0f,
					grip.z + (mid.z - grip.z) * 2.0f);

				const NiPoint3 gripToMid(grip.x - mid.x, grip.y - mid.y, grip.z - mid.z);
				const float len = std::sqrt(
					gripToMid.x * gripToMid.x + gripToMid.y * gripToMid.y + gripToMid.z * gripToMid.z);
				if (len > 1e-3f)
				{
					pullAxisWorld = NiPoint3(gripToMid.x / len, gripToMid.y / len, gripToMid.z / len);
				}

				const NiPoint3 probes[3] = {
					mid,
					tip,
					NiPoint3((mid.x + tip.x) * 0.5f, (mid.y + tip.y) * 0.5f, (mid.z + tip.z) * 0.5f)
				};
				for (int i = 0; i < 3; ++i)
				{
					Actor* candidateActor = nullptr;
					float dist = -1.0f;
					NiAVObject* candidate = NearestActorBone(probes[i], embedRadius, &candidateActor, &dist);
					if (candidate && (boneDist < 0.0f || dist < boneDist))
					{
						bone = candidate;
						victim = candidateActor;
						boneDist = dist;
					}
				}

				// Measure both ENDS against the bone that won, so the gate below can ask whether
				// the blade went IN rather than merely came NEAR.
				if (bone)
				{
					const NiPoint3 bonePos = bone->m_worldTransform.pos;
					const NiPoint3 gripRel(grip.x - bonePos.x, grip.y - bonePos.y, grip.z - bonePos.z);
					const NiPoint3 tipRel(tip.x - bonePos.x, tip.y - bonePos.y, tip.z - bonePos.z);
					geoGripDist = std::sqrt(gripRel.x * gripRel.x + gripRel.y * gripRel.y + gripRel.z * gripRel.z);
					geoTipDist = std::sqrt(tipRel.x * tipRel.x + tipRel.y * tipRel.y + tipRel.z * tipRel.z);
					geoTip = tip;
				}
			}
		}

		if (!bone || !havePose)
		{
			return false; // not a stab — the caller drops it normally
		}

		// ⚠ THE GEOMETRIC PATH MUST SHOW THE BLADE WENT IN. Finding a bone within EmbedRadius
		// (35 units - about half a metre) only says the weapon was let go NEAR somebody; on its
		// own that pinned weapons in mid air beside corpses that were never touched, and then
		// made the player drag them free as though they had been buried. IWP's own answer is
		// NOT gated: when it reports a wound it knows exactly where the blade is.
		// And since M3.0.0-test8 the TIP must be inside one of the victim's body parts: a blade POINTED
		// at somebody passes the gap test as well as one buried in them (the 2026-09-22 floating pins).
		TipInBody tipTest;
		if (!fromIwp)
		{
			EmbedBury::Config buryConfig;
			buryConfig.minBuryUnits = embedMinBuryUnits;
			buryConfig.tipMustBeInBody = embedNeedsTipInBody;
			if (buryConfig.tipMustBeInBody && EmbedBury::LooksBuried(geoGripDist, geoTipDist, buryConfig))
			{
				tipTest = TestTipInVictimBody(victim, geoTip);
			}
			switch (EmbedBury::JudgeGeometric(geoGripDist, geoTipDist, tipTest.looked, tipTest.inside, buryConfig))
			{
			case EmbedBury::Verdict::kNotBuried:
				LOG_INFO(
					"Embed: the %s controller let go %.1f units from '%s', but only %.1f units of the "
					"blade were in it (%.1f needed) - dropping it normally instead of pinning it.",
					isLeftVRController ? "left" : "right",
					boneDist,
					bone->m_name ? bone->m_name : "<bone>",
					EmbedBury::BuryUnits(geoGripDist, geoTipDist),
					embedMinBuryUnits);
				return false;
			case EmbedBury::Verdict::kTipOutsideBody:
				LOG_INFO(
					"Embed: the %s controller let go with the blade pointed %.1f units in towards '%s', but its tip "
					"is inside none of the %d body parts tested (nearest: '%s', %.1f units away) - dropping it "
					"normally instead of pinning it in the air.",
					isLeftVRController ? "left" : "right",
					EmbedBury::BuryUnits(geoGripDist, geoTipDist),
					bone->m_name ? bone->m_name : "<bone>",
					tipTest.looked,
					tipTest.nearestName ? tipTest.nearestName : "<none>",
					tipTest.nearestUnits);
				return false;
			case EmbedBury::Verdict::kNoBodies:
				LOG_INFO(
					"Embed: the %s controller let go with the blade pointed %.1f units in towards '%s', but none of "
					"that body's parts could be tested, so it cannot be shown to be in - dropping it normally "
					"(bEmbedNeedsTipInBody = 0 pins on the pointing alone, as before).",
					isLeftVRController ? "left" : "right",
					EmbedBury::BuryUnits(geoGripDist, geoTipDist),
					bone->m_name ? bone->m_name : "<bone>");
				return false;
			default:
				break;
			}
		}

		const NiPoint3 weaponWorldPos(
			weaponPosMeters.x * kHavokToSkyrim,
			weaponPosMeters.y * kHavokToSkyrim,
			weaponPosMeters.z * kHavokToSkyrim);

		EMBED_TRACE("Embed trace: 10 reading bone world transform");
		const NiTransform boneWorld = bone->m_worldTransform;
		const NiMatrix33 boneRotT = Mat3Transpose(boneWorld.rot);
		const float invScale = boneWorld.scale > 0.0f ? 1.0f / boneWorld.scale : 1.0f;

		PendingPin& pending = PendingFor(isLeftVRController);
		pending = PendingPin{};
		pending.active = true;
		pending.victimHandle = 0;
		if (victim)
		{
			pending.victimHandle = victim->CreateRefHandle();
		}
		pending.bone = bone;
		pending.relRot = Mat3Mul(boneRotT, weaponRot);
		const NiPoint3 rel(
			weaponWorldPos.x - boneWorld.pos.x,
			weaponWorldPos.y - boneWorld.pos.y,
			weaponWorldPos.z - boneWorld.pos.z);
		const NiPoint3 relLocal = Mat3MulVec(boneRotT, rel);
		pending.relPos = NiPoint3(relLocal.x * invScale, relLocal.y * invScale, relLocal.z * invScale);
		pending.pullAxisLocal = Mat3MulVec(boneRotT, pullAxisWorld);
		pending.fromIwp = fromIwp;
		pending.boneName = fromIwp ? boneName : BSFixedString(bone->m_name ? bone->m_name : "");
		pending.depthFrac = depthFrac;
		pending.bladeLen = bladeLen;
		if (fromIwp)
		{
			pending.woundLocal = InverseTransformPoint(boneWorld, woundWorld);
			pending.axisLocal = Mat3MulVec(boneRotT, channelAxisWorld);
		}
		else
		{
			pending.woundLocal = pending.relPos;
			pending.axisLocal = NiPoint3(
				-pending.pullAxisLocal.x, -pending.pullAxisLocal.y, -pending.pullAxisLocal.z);
		}

		EMBED_TRACE("Embed trace: 11 queueing the stick drop");
		g_task->AddTask(new EmbedStickDropTask(isLeftGameHand, isLeftVRController, weaponFormID));

		LOG_INFO(
			"Embed: %s controller released an embedded weapon — sticking it (%s, boneDist=%.1f, blade in by %.1f units%s%s%s)",
			isLeftVRController ? "left" : "right",
			fromIwp ? "IWP wound" : "geometric",
			boneDist,
			fromIwp ? bladeLen * depthFrac : EmbedBury::BuryUnits(geoGripDist, geoTipDist),
			tipTest.inside ? ", tip inside '" : "",
			tipTest.inside && tipTest.insideName ? tipTest.insideName : "",
			tipTest.inside ? "'" : "");
		return true;
	}

	bool EmbedIsRefPinned(const UInt32 refID)
	{
		if (refID == 0)
		{
			return false;
		}

		for (int i = 0; i < kMaxPins; ++i)
		{
			if (s_pins[i].active && s_pins[i].refID == refID)
			{
				return true;
			}
		}

		// Also covers the frames between the stick drop and the pin existing.
		return (s_pending[0].active && s_pending[0].dropRefID == refID) ||
			(s_pending[1].active && s_pending[1].dropRefID == refID);
	}

	void EmbedServicePins(const float deltaTime)
	{
		ServicePendingPin(s_pending[0], false);
		ServicePendingPin(s_pending[1], true);

		for (int i = 0; i < kMaxPins; ++i)
		{
			EmbedPin& pin = s_pins[i];
			if (!pin.active)
			{
				continue;
			}

			pin.age += deltaTime;
			if (embedMaxSeconds > 0.0f && pin.age > embedMaxSeconds)
			{
				ReleasePin(pin, "timed out");
				continue;
			}

			TESObjectREFR* refr = ResolveRefByID(pin.refID);
			if (!refr || !pin.bone || !pin.body)
			{
				// Losing the world model WHILE THE PLAYER IS PULLING IT is the reported
				// "I extracted the blade and it went into my inventory". The ref can be
				// taken out from under the pin - an auto-loot mod, the corpse being
				// cleaned up, the cell unloading - and the weapon then exists only as an
				// inventory entry, so the extraction ends with an empty hand.
				//
				// If the player owns it, finish the extraction the way it was going to
				// end anyway and put it in the hand that was doing the pulling.
				if (pin.heldBy != 0 && pin.weaponFormID != 0)
				{
					PlayerCharacter* player = *g_thePlayer;
					TESForm* weaponForm = LookupFormByID(pin.weaponFormID);
					const bool isLeftVRController = pin.heldBy == 2;

					if (player && weaponForm && PlayerItemCount(player, weaponForm) > 0)
					{
						LOG_ERR(
							"Embed: the lodged weapon's world model vanished mid-pull (looted, "
							"or the body was cleaned up). It is in the player's inventory, so "
							"it is being put back into the %s hand rather than lost.",
							isLeftVRController ? "left" : "right");
						ScheduleGripHandOff(VRControllerToGameHand(isLeftVRController), pin.weaponFormID);
					}
					else
					{
						LOG_ERR(
							"Embed: the lodged weapon's world model vanished mid-pull and it is "
							"NOT in the player's inventory - formId=%08X is gone.",
							pin.weaponFormID);
					}
				}

				ReleasePin(pin, "reference or bone gone");
				continue;
			}

			// The NiPointer keeps the bone alive even after the victim unloads, and a
			// detached node's world transform is stale — the victim's 3D is the real
			// liveness test.
			if (pin.victimHandle != 0)
			{
				Actor* victim = ResolveActorByHandle(pin.victimHandle);
				if (!victim || !victim->GetNiNode())
				{
					ReleasePin(pin, "victim unloaded");
					continue;
				}
			}

			void* hk = RawHavok::GetHkpRigidBody(pin.body.get());
			if (!hk)
			{
				ReleasePin(pin, "rigid body gone");
				continue;
			}

			const NiTransform boneWorld = pin.bone->m_worldTransform;
			NiPoint3 axisWorld = Mat3MulVec(boneWorld.rot, pin.pullAxisLocal);
			const float axisLen = std::sqrt(
				axisWorld.x * axisWorld.x + axisWorld.y * axisWorld.y + axisWorld.z * axisWorld.z);
			if (axisLen > 1e-4f)
			{
				axisWorld.x /= axisLen;
				axisWorld.y /= axisLen;
				axisWorld.z /= axisLen;
			}

			// Being drawn out: slide along the blade axis by however far the hand has
			// travelled that way.
			if (pin.heldBy != 0)
			{
				const bool isLeft = pin.heldBy == 2;
				NiPoint3 handPos;
				if (!ReadHandPositionSkyrim(isLeft, handPos))
				{
					pin.heldBy = 0;
				}
				else
				{
					const float travel =
						(handPos.x - pin.pullStartHandPos.x) * axisWorld.x +
						(handPos.y - pin.pullStartHandPos.y) * axisWorld.y +
						(handPos.z - pin.pullStartHandPos.z) * axisWorld.z;
					float offset = pin.pullStartOffset + travel;
					if (offset < 0.0f)
					{
						offset = 0.0f;
					}
					else if (offset > pin.requiredPull)
					{
						offset = pin.requiredPull;
					}
					pin.pullOffset = offset;

					if (pin.pullOffset >= pin.requiredPull - 0.01f)
					{
						const UInt32 refID = pin.refID;
						ReleasePin(pin, "pulled free");
						TESObjectREFR* freed = ResolveRefByID(refID);
						if (freed)
						{
							ScheduleGrabbedWeaponActivation(isLeft, freed);
						}
						continue;
					}
				}
			}

			const NiPoint3 localPos(
				pin.relPos.x + pin.pullAxisLocal.x * pin.pullOffset,
				pin.relPos.y + pin.pullAxisLocal.y * pin.pullOffset,
				pin.relPos.z + pin.pullAxisLocal.z * pin.pullOffset);
			const NiPoint3 scaled(
				localPos.x * boneWorld.scale, localPos.y * boneWorld.scale, localPos.z * boneWorld.scale);
			const NiPoint3 rotated = Mat3MulVec(boneWorld.rot, scaled);
			const NiPoint3 targetPos(
				boneWorld.pos.x + rotated.x, boneWorld.pos.y + rotated.y, boneWorld.pos.z + rotated.z);
			const NiMatrix33 targetRot = Mat3Mul(boneWorld.rot, pin.relRot);

			if (!pin.smoothInit)
			{
				pin.smoothPos = targetPos;
				pin.smoothRot = targetRot;
				pin.smoothInit = true;
			}
			else
			{
				const float a = 1.0f - embedSmoothing;
				pin.smoothPos.x += (targetPos.x - pin.smoothPos.x) * a;
				pin.smoothPos.y += (targetPos.y - pin.smoothPos.y) * a;
				pin.smoothPos.z += (targetPos.z - pin.smoothPos.z) * a;
				pin.smoothRot = BlendRot(pin.smoothRot, targetRot, a);
			}
			pin.lastWorldPos = pin.smoothPos;

			if (!pin.tracedFirstWrite)
			{
				pin.tracedFirstWrite = true;
				EMBED_TRACE("Embed trace: 19 writing the pinned pose for the first time");
			}

			__declspec(align(16)) float havokPos[4] = {
				pin.smoothPos.x / kHavokToSkyrim,
				pin.smoothPos.y / kHavokToSkyrim,
				pin.smoothPos.z / kHavokToSkyrim,
				0.0f
			};
			__declspec(align(16)) float quat[4];
			ToQuat(pin.smoothRot, quat);
			RawHavok::hkpRigidBody_setPositionAndRotation(hk, havokPos, quat);
			RawHavok::WriteHkVelocities(hk, NiPoint3(0.0f, 0.0f, 0.0f), NiPoint3(0.0f, 0.0f, 0.0f));
			// A sleeping island stops publishing its transform to the node.
			RawHavok::hkpEntity_activate(hk);

			if (!pin.tracedFirstDone)
			{
				pin.tracedFirstDone = true;
				EMBED_TRACE("Embed trace: 20 first pinned frame complete — the pin is live");
			}
		}
	}

	bool EmbedTryStartPull(const bool isLeftVRController)
	{
		NiPoint3 handPos;
		if (!ReadHandPositionSkyrim(isLeftVRController, handPos))
		{
			return false;
		}

		EmbedPin* best = nullptr;
		float bestDist = pullReach;
		for (int i = 0; i < kMaxPins; ++i)
		{
			EmbedPin& pin = s_pins[i];
			if (!pin.active || pin.heldBy != 0 || !pin.smoothInit)
			{
				continue;
			}

			const float dx = pin.lastWorldPos.x - handPos.x;
			const float dy = pin.lastWorldPos.y - handPos.y;
			const float dz = pin.lastWorldPos.z - handPos.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist < bestDist)
			{
				bestDist = dist;
				best = &pin;
			}
		}

		if (!best)
		{
			return false;
		}

		// Physical extraction comes first: take hold of the blade where it sits and
		// drag it out along its own axis, which is the whole point of the feature.
		//
		// Handing straight to IWP (below) re-equips the weapon the instant you touch
		// it, so however carefully you grab the tip or the side, the blade teleports
		// into your hand before IWP's resistance ever applies. That path is still here
		// for anyone who wants IWP to own the extraction — set PullOut = 0.
		if (resumeIwpEmbed && !pullOut && g_iwpInterface && best->fromIwp && best->bone)
		{
			ResumeState& resume = ResumeFor(isLeftVRController);
			resume = ResumeState{};
			resume.pending = true;
			resume.framesLeft = 0; // armed once the equip lands
			resume.victimHandle = best->victimHandle;
			resume.bone = best->bone;
			resume.boneName = best->boneName;
			// Bone frame: the victim keeps moving between taking hold and the resume firing.
			resume.woundLocal = best->woundLocal;
			resume.axisLocal = best->axisLocal;
			resume.depthFrac = best->depthFrac;
			resume.bladeLen = best->bladeLen;

			const UInt32 refID = best->refID;
			LOG_INFO(
				"Embed: %s controller took hold of a lodged weapon at %.1fu — handing the extraction to IWP (depth %.0f%%)",
				isLeftVRController ? "left" : "right",
				bestDist,
				best->depthFrac * 100.0f);
			ReleasePin(*best, "handed to IWP");

			TESObjectREFR* refr = ResolveRefByID(refID);
			if (refr)
			{
				ScheduleGrabbedWeaponActivation(isLeftVRController, refr);
			}
			else
			{
				resume = ResumeState{};
			}
			return true;
		}

		if (!pullOut)
		{
			return false;
		}

		best->heldBy = isLeftVRController ? 2 : 1;
		best->pullStartHandPos = handPos;
		best->pullStartOffset = best->pullOffset;
		LOG_INFO(
			"Embed: %s controller took hold of a lodged weapon at %.1fu — drag it %.0fu along the blade to work it free",
			isLeftVRController ? "left" : "right",
			bestDist,
			best->requiredPull - best->pullOffset);
		return true;
	}

	void EmbedReleasePullBy(const bool isLeftVRController)
	{
		const int tag = isLeftVRController ? 2 : 1;
		for (int i = 0; i < kMaxPins; ++i)
		{
			if (s_pins[i].active && s_pins[i].heldBy == tag)
			{
				s_pins[i].heldBy = 0;
			}
		}
	}

	void EmbedOnHandEquipped(const bool isLeftVRController)
	{
		ResumeState& resume = ResumeFor(isLeftVRController);
		if (!resume.pending || resume.framesLeft > 0)
		{
			return;
		}

		// The resume cannot fire here: IWP drops any embed whose weapon is not drawn,
		// and a weapon equipped this instant is not drawn yet. Arm it and let the
		// per-frame service fire it once the draw has actually happened.
		resume.framesLeft = kResumeWaitFrames;
		PlayerCharacter* player = *g_thePlayer;
		if (player && PlayerWeaponState(player) != 3)
		{
			ForcePlayerWeaponDraw(player);
		}
	}

	void EmbedServiceResume(const bool isLeftVRController, const float deltaTime)
	{
		ResumeState& resume = ResumeFor(isLeftVRController);
		PlayerCharacter* player = *g_thePlayer;

		// After a successful resume: hold the weapon at kDrawn for a moment — IWP ends
		// an embed the instant the weapon leaves that state.
		if (resume.drawGuard > 0.0f && player)
		{
			resume.drawGuard -= deltaTime;
			resume.redrawCooldown -= deltaTime;
			if (PlayerWeaponState(player) != 3 && resume.redrawCooldown <= 0.0f)
			{
				ForcePlayerWeaponDraw(player);
				resume.redrawCooldown = 0.3f;
			}
		}

		if (!resume.pending || resume.framesLeft <= 0)
		{
			return;
		}

		if (!player)
		{
			resume = ResumeState{};
			return;
		}

		// Exactly kDrawn (3) — the same test IWP applies.
		if (PlayerWeaponState(player) != 3)
		{
			resume.redrawCooldown -= deltaTime;
			if (resume.redrawCooldown <= 0.0f)
			{
				ForcePlayerWeaponDraw(player);
				resume.redrawCooldown = 0.3f;
			}
			if (--resume.framesLeft <= 0)
			{
				LOG_ERR("Embed: weapon never reached the drawn state — resume abandoned, the weapon is just held");
				resume = ResumeState{};
			}
			return;
		}

		resume.pending = false;
		resume.framesLeft = 0;

		bool resumed = false;
		if (g_iwpInterface && resume.bone)
		{
			const NiTransform boneWorld = resume.bone->m_worldTransform;
			const NiPoint3 scaledWound(
				resume.woundLocal.x * boneWorld.scale,
				resume.woundLocal.y * boneWorld.scale,
				resume.woundLocal.z * boneWorld.scale);
			const NiPoint3 rotatedWound = Mat3MulVec(boneWorld.rot, scaledWound);
			const NiPoint3 woundWorld(
				boneWorld.pos.x + rotatedWound.x,
				boneWorld.pos.y + rotatedWound.y,
				boneWorld.pos.z + rotatedWound.z);
			NiPoint3 axisWorld = Mat3MulVec(boneWorld.rot, resume.axisLocal);
			const float len = std::sqrt(
				axisWorld.x * axisWorld.x + axisWorld.y * axisWorld.y + axisWorld.z * axisWorld.z);
			if (len > 1e-4f)
			{
				axisWorld.x /= len;
				axisWorld.y /= len;
				axisWorld.z /= len;
			}

			Actor* victim = ResolveActorByHandle(resume.victimHandle);
			resumed = g_iwpInterface->ResumeEmbed(
				isLeftVRController,
				victim,
				resume.bone.get(),
				resume.boneName,
				woundWorld,
				axisWorld,
				resume.bladeLen,
				resume.depthFrac);
		}

		if (resumed)
		{
			resume.drawGuard = resumeDrawGuard;
			LOG_INFO(
				"Embed: %s controller — IWP resumed the embed at %.0f%%, pull it out",
				isLeftVRController ? "left" : "right",
				resume.depthFrac * 100.0f);
		}
		else
		{
			LOG_ERR("Embed: IWP declined to resume the embed — the weapon is just held");
		}

		resume.bone = nullptr;
		resume.victimHandle = 0;
	}

	void EmbedOnPreLoadGame()
	{
		for (int i = 0; i < kMaxPins; ++i)
		{
			ReleasePin(s_pins[i], "game load");
		}
		s_pending[0] = PendingPin{};
		s_pending[1] = PendingPin{};
		s_resume[0] = ResumeState{};
		s_resume[1] = ResumeState{};
	}
}
