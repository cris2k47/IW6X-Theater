#include <std_include.hpp>
#include "command_line.hpp"

namespace utils::command_line
{
	const std::vector<std::filesystem::path>& get_args()
	{
		static const auto args = []()
		{
			std::vector<std::filesystem::path> vec;

			auto argc = 0;
			const auto* cmd_line = ::GetCommandLineW();
			if (!cmd_line)
			{
				return vec;
			}

			auto* argv = ::CommandLineToArgvW(cmd_line, &argc);
			if (!argv)
			{
				return vec;
			}

			vec.reserve(argc);
			for (auto i = 0; i < argc; ++i)
			{
				vec.emplace_back(argv[i]);
			}

			::LocalFree(argv);
			return vec;
		}();

		return args;
	}
}
