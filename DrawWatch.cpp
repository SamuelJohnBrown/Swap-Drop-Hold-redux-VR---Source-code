#include "DrawWatch.h"

#include "DrawWatchRules.h"
#include "DropModelWatch.h"
#include "Helper.h"
#include "TrackedWeapons.h"
#include "WeaponDrawHandler.h"
#include "config.h"

#include <skse64/GameForms.h>
#include <skse64/GameReferences.h>
#include <skse64/NiNodes.h>

#include <cstdio>

namespace SwapDropAndHoldRedux
{
	namespace
	{
		// The draw state the engine settles at when a weapon is really out. 4 and 5 are
		// kWantToSheathe and kSheathing, and IsWeaponDrawn() is state >= 3 - so anything but a
		// strict 3 is a weapon on its way somewhere, not a weapon in your hand.
		const UInt32 kDrawnAndSettled = 3;

		// How often the timeline is sampled into the log while the weapon is still not out.
		// Every frame would bury the log; this is often enough to see the shape of the wait.
		const float kTraceEverySeconds = 0.25f;

		struct WatchedDraw
		{
			bool inUse = false;
			bool isLeftGameHand = false;
			UInt32 weaponFormID = 0;
			char name[64] = { 0 };
			float sinceTrace = 0.0f;
			bool everSheathed = false;   // was it ever seen NOT out? (a swallowed draw)
			bool ever3DMissing = false;  // was it ever out but with nothing in the hand?
			DrawWatch::Watch watch;
		};

		WatchedDraw s_draws[2];
		const DrawWatch::Config s_config{};
		bool s_reported = false;

		// The offset nodes a drawn weapon hangs from, per game hand. The crossbow, melee and
		// staff nodes are separate from the plain weapon node, so all of them are looked at
		// rather than guessing from the weapon's type.
		int WeaponNodeCount(PlayerCharacter* player, const bool isLeftGameHand, const char*& whichOut)
		{
			whichOut = "none";
			if (!player)
			{
				return -1;
			}

			// The skeleton has to be up before any of these mean anything.
			if (!player->GetNiRootNode(0) && !player->GetNiRootNode(1))
			{
				return -1;
			}

			struct Slot
			{
				PlayerCharacter::Node node;
				const char* name;
			};

			const Slot rightSlots[] = {
				{ PlayerCharacter::kNode_RightWeaponOffsetNode, "weapon" },
				{ PlayerCharacter::kNode_RightMeleeWeaponOffsetNode, "melee" },
				{ PlayerCharacter::kNode_RightCrossbowOffsetNode, "crossbow" },
				{ PlayerCharacter::kNode_RightStaffWeaponOffsetNode, "staff" },
			};
			const Slot leftSlots[] = {
				{ PlayerCharacter::kNode_LeftWeaponOffsetNode, "weapon" },
				{ PlayerCharacter::kNode_LeftMeleeWeaponOffsetNode, "melee" },
				{ PlayerCharacter::kNode_LeftCrossbowOffsetNode, "crossbow" },
				{ PlayerCharacter::kNode_LeftStaffWeaponOffsetNode, "staff" },
				{ PlayerCharacter::kNode_LeftShieldOffsetNode, "shield" },
			};

			const Slot* slots = isLeftGameHand ? leftSlots : rightSlots;
			const int count = isLeftGameHand
				? static_cast<int>(sizeof(leftSlots) / sizeof(leftSlots[0]))
				: static_cast<int>(sizeof(rightSlots) / sizeof(rightSlots[0]));

			int best = 0;
			for (int i = 0; i < count; ++i)
			{
				NiAVObject* offset = player->unk3F0[slots[i].node];
				if (!offset)
				{
					continue;
				}
				NiNode* asNode = offset->GetAsNiNode();
				if (!asNode)
				{
					continue;
				}

				int children = 0;
				for (UInt32 c = 0; c < asNode->m_children.m_arrayBufLen; ++c)
				{
					if (asNode->m_children.m_data[c])
					{
						++children;
					}
				}
				if (children > best)
				{
					best = children;
					whichOut = slots[i].name;
				}
			}
			return best;
		}

		// Still the weapon this watch was started for, still in that hand.
		bool StillEquipped(PlayerCharacter* player, const WatchedDraw& draw)
		{
			if (!player)
			{
				return false;
			}
			TESForm* equipped = player->GetEquippedObject(draw.isLeftGameHand);
			return equipped && equipped->formID == draw.weaponFormID;
		}

		void FormatJobs(const int jobs, char* out, const size_t size)
		{
			if (jobs < 0)
			{
				std::snprintf(out, size, "?");
			}
			else
			{
				std::snprintf(out, size, "%d", jobs);
			}
		}

		const char* HandName(const bool isLeftGameHand)
		{
			return isLeftGameHand ? "left" : "right";
		}
	}

	void DrawWatchReportArming()
	{
		if (s_reported)
		{
			return;
		}
		s_reported = true;

		LOG_INFO(
			"Draw watch: %s - a weapon you pick up is followed until it is really out of its sheath%s. "
			"It asks again after %.2fs, then every %.2fs, at most %d time(s), and stops looking after %.0fs.",
			grabDrawTrace ? "ON, and every pickup is logged" : "on",
			grabDrawVerify ? ", and the draw is asked for again while it is not" : " (measuring only - bGrabDrawVerify = 0)",
			s_config.retryAfterSeconds,
			s_config.retryEverySeconds,
			s_config.maxRetries,
			grabDrawVerifySeconds);
	}

	void DrawWatchBegin(const bool isLeftGameHand, TESForm* weaponForm)
	{
		if (!weaponForm)
		{
			return;
		}

		WatchedDraw& draw = s_draws[isLeftGameHand ? 1 : 0];
		draw = WatchedDraw{};
		draw.inUse = true;
		draw.isLeftGameHand = isLeftGameHand;
		draw.weaponFormID = weaponForm->formID;
		std::snprintf(draw.name, sizeof(draw.name), "%s", GetSafeFormName(weaponForm));
		draw.watch.Begin();
	}

	void DrawWatchTick(const float deltaTime)
	{
		PlayerCharacter* player = *g_thePlayer;

		for (int i = 0; i < 2; ++i)
		{
			WatchedDraw& draw = s_draws[i];
			if (!draw.inUse || !draw.watch.Active())
			{
				continue;
			}

			// Put away, swapped, dropped or the player is gone: nothing to wait for, and
			// nothing worth saying. A weapon deliberately sheathed must never be re-drawn.
			if (!StillEquipped(player, draw))
			{
				draw = WatchedDraw{};
				continue;
			}

			const UInt32 state = PlayerWeaponDrawState(player);
			const bool drawn = state == kDrawnAndSettled;
			if (!drawn)
			{
				draw.everSheathed = true;
			}

			const char* which = "none";
			const int nodes = WeaponNodeCount(player, draw.isLeftGameHand, which);
			if (drawn && nodes == 0)
			{
				draw.ever3DMissing = true;
			}

			const DrawWatch::Config config{
				s_config.retryAfterSeconds,
				s_config.retryEverySeconds,
				s_config.maxRetries,
				grabDrawVerifySeconds
			};
			const DrawWatch::Step step = draw.watch.Update(drawn, deltaTime, config);

			// The running timeline, while it is still not out.
			draw.sinceTrace += deltaTime;
			if (grabDrawTrace && !drawn && draw.sinceTrace >= kTraceEverySeconds)
			{
				draw.sinceTrace = 0.0f;
				char jobs[16];
				FormatJobs(DropModelWatchLoaderJobs(), jobs, sizeof(jobs));
				LOG_INFO(
					"Draw watch: %s is equipped in the %s hand but still not out %.2fs (%d frames) after the "
					"pickup - draw state %u (3 means out and settled), %d thing(s) on the %s node, "
					"loader jobs: %s.",
					draw.name, HandName(draw.isLeftGameHand),
					draw.watch.Seconds(), draw.watch.Frames(), state, nodes, which, jobs);
			}

			switch (step)
			{
			case DrawWatch::Step::kRetryDraw:
			{
				if (!grabDrawVerify)
				{
					LOG_INFO(
						"Draw watch: %s has been equipped but sheathed for %.2fs - NOT asking for the draw "
						"again (bGrabDrawVerify = 0).",
						draw.name, draw.watch.Seconds());
					break;
				}
				const bool asked = RequestWeaponDrawAgain();
				LOG_INFO(
					"Draw watch: %s was equipped but still sheathed %.2fs later - %s (attempt %d of %d).",
					draw.name,
					draw.watch.Seconds(),
					asked ? "asked the game to draw it again" : "could not ask (sneaking, swimming, or a two-handed proxy)",
					draw.watch.Retries(),
					s_config.maxRetries);
				break;
			}

			case DrawWatch::Step::kDrawn:
			{
				if (!draw.everSheathed)
				{
					// The overwhelmingly common case: it came out by itself. Only worth a line
					// while the trace is on.
					if (grabDrawTrace)
					{
						LOG_INFO(
							"Draw watch: %s was out of its sheath straight away (%.2fs, %d frame(s)), %d thing(s) "
							"on the %s node.",
							draw.name, draw.watch.Seconds(), draw.watch.Frames(), nodes, which);
					}
				}
				else
				{
					LOG_INFO(
						"Draw watch: %s came out after %.2fs (%d frames) and %d re-request(s)%s - %d thing(s) on "
						"the %s node.",
						draw.name,
						draw.watch.Seconds(),
						draw.watch.Frames(),
						draw.watch.Retries(),
						draw.ever3DMissing ? ", and it was drawn with nothing in the hand for a while" : "",
						nodes, which);
				}
				draw = WatchedDraw{};
				break;
			}

			case DrawWatch::Step::kGaveUp:
			{
				char jobs[16];
				FormatJobs(DropModelWatchLoaderJobs(), jobs, sizeof(jobs));
				LOG_ERR(
					"Draw watch: %s is STILL equipped but not out after %.2fs and %d re-request(s) - draw state "
					"%u, %d thing(s) on the %s node, loader jobs: %s. Sheathe and draw by hand to recover it.",
					draw.name,
					draw.watch.Seconds(),
					draw.watch.Retries(),
					state, nodes, which, jobs);
				draw = WatchedDraw{};
				break;
			}

			case DrawWatch::Step::kNone:
			default:
				break;
			}
		}
	}
}
