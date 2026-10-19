#pragma once

#include "skse64/GameReferences.h"

#include "HolsterRangeRules.h"

namespace SwapDropAndHoldRedux
{
	// VRIK's weapon holsters. VRIK exposes no holster API at all — no "is my hand
	// over a slot", no "holster this" — but it DOES expose every one of its ini keys
	// by name through getSettingDouble, and vrikslots.ini carries the transform of
	// all 14 slots plus the activation distance. So the geometry is readable even
	// though the intent is not, and we work out proximity ourselves.
	//
	// VRIK 0.8.7 (build 80700) added that API. When it is there, HolsterNearHand asks VRIK through
	// VrikHolsterApi.h, and the estimating, learning and calibration below stand down. They remain
	// for older VRIK.
	void HolsterRefreshSettings();

	// Samples where the palm sits relative to the hand bone, using a hand that holds
	// nothing. Call every frame; it stops on its own once it has enough.
	void HolsterCalibratePalms();

	// True when the hand is within activation range of an enabled slot that accepts
	// this hand. outSlot is 1-14 (see the table in VrikHolsters.cpp).
	// reach adjusts the catch radius for THIS call only: its rangeBonus lets the
	// stand-down make leaving a holster take more distance than entering one, and
	// vrikReachOnly drops the extra range so Simple mode matches VRIK's own reach
	// (see HolsterRangeRules.h).
	bool HolsterNearHand(
		bool isLeftVRController,
		int& outSlot,
		float& outDistance,
		const HolsterRange::Reach& reach = HolsterRange::Reach{});

	// The tighter test behind the holster buzz: within VRIK's own activation distance of a
	// slot whose centre was measured off real draws. Always false on VRIK 0.8.7 and newer,
	// where GripHold asks VRIK itself instead. See HolsterRange::ForBuzz.
	bool HolsterBuzzNearHand(bool isLeftVRController, int& outSlot, float& outDistance);

	// Whether this slot's centre has been measured off enough real VRIK interactions to be
	// worth the tight buzz (HolsterLearn::CentreIsTrusted). slot is 1-14; anything else is
	// false.
	bool HolsterSlotCentreIsTrusted(int slot);

	const char* HolsterSlotName(int slot);

	// Learning: VRIK is the only authority on where its slots really are, and every
	// successful VRIK interaction hands us the answer - a draw FROM a slot and a
	// press-holster INTO one both happen with the hand exactly at the true position.
	// These record that position, replace the ini-derived estimate for that slot,
	// and persist it, so release-holstering targets the spot VRIK itself uses.
	void HolsterLearnFromDraw(bool isLeftVRController);
	void HolsterCapturePressCandidate(bool isLeftVRController);
	void HolsterLearnFromCapture();

	// Where the hand is now, for holster purposes: HIGGS's palm body when it is live, the hand bone
	// otherwise. False when neither is available.
	bool HolsterHandWorldPosition(bool isLeftVRController, NiPoint3& outWorld);

	// VRIK's slotActivationDistance as read from its settings (8 if unread).
	float HolsterActivationDistance();

	// The nearest slot to this hand by VRIK's OWN ini positions, with no learned corrections, on each
	// of the two skeletons by name: GetNiRootNode(1) and GetNiRootNode(0). Slots are 1-14. Shared by the
	// holster A/B and the haptic census. False when the settings, the roots or the hand are missing.
	bool HolsterNearestRawSlotOnBothBodies(
		bool isLeftVRController,
		int& outFirstPersonSlot,
		float& outFirstPersonDistance,
		int& outThirdPersonSlot,
		float& outThirdPersonDistance);

	// Which skeleton the holster maths is actually running on.
	//
	// FindBone looks bones up under player->GetNiNode() and nothing has ever checked that this
	// is the body VRIK hangs its holsters on. The M2.2.2-test1 census found this body's anchor
	// bones OUTSIDE that root - which proves nothing on its own, because the census skips
	// everything under it by design, but it is worth one measurement. This reports every anchor
	// bone under all three roots the SDK offers, at one instant. DISCOVERY ONLY: writes nothing.
	void HolsterSkeletonProbe(const char* why);

	// Runs the probe above a few seconds after the body is up, then twice more. It needs only a
	// loaded skeleton - no weapon, no holster, no settings - so it must not be hung off a
	// gameplay event. M2.2.3-test1 hung it on a stand-aside and it never fired once.
	void HolsterSkeletonProbeTick(float deltaTime);

	// A census of what is in the scene beside the hand at the moment it reaches a holster -
	// which is the moment VRIK lights that slot's hover sphere. DISCOVERY ONLY: it writes no
	// slot position and never touches the learned file. It runs a handful of times per session
	// and reports what it finds, so the sphere nodes can be identified by name before anything
	// is ever pinned from them again. See the corruption note in HolsterScanSpheresTick.
	void HolsterCensusAtHand(bool isLeftVRController, const char* why);

	// VRIK's holster hover spheres are real rendered geometry, so when they are
	// switched on in its MCM they are nodes we can read - and a sphere sits at the
	// EXACT centre of its slot. Turn on 'Visualize All Holster Spheres (Debug)' and
	// every slot can be pinned at once, with no guessing and no reaching.
	void HolsterScanSpheresTick(float deltaTime);

	// Which position source the last HolsterNearHand used ("weapon-body" etc.).
	const char* HolsterLastPositionSource();

	// Logs the hand measured against every slot, in the frame of each candidate
	// anchor bone. This is calibration data: the slot whose distance collapses when
	// the player is genuinely over a holster identifies the right anchor.
	void HolsterTraceRelease(bool isLeftVRController);
}
