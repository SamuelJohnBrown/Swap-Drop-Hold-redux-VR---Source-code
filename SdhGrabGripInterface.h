#pragma once

// Swap Drop & Hold Redux -> grip drivers: "this weapon is about to be equipped, and this is where, and
// which way round, the player took hold of it."
//
// PUBLIC DOMAIN. Copy this file into your plugin unchanged; it includes only <cstdint> and compiles the
// same under the classic sksevr SDK and CommonLibSSE-NG. It ships BYTE-IDENTICAL in Swap Drop & Hold
// Redux and in TrueWield VR.
//
// HOW TO GET IT. Swap Drop & Hold Redux BROADCASTS an Offer (kMessage_Offer) at SKSE's PostPostLoad and
// again at DataLoaded. Register an all-sender listener during plugin load -
// RegisterListener(handle, nullptr, callback) - because a name-addressed Dispatch does not reliably find
// its receiver on SkyrimVR's SKSE. In the callback, when msg->type == kMessage_Offer and the data is at
// least sizeof(Offer), call offer->GetApiFunction(1) for an IGrabGrip001* and SetListener once.
//
// WHAT YOU GET. One GrabGripEvent per grab-to-equip, delivered on the game's frame thread from an SKSE
// task, just BEFORE the weapon is equipped - so before any weapon twin or skeleton rebuild exists. The
// callback must ONLY STORE the event and return what it intends to do with it; apply it later, when your
// own state for that hand and weapon is ready.
//
// THE COORDINATE. Everything is in the weapon collision shape's OWN frame, normalised to its LOCAL
// bounding box along longAxisLocal: -1 is the box's low end, +1 its high end. Metres are deliberately not
// sent: the equipped copy of a weapon is a different size from the loose one (HIGGS scales the hit shape
// by the player's hand size), so only a fraction survives the trip. hiltU is the shape's own origin on
// the same scale - where Skyrim weapon meshes keep the grip. Cross-check its sign against your own
// natural grip: if they disagree the two frames are mirrored, and alongU must be negated.
//
// NO POSITION (Swap Drop & Hold Redux 3.0.0 test 14). An event may carry kReversed WITHOUT kHasAlong: the
// player wants it reversed but held at the natural grip ("Hold where you grab" off). Keep the natural grip
// and only turn it over. A driver that predates this sees no kHasAlong and ignores the event: the weapon is
// held the natural way, which is safe.
//
// HAND SWITCHES. source kHandOff: the other hand took an equipped weapon over. alongU and kReversed describe
// where and how THAT hand held it, and weaponRefID is 0.
//
// ANYWHERE (3.0.0 test 15/16). kAnywhere with kHasAlong: the player wants the weapon held where it was taken,
// ABOVE the crossguard too - on the blade, or on a shaft above an axe or mace head. Whatever owns the grip keeps
// the hold on the side of the guard it was taken on: never ON the guard or head, and never slid THROUGH it.
// Without kAnywhere a grip driver keeps its normal window (the handle).

#include <cstdint>

namespace SdhGrabGripAPI
{
	constexpr std::uint32_t kMessage_Offer = 0x5D4A4812u;

	struct Offer
	{
		void* (*GetApiFunction)(unsigned int revision) = nullptr; // 1 -> IGrabGrip001*
	};

	enum Source : std::uint32_t
	{
		kLooseGrab = 1, // a loose weapon taken hold of with HIGGS and about to be equipped
		kHandOff = 2,   // an equipped weapon taken over by the other hand
	};

	enum Flags : std::uint32_t
	{
		kHasAlong = 1u << 0,      // alongU is a real measurement
		kHasHiltRef = 1u << 1,    // hiltU is far enough from the middle to say which end is the tip
		kReversed = 1u << 2,      // taken hold of backwards: hold it exactly half a turn from natural
		kHasFlipAxis = 1u << 3,   // flipAxisLocal was decided by how the weapon was really held
		kAxisAmbiguous = 1u << 4, // reserved: no clear long axis (such grabs are not sent today)
		kAnywhere = 1u << 5,      // hold it where it was taken, above the crossguard too (only with kHasAlong)
	};

	enum Answer : std::uint32_t
	{
		kIgnored = 0,
		kWillSlide = 1u << 0,
		kWillFlip = 1u << 1,
	};

	// 56 bytes: 4-byte fields only, no bool, no pointers. structSize is the provider's sizeof - read no
	// further than the smaller of that and your own.
	struct GrabGripEvent
	{
		std::uint32_t structSize = 0;
		std::uint32_t version = 0;       // 1
		std::uint32_t weaponFormID = 0;
		std::uint32_t weaponRefID = 0;   // the loose reference; 0 for a hand-off
		std::uint32_t isLeftGameHand = 0;
		std::uint32_t source = 0;        // Source
		std::uint32_t flags = 0;         // Flags
		std::uint32_t longAxisLocal = 0; // 0/1/2: dominant cardinal of the shape's LOCAL box
		std::uint32_t flipAxisLocal = 0; // 0/1/2: the perpendicular cardinal to turn over about
		float alongU = 0.0f;             // the palm along the long axis, -1 low end .. +1 high end
		float hiltU = 0.0f;              // the shape's own origin on the same scale
		float lengthM = 0.0f;            // the loose shape's length, Havok metres (informational)
		float reversedAngleDeg = 0.0f;   // blade vs the natural way round, 0..180 (diagnostic)
		float palmOffAxisM = 0.0f;       // palm distance from the weapon's centre line (diagnostic)
	};
	static_assert(sizeof(GrabGripEvent) == 56, "GrabGripEvent must stay 56 bytes");

	// Return Answer bits. ONLY STORE in here: no scene reads, no scene writes, no other mod's API.
	typedef std::uint32_t (*GrabGripCallback)(const GrabGripEvent& event);

	enum Capability : std::uint32_t
	{
		kCanSlide = 1u << 0,
		kCanFlip = 1u << 1,
	};

	struct IGrabGrip001
	{
		virtual std::uint32_t GetBuildNumber() = 0;  // Swap Drop & Hold Redux's numeric version
		virtual std::uint32_t GetApiRevision() = 0;  // 1
		// One listener; a second call replaces the first. capabilityMask: Capability bits you can honour
		// right now.
		virtual void SetListener(GrabGripCallback callback, std::uint32_t capabilityMask) = 0;
		// Capabilities can arrive late (a dependency arming after load): tell the provider when they change.
		virtual void SetCapabilities(std::uint32_t capabilityMask) = 0;
	};
}
