#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "theater_video.hpp"
#include "theater_capture.hpp"

#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "utils/thread.hpp"

#include <condition_variable>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <strmif.h>
#include <shlobj.h>

// GUID constants only (static data, no DLL import). Media Foundation itself is loaded at run time: a load-time
// import of mfplat.dll / mfreadwrite.dll would stop the game from loading XINPUT1_3.dll on Windows "N" editions.
#pragma comment(lib, "mfuuid.lib")

// Theater RENDER VIDEO: every frame the game presents is read back and encoded into an H.264 MP4.
//
// Flow (threads):
//  start()             main thread  loads Media Foundation, hooks Present (theater_capture::ensure_hook), starts
//                                   the encoder thread and waits until it has set up the sink writer for the
//                                   size in the engine's window table (theater_capture.cpp: 0x1480B1D80/84 are
//                                   the swap chain's BufferDesc.Width/Height) -> capturing
//  Present callback    render       first Present: staging ring (RING_SIZE textures). Skips 'skip_frames'
//  (theater_capture)   thread       Presents, then per Present: GetBuffer(0) (+ ResolveSubresource if multisampled)
//                                   -> CopyResource into the next ring slot; the slots of older frames are mapped
//                                   (MAP_LAG Presents later, DO_NOT_WAIT; blocking once the slot is needed again),
//                                   their rows copied into a pooled CPU frame and queued for the encoder; then the
//                                   progress bar is drawn on the back buffer (ClearView), after the copy.
//                                   No free CPU frame = the encoder is behind: the Present waits (backpressure),
//                                   up to STALL_TIMEOUT without encoder progress.
//  encoder thread                   CoInitializeEx(MTA) + MFStartup; per frame: BGRA/RGBA -> NV12 (BT.709, 16-235;
//                                   rows split over the conversion workers), IMFSinkWriter::WriteSample with
//                                   time = index * 10'000'000 / fps. After stop(): the render thread flushes the
//                                   ring at its next Present and closes the input, the encoder Finalize()s and
//                                   renames "<path>.part" -> path (copies it if the rename keeps failing, keeps the
//                                   .part if that fails too). The CPU frame pool is freed once both sides are done.
//
// Engine evidence: theater_capture.cpp (swap chain / window table / device mutex: every Present, including the
// main-thread re-present after a fullscreen change in MainWndProc, holds the device mutex 0x1480ADBF8, so the
// immediate context may be used in the callback; the device is created without SINGLETHREADED, so textures and
// views may be created there; nothing calls ResizeBuffers, the size only changes with a new swap chain) and
// theater_screenshot.cpp ("Frame lag": the render thread presents at most 2 frames after the main thread issued
// them -> default skip_frames 3).
//
// Waiting inside Present (device mutex held) cannot deadlock: the encoder thread and its Media Foundation
// pipeline never wait for the game's main or render thread (and never touch the game's D3D device or window);
// at worst the main thread waits for the render thread / the device mutex meanwhile, i.e. the game stalls.

namespace theater_video
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using theater_capture::pixel_layout;

		// ------------------------------------------------------------------ tuning

		constexpr int MAX_FPS = 240;
		constexpr int MAX_SKIP_FRAMES = 1000;
		constexpr UINT MIN_DIMENSION = 16;
		constexpr UINT MAX_DIMENSION = 8192;                    // H.264 level 6.x limit
		constexpr std::size_t RING_SIZE = 4;                     // staging textures
		constexpr std::uint64_t MAP_LAG = 2;                     // Presents between a frame's copy and its first Map attempt
		constexpr std::size_t FRAME_POOL_BYTES = 128ull << 20;  // CPU frames between the render thread and the encoder
		constexpr std::size_t MIN_POOL_FRAMES = 3;
		constexpr std::size_t MAX_POOL_FRAMES = 6;
		constexpr auto STALL_TIMEOUT = 5s;  // Present waited this long for an encoder that made no progress -> error
		constexpr auto FLUSH_TIMEOUT = 5s;  // no Present after stop(): finalize without the frames still in the ring
		constexpr auto SETUP_TIMEOUT = 20s; // start() waiting for the encoder thread
		constexpr std::uint64_t TICKS_PER_SECOND = 10'000'000; // Media Foundation time unit: 100 ns
		constexpr double MIN_BITRATE = 8e6;
		constexpr double MAX_BITRATE = 150e6;
		// quality presets (theater_video_quality) used while theater_video_bitrate is 0: bits per pixel of every frame, so
		// the bitrate follows the video size and the fps. The THEATER SETTINGS menu (lui\theater\__init__.lua) shows the
		// same table: low, medium, high (the default, the automatic bitrate of earlier versions), ultra, max
		constexpr std::array QUALITY_BITS_PER_PIXEL{0.1, 0.2, 0.3, 0.5, 0.8};
		constexpr int DEFAULT_VIDEO_QUALITY = 2;
		constexpr int DEFAULT_TEST_FPS = 60;

		// progress bar colors (linear RGBA, ClearView converts for _SRGB views)
		constexpr float BAR_BACKGROUND[4]{0.06f, 0.06f, 0.06f, 1.0f};
		constexpr float BAR_FILL[4]{1.0f, 0.62f, 0.05f, 1.0f};

		// mfreadwrite.h declares MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, but it is not in mfuuid.lib (only in
		// mfreadwrite.lib, which must not be linked): same value under a local name
		constexpr GUID READWRITE_ENABLE_HARDWARE_TRANSFORMS{0xa634a91c, 0x822b, 0x41b9, {0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0}};

		// FOLDERID_Videos {18989B1D-99B5-455B-841C-AB7C74E4DDFC} (KnownFolders.h), local copy: no uuid.lib dependency
		constexpr GUID VIDEOS_FOLDER{0x18989b1d, 0x99b5, 0x455b, {0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc}};

		// ------------------------------------------------------------------ console output

		// any thread: printed by the main thread (theater_capture flushes the queue in the renderer pipeline)
		void post(const int type, const std::string& text)
		{
			theater_capture::post(type, "theater_video: " + text);
		}

		std::mutex result_mutex;
		std::optional<std::filesystem::path> result;
		std::string last_error;

		// any thread
		void report_error(const std::string& text)
		{
			{
				std::lock_guard _(result_mutex);
				last_error = text;
			}

			post(console::con_type_warning, text);
		}

		// main thread
		bool fail_now(const std::string& text)
		{
			{
				std::lock_guard _(result_mutex);
				last_error = text;
			}

			console::warn("theater_video: %s\n", text.data());
			return false;
		}

		std::string hresult(const HRESULT hr)
		{
			return std::format("0x{:08X}", static_cast<std::uint32_t>(hr));
		}

		std::string narrow(const std::wstring& text)
		{
			std::string out;
			for (const auto c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
			return out;
		}

		// for messages: path::string() throws for characters outside the ANSI code page (e.g. a Cyrillic user name)
		std::string display(const std::filesystem::path& path)
		{
			const auto& wide = path.native();
			if (wide.empty())
			{
				return {};
			}

			const auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0)
			{
				return narrow(wide);
			}

			std::string out(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		// ------------------------------------------------------------------ Media Foundation (run-time linking)

		struct media_foundation
		{
			decltype(&::MFStartup) startup{};
			decltype(&::MFShutdown) shutdown{};
			decltype(&::MFCreateAttributes) create_attributes{};
			decltype(&::MFCreateMediaType) create_media_type{};
			decltype(&::MFCreateSample) create_sample{};
			decltype(&::MFCreateMemoryBuffer) create_memory_buffer{};
			decltype(&::MFCreateSinkWriterFromURL) create_sink_writer{};
		};

		// written once by start() on the main thread before the first encoder thread exists, read-only afterwards;
		// the DLLs are never unloaded
		media_foundation mf{};
		bool mf_loaded{};

		HMODULE load_system_dll(const wchar_t* name)
		{
			// System32 only: a DLL with the same name in the game folder must not be picked up
			if (auto* module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
			{
				return module;
			}

			// Windows 7 without KB2533623 does not know LOAD_LIBRARY_SEARCH_SYSTEM32
			return GetLastError() == ERROR_INVALID_PARAMETER ? LoadLibraryW(name) : nullptr;
		}

		bool load_media_foundation(std::string& error)
		{
			if (mf_loaded)
			{
				return true;
			}

			auto* mfplat = load_system_dll(L"mfplat.dll");
			auto* mfreadwrite = mfplat ? load_system_dll(L"mfreadwrite.dll") : nullptr;
			if (!mfplat || !mfreadwrite)
			{
				error = std::format("Media Foundation is not available ({} could not be loaded, error {}); "
				                    "Windows N/KN editions need the Media Feature Pack",
				                    mfplat ? "mfreadwrite.dll" : "mfplat.dll", GetLastError());
				if (mfplat) FreeLibrary(mfplat);
				return false;
			}

			media_foundation api{};
			const char* missing = nullptr;
			const auto get = [&missing](auto& function, HMODULE module, const char* name)
			{
				function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
				if (!function && !missing) missing = name;
			};

			get(api.startup, mfplat, "MFStartup");
			get(api.shutdown, mfplat, "MFShutdown");
			get(api.create_attributes, mfplat, "MFCreateAttributes");
			get(api.create_media_type, mfplat, "MFCreateMediaType");
			get(api.create_sample, mfplat, "MFCreateSample");
			get(api.create_memory_buffer, mfplat, "MFCreateMemoryBuffer");
			get(api.create_sink_writer, mfreadwrite, "MFCreateSinkWriterFromURL");

			if (missing)
			{
				error = std::format("Media Foundation is incomplete ({} not found)", missing);
				FreeLibrary(mfreadwrite);
				FreeLibrary(mfplat);
				return false;
			}

			mf = api;
			mf_loaded = true;
			return true;
		}

		// ------------------------------------------------------------------ RGB -> NV12 (BT.709, limited range)

		// Y = 16 + 219 * (Kr R + Kg G + Kb B), Cb = 128 + 224 * (B - Y') / (2 (1 - Kb)), Cr = 128 + 224 * (R - Y') /
		// (2 (1 - Kr)) with R, G, B the display-encoded (sRGB curve) 8-bit values / 255, as 16.16 fixed point. The
		// middle coefficient absorbs the rounding so that white is exactly 235 and every gray has Cb = Cr = 128.
		constexpr int fixed16(const double v)
		{
			const auto s = v * 65536.0;
			return static_cast<int>(s >= 0.0 ? s + 0.5 : s - 0.5);
		}

		constexpr double KR = 0.2126;
		constexpr double KB = 0.0722;
		constexpr double Y_RANGE = 219.0 / 255.0;
		constexpr double C_RANGE = 224.0 / 255.0;

		constexpr int YR = fixed16(Y_RANGE * KR);
		constexpr int YB = fixed16(Y_RANGE * KB);
		constexpr int YG = fixed16(Y_RANGE) - YR - YB;

		constexpr int UR = fixed16(-C_RANGE * KR / (2.0 * (1.0 - KB)));
		constexpr int UB = fixed16(C_RANGE * 0.5);
		constexpr int UG = -UR - UB;

		constexpr int VR = fixed16(C_RANGE * 0.5);
		constexpr int VB = fixed16(-C_RANGE * KB / (2.0 * (1.0 - KR)));
		constexpr int VG = -VR - VB;

		static_assert(YR + YG + YB == fixed16(Y_RANGE) && UR + UG + UB == 0 && VR + VG + VB == 0);

		struct rgb
		{
			int r;
			int g;
			int b;
		};

		std::uint8_t luma(const rgb& p)
		{
			// [16, 235] for any 8-bit input
			return static_cast<std::uint8_t>((YR * p.r + YG * p.g + YB * p.b + (16 << 16) + (1 << 15)) >> 16);
		}

		// r, g, b: sums of a 2x2 block (0..1020); results are in [16, 240], the sums never go negative
		std::uint8_t chroma_u(const int r, const int g, const int b)
		{
			return static_cast<std::uint8_t>((UR * r + UG * g + UB * b + (128 << 18) + (1 << 17)) >> 18);
		}

		std::uint8_t chroma_v(const int r, const int g, const int b)
		{
			return static_cast<std::uint8_t>((VR * r + VG * g + VB * b + (128 << 18) + (1 << 17)) >> 18);
		}

		struct conversion
		{
			const std::uint8_t* source{};
			std::size_t source_pitch{};
			std::size_t width{};          // even, <= source width
			std::uint8_t* luma{};         // width x height
			std::uint8_t* chroma{};       // width x height / 2, interleaved U V
			const std::uint8_t* half_table{};
		};

		template <pixel_layout Layout>
		rgb fetch(const conversion& c, const std::uint8_t* row, const std::size_t x)
		{
			if constexpr (Layout == pixel_layout::rgba8)
			{
				const auto* p = row + x * 4;
				return {p[0], p[1], p[2]};
			}
			else if constexpr (Layout == pixel_layout::bgra8)
			{
				const auto* p = row + x * 4;
				return {p[2], p[1], p[0]};
			}
			else if constexpr (Layout == pixel_layout::rgb10a2)
			{
				std::uint32_t v{};
				std::memcpy(&v, row + x * 4, sizeof(v));
				return {theater_capture::unorm10_to_8(v & 0x3FFu), theater_capture::unorm10_to_8((v >> 10) & 0x3FFu),
				        theater_capture::unorm10_to_8((v >> 20) & 0x3FFu)};
			}
			else
			{
				std::uint16_t h[3]{};
				std::memcpy(h, row + x * 8, sizeof(h));
				return {c.half_table[h[0]], c.half_table[h[1]], c.half_table[h[2]]};
			}
		}

		// rows [2 * pair_begin, 2 * pair_end): 2x2 blocks -> 4 luma samples + 1 averaged chroma pair
		template <pixel_layout Layout>
		void convert_rows(const conversion& c, const std::size_t pair_begin, const std::size_t pair_end)
		{
			for (auto pair = pair_begin; pair < pair_end; ++pair)
			{
				const auto* s0 = c.source + pair * 2 * c.source_pitch;
				const auto* s1 = s0 + c.source_pitch;
				auto* y0 = c.luma + pair * 2 * c.width;
				auto* y1 = y0 + c.width;
				auto* uv = c.chroma + pair * c.width;

				for (std::size_t x = 0; x < c.width; x += 2)
				{
					const auto a = fetch<Layout>(c, s0, x);
					const auto b = fetch<Layout>(c, s0, x + 1);
					const auto d = fetch<Layout>(c, s1, x);
					const auto e = fetch<Layout>(c, s1, x + 1);

					y0[x] = luma(a);
					y0[x + 1] = luma(b);
					y1[x] = luma(d);
					y1[x + 1] = luma(e);

					const auto r = a.r + b.r + d.r + e.r;
					const auto g = a.g + b.g + d.g + e.g;
					const auto bl = a.b + b.b + d.b + e.b;
					uv[x] = chroma_u(r, g, bl);
					uv[x + 1] = chroma_v(r, g, bl);
				}
			}
		}

		// small fixed pool: run(bands, fn) calls fn(0..bands-1) on the workers and the calling thread
		class band_workers
		{
		public:
			explicit band_workers(const unsigned count)
			{
				for (unsigned i = 0; i < count; ++i)
				{
					try
					{
						threads_.emplace_back([this]()
						{
							utils::thread::set_name(L"theater video convert");
							this->worker();
						});
					}
					catch (...)
					{
						break; // fewer workers, the encoder thread converts the rest itself
					}
				}
			}

			~band_workers()
			{
				{
					std::lock_guard _(mutex_);
					quit_ = true;
				}

				start_cv_.notify_all();
				for (auto& t : threads_)
				{
					t.join();
				}
			}

			band_workers(const band_workers&) = delete;
			band_workers& operator=(const band_workers&) = delete;

			[[nodiscard]] unsigned size() const
			{
				return static_cast<unsigned>(threads_.size());
			}

			void run(const unsigned bands, const std::function<void(unsigned)>& job)
			{
				if (threads_.empty() || bands <= 1)
				{
					for (unsigned i = 0; i < bands; ++i) job(i);
					return;
				}

				{
					std::lock_guard _(mutex_);
					job_ = &job;
					bands_ = bands;
					next_ = 0;
					finished_ = 0;
					++generation_;
				}

				start_cv_.notify_all();
				this->work();

				std::unique_lock lock(mutex_);
				done_cv_.wait(lock, [this]() { return finished_ == bands_; });
				job_ = nullptr;
			}

		private:
			std::vector<std::thread> threads_;
			std::mutex mutex_;
			std::condition_variable start_cv_;
			std::condition_variable done_cv_;
			const std::function<void(unsigned)>* job_{};
			unsigned bands_{};
			unsigned next_{};
			unsigned finished_{};
			std::uint64_t generation_{};
			bool quit_{};

			void work()
			{
				for (;;)
				{
					const std::function<void(unsigned)>* job{};
					unsigned band{};
					{
						std::lock_guard _(mutex_);
						if (!job_ || next_ >= bands_)
						{
							return;
						}

						job = job_;
						band = next_++;
					}

					(*job)(band);

					std::lock_guard _(mutex_);
					if (++finished_ == bands_)
					{
						done_cv_.notify_all();
					}
				}
			}

			void worker()
			{
				std::uint64_t seen{};
				for (;;)
				{
					{
						std::unique_lock lock(mutex_);
						start_cv_.wait(lock, [&]() { return quit_ || generation_ != seen; });
						if (quit_)
						{
							return;
						}

						seen = generation_;
					}

					this->work();
				}
			}
		};

		// ------------------------------------------------------------------ capture session

		struct cpu_frame
		{
			std::unique_ptr<std::uint8_t[]> pixels{}; // tightly packed rows in 'format'
			std::size_t capacity{};
			DXGI_FORMAT format{};
			UINT width{};
			UINT height{};
			std::uint64_t index{}; // video frame index
		};

		enum class state
		{
			capturing, // every Present after skip_frames is captured
			stopping,  // stop() or an error: the render side flushes its ring at the next Present, then the encoder finalizes
			cancelled, // cancel(): everything is dropped and the file deleted
			finished,  // encoder thread done, result / error published
		};

		struct staging_slot
		{
			ComPtr<ID3D11Texture2D> texture{};
			std::uint64_t index{};
			bool pending{};
		};

		// only touched inside the game's Present (serialized by the device mutex)
		struct render_side
		{
			bool initialized{};
			bool done{};
			bool no_flush{};   // the ring cannot be read any more (device lost, Map failure, encoder stalled)
			bool bar_failed{};
			IDXGISwapChain* swap_chain{}; // identity only, never used to call into the swap chain
			ComPtr<ID3D11Device> device{};
			ComPtr<ID3D11DeviceContext> context{};
			ComPtr<ID3D11DeviceContext1> context1{}; // ClearView (D3D 11.1 runtime), no progress bar without it
			D3D11_TEXTURE2D_DESC desc{};             // back buffer at the first Present after start()
			const theater_capture::format_info* format{};
			ComPtr<ID3D11Texture2D> resolved{};      // multisampled back buffers only
			std::array<staging_slot, RING_SIZE> ring{};
			std::size_t pending{};                   // frames copied but not read back: next_index - pending .. next_index - 1
			std::uint64_t next_index{};              // video frame index of the next captured frame
			int skip_left{};
		};

		struct session
		{
			// set by start(), read-only afterwards
			std::filesystem::path path{};
			std::filesystem::path part_path{};
			int fps{};
			int skip_frames{};
			UINT expected_width{};  // engine window table, the encoder is set up for this size by start()
			UINT expected_height{};
			int bitrate_mbps{};     // theater_video_bitrate, 0 = from the quality preset
			int quality{DEFAULT_VIDEO_QUALITY}; // theater_video_quality

			std::atomic<state> st{state::capturing};
			std::atomic<std::uint64_t> frames_written{0};

			std::mutex mutex;
			std::condition_variable encoder_cv; // queued frame, input closed, abort, stop
			std::condition_variable render_cv;  // CPU frame returned, encoder failed, abort
			std::condition_variable setup_cv;
			std::deque<cpu_frame*> queue;
			std::vector<cpu_frame*> free_frames;
			std::vector<std::unique_ptr<cpu_frame>> all_frames;
			std::size_t max_frames{};
			bool input_closed{};    // the render side will not queue any more frames
			bool render_closed{};   // finish_render() ran: the render side never touches a CPU frame again
			bool encoder_done{};    // encode_frames() returned: the encoder never touches a CPU frame again
			bool abort{};           // cancel()
			bool encoder_failed{};
			bool published{};       // result published, cancel() comes too late
			bool error_reported{};  // record_error() ran: a later "no frames" must not replace that error
			bool setup_done{};
			bool setup_ok{};
			std::string setup_error{};
			std::string encoder_description{};
			std::chrono::steady_clock::time_point stop_time{};     // when the capture left 'capturing'
			std::chrono::steady_clock::time_point last_progress{}; // last frame the encoder finished

			render_side render{};
		};

		std::mutex session_mutex;
		// the latest session (also after it finished). Intentionally never destroyed: a static destructor would
		// Release the D3D objects of an unfinished capture from DLL_PROCESS_DETACH (see theater_screenshot.cpp)
		std::shared_ptr<session>& current_session = *new std::shared_ptr<session>();
		std::atomic_bool render_needed{false};    // the current session's render side has not finished yet
		std::atomic<float> progress{-1.0f};
		game::dvar_t* bitrate_dvar{};
		game::dvar_t* quality_dvar{};

		std::shared_ptr<session> get_session()
		{
			std::lock_guard _(session_mutex);
			return current_session;
		}

		bool needs_present()
		{
			return render_needed.load();
		}

		// s.mutex held, once both render_closed and encoder_done are set: the CPU frame pool (50 MB at 1080p, up to
		// ~200 MB at 4K) goes back now instead of staying allocated in current_session until the next start(). Safe
		// then: a frame is only used outside the lock between acquire_frame() and submit_frame() (render side, before
		// finish_render) or between popping it and returning it (encoder, inside encode_frames)
		void release_frames(session& s)
		{
			s.queue.clear();
			s.free_frames.clear();
			s.all_frames.clear(); // owns the frames and their pixel buffers
		}

		// any thread: the first error ends the capture (frames written so far are kept)
		void record_error(session& s, const std::string& text)
		{
			{
				std::lock_guard lock(s.mutex);
				s.error_reported = true;
				if (s.stop_time == std::chrono::steady_clock::time_point{})
				{
					s.stop_time = std::chrono::steady_clock::now();
				}
			}

			auto expected = state::capturing;
			s.st.compare_exchange_strong(expected, state::stopping);
			s.encoder_cv.notify_all();
			s.render_cv.notify_all();
			report_error(text);
		}

		// ------------------------------------------------------------------ render side (inside Present)

		bool init_render(session& s, IDXGISwapChain* swap_chain, ID3D11Texture2D* back_buffer, const D3D11_TEXTURE2D_DESC& desc,
		                 std::string& error)
		{
			auto& r = s.render;

			r.format = theater_capture::find_format(desc.Format);
			if (!r.format)
			{
				error = std::format("unsupported back buffer format (DXGI_FORMAT {})", static_cast<int>(desc.Format));
				return false;
			}

			if (desc.Width < MIN_DIMENSION || desc.Height < MIN_DIMENSION || desc.Width > MAX_DIMENSION || desc.Height > MAX_DIMENSION)
			{
				error = std::format("unsupported back buffer size {}x{}", desc.Width, desc.Height);
				return false;
			}

			back_buffer->GetDevice(&r.device);
			if (r.device)
			{
				r.device->GetImmediateContext(&r.context);
			}

			if (!r.device || !r.context)
			{
				error = "the back buffer has no device / immediate context";
				return false;
			}

			if (FAILED(r.context.As(&r.context1)))
			{
				r.context1.Reset(); // Windows 7 without the platform update: no progress bar
			}

			D3D11_TEXTURE2D_DESC staging_desc{};
			staging_desc.Width = desc.Width;
			staging_desc.Height = desc.Height;
			staging_desc.MipLevels = 1;
			staging_desc.ArraySize = 1;
			staging_desc.Format = desc.Format;
			staging_desc.SampleDesc.Count = 1;
			staging_desc.Usage = D3D11_USAGE_STAGING;
			staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

			for (auto& slot : r.ring)
			{
				const auto hr = r.device->CreateTexture2D(&staging_desc, nullptr, &slot.texture);
				if (FAILED(hr) || !slot.texture)
				{
					error = std::format("CreateTexture2D(STAGING {}x{}) failed ({})", desc.Width, desc.Height, hresult(hr));
					return false;
				}
			}

			if (desc.SampleDesc.Count > 1)
			{
				UINT support{};
				if (FAILED(r.device->CheckFormatSupport(r.format->typed_format, &support)) ||
					!(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE))
				{
					error = std::format("multisampled back buffer format {} cannot be resolved", static_cast<int>(desc.Format));
					return false;
				}

				auto resolved_desc = staging_desc;
				resolved_desc.Usage = D3D11_USAGE_DEFAULT;
				resolved_desc.CPUAccessFlags = 0;

				auto hr = r.device->CreateTexture2D(&resolved_desc, nullptr, &r.resolved);
				if (FAILED(hr) || !r.resolved)
				{
					resolved_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
					hr = r.device->CreateTexture2D(&resolved_desc, nullptr, &r.resolved);
				}

				if (FAILED(hr) || !r.resolved)
				{
					error = std::format("CreateTexture2D(resolve target) failed ({})", hresult(hr));
					return false;
				}
			}

			r.desc = desc;
			r.swap_chain = swap_chain;
			r.skip_left = s.skip_frames;
			r.initialized = true;

			// start() sized the CPU frame pool for 4 bytes per pixel; FP16 back buffers have 8
			const auto frame_bytes = static_cast<std::size_t>(desc.Width) * desc.Height * r.format->bytes_per_pixel;
			std::lock_guard _(s.mutex);
			s.max_frames = std::clamp(FRAME_POOL_BYTES / frame_bytes, MIN_POOL_FRAMES, MAX_POOL_FRAMES);
			return true;
		}

		// waits for a free CPU frame while the encoder is behind; nullptr once the capture is over
		cpu_frame* acquire_frame(session& s, const std::size_t bytes)
		{
			const auto wait_start = std::chrono::steady_clock::now();

			std::unique_lock lock(s.mutex);
			for (;;)
			{
				if (s.abort || s.encoder_failed || s.input_closed)
				{
					return nullptr;
				}

				if (!s.free_frames.empty())
				{
					auto* frame = s.free_frames.back();
					s.free_frames.pop_back();
					if (frame->capacity >= bytes)
					{
						return frame;
					}

					// cannot happen (one size per capture), but never write past the buffer
					frame->pixels.reset(new (std::nothrow) std::uint8_t[bytes]);
					frame->capacity = frame->pixels ? bytes : 0;
					if (frame->pixels)
					{
						return frame;
					}

					s.free_frames.push_back(frame);
				}
				else if (s.all_frames.size() < s.max_frames)
				{
					auto frame = std::make_unique<cpu_frame>();
					frame->pixels.reset(new (std::nothrow) std::uint8_t[bytes]);
					if (frame->pixels)
					{
						frame->capacity = bytes;
						s.all_frames.push_back(std::move(frame));
						return s.all_frames.back().get();
					}

					s.max_frames = s.all_frames.size(); // out of memory: work with what exists
				}

				if (s.all_frames.empty())
				{
					lock.unlock();
					record_error(s, std::format("out of memory ({} bytes per frame)", bytes));
					return nullptr;
				}

				const auto since = std::max(wait_start, s.last_progress);
				if (std::chrono::steady_clock::now() - since > STALL_TIMEOUT)
				{
					lock.unlock();
					record_error(s, std::format("the encoder made no progress for {} s, recording stopped",
					                            std::chrono::duration_cast<std::chrono::seconds>(STALL_TIMEOUT).count()));
					return nullptr;
				}

				s.render_cv.wait_for(lock, 100ms);
			}
		}

		void submit_frame(session& s, cpu_frame* frame)
		{
			{
				std::lock_guard lock(s.mutex);
				if (s.abort || s.input_closed || s.encoder_failed)
				{
					s.free_frames.push_back(frame);
					return;
				}

				s.queue.push_back(frame);
			}

			s.encoder_cv.notify_all();
		}

		void drop_pending(render_side& r)
		{
			for (auto& slot : r.ring)
			{
				slot.pending = false;
			}

			r.pending = 0;
		}

		// maps the copied frames in order (oldest first): those at least MAP_LAG Presents old without waiting for
		// the GPU, the one whose slot the next Present needs (or all of them when 'final') blocking
		void read_back(session& s, const bool final)
		{
			auto& r = s.render;
			const auto row = static_cast<std::size_t>(r.desc.Width) * r.format->bytes_per_pixel;
			const auto bytes = row * r.desc.Height;

			while (r.pending > 0)
			{
				const auto index = r.next_index - r.pending;
				auto& slot = r.ring[index % RING_SIZE];
				const auto age = r.next_index - 1 - index; // Presents since its copy was queued
				if (!final && age < MAP_LAG)
				{
					return;
				}

				const auto blocking = final || age + 1 >= RING_SIZE;
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const auto hr = r.context->Map(slot.texture.Get(), 0, D3D11_MAP_READ, blocking ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING && !blocking)
				{
					return;
				}

				if (FAILED(hr) || !mapped.pData || mapped.RowPitch < row)
				{
					if (SUCCEEDED(hr))
					{
						r.context->Unmap(slot.texture.Get(), 0);
					}

					drop_pending(r);
					r.no_flush = true;
					const auto removed = r.device ? r.device->GetDeviceRemovedReason() : S_OK;
					record_error(s, FAILED(removed)
						                ? std::format("the Direct3D device was lost ({}), recording stopped", hresult(removed))
						                : std::format("reading the frame back failed (Map {}, row pitch {}), recording stopped",
						                              hresult(hr), mapped.RowPitch));
					return;
				}

				auto* frame = acquire_frame(s, bytes); // backpressure: waits while the encoder is behind
				if (frame)
				{
					const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
					if (mapped.RowPitch == row)
					{
						std::memcpy(frame->pixels.get(), src, bytes);
					}
					else
					{
						for (std::size_t y = 0; y < r.desc.Height; ++y)
						{
							std::memcpy(frame->pixels.get() + y * row, src + y * mapped.RowPitch, row);
						}
					}

					frame->format = r.desc.Format;
					frame->width = r.desc.Width;
					frame->height = r.desc.Height;
					frame->index = slot.index;
				}

				r.context->Unmap(slot.texture.Get(), 0);
				slot.pending = false;
				--r.pending;

				if (!frame)
				{
					// cancelled, encoder failed or stalled: nothing else can be delivered
					drop_pending(r);
					r.no_flush = true;
					return;
				}

				submit_frame(s, frame);
			}
		}

		void copy_frame(session& s, ID3D11Texture2D* back_buffer)
		{
			auto& r = s.render;
			auto& slot = r.ring[r.next_index % RING_SIZE];
			if (slot.pending)
			{
				// read_back() always frees the slot the next Present needs; never overwrite an unread frame
				read_back(s, true);
				if (slot.pending || s.st.load() != state::capturing)
				{
					return;
				}
			}

			if (r.desc.SampleDesc.Count > 1)
			{
				r.context->ResolveSubresource(r.resolved.Get(), 0, back_buffer, 0, r.format->typed_format);
				r.context->CopyResource(slot.texture.Get(), r.resolved.Get());
			}
			else
			{
				r.context->CopyResource(slot.texture.Get(), back_buffer);
			}

			slot.index = r.next_index++;
			slot.pending = true;
			++r.pending;
		}

		// after this frame's copy was queued: drawn on the back buffer only, never in the video. The view is created
		// per Present: a reference to the back buffer held across Presents would make anyone's ResizeBuffers fail.
		void draw_progress_bar(session& s, ID3D11Texture2D* back_buffer)
		{
			auto& r = s.render;
			const auto fraction = progress.load();
			if (!(fraction >= 0.0f) || !r.context1 || r.bar_failed)
			{
				return;
			}

			D3D11_RENDER_TARGET_VIEW_DESC view{};
			view.Format = r.format->typed_format;
			view.ViewDimension = r.desc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;

			ComPtr<ID3D11RenderTargetView> target;
			if (FAILED(r.device->CreateRenderTargetView(back_buffer, &view, &target)) || !target)
			{
				r.bar_failed = true;
				post(console::con_type_warning, "the progress bar cannot be drawn (CreateRenderTargetView failed)");
				return;
			}

			const auto width = static_cast<LONG>(r.desc.Width);
			const auto height = std::max<LONG>(4, static_cast<LONG>(r.desc.Height) / 180); // 6 px at 1080p
			const auto fill = static_cast<LONG>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * static_cast<float>(width)));

			if (fill < width)
			{
				const D3D11_RECT rect{fill, 0, width, height};
				r.context1->ClearView(target.Get(), BAR_BACKGROUND, &rect, 1);
			}

			if (fill > 0)
			{
				const D3D11_RECT rect{0, 0, fill, height};
				r.context1->ClearView(target.Get(), BAR_FILL, &rect, 1);
			}
		}

		void capture_present(session& s, IDXGISwapChain* swap_chain)
		{
			auto& r = s.render;

			ComPtr<ID3D11Texture2D> back_buffer;
			const auto hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
			if (FAILED(hr) || !back_buffer)
			{
				record_error(s, std::format("IDXGISwapChain::GetBuffer(ID3D11Texture2D) failed ({}), recording stopped", hresult(hr)));
				return;
			}

			D3D11_TEXTURE2D_DESC desc{};
			back_buffer->GetDesc(&desc);

			if (!r.initialized)
			{
				std::string error;
				if (!init_render(s, swap_chain, back_buffer.Get(), desc, error))
				{
					r.no_flush = true;
					record_error(s, error);
					return;
				}
			}
			else if (swap_chain != r.swap_chain || [&]()
			{
				// a new swap chain at the old address still belongs to another device
				ComPtr<ID3D11Device> device;
				back_buffer->GetDevice(&device);
				return device.Get() != r.device.Get();
			}())
			{
				record_error(s, "the game's swap chain was recreated (vid_restart / device reset), recording stopped");
				return;
			}
			else if (desc.Width != r.desc.Width || desc.Height != r.desc.Height || desc.Format != r.desc.Format ||
				desc.SampleDesc.Count != r.desc.SampleDesc.Count)
			{
				record_error(s, std::format("the back buffer changed from {}x{} to {}x{}, recording stopped",
				                            r.desc.Width, r.desc.Height, desc.Width, desc.Height));
				return;
			}

			if (r.skip_left > 0)
			{
				--r.skip_left; // still in flight from before start()
			}
			else
			{
				copy_frame(s, back_buffer.Get());
				read_back(s, false);
			}

			if (s.st.load() == state::capturing)
			{
				draw_progress_bar(s, back_buffer.Get());
			}
		}

		void finish_render(session& s)
		{
			auto& r = s.render;
			if (r.initialized && !r.no_flush && s.st.load() == state::stopping)
			{
				read_back(s, true); // frames still in the ring, in order (may wait for the encoder)
			}

			drop_pending(r);
			r.ring = {};
			r.resolved.Reset();
			r.context1.Reset();
			r.context.Reset();
			r.device.Reset();
			r.done = true;

			{
				std::lock_guard lock(s.mutex);
				s.input_closed = true;
				s.render_closed = true;
				if (s.encoder_done)
				{
					release_frames(s);
				}
			}

			s.encoder_cv.notify_all();

			std::lock_guard _(session_mutex);
			if (current_session.get() == &s)
			{
				render_needed = false; // theater_capture stops calling us after this Present
			}
		}

		void before_present(IDXGISwapChain* swap_chain)
		{
			const auto s = get_session();
			if (!s || s->render.done)
			{
				return;
			}

			if (s->st.load() == state::capturing)
			{
				capture_present(*s, swap_chain);
			}

			// stop() (main thread) or an error during this Present: flush and close the input right away
			if (s->st.load() != state::capturing)
			{
				finish_render(*s);
			}
		}

		void after_present(IDXGISwapChain*, const HRESULT result)
		{
			if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET)
			{
				return;
			}

			const auto s = get_session();
			if (!s || s->render.done)
			{
				return;
			}

			s->render.no_flush = true;
			record_error(*s, std::format("the Direct3D device was lost (Present {}), recording stopped", hresult(result)));
			finish_render(*s);
		}

		// ------------------------------------------------------------------ encoder (encoder thread)

		struct encoder
		{
			ComPtr<IMFSinkWriter> writer{};
			DWORD stream{};
			UINT width{};  // encoded size: the back buffer size made even (1 px cropped if needed)
			UINT height{};
			std::uint32_t bitrate{};
			std::string description{};
			bool has_samples{};
		};

		std::uint32_t choose_bitrate(const UINT width, const UINT height, const int fps, const int mbps, const int quality)
		{
			const auto preset = std::clamp(quality, 0, static_cast<int>(QUALITY_BITS_PER_PIXEL.size()) - 1);
			const auto bits_per_pixel = QUALITY_BITS_PER_PIXEL[static_cast<std::size_t>(preset)];
			const auto bits = mbps > 0
				                  ? static_cast<double>(mbps) * 1e6
				                  : std::round(static_cast<double>(width) * height * fps * bits_per_pixel);
			return static_cast<std::uint32_t>(std::clamp(bits, MIN_BITRATE, MAX_BITRATE));
		}

		void remove_file(const std::filesystem::path& path)
		{
			// the sink writer may close its file a moment after it was released
			for (auto attempt = 0; attempt < 40; ++attempt)
			{
				std::error_code ec;
				if (!std::filesystem::exists(path, ec) || std::filesystem::remove(path, ec))
				{
					return;
				}

				Sleep(50);
			}
		}

		HRESULT set_video_attributes(IMFMediaType* type, const UINT width, const UINT height, const int fps)
		{
			auto hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
			if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
			if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
			if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
			if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
			if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
			if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
			if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
			if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
			return hr;
		}

		std::string describe_encoder(IMFSinkWriter* writer, const DWORD stream, const bool hardware_allowed)
		{
			std::string name;
			auto hardware = false;

			ComPtr<IMFTransform> transform;
			if (SUCCEEDED(writer->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&transform))) && transform)
			{
				ComPtr<IMFAttributes> attributes;
				if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes)
				{
					WCHAR* text{};
					UINT32 length{};
					if (SUCCEEDED(attributes->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &text, &length)) && text)
					{
						name = narrow(text);
						CoTaskMemFree(text);
					}

					UINT32 url_length{};
					hardware = SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &url_length));
				}
			}

			if (name.empty())
			{
				name = hardware_allowed ? "H.264 encoder" : "Microsoft H.264 software encoder";
			}

			return name + (hardware ? " (hardware)" : hardware_allowed ? "" : " (software)");
		}

		bool create_writer(const session& s, encoder& e, const bool hardware, std::string& error)
		{
			const auto check = [&error](const HRESULT hr, const char* what)
			{
				if (SUCCEEDED(hr))
				{
					return true;
				}

				error = std::format("{} failed ({})", what, hresult(hr));
				return false;
			};

			e.writer.Reset();
			e.has_samples = false;

			ComPtr<IMFAttributes> attributes;
			if (!check(mf.create_attributes(&attributes, 2), "MFCreateAttributes")) return false;
			if (!check(attributes->SetUINT32(READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE), "IMFAttributes::SetUINT32")) return false;
			// the file is "<name>.mp4.part": the container cannot be guessed from the extension
			if (!check(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4), "IMFAttributes::SetGUID")) return false;

			ComPtr<IMFSinkWriter> writer;
			if (!check(mf.create_sink_writer(s.part_path.c_str(), nullptr, attributes.Get(), &writer), "MFCreateSinkWriterFromURL")) return false;

			ComPtr<IMFMediaType> output;
			if (!check(mf.create_media_type(&output), "MFCreateMediaType")) return false;
			auto hr = set_video_attributes(output.Get(), e.width, e.height, s.fps);
			if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
			if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AVG_BITRATE, e.bitrate);
			if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
			if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(s.fps));
			if (!check(hr, "output media type")) return false;

			DWORD stream{};
			if (!check(writer->AddStream(output.Get(), &stream), "IMFSinkWriter::AddStream(H.264 High)")) return false;

			ComPtr<IMFMediaType> input;
			if (!check(mf.create_media_type(&input), "MFCreateMediaType")) return false;
			hr = set_video_attributes(input.Get(), e.width, e.height, s.fps);
			if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
			if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_DEFAULT_STRIDE, e.width);
			if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
			if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_SAMPLE_SIZE, e.width * e.height * 3 / 2);
			if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
			if (!check(hr, "input media type")) return false;

			// Encoding parameters, handed to the encoder (ICodecAPI) by the sink writer before the encoder's media types
			// are set - measured with the harness: NVIDIA's MFT ignores a GOP size set afterwards through ICodecAPI and
			// MF_MT_MAX_KEYFRAME_SPACING (default GOP 120 frames), and the Microsoft software encoder uses B-frames,
			// which the MP4 sink stores as a one-frame start offset (first frame at 1/fps instead of 0):
			//  - keyframe every second
			//  - no B-frames
			// If the encoder rejects them, the encoder's defaults are used.
			ComPtr<IMFAttributes> encoding;
			if (FAILED(mf.create_attributes(&encoding, 2)) ||
				FAILED(encoding->SetUINT32(CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(s.fps))) ||
				FAILED(encoding->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0)))
			{
				encoding.Reset();
			}

			hr = writer->SetInputMediaType(stream, input.Get(), encoding.Get());
			const auto parameters = SUCCEEDED(hr) && encoding;
			if (FAILED(hr) && encoding)
			{
				hr = writer->SetInputMediaType(stream, input.Get(), nullptr);
			}

			if (!check(hr, "IMFSinkWriter::SetInputMediaType(NV12)")) return false;

			if (!parameters)
			{
				// best effort for encoders that accept a GOP size at this point
				ComPtr<ICodecAPI> codec;
				if (SUCCEEDED(writer->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&codec))) && codec)
				{
					VARIANT value{};
					value.vt = VT_UI4;
					value.ulVal = static_cast<ULONG>(s.fps);
					codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &value);
				}
			}

			e.description = describe_encoder(writer.Get(), stream, hardware) + (parameters ? "" : ", encoder default GOP / B-frames");

			if (!check(writer->BeginWriting(), "IMFSinkWriter::BeginWriting")) return false;

			e.writer = std::move(writer);
			e.stream = stream;
			return true;
		}

		// hardware encoders first; if the writer cannot be created / configured with them, the software encoder
		bool setup_encoder(const session& s, encoder& e, const UINT width, const UINT height, std::string& error)
		{
			e.width = width & ~1u;
			e.height = height & ~1u;
			e.bitrate = choose_bitrate(e.width, e.height, s.fps, s.bitrate_mbps, s.quality);

			std::string hardware_error;
			if (create_writer(s, e, true, hardware_error))
			{
				return true;
			}

			e.writer.Reset();
			remove_file(s.part_path);

			std::string software_error;
			if (create_writer(s, e, false, software_error))
			{
				post(console::con_type_info, "hardware encoding unavailable (" + hardware_error + "), using the software encoder");
				return true;
			}

			e.writer.Reset();
			remove_file(s.part_path);
			error = std::format("could not set up the H.264 encoder for {}x{} @ {} fps: {}; software encoder: {}",
			                    e.width, e.height, s.fps, hardware_error, software_error);
			return false;
		}

		bool encode_frame(session& s, encoder& e, const cpu_frame& frame, band_workers& workers, std::string& error)
		{
			const auto* info = theater_capture::find_format(frame.format);
			if (!info || frame.width < MIN_DIMENSION || frame.height < MIN_DIMENSION)
			{
				error = "invalid frame";
				return false;
			}

			if ((frame.width & ~1u) != e.width || (frame.height & ~1u) != e.height)
			{
				if (e.has_samples)
				{
					error = std::format("the frame size changed to {}x{}", frame.width, frame.height);
					return false;
				}

				// the back buffer is not the size the engine's window table had at start(): set up again (nothing
				// has been written yet)
				post(console::con_type_info, std::format("the back buffer is {}x{}, not {}x{}: setting the encoder up again",
				                                         frame.width, frame.height, e.width, e.height));
				e.writer.Reset();
				remove_file(s.part_path);
				if (!setup_encoder(s, e, frame.width, frame.height, error))
				{
					return false;
				}

				std::lock_guard _(s.mutex);
				s.encoder_description = e.description;
			}

			const auto size = static_cast<DWORD>(static_cast<std::size_t>(e.width) * e.height * 3 / 2);

			ComPtr<IMFMediaBuffer> buffer;
			auto hr = mf.create_memory_buffer(size, &buffer);
			if (FAILED(hr) || !buffer)
			{
				error = std::format("MFCreateMemoryBuffer({}) failed ({})", size, hresult(hr));
				return false;
			}

			BYTE* data{};
			DWORD max_length{};
			hr = buffer->Lock(&data, &max_length, nullptr);
			if (FAILED(hr) || !data || max_length < size)
			{
				if (SUCCEEDED(hr)) buffer->Unlock();
				error = std::format("IMFMediaBuffer::Lock failed ({})", hresult(hr));
				return false;
			}

			conversion c{};
			c.source = frame.pixels.get();
			c.source_pitch = static_cast<std::size_t>(frame.width) * info->bytes_per_pixel;
			c.width = e.width;
			c.luma = data;
			c.chroma = data + static_cast<std::size_t>(e.width) * e.height;
			c.half_table = info->layout == pixel_layout::rgba16f ? theater_capture::half_to_srgb8_table().data() : nullptr;

			const auto pairs = static_cast<std::size_t>(e.height / 2);
			const auto pixels = static_cast<std::size_t>(e.width) * e.height;
			const auto bands = pixels < 512 * 1024 ? 1u : std::min<unsigned>(static_cast<unsigned>(pairs), (workers.size() + 1) * 4);

			workers.run(bands, [&](const unsigned band)
			{
				const auto begin = pairs * band / bands;
				const auto end = pairs * (band + 1) / bands;
				switch (info->layout)
				{
				case pixel_layout::rgba8: convert_rows<pixel_layout::rgba8>(c, begin, end); break;
				case pixel_layout::bgra8: convert_rows<pixel_layout::bgra8>(c, begin, end); break;
				case pixel_layout::rgb10a2: convert_rows<pixel_layout::rgb10a2>(c, begin, end); break;
				case pixel_layout::rgba16f: convert_rows<pixel_layout::rgba16f>(c, begin, end); break;
				}
			});

			buffer->Unlock();
			hr = buffer->SetCurrentLength(size);
			if (FAILED(hr))
			{
				error = std::format("IMFMediaBuffer::SetCurrentLength failed ({})", hresult(hr));
				return false;
			}

			ComPtr<IMFSample> sample;
			hr = mf.create_sample(&sample);
			if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
			// exact integer timestamps: frame i starts at i * 10'000'000 / fps (64-bit)
			if (SUCCEEDED(hr)) hr = sample->SetSampleTime(static_cast<LONGLONG>(frame.index * TICKS_PER_SECOND / static_cast<std::uint64_t>(s.fps)));
			if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(static_cast<LONGLONG>(TICKS_PER_SECOND / static_cast<std::uint64_t>(s.fps)));
			if (FAILED(hr))
			{
				error = std::format("creating the sample failed ({})", hresult(hr));
				return false;
			}

			// blocks while the encoder is behind (sink writer throttling): backpressure up to the game's Present
			hr = e.writer->WriteSample(e.stream, sample.Get());
			if (FAILED(hr))
			{
				error = std::format("IMFSinkWriter::WriteSample failed ({})", hresult(hr));
				return false;
			}

			e.has_samples = true;
			++s.frames_written;
			return true;
		}

		void encode_frames(session& s, encoder& e)
		{
			const auto cores = std::max(1u, std::thread::hardware_concurrency());
			band_workers workers(std::min(3u, cores / 4));

			for (;;)
			{
				cpu_frame* frame{};
				{
					std::unique_lock lock(s.mutex);
					s.encoder_cv.wait_for(lock, 100ms, [&s]() { return s.abort || !s.queue.empty() || s.input_closed; });
					if (s.abort)
					{
						return;
					}

					if (!s.queue.empty())
					{
						frame = s.queue.front();
						s.queue.pop_front();
					}
					else if (s.input_closed)
					{
						return;
					}
					else if (s.st.load() != state::capturing && s.stop_time != std::chrono::steady_clock::time_point{} &&
						std::chrono::steady_clock::now() - std::max(s.stop_time, s.last_progress) > FLUSH_TIMEOUT)
					{
						// stop() / error, but the game has not presented since: give up on the frames in the ring. Measured
						// from the last encoded frame too: a slow encoder still working through the queue after stop()
						// must not make it give up while the render side is still flushing the ring into it
						s.input_closed = true;
						lock.unlock();
						post(console::con_type_warning, "no frame was presented after the recording stopped, the last frames are missing");
						return;
					}
					else
					{
						continue;
					}
				}

				std::string error;
				const auto failed = [&s]()
				{
					std::lock_guard lock(s.mutex);
					return s.encoder_failed;
				}();

				const auto ok = failed || encode_frame(s, e, *frame, workers, error);

				{
					std::lock_guard lock(s.mutex);
					s.free_frames.push_back(frame);
					s.last_progress = std::chrono::steady_clock::now();
					if (!ok)
					{
						s.encoder_failed = true;
					}
				}

				s.render_cv.notify_all();

				if (!ok)
				{
					// drops the frames still queued; the render side stops at its next Present
					record_error(s, "encoding failed: " + error);
				}
			}
		}

		std::filesystem::path free_path(const std::filesystem::path& path)
		{
			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
			{
				return path;
			}

			auto base = path;
			base.replace_extension();
			for (auto i = 2; i < 1000; ++i)
			{
				auto candidate = base;
				candidate += std::format("_{}", i);
				candidate += path.extension();
				if (!std::filesystem::exists(candidate, ec))
				{
					return candidate;
				}
			}

			return path;
		}

		// system error text without the trailing period / line break Windows puts there
		std::string system_message(const std::error_code& ec)
		{
			auto text = ec.message();
			while (!text.empty() && (text.back() == '.' || text.back() == ' ' || text.back() == '\r' || text.back() == '\n'))
			{
				text.pop_back();
			}

			return text;
		}

		bool rename_file(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error)
		{
			std::error_code ec;
			for (auto attempt = 0; attempt < 40; ++attempt)
			{
				std::filesystem::rename(from, to, ec);
				if (!ec)
				{
					return true;
				}

				Sleep(50);
			}

			error = "could not rename " + display(from.filename()) + " (" + system_message(ec) + ")";
			return false;
		}

		void finalize(session& s, encoder& e)
		{
			const auto aborted = [&s]()
			{
				std::lock_guard lock(s.mutex);
				return s.abort;
			};

			std::string error;
			auto ok = false;
			if (!aborted())
			{
				if (e.writer && e.has_samples)
				{
					const auto hr = e.writer->Finalize();
					ok = SUCCEEDED(hr);
					if (!ok)
					{
						error = std::format("IMFSinkWriter::Finalize failed ({}), the video was not saved", hresult(hr));
					}
				}
				else
				{
					error = "no frames were captured, nothing was saved";
				}
			}

			e.writer.Reset(); // closes the file

			std::filesystem::path final_path;
			std::string rename_error;
			auto kept_part = false;
			if (ok)
			{
				final_path = free_path(s.path);
				if (!rename_file(s.part_path, final_path, rename_error))
				{
					// another program keeps the .part file open. The video itself is complete: a copy under the final
					// name usually still works (readers that block a rename allow reading); if not, it stays under its
					// temporary name - never deleted (a later start() for the same name would remove a leftover .part,
					// which is why the copy is tried first)
					std::error_code ec;
					if (std::filesystem::copy_file(s.part_path, final_path, std::filesystem::copy_options::none, ec))
					{
						remove_file(s.part_path); // best effort
					}
					else
					{
						if (ec != std::errc::file_exists)
						{
							std::error_code ignored;
							std::filesystem::remove(final_path, ignored); // partial copy
						}

						rename_error += ", copying it failed too (" + system_message(ec) + ")";
						final_path = s.part_path;
						kept_part = true;
					}
				}
			}
			else
			{
				remove_file(s.part_path);
			}

			const auto frames = s.frames_written.load();
			auto cancelled = false;
			auto earlier_error = false;
			{
				std::lock_guard lock(s.mutex);
				cancelled = s.abort;
				earlier_error = s.error_reported;
				if (!cancelled && ok)
				{
					s.published = true;
					std::lock_guard _(result_mutex);
					result = final_path;
				}
			}

			if (cancelled)
			{
				if (ok)
				{
					remove_file(final_path); // cancel() while it was being finalized
				}

				post(console::con_type_info, "recording cancelled, the file was deleted");
			}
			else if (ok)
			{
				if (kept_part)
				{
					post(console::con_type_warning, rename_error + ", the video was kept under its temporary name");
				}
				else if (!rename_error.empty())
				{
					post(console::con_type_info, rename_error + ", copied it instead");
				}

				post(console::con_type_info, std::format("saved {} ({} frames, {:.2f} s at {} fps)", display(final_path), frames,
				                                         static_cast<double>(frames) / s.fps, s.fps));
			}
			else if (!e.has_samples && earlier_error)
			{
				// the error that stopped the capture stays the one take_error() returns
				post(console::con_type_info, "nothing was saved");
			}
			else
			{
				report_error(error);
			}
		}

		void encoder_main(const std::shared_ptr<session> s)
		{
			utils::thread::set_name(L"theater video encoder");

			auto setup_reported = false;
			const auto report_setup = [&](const bool ok, const std::string& error, const std::string& description)
			{
				{
					std::lock_guard lock(s->mutex);
					s->setup_done = true;
					s->setup_ok = ok;
					s->setup_error = error;
					s->encoder_description = description;
				}

				s->setup_cv.notify_all();
				setup_reported = true;
			};

			const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			auto mf_started = false;

			try
			{
				const auto hr = mf.startup(MF_VERSION, MFSTARTUP_LITE);
				mf_started = SUCCEEDED(hr);

				encoder e{};
				std::string error;
				auto ok = false;
				if (mf_started)
				{
					ok = setup_encoder(*s, e, s->expected_width, s->expected_height, error);
				}
				else
				{
					error = "MFStartup failed (" + hresult(hr) + ")";
				}

				report_setup(ok, error, e.description);

				const auto aborted = [&s]()
				{
					std::lock_guard lock(s->mutex);
					return s->abort;
				};

				if (ok && !aborted())
				{
					encode_frames(*s, e);
					finalize(*s, e);
				}
				else
				{
					// start() failed or timed out: it reports the error itself
					e.writer.Reset();
					remove_file(s->part_path);
				}
			}
			catch (const std::exception& ex)
			{
				if (!setup_reported)
				{
					report_setup(false, std::string("encoder thread: ") + ex.what(), {});
				}
				else
				{
					report_error(std::string("encoder thread failed: ") + ex.what());
				}

				remove_file(s->part_path);
			}
			catch (...)
			{
				if (!setup_reported)
				{
					report_setup(false, "encoder thread: unknown exception", {});
				}
				else
				{
					report_error("encoder thread failed (unknown exception)");
				}

				remove_file(s->part_path);
			}

			{
				std::lock_guard lock(s->mutex);
				s->input_closed = true;
				s->encoder_failed = true; // a render side that is still running drops everything from now on
				s->encoder_done = true;
				if (s->render_closed)
				{
					release_frames(*s);
				}
				// otherwise finish_render() releases them (or the session's destructor, if the render side never runs again)
			}

			s->render_cv.notify_all();

			if (mf_started)
			{
				mf.shutdown();
			}

			if (SUCCEEDED(com))
			{
				CoUninitialize();
			}

			s->st = state::finished;
		}

		// ------------------------------------------------------------------ main thread

		struct test_recording
		{
			bool running{};
			double seconds{};
			std::chrono::steady_clock::time_point begin{};
		};

		test_recording test{};

		std::string timestamp()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		}

		// renderer pipeline: drives the real-time test recording
		void on_frame()
		{
			if (!test.running || !theater_capture::on_main_thread())
			{
				return;
			}

			if (!capturing())
			{
				test.running = false;
				return;
			}

			const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - test.begin).count();
			if (elapsed < test.seconds)
			{
				set_progress(static_cast<float>(elapsed / test.seconds));
				return;
			}

			test.running = false;
			stop();
			console::info("theater_video: test recording stopped after %.1f s (%llu frames encoded so far), finalizing\n",
			              elapsed, static_cast<unsigned long long>(frames()));
		}

		void add_commands()
		{
			command::add("theater_video_test", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: theater_video_test <seconds> [fps]   (records the screen in real time to %s)\n",
					              display(default_folder()).data());
					return;
				}

				const auto seconds = std::atof(params[1]);
				const auto fps = params.size() > 2 ? std::atoi(params[2]) : DEFAULT_TEST_FPS;
				if (!(seconds > 0.0 && seconds <= 3600.0))
				{
					console::warn("theater_video: invalid duration '%s'\n", params[1]);
					return;
				}

				if (!start(default_folder() / ("test_" + timestamp() + ".mp4"), fps))
				{
					return; // start() printed why
				}

				test.running = true;
				test.seconds = seconds;
				test.begin = std::chrono::steady_clock::now();
				set_progress(0.0f);
			});

			command::add("theater_video_stop", []()
			{
				test.running = false;
				if (!capturing())
				{
					console::info("theater_video: not recording\n");
					return;
				}

				stop();
				console::info("theater_video: stopped (%llu frames encoded so far), finalizing\n", static_cast<unsigned long long>(frames()));
			});

			command::add("theater_video_cancel", []()
			{
				test.running = false;
				if (!busy())
				{
					console::info("theater_video: not recording\n");
					return;
				}

				cancel();
			});
		}
	}

	bool start(const std::filesystem::path& path, const int fps, const int skip_frames)
	{
		if (!theater_capture::on_main_thread())
		{
			report_error("start() was not called on the game's main thread");
			return false;
		}

		if (busy())
		{
			return fail_now("the previous video is still being recorded or finalized");
		}

		// a new video: an unclaimed result / error of an earlier one (theater_video_test never claims its result,
		// a render cancelled while it was being published neither) was already printed and must not be returned as
		// this video's outcome - theater.cpp reports take_result() as "video saved" after a failed render
		{
			std::lock_guard _(result_mutex);
			result.reset();
			last_error.clear();
		}

		if (fps < 1 || fps > MAX_FPS)
		{
			return fail_now(std::format("invalid frame rate {} (1..{})", fps, MAX_FPS));
		}

		if (skip_frames < 0 || skip_frames > MAX_SKIP_FRAMES)
		{
			return fail_now(std::format("invalid skip_frames {}", skip_frames));
		}

		if (path.empty() || !path.has_filename())
		{
			return fail_now("invalid file name");
		}

		std::string error;
		if (!load_media_foundation(error))
		{
			return fail_now(error);
		}

		// the size the engine created its swap chain with (theater_capture.cpp), read without calling into DXGI off
		// the render thread; the encoder is set up again if the first frame turns out to be different
		const auto window = theater_capture::read_engine_window();
		auto width = window.width;
		auto height = window.height;
		if (width < MIN_DIMENSION || height < MIN_DIMENSION || width > MAX_DIMENSION || height > MAX_DIMENSION)
		{
			RECT rect{};
			const auto hwnd = theater_capture::read_main_window();
			if (hwnd && GetClientRect(hwnd, &rect))
			{
				width = static_cast<UINT>(rect.right - rect.left);
				height = static_cast<UINT>(rect.bottom - rect.top);
			}
		}

		if (width < MIN_DIMENSION || height < MIN_DIMENSION || width > MAX_DIMENSION || height > MAX_DIMENSION)
		{
			return fail_now(std::format("unsupported game resolution {}x{}", width, height));
		}

		if (!theater_capture::ensure_hook())
		{
			return fail_now("the Direct3D 11 Present hook is not available");
		}

		auto s = std::make_shared<session>();
		try
		{
			s->path = std::filesystem::absolute(path);
			s->part_path = s->path;
			s->part_path += ".part";

			std::error_code ec;
			std::filesystem::create_directories(s->path.parent_path(), ec);
			if (!std::filesystem::is_directory(s->path.parent_path(), ec))
			{
				return fail_now("cannot create the folder " + display(s->path.parent_path()));
			}

			std::filesystem::remove(s->part_path, ec); // left over by a crash
		}
		catch (const std::exception& e)
		{
			return fail_now(std::string("invalid file name: ") + e.what());
		}

		s->fps = fps;
		s->skip_frames = skip_frames;
		s->expected_width = width;
		s->expected_height = height;
		s->bitrate_mbps = bitrate_dvar ? bitrate_dvar->current.integer : 0;
		s->quality = quality_dvar ? quality_dvar->current.integer : DEFAULT_VIDEO_QUALITY;
		s->last_progress = std::chrono::steady_clock::now();

		const auto frame_bytes = static_cast<std::size_t>(width) * height * 4;
		s->max_frames = std::clamp(FRAME_POOL_BYTES / frame_bytes, MIN_POOL_FRAMES, MAX_POOL_FRAMES);

		try
		{
			std::thread(encoder_main, s).detach();
		}
		catch (const std::exception& e)
		{
			return fail_now(std::string("could not start the encoder thread: ") + e.what());
		}

		// Media Foundation needs a moment to create the encoder (hardware MFTs: up to a few hundred ms); the
		// encoder thread never waits for the main thread
		{
			std::unique_lock lock(s->mutex);
			if (!s->setup_cv.wait_for(lock, SETUP_TIMEOUT, [&s]() { return s->setup_done; }))
			{
				s->abort = true;
				lock.unlock();
				s->encoder_cv.notify_all();
				return fail_now("the encoder did not start in time");
			}

			if (!s->setup_ok)
			{
				const auto setup_error = s->setup_error;
				lock.unlock();
				return fail_now(setup_error);
			}
		}

		const auto bitrate = choose_bitrate(width & ~1u, height & ~1u, fps, s->bitrate_mbps, s->quality);
		std::string description;
		{
			std::lock_guard lock(s->mutex);
			description = s->encoder_description;
		}

		{
			std::lock_guard _(session_mutex);
			current_session = s;
			render_needed = true;
		}

		theater_capture::update(theater_capture::client::video); // Present callbacks from now on

		console::info("theater_video: recording %ux%u @ %d fps, H.264 High %.1f Mbps, %s -> %s\n", width & ~1u, height & ~1u, fps,
		              bitrate / 1e6, description.data(), display(s->path).data());
		return true;
	}

	void stop()
	{
		progress = -1.0f;

		const auto s = get_session();
		if (!s)
		{
			return;
		}

		auto expected = state::capturing;
		if (s->st.compare_exchange_strong(expected, state::stopping))
		{
			{
				std::lock_guard lock(s->mutex);
				s->stop_time = std::chrono::steady_clock::now();
			}

			s->encoder_cv.notify_all();
		}
	}

	void cancel()
	{
		progress = -1.0f;

		const auto s = get_session();
		if (!s)
		{
			return;
		}

		{
			std::lock_guard lock(s->mutex);
			if (s->published)
			{
				return; // already saved
			}

			s->abort = true;
			if (s->stop_time == std::chrono::steady_clock::time_point{})
			{
				s->stop_time = std::chrono::steady_clock::now();
			}
		}

		for (auto expected : {state::capturing, state::stopping})
		{
			if (s->st.compare_exchange_strong(expected, state::cancelled))
			{
				break;
			}
		}

		s->encoder_cv.notify_all();
		s->render_cv.notify_all();
	}

	bool capturing()
	{
		const auto s = get_session();
		return s && s->st.load() == state::capturing;
	}

	bool busy()
	{
		const auto s = get_session();
		return s && s->st.load() != state::finished;
	}

	std::uint64_t frames()
	{
		const auto s = get_session();
		return s ? s->frames_written.load() : 0;
	}

	std::optional<std::filesystem::path> take_result()
	{
		std::lock_guard _(result_mutex);
		auto r = std::move(result);
		result.reset();
		return r;
	}

	std::string take_error()
	{
		std::lock_guard _(result_mutex);
		auto e = std::move(last_error);
		last_error.clear();
		return e;
	}

	void set_progress(const float fraction)
	{
		progress = fraction;
	}

	std::filesystem::path default_folder()
	{
		std::filesystem::path base;

		PWSTR known{};
		if (SUCCEEDED(SHGetKnownFolderPath(VIDEOS_FOLDER, KF_FLAG_CREATE, nullptr, &known)) && known)
		{
			base = known;
		}

		CoTaskMemFree(known);

		auto folder = base.empty() ? console::get_theater_dir() / "videos" : base / "IW6X Theater";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		return folder;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			theater_capture::set_callbacks(theater_capture::client::video, {before_present, after_present, needs_present});

			// Dvar_RegisterInt 0x1404EE270 (iw6-mod symbols.hpp): engine callers pass (name, value, min, max, flags,
			// description), e.g. FUN_1404fb1d0 ("friendsWidgetMinimumRefreshTimer", 30000, 0, 0x7fffffff, 0, ...) and
			// FUN_14039fd20 ("scr_%s_scorelimit", 0, INT_MIN, INT_MAX, 0x180, "script score limit")
			bitrate_dvar = game::Dvar_RegisterInt("theater_video_bitrate", 0, 0, 150, game::DVAR_FLAG_SAVED,
			                                      "Theater video bitrate in Mbps (0 = from theater_video_quality, the video size and the fps; 8..150)");
			quality_dvar = game::Dvar_RegisterInt("theater_video_quality", DEFAULT_VIDEO_QUALITY, 0, static_cast<int>(QUALITY_BITS_PER_PIXEL.size()) - 1,
			                                      game::DVAR_FLAG_SAVED,
			                                      "Theater video quality while theater_video_bitrate is 0: 0 low, 1 medium, 2 high, 3 ultra, 4 max "
			                                      "(width x height x fps x 0.1 / 0.2 / 0.3 / 0.5 / 0.8 bits per second)");

			add_commands();
			scheduler::loop(on_frame, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(theater_video::component)
