#pragma once

// Mod-support API for Swap Drop & Hold Redux.
// Copy this header (and swapdropandholdreduxinterface001.cpp fetcher pattern) into
// consuming mods. Matches the HIGGS / Biting Axes SKSE messaging pattern.

#include "skse64/PluginAPI.h"
#include <cstdint>

class TESForm;
class TESObjectREFR;

namespace SwapDropAndHoldReduxAPI
{
	// Must match Swap Drop & Hold Redux SKSEPlugin_Query::name exactly.
	inline constexpr const char* kInterfaceRecipient = "SwapDropAndHoldRedux";

	enum WeaponHandEventType : std::uint32_t
	{
		kWeaponGrabEquipped = 1,
		kTriggerHoldDropped = 2,
		kSwapPullComplete = 3,
		kWeaponGrabPickup = 4,
	};

	// Snapshot passed to all weapon-hand callbacks.
	struct WeaponHandEvent
	{
		WeaponHandEventType eventType = kWeaponGrabEquipped;
		bool isLeftVRController = false;
		bool isLeftGameHand = false;
		bool sourceIsLeftGameHand = false;
		std::uint32_t weaponFormID = 0;
		std::uint32_t weaponRefID = 0;
		float pullSpeed = 0.0f;
	};

	struct ISwapDropAndHoldReduxInterface001
	{
		virtual unsigned int GetBuildNumber() = 0;

		typedef void (*WeaponHandEventCallback)(const WeaponHandEvent& event);

		// Return true to skip SDHR's default EquipItem for this grab/swap. The callback runs after
		// pickup (weapon is in inventory) but before SDHR equips it, so consumers can arm hand intent
		// and equip through their own pipeline (e.g. Weapon Unlocked VR 2H proxy conversion).
		typedef bool (*WeaponGrabEquipInterceptCallback)(const WeaponHandEvent& event);

		// Return true to skip SDHR's default Activate (inventory pickup) for this grab. Runs before
		// the world ref is consumed, so the item can stay a physical HIGGS-held object.
		typedef bool (*WeaponGrabPickupInterceptCallback)(const WeaponHandEvent& event);

		virtual void AddWeaponGrabEquippedCallback(WeaponHandEventCallback callback) = 0;
		virtual void AddTriggerHoldDroppedCallback(WeaponHandEventCallback callback) = 0;
		virtual void AddSwapPullCompleteCallback(WeaponHandEventCallback callback) = 0;
		virtual void AddWeaponHandEventCallback(WeaponHandEventCallback callback) = 0;
		virtual void AddWeaponGrabEquipInterceptCallback(WeaponGrabEquipInterceptCallback callback) = 0;
		virtual void AddWeaponGrabPickupInterceptCallback(WeaponGrabPickupInterceptCallback callback) = 0;
	};

	ISwapDropAndHoldReduxInterface001* GetSwapDropAndHoldReduxInterface001(
		const PluginHandle& pluginHandle,
		SKSEMessagingInterface* messagingInterface);

	// Provider-side registration (call from kPostPostLoad or later).
	void RegisterSwapDropAndHoldReduxInterface(
		PluginHandle pluginHandle,
		SKSEMessagingInterface* messagingInterface);

	// Provider-side event dispatch (called from grab / drop / swap code paths).
	void NotifyWeaponGrabEquipped(const WeaponHandEvent& event);
	void NotifyTriggerHoldDropped(const WeaponHandEvent& event);
	void NotifySwapPullComplete(const WeaponHandEvent& event);

	// Provider-side: ask registered interceptors whether they handled the equip. If any returns true,
	// the grab/swap code path skips its default EquipItem / hand transfer.
	bool QueryWeaponGrabEquipIntercept(const WeaponHandEvent& event);

	// Provider-side: ask registered interceptors whether they handled pickup. If any returns true,
	// the grab code path skips Activate (inventory pickup) and does not schedule equip.
	bool QueryWeaponGrabPickupIntercept(const WeaponHandEvent& event);
}
