#pragma once

// Gamepad support for the theater. The add-on *is* the game's XInput DLL (XINPUT1_3.dll proxy), so it
// reads the controller directly and can hide the inputs it uses from the game while the theater owns them.

namespace theater_gamepad
{
	// XInput button bits
	constexpr std::uint16_t DPAD_UP = 0x0001;
	constexpr std::uint16_t DPAD_DOWN = 0x0002;
	constexpr std::uint16_t DPAD_LEFT = 0x0004;
	constexpr std::uint16_t DPAD_RIGHT = 0x0008;
	constexpr std::uint16_t START = 0x0010;
	constexpr std::uint16_t BACK = 0x0020;
	constexpr std::uint16_t LEFT_THUMB = 0x0040;
	constexpr std::uint16_t RIGHT_THUMB = 0x0080;
	constexpr std::uint16_t LEFT_SHOULDER = 0x0100;
	constexpr std::uint16_t RIGHT_SHOULDER = 0x0200;
	constexpr std::uint16_t GUIDE = 0x0400;
	constexpr std::uint16_t A = 0x1000;
	constexpr std::uint16_t B = 0x2000;
	constexpr std::uint16_t X = 0x4000;
	constexpr std::uint16_t Y = 0x8000;

	struct input
	{
		bool connected{};
		std::uint16_t buttons{};  // held
		std::uint16_t pressed{};  // went down this poll
		std::uint16_t released{}; // went up this poll

		// normalized, dead zone removed: sticks -1..1 (up/right positive), triggers 0..1
		float lx{}, ly{}, rx{}, ry{};
		float lt{}, rt{};

		[[nodiscard]] bool held(const std::uint16_t button) const { return (buttons & button) != 0; }
		[[nodiscard]] bool down(const std::uint16_t button) const { return (pressed & button) != 0; }
		[[nodiscard]] bool up(const std::uint16_t button) const { return (released & button) != 0; }
		[[nodiscard]] bool any_activity() const
		{
			return pressed != 0 || std::abs(lx) > 0.0f || std::abs(ly) > 0.0f || std::abs(rx) > 0.0f ||
				std::abs(ry) > 0.0f || lt > 0.0f || rt > 0.0f;
		}
	};

	// reads the first connected controller (call once per frame)
	const input& poll();

	// while true, the inputs used by the theater are removed from what the game reads (START stays)
	void set_consume(bool consume);

	// called by the XInputGetState export with the state the game is about to receive
	void filter_for_game(void* xinput_state);

	// developer testing: a virtual controller state that replaces the real one for 'duration'
	void inject(std::uint16_t buttons, float lx, float ly, float rx, float ry, float lt, float rt, std::chrono::milliseconds duration);
}

namespace xinput_proxy
{
	// the real system XInputGetState (xinput1_3 / 1_4 / 9_1_0)
	std::uint32_t get_state(std::uint32_t user_index, void* state);
}
