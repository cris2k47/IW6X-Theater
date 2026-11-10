#include <std_include.hpp>
#include "boot.hpp"
#include "component/theater_gamepad.hpp"

// XInput 1.3 proxy: the game imports XINPUT1_3.dll (ordinals 2, 3, 4). iw6x.exe resolves it with
// LoadLibraryA by bare name, so this DLL is picked up from the game folder. Every call is forwarded
// to the real system DLL.

namespace
{
	HMODULE real_xinput{};
	std::once_flag real_once;

	HMODULE get_real()
	{
		std::call_once(real_once, []()
		{
			wchar_t sys[MAX_PATH]{};
			GetSystemDirectoryW(sys, MAX_PATH);
			const std::wstring dir(sys);

			for (const auto* name : {L"\\xinput1_3.dll", L"\\xinput1_4.dll", L"\\xinput9_1_0.dll"})
			{
				real_xinput = LoadLibraryW((dir + name).data());
				if (real_xinput)
				{
					break;
				}
			}
		});

		return real_xinput;
	}

	template <typename T>
	T get_proc(const char* name)
	{
		auto* module = get_real();
		return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
	}

	constexpr DWORD ERROR_DEVICE_NOT_CONNECTED_ = 1167;

	DWORD real_get_state(const DWORD user_index, void* state)
	{
		static const auto fn = get_proc<DWORD(WINAPI*)(DWORD, void*)>("XInputGetState");
		return fn ? fn(user_index, state) : ERROR_DEVICE_NOT_CONNECTED_;
	}
}

namespace xinput_proxy
{
	std::uint32_t get_state(const std::uint32_t user_index, void* state)
	{
		return real_get_state(user_index, state);
	}
}

extern "C"
{
	DWORD WINAPI XInputGetState(const DWORD user_index, void* state)
	{
		boot::try_start();
		const auto result = real_get_state(user_index, state);
		if (result == ERROR_SUCCESS)
		{
			// hide the buttons/sticks the theater is using from the game
			theater_gamepad::filter_for_game(state);
		}

		return result;
	}

	DWORD WINAPI XInputSetState(const DWORD user_index, void* vibration)
	{
		static const auto fn = get_proc<DWORD(WINAPI*)(DWORD, void*)>("XInputSetState");
		return fn ? fn(user_index, vibration) : ERROR_DEVICE_NOT_CONNECTED_;
	}

	DWORD WINAPI XInputGetCapabilities(const DWORD user_index, const DWORD flags, void* capabilities)
	{
		boot::try_start();
		static const auto fn = get_proc<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetCapabilities");
		return fn ? fn(user_index, flags, capabilities) : ERROR_DEVICE_NOT_CONNECTED_;
	}

	void WINAPI XInputEnable(const BOOL enable)
	{
		static const auto fn = get_proc<void(WINAPI*)(BOOL)>("XInputEnable");
		if (fn) fn(enable);
	}

	DWORD WINAPI XInputGetDSoundAudioDeviceGuids(const DWORD user_index, GUID* render_guid, GUID* capture_guid)
	{
		static const auto fn = get_proc<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>("XInputGetDSoundAudioDeviceGuids");
		return fn ? fn(user_index, render_guid, capture_guid) : ERROR_DEVICE_NOT_CONNECTED_;
	}

	DWORD WINAPI XInputGetBatteryInformation(const DWORD user_index, const BYTE dev_type, void* battery_information)
	{
		static const auto fn = get_proc<DWORD(WINAPI*)(DWORD, BYTE, void*)>("XInputGetBatteryInformation");
		return fn ? fn(user_index, dev_type, battery_information) : ERROR_DEVICE_NOT_CONNECTED_;
	}

	DWORD WINAPI XInputGetKeystroke(const DWORD user_index, const DWORD reserved, void* keystroke)
	{
		static const auto fn = get_proc<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetKeystroke");
		return fn ? fn(user_index, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED_;
	}

	DWORD WINAPI XInputGetStateEx(const DWORD user_index, void* state)
	{
		boot::try_start();
		static const auto fn = get_proc<DWORD(WINAPI*)(DWORD, void*)>(MAKEINTRESOURCEA(100));
		const auto result = fn ? fn(user_index, state) : ERROR_DEVICE_NOT_CONNECTED_;
		if (result == ERROR_SUCCESS)
		{
			theater_gamepad::filter_for_game(state);
		}

		return result;
	}
}

BOOL APIENTRY DllMain(HMODULE module, const DWORD reason, LPVOID /*reserved*/)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		boot::on_dll_attach(module);
	}

	return TRUE;
}
