#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "theater_screenshot.hpp"
#include "theater_capture.hpp"

#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"
#include "demo_playback.hpp"

#include "game/game.hpp"
#include "utils/thread.hpp"

#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib") // WIC GUID constants only, the encoder itself is created through COM

// Theater SCREENSHOT (BO2 "Hold R / Hold X"): reads the frame back from the game's D3D11 swap chain.
//
// Flow (threads):
//  request()            main thread  (installs the Present hook on first use, theater_capture::ensure_hook)
//                                    idle -> waiting_ui (console / menu open) -> hiding
//  renderer pipeline    main thread  hiding: the theater HUD is skipped (hide_theater_hud()); after CLEAN_FRAMES
//                                    complete frames were submitted without it -> armed
//  IDXGISwapChain::     render       armed: GetBuffer(0) (+ ResolveSubresource if multisampled), copy into a
//  Present hook         thread       STAGING texture -> reading (the HUD is drawn again from the next frame)
//  (theater_capture)                 reading: Map(DO_NOT_WAIT) on the following Presents (blocking only after
//                                    MAX_MAP_POLLS frames), copy the rows out (at most COPY_BYTES_PER_FRAME
//                                    per Present, the texture stays mapped meanwhile) -> encoding
//  worker thread                     converts to 24-bit BGR, writes the PNG with WIC -> idle, take_result()
//
// The Present callback runs inside the engine's own Present wrapper, which holds the renderer's device mutex,
// so using the immediate context there is serialized with the game's own use of it (theater_capture.cpp has
// the evidence). Nothing is printed from the render thread or the worker (Conbuf_AppendText does several
// SendMessage calls): messages are queued (theater_capture::post) and printed by the renderer pipeline, and
// only when it runs on the main thread (during loading screens the engine runs R_EndFrame on the render thread).
//
// The hook is only installed by the first request() (or the first theater video), so a normal (online)
// session never has dxgi.dll's Present patched unless a theater capture is actually made.
//
// Engine evidence (iw6mp64_ship.exe 3.15, Ghidra on the decrypted dump; swap chain / Present / device mutex
// evidence: theater_capture.cpp):
//  - game::keyCatchers (0x1419E1ADC, bit 0 = console) comes from iw6-mod's symbols.hpp; theater.cpp uses
//    keyCatchers the same way.
//  - Frame lag (why CLEAN_FRAMES): FUN_140602490 (issue, after R_EndFrame) counts a frame as outstanding
//    (FUN_1404245d0: 0x1446B4B78++), the render thread (FUN_14062b390) decrements it (FUN_140423a30) right
//    after executing the frame and only then presents it (FUN_14062b960). The frontend waits for
//    "outstanding < 2" (FUN_1404239c0) before it starts a new frame (FUN_140602d70, double buffered at
//    0x148111808). So while the main thread is in R_EndFrame for frame N, the render thread may still be
//    about to present frame N - 2, never an older one.

namespace theater_screenshot
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using theater_capture::pixel_layout;

		// ------------------------------------------------------------------ tuning

		// complete frames submitted without the theater HUD before the capture is armed: the engine's render
		// thread can lag 2 frames behind the main thread (see "Frame lag" above), +1 so a request() that comes
		// after the theater HUD was already drawn into the current frame is still safe
		constexpr int CLEAN_FRAMES = 3;
		constexpr int MAX_MAP_POLLS = 8;   // non-blocking Map attempts (one per Present) before a blocking Map
		constexpr std::size_t COPY_BYTES_PER_FRAME = 4 * 1024 * 1024; // rows copied out of the mapped texture per Present
		constexpr UINT MAX_DIMENSION = 16384;
		constexpr auto UI_WAIT_TIMEOUT = 30s;
		constexpr auto UI_SETTLE_TIME = 300ms; // menus still fade out for a moment after keyCatchers is cleared
		constexpr auto PRESENT_TIMEOUT = 3s;

		// ------------------------------------------------------------------ state

		enum class phase
		{
			idle,
			waiting_ui, // requested while the console or a menu is open
			hiding,     // theater HUD hidden, counting frames (main thread)
			armed,      // capture at the next Present of the game's swap chain (render thread)
			reading,    // GPU copy queued, waiting for Map (render thread)
			encoding,   // worker thread converts and writes the PNG
		};

		std::atomic<phase> current{phase::idle};

		// main thread
		std::chrono::steady_clock::time_point request_time{};
		std::chrono::steady_clock::time_point ui_closed_since{};
		std::chrono::steady_clock::time_point armed_time{};
		std::chrono::steady_clock::time_point last_busy_warning{};
		int hidden_frames{};

		// written by request() before the capture is armed, read by the render thread once it sees 'armed'
		std::filesystem::path pending_base{};

		std::mutex result_mutex;
		std::optional<std::filesystem::path> result;

		// from the render thread and the worker: printed by the main thread
		void post(const int type, const std::string& text)
		{
			theater_capture::post(type, "theater_screenshot: " + text);
		}

		// the Present callbacks are only needed while a capture is armed or being read back
		bool needs_present()
		{
			const auto p = current.load();
			return p == phase::armed || p == phase::reading;
		}

		// ------------------------------------------------------------------ PNG writer (worker thread)

		struct capture_job
		{
			std::filesystem::path base_path{}; // without extension
			DXGI_FORMAT format{};
			UINT width{};
			UINT height{};
			std::unique_ptr<std::uint8_t[]> pixels{}; // tightly packed rows in 'format'
		};

		bool convert_to_bgr24(const capture_job& job, std::uint8_t* out, const std::size_t out_stride)
		{
			const auto* info = theater_capture::find_format(job.format);
			if (!info || !job.pixels)
			{
				return false;
			}

			const auto in_stride = static_cast<std::size_t>(job.width) * info->bytes_per_pixel;

			for (std::size_t y = 0; y < job.height; ++y)
			{
				const auto* s = job.pixels.get() + y * in_stride;
				auto* d = out + y * out_stride;

				switch (info->layout)
				{
				case pixel_layout::rgba8:
					for (std::size_t x = 0; x < job.width; ++x, s += 4, d += 3)
					{
						d[0] = s[2];
						d[1] = s[1];
						d[2] = s[0];
					}
					break;

				case pixel_layout::bgra8:
					for (std::size_t x = 0; x < job.width; ++x, s += 4, d += 3)
					{
						d[0] = s[0];
						d[1] = s[1];
						d[2] = s[2];
					}
					break;

				case pixel_layout::rgb10a2:
					for (std::size_t x = 0; x < job.width; ++x, s += 4, d += 3)
					{
						std::uint32_t v{};
						std::memcpy(&v, s, sizeof(v));
						d[0] = theater_capture::unorm10_to_8((v >> 20) & 0x3FFu);
						d[1] = theater_capture::unorm10_to_8((v >> 10) & 0x3FFu);
						d[2] = theater_capture::unorm10_to_8(v & 0x3FFu);
					}
					break;

				case pixel_layout::rgba16f:
				{
					const auto& table = theater_capture::half_to_srgb8_table();
					for (std::size_t x = 0; x < job.width; ++x, s += 8, d += 3)
					{
						std::uint16_t h[4]{};
						std::memcpy(h, s, sizeof(h));
						d[0] = table[h[2]];
						d[1] = table[h[1]];
						d[2] = table[h[0]];
					}
					break;
				}
				}
			}

			return true;
		}

		bool encode_png(const std::filesystem::path& path, const UINT width, const UINT height, const UINT stride,
		                const std::uint8_t* bgr, std::string& error)
		{
			const auto fail = [&error](const char* what, const HRESULT hr)
			{
				error = std::format("{} failed (0x{:08X})", what, static_cast<std::uint32_t>(hr));
				return false;
			};

			ComPtr<IWICImagingFactory> factory;
			auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
			if (FAILED(hr))
			{
				// Windows 7 without the platform update only has the version 1 factory
				hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
			}
			if (FAILED(hr) || !factory) return fail("CoCreateInstance(WICImagingFactory)", hr);

			ComPtr<IWICStream> stream;
			hr = factory->CreateStream(&stream);
			if (FAILED(hr)) return fail("IWICImagingFactory::CreateStream", hr);

			hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
			if (FAILED(hr)) return fail("IWICStream::InitializeFromFilename", hr);

			ComPtr<IWICBitmapEncoder> encoder;
			hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
			if (FAILED(hr)) return fail("IWICImagingFactory::CreateEncoder(PNG)", hr);

			hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
			if (FAILED(hr)) return fail("IWICBitmapEncoder::Initialize", hr);

			ComPtr<IWICBitmapFrameEncode> frame;
			ComPtr<IPropertyBag2> properties;
			hr = encoder->CreateNewFrame(&frame, &properties);
			if (FAILED(hr)) return fail("IWICBitmapEncoder::CreateNewFrame", hr);

			hr = frame->Initialize(properties.Get());
			if (FAILED(hr)) return fail("IWICBitmapFrameEncode::Initialize", hr);

			hr = frame->SetSize(width, height);
			if (FAILED(hr)) return fail("IWICBitmapFrameEncode::SetSize", hr);

			WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
			hr = frame->SetPixelFormat(&pixel_format);
			if (FAILED(hr)) return fail("IWICBitmapFrameEncode::SetPixelFormat", hr);
			if (!IsEqualGUID(pixel_format, GUID_WICPixelFormat24bppBGR))
			{
				error = "the PNG encoder does not accept 24-bit BGR pixels";
				return false;
			}

			hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgr));
			if (FAILED(hr)) return fail("IWICBitmapFrameEncode::WritePixels", hr);

			hr = frame->Commit();
			if (FAILED(hr)) return fail("IWICBitmapFrameEncode::Commit", hr);

			hr = encoder->Commit();
			if (FAILED(hr)) return fail("IWICBitmapEncoder::Commit", hr);

			return true;
		}

		std::filesystem::path unique_png_path(const std::filesystem::path& base)
		{
			std::error_code ec;
			auto candidate = base;
			candidate += ".png";

			for (auto i = 2; std::filesystem::exists(candidate, ec) && i < 1000; ++i)
			{
				candidate = base;
				candidate += std::format("_{}.png", i);
			}

			return candidate;
		}

		bool save_png(const capture_job& job, std::filesystem::path& path, std::string& error)
		{
			const auto stride = static_cast<std::size_t>(job.width) * 3;
			const std::unique_ptr<std::uint8_t[]> bgr(new (std::nothrow) std::uint8_t[stride * job.height]);
			if (!bgr)
			{
				error = "out of memory";
				return false;
			}

			if (!convert_to_bgr24(job, bgr.get(), stride))
			{
				error = "pixel conversion failed";
				return false;
			}

			std::error_code ec;
			std::filesystem::create_directories(job.base_path.parent_path(), ec);

			path = unique_png_path(job.base_path);
			auto temp = path;
			temp += ".tmp";

			const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const auto ok = encode_png(temp, job.width, job.height, static_cast<UINT>(stride), bgr.get(), error);
			if (SUCCEEDED(com))
			{
				CoUninitialize();
			}

			if (ok)
			{
				std::filesystem::rename(temp, path, ec);
				if (!ec)
				{
					return true;
				}

				error = "could not rename the temporary file: " + ec.message();
			}

			std::filesystem::remove(temp, ec);
			return false;
		}

		void encoder_thread(capture_job job)
		{
			utils::thread::set_name(L"theater screenshot");
			// the PNG compression must not take CPU time away from the game's main / render / worker threads
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

			try
			{
				std::filesystem::path path;
				std::string error;
				const auto ok = save_png(job, path, error);
				job.pixels.reset();

				if (ok)
				{
					{
						std::lock_guard _(result_mutex);
						result = path;
					}

					post(console::con_type_info, "saved " + path.string());
				}
				else
				{
					post(console::con_type_warning, "could not save the screenshot: " + error);
				}
			}
			catch (const std::exception& e)
			{
				post(console::con_type_warning, std::string("could not save the screenshot: ") + e.what());
			}
			catch (...)
			{
				post(console::con_type_warning, "could not save the screenshot (unknown error)");
			}

			current = phase::idle;
		}

		// ------------------------------------------------------------------ GPU readback (inside Present)

		struct readback_state
		{
			ComPtr<ID3D11DeviceContext> context{};
			ComPtr<ID3D11Texture2D> staging{};
			int polls{};
			const std::uint8_t* mapped{}; // staging texture stays mapped while its rows are copied over several frames
			std::size_t row_pitch{};
			std::size_t rows_done{};
			capture_job job{};
		};

		// only touched inside the game's Present (serialized by the engine's device mutex). Intentionally never
		// destroyed: a static destructor would Release the D3D objects of an unfinished capture from
		// DLL_PROCESS_DETACH, possibly after d3d11.dll has already been detached.
		readback_state& readback = *new readback_state{};

		void release_readback()
		{
			if (readback.mapped && readback.context && readback.staging)
			{
				readback.context->Unmap(readback.staging.Get(), 0);
			}

			readback = {};
		}

		bool begin_readback(IDXGISwapChain* swap_chain, std::string& error)
		{
			ComPtr<ID3D11Texture2D> back_buffer;
			auto hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
			if (FAILED(hr) || !back_buffer)
			{
				error = std::format("IDXGISwapChain::GetBuffer(ID3D11Texture2D) failed (0x{:08X}), not a Direct3D 11 swap chain?",
				                    static_cast<std::uint32_t>(hr));
				return false;
			}

			D3D11_TEXTURE2D_DESC desc{};
			back_buffer->GetDesc(&desc);

			const auto* info = theater_capture::find_format(desc.Format);
			if (!info)
			{
				error = std::format("unsupported back buffer format (DXGI_FORMAT {})", static_cast<int>(desc.Format));
				return false;
			}

			if (desc.Width == 0 || desc.Height == 0 || desc.Width > MAX_DIMENSION || desc.Height > MAX_DIMENSION)
			{
				error = std::format("unexpected back buffer size {}x{}", desc.Width, desc.Height);
				return false;
			}

			ComPtr<ID3D11Device> device;
			back_buffer->GetDevice(&device);
			ComPtr<ID3D11DeviceContext> context;
			if (device)
			{
				device->GetImmediateContext(&context);
			}

			if (!device || !context)
			{
				error = "the back buffer has no device / immediate context";
				return false;
			}

			// CPU copy allocated now: nothing may fail between Map and Unmap later
			const auto size = static_cast<std::size_t>(desc.Width) * desc.Height * info->bytes_per_pixel;
			std::unique_ptr<std::uint8_t[]> pixels(new (std::nothrow) std::uint8_t[size]);
			if (!pixels)
			{
				error = std::format("out of memory ({} bytes)", size);
				return false;
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

			ComPtr<ID3D11Texture2D> staging;
			hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
			if (FAILED(hr) || !staging)
			{
				error = std::format("CreateTexture2D(STAGING) failed (0x{:08X})", static_cast<std::uint32_t>(hr));
				return false;
			}

			if (desc.SampleDesc.Count > 1)
			{
				UINT support{};
				if (FAILED(device->CheckFormatSupport(info->typed_format, &support)) ||
					!(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE))
				{
					error = std::format("multisampled back buffer format {} cannot be resolved", static_cast<int>(desc.Format));
					return false;
				}

				auto resolved_desc = staging_desc;
				resolved_desc.Usage = D3D11_USAGE_DEFAULT;
				resolved_desc.CPUAccessFlags = 0;

				ComPtr<ID3D11Texture2D> resolved;
				hr = device->CreateTexture2D(&resolved_desc, nullptr, &resolved);
				if (FAILED(hr) || !resolved)
				{
					resolved_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
					hr = device->CreateTexture2D(&resolved_desc, nullptr, &resolved);
				}

				if (FAILED(hr) || !resolved)
				{
					error = std::format("CreateTexture2D(resolve target) failed (0x{:08X})", static_cast<std::uint32_t>(hr));
					return false;
				}

				context->ResolveSubresource(resolved.Get(), 0, back_buffer.Get(), 0, info->typed_format);
				context->CopyResource(staging.Get(), resolved.Get());
			}
			else
			{
				context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, back_buffer.Get(), 0, nullptr);
			}

			readback.context = std::move(context);
			readback.staging = std::move(staging);
			readback.polls = 0;
			readback.job.base_path = pending_base;
			readback.job.format = desc.Format;
			readback.job.width = desc.Width;
			readback.job.height = desc.Height;
			readback.job.pixels = std::move(pixels);
			return true;
		}

		enum class poll_status
		{
			pending,
			done,
			failed,
		};

		poll_status poll_readback(std::string& error)
		{
			const auto* info = theater_capture::find_format(readback.job.format);
			if (!readback.context || !readback.staging || !readback.job.pixels || !info)
			{
				error = "internal error (no readback in progress)";
				return poll_status::failed;
			}

			const auto row = static_cast<std::size_t>(readback.job.width) * info->bytes_per_pixel;

			if (!readback.mapped)
			{
				// the copy was queued during an earlier frame; only wait for the GPU once it has had several frames
				const auto blocking = readback.polls >= MAX_MAP_POLLS;
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const auto hr = readback.context->Map(readback.staging.Get(), 0, D3D11_MAP_READ,
				                                      blocking ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING && !blocking)
				{
					++readback.polls;
					return poll_status::pending;
				}

				if (FAILED(hr))
				{
					error = std::format("ID3D11DeviceContext::Map failed (0x{:08X})", static_cast<std::uint32_t>(hr));
					return poll_status::failed;
				}

				if (!mapped.pData)
				{
					readback.context->Unmap(readback.staging.Get(), 0);
					error = "ID3D11DeviceContext::Map returned no data";
					return poll_status::failed;
				}

				readback.mapped = static_cast<const std::uint8_t*>(mapped.pData);
				readback.row_pitch = mapped.RowPitch;
				readback.rows_done = 0;

				if (readback.row_pitch < row)
				{
					error = std::format("unexpected row pitch {} (< {})", readback.row_pitch, row);
					return poll_status::failed; // the caller unmaps through release_readback()
				}
			}

			// bounded amount per frame: a 4K capture is spread over a few frames instead of one long one
			const auto rows_per_frame = std::max<std::size_t>(1, COPY_BYTES_PER_FRAME / row);
			const auto end = std::min<std::size_t>(readback.job.height, readback.rows_done + rows_per_frame);
			for (auto y = readback.rows_done; y < end; ++y)
			{
				std::memcpy(readback.job.pixels.get() + y * row, readback.mapped + y * readback.row_pitch, row);
			}

			readback.rows_done = end;
			if (end < readback.job.height)
			{
				return poll_status::pending;
			}

			readback.context->Unmap(readback.staging.Get(), 0);
			readback.mapped = nullptr;
			readback.staging.Reset();
			readback.context.Reset();
			return poll_status::done;
		}

		void start_encoder()
		{
			auto job = std::move(readback.job);
			release_readback();

			current = phase::encoding;
			try
			{
				std::thread(encoder_thread, std::move(job)).detach();
			}
			catch (const std::exception& e)
			{
				post(console::con_type_warning, std::string("could not start the encoder thread: ") + e.what());
				current = phase::idle;
			}
		}

		// ------------------------------------------------------------------ Present callback (render thread)

		void on_present(IDXGISwapChain* swap_chain, const phase p)
		{
			std::string error;
			if (p == phase::armed)
			{
				auto expected = phase::armed;
				if (!current.compare_exchange_strong(expected, phase::reading))
				{
					return; // cancelled by the main thread
				}

				if (!begin_readback(swap_chain, error))
				{
					release_readback();
					post(console::con_type_warning, error);
					current = phase::idle;
				}

				return;
			}

			switch (poll_readback(error))
			{
			case poll_status::pending:
				break;
			case poll_status::done:
				start_encoder();
				break;
			case poll_status::failed:
				release_readback();
				post(console::con_type_warning, error);
				current = phase::idle;
				break;
			}
		}

		// theater_capture calls this before the real Present of the game's swap chain while needs_present() is set
		void before_present(IDXGISwapChain* swap_chain)
		{
			const auto p = current.load();
			if (p != phase::armed && p != phase::reading)
			{
				return;
			}

			try
			{
				on_present(swap_chain, p);
			}
			catch (...)
			{
				release_readback();
				post(console::con_type_warning, "capture failed (exception)");
				current = phase::idle;
			}
		}

		// ------------------------------------------------------------------ request handling (main thread)

		bool ui_has_focus()
		{
			// console, menus and chat set key catchers (same test as theater.cpp)
			return *game::keyCatchers != 0;
		}

		std::string sanitize_name(const std::string& name)
		{
			std::string out;
			for (const auto c : name)
			{
				const auto ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					c == '-' || c == '_' || c == '.' || c == ' ';
				out += ok ? c : '_';
			}

			const auto trim = [&out]()
			{
				while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
				while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
			};

			trim();
			if (out.size() > 64)
			{
				out.resize(64);
				trim();
			}

			return out.empty() ? "screenshot" : out;
		}

		std::string timestamp()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		}

		std::string default_name()
		{
			try
			{
				if (demo_playback::playing())
				{
					if (auto stem = demo_playback::get_path().stem().string(); !stem.empty())
					{
						return stem;
					}
				}
			}
			catch (...)
			{
			}

			return "screenshot";
		}

		void cancel(const phase from, const char* reason)
		{
			auto expected = from;
			if (current.compare_exchange_strong(expected, phase::idle))
			{
				post(console::con_type_warning, reason); // this can run on the render thread (loading screens)
			}
		}

		void advance()
		{
			const auto now = std::chrono::steady_clock::now();
			switch (current.load())
			{
			case phase::waiting_ui:
				if (ui_has_focus())
				{
					ui_closed_since = {};
					if (now - request_time > UI_WAIT_TIMEOUT)
					{
						cancel(phase::waiting_ui, "cancelled, the console or a menu stayed open");
					}
				}
				else if (ui_closed_since == std::chrono::steady_clock::time_point{})
				{
					ui_closed_since = now;
				}
				else if (now - ui_closed_since >= UI_SETTLE_TIME)
				{
					hidden_frames = 0;
					current = phase::hiding;
				}
				break;

			case phase::hiding:
				// this runs in R_EndFrame before the frame is submitted: once it has run CLEAN_FRAMES + 1 times,
				// at least CLEAN_FRAMES complete frames were handed to the render thread without the theater HUD,
				// and the render thread is at most 2 frames behind (see "Frame lag" at the top)
				if (++hidden_frames > CLEAN_FRAMES)
				{
					armed_time = now;
					current = phase::armed;
					theater_capture::update(theater_capture::client::screenshot); // Present callbacks from now on
				}
				break;

			case phase::armed:
				if (now - armed_time > PRESENT_TIMEOUT)
				{
					cancel(phase::armed, "cancelled, no frame was presented");
				}
				break;

			default:
				break;
			}
		}
	}

	void request(const std::string& base_name)
	{
		if (!theater_capture::on_main_thread())
		{
			post(console::con_type_warning, "ignored a request that was not made on the game's main thread");
			return;
		}

		if (theater_capture::hook_failed())
		{
			console::warn("theater_screenshot: unavailable (Direct3D 11 hook failed)\n");
			return;
		}

		auto expected = phase::idle;
		if (!current.compare_exchange_strong(expected, phase::waiting_ui))
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - last_busy_warning > 1s)
			{
				last_busy_warning = now;
				console::warn("theater_screenshot: still busy with the previous screenshot\n");
			}
			return;
		}

		// first capture of the session: hook IDXGISwapChain::Present now (nothing to race with while idle)
		if (!theater_capture::ensure_hook())
		{
			current = phase::idle;
			return;
		}

		try
		{
			pending_base = console::get_theater_dir() / "screenshots" / (sanitize_name(base_name) + "_" + timestamp());
		}
		catch (const std::exception& e)
		{
			current = phase::idle;
			console::warn("theater_screenshot: invalid file name: %s\n", e.what());
			return;
		}

		request_time = std::chrono::steady_clock::now();
		ui_closed_since = {};
		hidden_frames = 0;

		if (ui_has_focus())
		{
			console::info("theater_screenshot: the screenshot is taken when the console / menu is closed\n");
		}
		else
		{
			current = phase::hiding; // hidden from the next HUD draw on
		}
	}

	bool hide_theater_hud()
	{
		const auto p = current.load();
		return p == phase::hiding || p == phase::armed;
	}

	std::optional<std::filesystem::path> take_result()
	{
		std::lock_guard _(result_mutex);
		auto r = std::move(result);
		result.reset();
		return r;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			theater_capture::set_callbacks(theater_capture::client::screenshot, {before_present, nullptr, needs_present});

			command::add("theater_screenshot", [](const command::params& params)
			{
				request(params.size() > 1 ? params.join(1) : default_name());
			});

			scheduler::loop(advance, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(theater_screenshot::component)
