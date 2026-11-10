#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "theater_capture.hpp"

#include "console.hpp"
#include "scheduler.hpp"

#include "boot.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"

#include <wrl/client.h>

// The IDXGISwapChain::Present hook shared by theater_screenshot.cpp and theater_video.cpp (see the header).
//
// Engine evidence (iw6mp64_ship.exe 3.15, Ghidra on the decrypted dump):
//  - FUN_1405e4e70 (device creation): D3D11CreateDevice(adapter 0x1480AA0B8, ..., flags 0x80, ...,
//    &0x1480AA098 device, &0x1480AA0C8 feature level, &0x1480AA0A0 immediate context); flags 0x80 =
//    D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY, no SINGLETHREADED: the device is
//    free-threaded, so resources and views may be created from inside Present. Then
//    IDXGIFactory(0x1480AA0B0)->CreateSwapChain [vtbl+0x50](device, &desc, &0x1480B1D70) with
//    desc = { BufferDesc.Width/Height = *(u64*)(param + 0x2C), Format 0x1C (R8G8B8A8_UNORM), SampleDesc {1, 0},
//    BufferUsage 0x20, BufferCount 2, Windowed 1, SwapEffect DISCARD, Flags 2 };
//    MakeWindowAssociation [vtbl+0x40](0x1480B1D68, 1); CreateMutexA -> 0x1480ADBF8 (device mutex).
//  - FUN_1405e6a50: after the device is created, window table entry [count]: +0x00 HWND = *param,
//    +0x18 width = *(u32*)(param + 0x2C), +0x1C height = *(u32*)(param + 0x30) (0x1480B1D80/84, the same
//    values as the swap chain BufferDesc above), then count (0x1480B1D64)++. The table has stride 0x20 and one
//    entry (0x1480B1D88 is a different global) -> entry 0: HWND 0x1480B1D68, swap chain 0x1480B1D70.
//  - FUN_1405e7970 (lost device reset): CreateSwapChain(..., &0x1480B1D70) again with BufferDesc.Width/Height =
//    0x1480B1D80/84, "Couldn't reset a lost Direct3D device - IDXGIFactory::CreateSwapChain returned 0x%08x (%s)".
//    FUN_1405e7f00 (renderer shutdown, vid_restart) destroys the windows and sets the count back to 0.
//  - Every reference to the swap chain global 0x1480B1D70 (FUN_140166690 GetDesc [+0x60], FUN_140605060
//    GetBuffer [+0x48], FUN_14062b960 / FUN_140504a00 GetFullscreenState [+0x58] + Present [+0x40],
//    FUN_1405e76c0 SetFullscreenState [+0x50] + Release, FUN_1405e4e70 / FUN_1405e7970 CreateSwapChain) - no
//    ResizeBuffers [+0x68]: the buffer size only changes together with the swap chain object.
//  - FUN_14062b960 (present, called at the end of the backend frame by FUN_14062b390 / FUN_14062a640 on the
//    render thread): WaitForSingleObject(0x1480ADBF8); swap chain = *(0x1480B1D70 + 0x20 * window index at
//    0x1480B1D5C); ->GetFullscreenState; if it matches -> Present [vtbl+0x40 = IDXGISwapChain slot 8]
//    (sync 0..4, flags 0); otherwise PostMessageA(hwnd, 0x400, window index, sync interval) instead. On
//    failure device->GetDeviceRemovedReason [vtbl+0x138], "Direct3DDevice::Present failed: %s"; ReleaseMutex.
//  - FUN_140504a00 (MainWndProc, main thread), message 0x400: FUN_14014dfb0 (WaitForSingleObject(0x1480ADBF8),
//    owner thread / recursion count at 0x1480AA0AC / 0x1480AA0A8), GetFullscreenState, SetFullscreenState,
//    ShowWindow, Present [+0x40] of the posted frame, FUN_14014e0c0 (ReleaseMutex). So a Present can run on the
//    main thread, but every Present holds the device mutex: the callbacks are serialized with each other and
//    with the engine's use of the immediate context.
//  - FUN_140605060: 0x1480B1D70->GetBuffer [vtbl+0x48](0, IID 0x1408C7768, ...) and
//    device->CreateRenderTargetView [vtbl+0x48]; the 16 bytes at 0x1408C7768 are
//    f2 aa 15 6f 08 d2 89 4e 9a b4 48 95 35 d3 4f 9c = IID_ID3D11Texture2D.
//  - game::g_wv (0x147AD2630, WinVars_t::hWnd at +0x10) comes from iw6-mod's symbols.hpp.
//  - Loading screens: FUN_14062b450 (render thread loop) calls FUN_1402cf570 -> FUN_1402cf440 -> R_EndFrame
//    while 0x1480E8914 (remote screen update nesting) is set, i.e. the renderer pipeline then runs on the
//    render thread (flush_messages() only prints on the main thread).

namespace theater_capture
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		// ------------------------------------------------------------------ engine data (see the evidence above)

		constexpr std::uintptr_t R_WINDOW_COUNT = 0x1480B1D64;     // int, number of window table entries
		constexpr std::uintptr_t R_WINDOW_HWND = 0x1480B1D68;      // window table [0] +0x00 HWND
		constexpr std::uintptr_t R_WINDOW_SWAPCHAIN = 0x1480B1D70; // window table [0] +0x08 IDXGISwapChain*
		constexpr std::uintptr_t R_WINDOW_WIDTH = 0x1480B1D80;     // window table [0] +0x18 buffer width
		constexpr std::uintptr_t R_WINDOW_HEIGHT = 0x1480B1D84;    // window table [0] +0x1C buffer height

		constexpr std::size_t PRESENT_VTABLE_INDEX = 8; // IUnknown 0-2, IDXGIObject 3-6, GetDevice 7, Present 8

		// ------------------------------------------------------------------ state

		utils::hook::detour present_hook;
		std::atomic_bool hook_installed{false};
		std::atomic_bool hook_failed_flag{false};

		std::array<callbacks, static_cast<std::size_t>(client::count)> clients{};
		std::atomic<unsigned> active_clients{0}; // bit per client

		// hold_frames / release_frames: < 0 = the game's frames are not presented, n > 0 = n more are not, 0 = normal
		std::atomic<int> frames_held{0};

		// the game's main thread (post_unpack runs there): console output is only written from it
		std::atomic<DWORD> main_thread_id{0};

		struct message
		{
			int type{};
			std::string text{};
		};

		std::mutex message_mutex;
		std::vector<message> messages;

		void flush_messages()
		{
			// the renderer pipeline also runs on the render thread during loading screens: keep the messages
			if (!on_main_thread())
			{
				return;
			}

			std::vector<message> pending;
			{
				std::lock_guard _(message_mutex);
				pending.swap(messages);
			}

			for (const auto& m : pending)
			{
				console::print(m.type, "%s\n", m.text.data());
			}
		}

		// ------------------------------------------------------------------ pixel formats

		constexpr format_info FORMATS[]
		{
			{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, pixel_layout::rgba8, 4},
			{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, pixel_layout::rgba8, 4},
			{DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, pixel_layout::rgba8, 4},
			{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, pixel_layout::bgra8, 4},
			{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, pixel_layout::bgra8, 4},
			{DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, pixel_layout::bgra8, 4},
			{DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM, pixel_layout::bgra8, 4},
			{DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, pixel_layout::bgra8, 4},
			{DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, pixel_layout::bgra8, 4},
			{DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, pixel_layout::rgb10a2, 4},
			{DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, pixel_layout::rgb10a2, 4},
			{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, pixel_layout::rgba16f, 8},
		};

		float half_to_float(const std::uint16_t h)
		{
			const auto sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
			auto exponent = static_cast<std::uint32_t>(h >> 10) & 0x1Fu;
			auto mantissa = static_cast<std::uint32_t>(h) & 0x3FFu;

			std::uint32_t bits{};
			if (exponent == 0)
			{
				if (mantissa == 0)
				{
					bits = sign;
				}
				else
				{
					// subnormal half -> normal float
					exponent = 127 - 15 + 1;
					while (!(mantissa & 0x400u))
					{
						mantissa <<= 1;
						--exponent;
					}

					bits = sign | (exponent << 23) | ((mantissa & 0x3FFu) << 13);
				}
			}
			else if (exponent == 31)
			{
				bits = sign | 0x7F800000u | (mantissa << 13);
			}
			else
			{
				bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
			}

			return std::bit_cast<float>(bits);
		}

		std::uint8_t linear_to_srgb8(const float v)
		{
			if (!(v > 0.0f)) // also NaN
			{
				return 0;
			}

			if (v >= 1.0f)
			{
				return 255;
			}

			const auto s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
			return static_cast<std::uint8_t>(std::clamp(std::lround(s * 255.0f), 0l, 255l));
		}

		// ------------------------------------------------------------------ game swap chain identification

		bool is_game_swap_chain(IDXGISwapChain* swap_chain)
		{
			if (const auto window = read_engine_window(); window.swap_chain == swap_chain)
			{
				return true;
			}

			// e.g. an overlay that wraps the swap chain: accept the one presenting into the game window
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(swap_chain->GetDesc(&desc)))
			{
				return false;
			}

			const auto hwnd = read_main_window();
			return !hwnd || desc.OutputWindow == hwnd;
		}

		// ------------------------------------------------------------------ Present hook

		void call_before(const unsigned mask, IDXGISwapChain* swap_chain)
		{
			for (unsigned i = 0; i < clients.size(); ++i)
			{
				if ((mask & (1u << i)) && clients[i].before_present)
				{
					try
					{
						clients[i].before_present(swap_chain);
					}
					catch (...)
					{
						post(console::con_type_warning, "theater_capture: exception in a Present callback");
					}
				}
			}
		}

		void call_after(const unsigned mask, IDXGISwapChain* swap_chain, const HRESULT result)
		{
			for (unsigned i = 0; i < clients.size(); ++i)
			{
				if (!(mask & (1u << i)))
				{
					continue;
				}

				if (clients[i].after_present)
				{
					try
					{
						clients[i].after_present(swap_chain, result);
					}
					catch (...)
					{
						post(console::con_type_warning, "theater_capture: exception in a Present callback");
					}
				}

				// automatic deactivation: no more callbacks (and no cost) once the client is done
				if (clients[i].is_active && !clients[i].is_active())
				{
					update(static_cast<client>(i));
				}
			}
		}

		HRESULT present_stub(IDXGISwapChain* swap_chain, const UINT sync_interval, const UINT flags)
		{
			// frames held (a backward seek restarts the demo): the screen keeps showing the last frame presented. The
			// callbacks do not run either: a screenshot waits for the next presented frame, a video skips these.
			if (auto held = frames_held.load(std::memory_order_acquire);
				held != 0 && swap_chain && !(flags & DXGI_PRESENT_TEST) && is_game_swap_chain(swap_chain))
			{
				while (held > 0 && !frames_held.compare_exchange_weak(held, held - 1, std::memory_order_acq_rel))
				{
				}

				if (held != 0)
				{
					return S_OK;
				}
			}

			// one atomic load per frame while no capture is pending
			auto mask = active_clients.load(std::memory_order_acquire);
			if (mask && (!swap_chain || (flags & DXGI_PRESENT_TEST) || !is_game_swap_chain(swap_chain)))
			{
				mask = 0;
			}

			if (mask)
			{
				call_before(mask, swap_chain);
			}

			const auto result = present_hook.invoke<HRESULT>(swap_chain, sync_interval, flags);

			if (mask)
			{
				call_after(mask, swap_chain, result);
			}

			return result;
		}

		// ------------------------------------------------------------------ hook installation (main thread)

		bool is_executable(const void* address)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!address || !VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
			{
				return false;
			}

			return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}

		std::string module_name(const void* address)
		{
			HMODULE module{};
			wchar_t path[MAX_PATH]{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        static_cast<LPCWSTR>(address), &module) || !GetModuleFileNameW(module, path, MAX_PATH))
			{
				return "no module";
			}

			const auto name = std::filesystem::path(path).filename().wstring();
			std::string ascii;
			for (const auto c : name) ascii += c < 0x80 ? static_cast<char>(c) : '?';
			return ascii;
		}

		// The pointer comes from engine memory: a wrong value must not crash the game (SEH, no C++ objects here).
		bool query_swap_chain(IUnknown* object)
		{
			__try
			{
				IDXGISwapChain* swap_chain = nullptr;
				const auto hr = object->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&swap_chain));
				if (FAILED(hr) || !swap_chain)
				{
					return false;
				}

				swap_chain->Release();
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		enum class probe
		{
			not_ready,
			ok,
			invalid,
		};

		probe present_from_game_swap_chain(void*& present)
		{
			auto* object = read_engine_window().swap_chain;
			if (!object)
			{
				return probe::not_ready;
			}

			if (!utils::hook::is_readable(object, sizeof(void*)))
			{
				return probe::invalid;
			}

			const auto* vtable = *reinterpret_cast<void* const* const*>(object);
			if (!utils::hook::is_readable(vtable, sizeof(void*) * (PRESENT_VTABLE_INDEX + 1)) ||
				!is_executable(vtable[0]) || !is_executable(vtable[PRESENT_VTABLE_INDEX]))
			{
				return probe::invalid;
			}

			if (!query_swap_chain(object))
			{
				return probe::invalid;
			}

			present = vtable[PRESENT_VTABLE_INDEX];
			return probe::ok;
		}

		// fallback: the same IDXGISwapChain implementation through a throw-away device on a hidden window
		void* present_from_dummy_swap_chain(std::string& error)
		{
			auto* d3d11 = GetModuleHandleW(L"d3d11.dll");
			if (!d3d11)
			{
				d3d11 = LoadLibraryW(L"d3d11.dll");
			}

			const auto create = d3d11
				                    ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"))
				                    : nullptr;
			if (!create)
			{
				error = "D3D11CreateDeviceAndSwapChain not found";
				return nullptr;
			}

			const auto instance = boot::get_module();
			constexpr auto class_name = L"iw6_theater_capture_probe";

			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = DefWindowProcW;
			wc.hInstance = instance;
			wc.lpszClassName = class_name;
			RegisterClassExW(&wc);

			const auto window = CreateWindowExW(0, class_name, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
			if (!window)
			{
				error = std::format("CreateWindowExW failed ({})", GetLastError());
				UnregisterClassW(class_name, instance);
				return nullptr;
			}

			DXGI_SWAP_CHAIN_DESC desc{};
			desc.BufferDesc.Width = 64;
			desc.BufferDesc.Height = 64;
			desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = 1;
			desc.OutputWindow = window;
			desc.Windowed = TRUE;
			desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

			void* present = nullptr;
			for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
			{
				ComPtr<IDXGISwapChain> swap_chain;
				ComPtr<ID3D11Device> device;
				ComPtr<ID3D11DeviceContext> context;
				const auto hr = create(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap_chain, &device, nullptr, &context);
				if (SUCCEEDED(hr) && swap_chain)
				{
					present = (*reinterpret_cast<void* const* const*>(swap_chain.Get()))[PRESENT_VTABLE_INDEX];
					break;
				}

				error = std::format("D3D11CreateDeviceAndSwapChain failed (0x{:08X})", static_cast<std::uint32_t>(hr));
			}

			DestroyWindow(window);
			UnregisterClassW(class_name, instance);
			return present;
		}

		void install_present_hook(void* target, const char* source)
		{
			try
			{
				present_hook.create(target, reinterpret_cast<void*>(&present_stub));
				hook_installed = true;
				console::log(std::format("theater_capture: IDXGISwapChain::Present hooked at {} ({}, found through the {})",
				                         target, module_name(target), source));
			}
			catch (const std::exception& e)
			{
				hook_failed_flag = true;
				console::warn("theater_capture: could not hook IDXGISwapChain::Present: %s\n", e.what());
			}
		}
	}

	void set_callbacks(const client c, const callbacks& cb)
	{
		clients[static_cast<std::size_t>(c)] = cb;
	}

	void update(const client c)
	{
		const auto index = static_cast<std::size_t>(c);
		const auto is_active = clients[index].is_active;
		if (!is_active)
		{
			return;
		}

		const auto bit = 1u << index;
		if (is_active())
		{
			active_clients.fetch_or(bit);
			return;
		}

		active_clients.fetch_and(~bit);

		// an activation (state change + update() on another thread) may have happened between the check and
		// the clear: its fetch_or may already be overwritten, so look at the state again
		if (is_active())
		{
			active_clients.fetch_or(bit);
		}
	}

	// main thread, from theater_screenshot::request() / theater_video::start(): the renderer is running by then,
	// so a missing or invalid engine swap chain pointer goes straight to the dummy swap chain fallback.
	// Every failure prints why.
	bool ensure_hook()
	{
		if (hook_installed)
		{
			return true;
		}

		if (hook_failed_flag)
		{
			return false;
		}

		void* present{};
		const auto game = present_from_game_swap_chain(present);
		if (game == probe::ok)
		{
			install_present_hook(present, "engine swap chain");
			return hook_installed;
		}

		console::log(game == probe::invalid
			             ? "theater_capture: engine swap chain pointer failed validation, using a dummy swap chain"
			             : "theater_capture: engine swap chain not found, using a dummy swap chain");

		std::string error;
		if (auto* fn = present_from_dummy_swap_chain(error))
		{
			install_present_hook(fn, "dummy swap chain");
		}
		else
		{
			hook_failed_flag = true;
			console::warn("theater_capture: Direct3D 11 hook unavailable (%s), screenshots and videos are disabled\n", error.data());
		}

		return hook_installed;
	}

	bool hook_failed()
	{
		return hook_failed_flag;
	}

	void hold_frames()
	{
		if (ensure_hook())
		{
			frames_held.store(-1, std::memory_order_release);
		}
	}

	void release_frames(const int still_held)
	{
		frames_held.store(std::max(0, still_held), std::memory_order_release);
	}

	engine_window read_engine_window()
	{
		static const auto readable = utils::hook::is_readable(reinterpret_cast<const void*>(R_WINDOW_COUNT),
		                                                      R_WINDOW_HEIGHT + sizeof(UINT) - R_WINDOW_COUNT);
		if (!readable || *reinterpret_cast<const volatile int*>(R_WINDOW_COUNT) < 1)
		{
			return {};
		}

		return {
			*reinterpret_cast<HWND const volatile*>(R_WINDOW_HWND),
			*reinterpret_cast<IDXGISwapChain* const volatile*>(R_WINDOW_SWAPCHAIN),
			*reinterpret_cast<const volatile UINT*>(R_WINDOW_WIDTH),
			*reinterpret_cast<const volatile UINT*>(R_WINDOW_HEIGHT),
		};
	}

	HWND read_main_window()
	{
		if (const auto window = read_engine_window(); window.hwnd)
		{
			return window.hwnd;
		}

		static const auto readable = utils::hook::is_readable(game::g_wv.get(), sizeof(game::WinVars_t));
		return readable ? game::g_wv->hWnd : nullptr;
	}

	const format_info* find_format(const DXGI_FORMAT format)
	{
		for (const auto& f : FORMATS)
		{
			if (f.format == format)
			{
				return &f;
			}
		}

		return nullptr;
	}

	const std::array<std::uint8_t, 65536>& half_to_srgb8_table()
	{
		static const auto table = []()
		{
			std::array<std::uint8_t, 65536> t{};
			for (std::uint32_t i = 0; i < t.size(); ++i)
			{
				t[i] = linear_to_srgb8(half_to_float(static_cast<std::uint16_t>(i)));
			}
			return t;
		}();

		return table;
	}

	std::uint8_t unorm10_to_8(const std::uint32_t v)
	{
		return static_cast<std::uint8_t>((v * 255u + 511u) / 1023u);
	}

	bool on_main_thread()
	{
		const auto id = main_thread_id.load();
		return id == 0 || id == GetCurrentThreadId();
	}

	void post(const int type, std::string text)
	{
		std::lock_guard _(message_mutex);
		if (messages.size() < 64)
		{
			messages.push_back({type, std::move(text)});
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			main_thread_id = GetCurrentThreadId(); // post_unpack runs in the main pipeline
			scheduler::loop(flush_messages, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(theater_capture::component)
