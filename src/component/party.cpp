#include <std_include.hpp>
#include "party.hpp"

#include "command.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"

// Subset of iw6-mod's party component needed by the demo playback code.

namespace party
{
	namespace
	{
		void connect_to_party(const game::netadr_t& target, const std::string& mapname, const std::string& gametype)
		{
			if (game::Live_SyncOnlineDataFlags(0))
			{
				scheduler::once([=]()
				{
					connect_to_party(target, mapname, gametype);
				}, scheduler::pipeline::main, 1s);
				return;
			}

			switch_gamemode_if_necessary(gametype);
			perform_game_initialization();

			char session_info[0x100] = {};
			game::CL_ConnectFromParty(0, session_info, &target, mapname.data(), gametype.data());
		}
	}

	void connect_to_dummy_party(const std::string& mapname, const std::string& gametype)
	{
		const game::netadr_t dummy_target{};
		connect_to_party(dummy_target, mapname, gametype);
	}

	void switch_gamemode_if_necessary(const std::string& gametype)
	{
		const auto target_mode = gametype == "aliens" ? game::CODPLAYMODE_ALIENS : game::CODPLAYMODE_CORE;
		const auto current_mode = game::Com_GetCurrentCoDPlayMode();

		if (current_mode != target_mode)
		{
			switch (target_mode)
			{
			case game::CODPLAYMODE_CORE:
				game::SwitchToCoreMode();
				break;
			case game::CODPLAYMODE_ALIENS:
				game::SwitchToAliensMode();
				break;
			default:
				break;
			}
		}
	}

	void perform_game_initialization()
	{
		// same sequence iw6-mod uses before connecting; fixes several crashes and impure client issues
		command::execute("onlinegame 1", true);
		command::execute("exec default_xboxlive.cfg", true);
		command::execute("xstartprivateparty", true);
		command::execute("xblive_privatematch 0", true);
		command::execute("startentitlements", true);
	}
}
