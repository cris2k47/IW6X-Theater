#include "std_include.hpp"
#include "loader/component_loader.hpp"

#include "demo_playback.hpp"
#include "demo_recording.hpp"
#include "demo_utils.hpp"

#include "command.hpp"
#include "console.hpp"
#include "game/game.hpp"
#include "scheduler.hpp"
#include "utils/hook.hpp"

using namespace demo_utils;

namespace // demo class
{
	game::dvar_t* demo_autorecord;

	class demo_recording_t
	{
		static constexpr auto NON_DELTA_SNAPSHOT_COUNT = 4;

		struct stored_times_t
		{
			std::int32_t flush_time{};
			std::int32_t predicted_data_time{};
			std::int32_t cur_svr_time{};
			std::int32_t first_svr_time_user_file{};
			std::int32_t first_svr_time_auto_file{};
		};

	public:
		demo_recording_t() = default;
		~demo_recording_t();
		demo_recording_t(const demo_recording_t&) = delete;
		demo_recording_t(demo_recording_t&&) noexcept = delete;
		demo_recording_t& operator=(const demo_recording_t&) = delete;
		demo_recording_t& operator=(demo_recording_t&&) noexcept = delete;

		[[nodiscard]] std::uint32_t get_demo_count() const;
		[[nodiscard]] std::int32_t get_demo_length() const;
		[[nodiscard]] bool is_recording() const;
		[[nodiscard]] bool non_delta_data_requested() const;
		[[nodiscard]] bool start(const std::filesystem::path& user_path, const std::filesystem::path& auto_path);
		[[nodiscard]] bool stop();

		void shutdown_watcher();
		void process_network_data(auto old_connstate, auto new_connstate, std::span<const std::uint8_t> network_data);
		void process_predicted_data(auto old_connstate, auto new_connstate);

		// iw6-theater (demo_pov)
		[[nodiscard]] std::filesystem::path get_user_path() const { return is_recording() ? user_path_ : std::filesystem::path{}; }
		[[nodiscard]] std::filesystem::path get_auto_path() const { return is_auto_recording() ? auto_path_ : std::filesystem::path{}; }

	private:
		[[nodiscard]] bool is_auto_recording() const;
		[[nodiscard]] bool non_delta_data_request_acknowledged();
		[[nodiscard]] bool start_user_file(const std::filesystem::path& path);
		[[nodiscard]] bool start_auto_file(const std::filesystem::path& path);
		[[nodiscard]] static bool stop_internal(std::ofstream& file, std::int32_t first_svr_time, std::int32_t last_svr_time);

		void flush_filestreams();
		void check_auto_recording();
		void process_old_data();
		void process_new_gamestate(std::span<const std::uint8_t> network_data);
		void process_first_snapshot(std::span<const std::uint8_t> network_data);
		void process_special_network_data(auto old_connstate, auto new_connstate, std::span<const std::uint8_t> network_data);
		void process_helo_pilot_turret_fire();

		std::uint32_t demo_count_{};
		std::uint32_t non_delta_count_{};
		stored_times_t times_;
		buffer_t crit_data_;
		std::ofstream user_file_;
		buffer_t auto_buffer_;
		std::ofstream auto_file_;
		std::filesystem::path user_path_; // iw6-theater (demo_pov)
		std::filesystem::path auto_path_; // iw6-theater (demo_pov)
	} demo;

	demo_recording_t::~demo_recording_t()
	{
		process_old_data();
	}

	std::uint32_t demo_recording_t::get_demo_count() const
	{
		return demo_count_;
	}

	std::int32_t demo_recording_t::get_demo_length() const
	{
		return (times_.cur_svr_time - times_.first_svr_time_user_file) / 1000;
	}

	bool demo_recording_t::is_recording() const
	{
		return user_file_.good() && user_file_.is_open();
	}

	bool demo_recording_t::is_auto_recording() const
	{
		return auto_file_.good() && auto_file_.is_open();
	}

	bool demo_recording_t::non_delta_data_requested() const
	{
		return non_delta_count_ > 0;
	}

	bool demo_recording_t::non_delta_data_request_acknowledged()
	{
		if (non_delta_data_requested())
		{
			const auto delta_num = game::mp::cl->snap.deltaNum;
			if (static constexpr auto no_delta = -1; delta_num == no_delta)
			{
				--non_delta_count_;
				return true;
			}
		}

		return false;
	}

	bool demo_recording_t::start_user_file(const std::filesystem::path& path)
	{
		const auto crit_data = crit_data_.get();
		if (!crit_data.size())
		{
			return false;
		}

		std::ofstream file(path, std::ios::binary);
		if (!file.good() || !file.is_open())
		{
			return false;
		}

		write_general_header(file);

		// store mod name so if there's mod support the mod can be loaded
		write_mod_header(file);

		// store map name and game type so that the map can be preloaded
		if (!write_map_header(file))
		{
			return false;
		}

		// store the current gamestate config strings so that the original strings are overwritten if they were modified
		if (!write_gamestate_data(file))
		{
			return false;
		}

		// required for demos (and live matches) to function at all
		file.write(reinterpret_cast<const char*>(crit_data.data()), crit_data.size());

		// set the non-delta snapshot count, the demo won't be valid otherwise
		non_delta_count_ = NON_DELTA_SNAPSHOT_COUNT;

		// store first server time so that demo length can be tracked
		times_.first_svr_time_user_file = game::mp::cl->snap.serverTime;

		// increase demo count for the text render function to work properly
		++demo_count_;

		user_path_ = path; // iw6-theater (demo_pov)
		user_file_ = std::move(file);

		return true;
	}

	bool demo_recording_t::start_auto_file(const std::filesystem::path& path)
	{
		std::ofstream file(path, std::ios::binary);
		if (!file.good() || !file.is_open())
		{
			return false;
		}

		const auto buffer = auto_buffer_.get();
		file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());

		auto_path_ = path; // iw6-theater (demo_pov)
		auto_file_ = std::move(file);
		auto_buffer_.clear();

		return true;
	}

	bool demo_recording_t::start(const std::filesystem::path& user_path, const std::filesystem::path& auto_path)
	{
		if (!is_recording())
		{
			if (!is_auto_recording() && !start_auto_file(auto_path))
			{
				return false;
			}

			if (!start_user_file(user_path))
			{
				return false;
			}

			return true;
		}

		return false;
	}

	bool demo_recording_t::stop_internal(std::ofstream& file, std::int32_t first_svr_time, std::int32_t last_svr_time)
	{
		write_general_footer(file, first_svr_time, last_svr_time);
		write_end_of_file(file);

		file.flush();
		file.close();

		return file.good() && !file.is_open();
	}

	bool demo_recording_t::stop()
	{
		return is_recording() && stop_internal(user_file_, times_.first_svr_time_user_file, times_.cur_svr_time);
	}

	void demo_recording_t::process_old_data()
	{
		if (is_recording() && !stop())
		{
			console::error("could not stop user demo\n");
		}

		if (is_auto_recording() && !stop_internal(auto_file_, times_.first_svr_time_auto_file, times_.cur_svr_time))
		{
			console::error("could not close auto demo\n");
		}

		demo_count_ = {};
		non_delta_count_ = {};
		times_ = {};
		crit_data_.clear();
		auto_buffer_.clear();
		assert(!is_recording());
		assert(!is_auto_recording());
	}

	void demo_recording_t::shutdown_watcher()
	{
		auto execute_on_disconnect = [this, cgame_init = false]() mutable
		{
			if (demo_playback::playing())
			{
				if (is_recording() || is_auto_recording())
				{
					// this is only relevant when playing back demos without disconnecting from the server (instant play)
					cgame_init = false;
					process_old_data();
				}
				return;
			}

			if (!cgame_init && game::CL_IsCgameInitialized())
			{
				cgame_init = true;
			}

			if (cgame_init && !game::CL_IsCgameInitialized())
			{
				cgame_init = false;
				process_old_data();
			}
		};

		scheduler::loop(execute_on_disconnect, scheduler::main);
	}

	void demo_recording_t::flush_filestreams()
	{
		const auto real_time = game::mp::cls->realtime;
		if (real_time >= times_.flush_time + 15'000)
		{
			times_.flush_time = real_time;

			if (is_recording())
			{
				user_file_.flush();
			}
			if (is_auto_recording())
			{
				auto_file_.flush();
			}
		}
	}

	void demo_recording_t::check_auto_recording()
	{
		if (!is_auto_recording() && times_.first_svr_time_auto_file && demo_autorecord && demo_autorecord->current.enabled)
		{
			assert(!game::Live_SyncOnlineDataFlags(0) && game::CL_IsCgameInitialized());

			const auto opt_auto_dir = create_directory_auto_demo();
			if (!opt_auto_dir)
			{
				console::error("could not create demo auto directory\n");
				return;
			}

			const auto opt_auto_path = create_path_auto_demo(*opt_auto_dir);
			if (!opt_auto_path)
			{
				console::error("could not create demo auto file\n");
				return;
			}

			if (!start_auto_file(*opt_auto_path))
			{
				// turn auto recording off to prevent spamming the console with errors
				demo_autorecord->current.enabled = false;

				console::error("could not create demo auto file %s\n", opt_auto_path->string().c_str());
				return;
			}
		}
	}

	void demo_recording_t::process_new_gamestate(std::span<const std::uint8_t> network_data)
	{
		process_old_data();

		// store gamestate for user file
		write_network_data(crit_data_, network_data);

		// reserve memory; use smaller buffer size when auto recording is enabled,
		// since the data is written to disk almost right away.
		const auto reserve_size = (demo_autorecord && demo_autorecord->current.enabled) ? 8192 : 1024 * 1024;
		auto_buffer_.reserve_memory(reserve_size);

		// write headers to auto buffer (ahead of the gamestate itself)
		write_general_header(auto_buffer_);
		write_mod_header(auto_buffer_);
		write_map_header(auto_buffer_);
	}

	void demo_recording_t::process_first_snapshot(std::span<const std::uint8_t> network_data)
	{
		assert(!is_recording() && crit_data_.size()
			&& !times_.cur_svr_time && !times_.first_svr_time_user_file && !times_.first_svr_time_auto_file);

		// store first snapshot for user file
		write_network_data(crit_data_, network_data);

		times_.first_svr_time_auto_file = game::mp::cl->snap.serverTime;
	}

	void demo_recording_t::process_special_network_data(auto old_connstate, auto new_connstate, std::span<const std::uint8_t> network_data)
	{
		assert(new_connstate >= game::CA_PRIMED);

		if (old_connstate < game::CA_LOADING)
		{
			assert(!game::mp::cl->snap.valid && !game::mp::cl->snap.serverTime);

			process_new_gamestate(network_data);
		}
		else if (old_connstate >= game::CA_PRIMED)
		{
			if (game::mp::cl->snap.valid && game::mp::cl->snap.serverTime && !times_.cur_svr_time)
			{
				process_first_snapshot(network_data);
			}

			times_.cur_svr_time = game::mp::cl->snap.serverTime;
		}
	}

	void demo_recording_t::process_network_data(auto old_connstate, auto new_connstate, std::span<const std::uint8_t> network_data)
	{
		if (new_connstate < game::CA_PRIMED)
		{
			return;
		}

		process_special_network_data(old_connstate, new_connstate, network_data);
		check_auto_recording();

		if (is_recording())
		{
			if (!non_delta_data_requested() || non_delta_data_request_acknowledged())
			{
				write_network_data(user_file_, network_data);
			}
		}

		if (is_auto_recording())
		{
			write_network_data(auto_file_, network_data);
		}
		else
		{
			write_network_data(auto_buffer_, network_data);
		}

		flush_filestreams();
	}

	void demo_recording_t::process_helo_pilot_turret_fire()
	{
		const auto turret_fire = cpy_cast<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(0x1419A9394));
		if (turret_fire > 0)
		{
			if (is_recording())
			{
				write_helo_pilot_turret_fire(user_file_, turret_fire);
			}

			if (is_auto_recording())
			{
				write_helo_pilot_turret_fire(auto_file_, turret_fire);
			}
			else
			{
				write_helo_pilot_turret_fire(auto_buffer_, turret_fire);
			}
		}
	}

	void demo_recording_t::process_predicted_data(auto old_connstate, auto new_connstate)
	{
		if (old_connstate < game::CA_ACTIVE || new_connstate < game::CA_ACTIVE)
		{
			return;
		}

		process_helo_pilot_turret_fire();

		if (is_recording())
		{
			write_predicted_data(user_file_);
		}

		if (is_auto_recording())
		{
			write_predicted_data(auto_file_);
		}
		else
		{
			write_predicted_data(auto_buffer_);
		}
	}
}

namespace // hooks
{
	utils::hook::detour CL_ParseServerMessage_hook;

	void capture_data(const std::int32_t client_num, game::msg_t& msg)
	{
		const auto old_connstate = *game::mp::connstate;
		CL_ParseServerMessage_hook.invoke(client_num, &msg);
		const auto new_connstate = *game::mp::connstate;

		if (demo_playback::playing())
		{
			return;
		}

		if (new_connstate < game::CA_DISCONNECTED || new_connstate > game::CA_ACTIVE)
		{
			console::warn("invalid connection state after processing data in CL_PacketEvent\n");
			return;
		}

		if (!msg.data || msg.overflowed || static_cast<std::size_t>(msg.cursize) > MAX_SIZE)
		{
			if (demo.is_recording())
			{
				console::warn("invalid data in CL_PacketEvent\n");
			}
			return;
		}

		demo.process_predicted_data(old_connstate, new_connstate);
		demo.process_network_data(old_connstate, new_connstate,
			std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(msg.data), msg.cursize));
	}

	void request_non_delta_data(game::msg_t& msg, std::int32_t value, std::uint32_t bits)
	{
		if (!demo_playback::playing() && demo.non_delta_data_requested())
		{
			// request a non-delta snapshot from the server
			// to have a valid start for a new (user) demo
			value = 1;
		}

		// call MSG_WriteBits that this hook replaces
		game::MSG_WriteBits(&msg, value, bits);
	}
}

namespace // command execution
{
	void add_recording_text_to_hud(const std::filesystem::path& path)
	{
		const auto* font = game::R_RegisterFont("fonts/normalfont");
		if (!font)
		{
			console::error("could not register font\n");
			return;
		}

		const auto* view_placement = game::ScrPlace_GetViewPlacement();
		if (!view_placement)
		{
			console::error("could not find view placement\n");
			return;
		}

		auto create_resources = [&path, font, view_placement]()
		{
			struct resources_t
			{
				const game::Font_t* font{};
				float y{};
				std::int32_t demo_length{};
				std::uint32_t demo_count{};
				std::uint32_t org_text_size{};
				std::string text;
			};

			resources_t res{
				.font = font,
				.y = view_placement->realViewportSize[1],
				.demo_length = demo.get_demo_length(),
				.demo_count = demo.get_demo_count(),
				.text = "recording: " + path.stem().string()
			};

			res.org_text_size = static_cast<std::uint32_t>(res.text.size());
			res.text.reserve(res.text.size() + 12);

			return res;
		};

		scheduler::schedule([res = create_resources()]() mutable
		{
			if (!demo.is_recording() || res.demo_count != demo.get_demo_count() || !game::CL_IsCgameInitialized())
			{
				return scheduler::cond_end;
			}

			if (res.demo_length != demo.get_demo_length())
			{
				res.demo_length = demo.get_demo_length();

				// resize to original size and append demo length in seconds
				res.text.resize(res.org_text_size);
				res.text += std::format(" ({} sec)", res.demo_length);
			}

			static constexpr std::array<float, 4> color_red{ 178.0f / 255.0f, 34.0f / 255.0f, 34.0f / 255.0f, 1.0f };

			// draw in bottom left corner
			game::R_AddCmdDrawText(res.text.c_str(), std::numeric_limits<std::int32_t>::max(),
				res.font, 10.0f, res.y - 5.0f, 0.75f, 0.75f, 0.0f, color_red.data(), 0);

			return scheduler::cond_continue;
		}, scheduler::renderer);
	}

	bool execute_demo_start_command_internal(std::string_view file_name, bool overwrite)
	{
		if (game::Live_SyncOnlineDataFlags(0) || !game::CL_IsCgameInitialized() || *game::mp::connstate != game::CA_ACTIVE)
		{
			console::warn("must be in a match to record a demo\n");
			return false;
		}

		if (demo.is_recording())
		{
			console::warn("must stop demo recording before starting to record\n");
			return false;
		}

		if (demo_playback::playing())
		{
			console::warn("cannot record a demo during demo playback\n");
			return false;
		}

		const auto opt_user_dir = create_directory_user_demo();
		if (!opt_user_dir)
		{
			console::error("could not create demo user directory\n");
			return false;
		}

		const auto opt_auto_dir = create_directory_auto_demo();
		if (!opt_auto_dir)
		{
			console::error("could not create demo auto directory\n");
			return false;
		}

		const auto opt_user_path = (file_name.empty())
			? create_path_user_demo(*opt_user_dir)
			: create_path_user_demo(*opt_user_dir, file_name, overwrite);
		if (!opt_user_path)
		{
			console::error("could not create demo user file\n");
			return false;
		}

		const auto opt_auto_path = create_path_auto_demo(*opt_auto_dir);
		if (!opt_auto_path)
		{
			console::error("could not create demo auto file\n");
			return false;
		}

		if (!demo.start(*opt_user_path, *opt_auto_path))
		{
			console::error("could not create demo user file %s\n", opt_user_path->string().c_str());
			console::error("could not create demo auto file %s\n", opt_auto_path->string().c_str());
			return false;
		}

		add_recording_text_to_hud(*opt_user_path);

		return true;
	}

	void execute_demo_start_command(const command::params& params)
	{
		if (params.size() < 1 || params.size() > 3 || (params.size() == 3 && std::string_view(params.get(2)) != "overwrite"))
		{
			console::info("usage: demostart <file_name>(optional), overwrite(optional)\n");
			return;
		}

		assert(params.size() != 3 || (params.size() == 3 && std::string_view(params.get(2)) == "overwrite"));
		execute_demo_start_command_internal((params.size() >= 2) ? params.get(1) : std::string_view{}, (params.size() == 3));
	}

	void execute_demo_stop_command(const command::params& params)
	{
		if (params.size() != 1)
		{
			console::info("usage: demostop\n");
			return;
		}

		if (demo.is_recording() && !demo.stop())
		{
			console::error("could not stop demo\n");
		}
	}
}

namespace demo_recording
{
	bool recording()
	{
		return demo.is_recording();
	}

	bool startrecord(std::string_view file_name, bool overwrite)
	{
		return execute_demo_start_command_internal(file_name, overwrite);
	}

	bool demo_recording::stoprecord()
	{
		return demo.stop();
	}

	// iw6-theater (demo_pov)
	std::filesystem::path get_user_path()
	{
		return demo.get_user_path();
	}

	// iw6-theater (demo_pov)
	std::filesystem::path get_auto_path()
	{
		return demo.get_auto_path();
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (!game::environment::is_mp())
			{
				return;
			}

			// check run-time address assertions
			check_address_assertions();

			// capture incoming packets
			CL_ParseServerMessage_hook.create(game::CL_ParseServerMessage, capture_data);

			// request the server for no delta data (in CL_WritePacket)
			utils::hook::call(0x1402C2026, request_non_delta_data);

			// add support to auto record
			demo_autorecord = game::Dvar_RegisterBool(
				"demoautorecord", true, game::DVAR_FLAG_SAVED, "Automatically start recording a demo."); // iw6-theater: saved, on by default (GAME RECORDING)

			// add console command support to record demos
			command::add("demostart", execute_demo_start_command);
			command::add("demostop", execute_demo_stop_command);

			scheduler::on_game_initialized([]()
			{
				demo.shutdown_watcher();

				// check if demo directories exist / could be created
				if (!can_create_demo_directories())
				{
					console::error("could not create demo directories\n");
				}
			}, scheduler::main);
		}
	};
}

REGISTER_COMPONENT(demo_recording::component)
