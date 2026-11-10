#pragma once

namespace utils::thread
{
	// Names the calling thread for debuggers and crash dumps. SetThreadDescription only exists on
	// Windows 10 1607 and later (kernel32 exports it from 1703), so it is looked up at run time:
	// importing it would keep XINPUT1_3.dll, and with it the game, from loading on older Windows.
	inline void set_name(const wchar_t* name)
	{
		using set_thread_description_t = HRESULT(WINAPI*)(HANDLE, PCWSTR);
		static const auto set_thread_description = []() -> set_thread_description_t
		{
			for (const auto* dll : {L"kernel32.dll", L"kernelbase.dll"})
			{
				if (const auto handle = GetModuleHandleW(dll))
				{
					if (const auto proc = GetProcAddress(handle, "SetThreadDescription"))
					{
						return reinterpret_cast<set_thread_description_t>(proc);
					}
				}
			}
			return nullptr;
		}();

		if (set_thread_description)
		{
			set_thread_description(GetCurrentThread(), name);
		}
	}
}
