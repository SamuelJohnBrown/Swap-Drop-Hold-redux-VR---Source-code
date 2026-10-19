#pragma once

// Minimal raw Havok access for GripHold's throw momentum and embed pins.
//
// The classic SKSE64 SDK barely wraps Havok, so the layout constants and the two
// entry points below are taken from sources proven ON THIS RUNTIME (SkyrimVR
// 1.4.15): the function offsets and every struct offset were shipped and
// confirmed in-game by Weapon Pick Up VR 0.8.0 (built on CommonLibVR headers,
// whose layouts these constants mirror 1:1):
//
//   bhkRefObject::referencedObject                 +0x10   (-> hkpRigidBody*)
//   hkpEntity::motion (hkpMaxSizeMotion)           +0x150
//   hkpMotion::motionState.transform               +0x20   (=> entity +0x170)
//   hkTransform: rotation = 3 column hkVector4s (0x30), translation +0x30 (0x10)
//   hkpMotion::linearVelocity / angularVelocity    +0xE0 / +0xF0 (=> +0x230/+0x240)
//   hkpWorldObject::collidable                     +0x20
//   hkpCollidable::broadPhaseHandle                +0x24
//   hkpTypedBroadPhaseHandle::collisionFilterInfo  +0x08   (=> entity +0x4C)
//   CFilter::kNoCollision                          1 << 14
//   NiAVObject collision object slot               +0x40   (classic: unk040)
//   bhkNiCollisionObject::body                     +0x20   (-> bhkWorldObject*)

#include "Mat3Rules.h"

#include "skse64/NiNodes.h"
#include "skse64/NiObjects.h"
#include "skse64/NiRTTI.h"
#include "skse64_common/Relocation.h"

#include <cmath>

namespace SwapDropAndHoldRedux
{
	namespace RawHavok
	{
		static const float kHavokToSkyrim = 69.99125f;
		static const UInt32 kFilterNoCollision = 1u << 14;

		// ---- small matrix helpers (classic NiMatrix33 is row-major, data[3][3]: NO fourth column) ----
		// Mat3Transpose, Mat3Mul, Mat3Lerp and ReadHkTransform's column-to-row copy live in Mat3Rules.h,
		// where tools_mat3_tests.cpp checks that none of them writes past the matrix.

		inline NiPoint3 Mat3MulVec(const NiMatrix33& m, const NiPoint3& v)
		{
			return NiPoint3(
				m.data[0][0] * v.x + m.data[0][1] * v.y + m.data[0][2] * v.z,
				m.data[1][0] * v.x + m.data[1][1] * v.y + m.data[1][2] * v.z,
				m.data[2][0] * v.x + m.data[2][1] * v.y + m.data[2][2] * v.z);
		}

		// NiMatrix33 (row-major) -> quaternion (x, y, z, w) for the raw Havok pose setter.
		inline void ToQuat(const NiMatrix33& m, float outQuat[4])
		{
			const float m00 = m.data[0][0], m01 = m.data[0][1], m02 = m.data[0][2];
			const float m10 = m.data[1][0], m11 = m.data[1][1], m12 = m.data[1][2];
			const float m20 = m.data[2][0], m21 = m.data[2][1], m22 = m.data[2][2];
			float x, y, z, w;
			const float trace = m00 + m11 + m22;
			if (trace > 0.0f)
			{
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				w = 0.25f * s;
				x = (m21 - m12) / s;
				y = (m02 - m20) / s;
				z = (m10 - m01) / s;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				w = (m21 - m12) / s;
				x = 0.25f * s;
				y = (m01 + m10) / s;
				z = (m02 + m20) / s;
			}
			else if (m11 > m22)
			{
				const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				w = (m02 - m20) / s;
				x = (m01 + m10) / s;
				y = 0.25f * s;
				z = (m12 + m21) / s;
			}
			else
			{
				const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				w = (m10 - m01) / s;
				x = (m02 + m20) / s;
				y = (m12 + m21) / s;
				z = 0.25f * s;
			}
			outQuat[0] = x;
			outQuat[1] = y;
			outQuat[2] = z;
			outQuat[3] = w;
		}

		// hkpEntity::activate — wake the body's simulation island. A sleeping body
		// ignores velocity writes and stops publishing its transform to its node.
		typedef void (*hkpEntityActivate_t)(void* hkpEntity);
		static RelocAddr<hkpEntityActivate_t> hkpEntity_activate(0x00AA7130);

		// hkpRigidBody::setPositionAndRotation(hkVector4& pos, hkQuaternion& rot),
		// both in Havok metres / normalized quaternion (x, y, z, w).
		typedef void (*hkpRigidBodySetPosRot_t)(void* hkpRigidBody, const float* hkVec4Pos, const float* hkQuatRot);
		static RelocAddr<hkpRigidBodySetPosRot_t> hkpRigidBody_setPositionAndRotation(0x00AA9030);

		// bhkRigidBody (NiObject) -> the wrapped hkpRigidBody.
		inline void* GetHkpRigidBody(NiObject* bhkRigidBody)
		{
			if (!bhkRigidBody)
			{
				return nullptr;
			}

			return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bhkRigidBody) + 0x10);
		}

		// Body world transform in Havok metres. The motion state's rotation is
		// column-major (3 hkVector4 columns); NiMatrix33 is row-major.
		inline bool ReadHkTransform(NiObject* bhkRigidBody, NiMatrix33& outRot, NiPoint3& outPosMeters)
		{
			void* hk = GetHkpRigidBody(bhkRigidBody);
			if (!hk)
			{
				return false;
			}

			const float* t = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(hk) + 0x170);
			HkTransformToRows(t, outRot, outPosMeters);
			return true;
		}

		// Write both velocities (Havok m/s) directly into the motion.
		inline void WriteHkVelocities(void* hkpRigidBody, const NiPoint3& linear, const NiPoint3& angular)
		{
			if (!hkpRigidBody)
			{
				return;
			}

			float* lin = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(hkpRigidBody) + 0x230);
			float* ang = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(hkpRigidBody) + 0x240);
			lin[0] = linear.x;
			lin[1] = linear.y;
			lin[2] = linear.z;
			lin[3] = 0.0f;
			ang[0] = angular.x;
			ang[1] = angular.y;
			ang[2] = angular.z;
			ang[3] = 0.0f;
		}

		// Collision filter word (entity +0x4C): save/restore around kNoCollision.
		inline UInt32* GetFilterInfo(void* hkpRigidBody)
		{
			if (!hkpRigidBody)
			{
				return nullptr;
			}

			return reinterpret_cast<UInt32*>(reinterpret_cast<uintptr_t>(hkpRigidBody) + 0x4C);
		}

		// The rigid body of a dropped reference. A weapon's collision usually hangs
		// off a CHILD of the reference's root node, not the root itself, so the whole
		// subtree has to be searched — looking only at the root is why a thrown
		// weapon used to just fall (WPVR 0.2.0 lesson).
		inline NiObject* FindRigidBodyInSubtree(NiAVObject* obj)
		{
			if (!obj)
			{
				return nullptr;
			}

			NiAVObject* collisionObject = obj->unk040;
			if (collisionObject)
			{
				NiObject* body = *reinterpret_cast<NiObject**>(
					reinterpret_cast<uintptr_t>(collisionObject) + 0x20);
				// DoNiRTTICast, never a raw pointer compare: the SDK's NiRTTI_* globals
				// are UNRELOCATED RVAs (e.g. 0x03187F58) that only match once
				// RelocationManager::s_baseAddr is added, which is what the cast does.
				// A direct `GetRTTI() == NiRTTI_bhkRigidBody` is always false — that is
				// why every thrown weapon landed dead. The cast also walks the parent
				// chain, so bhkRigidBodyT matches through bhkRigidBody.
				if (body && DoNiRTTICast(body, NiRTTI_bhkRigidBody))
				{
					return body;
				}
			}

			NiNode* node = obj->GetAsNiNode();
			if (node)
			{
				for (UInt32 i = 0; i < node->m_children.m_size; ++i)
				{
					if (NiObject* found = FindRigidBodyInSubtree(node->m_children.m_data[i]))
					{
						return found;
					}
				}
			}

			return nullptr;
		}
	}
}
