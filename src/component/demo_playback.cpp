#include "std_include.hpp"
#include "loader/component_loader.hpp"

#include "demo_playback.hpp"
#include "demo_utils.hpp"

#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"
#include "utils/command_line.hpp"
#include "utils/hook.hpp"

using namespace demo_utils;

namespace // demo class
{
	class demo_playback_t
	{
	public:
		demo_playback_t();

		[[nodiscard]] bool is_demo_playing() const;
		[[nodiscard]] bool is_demo_repeating() const;
		[[nodiscard]] const std::optional<gamestate_t>& get_gamestate() const;
		[[nodiscard]] std::filesystem::path get_path() const { return is_demo_playing() ? path_ : std::filesystem::path{}; }
		[[nodiscard]] bool rewind_demo(std::optional<std::uint32_t> msec);
		[[nodiscard]] bool start(std::filesystem::path& path, bool repeat);
		[[nodiscard]] std::optional<std::reference_wrapper<std::filesystem::path>> restart();
		template <typename Func, typename... Args> [[nodiscard]] auto use_predicted_data(Func func, Args&&... args);
		template <typename Func, typename... Args> [[nodiscard]] auto handle_fast_forward(Func func, Args&&... args);

		void parse_demo_wrapper();
		void stop();

	private:
		void repeat();

		bool repeat_{};
		bool skip_timeout_{};
		bool retain_timescale_{};
		std::size_t post_map_header_file_offset_{};
		std::size_t pred_data_index_{};
		std::array<predicted_data_t, 256> pred_data_{};
		std::optional<std::int32_t> rewind_snap_svr_time_;
		std::optional<gamestate_t> gs_;
		std::vector<std::uint8_t> buffer_;
		std::filesystem::path path_;
		std::ifstream file_;
	} demo;

	// iw6-theater: internal restarts (repeat), see demo_playback::restarts()
	std::uint32_t restart_count{};

	// iw6-theater: a rewind restarts the demo from its first snapshot and runs it forward to the target. The HUD must not
	// see the snapshots on the way: their omnvar updates reach the LUI as if the match replayed. The demo's first
	// snapshot is in the pre-match countdown, so "Match begins in:" (matchstarthud.lua, ui_match_start_countdown) showed
	// for two frames after every jump back in the spectator view (seen in game) until the target's update hid it again.
	// The omnvar updates of those snapshots are skipped and the target gets one update from the snapshot the HUD saw
	// before the restart, as if the film had jumped there. CG_UpdateOmnvars compares the global omnvars with the client's
	// cached values (left at the pre-rewind state while the updates are skipped) and the playerState ones with the old
	// snapshot, so the pre-rewind snapshot is kept for the comparison.
	constexpr std::int32_t REWIND_HUD_LANDED_MSEC = 100;        // a snapshot this close to the target (or past it) has landed
	constexpr std::chrono::milliseconds REWIND_HUD_TIMEOUT{2000}; // the updates are never held back longer than this

	struct rewind_hud_t
	{
		bool pending{};
		bool have_base{};
		std::uint32_t restarts_at_request{};
		std::int32_t target{};
		std::chrono::steady_clock::time_point requested{};
		game::mp::snapshot_t base{}; // the last snapshot the HUD saw before the restart
	};

	rewind_hud_t rewind_hud{};

	// A seek runs the server commands of the skipped part of the film in the frame it lands, the vision set changes among
	// them, and each of those started its lerp there. A rewind restarts the demo from its beginning, so that was the black
	// and white 'mpIntro' of the pre-match followed by the return to the map's own vision set with the lerp the server
	// asked for: a grey picture that took about 2.5 s to get its color back after every jump back, also at the start of
	// a rendered clip (seen in game). While a rewind is in flight, and for a few frames after a seek, vision set changes
	// are applied at once (handle_vision_override, handle_client_side_vision).
	constexpr std::int32_t SEEK_VISION_FRAMES = 8;      // client frames after a seek
	constexpr std::int32_t SEEK_VISION_MIN_MSEC = 1000; // a shorter step forward plays its vision set changes as recorded
	std::int32_t seek_vision_frames{};

	bool seek_vision_instant()
	{
		return rewind_hud.pending || seek_vision_frames > 0;
	}

	// The same goes for the feed of kills and game messages (obituaryhud.lua): the lines of the skipped part of the film
	// all showed up where the seek landed. After every jump back that was a column of "<name> Connected", sent with the
	// demo's first snapshots, and after a jump forward the kills of the part that was skipped (seen in game). While a seek
	// is in flight those lines never reach the HUD (handle_feed_message, handle_feed_obituary). A rewind is in flight
	// until its target gets its omnvar update (rewind_hud), a jump forward until a snapshot around its target arrives:
	// in both cases the server commands of that snapshot have just been executed (CG_SetNextSnap runs them first).
	struct forward_seek_t
	{
		bool pending{};
		std::int32_t target{};
		std::chrono::steady_clock::time_point requested{};
	};

	forward_seek_t forward_seek{};

	bool seek_skips_feed()
	{
		return rewind_hud.pending || forward_seek.pending;
	}

	// The lines the feed is showing when a seek starts belong to the moment the film leaves, and stayed on screen where
	// it landed (a video rendered with the HUD on began with them). The add-on's LUI script closes them when it gets
	// this event; without the script nobody handles it. An LUI event without fields is sent the way the engine sends
	// its own (0x1401D0C50, "playerstate_client_changed"): the first call takes the LUI lock and counts the nesting,
	// the second one runs the handlers.
	void clear_feed()
	{
		auto* const lua_vm = *reinterpret_cast<void**>(0x141640DA0); // the LUI state CG_GameMessage sends its event to
		if (!lua_vm || *game::mp::connstate != game::CA_ACTIVE || !game::CL_IsCgameInitialized())
		{
			return;
		}

		if (utils::hook::invoke<bool>(0x1401CBD90, 0, "theater_feed_clear", lua_vm))
		{
			utils::hook::invoke<void>(0x1401CCDE0, 3, lua_vm);

			if (--*reinterpret_cast<std::int32_t*>(0x141640DB8) == 0)
			{
				*reinterpret_cast<std::uint8_t*>(0x141640E2C) = 0;
				*reinterpret_cast<std::uint8_t*>(0x141640E2D) = 0;
			}

			utils::hook::invoke<void>(0x1404F9890, 0x2C); // Sys_LeaveCriticalSection, the LUI lock
		}
	}

	void arm_forward_seek(const std::int32_t target)
	{
		clear_feed();

		forward_seek.pending = true;
		forward_seek.target = target;
		forward_seek.requested = std::chrono::steady_clock::now();
	}

	void arm_rewind_hud(const std::int32_t target)
	{
		// a second rewind before the first one landed keeps the snapshot the HUD saw before the first one
		if (!rewind_hud.pending)
		{
			rewind_hud.have_base = false;
			clear_feed();
		}

		rewind_hud.pending = true;
		rewind_hud.restarts_at_request = restart_count;
		rewind_hud.target = target;
		rewind_hud.requested = std::chrono::steady_clock::now();
	}

	void reset_rewind_hud()
	{
		rewind_hud.pending = false;
		rewind_hud.have_base = false;
	}

	demo_playback_t::demo_playback_t()
	{
		buffer_.reserve(MAX_SIZE);
	}

	bool demo_playback_t::is_demo_playing() const
	{
		return file_.good() && file_.is_open();
	}

	bool demo_playback_t::is_demo_repeating() const
	{
		return repeat_;
	}

	const std::optional<gamestate_t>& demo_playback_t::get_gamestate() const
	{
		return gs_;
	}

	template <typename Func, typename ...Args>
	auto demo_playback_t::use_predicted_data(Func func, Args&& ...args)
	{
		return func(pred_data_, pred_data_index_, std::forward<Args>(args)...);
	}

	template <typename Func, typename ...Args>
	auto demo_playback_t::handle_fast_forward(Func func, Args&& ...args)
	{
		return func(rewind_snap_svr_time_, skip_timeout_, std::forward<Args>(args)...);
	}

	bool demo_playback_t::rewind_demo(std::optional<std::uint32_t> msec)
	{
		if (!is_demo_playing())
		{
			return false;
		}

		// set demo end-of-file which will repeat the demo
		repeat_ = true;
		file_.seekg(0, std::ios::end);
		forward_seek.pending = false; // the restart replaces a jump forward that has not landed

		if (msec)
		{
			retain_timescale_ = true;

			const auto snap_svr_time = game::mp::cl->snap.serverTime;
			if (const auto delta_time = snap_svr_time - static_cast<std::int32_t>(*msec) - 50; delta_time > 0)
			{
				rewind_snap_svr_time_ = delta_time;
				arm_rewind_hud(delta_time);
			}
		}

		// workaround to get rewinding working without delay at low timescales
		fast_forward_demo(1000);

		return true;
	}

	bool demo_playback_t::start(std::filesystem::path& path, bool repeat)
	{
		// check if the file can be opened before taking any action with the demo file stream
		std::ifstream file(path, std::ios::binary);

		const auto can_open = file.good() && file.is_open();
		if (!can_open)
		{
			return false;
		}

		stop();

		file_ = std::move(file);
		assert(file_.good() && file_.is_open());

		// (re)set data
		repeat_ = repeat;
		gs_.reset();
		path_ = std::move(path);
		get_persistent_data() = {};

		if (auto* demo_timescale = game::Dvar_FindVar("demotimescale"); demo_timescale)
		{
			demo_timescale->current.value = 1.0f;
		}

		if (game::Live_SyncOnlineDataFlags(0))
		{
			console::info("the demo should start to load in a few seconds\n");
		}

		return true;
	}

	std::optional<std::reference_wrapper<std::filesystem::path>> demo_playback_t::restart()
	{
		if (!start(path_, repeat_))
		{
			return path_;
		}

		return std::nullopt;
	}

	void demo_playback_t::parse_demo_wrapper()
	{
		if (!parse_demo(file_, buffer_, post_map_header_file_offset_, pred_data_, pred_data_index_, gs_))
		{
			if (is_demo_repeating() && *game::mp::connstate != game::CA_DISCONNECTED)
			{
				repeat();
			}
			else
			{
				// don't repeat demo if it was stopped manually
				stop();
			}
		}

		assert(*game::mp::connstate != game::CA_DISCONNECTED || !is_demo_playing());
	}

	void demo_playback_t::stop()
	{
		skip_timeout_ = {};
		retain_timescale_ = {};
		post_map_header_file_offset_ = {};
		pred_data_index_ = {};
		pred_data_.fill({});
		rewind_snap_svr_time_ = {};
		reset_rewind_hud();
		forward_seek.pending = false;
		seek_vision_frames = {};
		gs_.reset();
		buffer_.clear();
		file_.clear();
		file_.close();

		// ensure that sv_cheats is always disabled after demo playback
		if (auto* sv_cheats = game::Dvar_FindVar("sv_cheats"); sv_cheats)
		{
			sv_cheats->current.enabled = false;
		}
	}

	void demo_playback_t::repeat()
	{
		assert(post_map_header_file_offset_ < 256);
		++restart_count;

		if (auto& connstate = *game::mp::connstate; connstate == game::CA_ACTIVE)
		{
			// prevent old snapshots from being processed; would mess up the server command sequences
			connstate = game::CA_PRIMED;
		}

		// reset game data
		game::mp::clc->serverCommandSequence = {};
		game::mp::clc->lastExecutedServerCommand = {};
		game::mp::cgs->serverCommandSequence = {};
		get_persistent_data() = {};

		if (auto* demo_timescale = game::Dvar_FindVar("demotimescale"); demo_timescale)
		{
			if (!retain_timescale_)
			{
				demo_timescale->current.value = 1.0f;
			}
		}

		// reset class data
		skip_timeout_ = {};
		retain_timescale_ = {};
		pred_data_index_ = {};
		forward_seek.pending = false; // a jump forward that ran into the end of the demo

		file_.clear();
		file_.seekg(post_map_header_file_offset_, std::ios::beg);
	}
}

namespace // hooks
{
	utils::hook::detour CG_UpdateOmnvars_hook;
	utils::hook::detour CG_VisionSetClientSide_hook; // not the engine's name, which is unknown
	utils::hook::detour CG_VisionSetStartLerp_To_hook;
	utils::hook::detour CL_GetPredictedPlayerInformationForServerTime_hook;
	utils::hook::detour CL_GetPredictedVehicleForServerTime_hook;
	utils::hook::detour CL_SetCGameTime_hook;
	utils::hook::detour CL_WritePacket_hook;
	utils::hook::detour GetRemoteEyeValues_hook;
	utils::hook::detour LUI_FeedMessage_hook;  // not the engine's names, which are unknown: the functions that
	utils::hook::detour LUI_FeedObituary_hook; // send the LUI its "game_message" and "obituary" events
	utils::hook::detour LUI_IntermissionBegan_hook;
	utils::hook::detour VehicleCam_UpdatePlayerControlCam_hook;

	game::dvar_t* demo_vision;

	static constexpr std::array VISION_SETS{
		"disabled", "no vision", "default",

		"ac130", "ac130_enhanced_mp", "ac130_inverted", "aftermath", "aftermath_glow", "aftermath_post", "black_bw",
		"cheat_bw", "coup_sunblind", "default_night", "default_night_mp", "end_game", "missilecam", "mpintro",
		"mpnuke", "mpnuke_aftermath", "mpoutro", "near_death", "near_death_mp", "nuke_global_flash", "thermal_mp", "thermal_snowlevel_mp",

		"aftermath_map", static_cast<const char*>(nullptr)
	};
	static_assert(
		std::string_view(VISION_SETS[0]) == "disabled" &&
		std::string_view(VISION_SETS[1]) == "no vision" &&
		std::string_view(VISION_SETS[2]) == "default" &&
		std::string_view(VISION_SETS[VISION_SETS.size() - 2]) == "aftermath_map" &&
		VISION_SETS.back() == nullptr // enum dvars require a nullptr as last value
		);

	bool handle_vision_override(game::mp::ClientVisionSetData& cvs_data, std::int32_t index, const char* vision_name, std::int32_t duration, std::int32_t time)
	{
		if (demo.is_demo_playing())
		{
			if (seek_vision_instant() && duration > 1)
			{
				duration = 1; // a change the seek skipped: no lerp from where the film lands (1 as in handle_vision)
			}

			const auto vision_set = static_cast<std::size_t>((demo_vision) ? demo_vision->current.integer : 0);

			if (vision_set == 1)
			{
				// enable 'post apply' vision
				cvs_data.visionSetLerpData[index].style = 1;
				cvs_data.postApplyLerpDest = 1.0f;

				return true;
			}
			else if (vision_set >= 2 && vision_set + 2 <= VISION_SETS.size())
			{
				// enable 'post apply' vision
				cvs_data.visionSetLerpData[index].style = 0;
				cvs_data.postApplyLerpDest = 1.0f;

				if (vision_set + 2 == VISION_SETS.size())
				{
					if (const auto mapname = get_mapname(false); mapname.empty())
					{
						return CG_VisionSetStartLerp_To_hook.invoke<bool>(&cvs_data, index, "aftermath_post", duration, time);
					}
					else
					{
						// a small number of maps have a vision called 'aftermath_mp_mapname'
						return CG_VisionSetStartLerp_To_hook.invoke<bool>(&cvs_data, index, std::format("aftermath_{}", mapname).c_str(), duration, time);
					}
				}
				else
				{
					return CG_VisionSetStartLerp_To_hook.invoke<bool>(&cvs_data, index, VISION_SETS[vision_set], duration, time);
				}
			}
		}

		return CG_VisionSetStartLerp_To_hook.invoke<bool>(&cvs_data, index, vision_name, duration, time);
	}

	// The client side vision set is the map's own one, picked by its client triggers (this runs every frame, and does
	// nothing while the vision set stays the same). A named vision set from the server switches it off (the black and
	// white 'mpIntro' of the pre-match), an empty name switches it on again with the lerp time the server gives (3 s seen
	// in game). A negative duration keeps the time of the trigger. It never passes through CG_VisionSetStartLerp_To.
	void handle_client_side_vision(const std::int32_t client_num, const std::int32_t index, const std::int32_t trigger_from,
		const std::int32_t trigger_to, const float blend, std::int32_t duration)
	{
		if (demo.is_demo_playing() && seek_vision_instant())
		{
			duration = 0; // see seek_vision_frames
		}

		CG_VisionSetClientSide_hook.invoke<void>(client_num, index, trigger_from, trigger_to, blend, duration);
	}

	// The feed lines of the part of the film a seek skips (see seek_skips_feed): a game message (CG_GameMessage and
	// CG_BoldGameMessage, the server's iprintln lines such as "<name> Connected") and a kill.
	void handle_feed_message(const std::int32_t controller, const char* message, const bool bold, void* lua_vm)
	{
		if (demo.is_demo_playing() && seek_skips_feed())
		{
			return;
		}

		LUI_FeedMessage_hook.invoke<void>(controller, message, bold, lua_vm);
	}

	void handle_feed_obituary(const std::int32_t controller, const std::int32_t attacker, const std::int32_t victim,
		void* icon, const bool flag, void* lua_vm)
	{
		if (demo.is_demo_playing() && seek_skips_feed())
		{
			return;
		}

		LUI_FeedObituary_hook.invoke<void>(controller, attacker, victim, icon, flag, lua_vm);
	}

	// The splashes are per-client omnvars: "First Blood!", an earned killstreak, a challenge (splasheshud.lua) and the
	// team emblem with the objective when the match starts (matchstarthud.lua). CG_UpdateOmnvars tells the LUI about an
	// omnvar whenever its time of change is not the one the client remembers, whatever happened in between: after a
	// seek, or when another player's view is selected, the last splash of the skipped part came up again however long
	// ago it had been ("First Blood!" minutes later, seen in game). A splash that is more than a second older than the
	// snapshot that brings it is remembered without telling the LUI; while a film plays normally it arrives within a
	// snapshot or two.
	constexpr std::array<std::string_view, 5> SPLASH_OMNVARS{
		"ui_splash_idx", "ui_splash_killstreak_idx", "ui_splash_playercard_idx", "ui_challenge_splash_idx", "ui_objective_text",
	};
	constexpr std::int32_t SPLASH_MAX_AGE_MSEC = 1000;
	constexpr std::uintptr_t OMNVAR_GLOBAL_COUNT = 0x147A5FA10; // the snapshot's omnvars are [global | per client]
	constexpr std::uintptr_t OMNVAR_CLIENT_COUNT = 0x147A5FA14;
	constexpr std::uintptr_t OMNVAR_CLIENT_CACHE = 0x147A63280; // OmnvarData the client remembers, indexed like the snapshot's

	// snapshot indices of SPLASH_OMNVARS, by name from the game's omnvar definitions (name pointer first)
	std::span<const std::size_t> splash_omnvar_indices()
	{
		static std::array<std::size_t, SPLASH_OMNVARS.size()> indices{};
		static std::size_t count = 0;
		static auto resolved = false;

		if (!resolved)
		{
			resolved = true;

			const auto global = *reinterpret_cast<const std::int32_t*>(OMNVAR_GLOBAL_COUNT);
			const auto client = *reinterpret_cast<const std::int32_t*>(OMNVAR_CLIENT_COUNT);
			if (global < 0 || client <= 0 || global + client > static_cast<std::int32_t>(std::extent_v<decltype(game::mp::snapshot_t::omnvars)>))
			{
				return {};
			}

			for (auto i = global; i < global + client && count < indices.size(); ++i)
			{
				const auto* def = game::Omnvar_GetDef(i);
				const auto* name = def && utils::hook::is_readable(def, sizeof(const char*)) ? *static_cast<const char* const*>(def) : nullptr;
				if (!name || !utils::hook::is_readable(name, 64))
				{
					count = 0;
					break;
				}

				if (std::ranges::find(SPLASH_OMNVARS, std::string_view(name, strnlen(name, 64))) != SPLASH_OMNVARS.end())
				{
					indices[count++] = static_cast<std::size_t>(i);
				}
			}
		}

		return {indices.data(), count};
	}

	void forget_stale_splashes(const std::int32_t client_num, const game::mp::snapshot_t& new_snap)
	{
		if (client_num != 0)
		{
			return;
		}

		auto* const remembered = reinterpret_cast<game::mp::OmnvarData*>(OMNVAR_CLIENT_CACHE);
		for (const auto index : splash_omnvar_indices())
		{
			const auto& splash = new_snap.omnvars[index];
			if (splash.timeModified != remembered[index].timeModified && (splash.timeModified == 0
				|| new_snap.serverTime - static_cast<std::int32_t>(splash.timeModified) > SPLASH_MAX_AGE_MSEC))
			{
				remembered[index] = splash;
			}
		}
	}

	void handle_vision()
	{
		if (demo_vision && demo_vision->modified)
		{
			const auto vision_set = demo_vision->current.integer;

			for (auto i = 0; i < game::CS_VISIONSET_COUNT; ++i)
			{
				const std::string_view vision_name = (!vision_set)
					? game::mp::cg->cvsData.visionName[i]
					: "";

				handle_vision_override(game::mp::cg->cvsData, i, vision_name.data(), 1, game::mp::cl->serverTime);
			}

			if (!vision_set)
			{
				// disable 'post apply' vision if the game hasn't used it yet
				if (game::mp::cg->cvsData.visionName[5][0] == '\0')
				{
					game::mp::cg->cvsData.postApplyLerpDest = 0.0f;
				}
			}
			else if (vision_set == 1)
			{
				std::memset(&game::mp::cg->cvsData.visionSetCurrent, 0, sizeof(game::mp::cg->cvsData.visionSetCurrent));
			}

			// reset modified flag to avoid doing the same work each frame
			demo_vision->modified = false;
		}
	}

	void handle_fast_forward(auto old_connstate)
	{
		if (const auto new_connstate = *game::mp::connstate; new_connstate == game::CA_ACTIVE)
		{
			const auto server_time = game::mp::cl->serverTime;
			const auto snap_svr_time = game::mp::cl->snap.serverTime;

			demo.handle_fast_forward([old_connstate, server_time, snap_svr_time](
				std::optional<std::int32_t>& rewind_snap_svr_time, bool& skip_timeout)
			{
				if (old_connstate == game::CA_PRIMED)
				{
					if (rewind_snap_svr_time)
					{
						// handle fast forward after rewinding
						if (*rewind_snap_svr_time > snap_svr_time)
						{
							fast_forward_demo(*rewind_snap_svr_time - snap_svr_time);
						}

						rewind_snap_svr_time = {};
					}
				}
				else if (old_connstate == game::CA_ACTIVE && !skip_timeout)
				{
					if (const auto delta_time = snap_svr_time - server_time; delta_time >= 1000)
					{
						// skip timeout between first and second snapshot (for user demos)
						fast_forward_demo(delta_time);
						skip_timeout = true;
					}
				}
			});
		}
	}

	void handle_demo_patches()
	{
		// set cg_t->demoType to DEMO_TYPE_CLIENT to disable mouse input and avoid frame interpolation issues
		game::mp::cg->demoType = game::mp::DEMO_TYPE_CLIENT;

		// ensure that sv_cheats is always enabled during demo playback
		if (auto* sv_cheats = game::Dvar_FindVar("sv_cheats"); sv_cheats)
		{
			sv_cheats->current.enabled = true;
		}

		// sometimes the hud isn't updated after a weapon change; force update the weapon name and ammo (clip) when changing weapons
		// various other Call of Duty games are also affected by this bug
		if (game::mp::cg->weaponSelect.data != static_cast<std::size_t>(game::mp::cg->ps.weapon))
		{
			game::CG_SelectWeapon(0, game::mp::cg->ps.weapon, 0);
		}
	}

	void handle_demo_reading(const std::int32_t client_num)
	{
		if (game::Live_SyncOnlineDataFlags(0) || !demo.is_demo_playing())
		{
			// set demoType to default value
			// normally the game does this unconditionally, but that code has been removed
			game::mp::cg->demoType = game::mp::DEMO_TYPE_NONE;

			CL_SetCGameTime_hook.invoke<void>(client_num);
		}
		else
		{
			const auto old_connstate = *game::mp::connstate;
			if (old_connstate == game::CA_ACTIVE)
			{
				// reset cl->newSnapshots to prevent CL_SetCGameTime from making a call to CL_AdjustTimeDelta,
				// which interferes with the timescale
				game::mp::cl->newSnapshots = 0;

				if (seek_vision_frames > 0)
				{
					--seek_vision_frames;
				}
			}

			CL_SetCGameTime_hook.invoke<void>(client_num);

			handle_fast_forward(old_connstate);
			handle_vision();
			handle_demo_patches();

			demo.parse_demo_wrapper();
		}
	}

	void write_packet(const std::int32_t client_num)
	{
		if (!demo.is_demo_playing())
		{
			CL_WritePacket_hook.invoke<void>(client_num);
		}
	}

	bool get_predicted_player_data(const game::mp::clientActive_t& cl, std::int32_t ps_time, game::mp::playerState_t& ps)
	{
		if (!demo.is_demo_playing())
		{
			return CL_GetPredictedPlayerInformationForServerTime_hook.invoke<bool>(&cl, ps_time, &ps);
		}

		// hide regular hud when showing special hud for killstreak reward
		// must not unset this 15th bit when dead to avoid a crash in CG_SimulateBulletFire
		ps.otherFlags = (ps.pm_type == 7 || ps.remoteControlEnt != 2047 || ps.vehicleState.entity != 2047)
			? ps.otherFlags | 0x4000
			: ps.otherFlags & ~0x4000;

		demo.use_predicted_data([&ps, ps_time](const std::array<predicted_data_t, 256>& pred_data, std::size_t pred_data_index)
		{
			static constexpr auto pred_data_size = std::tuple_size_v<std::remove_reference_t<decltype(pred_data)>>;

			// find most recent predicted player data
			for (std::size_t count = 0, index = pred_data_index - 1; count < pred_data_size; ++count, --index)
			{
				const auto& p_data = pred_data[index % pred_data_size];
				if (p_data.cad.serverTime <= ps_time && ps_time - p_data.cad.serverTime < 500)
				{
					std::memcpy(&ps.origin[0], &p_data.cad.origin[0], sizeof(ps.origin));
					std::memcpy(&ps.velocity[0], &p_data.cad.velocity[0], sizeof(ps.velocity));
					std::memcpy(&ps.viewangles[0], &p_data.viewangles[0], sizeof(ps.viewangles));
					ps.bobCycle = p_data.cad.bobCycle;
					ps.movementDir = p_data.cad.movementDir;

					break;
				}
			}
		});

		// must return true otherwise branch is taken that undoes the work of this function
		return true;
	}

	bool get_predicted_vehicle_data(const game::mp::clientActive_t& cl, std::int32_t ps_time, game::PlayerVehicleState& ps_vehicle)
	{
		if (!demo.is_demo_playing())
		{
			return CL_GetPredictedVehicleForServerTime_hook.invoke<bool>(&cl, ps_time, &ps_vehicle);
		}

		demo.use_predicted_data([&ps_vehicle, ps_time](const std::array<predicted_data_t, 256>& pred_data, std::size_t pred_data_index)
		{
			static constexpr auto pred_data_size = std::tuple_size_v<std::remove_reference_t<decltype(pred_data)>>;

			// find most recent predicted vehicle data
			for (std::size_t count = 0, index = pred_data_index - 1; count < pred_data_size; ++count, --index)
			{
				const auto& p_data = pred_data[index % pred_data_size];
				if (p_data.cad.serverTime <= ps_time && ps_time - p_data.cad.serverTime < 500)
				{
					const auto org_entity = ps_vehicle.entity;
					ps_vehicle = p_data.cad.playerVehStateClientArchive;
					ps_vehicle.entity = org_entity;

					break;
				}
			}
		});

		// must return true otherwise branch is taken that undoes the work of this function
		return true;
	}

	bool show_intermission_menu(const std::int32_t client_num, void* lua_vm)
	{
		// it appeared easier to prevent the intermission menu from being shown instead of closing or hiding it

		if (!demo.is_demo_playing() || !demo.is_demo_repeating())
		{
			return LUI_IntermissionBegan_hook.invoke<bool>(client_num, lua_vm);
		}

		// rewind demo and don't display intermission menu for repeated demos
		static_cast<void>(demo.rewind_demo(std::nullopt));

		return false;
	}

	void control_ui_element(const std::int32_t client_num, const game::mp::snapshot_t& old_snap, game::mp::snapshot_t& new_snap)
	{
		// it appeared easier to prevent these menus from being shown instead of closing or hiding them

		if (demo.is_demo_playing())
		{
			// skip team selection menu (and loadout selection menu)
			assert(std::string_view(*reinterpret_cast<const char**>(game::Omnvar_GetDef(103))) == "ui_options_menu");
			new_snap.omnvars[103] = {};

			if (demo.is_demo_repeating())
			{
				// skip extinction mode end-of-game score / stats menu
				assert(std::string_view(*reinterpret_cast<const char**>(game::Omnvar_GetDef(194))) == "ui_alien_show_eog_score");
				new_snap.omnvars[194] = {};
			}
		}

		// a jump forward has landed once a snapshot around its target is here (see seek_skips_feed)
		if (forward_seek.pending && (new_snap.serverTime >= forward_seek.target - REWIND_HUD_LANDED_MSEC
			|| std::chrono::steady_clock::now() - forward_seek.requested > REWIND_HUD_TIMEOUT))
		{
			forward_seek.pending = false;
		}

		// iw6-theater: the snapshots between a rewind's restart and its target never reach the HUD (see rewind_hud)
		if (rewind_hud.pending && demo.is_demo_playing() && restart_count != rewind_hud.restarts_at_request)
		{
			if (!rewind_hud.have_base)
			{
				rewind_hud.base = old_snap;
				rewind_hud.have_base = true;
			}

			const auto timed_out = std::chrono::steady_clock::now() - rewind_hud.requested > REWIND_HUD_TIMEOUT;
			if (!timed_out && new_snap.serverTime < rewind_hud.target - REWIND_HUD_LANDED_MSEC)
			{
				return;
			}

			reset_rewind_hud();
			seek_vision_frames = SEEK_VISION_FRAMES; // the server commands of the landing can still follow this call
			forget_stale_splashes(client_num, new_snap);
			CG_UpdateOmnvars_hook.invoke<void>(client_num, &rewind_hud.base, &new_snap);
			return;
		}

		if (demo.is_demo_playing())
		{
			forget_stale_splashes(client_num, new_snap);
		}

		CG_UpdateOmnvars_hook.invoke<void>(client_num, &old_snap, &new_snap);
	}

	std::int32_t update_gamestate_config_strings(game::msg_t& msg)
	{
		if (demo.is_demo_playing())
		{
			// update command sequences and overwrite config strings if that data is available
			if (const auto& gs = demo.get_gamestate(); gs)
			{
				game::mp::cls->gameState = gs->data;
				game::mp::clc->serverCommandSequence = gs->svr_cmd_seq;
				game::mp::clc->lastExecutedServerCommand = gs->svr_cmd_seq;
				game::mp::cgs->serverCommandSequence = gs->svr_cmd_seq;
			}
			else
			{
				assert(is_gamestate_valid(game::mp::cls->gameState, false));

				game::mp::clc->lastExecutedServerCommand = game::mp::clc->serverCommandSequence;
				game::mp::cgs->serverCommandSequence = game::mp::clc->serverCommandSequence;
			}
		}

		// call MSG_ReadLong that this hook replaces
		return game::MSG_ReadLong(&msg);
	}

	void ignore_mouse_input_killstreak_reward_1(const std::int32_t client_num, game::mp::cg_t& cg, game::vec3_t& out_view_origin, game::vec3_t& out_view_angles)
	{
		auto& in_killcam = cg.inKillCam;

		if (!demo.is_demo_playing() || in_killcam)
		{
			VehicleCam_UpdatePlayerControlCam_hook.invoke<void>(client_num, &cg, &out_view_origin, &out_view_angles);
		}
		else
		{
			// required for e.g. the Gryphon killstreak reward
			in_killcam = 1;
			VehicleCam_UpdatePlayerControlCam_hook.invoke<void>(client_num, &cg, &out_view_origin, &out_view_angles);
			in_killcam = 0;
		}
	}

	bool ignore_mouse_input_killstreak_reward_2(const std::int32_t client_num)
	{
		auto& other_flags = game::mp::cg->ps.otherFlags;

		if (!demo.is_demo_playing() || !(other_flags & 0x4000))
		{
			return GetRemoteEyeValues_hook.invoke<bool>(client_num);
		}
		else
		{
			// required for e.g. the Trinity Rocket killstreak reward
			other_flags &= ~0x4000;
			const auto result = GetRemoteEyeValues_hook.invoke<bool>(client_num);
			other_flags |= 0x4000;

			return result;
		}
	}
}

namespace // command execution
{
	bool execute_playback_command_internal(std::filesystem::path& file_path, bool repeat, bool print_error)
	{
		if (demo.start(file_path, repeat))
		{
			return true;
		}

		if (print_error)
		{
			console::error("could not play demo %s\n", file_path.string().c_str());
		}

		return false;
	}

	bool execute_playback_command_internal(std::string_view file_name, bool repeat)
	{
		const auto fs_basepath = get_base_path(false);
		if (fs_basepath.empty())
		{
			console::error("could not find fs_basepath\n");
			return false;
		}

		const auto mod = get_mod_directory();

		auto file_path1 = std::filesystem::path(fs_basepath) / std::format("demos/client/{2}/user/{0}{1}", file_name, DEMO_EXTENSION, mod);
		if (execute_playback_command_internal(file_path1, repeat, false))
		{
			return true;
		}

		auto file_path2 = std::filesystem::path(fs_basepath) / std::format("demos/client/{2}/auto/{0}{1}", file_name, DEMO_EXTENSION, mod);
		if (execute_playback_command_internal(file_path2, repeat, false))
		{
			return true;
		}

		auto file_path3 = std::filesystem::path(fs_basepath) / std::format("demos/client/{0}{1}", file_name, DEMO_EXTENSION);
		if (execute_playback_command_internal(file_path3, repeat, false))
		{
			return true;
		}

		auto file_path4 = std::filesystem::path(fs_basepath) / std::format("demos/server/{2}/{0}{1}", file_name, DEMO_EXTENSION, mod);
		if (execute_playback_command_internal(file_path4, repeat, false))
		{
			return true;
		}

		auto file_path5 = std::filesystem::path(fs_basepath) / std::format("demos/server/{0}{1}", file_name, DEMO_EXTENSION);
		if (execute_playback_command_internal(file_path5, repeat, false))
		{
			return true;
		}

		auto file_path6 = std::filesystem::path(fs_basepath) / std::format("demos/{0}{1}", file_name, DEMO_EXTENSION);
		if (execute_playback_command_internal(file_path6, repeat, false))
		{
			return true;
		}

		console::error("could not play demo %s\n", file_path1.string().c_str());
		console::error("could not play demo %s\n", file_path2.string().c_str());
		console::error("could not play demo %s\n", file_path3.string().c_str());
		console::error("could not play demo %s\n", file_path4.string().c_str());
		console::error("could not play demo %s\n", file_path5.string().c_str());
		console::error("could not play demo %s\n", file_path6.string().c_str());

		return false;
	}

	template <bool force_playback>
	bool execute_playback_command(const command::params& params)
	{
		struct args_t
		{
			bool fullpath{};
			bool repeat{};
		};

		const auto parsed_arguments = [&params]() -> std::optional<args_t>
		{
			if (params.size() == 3)
			{
				if (std::string_view(params.get(2)) == "fullpath")
				{
					return args_t{ true, false };
				}
				else if (std::string_view(params.get(2)) == "repeat")
				{
					return args_t{ false, true };
				}
				else
				{
					return {};
				}
			}
			else if (params.size() == 4)
			{
				if (std::string_view(params.get(2)) == "fullpath" && std::string_view(params.get(3)) == "repeat")
				{
					return args_t{ true, true };
				}
				else
				{
					return {};
				}
			}
			else
			{
				return args_t{ false, false };
			}
		}();

		if (params.size() < 2 || params.size() > 4 || !parsed_arguments)
		{
			console::info("usage: demoplay <file_name>, fullpath(optional), repeat(optional)\n");
			return false;
		}

		if constexpr (!force_playback)
		{
			if (!demo.is_demo_playing() && game::CL_IsCgameInitialized())
			{
				console::warn("could not play demo; please disconnect first\n");
				return false;
			}
		}

		if (!parsed_arguments->fullpath)
		{
			return execute_playback_command_internal(params.get(1), parsed_arguments->repeat);
		}
		else
		{
			auto file_path = std::filesystem::path(params.get(1));
			return execute_playback_command_internal(file_path, parsed_arguments->repeat, true);
		}
	}

	void execute_playback_instant_play_command(const command::params& params)
	{
		// the demo recording code (demo_recording_t::shutdown_watcher) should take care of properly closing any recording demo files!

		const auto demo_playing = demo.is_demo_playing();
		if (execute_playback_command<true>(params))
		{
			if (auto* sv_running = game::Dvar_FindVar("sv_running"); sv_running && sv_running->current.enabled)
			{
				// demo cannot play if local server is still considered to be running
				sv_running->current.enabled = false;
			}

			if (!demo_playing)
			{
				if (auto& connstate = *game::mp::connstate; connstate == game::CA_ACTIVE)
				{
					// prevent old snapshots from being processed,
					// would mess up the server command sequences and server times
					connstate = game::CA_PRIMED;
				}
			}
		}
	}

	void execute_playback_replay_command(const command::params& params)
	{
		if (params.size() != 1)
		{
			console::info("usage: demoreplay\n");
			return;
		}

		if (const auto path = demo.restart(); path)
		{
			console::error("could not play demo %s\n", path->get().string().c_str());
		}
	}

	void execute_playback_rewind_command(const command::params& params)
	{
		if (params.size() < 1 || params.size() > 2)
		{
			console::info("usage: demorewind <milliseconds>(optional)\n");
			return;
		}

		if (params.size() == 2)
		{
			const auto msec = std::atoi(params.get(1));
			if (msec <= 0)
			{
				console::error("demorewind invalid value %s\n", params.get(1));
				return;
			}

			if (!demo.rewind_demo(msec))
			{
				console::error("demo must be playing to rewind\n");
			}
		}
		else if (!demo.rewind_demo(std::nullopt))
		{
			console::error("demo must be playing to rewind\n");
		}
	}

	void execute_playback_fast_forward_command(const command::params& params)
	{
		if (params.size() != 2)
		{
			console::info("usage: demoforward <milliseconds>\n");
			return;
		}

		const auto msec = std::atoi(params.get(1));
		if (msec <= 0)
		{
			console::error("demoforward invalid value %s\n", params.get(1));
			return;
		}

		if (demo.is_demo_playing())
		{
			if (msec >= SEEK_VISION_MIN_MSEC)
			{
				seek_vision_frames = SEEK_VISION_FRAMES;

				if (*game::mp::connstate == game::CA_ACTIVE) // fast_forward_demo does nothing otherwise
				{
					arm_forward_seek(game::mp::cl->serverTime + msec);
				}
			}

			fast_forward_demo(static_cast<std::uint32_t>(msec));
		}
		else
		{
			console::error("demo must be playing to fast forward\n");
		}
	}
}

namespace demo_playback
{
	bool playing()
	{
		return demo.is_demo_playing();
	}

	std::uint32_t restarts()
	{
		return restart_count;
	}

	bool startplay(std::string_view file_name, bool repeat)
	{
		return execute_playback_command_internal(file_name, repeat);
	}

	bool startplay(std::filesystem::path& file_name, bool repeat)
	{
		return execute_playback_command_internal(file_name, repeat, true);
	}

	void stopplay()
	{
		demo.stop();
	}

	std::filesystem::path get_path()
	{
		return demo.get_path();
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

			// primary hook for demo playback; read demo data after server time has been updated
			CL_SetCGameTime_hook.create(game::CL_SetCGameTime, handle_demo_reading);

			// don't write packets when playing a demo
			CL_WritePacket_hook.create(game::CL_WritePacket, write_packet);

			// replace functions to get predicted data, because they skip the viewangles
			CL_GetPredictedPlayerInformationForServerTime_hook.create(
				game::CL_GetPredictedPlayerInformationForServerTime, get_predicted_player_data);
			CL_GetPredictedVehicleForServerTime_hook.create(
				game::CL_GetPredictedVehicleForServerTime, get_predicted_vehicle_data);

			// ignore mouse input when updating the camera for certain killstreak rewards
			VehicleCam_UpdatePlayerControlCam_hook.create(
				game::VehicleCam_UpdatePlayerControlCam, ignore_mouse_input_killstreak_reward_1);
			GetRemoteEyeValues_hook.create(
				game::GetRemoteEyeValues, ignore_mouse_input_killstreak_reward_2);

			// optional skip of intermission menu
			LUI_IntermissionBegan_hook.create(game::LUI_IntermissionBegan, show_intermission_menu);

			// don't open any menu when demo playback has started
			CG_UpdateOmnvars_hook.create(game::CG_UpdateOmnvars, control_ui_element);

			// keep the feed lines of the part of the film a seek skips away from the HUD
			LUI_FeedMessage_hook.create(0x1401CD410, handle_feed_message);
			LUI_FeedObituary_hook.create(0x1401D0CB0, handle_feed_obituary);

			// allow user to override visual settings
			CG_VisionSetStartLerp_To_hook.create(0x1402A3200, handle_vision_override);
			CG_VisionSetClientSide_hook.create(0x140296CA0, handle_client_side_vision);
			demo_vision = game::Dvar_RegisterEnum(
				"demovision", const_cast<const char**>(VISION_SETS.data()), 0, game::DVAR_FLAG_NONE, "Override vision set for demos");

			// update the gamestate configs strings with 'live' data from the demo (in CL_ParseGamestate)
			utils::hook::call(0x1402CCD56, update_gamestate_config_strings);

			// don't let the game set cg_t->demoType in CG_DrawActiveFrame, 
			// otherwise demo playback will have mouse input and suffer from frame interpolation issues
			utils::hook::nop(0x14029981C, 6);

			// replace thread local storage lookup for cg.bgs in function CG_EmissiveBlendCommand with hardcoded address
			// when CG_ExecuteNewServerCommands is called manually (e.g. from demo_utils::process_server_commands),
			// this address in tls may be a nullptr and the game could crash here (in extinction mode at least)
			static constexpr auto patch_address = 0x14028844B;
			static constexpr std::array<std::uint8_t, 7> patch{ 0x48, 0x8D, 0x1D, 0x68, 0xBC, 0x59, 0x01 };

			utils::hook::nop(patch_address, 0xE);
			utils::hook::nop(patch_address + 0x1C, 4);
			utils::hook::nop(patch_address + 0x22, 4);
			utils::hook::copy(patch_address + 0xE, patch.data(), patch.size()); // lea rbx, [0x1418240C8] (point RBX to cg.bgs)
			assert(std::bit_cast<std::size_t>(&game::mp::cg->bgs) ==
				patch_address + 0xE + patch.size() + *reinterpret_cast<const std::uint32_t*>(patch_address + 0x11));

			// add console command support to play demos
			command::add("demoplay", execute_playback_command<false>);

			// add experimental command console support to play demos,
			// skip disconnecting from local or online server and skip map (re)loading
			command::add("demoinstantplay", execute_playback_instant_play_command);

			// replay most recent demo (if any)
			command::add("demoreplay", execute_playback_replay_command);

			// rewind demo (by N msec)
			command::add("demorewind", execute_playback_rewind_command);

			// fast forward demo by N msec
			command::add("demoforward", execute_playback_fast_forward_command);

			// add command line support to play demos; this makes it possible to double click on a demo file to play it
			const auto& args = utils::command_line::get_args();
			for (const auto& arg : args)
			{
				if (arg.extension() == DEMO_EXTENSION)
				{
					scheduler::on_game_initialized([cmd = std::format(R"(demoplay "{}" fullpath repeat)", arg.string())]() mutable
					{
						command::execute(std::move(cmd), true);
					}, scheduler::main);

					console::info("the demo should start to load in a few seconds\n");
					break;
				}
			}
		}
	};
}

REGISTER_COMPONENT(demo_playback::component)
