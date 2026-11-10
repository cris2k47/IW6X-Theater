#include <std_include.hpp>
#include "console.hpp"

#include "game/game.hpp"
#include "boot.hpp"

namespace console
{
	namespace
	{
		std::mutex log_mutex;

		const char* prefix(const int type)
		{
			switch (type)
			{
			case con_type_error: return "^1[theater] ";
			case con_type_warning: return "^3[theater] ";
			default: return "[theater] ";
			}
		}
	}

	std::filesystem::path get_theater_dir()
	{
		static const auto dir = []()
		{
			char path[MAX_PATH]{};
			GetModuleFileNameA(boot::get_module(), path, sizeof(path));
			auto p = std::filesystem::path(path).parent_path() / "theater";
			std::error_code ec;
			std::filesystem::create_directories(p, ec);
			return p;
		}();
		return dir;
	}

	void log(const std::string& line)
	{
		std::lock_guard _(log_mutex);
		static std::ofstream file(get_theater_dir() / "theater.log", std::ios::out | std::ios::trunc);
		if (file.is_open())
		{
			const auto now = std::chrono::system_clock::now();
			file << std::format("{:%H:%M:%S} ", std::chrono::floor<std::chrono::milliseconds>(now)) << line;
			if (line.empty() || line.back() != '\n') file << '\n';
			file.flush();
		}
	}

	void print(const int type, const char* fmt, ...)
	{
		char buffer[4096]{};

		va_list ap;
		va_start(ap, fmt);
		vsnprintf_s(buffer, _TRUNCATE, fmt, ap);
		va_end(ap);

		log(buffer);
		OutputDebugStringA(buffer);

		if (boot::is_initialized())
		{
			const auto message = std::string(prefix(type)) + buffer;
			game::Conbuf_AppendText(message.data());
		}
	}
}
