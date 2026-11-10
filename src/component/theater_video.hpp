#pragma once

// Theater "RENDER VIDEO": encodes every frame the game presents into an H.264 MP4 (Media Foundation, loaded at
// run time: Windows "N" editions without the Media Feature Pack only lose this feature).
//
// One presented frame = one video frame, with timestamps index * 10'000'000 / fps (100 ns units): the video
// plays at 'fps' whatever the real frame rate was, so the theater renders clips with a fixed-step demo clock
// (each game frame advances exactly 1000 / fps ms of film time). Frames are never dropped or duplicated while
// capturing: when the encoder falls behind, the game's Present waits for it (the game stalls, fine with a
// fixed-step clock). If the encoder stalls for more than ~5 s, fails, or the game's swap chain / resolution
// changes or the device is lost, the capture stops with an error and the frames written so far are kept.
//
// Frame lag: the engine's render thread presents a frame up to 2 frames after the main thread issued it, so
// start() skips 'skip_frames' presented frames (those still in flight), and frames the main thread issued
// before stop() but that were not presented yet are not in the video (call stop() 2 frames after the last
// frame of a clip was issued to be sure it is included).
//
// The file is written as "<path>.part" and renamed to 'path' once it has been finalized (if 'path' exists by
// then, a free "<stem>_2.mp4", "<stem>_3.mp4", ... is used instead; take_result() returns the real name). If the
// rename keeps failing (another program holds the .part file open), the video is copied to that name instead, and
// if that fails too it stays "<path>.part" (take_result() returns that name): a finished video is never deleted
// because of the rename.
//
// All functions are for the main thread (console commands, theater input / frame handlers).
// Console: theater_video_test <seconds> [fps], theater_video_stop, theater_video_cancel; dvar
// theater_video_bitrate (Mbps, 0 = automatic).

namespace theater_video
{
	// Starts encoding every frame the game presents into an H.264 MP4 at 'path' with exactly 'fps' frames per
	// second. The first 'skip_frames' presented frames are dropped (frames still in flight from before the call).
	// Installs the Present hook if needed. Returns false (see take_error) if the capture or encoder cannot be set
	// up, or while a previous video is still being captured or finalized (busy()).
	bool start(const std::filesystem::path& path, int fps, int skip_frames = 3);

	// no more frames are taken from the next Present on; the file is finalized in the background
	void stop();

	// stop and delete the file
	void cancel();

	// between start() and stop()/cancel() (false as soon as an error stopped the capture)
	[[nodiscard]] bool capturing();

	// capturing or still finalizing
	[[nodiscard]] bool busy();

	// frames handed to the encoder so far (the frame count of the file once it is finalized)
	[[nodiscard]] std::uint64_t frames();

	// finished file, once. A start() that is not refused as busy() discards an unclaimed result / error of an
	// earlier video, so after it these two only ever describe the new one.
	[[nodiscard]] std::optional<std::filesystem::path> take_result();

	// last error, once (empty if none)
	[[nodiscard]] std::string take_error();

	// 0..1 progress bar (thin bar at the top of the screen) drawn on the back buffer AFTER the frame was copied,
	// so it is never in the video; < 0 hides it. Only drawn while capturing; reset (hidden) by stop()/cancel().
	void set_progress(float fraction);

	// <Windows "Videos" known folder>\IW6X Theater (created on demand)
	[[nodiscard]] std::filesystem::path default_folder();
}
