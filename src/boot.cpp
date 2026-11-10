#include <std_include.hpp>
#include "boot.hpp"

#include "loader/component_loader.hpp"
#include "component/console.hpp"
#include "component/scheduler.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/thread.hpp"

// Start-up sequence of the add-on:
//  1. iw6x.exe resolves the imports of iw6mp64_ship.exe with LoadLibraryA("XINPUT1_3.dll"),
//     which loads this DLL from the game folder (DllMain -> on_dll_attach). The game code is still
//     Arxan-encrypted at that point, so nothing may be patched yet.
//  2. A watcher thread (and every XInput call) checks whether the game has been unpacked AND the host
//     client has finished its own post_unpack (its MinHook detour on R_EndFrame is present).
//  3. Then the frame hooks are installed and the components are initialized on the game's main thread.

namespace boot
{
	namespace
	{
		HMODULE self_module{};
		std::atomic_bool started{false};
		std::atomic_bool initialized{false};
		std::atomic_bool host_demo{false};
		std::mutex start_mutex;

		HMODULE host_module{};

		HMODULE module_from_address(const std::uintptr_t address)
		{
			HMODULE module{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        reinterpret_cast<LPCWSTR>(address), &module))
			{
				return nullptr;
			}

			return module;
		}

		// Returns the module (other than the game and this add-on) that a hooked function jumps into.
		// Note: iw6-mod patches the PEB so GetModuleHandle(nullptr) returns the GAME module, therefore the
		// host client (iw6x.exe) is identified through the detour destination instead.
		HMODULE get_hook_owner(const std::uintptr_t branch_site, const HMODULE game_module)
		{
			if (!utils::hook::is_readable(reinterpret_cast<void*>(branch_site), 5))
			{
				return nullptr;
			}

			const auto* code = reinterpret_cast<const std::uint8_t*>(branch_site);
			if (code[0] != 0xE9)
			{
				return nullptr;
			}

			auto target = reinterpret_cast<std::uintptr_t>(utils::hook::get_call_target(code));

			// MinHook x64 relay: FF 25 00 00 00 00 <absolute address>
			if (utils::hook::is_readable(reinterpret_cast<void*>(target), 14))
			{
				const auto* relay = reinterpret_cast<const std::uint8_t*>(target);
				if (relay[0] == 0xFF && relay[1] == 0x25 && *reinterpret_cast<const std::int32_t*>(relay + 2) == 0)
				{
					target = *reinterpret_cast<const std::uintptr_t*>(relay + 6);
				}
			}

			const auto owner = module_from_address(target);
			if (!owner || owner == game_module || owner == self_module)
			{
				return nullptr;
			}

			return owner;
		}

		// iw6-mod's system_check reads the DWORD at 0x140001337 before the game is unpacked, i.e. it
		// compares encrypted bytes. After unpacking the value changes, so read it from the file on disk.
		bool game_version_ok(HMODULE game_module)
		{
			static std::optional<bool> result;
			if (result)
			{
				return *result;
			}

			char path[MAX_PATH]{};
			GetModuleFileNameA(game_module, path, sizeof(path));

			const auto base = reinterpret_cast<std::uintptr_t>(game_module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto rva = static_cast<std::uint32_t>(game::VERSION_MARKER_ADDRESS - base);

			std::optional<std::uint32_t> file_offset;
			const auto* section = IMAGE_FIRST_SECTION(nt);
			for (auto i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
			{
				if (rva >= section->VirtualAddress && rva < section->VirtualAddress + section->SizeOfRawData)
				{
					file_offset = section->PointerToRawData + (rva - section->VirtualAddress);
					break;
				}
			}

			std::uint32_t value{};
			if (file_offset)
			{
				std::ifstream file(path, std::ios::binary);
				file.seekg(*file_offset);
				file.read(reinterpret_cast<char*>(&value), sizeof(value));
			}

			result = value == game::VERSION_MARKER_VALUE;
			console::log(std::format("game binary {} marker 0x{:08X} -> {}", path, value, *result ? "supported" : "UNSUPPORTED"));
			return *result;
		}

		bool game_ready_reason(const char*& reason)
		{
			// 1. multiplayer game binary mapped at its preferred base
			const auto game_module = GetModuleHandleA("iw6mp64_ship.exe");
			if (reinterpret_cast<std::uintptr_t>(game_module) != 0x140000000)
			{
				reason = "iw6mp64_ship.exe not mapped at 0x140000000";
				return false;
			}

			// 2. exact game version (3.15, the build pinned by CBServers)
			if (!game_version_ok(game_module))
			{
				reason = "unsupported game version";
				return false;
			}

			// 3. unpacked and host hooks placed (host scheduler detours R_EndFrame in its post_unpack)
			const auto owner = get_hook_owner(reinterpret_cast<std::uintptr_t>(game::R_EndFrame.get()), game_module);
			if (!owner)
			{
				reason = "waiting for the host client to finish unpacking/hooking (R_EndFrame)";
				return false;
			}

			host_module = owner;
			reason = "ready";
			return true;
		}

		bool game_ready()
		{
			thread_local const char* unused_reason{};
			return game_ready_reason(unused_reason);
		}

		void detect_host()
		{
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(host_module, path, MAX_PATH);
			auto name = std::filesystem::path(path).filename().wstring();
			std::ranges::transform(name, name.begin(), ::towlower);
			host_demo = name.find(L"anomaly") != std::wstring::npos;
			console::log(std::format("host executable: {}", std::filesystem::path(path).string()));
		}

		void initialize_components()
		{
			try
			{
				component_loader::post_unpack([](const std::string& component_name)
				{
					if (host_demo && component_name.find("demo_") != std::string::npos)
					{
						console::log("skipping " + component_name + " (host has its own demo system)");
						return false;
					}

					return true;
				});
			}
			catch (const std::exception& e)
			{
				console::log(std::string("component initialization failed: ") + e.what());
			}

			initialized = true;
			console::info("iw6-theater loaded\n");
		}

		void start()
		{
			std::lock_guard _(start_mutex);
			if (started)
			{
				return;
			}

			started = true;
			detect_host();
			console::log("game unpacked and host ready; installing frame hooks");

			try
			{
				scheduler::install();
				scheduler::once(initialize_components, scheduler::pipeline::main);
			}
			catch (const std::exception& e)
			{
				console::log(std::string("failed to install frame hooks: ") + e.what());
			}
		}
	}

	namespace
	{
		using PeekMessageA_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
		PeekMessageA_t peek_message_original{};

		// the game pumps window messages from its main loop every frame -> main-thread start trigger
		BOOL WINAPI peek_message_stub(const LPMSG msg, const HWND wnd, const UINT min, const UINT max, const UINT remove)
		{
			try_start();
			return peek_message_original(msg, wnd, min, max, remove);
		}
	}

	void on_dll_attach(const HMODULE module)
	{
		self_module = module;
		console::log("iw6-theater attached");

		MH_Initialize();
		if (MH_CreateHookApi(L"user32", "PeekMessageA", reinterpret_cast<void*>(peek_message_stub),
		                     reinterpret_cast<void**>(&peek_message_original)) == MH_OK)
		{
			MH_EnableHook(MH_ALL_HOOKS);
		}
		else
		{
			console::log("could not hook PeekMessageA; relying on the watcher thread");
		}

		std::thread([]()
		{
			utils::thread::set_name(L"theater boot");
			const char* last_reason = nullptr;
			for (auto i = 0; i < 20 * 60 * 10 && !started; ++i) // give up after ~10 minutes
			{
				const char* reason = nullptr;
				const auto ready = game_ready_reason(reason);
				if (reason != last_reason || i % 100 == 0)
				{
					console::log(std::format("boot check #{}: {}", i, reason ? reason : "?"));
					last_reason = reason;
				}

				if (ready)
				{
					// the main thread (PeekMessageA) normally performs the start; fall back after 5 seconds
					for (auto w = 0; w < 100 && !started; ++w)
					{
						std::this_thread::sleep_for(50ms);
					}

					if (!started)
					{
						console::log("main thread did not start the add-on; starting from the watcher thread");
						try_start();
					}
					break;
				}

				std::this_thread::sleep_for(50ms);
			}
		}).detach();
	}

	void try_start()
	{
		if (started)
		{
			return;
		}

		if (game_ready())
		{
			start();
		}
	}

	HMODULE get_module()
	{
		return self_module;
	}

	bool is_initialized()
	{
		return initialized;
	}

	bool host_has_demo_system()
	{
		return host_demo;
	}
}
