#pragma once

namespace demo_playback
{
	[[nodiscard]] bool playing();
	[[nodiscard]] bool startplay(std::string_view file_name, bool repeat = true);
	[[nodiscard]] bool startplay(std::filesystem::path& file_path, bool repeat = true);
	[[nodiscard]] void stopplay();

	// iw6-theater: path of the demo file currently being played (empty when not playing)
	[[nodiscard]] std::filesystem::path get_path();

	// iw6-theater: how often a demo was restarted internally (rewinds, loops) since the game started, main thread
	[[nodiscard]] std::uint32_t restarts();
}
