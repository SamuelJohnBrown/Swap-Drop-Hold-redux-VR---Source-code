#pragma once

// When a dropped weapon's late model gets hurried, and when the wait for it ends. No
// engine types in here, so tools_model_watch_tests.cpp can drive it outside the game;
// DropModelWatch.cpp feeds it whether the dropped reference has its 3D yet.

namespace SwapDropAndHoldRedux
{
	namespace ModelWatch
	{
		struct Config
		{
			// A normal drop has its model three or four frames later - 27 to 39ms across 25
			// drops in two logs. Hurrying before that would hurry every drop, and blur the one
			// thing the log is for: which drops were actually late.
			float hurryAfterSeconds = 0.10f;

			// Stop waiting for a model that never came.
			float giveUpSeconds = 20.0f;
		};

		enum class Step
		{
			kNone,      // nothing to do this frame
			kHurry,     // the model is late: ask the loader for it now (once per drop)
			kAppeared,  // the model is here, and the watch is over
			kGaveUp     // still no model after giveUpSeconds, and the watch is over
		};

		class Watch
		{
		public:
			// A reference was just dropped and has no model yet.
			void Begin()
			{
				*this = Watch{};
				m_active = true;
			}

			bool Active() const { return m_active; }

			// One frame. hasModel: the dropped reference has its 3D now.
			Step Update(const bool hasModel, const float deltaTime, const Config& config)
			{
				if (!m_active)
				{
					return Step::kNone;
				}

				++m_frames;
				m_seconds += deltaTime;

				if (hasModel)
				{
					m_active = false;
					return Step::kAppeared;
				}

				if (m_seconds >= config.giveUpSeconds)
				{
					m_active = false;
					return Step::kGaveUp;
				}

				// Once per drop. Asking again cannot help: a job still waiting is already at
				// the front, and one the loader has started cannot be moved at all.
				if (!m_hurried && m_seconds >= config.hurryAfterSeconds)
				{
					m_hurried = true;
					m_hurrySeconds = m_seconds;
					return Step::kHurry;
				}

				return Step::kNone;
			}

			// Time and frames since Begin, as of the last Update.
			float Seconds() const { return m_seconds; }
			int Frames() const { return m_frames; }

			bool Hurried() const { return m_hurried; }
			float HurrySeconds() const { return m_hurrySeconds; }

		private:
			bool m_active = false;
			bool m_hurried = false;
			int m_frames = 0;
			float m_seconds = 0.0f;
			float m_hurrySeconds = 0.0f;
		};
	}
}
