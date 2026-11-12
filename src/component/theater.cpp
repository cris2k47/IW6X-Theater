#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "theater.hpp"
#include "theater_math.hpp"
#include "theater_markers.hpp"
#include "theater_world.hpp"
#include "theater_screenshot.hpp"
#include "theater_video.hpp"
#include "theater_capture.hpp"
#include "demo_timescale.hpp"
#include "demo_pov.hpp"

#include "command.hpp"
#include "console.hpp"
#include "demo_playback.hpp"
#include "scheduler.hpp"
#include "theater_gamepad.hpp"

#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/string.hpp"

// BO2-style theater (Black Ops II "Film Editor") on top of the demo system.
//
// Cameras: 1ST PERSON / 3RD PERSON of any player, FREE CAMERA with three modes (Free Roam, Dolly Camera,
// Edit Camera Mode). Keyboard and controller bindings copy BO2 on PC (Plutonium T6) exactly:
//   F4 / L3 toggle controls, F2 / B toggle HUD, F3 / Y camera, Mouse1-2 / LB-RB player or free camera mode,
//   Space / A play-pause, Up-Down speed 0.1x-6.0x (0.2x steps), Dpad up-down speed in 0.1x steps (BO2 controller
//   layout 2), Left-Right / LT-Dpad right jump to 10 s marks, RT (analog) fast forward, R / X record a clip,
//   hold R / hold X screenshot, F / R3 camera markers (drawn like BO2 by theater_markers.cpp).
//
// Camera override: CG_CalcViewValues (0x14029F980, called from CG_DrawActiveFrame) fills cg->refdef and
// cg->refdefViewAngles. After it returns, the theater writes its own origin/axis/fov into the refdef, so
// everything that consumes the view afterwards (FX camera, sound listener, culling, renderer) uses it. The level
// of detail of the models is the exception: its origin is set at the start of the frame (set_lod_origin_stub).

namespace theater
{
	using namespace math;

	namespace
	{
		// ------------------------------------------------------------------ engine data (iw6mp64_ship.exe 3.15)

		constexpr std::uintptr_t CG_CALC_VIEW_VALUES = 0x14029F980;
		constexpr std::uintptr_t R_SET_LOD_ORIGIN = 0x14060D490; // void(const refdef_t* refdef, float lodScale)
		constexpr std::uintptr_t FOV_TO_TANGENTS = 0x1404E2A80; // void(float fov, float aspect, float* tanX, float* tanY)
		constexpr std::uintptr_t CG_ARRAY = 0x14176EC00;

		constexpr std::uintptr_t REFDEF_OFFSET = 0x718C8; // cg->refdef
		constexpr std::uintptr_t CGS_VIEW_ASPECT = 0x14187EB90;
		constexpr std::uintptr_t CGS_VIEW_ASPECT_SCALE = 0x14187EB94;

		struct refdef_view
		{
			std::int32_t x, y, width, height;
			float tan_half_fov_x, tan_half_fov_y;
			float vieworg[3];
			float viewaxis[3][3];
		};
		static_assert(offsetof(refdef_view, tan_half_fov_x) == 0x10);
		static_assert(offsetof(refdef_view, vieworg) == 0x18);
		static_assert(offsetof(refdef_view, viewaxis) == 0x24);

		refdef_view* get_refdef()
		{
			return reinterpret_cast<refdef_view*>(reinterpret_cast<std::uint8_t*>(game::mp::cg.get()) + REFDEF_OFFSET);
		}

		void fov_to_tangents(const float fov, float* tan_x, float* tan_y)
		{
			const auto aspect = *reinterpret_cast<float*>(CGS_VIEW_ASPECT) * *reinterpret_cast<float*>(CGS_VIEW_ASPECT_SCALE);
			reinterpret_cast<void(*)(float, float, float*, float*)>(FOV_TO_TANGENTS)(fov, aspect, tan_x, tan_y);
		}

		double tangent_to_fov(const float tan_half_fov_y)
		{
			// inverse of tanY = tan(fov / 2) * 0.75
			return rad2deg(2.0 * std::atan(tan_half_fov_y / 0.75));
		}

		// ------------------------------------------------------------------ BO2 constants

		// playback speed, kept in whole tenths so the steps stay exact: 0.1x .. 6.0x. Up / Down step 0.2x like BO2's
		// keyboard, but go down to 0.1x (BO2's keyboard stops at 0.2x); D-pad up / down step 0.1x like BO2's controller
		// layout 2, whose floor is 0.1x (static slow motion for cinematics)
		constexpr int MIN_SPEED_TENTHS = 1;
		constexpr int MAX_SPEED_TENTHS = 60;   // demo_maxTimeScale
		constexpr float MIN_SPEED = MIN_SPEED_TENTHS / 10.0f;
		constexpr float MAX_SPEED = MAX_SPEED_TENTHS / 10.0f;
		constexpr auto SPEED_REPEAT_DELAY = 350ms;    // D-pad up / down held: first repeat
		constexpr auto SPEED_REPEAT_INTERVAL = 90ms;  // then every
		constexpr auto LENS_TAP_TIME = 500ms;         // D-pad left released sooner without moving the right stick: reset the lens
		constexpr int JUMP_INTERVAL = 10000;   // Left / Right, LT / Dpad right: previous / next 10 s mark
		constexpr auto HOLD_TIME = 600ms;      // "Hold R" / "Hold X": screenshot
		constexpr double MARKER_HIGHLIGHT_DISTANCE = 40.0; // BO2 demo_dollycamHighlightThreshholdDistance: "Edit Camera Marker"
		constexpr double MARKER_HIGHLIGHT_BEHIND = 15.0;   // BO2: a marker up to 15 units behind the camera plane still counts
		constexpr double ORBIT_DISTANCE = 120.0;

		// ------------------------------------------------------------------ state

		utils::hook::detour cg_calc_view_values_hook;
		utils::hook::detour r_set_lod_origin_hook;
		utils::hook::detour cl_key_event_hook;
		utils::hook::detour cl_mouse_event_hook;

		game::dvar_t* theater_freecam_speed{};
		game::dvar_t* theater_hud{};
		game::dvar_t* theater_film{}; // read only: 1 while a film plays (the LUI pause menu then offers THEATER SETTINGS)
		game::dvar_t* theater_mouse_scale{};

		view_mode view = view_mode::first_person;
		free_mode fmode = free_mode::free_roam;
		int target = -1; // viewed player, -1 = the view the demo recorded
		bool was_active = false;

		// free camera
		vec3 free_pos{};
		vec3 free_vel{};
		double free_pitch{}, free_yaw{}, free_roll{};
		double free_fov = 65.0;

		// 3rd person orbit around the viewed player (relative to the player's yaw)
		double orbit_yaw{};
		double orbit_pitch = 12.0;

		// 1st person view from another player's eyes (no sidecar): smoothed eye and angles
		vec3 follow_eye{};
		vec3 follow_angles{};
		bool follow_valid = false;
		camera_pose last_pose{};
		bool last_pose_valid = false;
		bool camera_written = false; // the theater placed the camera in the last frame that was drawn (write_pose)

		// death hold (demo_pov::held_record): the dead camera stays where it was when the hold started
		constexpr int HOLD_POSE_SETTLE_FRAMES = 3;   // frames of a new hold (or after a seek) that still take the engine's pose
		constexpr int HOLD_POSE_JUMP_MSEC = 500;     // film time moved more than this in one frame: a seek
		camera_pose hold_pose{};
		std::int64_t hold_pose_record = -1;
		int hold_pose_target = -1;
		int hold_pose_settle = 0;
		int hold_pose_server_time = 0;

		std::array<bool, 256> keys_down{};
		std::atomic<int> mouse_dx{0};
		std::atomic<int> mouse_dy{0};

		std::vector<keyframe> keyframes;
		std::uint32_t keyframes_rev = 0; // changes whenever the markers do (theater_markers.cpp caches its track by it)
		demo_info info{};

		double smooth_time{};
		LARGE_INTEGER qpc_frequency{};
		LARGE_INTEGER last_camera_frame{};

		// playback speed
		bool paused = false;
		float selected_speed = 1.0f;
		bool speed_label = false; // BO2 shows the speed text after Up / Down
		float ff_speed = 0.0f;    // RT fast forward, 0 = off

		// a backward seek in progress: its frames are held (seek_relative, on_frame)
		constexpr int BACKWARD_SEEK_LANDED_MSEC = 200;   // the snapshot is this close to the target: landed (it is the first
		                                                 // one after the target, measured 50 ms later)
		constexpr int BACKWARD_SEEK_TRAILING_FRAMES = 2; // then still held: the render thread presents a frame a frame or two late
		constexpr auto BACKWARD_SEEK_TIMEOUT = 1500ms;   // the frames are let through by then in any case
		std::optional<int> backward_seek_target;
		std::uint32_t backward_seek_restarts = 0;
		std::chrono::steady_clock::time_point backward_seek_at{};

		// D-pad up / down held: the speed step repeats
		int speed_repeat_direction = 0;
		std::chrono::steady_clock::time_point speed_repeat_at{};

		// D-pad left held in the free camera: the right stick zooms and rolls; released quickly without that, a tap
		std::optional<std::chrono::steady_clock::time_point> lens_pad_down;
		bool lens_pad_used = false;

		input_device last_device = input_device::keyboard;
		std::chrono::steady_clock::time_point last_ui_focus{};

		popup_state popup{};
		bool repositioning = false;
		int reposition_marker = -1;

		// Free Roam "Attach To Object": the free camera follows a player, keeping its offset
		int attached = -1;
		vec3 attach_offset{};
		int candidate = -1;

		// R / X: press = RECORD, hold = SCREENSHOT
		std::optional<std::chrono::steady_clock::time_point> record_key_down;
		bool record_key_fired = false;
		std::optional<std::chrono::steady_clock::time_point> record_pad_down;
		bool record_pad_fired = false;

		// clip recording (BO2 RECORD): the camera of every frame, replayed exactly by "Preview Clip"
		struct clip_sample
		{
			double time{};
			view_mode view{};
			free_mode fmode{};
			int target{-1};
			camera_pose pose{};
			float timescale{1.0f};
		};

		std::vector<clip_sample> clip;
		bool clip_recording = false;
		std::vector<clip_sample> replay;
		bool replaying = false;
		bool replay_is_preview = false;

		// video rendering: a clip is replayed with a fixed-step clock and every frame is encoded into an MP4
		enum class render_phase
		{
			none,
			seeking,   // jumping to the clip start (with a short pre-roll)
			capturing, // frames are encoded
			finishing, // the file is being finalized
		};

		render_phase render = render_phase::none;
		std::filesystem::path render_path;
		double render_start{};
		double render_end{};
		int render_fps = 60;
		int render_tail = 0;
		int render_frames = 0; // frames run while capturing (main thread)
		int render_seek_frames = 0;
		double render_seek_target{};
		std::chrono::steady_clock::time_point render_capture_since{};
		std::chrono::steady_clock::time_point render_since{};
		std::optional<int> saved_maxfps;
		game::dvar_t* theater_video_fps{};
		game::dvar_t* theater_video_on_save{};

		std::unordered_map<std::string, std::string> saved_dvars;

		// ------------------------------------------------------------------ stats protection
		// Demos replay server commands that were meant for the recording player (XP, challenges, unlocks).
		// The local persistent stats of controller 0 are backed up when a theater session starts, stats
		// writes are blocked while it runs, and the backup is restored when it ends.

		constexpr std::uintptr_t PERSISTENT_STATS_BLOCK = 0x14459E874; // controller 0, all stats groups
		constexpr std::size_t PERSISTENT_STATS_BLOCK_SIZE = 0x20468;
		constexpr std::uintptr_t LIVESTORAGE_STATS_WRITE_NEEDED = 0x1404090E0;

		utils::hook::detour stats_write_needed_hook;
		std::vector<std::uint8_t> stats_backup;

		void backup_stats()
		{
			const auto* block = reinterpret_cast<const std::uint8_t*>(PERSISTENT_STATS_BLOCK);
			stats_backup.assign(block, block + PERSISTENT_STATS_BLOCK_SIZE);
		}

		void restore_stats()
		{
			if (stats_backup.size() == PERSISTENT_STATS_BLOCK_SIZE)
			{
				std::memcpy(reinterpret_cast<void*>(PERSISTENT_STATS_BLOCK), stats_backup.data(), PERSISTENT_STATS_BLOCK_SIZE);
			}

			stats_backup.clear();
		}

		void stats_write_needed_stub(const int controller)
		{
			if (demo_playback::playing())
			{
				return; // never schedule a stats upload while a demo is playing
			}

			stats_write_needed_hook.invoke<void>(controller);
		}

		// ------------------------------------------------------------------ render mute
		// The film runs several times faster than real time while a video is rendered (and the video has no
		// sound), so the game is muted meanwhile. snd_volume must not be touched for that: it is a profile
		// setting, the game puts it back right away and saves its config (with whatever other values are
		// temporary at that moment). Instead the final master volume the sound update computes every frame
		// (snd_volume * fades, read by every voice and stream) is forced to 0; nothing is saved anywhere.

		constexpr std::uintptr_t SND_UPDATE_MASTER_VOLUME = 0x140660ED0; // void (int msec), called by SND_Update every frame
		constexpr std::uintptr_t SND_MASTER_VOLUME = 0x14984AB5C;        // float, = fade * snd_volume * scale

		utils::hook::detour snd_update_master_volume_hook;
		std::atomic<bool> render_muted{false};

		void snd_update_master_volume_stub(const int msec)
		{
			snd_update_master_volume_hook.invoke<void>(msec);

			if (render_muted)
			{
				*reinterpret_cast<float*>(SND_MASTER_VOLUME) = 0.0f;
			}
		}

		void set_render_mute(const bool mute)
		{
			render_muted = mute;

			if (mute)
			{
				*reinterpret_cast<float*>(SND_MASTER_VOLUME) = 0.0f;
			}
			else if (auto* volume = game::Dvar_FindVar("snd_volume"))
			{
				volume->modified = true; // the next sound update computes the real master volume again
			}
		}

		// ------------------------------------------------------------------ film pause: game sound
		// BO2: while the film is paused the game's sounds are paused too, and they continue where they were when the
		// film plays again. The film is paused with demotimescale = 0 (Anomaly's Com_TimeScaleMsec hook then returns
		// 0 ms per frame), but that does not reach the sound: SND_Update (0x1406602F0) turns a sound timescale <= 0
		// into 1.0, so the sounds already playing (ambience, music, tails) went on at normal speed.
		//
		// The engine has its own sound pause, driven by cl_paused. Every SND_Update reads it after its gate (in MP the
		// gate always passes) and, when the value changed, calls SND_PauseSounds (0x1406598F0: every voice of a
		// pausable entchannel is held at its sample position) or SND_UnpauseSounds (0x140660260: the held voices
		// continue). Menu sounds (entchannels menu, menulim1, menulim2, musicnopause) are not pausable and keep playing.
		// While paused, SND_Update also skips the sound timescale, doppler and EQ updates and freezes the fades of the
		// pausable entchannels; the master volume is still computed, so the render mute above keeps working.
		//
		// cl_paused itself must not be set: the HUD dims, input is deactivated and other code and the menus read and
		// write it. Only the one load in SND_Update is redirected:
		//   1406603A1  48 8B 05 10 EB F6 03   mov rax, [0x1445CEEB8]   ->  call snd_cl_paused_stub / nop / nop
		//   1406603A8  83 78 10 00            cmp dword [rax+10h], 0    (kept: reads current.integer)
		//   1406603AC  40 0F 95 C6            setnz sil                 (sil = sounds paused for this update)
		// The stub returns the real cl_paused, or a private dvar_t with current.integer = 1 while the film is paused. No
		// dvar or saved setting is written, nothing else sees "paused", and the decision is made again on every sound
		// update: as soon as the film plays or is left (stop, disconnect, error, new demo) the next update unpauses.

		constexpr std::uintptr_t SND_UPDATE_CL_PAUSED_LOAD = 0x1406603A1; // in SND_Update, 7 bytes, see above
		constexpr std::uintptr_t CL_PAUSED = 0x1445CEEB8;          // dvar_t* cl_paused (int 0..2, flags 0x2000, not archived)
		constexpr std::uintptr_t SND_PAUSED = 0x149842B30;         // bool g_snd.paused, written by Pause / Unpause (and SP save restore)
		constexpr std::uintptr_t SND_PAUSABLE_MASK = 0x14984F7A0;  // uint32[4], one bit per entchannel (from SND_Init)
		constexpr std::uintptr_t SND_VOICE_ACTIVE = 0x1474FACF0;   // int[54], 0 = free voice
		constexpr std::uintptr_t SND_VOICE_HELD = 0x1474F7C68;     // int, stride 0xE0: set by SND_PauseVoice (0x140490E40)
		constexpr std::uintptr_t SND_CHANNEL_PAUSED = 0x14984FA6C; // bool, stride 0xF8
		constexpr std::uintptr_t SND_CHANNEL_ALIAS = 0x14984FA38;  // snd_alias_t*, stride 0xF8
		constexpr std::uintptr_t SND_STOP_VOICE = 0x140491530;     // void (int voice)
		constexpr std::uintptr_t SYS_ENTER_CRITICAL_SECTION = 0x1404F9820; // void (int index)
		constexpr std::uintptr_t SYS_LEAVE_CRITICAL_SECTION = 0x1404F9890; // void (int index)
		constexpr int CRITSECT_SOUND = 0x28;     // held by 0x1402CA5B0, the only caller of SND_Update, around the call
		constexpr int SND_VOICE_COUNT = 0x36;    // 8 2D, 32 3D and 14 stream voices
		constexpr std::size_t SND_CHANNEL_STRIDE = 0xF8;
		constexpr std::size_t SND_DRIVER_STRIDE = 0xE0;
		constexpr std::size_t SND_ALIAS_FLAGS = 0x54;            // bit 0: looping, bits 9..15: entchannel
		constexpr std::uint32_t SND_ALIAS_FLAG_LOOPING = 1;

		game::dvar_t film_pause_dvar{};           // stand-in for cl_paused, current.integer = 1
		game::dvar_t* demotimescale_dvar{};       // cached by on_frame: Dvar_FindVar takes the dvar lock, the stub runs in the sound lock
		std::atomic<bool> film_sound_paused{false}; // last decision of the main thread (SND_Update also runs on the render thread)
		bool film_sound_pause_installed = false;

		// seeks and the paused sound (main thread only)
		constexpr int POV_STEP_MSEC = 50;        // sync_pov_target: one snapshot forward, so a new viewed player shows at once
		constexpr int SEEK_DROP_FRAMES = 2;      // the frame a seek moved the film and the next one (its snapshots may come then)
		bool film_frozen_frame = false;          // film_held() at the start of the frame (on_frame): the film clock stands still in it
		int seek_drop_frames = 0;                // > 0: a seek moved the paused film, the one-shots it starts are dropped
		int snd_server_time = 0;                 // cl->serverTime at the previous sound update of the main thread
		bool film_jump_pending = false;          // seek_relative: the film jumps, the paused one-shots belong to the old position

		// the film is on screen: cgame running, CA_ACTIVE, or CA_PRIMED / CA_LOADING while a rewind or loop reads the
		// gamestate again (demo_playback repeat(), CL_ParseGamestate). Plain reads of engine globals, valid on any thread.
		bool film_on_screen()
		{
			const auto state = *game::mp::connstate;
			return game::CL_IsCgameInitialized() && state >= game::CA_LOADING && state <= game::CA_ACTIVE;
		}

		// main thread only: the demo clock is stopped
		bool film_held()
		{
			if (!demo_playback::playing())
			{
				return false;
			}

			// Anomaly's Com_TimeScaleMsec hook returns 0 ms per frame (demo_timescale.cpp)
			if (demotimescale_dvar && demotimescale_dvar->current.value <= 0.0f)
			{
				return true;
			}

			// the theater holds the film and apply_timescale() has not written the 0 yet (key, console or end_render this
			// frame, or a loop restart that set demotimescale back to 1). RT fast forward runs the film even when paused.
			return paused && ff_speed <= 0.0f;
		}

		// Stops the paused one-shot sounds of the pausable entchannels. Loops (ambience, entity loops) are kept so they
		// play when the film resumes, and menu sounds are never paused. include_held = false: only the sounds started
		// while the engine pause was on (SND_SetupChannel 0x14065D8C0 creates them paused, they are not held by
		// SND_PauseSounds and get no mixer packet until the pause ends); true: also the sounds SND_PauseSounds held.
		// The engine stops voices with SND_StopVoice in the same context (stale loops in 0x140660C60 right before
		// SND_Update, the stop-all in 0x140660ED0 inside it): main thread, sound lock held.
		void stop_paused_one_shots(const bool include_held)
		{
			const auto* pausable = reinterpret_cast<const std::uint32_t*>(SND_PAUSABLE_MASK);
			for (auto i = 0; i < SND_VOICE_COUNT; ++i)
			{
				const auto active = *reinterpret_cast<const int*>(SND_VOICE_ACTIVE + i * sizeof(int)) != 0;
				const auto channel_paused = *reinterpret_cast<const std::uint8_t*>(SND_CHANNEL_PAUSED + i * SND_CHANNEL_STRIDE) != 0;
				const auto held = *reinterpret_cast<const int*>(SND_VOICE_HELD + i * SND_DRIVER_STRIDE) != 0;
				const auto alias = *reinterpret_cast<const std::uintptr_t*>(SND_CHANNEL_ALIAS + i * SND_CHANNEL_STRIDE);
				if (!active || !channel_paused || (held && !include_held) || !alias)
				{
					continue;
				}

				const auto flags = *reinterpret_cast<const std::uint32_t*>(alias + SND_ALIAS_FLAGS);
				const auto entchannel = (flags >> 9) & 0x7F;
				if ((flags & SND_ALIAS_FLAG_LOOPING) != 0 || ((pausable[entchannel >> 5] >> (entchannel & 31)) & 1) == 0)
				{
					continue;
				}

				reinterpret_cast<void(*)(int)>(SND_STOP_VOICE)(i);
			}
		}

		// replaces "mov rax, [cl_paused]" in SND_Update: sound lock held, on the main thread, or on the render thread
		// while it services a loading screen. It only ever adds a pause; otherwise the engine reads the real cl_paused.
		//
		// Seeks while the film is paused: a rewind restarts the demo and runs forward to the target, demoforward runs the
		// skipped snapshots, and the viewed player change steps 50 ms. The sounds they start are created paused and would
		// all start together when the film resumes, so the one-shots among them are dropped. A seek is recognized by the
		// film moving although its clock stands still (or by the restart: not CA_ACTIVE); the drop lasts for that frame
		// and the next one. Other sounds that are created paused are kept and play when the film resumes: snd_restart
		// restores every voice that way, and the last frame before a pause can create sounds after the first paused
		// update.
		//
		// A jump (seek_relative, more than the 50 ms step) also stops the one-shots the pause held: they belong to the old
		// position. This happens at the first sound update after the request, before the engine could unpause them: Preview
		// Clip and Space at the end of the film play the film again in the same frame that queues their seek.
		game::dvar_t* snd_cl_paused_stub()
		{
			auto hold = false;
			if (game::Sys_IsMainThread())
			{
				const auto on_screen = film_on_screen();
				hold = on_screen && film_held();
				film_sound_paused = hold;

				const auto server_time = game::mp::cl->serverTime;
				const auto moved = server_time != snd_server_time;
				snd_server_time = server_time;
				if (hold && ((moved && film_frozen_frame) || *game::mp::connstate != game::CA_ACTIVE))
				{
					seek_drop_frames = SEEK_DROP_FRAMES;
				}

				if (*reinterpret_cast<const std::uint8_t*>(SND_PAUSED) != 0)
				{
					if (film_jump_pending && on_screen)
					{
						stop_paused_one_shots(true);
					}
					else if (hold && seek_drop_frames > 0)
					{
						stop_paused_one_shots(false);
					}
				}

				film_jump_pending = false; // during playback a jump changes nothing, like before
			}
			else
			{
				// the theater state belongs to the main thread: use its last decision while the film is still on screen
				hold = film_sound_paused && film_on_screen();
			}

			return hold ? &film_pause_dvar : *reinterpret_cast<game::dvar_t**>(CL_PAUSED);
		}

		struct sound_lock
		{
			sound_lock() { reinterpret_cast<void(*)(int)>(SYS_ENTER_CRITICAL_SECTION)(CRITSECT_SOUND); }
			~sound_lock() { reinterpret_cast<void(*)(int)>(SYS_LEAVE_CRITICAL_SECTION)(CRITSECT_SOUND); }

			sound_lock(const sound_lock&) = delete;
			sound_lock& operator=(const sound_lock&) = delete;
		};

		void install_film_sound_pause()
		{
			// exactly the code this was written for (iw6mp64_ship.exe 3.15), not patched by anybody else
			static constexpr std::array<std::uint8_t, 15> expected{
				0x48, 0x8B, 0x05, 0x10, 0xEB, 0xF6, 0x03, // mov rax, [rip+03F6EB10h] (0x1445CEEB8)
				0x83, 0x78, 0x10, 0x00,                   // cmp dword ptr [rax+10h], 0
				0x40, 0x0F, 0x95, 0xC6,                   // setnz sil
			};

			auto* const place = reinterpret_cast<std::uint8_t*>(SND_UPDATE_CL_PAUSED_LOAD);
			if (std::memcmp(place, expected.data(), expected.size()) != 0)
			{
				console::warn("theater: unexpected code in SND_Update, game sound will not pause with the film\n");
				return;
			}

			try
			{
				film_pause_dvar.name = "theater_film_paused";
				film_pause_dvar.current.integer = 1;

				// call rel32 (through a near trampoline when the add-on is out of range) + 2 NOPs over the 7-byte load.
				// Nothing volatile is live there (RAX is the load's result, ECX is set right after, RDX / R8-R11 / XMM0-5
				// are not read before the next call), msec and the result live in EDI / ESI, and RSP is the body's aligned
				// frame (sub rsp, 78h is the whole prologue). The stub returns to 0x1406603A6, the NOPs, then the cmp.
				void* target = reinterpret_cast<void*>(&snd_cl_paused_stub);
				if (utils::hook::is_relatively_far(place, target))
				{
					target = utils::hook::get_near_trampoline(place, target);
				}

				std::array<std::uint8_t, 7> patch{0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90};
				const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) - reinterpret_cast<std::intptr_t>(place + 5));
				std::memcpy(&patch[1], &rel, sizeof(rel));

				// SND_Update also runs on the render thread during loading screens, always inside the sound critical
				// section: holding it guarantees that no thread executes the bytes while they are replaced
				{
					const sound_lock lock;
					utils::hook::copy(place, patch.data(), patch.size());
				}

				film_sound_pause_installed = true;
			}
			catch (const std::exception& e)
			{
				console::error("theater: game sound will not pause with the film: %s\n", e.what());
			}
		}

		// ------------------------------------------------------------------ dvar helpers

		game::dvar_t* find_dvar(const char* name)
		{
			return game::Dvar_FindVar(name);
		}

		float get_timescale()
		{
			const auto* dvar = find_dvar("demotimescale");
			return dvar ? dvar->current.value : 1.0f;
		}

		void set_timescale(const float value)
		{
			if (auto* dvar = find_dvar("demotimescale"))
			{
				dvar->current.value = std::clamp(value, 0.0f, 1000.0f);
			}
		}

		// remember a bool dvar's original value the first time the theater changes it
		void set_bool_dvar(const char* name, const bool value)
		{
			auto* dvar = find_dvar(name);
			if (!dvar)
			{
				return;
			}

			if (!saved_dvars.contains(name))
			{
				saved_dvars[name] = dvar->current.enabled ? "1" : "0";
			}

			if (dvar->current.enabled != value)
			{
				dvar->current.enabled = value;
				dvar->latched.enabled = value;
				dvar->modified = true;
			}
		}

		void restore_dvars()
		{
			for (const auto& [name, value] : saved_dvars)
			{
				if (auto* dvar = find_dvar(name.data()))
				{
					dvar->current.enabled = value == "1";
					dvar->latched.enabled = dvar->current.enabled;
					dvar->modified = true;
				}
			}

			saved_dvars.clear();
		}

		bool sidecar_view()
		{
			return target >= 0 && demo_pov::substituting();
		}

		// the engine renders the view itself: the recorded player, or another player through the sidecar
		bool engine_first_person()
		{
			return view == view_mode::first_person && (target < 0 || sidecar_view());
		}

		void apply_view_dvars()
		{
			const auto engine_view = engine_first_person();
			set_bool_dvar("camera_thirdPerson", !engine_view);
			set_bool_dvar("cg_drawGun", engine_view);

			// the local player's body is needed whenever the camera is not inside its head
			world::set_force_local_body(!engine_view);
			world::set_hidden_player(view == view_mode::first_person && !engine_view ? target : -1);
		}

		// ------------------------------------------------------------------ demo info / marker storage

		std::filesystem::path camera_file()
		{
			const auto dir = console::get_theater_dir() / "cameras";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			return dir / (info.path.stem().string() + ".cam");
		}

		// every change of the markers ends here (insert, reposition, popup, delete)
		void save_keyframes()
		{
			++keyframes_rev;
			if (info.path.empty())
			{
				return;
			}

			std::ofstream file(camera_file(), std::ios::trunc);
			file << "iw6-theater-camera 2\n";
			for (const auto& k : keyframes)
			{
				file << std::format("key {:.3f} {:.4f} {:.4f} {:.4f} {:.6f} {:.6f} {:.6f} {:.6f} {:.3f} {} {:.2f}\n",
				                    k.time, k.pos.x, k.pos.y, k.pos.z, k.rot.w, k.rot.x, k.rot.y, k.rot.z, k.fov,
				                    static_cast<int>(k.mode), k.speed);
			}
		}

		void load_keyframes()
		{
			++keyframes_rev;
			keyframes.clear();

			std::ifstream file(camera_file());
			std::string line;
			while (std::getline(file, line))
			{
				std::istringstream s(line);
				std::string tag;
				s >> tag;
				if (tag != "key")
				{
					continue;
				}

				keyframe k{};
				s >> k.time >> k.pos.x >> k.pos.y >> k.pos.z >> k.rot.w >> k.rot.x >> k.rot.y >> k.rot.z >> k.fov;
				if (!s)
				{
					continue;
				}

				// version 2: speed mode and speed
				int mode{};
				double speed{};
				if (s >> mode >> speed)
				{
					k.mode = static_cast<speed_mode>(std::clamp(mode, 0, 2));
					k.speed = std::clamp(speed, 0.1, 6.0);
				}

				k.rot = normalize(k.rot);
				keyframes.push_back(k);
			}

			std::ranges::sort(keyframes, {}, &keyframe::time);
		}

		// reads the gen_footer record (first/last server time) of a .dm_iw6 file
		void read_demo_info(const std::filesystem::path& path)
		{
			info = {};
			info.path = path;

			std::ifstream file(path, std::ios::binary);
			while (file)
			{
				std::uint8_t id{};
				if (!file.read(reinterpret_cast<char*>(&id), 1))
				{
					break;
				}

				std::uint32_t size{};
				if (id & 0x80)
				{
					std::uint8_t size_byte{};
					file.read(reinterpret_cast<char*>(&size_byte), 1);
					size = size_byte;
				}
				else
				{
					file.read(reinterpret_cast<char*>(&size), 4);
				}

				const auto type = id & 0x1F;
				if (type == 31) // eof
				{
					break;
				}

				if (type == 6 && size >= 8) // gen_footer
				{
					file.read(reinterpret_cast<char*>(&info.first_time), 4);
					file.read(reinterpret_cast<char*>(&info.last_time), 4);
					break;
				}

				file.seekg(size, std::ios::cur);
			}
		}

		// ------------------------------------------------------------------ dolly evaluation

		camera_pose pose_of(const keyframe& k)
		{
			return {k.pos, k.rot, k.fov};
		}

		std::optional<camera_pose> evaluate(const double time)
		{
			const auto n = keyframes.size();
			if (n == 0)
			{
				return {};
			}

			if (n == 1 || time <= keyframes.front().time)
			{
				return pose_of(keyframes.front());
			}

			if (time >= keyframes.back().time)
			{
				return pose_of(keyframes.back());
			}

			std::size_t i = 0;
			while (i + 1 < n && keyframes[i + 1].time <= time)
			{
				++i;
			}

			const auto& k1 = keyframes[i];
			const auto& k2 = keyframes[i + 1];
			const auto& k0 = i > 0 ? keyframes[i - 1] : k1;
			const auto& k3 = i + 2 < n ? keyframes[i + 2] : k2;

			const auto t1 = k1.time;
			const auto t2 = k2.time;
			const auto t0 = i > 0 ? k0.time : t1 - (t2 - t1);
			const auto t3 = i + 2 < n ? k3.time : t2 + (t2 - t1);

			// keep rotations on the same hemisphere as their predecessor
			auto q0 = k0.rot;
			auto q1 = k1.rot;
			auto q2 = k2.rot;
			auto q3 = k3.rot;
			if (dot(q0, q1) < 0) q1 = -q1;
			if (dot(q1, q2) < 0) q2 = -q2;
			if (dot(q2, q3) < 0) q3 = -q3;

			camera_pose pose{};
			pose.pos = catmull_rom(k0.pos, k1.pos, k2.pos, k3.pos, t0, t1, t2, t3, time,
			                       [](const vec3& a, const vec3& b, const double u) { return lerp(a, b, u); });
			pose.rot = catmull_rom(q0, q1, q2, q3, t0, t1, t2, t3, time,
			                       [](const quat& a, const quat& b, const double u) { return slerp(a, b, u); });
			pose.fov = catmull_rom(k0.fov, k1.fov, k2.fov, k3.fov, t0, t1, t2, t3, time,
			                       [](const double a, const double b, const double u) { return lerp(a, b, u); });
			pose.fov = std::clamp(pose.fov, 1.0, 170.0);
			return pose;
		}

		// BO2 marker speed modes: the playback speed while the dolly camera runs from a marker to the next
		float dolly_speed(const double time)
		{
			if (keyframes.size() < 2 || time < keyframes.front().time || time >= keyframes.back().time)
			{
				return selected_speed;
			}

			std::size_t i = 0;
			while (i + 2 < keyframes.size() && keyframes[i + 1].time <= time)
			{
				++i;
			}

			const auto& k1 = keyframes[i];
			const auto& k2 = keyframes[i + 1];
			switch (k1.mode)
			{
			case speed_mode::fixed:
				return static_cast<float>(k1.speed);
			case speed_mode::linear:
			{
				const auto u = std::clamp((time - k1.time) / std::max(1.0, k2.time - k1.time), 0.0, 1.0);
				return static_cast<float>(lerp(k1.speed, k2.speed, u));
			}
			default:
				return selected_speed;
			}
		}

		// ------------------------------------------------------------------ camera application

		void write_pose(const camera_pose& pose)
		{
			auto* refdef = get_refdef();

			vec3 axis[3];
			to_axis(pose.rot, axis);

			refdef->vieworg[0] = static_cast<float>(pose.pos.x);
			refdef->vieworg[1] = static_cast<float>(pose.pos.y);
			refdef->vieworg[2] = static_cast<float>(pose.pos.z);

			for (auto i = 0; i < 3; ++i)
			{
				refdef->viewaxis[i][0] = static_cast<float>(axis[i].x);
				refdef->viewaxis[i][1] = static_cast<float>(axis[i].y);
				refdef->viewaxis[i][2] = static_cast<float>(axis[i].z);
			}

			double pitch{}, yaw{}, roll{};
			to_angles(pose.rot, pitch, yaw, roll);
			auto& angles = game::mp::cg->refdefViewAngles;
			angles[0] = static_cast<float>(pitch);
			angles[1] = static_cast<float>(yaw);
			angles[2] = static_cast<float>(roll);

			fov_to_tangents(static_cast<float>(pose.fov), &refdef->tan_half_fov_x, &refdef->tan_half_fov_y);

			last_pose = pose;
			last_pose_valid = true;
			camera_written = true;
		}

		// The renderer picks the level of detail of every model by its distance to the LOD origin, and
		// CG_DrawActiveFrame sets that at the start of the frame (0x140299987, long before CG_CalcViewValues) from the
		// view of the recorded player. The theater's cameras are somewhere else, so a player far from where the
		// recorder was got its lowest detail however close the camera came: no weapon in its hands (a weapon's last
		// level is nothing at all) and a head of a few flat polygons (seen in game in 3rd person and in the free
		// camera, from about 1500 and 2500 units). While the theater places the camera, the level of detail is taken
		// from where it was, and with its field of view, in the previous frame.
		void set_lod_origin_stub(refdef_view* refdef, const float lod_scale)
		{
			if (!camera_written || !last_pose_valid || refdef != get_refdef() || !active())
			{
				r_set_lod_origin_hook.invoke<void>(refdef, lod_scale);
				return;
			}

			// for this call only: the engine keeps working with its own view until CG_CalcViewValues
			const auto engine_view = *refdef;
			refdef->vieworg[0] = static_cast<float>(last_pose.pos.x);
			refdef->vieworg[1] = static_cast<float>(last_pose.pos.y);
			refdef->vieworg[2] = static_cast<float>(last_pose.pos.z);
			fov_to_tangents(static_cast<float>(last_pose.fov), &refdef->tan_half_fov_x, &refdef->tan_half_fov_y);

			r_set_lod_origin_hook.invoke<void>(refdef, lod_scale);
			*refdef = engine_view;
		}

		camera_pose current_view_pose()
		{
			const auto* refdef = get_refdef();

			// the rendered axis, not cg.refdefViewAngles: the dead camera (CG_OffsetThirdPersonView 0x14029B7B0) aims
			// the axis at its focus point and leaves refdefViewAngles at the playerState's view angles
			vec3 axis[3];
			for (auto i = 0; i < 3; ++i)
			{
				axis[i] = {refdef->viewaxis[i][0], refdef->viewaxis[i][1], refdef->viewaxis[i][2]};
			}

			double pitch{}, yaw{}, roll{};
			axis_to_angles(axis, pitch, yaw, roll);

			camera_pose pose{};
			pose.pos = {refdef->vieworg[0], refdef->vieworg[1], refdef->vieworg[2]};
			pose.rot = from_angles(pitch, yaw, roll);
			pose.fov = tangent_to_fov(refdef->tan_half_fov_y);
			return pose;
		}

		camera_pose free_pose()
		{
			return {free_pos, from_angles(free_pitch, free_yaw, free_roll), free_fov};
		}

		void set_free_from_pose(const camera_pose& pose)
		{
			free_pos = pose.pos;
			free_vel = {};
			to_angles(pose.rot, free_pitch, free_yaw, free_roll);
			free_fov = pose.fov;
		}

		double view_fov()
		{
			const auto* dvar = find_dvar("cg_fov");
			return dvar ? std::clamp(static_cast<double>(dvar->current.value), 40.0, 120.0) : 65.0;
		}

		bool key(const int k)
		{
			return k >= 0 && k < static_cast<int>(keys_down.size()) && keys_down[k];
		}

		double stick_curve(const float value)
		{
			// quadratic response: precise near the center, full speed at the edge
			return static_cast<double>(value) * std::abs(static_cast<double>(value));
		}

		double mouse_sensitivity()
		{
			const auto* sensitivity = find_dvar("sensitivity");
			return (sensitivity ? sensitivity->current.value : 5.0f) * (theater_mouse_scale ? theater_mouse_scale->current.value : 1.0f);
		}

		void update_free_camera(const double dt, const theater_gamepad::input& pad)
		{
			const auto fov_scale = std::clamp(free_fov / 65.0, 0.05, 2.0);

			// mouse look
			const auto dx = mouse_dx.exchange(0);
			const auto dy = mouse_dy.exchange(0);
			free_yaw -= dx * 0.022 * mouse_sensitivity() * fov_scale;
			free_pitch += dy * 0.022 * mouse_sensitivity() * fov_scale;

			// controller: the right stick looks. While D-pad left is held it is the lens instead (extra, the button is
			// unused in BO2's default layout): up / down zoom in / out (FOV), left / right roll
			if (pad.connected)
			{
				if (pad.held(theater_gamepad::DPAD_LEFT))
				{
					free_fov -= stick_curve(pad.ry) * 40.0 * dt;
					free_roll += stick_curve(pad.rx) * 45.0 * dt;
				}
				else
				{
					free_yaw -= stick_curve(pad.rx) * 150.0 * dt * fov_scale;
					free_pitch -= stick_curve(pad.ry) * 110.0 * dt * fov_scale;
				}
			}

			free_pitch = std::clamp(free_pitch, -89.9, 89.9);
			free_yaw = std::remainder(free_yaw, 360.0);
			free_fov = std::clamp(free_fov, 5.0, 160.0);

			if (key('z')) free_roll -= 45.0 * dt;
			if (key('x')) free_roll += 45.0 * dt;

			const auto q = from_angles(free_pitch, free_yaw, 0.0);
			vec3 axis[3];
			to_axis(q, axis);

			// keyboard direction (unit length); like BO2 the camera flies where it looks, E / Q go up / down
			vec3 keyboard_wish{};
			if (key('w')) keyboard_wish += axis[0];
			if (key('s')) keyboard_wish += axis[0] * -1.0;
			if (key('a')) keyboard_wish += axis[1];
			if (key('d')) keyboard_wish += axis[1] * -1.0;
			if (key('e')) keyboard_wish += vec3{0, 0, 1};
			if (key('q')) keyboard_wish += vec3{0, 0, -1};

			vec3 wish{};
			if (const auto len = length(keyboard_wish); len > 0.0)
			{
				wish = keyboard_wish * (1.0 / len);
			}

			// controller: left stick flies along the view (BO2 has no up / down on the pad, triggers are playback)
			if (pad.connected)
			{
				wish += axis[0] * stick_curve(pad.ly);
				wish += axis[1] * -stick_curve(pad.lx);
			}

			if (const auto len = length(wish); len > 1.0)
			{
				wish = wish * (1.0 / len);
			}

			auto speed = theater_freecam_speed ? static_cast<double>(theater_freecam_speed->current.value) : 250.0;
			if (key(game::K_SHIFT)) speed *= 3.5;
			if (key(game::K_CTRL)) speed *= 0.25;

			const auto wanted = wish * speed;
			const auto blend = std::min(1.0, dt * 10.0);
			free_vel = free_vel + (wanted - free_vel) * blend;
			free_pos += free_vel * dt;
		}

		double frame_seconds()
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const auto dt = last_camera_frame.QuadPart
				                ? static_cast<double>(now.QuadPart - last_camera_frame.QuadPart) / static_cast<double>(qpc_frequency.QuadPart)
				                : 0.0;
			last_camera_frame = now;
			return std::clamp(dt, 0.0, 0.25);
		}

		void update_smooth_time(const double dt)
		{
			const auto wanted = static_cast<double>(game::mp::cl->serverTime);
			smooth_time += dt * 1000.0 * get_timescale();

			// follow the engine clock; snap on jumps (seek, rewind, restart)
			const auto error = wanted - smooth_time;
			if (std::abs(error) > 250.0)
			{
				smooth_time = wanted;
			}
			else
			{
				smooth_time += error * std::min(1.0, dt * 4.0);
			}
		}

		double angle_delta(const double to, const double from)
		{
			return std::remainder(to - from, 360.0);
		}

		// the client whose view the demo recorded (cg.ps.clientNum before any sidecar substitution)
		int recorded_client()
		{
			static auto cached = -1;
			if (!demo_pov::substituting())
			{
				cached = world::recorded_view_client();
			}

			return cached;
		}

		int viewed_client()
		{
			return target >= 0 ? target : recorded_client();
		}

		// 1st person through another player's eyes without a sidecar: tag_eye + interpolated view angles
		void update_follow_first_person(const double dt)
		{
			const auto eye = world::alive(target) ? world::eye(target) : std::optional<vec3>{};
			if (eye)
			{
				const auto angles = world::view_angles(target);
				if (!follow_valid || length(*eye - follow_eye) > 64.0)
				{
					follow_eye = *eye;
					follow_angles = angles;
				}
				else
				{
					// snapshots carry whole degrees at 20 Hz: smooth a little (~40 ms position, ~60 ms angles)
					const auto k_pos = 1.0 - std::exp(-dt / 0.04);
					const auto k_ang = 1.0 - std::exp(-dt / 0.06);
					follow_eye += (*eye - follow_eye) * k_pos;
					follow_angles.x += angle_delta(angles.x, follow_angles.x) * k_ang;
					follow_angles.y += angle_delta(angles.y, follow_angles.y) * k_ang;
				}

				follow_valid = true;
			}

			if (follow_valid)
			{
				write_pose({follow_eye, from_angles(follow_angles.x, follow_angles.y, 0.0), view_fov()});
			}
			else if (last_pose_valid)
			{
				write_pose(last_pose);
			}
		}

		void update_third_person(const double dt, const theater_gamepad::input& pad)
		{
			// mouse / right stick orbit around the player
			const auto dx = mouse_dx.exchange(0);
			const auto dy = mouse_dy.exchange(0);
			orbit_yaw -= dx * 0.022 * mouse_sensitivity();
			orbit_pitch += dy * 0.022 * mouse_sensitivity();
			if (pad.connected)
			{
				orbit_yaw -= stick_curve(pad.rx) * 150.0 * dt;
				orbit_pitch -= stick_curve(pad.ry) * 110.0 * dt;
			}

			orbit_pitch = std::clamp(orbit_pitch, -60.0, 85.0);
			orbit_yaw = std::remainder(orbit_yaw, 360.0);

			const auto client = viewed_client();
			auto feet = world::alive(client) || client == world::recorded_view_client() ? world::origin(client) : std::optional<vec3>{};
			if (feet && !world::alive(client) && client == world::recorded_view_client() && *reinterpret_cast<int*>(0x14176EC04) >= 5)
			{
				feet.reset(); // the recorded player is dead or spectating: look for the corpse
			}
			if (!feet)
			{
				feet = world::corpse(client);
			}

			if (!feet)
			{
				// nobody to orbit (the recorded view is a spectator): keep the recorded view
				return;
			}

			const auto eye = world::eye(client);
			const auto height = eye ? std::clamp(eye->z - feet->z, 10.0, 64.0) : 60.0;
			const auto pivot = *feet + vec3{0.0, 0.0, height + 6.0};

			const auto player_yaw = target < 0 ? static_cast<double>(game::mp::cg->refdefViewAngles[1]) : world::view_angles(client).y;
			const auto yaw = player_yaw + orbit_yaw;
			const auto rot = from_angles(orbit_pitch, yaw, 0.0);
			vec3 axis[3];
			to_axis(rot, axis);

			const auto wanted = pivot - axis[0] * ORBIT_DISTANCE;
			auto camera = world::trace(pivot, wanted, client);
			const auto back = camera - pivot;
			if (const auto len = length(back); len > 4.0)
			{
				camera = pivot + back * ((len - 3.0) / len); // stay off the wall
			}

			write_pose({camera, rot, view_fov()});
		}

		// ------------------------------------------------------------------ playback actions

		void seek_relative(int msec)
		{
			// one backward seek at a time: a demorewind requested while the demo restarts for the previous one left it
			// frozen in the restart when the film was paused (seen in game); a second press that fast is ignored
			if (msec < 0 && backward_seek_target)
			{
				return;
			}

			// keep the target inside the recording so playback never runs off the end
			if (info.has_length())
			{
				const auto now = game::mp::cl->serverTime;
				const auto wanted = std::clamp(now + msec, info.first_time + 100, info.last_time - 300);

				// a clamped jump must never turn into a step in the opposite direction
				// (a backwards step restarts the demo internally, which is expensive)
				if ((msec > 0 && wanted <= now) || (msec < 0 && wanted >= now))
				{
					return;
				}

				msec = wanted - now;
			}

			if (msec != 0)
			{
				console::info("theater: seek %+d ms (from %d)\n", msec, game::mp::cl->serverTime);

				// the sounds the film pause holds belong to the old position (the player switch step keeps them)
				if (std::abs(msec) > POV_STEP_MSEC && demo_playback::playing())
				{
					film_jump_pending = true;
				}
			}

			if (msec > 0)
			{
				command::execute(std::format("demoforward {}", msec));
			}
			else if (msec < 0)
			{
				// a backward seek restarts the demo from its beginning and runs it forward to the target: for a frame or
				// two the screen would show the demo's start (measured: CA_ACTIVE at its first snapshot, then the target
				// time with snapshots from the start). Those frames are not presented (on_frame lets them through again
				// once the demo is back at the target). The seek to the start of a clip before a video render too: the
				// restart showed on screen as the loading screen without its picture, a checkerboard, for two frames
				// (seen in game); update_render waits for the frames to be presented again.
				if (demo_playback::playing())
				{
					theater_capture::hold_frames();
					backward_seek_target = game::mp::cl->serverTime + msec;
					backward_seek_restarts = demo_playback::restarts();
					backward_seek_at = std::chrono::steady_clock::now();
				}

				command::execute(std::format("demorewind {}", -msec));
			}
		}

		void seek_absolute(const double server_time)
		{
			seek_relative(static_cast<int>(server_time - game::mp::cl->serverTime));
		}

		std::chrono::steady_clock::time_point restart_requested{};

		bool at_end()
		{
			return info.has_length() && game::mp::cl->serverTime >= info.last_time - 150 &&
				std::chrono::steady_clock::now() - restart_requested > 3s;
		}

		void toggle_pause()
		{
			if (paused)
			{
				// resuming at the end of the recording restarts it from the beginning
				if (info.has_length() && game::mp::cl->serverTime >= info.last_time - 300)
				{
					restart_requested = std::chrono::steady_clock::now();
					seek_absolute(info.first_time);
				}
			}

			paused = !paused;
		}

		// the selected speed in tenths; a speed between two tenths (theater_speed 0.25) is rounded in the direction of
		// the step, so the step never goes the wrong way
		int speed_tenths(const int direction)
		{
			const auto tenths = static_cast<double>(selected_speed) * 10.0;
			return static_cast<int>(direction > 0 ? std::floor(tenths + 1e-3) : std::ceil(tenths - 1e-3));
		}

		void set_speed_tenths(const int tenths)
		{
			selected_speed = static_cast<float>(std::clamp(tenths, MIN_SPEED_TENTHS, MAX_SPEED_TENTHS)) / 10.0f;
			speed_label = true;
		}

		// Up / Down: BO2's 0.2x steps, plus 0.1x below 0.2x (0.2 -> 0.1 -> 0.2; 0.3 -> 0.4 up, 0.2 down)
		void step_speed(const int direction)
		{
			const auto t = speed_tenths(direction);
			set_speed_tenths(direction > 0 ? (t < 2 ? 2 : (t / 2 + 1) * 2) : (t <= 2 ? 1 : ((t + 1) / 2 - 1) * 2));
		}

		// D-pad up / down: 0.1x steps (BO2 controller layout 2)
		void step_speed_fine(const int direction)
		{
			set_speed_tenths(speed_tenths(direction) + direction);
		}

		double demo_position()
		{
			return info.has_length() ? static_cast<double>(game::mp::cl->serverTime - info.first_time) : 0.0;
		}

		bool jump_back_allowed()
		{
			return info.has_length() && !clip_recording && !replaying && demo_position() > 600.0;
		}

		bool jump_forward_allowed()
		{
			if (!info.has_length() || clip_recording || replaying)
			{
				return false;
			}

			const auto mark = (std::floor(demo_position() / JUMP_INTERVAL) + 1.0) * JUMP_INTERVAL;
			return info.first_time + mark < info.last_time - 300;
		}

		void jump_back()
		{
			if (!jump_back_allowed())
			{
				return;
			}

			// 01:44 -> 01:40, 01:40 -> 01:30
			const auto mark = std::max(0.0, std::floor((demo_position() - 500.0) / JUMP_INTERVAL) * JUMP_INTERVAL);
			seek_absolute(info.first_time + mark);
		}

		void jump_forward()
		{
			if (!jump_forward_allowed())
			{
				return;
			}

			const auto mark = (std::floor(demo_position() / JUMP_INTERVAL) + 1.0) * JUMP_INTERVAL;
			seek_absolute(info.first_time + mark);
		}

		void go_to_start()
		{
			if (info.has_length() && !clip_recording && !replaying)
			{
				seek_absolute(info.first_time);
			}
		}

		void toggle_controls()
		{
			if (theater_hud)
			{
				theater_hud->current.enabled = !theater_hud->current.enabled;
			}
		}

		// With the HUD off (cg_draw2D 0) the engine does not run the game's LUI at all: Esc / START would open the Start
		// menu only once the HUD is back on (the key waits in the LUI queue), so the menu could not be seen. While the HUD
		// is off, Esc / START turn cg_draw2D on until that menu is closed again (or did not open after all).
		constexpr auto HUD_FOR_MENU_TIMEOUT = 1500ms;

		struct
		{
			bool on{};        // cg_draw2D was turned on for a menu
			bool menu_seen{}; // the menu has opened since
			std::chrono::steady_clock::time_point since{};
		} hud_for_menu;

		// F2 / B "Toggle Hud". cg_draw2D is a cheat dvar: the engine puts it back to its default every time the demo
		// restarts (each backward seek, also the seek to the start of a clip before a video render), so the choice is
		// kept here and applied again every frame, like the camera dvars in apply_view_dvars
		bool game_hud_hidden = false;

		void apply_game_hud()
		{
			const auto* dvar = find_dvar("cg_draw2D");
			const auto shown = hud_for_menu.on || !game_hud_hidden;
			if (dvar && dvar->current.enabled != shown)
			{
				set_bool_dvar("cg_draw2D", shown);
			}
		}

		void toggle_game_hud()
		{
			game_hud_hidden = !game_hud_hidden;
			apply_game_hud();
		}

		bool menu_open()
		{
			return (*game::keyCatchers & ~1) != 0; // menus set key catchers; 1 is the console
		}

		void show_hud_for_menu()
		{
			if (hud_for_menu.on || !game_hud_hidden)
			{
				return;
			}

			hud_for_menu.on = true;
			hud_for_menu.menu_seen = false;
			hud_for_menu.since = std::chrono::steady_clock::now();
			apply_game_hud();
		}

		void update_hud_for_menu()
		{
			if (!hud_for_menu.on)
			{
				return;
			}

			if (menu_open())
			{
				hud_for_menu.menu_seen = true;
				return;
			}

			if (hud_for_menu.menu_seen || std::chrono::steady_clock::now() - hud_for_menu.since > HUD_FOR_MENU_TIMEOUT)
			{
				hud_for_menu = {};
				apply_game_hud();
			}
		}

		// ------------------------------------------------------------------ cameras

		void reset_follow()
		{
			follow_valid = false;
			orbit_yaw = 0.0;
			orbit_pitch = 12.0;
		}

		// the sidecar swaps in the viewed player's own view only in 1ST PERSON: in 3RD PERSON and the free camera
		// the world is shown as recorded, so the viewed player is drawn like everybody else
		void sync_pov_target()
		{
			const auto wanted = view == view_mode::first_person ? target : -1;
			if (demo_pov::get_target() != wanted)
			{
				demo_pov::set_target(wanted);

				// the sidecar view changes with the next snapshot: while paused, step one snapshot so it shows at once
				if (paused && demo_pov::available() && !replaying)
				{
					seek_relative(POV_STEP_MSEC);
				}
			}
		}

		void set_target(const int client)
		{
			// the player the demo recorded is shown through the engine's own view
			target = client == recorded_client() ? -1 : client;

			sync_pov_target();
			reset_follow();
			apply_view_dvars();
		}

		void enter_free_mode(const free_mode mode)
		{
			// leaving the dolly path keeps the camera where the path put it
			if (fmode == free_mode::dolly && mode != free_mode::dolly)
			{
				if (const auto pose = evaluate(smooth_time))
				{
					set_free_from_pose(*pose);
				}
			}

			fmode = mode;
			repositioning = false;
			attached = -1;
			candidate = -1;
			if (mode == free_mode::dolly && keyframes.empty())
			{
				popup = {};
				popup.kind = popup_kind::dolly_notice;
			}
		}

		void cycle_view()
		{
			switch (view)
			{
			case view_mode::first_person:
				view = view_mode::third_person;
				reset_follow();
				break;
			case view_mode::third_person:
				view = view_mode::free_camera;
				set_free_from_pose(last_pose_valid ? last_pose : current_view_pose());
				enter_free_mode(fmode);
				break;
			default:
				view = view_mode::first_person;
				reset_follow();
				break;
			}

			sync_pov_target();
			keys_down.fill(false);
			apply_view_dvars();
		}

		// Mouse1 / Mouse2, LB / RB: the viewed player in 1st / 3rd person, the free camera mode in FREE CAMERA
		void cycle_selection(const int direction)
		{
			if (view == view_mode::free_camera)
			{
				// BO2 order with Mouse2 / RB: Free Roam Mode -> Dolly Camera -> Edit Camera Mode
				static constexpr std::array order{free_mode::free_roam, free_mode::dolly, free_mode::edit};
				auto index = 0;
				for (auto i = 0; i < 3; ++i)
				{
					if (order[i] == fmode) index = i;
				}

				enter_free_mode(order[(index + direction + 3) % 3]);
				return;
			}

			// every player, plus the recorded view (also when the recorder was spectating)
			std::vector<int> list;
			for (const auto& p : world::players())
			{
				list.push_back(p.client);
			}

			if (const auto recorded = recorded_client(); recorded >= 0 && std::ranges::find(list, recorded) == list.end())
			{
				list.push_back(recorded);
			}

			if (list.empty())
			{
				return;
			}

			std::ranges::sort(list);
			const auto current = viewed_client();
			auto index = static_cast<int>(std::ranges::find(list, current) - list.begin());
			if (index >= static_cast<int>(list.size()))
			{
				index = direction > 0 ? -1 : static_cast<int>(list.size());
			}

			const auto n = static_cast<int>(list.size());
			set_target(list[(index + direction + n) % n]);
		}

		// ------------------------------------------------------------------ markers (Edit Camera Mode)

		// the highlighted marker ("Edit Camera Marker", F / R3, Del), BO2's rule (t6mp FUN_006df630): the first marker in
		// time order within 40 units of the camera that is not more than 15 units behind the camera plane
		int find_near_marker()
		{
			vec3 axis[3];
			to_axis(from_angles(free_pitch, free_yaw, 0.0), axis);
			for (auto i = 0; i < static_cast<int>(keyframes.size()); ++i)
			{
				const auto to = keyframes[i].pos - free_pos;
				if (length(to) <= MARKER_HIGHLIGHT_DISTANCE && dot(axis[0], to) + MARKER_HIGHLIGHT_BEHIND >= 0.0)
				{
					return i;
				}
			}

			return -1;
		}

		void open_marker_popup(const int index)
		{
			popup = {};
			popup.kind = popup_kind::marker;
			popup.marker = index;
		}

		void insert_marker()
		{
			if (get_refdef()->tan_half_fov_y <= 0.0f)
			{
				return;
			}

			const auto pose = free_pose();
			const auto time = std::round(smooth_time);

			// BO2 rejects a second marker at the same moment of the film
			if (std::ranges::any_of(keyframes, [time](const keyframe& k) { return std::abs(k.time - time) < 50.0; }))
			{
				theater_hud::show_message("There is already a camera marker at this time");
				return;
			}

			keyframes.push_back({time, pose.pos, pose.rot, pose.fov});
			std::ranges::sort(keyframes, {}, &keyframe::time);
			save_keyframes();
		}

		// Free Roam: the player under the crosshair that "Attach To Object" would follow
		int find_attach_candidate()
		{
			if (view != view_mode::free_camera || fmode != free_mode::free_roam || attached >= 0)
			{
				return -1;
			}

			vec3 axis[3];
			to_axis(from_angles(free_pitch, free_yaw, 0.0), axis);

			auto best = -1;
			auto best_cos = std::cos(deg2rad(6.0));
			for (const auto& p : world::players())
			{
				if (!world::alive(p.client))
				{
					continue;
				}

				const auto feet = world::origin(p.client);
				if (!feet)
				{
					continue;
				}

				const auto to = (*feet + vec3{0.0, 0.0, 40.0}) - free_pos;
				const auto distance = length(to);
				if (distance < 16.0 || distance > 2500.0)
				{
					continue;
				}

				const auto c = dot(to * (1.0 / distance), axis[0]);
				if (c > best_cos)
				{
					best = p.client;
					best_cos = c;
				}
			}

			return best;
		}

		void attach_action()
		{
			if (attached >= 0)
			{
				attached = -1;
				return;
			}

			if (const auto client = find_attach_candidate(); client >= 0)
			{
				if (const auto feet = world::origin(client))
				{
					attached = client;
					attach_offset = free_pos - *feet;
				}
			}
		}

		// F / R3: Free Roam attach / detach, Edit Camera Mode markers
		void marker_action()
		{
			if (view == view_mode::free_camera && fmode == free_mode::free_roam)
			{
				attach_action();
				return;
			}

			if (view != view_mode::free_camera || fmode != free_mode::edit)
			{
				return;
			}

			if (repositioning)
			{
				if (reposition_marker >= 0 && reposition_marker < static_cast<int>(keyframes.size()))
				{
					const auto pose = free_pose();
					auto& k = keyframes[reposition_marker];
					k.pos = pose.pos;
					k.rot = pose.rot;
					k.fov = pose.fov;
					save_keyframes();
				}

				repositioning = false;
				reposition_marker = -1;
				return;
			}

			if (const auto near_index = find_near_marker(); near_index >= 0)
			{
				open_marker_popup(near_index);
				return;
			}

			insert_marker();
		}

		void goto_marker(const int index)
		{
			if (index < 0 || index >= static_cast<int>(keyframes.size()))
			{
				return;
			}

			set_free_from_pose(pose_of(keyframes[index]));
			seek_absolute(keyframes[index].time);
		}

		void delete_near_marker()
		{
			if (keyframes.empty())
			{
				return;
			}

			auto index = find_near_marker();
			if (index < 0)
			{
				const auto nearest = std::ranges::min_element(keyframes, {}, [](const keyframe& k) { return std::abs(k.time - smooth_time); });
				index = static_cast<int>(nearest - keyframes.begin());
			}

			keyframes.erase(keyframes.begin() + index);
			save_keyframes();
		}

		void jump_marker(const int direction)
		{
			const keyframe* wanted = nullptr;
			for (const auto& k : keyframes)
			{
				if (direction > 0 && k.time > smooth_time + 50.0)
				{
					wanted = &k;
					break;
				}

				if (direction < 0 && k.time < smooth_time - 50.0)
				{
					wanted = &k;
				}
			}

			if (wanted)
			{
				goto_marker(static_cast<int>(wanted - keyframes.data()));
			}
		}

		void reset_lens()
		{
			free_roll = 0.0;
			free_fov = 65.0;
		}

		void step_camera_speed(const int direction)
		{
			if (!theater_freecam_speed)
			{
				return;
			}

			auto& speed = theater_freecam_speed->current.value;
			speed = std::clamp(direction > 0 ? speed * 1.5f : speed / 1.5f, 10.0f, 5000.0f);
			theater_hud::show_message(std::format("camera speed {:.0f}", speed));
		}

		// ------------------------------------------------------------------ clips (BO2 RECORD)

		std::filesystem::path clips_dir()
		{
			const auto dir = console::get_theater_dir() / "clips";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			return dir;
		}

		bool can_record()
		{
			return !(view == view_mode::free_camera && fmode == free_mode::edit) && !replaying && popup.kind == popup_kind::none;
		}

		void start_clip()
		{
			if (!can_record())
			{
				return;
			}

			clip.clear();
			clip_recording = true;
		}

		void stop_clip()
		{
			clip_recording = false;
			if (clip.size() < 2 || clip.back().time - clip.front().time < 1000.0)
			{
				clip.clear();
				theater_hud::show_message("clip too short");
				return;
			}

			paused = true;
			popup = {};
			popup.kind = popup_kind::keep_clip;
		}

		void toggle_record()
		{
			if (clip_recording)
			{
				stop_clip();
			}
			else
			{
				start_clip();
			}
		}

		bool can_screenshot()
		{
			return !clip_recording && !(view == view_mode::free_camera && fmode != free_mode::free_roam) && popup.kind == popup_kind::none;
		}

		void take_screenshot()
		{
			if (!can_screenshot())
			{
				return;
			}

			const auto pos = static_cast<int>(demo_position() / 1000.0);
			theater_screenshot::request(std::format("{}_{:02}-{:02}", info.path.stem().string(), pos / 60, pos % 60));
		}

		void record_sample()
		{
			if (!clip_recording)
			{
				return;
			}

			// one sample every 4 ms of film time is plenty for the replay; nothing while paused
			if (!clip.empty() && smooth_time <= clip.back().time + 4.0 && clip.back().view == view && clip.back().target == target &&
				clip.back().fmode == fmode)
			{
				return;
			}

			clip.push_back({smooth_time, view, fmode, target, last_pose_valid ? last_pose : current_view_pose(), get_timescale()});
		}

		std::string clip_name_free(const std::string& base)
		{
			for (auto n = 1; n < 1000; ++n)
			{
				const auto name = std::format("{}_clip{}", base, n);
				if (!std::filesystem::exists(clips_dir() / (name + ".clip")))
				{
					return name;
				}
			}

			return base + "_clip";
		}

		bool save_clip(std::string name)
		{
			if (clip.empty())
			{
				return false;
			}

			for (auto& c : name)
			{
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
			}

			const auto path = clips_dir() / (name + ".clip");
			std::ofstream file(path, std::ios::trunc);
			file << "iw6-theater-clip 1\n";
			file << "demo " << info.path.filename().string() << "\n";
			for (const auto& s : clip)
			{
				file << std::format("s {:.2f} {} {} {} {:.3f} {:.3f} {:.3f} {:.6f} {:.6f} {:.6f} {:.6f} {:.2f} {:.3f}\n",
				                    s.time, static_cast<int>(s.view), static_cast<int>(s.fmode), s.target,
				                    s.pose.pos.x, s.pose.pos.y, s.pose.pos.z, s.pose.rot.w, s.pose.rot.x, s.pose.rot.y, s.pose.rot.z,
				                    s.pose.fov, s.timescale);
			}

			theater_hud::show_message("clip saved: " + name);
			console::info("theater: clip saved to %s\n", path.string().data());
			return true;
		}

		std::vector<clip_sample> load_clip(const std::string& name, std::string& demo_name)
		{
			std::vector<clip_sample> samples;
			std::ifstream file(clips_dir() / (name + ".clip"));
			std::string line;
			while (std::getline(file, line))
			{
				std::istringstream s(line);
				std::string tag;
				s >> tag;
				if (tag == "demo")
				{
					s >> demo_name;
					continue;
				}

				if (tag != "s")
				{
					continue;
				}

				clip_sample c{};
				int v{}, f{};
				s >> c.time >> v >> f >> c.target >> c.pose.pos.x >> c.pose.pos.y >> c.pose.pos.z >> c.pose.rot.w >> c.pose.rot.x >>
					c.pose.rot.y >> c.pose.rot.z >> c.pose.fov >> c.timescale;
				if (s)
				{
					c.view = static_cast<view_mode>(std::clamp(v, 0, 2));
					c.fmode = static_cast<free_mode>(std::clamp(f, 0, 2));
					c.pose.rot = normalize(c.pose.rot);
					samples.push_back(c);
				}
			}

			return samples;
		}

		void start_replay(std::vector<clip_sample> samples, const bool preview)
		{
			if (samples.size() < 2)
			{
				return;
			}

			replay = std::move(samples);
			replaying = true;
			replay_is_preview = preview;
			popup = {};
			paused = false;
			seek_absolute(replay.front().time);
		}

		void stop_replay(const bool reached_end)
		{
			replaying = false;
			replay.clear();
			if (reached_end)
			{
				paused = true;
				if (replay_is_preview)
				{
					popup = {};
					popup.kind = popup_kind::keep_clip;
					popup.selection = 2;
				}
				else
				{
					theater_hud::show_message("end of clip");
				}
			}
		}

		// replays the recorded camera of the clip at the current demo time
		void apply_replay()
		{
			if (!replaying || replay.size() < 2)
			{
				return;
			}

			const auto now = smooth_time;
			if (now < replay.front().time - 1500.0)
			{
				return; // still seeking to the start
			}

			if (now >= replay.back().time)
			{
				if (render != render_phase::none)
				{
					// rendering: hold the last camera for the frames still in flight, update_render ends it
					if (!engine_first_person())
					{
						write_pose(replay.back().pose);
					}
					return;
				}

				stop_replay(true);
				return;
			}

			const auto it = std::ranges::upper_bound(replay, now, {}, &clip_sample::time);
			const auto& b = it == replay.end() ? replay.back() : *it;
			const auto& a = it == replay.begin() ? replay.front() : *(it - 1);
			const auto u = std::clamp((now - a.time) / std::max(0.01, b.time - a.time), 0.0, 1.0);

			if (a.view != view || a.target != target)
			{
				view = a.view;
				fmode = a.fmode;
				target = a.target;
				sync_pov_target();
				apply_view_dvars();
			}

			if (!engine_first_person())
			{
				auto qb = b.pose.rot;
				if (dot(a.pose.rot, qb) < 0) qb = -qb;
				write_pose({lerp(a.pose.pos, b.pose.pos, u), slerp(a.pose.rot, qb, u), lerp(a.pose.fov, b.pose.fov, u)});
			}
		}

		// ------------------------------------------------------------------ video rendering (RENDER VIDEO)

		bool rendering()
		{
			return render != render_phase::none;
		}

		std::filesystem::path unique_video_path(const std::string& name)
		{
			const auto folder = theater_video::default_folder();
			std::error_code ec;
			std::filesystem::create_directories(folder, ec);

			auto path = folder / (name + ".mp4");
			for (auto n = 2; std::filesystem::exists(path, ec) && n < 1000; ++n)
			{
				path = folder / std::format("{}_{}.mp4", name, n);
			}

			return path;
		}

		// for messages: path::string() throws for characters outside the ANSI code page (the Videos folder is in
		// the user's profile, e.g. a Cyrillic user name), and a throw inside the frame hook would crash the game
		std::string display_path(const std::filesystem::path& path)
		{
			const auto& wide = path.native();
			const auto size = wide.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0)
			{
				return {};
			}

			std::string out(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		void end_render()
		{
			render = render_phase::none;
			demo_timescale::set_fixed_fps(0.0);
			theater_video::set_progress(-1.0f);

			if (saved_maxfps)
			{
				if (auto* maxfps = find_dvar("com_maxfps"))
				{
					maxfps->current.integer = *saved_maxfps;
				}

				saved_maxfps.reset();
			}

			set_render_mute(false);

			replaying = false;
			replay.clear();
			paused = true;
			apply_view_dvars();
		}

		void start_render(std::vector<clip_sample> samples, const std::string& name)
		{
			if (samples.size() < 2 || rendering())
			{
				return;
			}

			if (theater_video::busy())
			{
				theater_hud::show_message("a video is still being saved");
				return;
			}

			clip_recording = false; // a render replaces whatever was being recorded

			render_fps = theater_video_fps ? std::clamp(theater_video_fps->current.integer, 10, 240) : 60;
			render_path = unique_video_path(name);
			render_start = samples.front().time;
			render_end = samples.back().time;
			render_tail = 0;

			// the replay drives the camera and the speed of every moment; the demo clock advances exactly
			// 1000 / fps ms of film per rendered frame, so the video is smooth whatever the frame rate of the PC
			replay = std::move(samples);
			replaying = true;
			replay_is_preview = false;
			popup = {};
			paused = true; // the film stays still until the seek below has landed
			ff_speed = 0.0f;
			demo_timescale::set_fixed_fps(render_fps);

			// render as fast as the PC allows. Only the live value changes: the game saves the latched value in
			// its config (it may save it during the render), so the user's frame rate limit is never saved as 0
			if (auto* maxfps = find_dvar("com_maxfps"))
			{
				saved_maxfps = maxfps->current.integer;
				maxfps->current.integer = 0;
			}

			// the film runs faster than real time while rendering and the video has no sound anyway
			set_render_mute(true);

			render = render_phase::seeking;
			render_since = std::chrono::steady_clock::now();
			render_seek_frames = 0;

			// short pre-roll so the first frame is settled (seek_relative keeps the target inside the recording)
			render_seek_target = info.has_length()
				                     ? std::clamp(render_start - 400.0, static_cast<double>(info.first_time + 100), static_cast<double>(info.last_time - 300))
				                     : render_start - 400.0;
			seek_absolute(render_seek_target);

			console::info("theater: rendering %s at %d fps (Esc cancels)\n", display_path(render_path).data(), render_fps);
		}

		void cancel_render()
		{
			if (!rendering())
			{
				return;
			}

			theater_video::cancel();
			end_render();
			theater_hud::show_message("video cancelled");
		}

		// once per frame while rendering (main thread, before the camera of the frame is set)
		void update_render()
		{
			switch (render)
			{
			case render_phase::seeking:
			{
				++render_seek_frames;

				// the film is paused while the seek lands (a rewind restarts the demo internally); this must not
				// depend on real time: with the fixed clock the film can run many times faster than real time.
				// A rewind's frames are not presented (seek_relative): the film waits until they are again, so the
				// capture, which only sees presented frames, never starts on a held one
				if (paused)
				{
					const auto server = static_cast<double>(game::mp::cl->serverTime);
					const auto landed = render_seek_frames >= 3 && server >= render_seek_target - 200.0 && server <= render_start + 50.0;
					if (landed && !backward_seek_target && !theater_capture::holding_frames())
					{
						paused = false; // run the pre-roll with the fixed clock
					}
					else if (std::chrono::steady_clock::now() - render_since > 20s)
					{
						console::error("theater: the clip start could not be reached (at %.0f, clip starts at %.0f)\n", server, render_start);
						end_render();
						theater_hud::show_message("the video could not be started");
					}
					break;
				}

				// start a few frames early: the frames still in flight are skipped by the capture
				const auto step = 1000.0 * std::max(0.05f, get_timescale()) / render_fps;
				if (smooth_time < render_start - 3.0 * step)
				{
					break;
				}

				if (!theater_video::start(render_path, render_fps, 3))
				{
					const auto error = theater_video::take_error();
					console::error("theater: video could not be started: %s\n", error.data());
					end_render();
					theater_hud::show_message("video could not be started: " + error);
					break;
				}

				render = render_phase::capturing;
				render_frames = 0;
				render_capture_since = std::chrono::steady_clock::now();
				break;
			}
			case render_phase::capturing:
			{
				++render_frames;
				if (!theater_video::capturing())
				{
					// the capture stopped on its own (error)
					const auto error = theater_video::take_error();
					console::error("theater: video stopped: %s\n", error.data());
					render = render_phase::finishing;
					break;
				}

				theater_video::set_progress(static_cast<float>(std::clamp((smooth_time - render_start) / std::max(1.0, render_end - render_start), 0.0, 1.0)));

				// keep capturing a few frames after the end: the last frames of the clip are still in flight
				if (smooth_time >= render_end && ++render_tail > 3)
				{
					theater_video::stop();
					render = render_phase::finishing;
				}
				break;
			}
			case render_phase::finishing:
			{
				if (theater_video::busy())
				{
					break;
				}

				const auto result = theater_video::take_result();
				const auto error = theater_video::take_error();
				const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - render_capture_since).count();
				console::info("theater: rendered %d frames (%llu encoded) of %.2f s of film in %.1f s\n", render_frames,
				              static_cast<unsigned long long>(theater_video::frames()), (render_end - render_start) / 1000.0, seconds);
				end_render();

				if (result)
				{
					console::info("theater: video saved to %s\n", display_path(*result).data());
					theater_hud::show_message("video saved: " + display_path(result->filename()));
				}
				else
				{
					console::error("theater: the video could not be saved: %s\n", error.data());
					theater_hud::show_message("the video could not be saved");
				}
				break;
			}
			default:
				break;
			}
		}

		// Keep Clip "save": the .clip file and, like BO2's RENDER VIDEO, the video
		void save_and_render(const std::string& name)
		{
			if (!save_clip(name))
			{
				return;
			}

			if (!theater_video_on_save || theater_video_on_save->current.enabled)
			{
				start_render(clip, name);
			}
		}

		// ------------------------------------------------------------------ popups

		void close_popup()
		{
			popup = {};
		}

		void marker_popup_input(const int vertical, const int horizontal, const bool accept, const bool back, const int switch_marker)
		{
			if (popup.marker < 0 || popup.marker >= static_cast<int>(keyframes.size()))
			{
				close_popup();
				return;
			}

			if (back)
			{
				save_keyframes();
				close_popup();
				return;
			}

			if (switch_marker)
			{
				const auto n = static_cast<int>(keyframes.size());
				const auto index = (popup.marker + switch_marker + n) % n;
				save_keyframes();
				open_marker_popup(index);
				goto_marker(index);
				return;
			}

			const auto rows = marker_rows(popup.marker);
			popup.selection = std::clamp(popup.selection + vertical, 0, static_cast<int>(rows.size()) - 1);
			if (vertical)
			{
				popup.confirm = false;
			}

			auto& k = keyframes[popup.marker];
			const auto row = rows[popup.selection];
			if (horizontal || (accept && row == marker_row::speed_mode))
			{
				if (row == marker_row::speed_mode)
				{
					const auto step = horizontal ? horizontal : 1;
					k.mode = static_cast<speed_mode>((static_cast<int>(k.mode) + step + 3) % 3);
				}
				else if (row == marker_row::speed)
				{
					k.speed = std::clamp(std::round((k.speed + horizontal * 0.1) * 10.0) / 10.0, 0.1, 6.0);
				}

				save_keyframes();
				return;
			}

			if (!accept)
			{
				return;
			}

			switch (row)
			{
			case marker_row::reposition:
				repositioning = true;
				reposition_marker = popup.marker;
				close_popup();
				break;
			case marker_row::remove:
				keyframes.erase(keyframes.begin() + popup.marker);
				save_keyframes();
				close_popup();
				break;
			case marker_row::remove_all:
				if (!popup.confirm)
				{
					popup.confirm = true;
					break;
				}

				keyframes.clear();
				save_keyframes();
				close_popup();
				break;
			default:
				break;
			}
		}

		// Keep Clip: Autoname and Save Clip / Name and Save Clip / Preview Clip / Discard Clip
		void keep_clip_input(const int vertical, const bool accept)
		{
			popup.selection = std::clamp(popup.selection + vertical, 0, 3);
			if (!accept)
			{
				return;
			}

			switch (popup.selection)
			{
			case 0:
				close_popup();
				save_and_render(clip_name_free(info.path.stem().string()));
				clip.clear();
				break;
			case 1:
				if (last_device == input_device::gamepad)
				{
					// no on-screen keyboard: name it automatically
					close_popup();
					save_and_render(clip_name_free(info.path.stem().string()));
					clip.clear();
					break;
				}

				popup.kind = popup_kind::name_clip;
				popup.text.clear();
				break;
			case 2:
				start_replay(clip, true);
				break;
			default:
				// BO2: discarding a clip returns to where its recording started
				if (!clip.empty())
				{
					seek_absolute(clip.front().time);
				}

				clip.clear();
				close_popup();
				break;
			}
		}

		bool popup_key(const int k, const bool shift)
		{
			const auto enter = k == game::K_ENTER || k == game::K_KP_ENTER;
			const auto escape = k == game::K_ESCAPE;
			const auto vertical = k == game::K_UPARROW ? -1 : k == game::K_DOWNARROW ? 1 : 0;
			const auto horizontal = k == game::K_LEFTARROW ? -1 : k == game::K_RIGHTARROW ? 1 : 0;

			switch (popup.kind)
			{
			case popup_kind::dolly_notice:
				if (enter || escape || k == game::K_SPACE || k == game::K_MOUSE1)
				{
					close_popup();
				}
				return true;
			case popup_kind::marker:
				marker_popup_input(vertical, horizontal, enter || k == 'f', escape, k == game::K_MOUSE1 ? -1 : k == game::K_MOUSE2 ? 1 : 0);
				return true;
			case popup_kind::keep_clip:
				keep_clip_input(vertical, enter);
				return true;
			case popup_kind::name_clip:
				if (escape)
				{
					popup.kind = popup_kind::keep_clip;
					popup.selection = 1;
				}
				else if (enter)
				{
					const auto name = popup.text.empty() ? clip_name_free(info.path.stem().string()) : info.path.stem().string() + "_" + popup.text;
					close_popup();
					save_and_render(name);
					clip.clear();
				}
				else if (k == game::K_BACKSPACE && !popup.text.empty())
				{
					popup.text.pop_back();
				}
				else if (popup.text.size() < 24 && ((k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') || k == '-' || k == game::K_SPACE))
				{
					auto c = k == game::K_SPACE ? '_' : static_cast<char>(k);
					if (shift && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
					popup.text.push_back(c);
				}
				return true;
			default:
				return false;
			}
		}

		void popup_pad(const theater_gamepad::input& pad)
		{
			using namespace theater_gamepad;
			const auto vertical = pad.down(DPAD_UP) ? -1 : pad.down(DPAD_DOWN) ? 1 : 0;
			const auto horizontal = pad.down(DPAD_LEFT) ? -1 : pad.down(DPAD_RIGHT) ? 1 : 0;

			switch (popup.kind)
			{
			case popup_kind::dolly_notice:
				if (pad.down(A) || pad.down(B)) close_popup();
				break;
			case popup_kind::marker:
				marker_popup_input(vertical, horizontal, pad.down(A), pad.down(B),
				                   pad.down(LEFT_SHOULDER) ? -1 : pad.down(RIGHT_SHOULDER) ? 1 : 0);
				break;
			case popup_kind::keep_clip:
				keep_clip_input(vertical, pad.down(A));
				break;
			case popup_kind::name_clip:
				if (pad.down(B))
				{
					popup.kind = popup_kind::keep_clip;
					popup.selection = 1;
				}
				break;
			default:
				break;
			}
		}

		// ------------------------------------------------------------------ input

		bool ui_has_focus()
		{
			// console, menus and chat set key catchers
			return *game::keyCatchers != 0;
		}

		void update_hold(std::optional<std::chrono::steady_clock::time_point>& down, bool& fired)
		{
			if (down && !fired && std::chrono::steady_clock::now() - *down >= HOLD_TIME)
			{
				fired = true;
				take_screenshot();
			}
		}

		// the D-pad holds below end when the theater stops taking the controller's input (menu, popup, render)
		void stop_pad_holds()
		{
			speed_repeat_direction = 0;
			lens_pad_down.reset();
		}

		// D-pad up / down: permanent speed in 0.1x steps (BO2 controller layout 2), repeated while held. Like the
		// keyboard it works in every view, while paused (applies on resume) and while recording a clip.
		void update_pad_speed(const theater_gamepad::input& pad)
		{
			using namespace theater_gamepad;
			const auto now = std::chrono::steady_clock::now();
			if (pad.down(DPAD_UP) || pad.down(DPAD_DOWN))
			{
				speed_repeat_direction = pad.down(DPAD_UP) ? 1 : -1;
				speed_repeat_at = now + SPEED_REPEAT_DELAY;
				step_speed_fine(speed_repeat_direction);
				return;
			}

			const auto held = speed_repeat_direction > 0 ? pad.held(DPAD_UP) : speed_repeat_direction < 0 && pad.held(DPAD_DOWN);
			if (!held)
			{
				speed_repeat_direction = 0;
				return;
			}

			if (now >= speed_repeat_at)
			{
				speed_repeat_at += SPEED_REPEAT_INTERVAL;
				if (speed_repeat_at < now)
				{
					speed_repeat_at = now + SPEED_REPEAT_INTERVAL; // a long frame: no burst of steps
				}

				step_speed_fine(speed_repeat_direction);
			}
		}

		// D-pad left in the free camera (extra): held, the right stick zooms and rolls (update_free_camera); a short tap
		// without moving the stick resets the lens like C
		void update_pad_lens(const theater_gamepad::input& pad)
		{
			using namespace theater_gamepad;
			if (pad.down(DPAD_LEFT) && view == view_mode::free_camera)
			{
				lens_pad_down = std::chrono::steady_clock::now();
				lens_pad_used = false;
			}

			if (!lens_pad_down)
			{
				return;
			}

			if (pad.rx != 0.0f || pad.ry != 0.0f)
			{
				lens_pad_used = true;
			}

			if (!pad.held(DPAD_LEFT))
			{
				if (!lens_pad_used && view == view_mode::free_camera && std::chrono::steady_clock::now() - *lens_pad_down < LENS_TAP_TIME)
				{
					reset_lens();
				}

				lens_pad_down.reset();
			}
		}

		void handle_gamepad(const theater_gamepad::input& pad)
		{
			using namespace theater_gamepad;

			// no controller input (pad unplugged, or a menu / the console has the focus): RT counts as released, or the
			// film would keep fast-forwarding (Space could not pause it) until its end
			if (!pad.connected)
			{
				ff_speed = 0.0f;
				stop_pad_holds();
				return;
			}

			if (pad.any_activity())
			{
				last_device = input_device::gamepad;
			}

			// the button that closed a menu must not also trigger a theater action
			if (std::chrono::steady_clock::now() - last_ui_focus < 300ms)
			{
				ff_speed = 0.0f;
				stop_pad_holds();
				return;
			}

			// while a video is rendered only B (cancel) does something
			if (rendering())
			{
				ff_speed = 0.0f;
				stop_pad_holds();
				if (pad.down(B))
				{
					cancel_render();
				}
				return;
			}

			// popups use the D-pad themselves
			if (popup.kind != popup_kind::none)
			{
				ff_speed = 0.0f;
				stop_pad_holds();
				popup_pad(pad);
				return;
			}

			// BO2 "DEFAULT" theater button layout
			if (pad.down(A)) toggle_pause();
			if (pad.down(Y)) cycle_view();
			if (pad.down(B)) toggle_game_hud();
			if (pad.down(LEFT_THUMB)) toggle_controls();
			if (pad.down(LEFT_SHOULDER)) cycle_selection(-1);
			if (pad.down(RIGHT_SHOULDER)) cycle_selection(+1);
			if (pad.down(DPAD_RIGHT)) jump_forward();
			if (pad.down(RIGHT_THUMB)) marker_action();

			// D-pad up / down: speed (BO2 controller layout 2); D-pad left: the free camera's lens (extra)
			update_pad_speed(pad);
			update_pad_lens(pad);

			// LT: jump back to the previous 10 s mark
			static auto lt_was_down = false;
			const auto lt_down = pad.lt > 0.5f;
			if (lt_down && !lt_was_down) jump_back();
			lt_was_down = lt_down;

			// RT (pressure sensitive, BO2 "DEFAULT" layout): playback speed while held, 0.1x .. 6.0x, also from pause
			// (measured in Plutonium T6: 35% pull 1.5x, 67% 3.7x, 100% 6.0x; light pulls give slow motion)
			if (pad.rt > 0.02f && !clip_recording && !replaying && !at_end())
			{
				// the HUD shows this speed in yellow while RT is held; released, the speed from before is back (running_speed:
				// the selected one, or a Dolly Camera segment's), with its label if it had one (speed_label stays as it was)
				ff_speed = std::clamp(MAX_SPEED * pad.rt, MIN_SPEED, MAX_SPEED);
			}
			else
			{
				ff_speed = 0.0f;
			}

			// X: press = RECORD, hold = SCREENSHOT
			if (pad.down(X))
			{
				record_pad_down = std::chrono::steady_clock::now();
				record_pad_fired = false;
			}

			update_hold(record_pad_down, record_pad_fired);

			if (pad.up(X) && record_pad_down)
			{
				if (!record_pad_fired) toggle_record();
				record_pad_down.reset();
			}
		}

		// returns true if the key is consumed by the theater
		bool handle_key(const int k, const bool down, const bool repeat)
		{
			const auto shift = key(game::K_SHIFT);

			// while a video is rendered only Esc (cancel) does something; nothing else may reach the game
			if (rendering())
			{
				if (down && !repeat && k == game::K_ESCAPE)
				{
					cancel_render();
				}
				return true;
			}

			if (popup.kind != popup_kind::none)
			{
				if (down)
				{
					last_device = input_device::keyboard;
					popup_key(k, shift);
				}
				return true; // a popup owns the keyboard (Escape included)
			}

			// free camera movement keys are held, not pressed
			const auto movement_key = k == 'w' || k == 'a' || k == 's' || k == 'd' || k == 'q' || k == 'e' ||
				k == 'z' || k == 'x' || k == game::K_SHIFT || k == game::K_CTRL;
			if (view == view_mode::free_camera && (movement_key || k == game::K_MWHEELUP || k == game::K_MWHEELDOWN))
			{
				if (down && k == game::K_MWHEELUP) free_fov = std::clamp(free_fov - 3.0, 5.0, 160.0);
				if (down && k == game::K_MWHEELDOWN) free_fov = std::clamp(free_fov + 3.0, 5.0, 160.0);
				return true;
			}

			// R: press = RECORD, hold = SCREENSHOT
			if (k == 'r')
			{
				if (down && !repeat)
				{
					record_key_down = std::chrono::steady_clock::now();
					record_key_fired = false;
				}
				else if (!down && record_key_down)
				{
					if (!record_key_fired) toggle_record();
					record_key_down.reset();
				}
				return true;
			}

			static const std::unordered_set<int> theater_keys{
				game::K_F2, game::K_F3, game::K_F4, game::K_SPACE, game::K_LEFTARROW, game::K_RIGHTARROW, game::K_UPARROW,
				game::K_DOWNARROW, game::K_MOUSE1, game::K_MOUSE2, game::K_HOME, game::K_PGUP, game::K_PGDN, game::K_DEL,
				'f', 'c', 'j', 'l'};

			if (!theater_keys.contains(k))
			{
				return false;
			}

			if (!down || repeat)
			{
				return true;
			}

			last_device = input_device::keyboard;

			switch (k)
			{
			case game::K_F4: toggle_controls(); break;           // Toggle Controls
			case game::K_F2: toggle_game_hud(); break;           // Toggle Hud
			case game::K_F3: cycle_view(); break;                // 1ST PERSON / 3RD PERSON / FREE CAMERA
			case game::K_MOUSE1: cycle_selection(-1); break;     // previous player / free camera mode
			case game::K_MOUSE2: cycle_selection(+1); break;     // next player / free camera mode
			case game::K_SPACE: toggle_pause(); break;
			case game::K_UPARROW: step_speed(+1); break;
			case game::K_DOWNARROW: step_speed(-1); break;
			case game::K_LEFTARROW: shift ? seek_relative(-30000) : jump_back(); break;   // shift: extra
			case game::K_RIGHTARROW: shift ? seek_relative(30000) : jump_forward(); break;
			case 'f': marker_action(); break;                    // Insert / Edit Camera Marker
			// extras (not in BO2)
			case game::K_HOME: go_to_start(); break;
			case game::K_PGUP: step_camera_speed(+1); break;
			case game::K_PGDN: step_camera_speed(-1); break;
			case 'c': reset_lens(); break;
			case 'j': jump_marker(-1); break;
			case 'l': jump_marker(+1); break;
			case game::K_DEL: if (view == view_mode::free_camera && fmode == free_mode::edit) delete_near_marker(); break;
			default: break;
			}

			return true;
		}

		void cl_key_event_stub(const int local_client_num, const int k, const int down, const unsigned int time)
		{
			if (active() && !ui_has_focus())
			{
				auto repeat = false;
				if (k >= 0 && k < static_cast<int>(keys_down.size()))
				{
					repeat = down != 0 && keys_down[k];
					keys_down[k] = down != 0;
				}

				if (handle_key(k, down != 0, repeat))
				{
					return;
				}

				// Esc goes on to the game, which opens the Start menu: with the HUD off it needs cg_draw2D
				if (down != 0 && !repeat && k == game::K_ESCAPE)
				{
					show_hud_for_menu();
				}
			}
			else
			{
				keys_down.fill(false);
			}

			cl_key_event_hook.invoke<void>(local_client_num, k, down, time);
		}

		int cl_mouse_event_stub(const int x, const int y, const int dx, const int dy)
		{
			const auto looks = view == view_mode::third_person || (view == view_mode::free_camera && fmode != free_mode::dolly);
			if (active() && !ui_has_focus() && (looks || popup.kind != popup_kind::none || rendering()))
			{
				if (popup.kind == popup_kind::none && !rendering())
				{
					mouse_dx += dx;
					mouse_dy += dy;
				}

				if (dx != 0 || dy != 0)
				{
					last_device = input_device::keyboard;
				}
				return 1; // keep the cursor captured
			}

			return cl_mouse_event_hook.invoke<int>(x, y, dx, dy);
		}

		// ------------------------------------------------------------------ per frame

		// the film's speed when it runs without RT: the selected speed, unless a clip replay or a Dolly Camera marker segment
		// (Linear / Fixed) sets it, as those set BO2's one playback speed. The speed label shows this one too.
		float running_speed()
		{
			if (replaying && !replay.empty())
			{
				const auto it = std::ranges::upper_bound(replay, smooth_time, {}, &clip_sample::time);
				const auto timescale = (it == replay.begin() ? replay.front() : *(it - 1)).timescale;
				return timescale <= 0.0f ? 1.0f : timescale;
			}

			if (view == view_mode::free_camera && fmode == free_mode::dolly)
			{
				return dolly_speed(smooth_time);
			}

			return selected_speed;
		}

		void apply_timescale()
		{
			const auto wanted = ff_speed > 0.0f ? ff_speed : paused ? 0.0f : running_speed();
			if (get_timescale() != wanted)
			{
				set_timescale(wanted);
			}
		}

		// Death hold (demo_pov): after the viewed player's death the sidecar keeps its last dead playerState until it
		// respawns, and the engine draws its dead camera from it: CG_OffsetThirdPersonView 0x14029B7B0 pivots on the
		// corpse's j_mainroot (centity 0x2A + ps.corpseIndex) and pulls back 120 units until a trace hits something.
		// About 2 s after the death the corpse turns into a ragdoll: the pivot moves and the
		// camera can collapse onto the body (ejemplo_bots, client 6 at 00:51.3: the pivot moved 21 units and the camera
		// went from 92 to 8 units behind the body, looking at the ground). Nobody saw that in the match, where the
		// player was in its killcam or had respawned by then. Like BO2's death camera, the camera stays where it was
		// when the hold started; the world keeps moving. Called after CG_CalcViewValues, in the engine's 1st person.
		void hold_death_camera()
		{
			const auto held = target >= 0 && view == view_mode::first_person ? demo_pov::held_record() : -1;
			if (held < 0)
			{
				hold_pose_record = -1;
				return;
			}

			const auto now = game::mp::cl->serverTime;
			const auto seek = std::abs(now - hold_pose_server_time) > HOLD_POSE_JUMP_MSEC;
			hold_pose_server_time = now;
			if (held != hold_pose_record || target != hold_pose_target || seek)
			{
				hold_pose_record = held;
				hold_pose_target = target;
				hold_pose_settle = HOLD_POSE_SETTLE_FRAMES;
			}

			// the first frames still show the engine's pose (after a seek the corpse may need a frame before the dead
			// camera can pivot on it); the last of them is kept
			if (hold_pose_settle > 0)
			{
				--hold_pose_settle;
				hold_pose = current_view_pose();
				return;
			}

			write_pose(hold_pose);
		}

		void calc_view_values_stub(const int local_client_num)
		{
			cg_calc_view_values_hook.invoke<void>(local_client_num);

			camera_written = false; // until write_pose places the camera of this frame
			if (!active())
			{
				return;
			}

			// while a video is rendered the film advances a fixed step per frame, whatever the real frame time was
			const auto real_dt = frame_seconds();
			const auto fixed_fps = demo_timescale::get_fixed_fps();
			const auto dt = fixed_fps > 0.0 ? 1.0 / fixed_fps : real_dt;
			update_smooth_time(dt);

			// poll every frame (keeps button edges correct), act only when no menu has focus
			static const theater_gamepad::input no_input{};
			const auto& polled = theater_gamepad::poll();
			const auto& pad = ui_has_focus() ? no_input : polled;
			handle_gamepad(pad);

			// START goes on to the game, which opens the Start menu: with the HUD off it needs cg_draw2D
			if (pad.down(theater_gamepad::START) && !rendering())
			{
				show_hud_for_menu();
			}

			update_hold(record_key_down, record_key_fired);
			update_render();
			apply_timescale();
			apply_replay();

			// the view the demo recorded, or another player's real view through the sidecar
			if (engine_first_person())
			{
				hold_death_camera();
				last_pose = current_view_pose();
				last_pose_valid = true;

				// an eye followed before the engine took over the view is stale by now: when the sidecar view of the
				// player stops while it is dead (a death hold that ends at the final killcam, a disconnect, ...), the
				// camera stays on the view the engine showed last (last_pose) instead of jumping back to where the
				// player was when it was selected; an alive player is picked up again at its current eye
				follow_valid = false;
			}
			else if (!replaying || view == view_mode::first_person)
			{
				switch (view)
				{
				case view_mode::first_person:
					update_follow_first_person(dt);
					break;
				case view_mode::third_person:
					update_third_person(dt, popup.kind == popup_kind::none ? pad : no_input);
					break;
				case view_mode::free_camera:
					if (fmode == free_mode::dolly && !keyframes.empty())
					{
						if (const auto pose = evaluate(smooth_time))
						{
							write_pose(*pose);
						}
					}
					else
					{
						// attached: the camera moves with the player, the user moves the offset
						std::optional<vec3> base{};
						if (attached >= 0 && fmode == free_mode::free_roam)
						{
							base = world::alive(attached) ? world::origin(attached) : std::optional<vec3>{};
							if (base)
							{
								free_pos = *base + attach_offset;
							}
							else
							{
								attached = -1; // died or left
							}
						}

						update_free_camera(dt, popup.kind == popup_kind::none ? pad : no_input);
						if (base)
						{
							attach_offset = free_pos - *base;
						}

						write_pose(free_pose());
						candidate = find_attach_candidate();
					}
					break;
				}
			}

			record_sample();

			// Edit Camera Mode: camera models and light cones of the markers (the scene and FX beam lists of this frame
			// are open here; CG_DrawActiveFrame adds the other players' lasers after this)
			markers::draw_scene();
		}

		// ------------------------------------------------------------------ spectator text

		// BO2's theater never shows a "spectating" text. Ghosts' spectator HUD (ui\lui\mp_hud\spectatorhud.lua: UIText
		// "spectatorTextId" = "@LUA_MENU_SPECTATING_CAPS") shows "SPECTATING" whenever the recorder was spectating: for the
		// whole film, in every theater view, when it recorded as a spectator (a local bot match started from the console
		// leaves the host as one), otherwise during its spectator periods. The LUI localizes that text only when the map's
		// HUD is built (checked in game: a later change shows at the next map load), through Engine.Localize ->
		// SEH_StringEd_GetString, so that lookup returns "" for this key while a demo plays: from the frame the demo file
		// opens (before its map loads) until playback ends (before a real match can build its HUD). A detour, not an
		// emptied LocalizeEntry: vid_restart (also run by CL_ParseGamestate when fs_game changed, as for a demo recorded
		// with a mod) unloads and loads every zone, so the HUD it builds again would get a fresh entry. CB's iw6x.exe
		// already detours this function (iw6-mod localized_strings.cpp: the game:addlocalizedstring overrides); this
		// detour sits on top of it, like the CL_KeyEvent one, so it also wins over an override of this key.
		utils::hook::detour seh_string_ed_get_string_hook;
		std::atomic_bool spectating_text_hidden{false}; // set by on_frame; the lookup may run on any thread

		constexpr auto SPECTATING_TEXT = "LUA_MENU_SPECTATING_CAPS";

		const char* seh_string_ed_get_string_stub(const char* reference)
		{
			// "" and not NULL, for which Engine.Localize would show the key itself
			if (spectating_text_hidden && reference && _stricmp(reference, SPECTATING_TEXT) == 0)
			{
				return "";
			}

			return seh_string_ed_get_string_hook.invoke<const char*>(reference);
		}

		// ------------------------------------------------------------------ playback lifecycle

		void reset_session()
		{
			if (rendering())
			{
				theater_video::cancel();
				end_render();
			}

			if (backward_seek_target)
			{
				theater_capture::release_frames(0);
				backward_seek_target.reset();
			}

			view = view_mode::first_person;
			fmode = free_mode::free_roam;
			target = -1;
			demo_pov::set_target(-1);
			paused = false;
			selected_speed = 1.0f;
			speed_label = false;
			ff_speed = 0.0f;
			stop_pad_holds();
			popup = {};
			repositioning = false;
			reposition_marker = -1;
			attached = -1;
			candidate = -1;
			clip_recording = false;
			clip.clear();
			replaying = false;
			replay.clear();
			keys_down.fill(false);
			reset_follow();
			last_pose_valid = false;
			world::set_hidden_player(-1);
			world::set_force_local_body(false);
			game_hud_hidden = false;

			// the camera model of the markers belongs to the map: free it (after the render thread is done with it)
			markers::reset();
		}

		void on_frame()
		{
			// read by the sound update (film pause), which must not look the dvar up itself
			if (!demotimescale_dvar)
			{
				demotimescale_dvar = find_dvar("demotimescale");
			}

			// film pause / game sound: whether the film clock stands still in this frame. The demo clock of the frame
			// (Com_TimeScaleMsec, later in the frame) uses the timescale apply_timescale wrote in the previous frame;
			// the paused term also covers a demo restart that set the timescale back to 1 while the film is paused
			film_frozen_frame = film_held();
			if (seek_drop_frames > 0)
			{
				--seek_drop_frames;
			}

			// leaving the film while a video is rendered (Start menu "Leave Game", disconnect, error) cancels the render
			// at once: playing() only turns false seconds later, and until then the main menu would stay muted, run
			// without the frame rate limit and be captured into the video. A rewind never goes below CA_LOADING.
			if (rendering() && *game::mp::connstate < game::CA_LOADING)
			{
				console::warn("theater: the film was left during a video render, the video is cancelled\n");
				cancel_render();
			}

			// A theater session lasts as long as the same demo file is being played. Rewinding restarts the
			// demo internally (connstate drops to CA_PRIMED for a moment), which must not reset the session.
			const auto playing = demo_playback::playing();
			const auto path = playing ? demo_playback::get_path() : std::filesystem::path{};

			// no "SPECTATING" while a demo plays (BO2); set before the demo's map builds its HUD
			spectating_text_hidden = playing;

			if (theater_film)
			{
				theater_film->current.enabled = playing;
			}

			// a backward seek's frames are presented again once the demo has restarted and is back at the target: active,
			// with the snapshot around the target (right after the restart the time can already be there while the
			// snapshot is still the demo's start). Two more frames stay held for the render thread's delay. Given up after
			// 1.5 s, or when the film is left, so the screen can never stay frozen.
			if (backward_seek_target)
			{
				const auto landed = demo_playback::restarts() != backward_seek_restarts && *game::mp::connstate == game::CA_ACTIVE &&
					std::abs(game::mp::cl->snap.serverTime - *backward_seek_target) <= BACKWARD_SEEK_LANDED_MSEC;
				const auto gave_up = !playing || std::chrono::steady_clock::now() - backward_seek_at > BACKWARD_SEEK_TIMEOUT;
				if (landed || gave_up)
				{
					theater_capture::release_frames(gave_up ? 0 : BACKWARD_SEEK_TRAILING_FRAMES);
					backward_seek_target.reset();
				}
			}

			// the controller belongs to the theater while a demo is on screen and no menu is open
			const auto menu_focus = ui_has_focus();
			if (menu_focus)
			{
				last_ui_focus = std::chrono::steady_clock::now();
			}
			theater_gamepad::set_consume(active() && !menu_focus);

			if (playing && !was_active)
			{
				backup_stats();
			}

			if (playing && (!was_active || path != info.path))
			{
				read_demo_info(path);
				load_keyframes();
				reset_session();
				demo_pov::set_show_death(true); // like BO2: the viewed player's death is shown, not someone else's view
				smooth_time = game::mp::cl->serverTime;
				apply_view_dvars();
				console::info("theater: %s (%d markers, %s)\n", info.path.filename().string().data(), static_cast<int>(keyframes.size()),
				              demo_pov::available() ? "all players' views" : "recorded view only");
			}
			else if (!playing && was_active)
			{
				reset_session();
				hud_for_menu = {}; // restore_dvars puts cg_draw2D back as it was before the film
				restore_dvars();
				restore_stats();
				info = {};
			}

			// the markers' camera model of a map that was loaded again (demo restarted from the menu) is freed here,
			// at the start of the frame and not while CG_DrawActiveFrame builds the scene
			markers::update();

			const auto now_active = active();
			if (now_active)
			{
				update_hud_for_menu();
				apply_game_hud();
				apply_view_dvars();

				// like BO2: hold on the last frame instead of leaving the match when the recording ends
				if (at_end() && (!paused || ff_speed > 0.0f))
				{
					paused = true;
					ff_speed = 0.0f;
					if (clip_recording)
					{
						stop_clip();
					}
				}

				if (const auto shot = theater_screenshot::take_result())
				{
					theater_hud::show_message("screenshot saved: " + shot->filename().string());
				}
			}

			was_active = playing;
		}

		void print_help()
		{
			console::info("theater controls (BO2 Film Editor):\n");
			console::info("  F4 / L3        toggle controls        F2 / B         toggle hud\n");
			console::info("  F3 / Y         1st person, 3rd person, free camera\n");
			console::info("  mouse1-2 / LB-RB  previous / next player (free camera: Free Roam, Dolly Camera, Edit Camera Mode)\n");
			console::info("  space / A      pause / play\n");
			console::info("  up-down        speed 0.1x - 6.0x (0.2x steps)     dpad up-down   speed in 0.1x steps (hold to repeat)\n");
			console::info("  left-right / LT-dpad right   previous / next 10 s mark      RT (hold)  fast forward\n");
			console::info("  R / X          record a clip          hold R / X     screenshot\n");
			console::info("  F / R3         Edit Camera Mode: insert marker, or edit the highlighted marker (within 40 units)\n");
			console::info("  free camera: W A S D + mouse (left stick + right stick), E / Q up / down, shift / ctrl faster / slower\n");
			console::info("extras: Z / X roll, wheel zoom, hold dpad left + right stick zoom / roll, C / tap dpad left reset zoom and roll,\n");
			console::info("        PgUp / PgDn camera speed, Home start, shift + left / right -30 s / +30 s, J / L previous / next marker,\n");
			console::info("        Del delete marker\n");
			console::info("commands: theater_list, theater_play <demo>, theater_clips, theater_clip <name>, theater_cam <1st|3rd|free|dolly|edit>,\n");
			console::info("          theater_player <client|-1>, theater_speed <x>, theater_seek <sec>, theater_goto <sec>\n");
		}

		void add_commands()
		{
			command::add("theater_help", print_help);

			command::add("theater_play", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: theater_play <demo name>   (demos are in <game>\\demos\\client\\user and \\auto)\n");
					return;
				}

				command::execute(std::format("demoplay \"{}\" repeat", params.join(1)));
			});

			command::add("theater_cam", [](const command::params& params)
			{
				const std::string arg = params.size() > 1 ? utils::string::to_lower(params[1]) : "";
				if (arg == "1st" || arg == "game")
				{
					while (view != view_mode::first_person) cycle_view();
				}
				else if (arg == "3rd")
				{
					while (view != view_mode::third_person) cycle_view();
				}
				else if (arg == "free" || arg == "dolly" || arg == "edit")
				{
					while (view != view_mode::free_camera) cycle_view();
					enter_free_mode(arg == "free" ? free_mode::free_roam : arg == "dolly" ? free_mode::dolly : free_mode::edit);
				}
				else
				{
					console::info("usage: theater_cam <1st|3rd|free|dolly|edit>\n");
				}
			});

			command::add("theater_player", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					for (const auto& p : world::players())
					{
						console::info("  %2d  %-16s team %d %s\n", p.client, p.name.data(), p.team, world::alive(p.client) ? "" : "(dead)");
					}
					console::info("viewing %d (recorded view %d, recorder %d)%s\n", target, world::recorded_view_client(), world::recorder_client(),
					              demo_pov::available() ? ", sidecar loaded" : "");
					return;
				}

				set_target(std::atoi(params[1]));
			});

			command::add("theater_speed", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: theater_speed <0.1 - 6>\n");
					return;
				}

				selected_speed = std::clamp(static_cast<float>(std::atof(params[1])), MIN_SPEED, MAX_SPEED);
				speed_label = true;
			});

			command::add("theater_pause", toggle_pause);

			command::add("theater_list", []()
			{
				char base[MAX_PATH]{};
				GetModuleFileNameA(nullptr, base, sizeof(base));
				const auto demos_dir = std::filesystem::path(base).parent_path() / "demos";

				std::error_code ec;
				std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
				for (const auto& entry : std::filesystem::recursive_directory_iterator(demos_dir, ec))
				{
					if (entry.is_regular_file(ec) && entry.path().extension() == ".dm_iw6")
					{
						files.emplace_back(entry.last_write_time(ec), entry.path());
					}
				}

				std::ranges::sort(files, std::greater{});
				console::info("%zu demos in %s (newest first) - play with: theater_play <name>\n", files.size(), demos_dir.string().data());
				for (const auto& [time, file] : files)
				{
					const auto saved = info;
					read_demo_info(file);
					const auto length = info.has_length() ? (info.last_time - info.first_time) / 1000 : -1;
					info = saved;

					const auto folder = file.parent_path().filename().string();
					const auto pov = std::filesystem::exists(file.string() + ".pov") ? " [all players]" : "";
					if (length >= 0)
					{
						console::info("  %-40s %02d:%02d  [%s]%s\n", file.stem().string().data(), length / 60, length % 60, folder.data(), pov);
					}
					else
					{
						console::info("  %-40s --:--  [%s] (incomplete)\n", file.stem().string().data(), folder.data());
					}
				}
			});

			command::add("theater_seek", [](const command::params& params)
			{
				if (params.size() < 2) return;
				seek_relative(static_cast<int>(std::atof(params[1]) * 1000.0));
			});

			command::add("theater_goto", [](const command::params& params)
			{
				if (params.size() < 2 || !info.has_length()) return;
				seek_absolute(info.first_time + std::atof(params[1]) * 1000.0);
			});

			command::add("theater_clips", []()
			{
				std::error_code ec;
				for (const auto& entry : std::filesystem::directory_iterator(clips_dir(), ec))
				{
					if (entry.path().extension() == ".clip")
					{
						console::info("  %s\n", entry.path().stem().string().data());
					}
				}
			});

			command::add("theater_clip", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: theater_clip <name>   (plays a saved clip of the demo being watched; list: theater_clips)\n");
					return;
				}

				std::string demo_name;
				auto samples = load_clip(params[1], demo_name);
				if (samples.size() < 2)
				{
					console::warn("theater: clip %s not found or empty\n", params[1]);
					return;
				}

				if (!demo_name.empty() && demo_name != info.path.filename().string())
				{
					console::warn("theater: clip %s belongs to %s\n", params[1], demo_name.data());
					return;
				}

				start_replay(std::move(samples), false);
			});

			command::add("theater_render", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: theater_render <clip>   (renders a saved clip of the demo being watched to Videos\\IW6X Theater)\n");
					return;
				}

				if (!active())
				{
					console::warn("theater: play the clip's demo first\n");
					return;
				}

				std::string demo_name;
				auto samples = load_clip(params[1], demo_name);
				if (samples.size() < 2)
				{
					console::warn("theater: clip %s not found or empty\n", params[1]);
					return;
				}

				if (!demo_name.empty() && demo_name != info.path.filename().string())
				{
					console::warn("theater: clip %s belongs to %s\n", params[1], demo_name.data());
					return;
				}

				start_render(std::move(samples), params[1]);
			});

			command::add("theater_render_cancel", cancel_render);

			command::add("theater_markers", []()
			{
				for (std::size_t i = 0; i < keyframes.size(); ++i)
				{
					const auto& k = keyframes[i];
					console::info("%2zu  t=%.2fs  pos=(%.0f %.0f %.0f)  fov=%.1f  mode=%d speed=%.1f\n", i, (k.time - info.first_time) / 1000.0,
					              k.pos.x, k.pos.y, k.pos.z, k.fov, static_cast<int>(k.mode), k.speed);
				}
			});

			// developer helpers
			command::add("theater_camstate", []()
			{
				const auto* refdef = get_refdef();
				console::info("refdef org=(%.1f %.1f %.1f) fwd=(%.2f %.2f %.2f) tan=(%.3f %.3f) view=%d free=%d target=%d time=%.0f popup=%d\n",
				              refdef->vieworg[0], refdef->vieworg[1], refdef->vieworg[2],
				              refdef->viewaxis[0][0], refdef->viewaxis[0][1], refdef->viewaxis[0][2],
				              refdef->tan_half_fov_x, refdef->tan_half_fov_y, static_cast<int>(view), static_cast<int>(fmode), target,
				              smooth_time, static_cast<int>(popup.kind));
			});

			command::add("theater_freecam_set", [](const command::params& params)
			{
				// theater_freecam_set x y z pitch yaw [roll] [fov]
				if (params.size() < 6) return;
				free_pos = {std::atof(params[1]), std::atof(params[2]), std::atof(params[3])};
				free_pitch = std::atof(params[4]);
				free_yaw = std::atof(params[5]);
				free_roll = params.size() > 6 ? std::atof(params[6]) : 0.0;
				if (params.size() > 7) free_fov = std::atof(params[7]);
				free_vel = {};
			});

			// film pause / game sound: held = paused by SND_PauseSounds, waiting = started during the pause (no packet yet)
			command::add("theater_soundstate", []()
			{
				auto active = 0, held = 0, waiting = 0;
				for (auto i = 0; i < SND_VOICE_COUNT; ++i)
				{
					if (*reinterpret_cast<const int*>(SND_VOICE_ACTIVE + i * sizeof(int)) == 0)
					{
						continue;
					}

					++active;
					if (*reinterpret_cast<const int*>(SND_VOICE_HELD + i * SND_DRIVER_STRIDE) != 0)
					{
						++held;
					}
					else if (*reinterpret_cast<const std::uint8_t*>(SND_CHANNEL_PAUSED + i * SND_CHANNEL_STRIDE) != 0)
					{
						++waiting;
					}
				}

				const auto* cl_paused = *reinterpret_cast<game::dvar_t* const*>(CL_PAUSED);
				console::info("sound: film paused %d (patch %s), engine paused %d, cl_paused %d, voices %d (%d held, %d waiting), seek drop %d, master volume %.3f%s\n",
				              film_on_screen() && film_held() ? 1 : 0, film_sound_pause_installed ? "on" : "off",
				              static_cast<int>(*reinterpret_cast<const std::uint8_t*>(SND_PAUSED)), cl_paused ? cl_paused->current.integer : -1,
				              active, held, waiting, seek_drop_frames, static_cast<double>(*reinterpret_cast<const float*>(SND_MASTER_VOLUME)),
				              render_muted ? " (render mute)" : "");
			});
		}
	}

	bool active()
	{
		return demo_playback::playing() && *game::mp::connstate == game::CA_ACTIVE && game::CL_IsCgameInitialized();
	}

	view_mode get_view_mode() { return view; }
	free_mode get_free_mode() { return fmode; }
	const std::vector<keyframe>& get_keyframes() { return keyframes; }
	const demo_info& get_demo_info() { return info; }
	double get_smooth_time() { return smooth_time; }
	bool is_paused() { return get_timescale() == 0.0f; }
	float get_speed() { return running_speed(); }
	bool speed_label_visible() { return speed_label; }
	bool fast_forwarding() { return ff_speed > 0.0f; }
	float fast_forward_speed() { return ff_speed; }
	input_device last_input_device() { return last_device; }
	float camera_speed() { return theater_freecam_speed ? theater_freecam_speed->current.value : 250.0f; }
	bool recording_clip() { return clip_recording; }
	bool can_jump_back() { return jump_back_allowed(); }
	bool can_jump_forward() { return jump_forward_allowed(); }
	const popup_state& get_popup() { return popup; }
	bool repositioning_marker() { return repositioning; }
	int reposition_index() { return repositioning ? reposition_marker : -1; }
	std::uint32_t keyframes_revision() { return keyframes_rev; }
	int attach_target() { return view == view_mode::free_camera && fmode == free_mode::free_roam ? attached : -1; }
	int attach_candidate() { return view == view_mode::free_camera && fmode == free_mode::free_roam && attached < 0 ? candidate : -1; }
	bool previewing_clip() { return replaying && replay_is_preview; }
	bool playing_clip() { return replaying && !replay_is_preview; }
	bool hide_theater_hud() { return theater_screenshot::hide_theater_hud() || render != render_phase::none; }
	bool controls_visible() { return !theater_hud || theater_hud->current.enabled; }

	std::optional<camera_pose> evaluate_dolly(const double time)
	{
		return evaluate(time);
	}

	std::string selector_text()
	{
		if (view == view_mode::free_camera)
		{
			switch (fmode)
			{
			case free_mode::dolly: return "Dolly Camera";
			case free_mode::edit: return "Edit Camera Mode";
			default: return "Free Roam Mode";
			}
		}

		return world::client_name(viewed_client());
	}

	int near_marker()
	{
		if (view != view_mode::free_camera || fmode != free_mode::edit)
		{
			return -1;
		}

		return find_near_marker();
	}

	std::vector<marker_row> marker_rows(const int marker)
	{
		std::vector<marker_row> rows{marker_row::speed_mode};
		if (marker >= 0 && marker < static_cast<int>(keyframes.size()) && keyframes[marker].mode != speed_mode::manual)
		{
			rows.push_back(marker_row::speed);
		}

		rows.push_back(marker_row::reposition);
		rows.push_back(marker_row::remove);
		rows.push_back(marker_row::remove_all);
		return rows;
	}

	life_marks get_life_marks()
	{
		static int cached_client = -2;
		static std::chrono::steady_clock::time_point cached_at{};
		static life_marks cached{};

		const auto client = viewed_client();
		const auto now = std::chrono::steady_clock::now();
		if (client == cached_client && now - cached_at < 2s)
		{
			return cached;
		}

		cached = {};
		auto first_event = true;
		for (const auto& e : demo_pov::get_life_events(client))
		{
			switch (e.kind)
			{
			case demo_pov::life_event::type::kill: cached.kills.push_back(e.server_time); break;
			case demo_pov::life_event::type::death: cached.deaths.push_back(e.server_time); break;
			case demo_pov::life_event::type::spawn:
				// BO2 paints the film red until the player's first spawn; a spawn after a death is a respawn
				if (first_event) cached.first_spawn = e.server_time;
				break;
			}

			first_event = false;
		}

		cached_client = client;
		cached_at = now;
		return cached;
	}

	bool get_render_camera(vec3& origin, vec3 axis[3], float& tan_half_fov_x, float& tan_half_fov_y)
	{
		const auto* refdef = get_refdef();
		origin = {refdef->vieworg[0], refdef->vieworg[1], refdef->vieworg[2]};
		for (auto i = 0; i < 3; ++i)
		{
			axis[i] = {refdef->viewaxis[i][0], refdef->viewaxis[i][1], refdef->viewaxis[i][2]};
		}

		tan_half_fov_x = refdef->tan_half_fov_x;
		tan_half_fov_y = refdef->tan_half_fov_y;
		return tan_half_fov_x > 0.0f && tan_half_fov_y > 0.0f;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (reinterpret_cast<std::uintptr_t>(game::mp::cg.get()) != CG_ARRAY)
			{
				console::error("theater: unexpected cg address, camera features disabled\n");
				return;
			}

			QueryPerformanceFrequency(&qpc_frequency);

			theater_freecam_speed = game::Dvar_RegisterFloat("theater_freecam_speed", 250.0f, 1.0f, 5000.0f, game::DVAR_FLAG_SAVED,
			                                                 "Free camera speed (units per second)");
			theater_mouse_scale = game::Dvar_RegisterFloat("theater_mouse_scale", 1.0f, 0.05f, 10.0f, game::DVAR_FLAG_SAVED,
			                                               "Free camera mouse sensitivity multiplier");
			theater_hud = game::Dvar_RegisterBool("theater_hud", true, game::DVAR_FLAG_NONE, "Show the theater controls during demo playback");
			theater_film = game::Dvar_RegisterBool("theater_film", false, game::DVAR_FLAG_READ,
			                                       "1 while a film (demo) plays: the Start menu then has THEATER SETTINGS");
			theater_video_fps = game::Dvar_RegisterInt("theater_video_fps", 60, 10, 240, game::DVAR_FLAG_SAVED,
			                                           "Frame rate of the videos rendered from clips (Videos\\IW6X Theater)");
			theater_video_on_save = game::Dvar_RegisterBool("theater_video_on_save", true, game::DVAR_FLAG_SAVED,
			                                                "Render the video when a clip is saved (Keep Clip)");

			cg_calc_view_values_hook.create(CG_CALC_VIEW_VALUES, calc_view_values_stub);
			r_set_lod_origin_hook.create(R_SET_LOD_ORIGIN, set_lod_origin_stub);
			stats_write_needed_hook.create(LIVESTORAGE_STATS_WRITE_NEEDED, stats_write_needed_stub);
			snd_update_master_volume_hook.create(SND_UPDATE_MASTER_VOLUME, snd_update_master_volume_stub);
			cl_key_event_hook.create(game::CL_KeyEvent.get(), cl_key_event_stub);
			cl_mouse_event_hook.create(game::CL_MouseEvent.get(), cl_mouse_event_stub);
			seh_string_ed_get_string_hook.create(game::SEH_StringEd_GetString.get(), seh_string_ed_get_string_stub);

			// game sound pauses with the film (engine sound pause, see snd_cl_paused_stub)
			install_film_sound_pause();

			add_commands();

			scheduler::loop(on_frame, scheduler::pipeline::main);
			scheduler::loop([]()
			{
				// hide the theater HUD while a menu is open (the console is fine) and while a screenshot is taken;
				// popups and the Edit Camera Mode markers stay visible when the controls are toggled off
				const auto menu_open = (*game::keyCatchers & ~1) != 0;
				if (active() && !menu_open && !hide_theater_hud())
				{
					theater_hud::draw();
				}
			}, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(theater::component)
