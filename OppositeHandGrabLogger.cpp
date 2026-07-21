#include "OppositeHandGrabLogger.h"

#include "Engine.h"
#include "Helper.h"
#include "TrackedWeapons.h"
#include "WeaponGrabActivator.h"
#include "config.h"

#include <skse64/GameObjects.h>
#include <skse64/GameVR.h>
#include <skse64/NiRTTI.h>
#include <skse64/gamethreads.h>

#include <chrono>
#include <cmath>

namespace SwapDropAndHoldRedux
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		static const uint64_t GRIP_BUTTON_MASK = (1ull << 2);
		static const float kHavokToSkyrim = 69.99125f;
		static const float kOppositeGrabMaxDistance = 50.0f;
		static const float kOppositeGrabReleaseGraceSeconds = 0.35f;
		static const float kSwapPullSampleWindowSeconds = 0.10f;
		static const float kSwapPullMaxVerticalRatio = 1.25f;
		static bool s_registered = false;

		struct hkVector4
		{
			float x;
			float y;
			float z;
			float w;
		};

		struct EquippedHandGrabState
		{
			bool wasRawOppositeGrabActive = false;
			bool latchedOppositeGrab = false;
			bool wasSwapPullDetected = false;
			bool hasPreviousOppositeHandPos = false;
			bool hasWindowOppositeHandPos = false;
			float oppositeGrabReleaseTimer = 0.0f;
			float oppositeHandWindowAge = 0.0f;
			float latchStartHandDistance = 0.0f;
			NiPoint3 previousOppositeHandPos{};
			NiPoint3 windowOppositeHandPos{};
			NiPoint3 latchEquippedAnchorPos{};
		};

		static EquippedHandGrabState s_leftEquippedGrabState;
		static EquippedHandGrabState s_rightEquippedGrabState;
		static RelocPtr<bool> s_leftHandedMode(0x01E71778);

		bool VRControllerToGameHand(const bool isLeftVRController)
		{
			if (s_leftHandedMode && *s_leftHandedMode)
			{
				return !isLeftVRController;
			}

			return isLeftVRController;
		}

		NiPoint3 GetEquippedHandWorldPosition(PlayerCharacter* player, const bool isLeftEquippedHand)
		{
			NiPoint3 handPos = player->pos;

			NiNode* rootNode = player->GetNiRootNode(0);
			if (!rootNode)
			{
				rootNode = player->GetNiRootNode(1);
			}

			if (!rootNode)
			{
				return handPos;
			}

			const bool isLeftVRController = GameHandToVRController(isLeftEquippedHand);
			const char* handNodeName = isLeftVRController ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]";
			BSFixedString handNodeStr(handNodeName);
			NiAVObject* handNode = rootNode->GetObjectByName(&handNodeStr.data);
			if (handNode)
			{
				handPos = handNode->m_worldTransform.pos;
			}

			return handPos;
		}

		float DistanceBetween(const NiPoint3& a, const NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		float LengthHorizontal(const NiPoint3& v)
		{
			return std::sqrt(v.x * v.x + v.y * v.y);
		}

		NiPoint3 GetPlayerHorizontalRight(PlayerCharacter* player)
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

			const float len = LengthHorizontal(right);
			if (len < 0.0001f)
			{
				return NiPoint3(1.0f, 0.0f, 0.0f);
			}

			right.x /= len;
			right.y /= len;
			return right;
		}

		float Length3D(const NiPoint3& v)
		{
			return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		}

		NiPoint3 Normalize3D(const NiPoint3& v)
		{
			const float len = Length3D(v);
			if (len < 0.0001f)
			{
				return NiPoint3(0.0f, 0.0f, 0.0f);
			}

			return NiPoint3(v.x / len, v.y / len, v.z / len);
		}

		bool TryGetHiggsWeaponPosition(const bool isLeftVRController, NiPoint3& out);

		bool TryGetEquippedWeaponAnchorPosition(
			PlayerCharacter* player,
			const bool isLeftEquippedHand,
			NiPoint3& out)
		{
			const bool equippedVRController = GameHandToVRController(isLeftEquippedHand);
			if (TryGetHiggsWeaponPosition(equippedVRController, out))
			{
				return true;
			}

			out = GetEquippedHandWorldPosition(player, isLeftEquippedHand);
			return player != nullptr;
		}

		bool IsAbruptSwapPull(
			const NiPoint3& equippedAnchorPos,
			const NiPoint3& currentHandPos,
			const NiPoint3& previousHandPos,
			const NiPoint3& windowHandPos,
			const float deltaTime,
			const float windowAge,
			const float latchStartHandDistance,
			float& outPullSpeed)
		{
			outPullSpeed = 0.0f;

			if (deltaTime <= 0.0f)
			{
				return false;
			}

			const NiPoint3 toOpposite{
				currentHandPos.x - equippedAnchorPos.x,
				currentHandPos.y - equippedAnchorPos.y,
				currentHandPos.z - equippedAnchorPos.z};
			const NiPoint3 radialDir = Normalize3D(toOpposite);
			if (radialDir.x == 0.0f && radialDir.y == 0.0f && radialDir.z == 0.0f)
			{
				return false;
			}

			const NiPoint3 instantDelta{
				currentHandPos.x - previousHandPos.x,
				currentHandPos.y - previousHandPos.y,
				currentHandPos.z - previousHandPos.z};
			const NiPoint3 instantVelocity{
				instantDelta.x / deltaTime,
				instantDelta.y / deltaTime,
				instantDelta.z / deltaTime};
			const float instantRadialSpeed =
				instantVelocity.x * radialDir.x +
				instantVelocity.y * radialDir.y +
				instantVelocity.z * radialDir.z;

			float windowRadialSpeed = 0.0f;
			if (windowAge >= kSwapPullSampleWindowSeconds * 0.5f)
			{
				const NiPoint3 windowDelta{
					currentHandPos.x - windowHandPos.x,
					currentHandPos.y - windowHandPos.y,
					currentHandPos.z - windowHandPos.z};
				windowRadialSpeed =
					(windowDelta.x * radialDir.x + windowDelta.y * radialDir.y + windowDelta.z * radialDir.z) /
					windowAge;
			}

			outPullSpeed = instantRadialSpeed > windowRadialSpeed ? instantRadialSpeed : windowRadialSpeed;
			if (outPullSpeed <= 0.0f)
			{
				return false;
			}

			const float currentHandDistance = Length3D(toOpposite);
			const float distanceGrowth = currentHandDistance - latchStartHandDistance;
			const bool fastPull = outPullSpeed >= swapPullSpeedThreshold;
			const bool sustainedPull = distanceGrowth >= swapPullMinDistanceGrowth &&
				outPullSpeed >= swapPullSpeedThreshold * 0.45f;

			return fastPull || sustainedPull;
		}

		bool TryGetOpenVRControllerWorldPosition(const bool isLeftVRController, NiPoint3& out)
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
			{
				return false;
			}

			vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
			const vr_1_0_12::TrackedDeviceIndex_t deviceIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(
				isLeftVRController
					? vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand
					: vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);

			if (deviceIndex == vr_1_0_12::k_unTrackedDeviceIndexInvalid)
			{
				return false;
			}

			vr_1_0_12::TrackedDevicePose_t poses[vr_1_0_12::k_unMaxTrackedDeviceCount]{};
			vrSystem->GetDeviceToAbsoluteTrackingPose(
				vr_1_0_12::TrackingUniverseStanding,
				0.0f,
				poses,
				vr_1_0_12::k_unMaxTrackedDeviceCount);

			if (!poses[deviceIndex].bPoseIsValid)
			{
				vrSystem->GetDeviceToAbsoluteTrackingPose(
					vr_1_0_12::TrackingUniverseSeated,
					0.0f,
					poses,
					vr_1_0_12::k_unMaxTrackedDeviceCount);
			}

			if (!poses[deviceIndex].bPoseIsValid)
			{
				return false;
			}

			NiTransform xForm{};
			HmdMatrixToNiTransform(xForm, poses[deviceIndex].mDeviceToAbsoluteTracking);
			out = xForm.pos;
			return true;
		}

		bool TryGetVRControllerWorldPosition(const bool isLeftVRController, NiPoint3& out)
		{
			if (TryGetOpenVRControllerWorldPosition(isLeftVRController, out))
			{
				return true;
			}

			BSOpenVR* openVR = *g_openVR;
			if (!openVR)
			{
				return false;
			}

			const UInt32 controllerIndex = isLeftVRController ? 0u : 1u;
			NiNode* controllerNode = openVR->controller[controllerIndex];
			if (!controllerNode)
			{
				return false;
			}

			out = controllerNode->m_worldTransform.pos;
			return true;
		}

		bool GetControllerState(const bool isLeftVRController, vr_1_0_12::VRControllerState_t& outState)
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
			{
				return false;
			}

			vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
			const vr_1_0_12::TrackedDeviceIndex_t controller = vrSystem->GetTrackedDeviceIndexForControllerRole(
				isLeftVRController
					? vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand
					: vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);

			return vrSystem->GetControllerState(controller, &outState, sizeof(outState));
		}

		bool IsGripActive(const vr_1_0_12::VRControllerState_t& state)
		{
			return ((state.ulButtonPressed & GRIP_BUTTON_MASK) != 0) ||
				((state.ulButtonTouched & GRIP_BUTTON_MASK) != 0);
		}

		bool IsOppositeControllerGripActive(const bool oppositeVRController)
		{
			vr_1_0_12::VRControllerState_t controllerState{};
			if (!GetControllerState(oppositeVRController, controllerState))
			{
				return false;
			}

			return IsGripActive(controllerState);
		}

		bool TryGetHiggsRigidBodyPosition(NiObject* obj, NiPoint3& out)
		{
			if (!obj)
			{
				return false;
			}

			NiRTTI* rtti = obj->GetRTTI();
			if (rtti != NiRTTI_bhkRigidBody && rtti != NiRTTI_bhkRigidBodyT)
			{
				return false;
			}

			hkVector4 pos{};
			void** vtable = *reinterpret_cast<void***>(obj);
			typedef void(__fastcall * GetPositionFn)(void* thisPtr, hkVector4* outPos);
			const GetPositionFn getPosition = reinterpret_cast<GetPositionFn>(vtable[0x33]);
			getPosition(obj, &pos);

			out.x = pos.x * kHavokToSkyrim;
			out.y = pos.y * kHavokToSkyrim;
			out.z = pos.z * kHavokToSkyrim;
			return true;
		}

		bool TryGetHiggsHandPosition(const bool isLeftVRController, NiPoint3& out)
		{
			if (!higgsInterface)
			{
				return false;
			}

			return TryGetHiggsRigidBodyPosition(
				static_cast<NiObject*>(higgsInterface->GetHandRigidBody(isLeftVRController)),
				out);
		}

		bool TryGetHiggsWeaponPosition(const bool isLeftVRController, NiPoint3& out)
		{
			if (!higgsInterface)
			{
				return false;
			}

			return TryGetHiggsRigidBodyPosition(
				static_cast<NiObject*>(higgsInterface->GetWeaponRigidBody(isLeftVRController)),
				out);
		}

		bool TryGetOppositeHandPosition(
			PlayerCharacter* player,
			const bool isLeftEquippedHand,
			const bool oppositeVRController,
			NiPoint3& out)
		{
			if (TryGetVRControllerWorldPosition(oppositeVRController, out))
			{
				return true;
			}

			out = GetEquippedHandWorldPosition(player, !isLeftEquippedHand);
			if (TryGetHiggsHandPosition(oppositeVRController, out))
			{
				return true;
			}

			return player != nullptr;
		}

		bool IsOppositeHandNearEquippedWeapon(
			PlayerCharacter* player,
			const bool isLeftEquippedHand,
			const bool oppositeVRController,
			const bool equippedVRController)
		{
			NiPoint3 oppositeHandPos = GetEquippedHandWorldPosition(player, !isLeftEquippedHand);
			NiPoint3 weaponPos = GetEquippedHandWorldPosition(player, isLeftEquippedHand);

			NiPoint3 higgsHandPos;
			if (TryGetHiggsHandPosition(oppositeVRController, higgsHandPos))
			{
				oppositeHandPos = higgsHandPos;
			}

			NiPoint3 higgsWeaponPos;
			if (TryGetHiggsWeaponPosition(equippedVRController, higgsWeaponPos))
			{
				weaponPos = higgsWeaponPos;
			}

			return DistanceBetween(oppositeHandPos, weaponPos) <= kOppositeGrabMaxDistance;
		}

		bool IsOppositeHandEngagingEquippedWeapon(
			PlayerCharacter* player,
			const bool isLeftEquippedHand,
			TESForm* equipped,
			TESObjectREFR*& outGrabbedRefr)
		{
			outGrabbedRefr = nullptr;

			if (!player || !equipped || !higgsInterface)
			{
				return false;
			}

			const bool equippedVRController = GameHandToVRController(isLeftEquippedHand);
			const bool oppositeVRController = !equippedVRController;

			if (higgsInterface->IsTwoHanding() &&
				IsOppositeHandNearEquippedWeapon(
					player, isLeftEquippedHand, oppositeVRController, equippedVRController))
			{
				return true;
			}

			TESObjectREFR* grabbedRefr = higgsInterface->GetGrabbedObject(oppositeVRController);
			outGrabbedRefr = grabbedRefr;

			if (grabbedRefr && grabbedRefr->baseForm && grabbedRefr->baseForm->formID == equipped->formID)
			{
				return true;
			}

			const bool oppositeHolding =
				higgsInterface->IsHoldingObject(oppositeVRController) ||
				higgsInterface->GetGrabbedRigidBody(oppositeVRController) != nullptr;

			if (oppositeHolding &&
				IsOppositeHandNearEquippedWeapon(
					player, isLeftEquippedHand, oppositeVRController, equippedVRController))
			{
				return true;
			}

			if (IsOppositeControllerGripActive(oppositeVRController) &&
				IsOppositeHandNearEquippedWeapon(
					player, isLeftEquippedHand, oppositeVRController, equippedVRController))
			{
				return true;
			}

			return false;
		}

		class LogOppositeHandGrabTask : public TaskDelegate
		{
		public:
			LogOppositeHandGrabTask(
				const bool isLeftGrabController,
				const bool isLeftEquippedHand,
				const UInt32 weaponFormID,
				const UInt32 grabbedRefID)
				: m_isLeftGrabController(isLeftGrabController)
				, m_isLeftEquippedHand(isLeftEquippedHand)
				, m_weaponFormID(weaponFormID)
				, m_grabbedRefID(grabbedRefID)
			{
			}

			virtual void Run() override
			{
				PlayerCharacter* player = *g_thePlayer;
				TESForm* weaponForm = LookupFormByID(m_weaponFormID);
				if (!player || !weaponForm || !IsTrackedWeaponForm(weaponForm))
				{
					return;
				}

				TESForm* stillEquipped = player->GetEquippedObject(m_isLeftEquippedHand);
				if (!stillEquipped || stillEquipped->formID != m_weaponFormID)
				{
					return;
				}

				auto* weapon = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
				if (!weapon)
				{
					return;
				}

				const char* typeLabel = GetTrackedWeaponTypeLabel(weapon->type());
				if (!typeLabel)
				{
					return;
				}

				LOG_INFO(
					"Opposite hand grab: %s controller grabbed weapon equipped on %s hand: %s (%s) formId=%08X refId=%08X",
					m_isLeftGrabController ? "left" : "right",
					m_isLeftEquippedHand ? "left" : "right",
					GetSafeFormName(weaponForm),
					typeLabel,
					m_weaponFormID,
					m_grabbedRefID);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftGrabController;
			bool m_isLeftEquippedHand;
			UInt32 m_weaponFormID;
			UInt32 m_grabbedRefID;
		};

		void QueueOppositeHandGrabLog(
			const bool isLeftGrabController,
			const bool isLeftEquippedHand,
			const UInt32 weaponFormID,
			const UInt32 grabbedRefID)
		{
			if (!g_task || weaponFormID == 0)
			{
				return;
			}

			g_task->AddTask(new LogOppositeHandGrabTask(
				isLeftGrabController,
				isLeftEquippedHand,
				weaponFormID,
				grabbedRefID));
		}

		class LogSwapPullTask : public TaskDelegate
		{
		public:
			LogSwapPullTask(
				const bool isLeftGrabController,
				const bool isLeftEquippedHand,
				const UInt32 weaponFormID,
				const float pullSpeed)
				: m_isLeftGrabController(isLeftGrabController)
				, m_isLeftEquippedHand(isLeftEquippedHand)
				, m_weaponFormID(weaponFormID)
				, m_pullSpeed(pullSpeed)
			{
			}

			virtual void Run() override
			{
				PlayerCharacter* player = *g_thePlayer;
				TESForm* weaponForm = LookupFormByID(m_weaponFormID);
				if (!player || !weaponForm || !IsTrackedWeaponForm(weaponForm))
				{
					return;
				}

				if (!enableSwapping || IsSwapPullExcludedForm(weaponForm))
				{
					return;
				}

				TESForm* equippedOnDest = player->GetEquippedObject(!m_isLeftEquippedHand);
				if (equippedOnDest && equippedOnDest->formID == m_weaponFormID)
				{
					return;
				}

				auto* weapon = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
				if (!weapon)
				{
					return;
				}

				const char* typeLabel = GetTrackedWeaponTypeLabel(weapon->type());
				if (!typeLabel)
				{
					return;
				}

				const char* pullDirection = m_isLeftEquippedHand ? "right" : "left";

				LOG_INFO(
					"Swap pull detected: %s controller pulled %s across body from %s hand: %s (%s) formId=%08X pullSpeed=%.1f",
					m_isLeftGrabController ? "left" : "right",
					pullDirection,
					m_isLeftEquippedHand ? "left" : "right",
					GetSafeFormName(weaponForm),
					typeLabel,
					m_weaponFormID,
					m_pullSpeed);

				ScheduleSwapPullToOppositeHand(m_isLeftEquippedHand, m_weaponFormID, m_pullSpeed);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			bool m_isLeftGrabController;
			bool m_isLeftEquippedHand;
			UInt32 m_weaponFormID;
			float m_pullSpeed;
		};

		void QueueSwapPullAction(
			const bool isLeftGrabController,
			const bool isLeftEquippedHand,
			const UInt32 weaponFormID,
			const float pullSpeed)
		{
			if (!g_task || weaponFormID == 0)
			{
				return;
			}

			g_task->AddTask(new LogSwapPullTask(
				isLeftGrabController,
				isLeftEquippedHand,
				weaponFormID,
				pullSpeed));
		}

		void ResetEquippedHandGrabTracking(EquippedHandGrabState& state, const bool preserveSwapDetected = false)
		{
			const bool swapDetected = state.wasSwapPullDetected;
			state.latchedOppositeGrab = false;
			state.wasSwapPullDetected = preserveSwapDetected ? swapDetected : false;
			state.hasPreviousOppositeHandPos = false;
			state.hasWindowOppositeHandPos = false;
			state.oppositeGrabReleaseTimer = 0.0f;
			state.oppositeHandWindowAge = 0.0f;
			state.latchStartHandDistance = 0.0f;
		}

		void BeginOppositeGrabSession(
			PlayerCharacter* player,
			const bool isLeftEquippedHand,
			const bool oppositeVRController,
			EquippedHandGrabState& state)
		{
			state.hasPreviousOppositeHandPos = false;
			state.hasWindowOppositeHandPos = false;
			state.oppositeHandWindowAge = 0.0f;
			state.wasSwapPullDetected = false;
			state.latchStartHandDistance = 0.0f;

			NiPoint3 oppositeHandPos{};
			if (!TryGetOppositeHandPosition(player, isLeftEquippedHand, oppositeVRController, oppositeHandPos))
			{
				return;
			}

			TryGetEquippedWeaponAnchorPosition(player, isLeftEquippedHand, state.latchEquippedAnchorPos);
			state.latchStartHandDistance = DistanceBetween(state.latchEquippedAnchorPos, oppositeHandPos);
		}

		void UpdateEquippedHandOppositeGrabDetection(
			const bool isLeftEquippedHand,
			EquippedHandGrabState& state,
			const float deltaTime)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				state.wasRawOppositeGrabActive = false;
				ResetEquippedHandGrabTracking(state);
				return;
			}

			TESForm* equipped = player->GetEquippedObject(isLeftEquippedHand);
			if (!equipped || !IsTrackedWeaponForm(equipped))
			{
				state.wasRawOppositeGrabActive = false;
				ResetEquippedHandGrabTracking(state);
				return;
			}

			const bool oppositeVRController = !GameHandToVRController(isLeftEquippedHand);

			TESObjectREFR* grabbedRefr = nullptr;
			const bool rawActive = IsOppositeHandEngagingEquippedWeapon(
				player, isLeftEquippedHand, equipped, grabbedRefr);

			if (rawActive)
			{
				state.oppositeGrabReleaseTimer = kOppositeGrabReleaseGraceSeconds;
				if (!state.latchedOppositeGrab)
				{
					QueueOppositeHandGrabLog(
						oppositeVRController,
						isLeftEquippedHand,
						equipped->formID,
						grabbedRefr ? grabbedRefr->formID : 0);
					BeginOppositeGrabSession(player, isLeftEquippedHand, oppositeVRController, state);
				}
				state.latchedOppositeGrab = true;
			}
			else if (state.latchedOppositeGrab)
			{
				state.oppositeGrabReleaseTimer -= deltaTime;
				if (state.oppositeGrabReleaseTimer <= 0.0f)
				{
					ResetEquippedHandGrabTracking(state);
				}
			}

			// No hand swapping when disabled in the ini, or for 2H weapons
			// (including 2H Weapons Unlocked proxy forms).
			if (state.latchedOppositeGrab && enableSwapping && !IsSwapPullExcludedForm(equipped))
			{
				NiPoint3 oppositeHandPos{};
				if (TryGetOppositeHandPosition(player, isLeftEquippedHand, oppositeVRController, oppositeHandPos))
				{
					if (state.hasPreviousOppositeHandPos && !state.wasSwapPullDetected)
					{
						float pullSpeed = 0.0f;
						const NiPoint3 windowPos = state.hasWindowOppositeHandPos
							? state.windowOppositeHandPos
							: state.previousOppositeHandPos;
						const float windowAge = state.hasWindowOppositeHandPos
							? state.oppositeHandWindowAge
							: deltaTime;

						NiPoint3 equippedAnchorPos = state.latchEquippedAnchorPos;
						TryGetEquippedWeaponAnchorPosition(player, isLeftEquippedHand, equippedAnchorPos);

						if (IsAbruptSwapPull(
							equippedAnchorPos,
							oppositeHandPos,
							state.previousOppositeHandPos,
							windowPos,
							deltaTime,
							windowAge,
							state.latchStartHandDistance,
							pullSpeed))
						{
							state.wasSwapPullDetected = true;
							ResetEquippedHandGrabTracking(state, true);
							QueueSwapPullAction(
								oppositeVRController,
								isLeftEquippedHand,
								equipped->formID,
								pullSpeed);
						}
					}

					state.previousOppositeHandPos = oppositeHandPos;
					state.hasPreviousOppositeHandPos = true;

					state.oppositeHandWindowAge += deltaTime;
					if (!state.hasWindowOppositeHandPos ||
						state.oppositeHandWindowAge >= kSwapPullSampleWindowSeconds)
					{
						state.windowOppositeHandPos = oppositeHandPos;
						state.oppositeHandWindowAge = 0.0f;
						state.hasWindowOppositeHandPos = true;
					}
				}
			}

			state.wasRawOppositeGrabActive = rawActive;
		}

		void OnSwapTrackingStep()
		{
			static auto lastTime = std::chrono::high_resolution_clock::now();
			const auto currentTime = std::chrono::high_resolution_clock::now();
			float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
			lastTime = currentTime;

			if (deltaTime > 0.1f)
			{
				deltaTime = 0.1f;
			}
			if (deltaTime < 0.0001f)
			{
				deltaTime = 0.0001f;
			}

			UpdateEquippedHandOppositeGrabDetection(true, s_leftEquippedGrabState, deltaTime);
			UpdateEquippedHandOppositeGrabDetection(false, s_rightEquippedGrabState, deltaTime);
		}

		void OnStartTwoHanding()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !higgsInterface)
			{
				return;
			}

			UpdateEquippedHandOppositeGrabDetection(true, s_leftEquippedGrabState, 0.016f);
			UpdateEquippedHandOppositeGrabDetection(false, s_rightEquippedGrabState, 0.016f);
		}

		void OnStopTwoHanding()
		{
			s_leftEquippedGrabState.wasRawOppositeGrabActive = false;
			s_rightEquippedGrabState.wasRawOppositeGrabActive = false;
			ResetEquippedHandGrabTracking(s_leftEquippedGrabState);
			ResetEquippedHandGrabTracking(s_rightEquippedGrabState);
		}

		void OnOppositeHandWeaponGrabbed(const bool isLeftGrabController, TESObjectREFR* grabbedRefr)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !grabbedRefr || !grabbedRefr->baseForm)
			{
				return;
			}

			bool isLeftEquippedHand = !VRControllerToGameHand(isLeftGrabController);
			TESForm* equipped = player->GetEquippedObject(isLeftEquippedHand);
			if ((!equipped || !IsTrackedWeaponForm(equipped)) && enableTwoHandedWeapons)
			{
				const bool otherHand = !isLeftEquippedHand;
				TESForm* otherEquipped = player->GetEquippedObject(otherHand);
				if (IsTwoHandedWeaponForm(otherEquipped))
				{
					equipped = otherEquipped;
					isLeftEquippedHand = otherHand;
				}
			}
			if (!equipped || !IsTrackedWeaponForm(equipped))
			{
				return;
			}

			if (grabbedRefr->baseForm->formID != equipped->formID)
			{
				return;
			}

			EquippedHandGrabState& state = isLeftEquippedHand
				? s_leftEquippedGrabState
				: s_rightEquippedGrabState;
			if (state.latchedOppositeGrab)
			{
				return;
			}

			state.latchedOppositeGrab = true;
			state.oppositeGrabReleaseTimer = kOppositeGrabReleaseGraceSeconds;
			BeginOppositeGrabSession(player, isLeftEquippedHand, isLeftGrabController, state);
			QueueOppositeHandGrabLog(
				isLeftGrabController,
				isLeftEquippedHand,
				equipped->formID,
				grabbedRefr->formID);
		}
	}

	void RegisterOppositeHandGrabLogger()
	{
		if (s_registered)
		{
			return;
		}

		if (!higgsInterface)
		{
			LOG_ERR("Opposite hand grab logging requires HIGGS.");
			return;
		}

		higgsInterface->AddGrabbedCallback(OnOppositeHandWeaponGrabbed);
		higgsInterface->AddPostVrikPostHiggsCallback(OnSwapTrackingStep);
		higgsInterface->AddStartTwoHandingCallback(OnStartTwoHanding);
		higgsInterface->AddStopTwoHandingCallback(OnStopTwoHanding);
		s_registered = true;
		LOG_INFO("Opposite hand grab logger registered (cross-hand grab and swap pull detection).");
	}
}
