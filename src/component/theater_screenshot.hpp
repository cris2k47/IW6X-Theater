#pragma once

// BO2-style theater "SCREENSHOT": saves the next frame rendered without the theater HUD as a PNG in
// <Windows "Pictures" folder>\IW6X Theater\<base_name>_<YYYY-MM-DD_HH-MM-SS>.png, like the videos in
// <Videos>\IW6X Theater (<game>\theater\screenshots if there is no Pictures folder). The frame is read back from the
// game's Direct3D 11 swap chain, the engine's own "screenshot" command does not write files in this build.
//
// Integration (theater.cpp): skip theater_hud::draw() while hide_theater_hud() is true, call request()
// from the screenshot button and poll take_result() once per frame to show "screenshot saved".

namespace theater_screenshot
{
	// capture the next frame that is rendered WITHOUT the theater HUD. Main thread only (input handlers,
	// console commands, scheduler main / renderer pipeline); calls from other threads are ignored.
	// The first call of the session hooks IDXGISwapChain::Present (nothing is hooked before that).
	// If the console or a menu is open, the capture waits until it has been closed for 300 ms (gives up after 30 s).
	// Ignored with a console warning while a previous screenshot is still being taken or saved.
	void request(const std::string& base_name);

	// true while a capture is pending: the theater HUD must not be drawn in these frames
	[[nodiscard]] bool hide_theater_hud();

	// path of the last saved file once, then empty
	[[nodiscard]] std::optional<std::filesystem::path> take_result();
}
