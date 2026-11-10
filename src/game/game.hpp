#pragma once

#include "structs.hpp"

// The add-on only supports the multiplayer binary (iw6mp64_ship.exe 3.15 pinned by CBServers).
#define SELECT_VALUE(sp, mp) (game::environment::is_sp() ? (sp) : (mp))

namespace launcher
{
	enum class mode
	{
		none,
		singleplayer,
		multiplayer,
		server,
	};
}

namespace game
{
	namespace environment
	{
		launcher::mode get_mode();

		bool is_mp();
		bool is_sp();
		bool is_dedi();

		void set_mode(launcher::mode mode);
	}

	template <typename T>
	class symbol
	{
	public:
		symbol(const size_t sp_address, const size_t mp_address)
			: sp_object_(reinterpret_cast<T*>(sp_address))
			, mp_object_(reinterpret_cast<T*>(mp_address))
		{
		}

		T* get() const
		{
			if (environment::is_sp())
			{
				return sp_object_;
			}

			return mp_object_;
		}

		operator T*() const
		{
			return this->get();
		}

		T* operator->() const
		{
			return this->get();
		}

	private:
		T* sp_object_;
		T* mp_object_;
	};

	[[nodiscard]] int Cmd_Argc();
	[[nodiscard]] const char* Cmd_Argv(int index);

	[[nodiscard]] int SV_Cmd_Argc();
	[[nodiscard]] const char* SV_Cmd_Argv(int index);

	constexpr auto MAX_GENTITIES = 2048;
	constexpr auto ENTITYNUM_NONE = MAX_GENTITIES - 1;

	constexpr auto JUMP_LAND_SLOWDOWN_TIME = 1800;
}

#include "symbols.hpp"
#include "symbols_theater.hpp"
