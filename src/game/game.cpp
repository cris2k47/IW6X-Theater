#include <std_include.hpp>
#include "game.hpp"

namespace game
{
	int Cmd_Argc()
	{
		return cmd_args->argc[cmd_args->nesting];
	}

	const char* Cmd_Argv(const int index)
	{
		return cmd_args->argv[cmd_args->nesting][index];
	}

	int SV_Cmd_Argc()
	{
		return sv_cmd_args->argc[sv_cmd_args->nesting];
	}

	const char* SV_Cmd_Argv(const int index)
	{
		return sv_cmd_args->argv[sv_cmd_args->nesting][index];
	}

	namespace environment
	{
		// boot.cpp only initializes the add-on inside the multiplayer client
		launcher::mode mode = launcher::mode::multiplayer;

		launcher::mode get_mode()
		{
			return mode;
		}

		bool is_dedi()
		{
			return mode == launcher::mode::server;
		}

		bool is_mp()
		{
			return mode == launcher::mode::multiplayer;
		}

		bool is_sp()
		{
			return mode == launcher::mode::singleplayer;
		}

		void set_mode(const launcher::mode _mode)
		{
			mode = _mode;
		}
	}
}
