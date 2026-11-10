#include "std_include.hpp"
#include "loader/component_loader.hpp"

#include "demo_playback.hpp"
#include "demo_timescale.hpp"

#include "fps.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/string.hpp"

namespace demo_timescale
{
	namespace
	{
		utils::hook::detour Com_TimeScaleMsec_hook;
		utils::hook::detour LUI_Layout_hook;
		utils::hook::detour Com_GetTimescaleForSnd_hook;

		game::dvar_t* demo_timescale;

		std::int32_t latest_delta_time;

		// iw6-theater: fixed frame time for video rendering (see demo_timescale.hpp)
		std::atomic<double> fixed_fps{0.0};
		double fixed_accumulator{};

		std::int32_t get_fixed_delta_time()
		{
			const auto timescale = demo_timescale ? static_cast<double>(demo_timescale->current.value) : 1.0;
			if (timescale <= 0.0)
			{
				return 0; // paused
			}

			// 60 fps at 1x: 16.67 ms per frame -> 17, 17, 16, ... (the engine counts whole milliseconds)
			fixed_accumulator += timescale * 1000.0 / fixed_fps.load();
			const auto whole = static_cast<std::int32_t>(std::floor(fixed_accumulator));
			fixed_accumulator -= whole;
			return whole;
		}

		void generate_pattern(std::array<std::uint8_t, 1000>& pattern, std::size_t ones_count, std::size_t zeros_count)
		{
			assert(ones_count + zeros_count == pattern.size());

			for (std::size_t i = 0, zeros = 1, ones = 1; i < pattern.size(); ++i)
			{
				if (ones * zeros_count < zeros * ones_count)
				{
					++ones;
					pattern[i] = 1;
				}
				else
				{
					++zeros;
					pattern[i] = 0;
				}
			}

			assert(std::accumulate(pattern.begin(), pattern.end(), 0) > 0);
		}

		void calculate_pattern(std::array<std::uint8_t, 1000>& pattern, float fps, float timescale)
		{
			// Com_TimeScaleMsec is called once per frame, so the number of calls it takes to advance 1000 ms
			// can be calculated by using the following formula: fps / timescale

			// example: 500 fps and 0.01 timescale -> 500 / 0.01 = 50'000
			// a pattern needs to be generated where 1000 * 1ms and 49'000 * 0ms are interleaved, 
			// and fit in an array of size 1000

			const auto call_count_per_sec = static_cast<std::size_t>(std::clamp(fps / timescale, 1000.0f, 1'000'000.0f));
			const auto ones_count = static_cast<std::size_t>(pattern.size() / (call_count_per_sec / static_cast<float>(pattern.size())));
			const auto zeros_count = pattern.size() - ones_count;

			generate_pattern(pattern, ones_count, zeros_count);
		}

		std::int32_t get_delta_time_internal(std::int32_t msec)
		{
			if (fixed_fps.load() > 0.0 && demo_playback::playing())
			{
				return get_fixed_delta_time();
			}

			if (!demo_playback::playing() || !demo_timescale || demo_timescale->current.value == 1.0f)
			{
				return Com_TimeScaleMsec_hook.invoke<std::int32_t>(msec);
			}
			if (demo_timescale->current.value == 0.0f)
			{
				return 0; // pause game
			}

			// the code below generates a pattern of interleaved 0s and 1s based on calculated avg fps
			// a new pattern is generated every 1000 frames, or after timescale or maxfps changes
			// the pattern determines the speed at which the game advances

			const auto timescale = demo_timescale->current.value;
			const auto avg_fps = fps::get_avg_fps();
			const auto frame_time = timescale * 1000.0f / avg_fps;
			const auto* com_maxfps = game::Dvar_FindVar("com_maxfps");
			const auto maxfps = (com_maxfps) ? com_maxfps->current.integer : static_cast<std::int32_t>(avg_fps);

			static auto last_timescale = timescale;
			static auto last_maxfps = maxfps;
			static std::size_t pattern_index;
			static std::array<std::uint8_t, 1000> pattern;

			if (last_timescale != timescale || last_maxfps != maxfps)
			{
				last_timescale = timescale;
				last_maxfps = maxfps;

				// update pattern using the maxfps instead of avg fps for now
				calculate_pattern(pattern, static_cast<float>(maxfps), timescale);

				// update the pattern again in the near future when the average fps is more accurate
				pattern_index = 95 * pattern.size() / 100;
			}

			if (frame_time > 1.0f)
			{
				const auto i_frame_time = static_cast<std::int32_t>(frame_time);
				const auto ones_count = static_cast<std::size_t>(frame_time * 1000 - i_frame_time * 1000);
				if (ones_count <= 1)
				{
					return i_frame_time;
				}

				if (pattern_index % pattern.size() == 0)
				{
					const auto zeros_count = pattern.size() - ones_count;
					generate_pattern(pattern, ones_count, zeros_count);
				}

				return i_frame_time + pattern[pattern_index++ % pattern.size()];
			}
			else if (pattern_index % pattern.size() == 0)
			{
				calculate_pattern(pattern, avg_fps, timescale);
			}

			// advance (1ms) or pause (0ms) based on the pattern
			return pattern[pattern_index++ % pattern.size()];
		}

		std::int32_t get_delta_time(std::int32_t msec)
		{
			latest_delta_time = get_delta_time_internal(msec);
			return latest_delta_time;
		}

		void set_delta_time_ui_elements(const std::int32_t client_num, const char* root_name, std::int32_t delta_time, void* lua_vm)
		{
			// iw6-theater: menus (pause menu, options) must animate in real time; with the demo paused
			// (timescale 0) their buttons would otherwise never finish their intro animation and stay invisible
			const auto menu_open = (*game::keyCatchers & ~1) != 0;
			if (demo_playback::playing() && !menu_open)
			{
				delta_time = latest_delta_time;
			}

			LUI_Layout_hook.invoke<void>(client_num, root_name, delta_time, lua_vm);
		}

		float get_timescale_for_sound()
		{
			if (!demo_playback::playing() || !demo_timescale || demo_timescale->current.value == 1.0f)
			{
				return Com_GetTimescaleForSnd_hook.invoke<float>();
			}

			return demo_timescale->current.value;
		}
	}

	void set_fixed_fps(const double fps)
	{
		fixed_accumulator = 0.0;
		fixed_fps = fps > 0.0 ? fps : 0.0;
	}

	double get_fixed_fps()
	{
		return fixed_fps.load();
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (!game::environment::is_mp())
			{
				return;
			}

			// add timescale support for demo playback
			Com_TimeScaleMsec_hook.create(game::Com_TimeScaleMsec, get_delta_time);

			// add timescale support for lua ui elements during demo playback
			LUI_Layout_hook.create(game::LUI_Layout, set_delta_time_ui_elements);

			// add timescale support for sounds during demo playback
			Com_GetTimescaleForSnd_hook.create(game::Com_GetTimescaleForSnd, get_timescale_for_sound);

			// allow the user to control the timescale during demo playback
			demo_timescale = game::Dvar_RegisterFloat(
				"demotimescale", 1.0f, 0.0f, 1000.0f, game::DVAR_FLAG_NONE, "Set playback speed for demos");
		}
	};
}

REGISTER_COMPONENT(demo_timescale::component)
