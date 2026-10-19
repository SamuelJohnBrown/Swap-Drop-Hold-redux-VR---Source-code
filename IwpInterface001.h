#pragma once

// Immersive Weapon Penetration VR — mod-support interface, version 001.
// Classic-SDK mirror of IWP's own IwpInterface.h (CommonLib). The vtable order and
// the EmbedInfo memory layout are ABI: never reorder, only append.
//
// Layout equivalences (verified field-by-field against the CommonLib original):
//   RE::NiPointer<NiAVObject>  ->  NiPointer<NiAVObject>   (one refcounted pointer, 8)
//   RE::BSFixedString          ->  BSFixedString           (one cache pointer, 8)
//   RE::ActorHandle            ->  UInt32                  (BSPointerHandle payload)
//   RE::NiPoint3               ->  NiPoint3                (3 floats)
//
// IWP >= 1.7.2 BROADCASTS the interface at kPostLoad/kPostPostLoad (message
// 0x49575002 to all listeners) because a name-addressed Dispatch to
// "ImmersiveWeaponPenetrationVR" fails on SkyrimVR's SKSE. Listen with a
// RegisterListener(handle, nullptr, cb) registered during plugin load.

#include "skse64/GameReferences.h"
#include "skse64/NiObjects.h"
#include "skse64/NiTypes.h"

namespace IwpAPI
{
	static const UInt32 kMessage_GetInterface = 0x49575001; // 'IWP\1' request (unreliable on VR)
	static const UInt32 kMessage_Interface = 0x49575002;    // 'IWP\2' broadcast offer (the real path)

	struct IwpApiOffer
	{
		void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
	};

	// A blade currently lodged in an actor.
	struct EmbedInfo
	{
		NiPointer<NiAVObject> bone;  // 00 - victim bone the blade is in (has a ragdoll body)
		BSFixedString boneName;      // 08
		UInt32 victimHandle = 0;     // 10 - resolve with LookupREFRByHandle
		NiPoint3 woundPos;           // 14 - world position of the wound entry
		NiPoint3 axis;               // 20 - unit vector along the channel, INTO the body
		float bladeLen = 0.0f;       // 2C - grip->entry distance at the stab, game units
		float depthFrac = 0.0f;      // 30 - 0..1 buried fraction
		bool isLeft = false;         // 34
	};

	struct IIwpInterface001
	{
		virtual UInt32 GetBuildNumber() = 0;

		// Is this hand's weapon currently lodged in someone?
		virtual bool IsEmbedded(bool isLeft) = 0;

		// Wound details for a lodged hand. False (and out untouched) when free.
		virtual bool GetEmbedInfo(bool isLeft, EmbedInfo& out) = 0;

		// Begin an embed for the weapon equipped in this hand, as though just stabbed.
		virtual bool StartEmbed(bool isLeft, Actor* victim, NiAVObject* bone,
			const BSFixedString& boneName, const NiPoint3& woundPos,
			const NiPoint3& stabVelocity, float depthFrac) = 0;

		// Drop this hand's embed without the extraction haptic. No-op if not lodged.
		virtual void EndEmbed(bool isLeft) = 0;

		// Resume an embed with the wound geometry supplied in full. The weapon must be
		// DRAWN (exactly kDrawn) when this is called, or the embed is released next frame.
		virtual bool ResumeEmbed(bool isLeft, Actor* victim, NiAVObject* bone,
			const BSFixedString& boneName, const NiPoint3& woundPos,
			const NiPoint3& axis, float bladeLen, float depthFrac) = 0;
	};
}
