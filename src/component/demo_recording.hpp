#pragma once

namespace demo_recording
{
	[[nodiscard]] bool recording();
	[[nodiscard]] bool startrecord(std::string_view file_name = {}, bool overwrite = {});
	[[nodiscard]] bool stoprecord();

	// iw6-theater (demo_pov): demo files currently being written; empty when that file is not open
	[[nodiscard]] std::filesystem::path get_user_path();
	[[nodiscard]] std::filesystem::path get_auto_path();
}
