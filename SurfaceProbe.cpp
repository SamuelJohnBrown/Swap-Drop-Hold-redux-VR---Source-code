#include "SurfaceProbe.h"

#include "CellLoadGuard.h"

#include <skse64/GameReferences.h>
#include <skse64/NiNodes.h>
#include <skse64/NiTypes.h>
#include <skse64_common/Relocation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		// Skyrim units to Havok metres: SkyrimVR's bhkWorld scale, 1/70.
		constexpr float kSkyrimToHavok = 0.0142875f;

		// RE::bhkPickData as CommonLibVR declares it - and as VR Climbing builds it on its own stack
		// before calling PickObject (read from its DLL: hitFraction 1.0 at +0x40, extraInfo and shapeKey
		// -1 at +0x44 and +0x48, shapeKeys[0] -1 at +0x50, the hit's collidable read back from +0x80).
		struct alignas(16) PickData
		{
			float from[4];                       // 00  hkVector4, Havok metres
			float to[4];                         // 10
			bool enableShapeCollectionFilter;    // 20
			std::uint8_t pad21[3];
			std::uint32_t filterInfo;            // 24  0, exactly as VR Climbing casts
			std::uint64_t pad28;
			float normal[4];                     // 30  the output begins here
			float hitFraction;                   // 40
			std::int32_t extraInfo;              // 44
			std::uint32_t shapeKey;              // 48
			std::int32_t pad4C;
			std::uint32_t shapeKeys[8];          // 50
			std::int32_t shapeKeyIndex;          // 70
			std::uint32_t pad74;
			std::uint64_t pad78;
			const std::uint8_t* rootCollidable;  // 80  what the ray hit, or null
			std::uint64_t pad88;
			float ray[4];                        // 90
			void* aabbCache;                     // A0
			void* closestRayHitCollector;        // A8
			void* allRayHitCollector;            // B0
			void* allRayHitTempCollector;        // B8
			bool pickFailed;                     // C0
			std::uint8_t padC1[15];
		};
		static_assert(offsetof(PickData, to) == 0x10, "bhkPickData layout");
		static_assert(offsetof(PickData, enableShapeCollectionFilter) == 0x20, "bhkPickData layout");
		static_assert(offsetof(PickData, filterInfo) == 0x24, "bhkPickData layout");
		static_assert(offsetof(PickData, normal) == 0x30, "bhkPickData layout");
		static_assert(offsetof(PickData, hitFraction) == 0x40, "bhkPickData layout");
		static_assert(offsetof(PickData, extraInfo) == 0x44, "bhkPickData layout");
		static_assert(offsetof(PickData, shapeKey) == 0x48, "bhkPickData layout");
		static_assert(offsetof(PickData, shapeKeys) == 0x50, "bhkPickData layout");
		static_assert(offsetof(PickData, shapeKeyIndex) == 0x70, "bhkPickData layout");
		static_assert(offsetof(PickData, rootCollidable) == 0x80, "bhkPickData layout");
		static_assert(offsetof(PickData, ray) == 0x90, "bhkPickData layout");
		static_assert(offsetof(PickData, aabbCache) == 0xA0, "bhkPickData layout");
		static_assert(offsetof(PickData, pickFailed) == 0xC0, "bhkPickData layout");
		static_assert(sizeof(PickData) == 0xD0, "bhkPickData layout");

		// bhkWorld::PickObject(bhkPickData&): vtable slot 0x33. VR Climbing calls [vtable + 0x198] with
		// the world in rcx and the pick data in rdx - a member call, which on x64 is this signature.
		constexpr std::size_t kPickObjectSlot = 0x33;
		using PickObject_t = bool (*)(void* world, PickData* pickData);

		// hkpCollidable: its broadphase handle's collision filter info, whose low seven bits are the
		// layer. VR Climbing reads exactly [collidable + 0x2C] & 0x7F.
		constexpr std::size_t kCollidableFilterInfo = 0x2C;

		std::atomic<void*> s_world{ nullptr };
		std::atomic<long long> s_worldNotedAt{ 0 };

		// A world pointer older than this is not used: physics steps run every frame, so a stale one
		// means they have stopped - a load or a pause - and the world may be about to go.
		constexpr long long kWorldFreshNanoseconds = 250LL * 1000LL * 1000LL;

		long long NowNanoseconds()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// Whether a pointer lies inside SkyrimVR.exe's own image, which is where a real bhkWorld's
		// vtable lives. A cheap refusal of a pointer that is not a world at all.
		bool InsideGameImage(const void* p)
		{
			const uintptr_t base = RelocationManager::s_baseAddr;
			if (base == 0 || !p)
			{
				return false;
			}
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const uintptr_t size = nt->OptionalHeader.SizeOfImage;
			const uintptr_t at = reinterpret_cast<uintptr_t>(p);
			return at >= base && at < base + size;
		}

		void CastOne(void* world, const PickObject_t pick, const NiPoint3& fromU, const NiPoint3& toU,
			const float reachUnits, GripBlink::ProbeRay& out)
		{
			out = GripBlink::ProbeRay{};

			PickData pd;
			std::memset(&pd, 0, sizeof(pd));
			pd.from[0] = fromU.x * kSkyrimToHavok;
			pd.from[1] = fromU.y * kSkyrimToHavok;
			pd.from[2] = fromU.z * kSkyrimToHavok;
			pd.to[0] = toU.x * kSkyrimToHavok;
			pd.to[1] = toU.y * kSkyrimToHavok;
			pd.to[2] = toU.z * kSkyrimToHavok;
			pd.hitFraction = 1.0f;
			pd.extraInfo = -1;
			pd.shapeKey = 0xFFFFFFFFu;
			pd.shapeKeys[0] = 0xFFFFFFFFu;

			pick(world, &pd);
			// VR Climbing, like CommonLib's HasHit, reads the hit from the collidable, not the return.
			if (!pd.rootCollidable)
			{
				return;
			}
			std::uint32_t filterInfo = 0;
			std::memcpy(&filterInfo, pd.rootCollidable + kCollidableFilterInfo, sizeof(filterInfo));
			out.hit = true;
			out.layer = static_cast<int>(filterInfo & 0x7Fu);
			out.distUnits = std::isfinite(pd.hitFraction) ? pd.hitFraction * reachUnits : reachUnits;
		}
	}

	void SurfaceProbeNoteWorld(void* bhkWorld)
	{
		s_world.store(bhkWorld, std::memory_order_relaxed);
		s_worldNotedAt.store(NowNanoseconds(), std::memory_order_relaxed);
	}

	bool SurfaceProbeHand(const bool isLeftVRController, const float reachUnits, GripBlink::ProbeRay (&rays)[6])
	{
		for (GripBlink::ProbeRay& r : rays)
		{
			r = GripBlink::ProbeRay{};
		}
		if (CellLoadGuardLocked() || !(reachUnits > 0.0f))
		{
			return false;
		}

		void* world = s_world.load(std::memory_order_relaxed);
		if (!world || NowNanoseconds() - s_worldNotedAt.load(std::memory_order_relaxed) > kWorldFreshNanoseconds)
		{
			return false;
		}
		void** vtable = *reinterpret_cast<void***>(world);
		if (!InsideGameImage(vtable))
		{
			return false;
		}
		const auto pick = reinterpret_cast<PickObject_t>(vtable[kPickObjectSlot]);
		if (!pick)
		{
			return false;
		}

		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			return false;
		}
		// VR Climbing's own hand point: the controller's wand node (VR node data +0xA0 left, +0x108 right).
		NiAVObject* wand = player->unk3F0[isLeftVRController ? PlayerCharacter::kNode_LeftWandNode
		                                                     : PlayerCharacter::kNode_RightWandNode];
		if (!wand)
		{
			return false;
		}
		const NiPoint3 from = wand->m_worldTransform.pos;
		if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z))
		{
			return false;
		}

		// VR Climbing's six directions, in the same order: +X -X +Y -Y +Z -Z, in world axes.
		static const float kDirections[6][3] = {
			{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
			{ 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
		};
		for (int i = 0; i < 6; ++i)
		{
			const NiPoint3 to(from.x + kDirections[i][0] * reachUnits,
				from.y + kDirections[i][1] * reachUnits,
				from.z + kDirections[i][2] * reachUnits);
			CastOne(world, pick, from, to, reachUnits, rays[i]);
		}
		return true;
	}
}
