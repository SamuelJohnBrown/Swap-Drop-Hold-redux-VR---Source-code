#pragma once

#include <cstdint>

// Which frames of a watched HIGGS hand are worth a log line, and when the watch ends. No
// engine or HIGGS types in here, so tools_hand_watch_tests.cpp can drive it outside the
// game; HiggsHandWatch.cpp fills a Snapshot from HIGGS every frame.

namespace SwapDropAndHoldRedux
{
	namespace HandWatch
	{
		// HIGGS's view of one hand, plus this mod's read of its buttons. A frame is logged when
		// any of these differs from the frame before.
		struct Snapshot
		{
			bool canGrab = false;       // CanGrabObject: could it grab right now
			bool grabbable = false;     // IsHandInGrabbableState: idle, nothing held or locked in
			bool holding = false;       // IsHoldingObject
			bool disabled = false;      // IsDisabled: switched off through the HIGGS API
			bool twoHanding = false;    // IsTwoHanding
			uint32_t heldRefID = 0;     // GetGrabbedObject's reference, 0 for none
			bool gripClosed = false;    // this mod's grip read
			bool triggerClosed = false; // this mod's trigger read

			bool operator==(const Snapshot& other) const
			{
				return canGrab == other.canGrab && grabbable == other.grabbable &&
					holding == other.holding && disabled == other.disabled &&
					twoHanding == other.twoHanding && heldRefID == other.heldRefID &&
					gripClosed == other.gripClosed && triggerClosed == other.triggerClosed;
			}

			bool operator!=(const Snapshot& other) const
			{
				return !(*this == other);
			}
		};

		struct Config
		{
			// How long a hand is watched after whatever opened the watch.
			float windowSeconds = 15.0f;

			// A hand flickering between two states every frame must not flood the log. Past
			// this many lines in one second the rest are counted and reported as a number.
			int maxLinesPerSecond = 10;
		};

		// What one frame should log.
		struct Output
		{
			bool started = false; // the state the hand was in when the watch opened
			bool changed = false; // the state it just changed to
			int suppressed = 0;   // changes left unlogged by the cap, reported once
			bool ended = false;   // the state it was in when the watch ran out
		};

		class Window
		{
		public:
			// Opens a watch, or pushes back the end of one already open.
			void Open(const Config& config)
			{
				if (!m_open)
				{
					*this = Window{};
					m_open = true;
				}
				m_secondsLeft = config.windowSeconds;
			}

			bool IsOpen() const { return m_open; }

			// Seconds since the watch opened.
			float Seconds() const { return m_seconds; }

			Output Update(const Snapshot& now, const float deltaTime, const Config& config)
			{
				Output out;
				if (!m_open)
				{
					return out;
				}

				m_seconds += deltaTime;
				m_secondsLeft -= deltaTime;

				// A new second of the cap: whatever the last one held back is reported, once.
				m_secondTimer += deltaTime;
				if (m_secondTimer >= 1.0f)
				{
					m_secondTimer = 0.0f;
					m_linesThisSecond = 0;
					out.suppressed = m_suppressed;
					m_suppressed = 0;
				}

				if (!m_haveLast)
				{
					m_haveLast = true;
					m_last = now;
					out.started = true;
					++m_linesThisSecond;
				}
				else if (now != m_last)
				{
					m_last = now;
					if (m_linesThisSecond < config.maxLinesPerSecond)
					{
						out.changed = true;
						++m_linesThisSecond;
					}
					else
					{
						++m_suppressed;
					}
				}

				if (m_secondsLeft <= 0.0f)
				{
					m_open = false;
					out.ended = true;
					out.suppressed += m_suppressed;
					m_suppressed = 0;
				}

				return out;
			}

		private:
			bool m_open = false;
			bool m_haveLast = false;
			Snapshot m_last;
			float m_seconds = 0.0f;
			float m_secondsLeft = 0.0f;
			float m_secondTimer = 0.0f;
			int m_linesThisSecond = 0;
			int m_suppressed = 0;
		};
	}
}
