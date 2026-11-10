#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "fps.hpp"
#include "scheduler.hpp"

#include "demo_playback.hpp"
#include "game/game.hpp"

// Average frame rate measurement (replaces iw6-mod's cg_perf based get_avg_fps) and the
// max-fps workaround the demo timescale code relies on.

namespace fps
{
	namespace
	{
		constexpr std::size_t SAMPLE_COUNT = 32;

		std::array<float, SAMPLE_COUNT> frame_ms{};
		std::size_t frame_index{};
		float average_ms{16.0f};
		LARGE_INTEGER last_counter{};
		LARGE_INTEGER frequency{};

		void sample_frame()
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);

			if (last_counter.QuadPart)
			{
				const auto ms = static_cast<float>(now.QuadPart - last_counter.QuadPart) * 1000.0f / static_cast<float>(frequency.QuadPart);
				frame_ms[frame_index++ % SAMPLE_COUNT] = ms;

				const auto count = std::min(frame_index, SAMPLE_COUNT);
				average_ms = std::accumulate(frame_ms.begin(), frame_ms.begin() + count, 0.0f) / static_cast<float>(count);
			}

			last_counter = now;
		}

		void enforce_fps_limit()
		{
			if (!demo_playback::playing())
			{
				return;
			}

			const auto* maxfps = game::Dvar_FindVar("com_maxfps");
			const auto max = (maxfps) ? std::min(2 * maxfps->current.integer, maxfps->domain.integer.max + 250) : 1250;
			const auto fps = static_cast<std::int32_t>(1000.0f / std::max(average_ms, 0.01f));

			if (max > 0 && fps > max)
			{
				// workaround to limit fps (from the original demo code)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
	}

	float get_avg_fps()
	{
		return 1000.0f / (average_ms + std::numeric_limits<float>::epsilon());
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			QueryPerformanceFrequency(&frequency);

			scheduler::loop([]()
			{
				sample_frame();
				enforce_fps_limit();
			}, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(fps::component)
