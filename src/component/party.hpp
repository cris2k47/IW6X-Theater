#pragma once

namespace party
{
	// load a map without a server (used to start demo playback)
	void connect_to_dummy_party(const std::string& mapname, const std::string& gametype);

	void switch_gamemode_if_necessary(const std::string& gametype);
	void perform_game_initialization();
}
