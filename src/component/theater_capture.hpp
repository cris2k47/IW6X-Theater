#pragma once

#include <dxgi.h>

// Plumbing shared by the theater capture features (theater_screenshot.cpp, theater_video.cpp). theater.cpp only
// uses the public headers of those two modules, plus hold_frames / release_frames here.
//
//  - ONE detour on IDXGISwapChain::Present, installed by the first ensure_hook() (never before). It calls the
//    callbacks of the active clients, for the game's swap chain only, and skips the game's frames while they are
//    held. While no client is active and nothing is held it costs two atomic loads per Present.
//  - the back buffer formats both modules can read back
//  - a message queue: text from the render / worker threads is printed to the console by the main thread
//
// Threads: the engine presents on its render thread, and on the main thread when the window procedure
// re-presents a frame after a fullscreen state change. Both paths hold the engine's device mutex (evidence in
// theater_capture.cpp), so Present callbacks never run concurrently and may use the immediate context.

namespace theater_capture
{
	// callbacks run in this order: the screenshot copies the back buffer before the video module draws its
	// progress bar on it
	enum class client : unsigned
	{
		screenshot,
		video,
		count,
	};

	struct callbacks
	{
		// inside the game's Present, before the real Present (game swap chain only, never DXGI_PRESENT_TEST)
		void (*before_present)(IDXGISwapChain* swap_chain){};
		// after the real Present returned 'result' (optional)
		void (*after_present)(IDXGISwapChain* swap_chain, HRESULT result){};
		// the client's own state, any thread: true while it needs the Present callbacks
		bool (*is_active)(){};
	};

	// once per client, before its first update() (component post_unpack, main thread)
	void set_callbacks(client c, const callbacks& cb);

	// call after a change of the client's state that may have made it active (any thread). Deactivation is
	// automatic: the Present hook clears the client's bit once is_active() returns false; an activation that
	// races with that is never lost (the state is re-checked after the bit was cleared).
	void update(client c);

	// main thread: hooks IDXGISwapChain::Present on first use (engine swap chain, dummy swap chain fallback)
	// and prints why it failed. false while the hook is unavailable.
	bool ensure_hook();
	[[nodiscard]] bool hook_failed();

	// main thread (theater.cpp, backward seeks): from now on the game's frames are not presented, so the screen keeps
	// the last one; release_frames(n) presents again after n more frames (the render thread presents a frame a frame
	// or two after the main thread built it). Without the hook nothing is held.
	void hold_frames();
	void release_frames(int still_held);
	[[nodiscard]] bool holding_frames(); // some of the game's frames are still not presented (any thread)

	// entry 0 of the engine's window table, all zero while it is not available
	struct engine_window
	{
		HWND hwnd{};
		IDXGISwapChain* swap_chain{};
		UINT width{};  // buffer size the engine created that swap chain with
		UINT height{};
	};

	[[nodiscard]] engine_window read_engine_window();
	[[nodiscard]] HWND read_main_window();

	// ------------------------------------------------------------------ back buffer formats

	enum class pixel_layout
	{
		rgba8,   // R8 G8 B8 A8
		bgra8,   // B8 G8 R8 A8/X8
		rgb10a2, // R10 G10 B10 A2 (little endian dword)
		rgba16f, // R16 G16 B16 A16 half floats, linear (scRGB)
	};

	struct format_info
	{
		DXGI_FORMAT format;
		DXGI_FORMAT typed_format; // for ResolveSubresource and render target views
		pixel_layout layout;
		UINT bytes_per_pixel;
	};

	// 8-bit formats are stored display-encoded (sRGB curve) whether the resource is _SRGB or not, so their bytes
	// are used as they are; only the linear FP16 format needs the sRGB curve applied (half_to_srgb8_table)
	[[nodiscard]] const format_info* find_format(DXGI_FORMAT format);
	[[nodiscard]] const std::array<std::uint8_t, 65536>& half_to_srgb8_table();
	[[nodiscard]] std::uint8_t unorm10_to_8(std::uint32_t v);

	// ------------------------------------------------------------------ threads / console

	// the game's main thread (theater_capture's post_unpack); true for any thread before that
	[[nodiscard]] bool on_main_thread();

	// any thread: queued and printed by the main thread (renderer pipeline), at most 64 pending messages
	void post(int type, std::string text);
}
