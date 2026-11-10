#include <std_include.hpp>
#include "theater_gamepad.hpp"

namespace theater_gamepad
{
	namespace
	{
		// layout of XINPUT_STATE (xinput.h is not included because it declares the functions we export)
		struct xinput_gamepad
		{
			std::uint16_t buttons;
			std::uint8_t left_trigger;
			std::uint8_t right_trigger;
			std::int16_t thumb_lx;
			std::int16_t thumb_ly;
			std::int16_t thumb_rx;
			std::int16_t thumb_ry;
		};

		struct xinput_state
		{
			std::uint32_t packet_number;
			xinput_gamepad gamepad;
		};

		static_assert(sizeof(xinput_gamepad) == 12);
		static_assert(sizeof(xinput_state) == 16);

		constexpr float LEFT_DEADZONE = 7849.0f;
		constexpr float RIGHT_DEADZONE = 8689.0f;
		constexpr float TRIGGER_THRESHOLD = 30.0f;

		std::atomic_bool consume{false};
		input current{};
		std::uint16_t previous_buttons{};
		std::uint32_t active_user{0};

		float normalize_axis(const std::int16_t value, const float deadzone)
		{
			const auto v = static_cast<float>(value);
			const auto magnitude = std::abs(v);
			if (magnitude <= deadzone)
			{
				return 0.0f;
			}

			const auto scaled = (std::min(magnitude, 32767.0f) - deadzone) / (32767.0f - deadzone);
			return v < 0.0f ? -scaled : scaled;
		}

		float normalize_trigger(const std::uint8_t value)
		{
			const auto v = static_cast<float>(value);
			return v <= TRIGGER_THRESHOLD ? 0.0f : (v - TRIGGER_THRESHOLD) / (255.0f - TRIGGER_THRESHOLD);
		}

		std::mutex inject_mutex;
		std::optional<xinput_gamepad> injected;
		std::chrono::steady_clock::time_point injected_until{};

		std::int16_t to_axis(const float v)
		{
			return static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
		}
	}

	void inject(const std::uint16_t buttons, const float lx, const float ly, const float rx, const float ry,
	            const float lt, const float rt, const std::chrono::milliseconds duration)
	{
		std::lock_guard _(inject_mutex);
		injected = xinput_gamepad{
			buttons,
			static_cast<std::uint8_t>(std::clamp(lt, 0.0f, 1.0f) * 255.0f),
			static_cast<std::uint8_t>(std::clamp(rt, 0.0f, 1.0f) * 255.0f),
			to_axis(lx), to_axis(ly), to_axis(rx), to_axis(ry),
		};
		injected_until = std::chrono::steady_clock::now() + duration;
	}

	const input& poll()
	{
		xinput_state state{};
		auto found = xinput_proxy::get_state(active_user, &state) == 0;

		{
			std::lock_guard _(inject_mutex);
			const auto now = std::chrono::steady_clock::now();
			if (injected && now < injected_until)
			{
				state.gamepad = *injected;
				found = true;
			}
			else if (!found && injected_until.time_since_epoch().count() && now < injected_until + 500ms)
			{
				// keep the virtual controller "connected" and idle for a moment so button releases register
				state.gamepad = {};
				found = true;
				injected.reset();
			}
			else
			{
				injected.reset();
			}
		}

		// look for another controller when the remembered one is gone
		for (std::uint32_t user = 0; !found && user < 4; ++user)
		{
			if (xinput_proxy::get_state(user, &state) == 0)
			{
				active_user = user;
				found = true;
			}
		}

		if (!found)
		{
			current = {};
			previous_buttons = 0;
			return current;
		}

		const auto& pad = state.gamepad;
		current.connected = true;
		current.buttons = pad.buttons;
		current.pressed = static_cast<std::uint16_t>(pad.buttons & ~previous_buttons);
		current.released = static_cast<std::uint16_t>(~pad.buttons & previous_buttons);
		current.lx = normalize_axis(pad.thumb_lx, LEFT_DEADZONE);
		current.ly = normalize_axis(pad.thumb_ly, LEFT_DEADZONE);
		current.rx = normalize_axis(pad.thumb_rx, RIGHT_DEADZONE);
		current.ry = normalize_axis(pad.thumb_ry, RIGHT_DEADZONE);
		current.lt = normalize_trigger(pad.left_trigger);
		current.rt = normalize_trigger(pad.right_trigger);
		previous_buttons = pad.buttons;
		return current;
	}

	void set_consume(const bool value)
	{
		consume = value;
	}

	void filter_for_game(void* state_ptr)
	{
		if (!consume || !state_ptr)
		{
			return;
		}

		auto& pad = static_cast<xinput_state*>(state_ptr)->gamepad;
		pad.buttons &= (START | BACK | GUIDE); // like BO2: Start opens the menu, Back the scoreboard
		pad.left_trigger = 0;
		pad.right_trigger = 0;
		pad.thumb_lx = 0;
		pad.thumb_ly = 0;
		pad.thumb_rx = 0;
		pad.thumb_ry = 0;
	}
}
