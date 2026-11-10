#include <std_include.hpp>
#include "command.hpp"
#include "console.hpp"

#include "game/game.hpp"
#include "utils/string.hpp"

namespace command
{
	namespace
	{
		constexpr auto CMD_MAX_NESTING = 8;

		std::unordered_map<std::string, std::function<void(params&)>> handlers;

		void main_handler()
		{
			params params = {};

			const auto command = utils::string::to_lower(params[0]);
			if (const auto itr = handlers.find(command); itr != handlers.end())
			{
				try
				{
					itr->second(params);
				}
				catch (const std::exception& e)
				{
					console::error("command '%s' failed: %s\n", command.data(), e.what());
				}
			}
		}
	}

	params::params()
		: nesting_(game::cmd_args->nesting)
	{
		assert(this->nesting_ < CMD_MAX_NESTING);
	}

	int params::size() const
	{
		return game::cmd_args->argc[this->nesting_];
	}

	const char* params::get(const int index) const
	{
		if (index >= this->size())
		{
			return "";
		}

		return game::cmd_args->argv[this->nesting_][index];
	}

	std::string params::join(const int index) const
	{
		std::string result = {};

		for (auto i = index; i < this->size(); i++)
		{
			if (i > index) result.append(" ");
			result.append(this->get(i));
		}
		return result;
	}

	params_sv::params_sv()
		: nesting_(game::sv_cmd_args->nesting)
	{
		assert(this->nesting_ < CMD_MAX_NESTING);
	}

	int params_sv::size() const
	{
		return game::sv_cmd_args->argc[this->nesting_];
	}

	const char* params_sv::get(const int index) const
	{
		if (index >= this->size())
		{
			return "";
		}

		return game::sv_cmd_args->argv[this->nesting_][index];
	}

	std::string params_sv::join(const int index) const
	{
		std::string result = {};

		for (auto i = index; i < this->size(); i++)
		{
			if (i > index) result.append(" ");
			result.append(this->get(i));
		}
		return result;
	}

	void add_raw(const char* name, void (*callback)())
	{
		// the engine keeps a pointer to the name and to the cmd_function_t for the lifetime of the process
		static std::deque<std::string> names;
		static std::deque<game::cmd_function_t> functions;

		names.emplace_back(name);
		functions.emplace_back();
		game::Cmd_AddCommandInternal(names.back().data(), callback, &functions.back());
	}

	void add(const char* name, const std::function<void(const params&)>& callback)
	{
		const auto command = utils::string::to_lower(name);

		if (!handlers.contains(command))
		{
			add_raw(name, main_handler);
		}

		handlers[command] = callback;
	}

	void add(const char* name, const std::function<void()>& callback)
	{
		add(name, [callback](const params&)
		{
			callback();
		});
	}

	void execute(std::string command, const bool sync)
	{
		if (!command.ends_with('\n'))
		{
			command += "\n";
		}

		if (sync)
		{
			game::Cmd_ExecuteSingleCommand(0, 0, command.data());
		}
		else
		{
			game::Cbuf_AddText(0, command.data());
		}
	}
}
