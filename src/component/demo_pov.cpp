#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "demo_pov.hpp"
#include "demo_pov_format.hpp"

#include "command.hpp"
#include "console.hpp"
#include "demo_playback.hpp"
#include "demo_recording.hpp"
#include "demo_utils.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/string.hpp"
#include "utils/thread.hpp"

#include <bitset>
#include <charconv>
#include <condition_variable>

// demo_pov: BO2-style true first-person view of any player during demo playback, for client demos recorded
// by the private-match host (listen server in the same process, bots included).
//
// RECORDING (server thread + worker thread)
//   SV_SendClientMessages is detoured. At its entry svs.time is the serverTime every snapshot of this server
//   frame will carry, and G_RunFrame for that time has just finished. While a demo file is being written and
//   this process hosts the match, the authoritative state of every client slot is copied into a frame buffer
//   (playerState, entityState, linked/svFlags, per-client omnvars, score, session/killcam fields, the entity
//   exclusion list SV_WriteSnapshotEntities applies for that client) plus the full entityStates the host's
//   own snapshot leaves out (its body, attachments, entities hidden from it). That is a ~300 KB memcpy plus
//   one pass over the entities (well under 1 ms). A worker thread XORs the frame against the previous one,
//   deflates it and appends it to "<demo>.dm_iw6.pov" next to every demo file being written (user + auto).
//
// PLAYBACK (main thread)
//   CL_GetSnapshot (the only producer of cgame snapshots) is detoured. With a target selected, the snapshot
//   copy handed to cgame gets the target's playerState (with the otherFlags 0x4000 normalization the demo
//   playback applies to the recorder), the entity list the server would have sent to the target and the
//   target's per-client omnvars. cl.snap / cl.snapshots (the delta bases) are never touched. A change of
//   ps.clientNum makes CG_SetNextSnap take the engine's own follow-switch path (cg.snap->ps = new ps, player
//   centity reset, no interpolation / event replay), which is the clean transition used for target changes.
//   Death hold (death camera on): after a death the match may show the target its killcam or let it spectate
//   until it respawns. Those snapshots keep the target's last dead playerState (its own death camera, like BO2)
//   and its killcam / camera scene / session state omnvars, with the world and the rest of its per-client
//   omnvars from the current record, instead of the recorder's view. The spawn count of that death's snapshots
//   is marked so that the respawn always takes CG_SetNextSnap's reset path.

namespace demo_pov
{
	namespace
	{
		using namespace format;

		// ======================================================================================================
		// engine (iw6mp64_ship.exe 3.15). Evidence: Ghidra on the unpacked dump and the struct /
		// address assertions in demo_utils.cpp.
		// ======================================================================================================

		// void SV_SendClientMessages(void). Only caller 0x14047A2E0, which the server thread loop 0x14047AD90
		// calls right after `svs.time += 0x32; G_RunFrame(svs.time)`. It builds the snapshots
		// (0x140485DE0) with serverTime = svs.time (0x140487170 copies svs.time to 0x1474EC288, which
		// SV_WriteSnapshotToClient 0x140487820 writes with MSG_WriteLong).
		// Prologue 48 89 5C 24 08 | 48 89 74 24 10 | 57 | 48 83 EC 70.
		constexpr std::uintptr_t SV_SEND_CLIENT_MESSAGES = 0x140486A50;
		constexpr std::array<std::uint8_t, 14> SV_SEND_CLIENT_MESSAGES_PROLOGUE{
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC};

		// int CL_GetSnapshot(int localClientNum, int snapshotNumber, snapshot_t* snapshot). Callers: only
		// CG_ProcessSnapshots 0x14028C090 (0x14028C141, 0x14028C213; both `TEST EAX,EAX`).
		// Prologue 40 53 | 41 57 | 48 83 EC 38.
		constexpr std::uintptr_t CL_GET_SNAPSHOT = 0x1402B8EB0;
		constexpr std::array<std::uint8_t, 8> CL_GET_SNAPSHOT_PROLOGUE{0x40, 0x53, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38};

		// sv.state (2 = SS_GAME): SV_ArchiveSnapshot 0x14048756F `CMP dword ptr [0x14749dc00],0x2`;
		// SV_BuildClientSnapshot 0x1404824A0 zeroes the frame ps when it is not 2.
		constexpr std::uintptr_t SV_STATE = 0x14749DC00;
		constexpr std::int32_t SS_GAME = 2;

		// SV_LocateGameData 0x140475C60 stores (gentities, numGEntities, gameClients, sizeofGameClient) here;
		// SV_GetPlayerstateForClientNum 0x140475A10 = gameClients + n * sizeofGameClient; SV_AddEntitiesToSnapshot
		// 0x140482390 iterates gentities (stride 0x2F0) for n < numGEntities.
		constexpr std::uintptr_t SV_GENTITIES = 0x1474AF338;
		constexpr std::uintptr_t SV_NUM_GENTITIES = 0x1474AF340;
		constexpr std::uintptr_t SV_GAME_CLIENTS = 0x1474AF348;
		constexpr std::uintptr_t SV_SIZEOF_GAME_CLIENT = 0x1474AF350;

		// omnvar counts: global = 0x1404F40D0 `return DAT_147a5fa10`, per client = 0x1404F40C0 `return
		// DAT_147a5fa14`. The snapshot omnvar block is [global | per client] (0x140482800, CL_GetSnapshot).
		constexpr std::uintptr_t OMNVAR_GLOBAL_COUNT = 0x147A5FA10;
		constexpr std::uintptr_t OMNVAR_CLIENT_COUNT = 0x147A5FA14;

		constexpr std::int32_t MAX_GENTITIES = 2048;
		constexpr std::int32_t ENTITYNUM_NONE = 2047;

		// gentity_t (stride 0x2F0, SV_AddEntitiesToSnapshot 0x140482390)
		constexpr std::size_t GENTITY_SIZE = 0x2F0;
		constexpr std::size_t GENT_LINKED = 0x108;   // r.linked (byte), must be != 0 to be sent
		constexpr std::size_t GENT_SVFLAGS = 0x10A;  // r.svFlags (byte), bit 0 = SVF_NOCLIENT
		constexpr std::size_t ES_TYPE = 0x4;         // s.eType (4 = missile, exempt from the attachment rule)
		constexpr std::size_t ES_PARENT = 0xDC;      // s+0xDC: parent link, parent = (v & 0x7FF) - 1 (0x140486770)
		constexpr std::size_t ES_HIDE_MASK = 0x100;  // s+0x100: -1 = hidden from all, bit n = hidden from client n
		constexpr std::int32_t ET_MISSILE = 4;

		// gclient_t (stride 0x3A68, G_GetClientArchiveTime 0x14039EF20 `i*0x3a68 + 0x333c + level.clients`)
		constexpr std::int32_t GCLIENT_SIZE = 0x3A68;
		constexpr std::size_t GC_SESSION_STATE = 0x3328; // ClientEndFrame `CMP [RBX+0x3328],1` -> pm_type 7/8
		constexpr std::size_t GC_IN_KILLCAM = 0x3330;    // SV_SendMessageToClient 0x140486C90 wasKillcam = != 0
		constexpr std::size_t GC_ARCHIVE_TIME = 0x333C;  // G_GetClientArchiveTime 0x14039EF20 (killcam / delayed follow)
		constexpr std::size_t GC_SCORE = 0x34CC;         // ScoreInfo, copied into the snapshot by 0x1404828D0
		constexpr std::size_t GC_OMNVARS = 0x36A8;       // OmnvarData* per-client omnvars (0x140482800)
		constexpr std::size_t GC_SPECTATOR_CLIENT = 0x36B4; // followed client (SpectatorClientEndFrame, informational)

		// playerState_t offsets
		constexpr std::size_t PS_PM_TYPE = 0x4;
		constexpr std::size_t PS_OTHER_FLAGS = 0x10;
		constexpr std::size_t PS_REMOTE_CONTROL_ENT = 0x50;
		constexpr std::size_t PS_VEHICLE_ENTITY = 0xBC;
		constexpr std::size_t PS_CLIENT_NUM = 0x17C;
		// killcam start time: ClientEndFrame zeroes it every frame for playing clients (`MOV [RBX+0x13cc],R14D`
		// at 0x1403828C1); the follow / killcam copy 0x140386550 carries the archived value; cgame keys
		// cg.inKillCam on it (0x14028DBF0).
		constexpr std::size_t PS_KILLCAM_TIME = 0x13CC;

		static_assert(sizeof(game::mp::playerState_t) == PS_SIZE);
		static_assert(offsetof(game::mp::playerState_t, pm_type) == PS_PM_TYPE);
		static_assert(offsetof(game::mp::playerState_t, otherFlags) == PS_OTHER_FLAGS);
		static_assert(offsetof(game::mp::playerState_t, remoteControlEnt) == PS_REMOTE_CONTROL_ENT);
		static_assert(offsetof(game::mp::playerState_t, vehicleState) == PS_VEHICLE_ENTITY);
		static_assert(offsetof(game::mp::playerState_t, clientNum) == PS_CLIENT_NUM);
		static_assert(sizeof(game::mp::EntityState) == ES_SIZE);
		static_assert(sizeof(game::mp::OmnvarData) == OMNVAR_SIZE);
		static_assert(sizeof(game::mp::ScoreInfo) == SCORE_SIZE);
		static_assert(offsetof(game::mp::ScoreInfo, kills) == SCORE_KILLS && offsetof(game::mp::ScoreInfo, deaths) == SCORE_DEATHS);

		// playerState otherFlags. ClientEndFrame (0x140382999..0x1403829A5) does `otherFlags = (otherFlags &
		// 0xfffc7fff) | 0x4000` for every client that is not spectating (sessionState playing or dead);
		// SpectatorClientEndFrame 0x140386170 clears 0x4000 and the follow / killcam copy 0x140386550 clears
		// 0x4000 and sets 0x2000. cgame treats (otherFlags & 0x6000) as "this ps has a body in the world".
		constexpr std::uint32_t OTHERFLAG_FOLLOWING = 0x2000;
		constexpr std::uint32_t OTHERFLAG_PLAYING = 0x4000;

		// pm_type values (BG_PlayerStateToEntityState sets EF_DEAD iff pm_type >= 7; ClientEndFrame sets 7/8 when
		// sessionState == 1)
		constexpr std::int32_t PM_SPECTATOR = 5;
		constexpr std::int32_t PM_INTERMISSION = 6;
		constexpr std::int32_t PM_DEAD = 7;
		constexpr std::int32_t PM_DEAD_LINKED = 8;

		// the view selection in demo_pov_format.hpp reads these without the game headers
		static_assert(PS_FIELD_PM_TYPE == PS_PM_TYPE && PS_FIELD_OTHER_FLAGS == PS_OTHER_FLAGS && PS_FIELD_CLIENT_NUM == PS_CLIENT_NUM
			&& PS_FIELD_KILLCAM_TIME == PS_KILLCAM_TIME);
		static_assert(OTHER_FLAGS_FOLLOWING == OTHERFLAG_FOLLOWING && PM_TYPE_INTERMISSION == PM_INTERMISSION);
		static_assert(SLOT_ACTIVE == game::CS_ACTIVE);

		// corpses. CG_OffsetThirdPersonView 0x14029B7B0 pivots the dead camera (pm_type >= 7) on the j_mainroot of
		// centity 0x2A + ps.corpseIndex when `(unsigned)corpseIndex <= 7` and that centity is in the newest snapshot,
		// else on ps.origin + viewheight (0x14029B83D `CMP EAX,7; JA`). CG_AddCEntity 0x140265D00
		// draws eType 2 with CG_Corpse 0x14027D300, which indexes its corpse info with es.number - 0x2A and
		// takes the owner client from es+0x90 (cent+0x188 = nextState+0x90)
		constexpr std::size_t PS_CORPSE_INDEX = 0xB8;
		constexpr std::int32_t CORPSE_FIRST_ENTITY = 0x2A;
		constexpr std::int32_t MAX_CORPSES = 8;
		constexpr std::size_t ES_CLIENT_NUM = 0x90;
		constexpr std::int32_t ET_PLAYER_CORPSE = 2;

		static_assert(offsetof(game::mp::playerState_t, corpseIndex) == PS_CORPSE_INDEX);
		static_assert(offsetof(game::mp::EntityState, eType) == ES_TYPE);
		static_assert(offsetof(game::mp::EntityState, clientNum) == ES_CLIENT_NUM);

		// snapshot_t as filled by CL_GetSnapshot 0x1402B8EB0: ps +0, omnvars +0x3474, snapFlags +0x3C74,
		// serverTime +0x3C7C, numEntities +0x3C80 (clamped to 0x300), numClients +0x3C84, entities +0x3C88
		// (0x108 each), clients +0x35488 (clientState_t 0xD8: clientIndex +0, name[16] +0x40).
		constexpr std::size_t SNAP_OMNVARS = 0x3474;
		constexpr std::size_t SNAP_SERVER_TIME = 0x3C7C;
		constexpr std::size_t SNAP_NUM_ENTITIES = 0x3C80;
		constexpr std::size_t SNAP_NUM_CLIENTS = 0x3C84;
		constexpr std::size_t SNAP_ENTITIES = 0x3C88;
		constexpr std::size_t SNAP_CLIENTS = 0x35488;
		constexpr std::size_t CLIENT_STATE_SIZE = 0xD8;
		constexpr std::size_t CLIENT_STATE_NAME = 0x40;
		constexpr std::int32_t MAX_SNAP_ENTITIES = 0x300;

		static_assert(offsetof(game::mp::snapshot_t, omnvars) == SNAP_OMNVARS);
		static_assert(offsetof(game::mp::snapshot_t, serverTime) == SNAP_SERVER_TIME);
		static_assert(offsetof(game::mp::snapshot_t, numEntities) == SNAP_NUM_ENTITIES);
		static_assert(offsetof(game::mp::snapshot_t, numClients) == SNAP_NUM_CLIENTS);
		static_assert(sizeof(game::mp::clientState_t) == CLIENT_STATE_SIZE);
		static_assert(offsetof(game::mp::clientState_t, name) == CLIENT_STATE_NAME);
		static_assert(SNAP_ENTITIES + MAX_SNAP_ENTITIES * ES_SIZE == SNAP_CLIENTS); // entities[768] end where clients[] start

		// svs.clients[] (client_t, stride 0x83570): header.state +0, netchan.remoteAddress.type +0x28
		// (SV_BuildClientSnapshots 0x140485DE0 tests `(&DAT_14647b2b8)[i*0x20d5c] == 2` = NA_LOOPBACK),
		// gentity +0x41E68, testClient +0x41ECC (SV_BotIsBot 0x140461340 `== 2`).
		static_assert(offsetof(game::mp::client_t, header.netchan.remoteAddress.type) == 0x28);
		static_assert(offsetof(game::mp::client_t, gentity) == 0x41E68);
		static_assert(offsetof(game::mp::client_t, testClient) == 0x41ECC);

		constexpr std::int32_t MAX_SHARED_ENTITIES = 0x3FF; // 0x140482390 stops when its count reaches 0x400
		constexpr std::int32_t MAX_FRAME_AGE = 100;        // ms; a snapshot without an exact frame uses an older one
		constexpr int ZLIB_LEVEL = 3;

		template <typename T>
		T read_global(const std::uintptr_t address)
		{
			return read_as<T>(reinterpret_cast<const void*>(address));
		}

		void log_main(std::string message)
		{
			// console output is only safe from the main thread
			scheduler::once([message = std::move(message)]()
			{
				console::info("%s", message.data());
			}, scheduler::main);
		}

		bool prologue_ok(const std::uintptr_t address, const std::span<const std::uint8_t> expected)
		{
			if (!utils::hook::is_readable(reinterpret_cast<const void*>(address), expected.size()))
			{
				return false;
			}

			const auto* code = reinterpret_cast<const std::uint8_t*>(address);

			// another module may already have a detour here; MinHook chains on top of a leading JMP rel32
			return code[0] == 0xE9 || std::equal(expected.begin(), expected.end(), code);
		}

		// shared state between the main thread (controller), the server thread (capture) and the worker
		std::atomic_bool capture_enabled{false};
		std::atomic_bool playback_active{false};
		std::atomic_int target_client{-1};
		std::atomic_bool show_death{false};
		std::atomic_bool latest_substituted{false};
		std::atomic_bool latest_held{false}; // the latest snapshot got a death hold (also counts as substituted)
		std::atomic_int64_t latest_held_record{-1}; // its held record, -1 when not a death hold

		// ======================================================================================================
		// recording: worker thread (compression + file I/O)
		// ======================================================================================================

		struct frame_buffer
		{
			std::vector<std::uint8_t> data;
			std::size_t size{};
			frame_info info{};
		};

		class recorder_t
		{
		public:
			struct stats_t
			{
				std::uint32_t open_files{};
				std::uint64_t frames{};
				std::uint64_t bytes{};
				std::uint64_t dropped{};
			};

			void start()
			{
				std::lock_guard _(mutex_);
				if (started_)
				{
					return;
				}

				started_ = true;
				std::thread([this]()
				{
					utils::thread::set_name(L"theater pov writer");
					this->run();
				}).detach();
			}

			// ---- main thread

			void open(const std::filesystem::path& demo_path, std::string mapname, std::string gametype)
			{
				work_item item{};
				item.type = work_item::kind::open;
				item.demo_path = demo_path;
				item.mapname = std::move(mapname);
				item.gametype = std::move(gametype);
				push(std::move(item));
			}

			void close(const std::filesystem::path& demo_path)
			{
				work_item item{};
				item.type = work_item::kind::close;
				item.demo_path = demo_path;
				push(std::move(item));
			}

			[[nodiscard]] stats_t stats() const
			{
				return {
					open_files_.load(),
					frames_written_.load(),
					bytes_written_.load(),
					frames_dropped_.load(),
				};
			}

			// ---- server thread

			[[nodiscard]] std::unique_ptr<frame_buffer> acquire()
			{
				{
					std::lock_guard _(mutex_);
					if (!pool_.empty())
					{
						auto buffer = std::move(pool_.back());
						pool_.pop_back();
						return buffer;
					}

					if (allocated_ >= MAX_BUFFERS)
					{
						++frames_dropped_;
						return {};
					}

					++allocated_;
				}

				auto buffer = std::make_unique<frame_buffer>();
				buffer->data.resize(MAX_FRAME_SIZE);
				return buffer;
			}

			void submit(std::unique_ptr<frame_buffer> frame)
			{
				work_item item{};
				item.type = work_item::kind::frame;
				item.frame = std::move(frame);
				push(std::move(item));
			}

			void recycle(std::unique_ptr<frame_buffer> frame)
			{
				if (frame)
				{
					std::lock_guard _(mutex_);
					pool_.emplace_back(std::move(frame));
				}
			}

		private:
			static constexpr std::size_t MAX_BUFFERS = 32; // ~10 MB, absorbs ~1.5 s of disk stalls

			struct work_item
			{
				enum class kind
				{
					open,
					close,
					frame,
				} type{};

				std::filesystem::path demo_path{};
				std::string mapname{};
				std::string gametype{};
				std::unique_ptr<frame_buffer> frame{};
			};

			struct writer
			{
				std::filesystem::path demo_path{};
				std::filesystem::path file_path{};
				std::string mapname{};
				std::string gametype{};
				std::ofstream file{};
				bool created{};
				bool failed{};
				std::uint64_t frames{};
				std::uint64_t bytes{};
				std::chrono::steady_clock::time_point last_flush{};
			};

			void push(work_item&& item)
			{
				{
					std::lock_guard _(mutex_);
					queue_.emplace_back(std::move(item));
				}
				cv_.notify_one();
			}

			void run()
			{
				encoder_ = std::make_unique<encoder>(ZLIB_LEVEL);
				if (!encoder_->ready())
				{
					log_main("^1pov: zlib deflateInit failed, sidecars disabled\n");
				}

				while (true)
				{
					work_item item{};
					{
						std::unique_lock lock(mutex_);
						cv_.wait(lock, [this]() { return !queue_.empty(); });
						item = std::move(queue_.front());
						queue_.pop_front();
					}

					try
					{
						switch (item.type)
						{
						case work_item::kind::open:
							handle_open(item);
							break;
						case work_item::kind::close:
							handle_close(item.demo_path);
							break;
						case work_item::kind::frame:
							if (item.frame)
							{
								handle_frame(*item.frame);
							}
							break;
						}
					}
					catch (const std::exception& e)
					{
						log_main(std::format("^1pov: sidecar writer error: {}\n", e.what()));
					}

					recycle(std::move(item.frame));
				}
			}

			void handle_open(const work_item& item)
			{
				for (const auto& w : writers_)
				{
					if (w.demo_path == item.demo_path)
					{
						return;
					}
				}

				writer w{};
				w.demo_path = item.demo_path;
				w.file_path = sidecar_path(item.demo_path);
				w.mapname = item.mapname;
				w.gametype = item.gametype;

				// the demo file is being (re)created right now: a sidecar left from an older demo with the same
				// name must not be paired with it. The new sidecar is only created once a frame is captured,
				// i.e. only when this process really hosts the match.
				std::error_code ec;
				std::filesystem::remove(w.file_path, ec);

				writers_.emplace_back(std::move(w));
			}

			void finish(writer& w)
			{
				if (w.created && w.file.is_open())
				{
					w.file.flush();
					w.file.close();
					--open_files_;

					log_main(std::format("pov: sidecar closed {} ({} frames, {:.1f} MB)\n",
					                     w.file_path.filename().string(), w.frames, static_cast<double>(w.bytes) / (1024.0 * 1024.0)));
				}
			}

			void handle_close(const std::filesystem::path& demo_path)
			{
				for (auto it = writers_.begin(); it != writers_.end(); ++it)
				{
					if (it->demo_path == demo_path)
					{
						finish(*it);
						writers_.erase(it);
						break;
					}
				}

				if (writers_.empty() && encoder_)
				{
					encoder_->reset();
				}
			}

			bool create_file(writer& w, const frame_buffer& frame)
			{
				w.created = true;
				w.file.open(w.file_path, std::ios::binary | std::ios::trunc);
				if (!w.file.is_open() || !w.file.good())
				{
					w.failed = true;
					log_main(std::format("^1pov: could not create sidecar {}\n", w.file_path.string()));
					return false;
				}

				const auto header = make_file_header(game::VERSION_MARKER_VALUE, frame.info.server_time, frame.info.host_client,
				                                     w.mapname, w.gametype, w.demo_path.filename().string());
				w.file.write(reinterpret_cast<const char*>(&header), sizeof(header));
				if (!w.file.good())
				{
					w.failed = true;
					w.file.close();
					log_main(std::format("^1pov: could not write sidecar {}\n", w.file_path.string()));
					return false;
				}

				w.last_flush = std::chrono::steady_clock::now();
				++open_files_;

				log_main(std::format("pov: recording sidecar {} (host client {})\n", w.file_path.filename().string(), frame.info.host_client));
				return true;
			}

			void handle_frame(const frame_buffer& frame)
			{
				if (writers_.empty() || !encoder_ || !encoder_->ready())
				{
					return;
				}

				// a new sidecar must start with a keyframe; all sidecars share the same records
				auto force_key = false;
				for (const auto& w : writers_)
				{
					force_key |= !w.created;
				}

				record_header record{};
				std::span<const std::uint8_t> payload{};
				if (!encoder_->encode(frame.data.data(), frame.size, frame.info, force_key, record, payload))
				{
					return;
				}

				const auto now = std::chrono::steady_clock::now();
				for (auto& w : writers_)
				{
					if (w.failed)
					{
						continue;
					}

					if (!w.created && !create_file(w, frame))
					{
						continue;
					}

					w.file.write(reinterpret_cast<const char*>(&record), sizeof(record));
					w.file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

					if (!w.file.good())
					{
						w.failed = true;
						w.file.close();
						--open_files_;
						log_main(std::format("^1pov: write error, sidecar {} stopped\n", w.file_path.string()));
						continue;
					}

					const auto bytes = sizeof(record) + payload.size();
					++w.frames;
					w.bytes += bytes;
					++frames_written_;
					bytes_written_ += bytes;

					// records are self-delimiting: a sidecar cut short by a crash stays readable up to the last
					// flushed record
					if (now - w.last_flush > 2s)
					{
						w.file.flush();
						w.last_flush = now;
					}
				}
			}

			mutable std::mutex mutex_;
			std::condition_variable cv_;
			std::deque<work_item> queue_;
			std::vector<std::unique_ptr<frame_buffer>> pool_;
			std::size_t allocated_{};
			bool started_{};

			std::atomic_uint32_t open_files_{0};
			std::atomic_uint64_t frames_written_{0};
			std::atomic_uint64_t bytes_written_{0};
			std::atomic_uint64_t frames_dropped_{0};

			// worker-only state
			std::vector<writer> writers_;
			std::unique_ptr<encoder> encoder_;
		};

		recorder_t recorder;

		// ======================================================================================================
		// recording: capture (server thread)
		// ======================================================================================================

		// remembers the last memory range validated with VirtualQuery so the check runs only when a pointer changes
		struct readable_cache
		{
			const void* address{};
			std::size_t size{};

			bool check(const void* pointer, const std::size_t bytes)
			{
				if (pointer == address && bytes <= size)
				{
					return true;
				}

				if (!pointer || !utils::hook::is_readable(pointer, bytes))
				{
					return false;
				}

				address = pointer;
				size = bytes;
				return true;
			}
		};

		struct capture_scratch
		{
			std::array<std::int16_t, MAX_GENTITIES> shared_index{};      // entity number -> index in shared[]
			std::array<const std::uint8_t*, MAX_SHARED_ENTITIES> shared{}; // gentities that pass the snapshot predicate
			std::array<std::uint32_t, MAX_SHARED_ENTITIES> chain_mask{};   // clients in each entity's parent chain
			readable_cache gentities{};
			readable_cache gclients{};
			std::array<readable_cache, MAX_CLIENTS> omnvars{};
		};

		capture_scratch scratch; // used by the server thread only

		// SV_WriteSnapshotEntities 0x140486250 does not send an entity to client c when
		//   es.number == c || (es+0x100 >> (c & 31)) & 1 || (attached to c through the es+0xDC chain && eType != 4)
		// The chain walk 0x140486770 compares each parent number with c before looking the parent up in the
		// frame's entity list, and stops when the parent is not in the list.
		std::uint32_t parent_chain_mask(const std::uint8_t* es)
		{
			std::uint32_t mask{};
			const auto* current = es;

			for (auto depth = 0; depth < 32 && current; ++depth)
			{
				const auto link = read_as<std::uint32_t>(current, ES_PARENT) & 0x7FF;
				if (link == 0)
				{
					break;
				}

				const auto parent = static_cast<std::int32_t>(link) - 1;
				if (parent < static_cast<std::int32_t>(MAX_CLIENTS))
				{
					mask |= 1u << parent;
				}

				const auto index = scratch.shared_index[static_cast<std::size_t>(parent)];
				current = index >= 0 ? scratch.shared[static_cast<std::size_t>(index)] : nullptr;
			}

			return mask;
		}

		bool excluded_for(const std::uint8_t* es, const std::uint32_t chain_mask, const std::int32_t client)
		{
			if (read_as<std::int32_t>(es) == client)
			{
				return true;
			}

			if ((read_as<std::uint32_t>(es, ES_HIDE_MASK) >> (client & 31)) & 1)
			{
				return true;
			}

			return ((chain_mask >> client) & 1) && read_as<std::int32_t>(es, ES_TYPE) != ET_MISSILE;
		}

		void capture_server_frame()
		{
			if (!capture_enabled.load(std::memory_order_acquire) || playback_active.load(std::memory_order_relaxed))
			{
				return;
			}

			if (read_global<std::int32_t>(SV_STATE) != SS_GAME)
			{
				return;
			}

			auto& svs = *game::mp::svs;
			const auto client_count = std::min<std::int32_t>(svs.clientCount, static_cast<std::int32_t>(MAX_CLIENTS));
			if (client_count <= 0)
			{
				return;
			}

			// the listen-server host is the client connected through the loopback address
			std::int32_t host = -1;
			for (std::int32_t i = 0; i < client_count; ++i)
			{
				const auto& cl = svs.clients[i];
				if (cl.header.state >= game::CS_CONNECTED && cl.header.netchan.remoteAddress.type == game::NA_LOOPBACK)
				{
					host = i;
					break;
				}
			}

			if (host < 0)
			{
				return; // not hosting: never write anything
			}

			const auto* gentities = read_global<const std::uint8_t*>(SV_GENTITIES);
			const auto num_gentities = read_global<std::int32_t>(SV_NUM_GENTITIES);
			const auto* gclients = read_global<const std::uint8_t*>(SV_GAME_CLIENTS);
			const auto gclient_size = read_global<std::int32_t>(SV_SIZEOF_GAME_CLIENT);
			const auto omnvar_global = read_global<std::int32_t>(OMNVAR_GLOBAL_COUNT);
			const auto omnvar_client = read_global<std::int32_t>(OMNVAR_CLIENT_COUNT);

			if (!gentities || !gclients || gclient_size != GCLIENT_SIZE || num_gentities < client_count || num_gentities > MAX_GENTITIES
				|| omnvar_global < 0 || omnvar_client < 0 || omnvar_global + omnvar_client > static_cast<std::int32_t>(MAX_OMNVARS))
			{
				return;
			}

			if (!scratch.gentities.check(gentities, static_cast<std::size_t>(num_gentities) * GENTITY_SIZE)
				|| !scratch.gclients.check(gclients, static_cast<std::size_t>(client_count) * GCLIENT_SIZE))
			{
				return;
			}

			auto frame = recorder.acquire();
			if (!frame)
			{
				return; // worker is behind; the frame is dropped (the reader falls back to the previous one)
			}

			const auto count = static_cast<std::uint32_t>(client_count);
			const auto omnvars = static_cast<std::uint32_t>(omnvar_client);
			auto* data = frame->data.data();
			auto& info = frame->info;

			frame->size = frame_size(count, omnvars);
			info = {};
			info.server_time = svs.time;
			info.client_count = count;
			info.host_client = host;
			info.omnvar_global = static_cast<std::uint32_t>(omnvar_global);
			info.omnvar_client = omnvars;
			info.server_bit = static_cast<std::uint32_t>(svs.snapFlagServerBit);
			std::memset(data, 0, frame->size);

			// shared entity list, same predicate and cap as SV_AddEntitiesToSnapshot 0x140482390:
			// r.linked && !(r.svFlags & SVF_NOCLIENT) && hide mask != -1
			scratch.shared_index.fill(-1);
			std::int32_t shared_count = 0;
			for (std::int32_t i = 0; i < num_gentities; ++i)
			{
				const auto* ent = gentities + static_cast<std::size_t>(i) * GENTITY_SIZE;
				if (!ent[GENT_LINKED] || (ent[GENT_SVFLAGS] & 1) || read_as<std::int32_t>(ent, ES_HIDE_MASK) == -1)
				{
					continue;
				}

				if (shared_count >= MAX_SHARED_ENTITIES)
				{
					break;
				}

				const auto number = read_as<std::int32_t>(ent);
				if (number < 0 || number >= MAX_GENTITIES)
				{
					continue;
				}

				scratch.shared[static_cast<std::size_t>(shared_count)] = ent;
				scratch.shared_index[static_cast<std::size_t>(number)] = static_cast<std::int16_t>(shared_count);
				++shared_count;
			}

			for (std::int32_t k = 0; k < shared_count; ++k)
			{
				scratch.chain_mask[static_cast<std::size_t>(k)] = parent_chain_mask(scratch.shared[static_cast<std::size_t>(k)]);
			}

			// client blocks
			const auto block_size = client_block_size(omnvars);
			for (std::int32_t c = 0; c < client_count; ++c)
			{
				auto* block = data + sizeof(frame_header) + static_cast<std::size_t>(c) * block_size;
				const auto& cl = svs.clients[c];

				client_header ch{};
				ch.slot_state = static_cast<std::uint8_t>(std::clamp(static_cast<std::int32_t>(cl.header.state), 0, 255));

				// SV_BuildClientSnapshot only uses the gclient ps when the client has a gentity and is not a zombie
				if (cl.header.state < game::CS_CONNECTED || !cl.gentity)
				{
					write_as(block, 0, ch);
					continue;
				}

				const auto bit = 1u << c;
				const auto* gclient = gclients + static_cast<std::size_t>(c) * GCLIENT_SIZE;
				const auto* ent = gentities + static_cast<std::size_t>(c) * GENTITY_SIZE;
				auto* ps = block + sizeof(client_header);
				auto* es = ps + PS_SIZE;
				auto* omn = es + ES_SIZE;

				std::memcpy(ps, gclient, PS_SIZE);
				std::memcpy(es, ent, ES_SIZE);
				ch.linked = ent[GENT_LINKED];
				ch.sv_flags = ent[GENT_SVFLAGS];
				ch.session_state = read_as<std::int32_t>(gclient, GC_SESSION_STATE);
				ch.in_killcam = read_as<std::int32_t>(gclient, GC_IN_KILLCAM);
				ch.archive_time = read_as<std::int32_t>(gclient, GC_ARCHIVE_TIME);
				ch.spectator_client = read_as<std::int32_t>(gclient, GC_SPECTATOR_CLIENT);
				std::memcpy(ch.score.data(), gclient + GC_SCORE, SCORE_SIZE);

				if (omnvars)
				{
					const auto* src = read_as<const std::uint8_t*>(gclient, GC_OMNVARS);
					const auto bytes = static_cast<std::size_t>(omnvars) * OMNVAR_SIZE;
					if (scratch.omnvars[static_cast<std::size_t>(c)].check(src, bytes))
					{
						std::memcpy(omn, src, bytes);
					}
				}

				// entities SV_WriteSnapshotEntities would leave out of this client's snapshot
				for (std::int32_t k = 0; k < shared_count && ch.excluded_count < MAX_EXCLUDED; ++k)
				{
					const auto* entity = scratch.shared[static_cast<std::size_t>(k)];
					if (excluded_for(entity, scratch.chain_mask[static_cast<std::size_t>(k)], c))
					{
						ch.excluded[ch.excluded_count++] = static_cast<std::uint16_t>(read_as<std::int32_t>(entity));
					}
				}

				// life state, from the fields ClientEndFrame / SpectatorClientEndFrame maintain every frame:
				// playing clients (sessionState playing or dead) get otherFlags 0x4000 and ps+0x13CC = 0, dead ones
				// pm_type 7/8; spectators and killcam viewers have a follow copy (0x2000 set, 0x4000 clear)
				const auto other_flags = read_as<std::uint32_t>(ps, PS_OTHER_FLAGS);
				const auto pm_type = read_as<std::int32_t>(ps, PS_PM_TYPE);
				const auto following = (other_flags & OTHERFLAG_FOLLOWING) != 0;
				const auto playing = (other_flags & OTHERFLAG_PLAYING) != 0 && !following;
				const auto killcam = read_as<std::int32_t>(ps, PS_KILLCAM_TIME) != 0;
				const auto dead = playing && (pm_type == PM_DEAD || pm_type == PM_DEAD_LINKED);
				const auto alive = playing && !dead && !killcam && pm_type != PM_SPECTATOR && pm_type != PM_INTERMISSION
					&& cl.header.state == game::CS_ACTIVE;

				ch.flags = static_cast<std::uint8_t>(CF_CONNECTED
					| (playing ? CF_PLAYING : 0)
					| (alive ? CF_ALIVE : 0)
					| (dead ? CF_DEAD : 0)
					| (following ? CF_FOLLOWING : 0)
					| (killcam ? CF_KILLCAM : 0)
					| (c == host ? CF_HOST : 0)
					| (static_cast<std::int32_t>(cl.testClient) == game::TC_BOT ? CF_BOT : 0));

				write_as(block, 0, ch);

				info.summary.connected_mask |= bit;
				info.summary.alive_mask |= alive ? bit : 0;
				info.summary.dead_mask |= dead ? bit : 0;
				info.summary.kills[static_cast<std::size_t>(c)] = read_as<std::uint16_t>(ch.score.data(), SCORE_KILLS);
				info.summary.deaths[static_cast<std::size_t>(c)] = read_as<std::uint16_t>(ch.score.data(), SCORE_DEATHS);
			}

			// full entityStates the host's own snapshot does not contain (its body, attachments, entities hidden
			// from it); the target's snapshot needs them
			std::uint32_t host_excluded = 0;
			auto* host_excluded_base = data + sizeof(frame_header) + count * block_size;
			for (std::int32_t k = 0; k < shared_count && host_excluded < MAX_HOST_EXCLUDED; ++k)
			{
				const auto* entity = scratch.shared[static_cast<std::size_t>(k)];
				if (excluded_for(entity, scratch.chain_mask[static_cast<std::size_t>(k)], host))
				{
					std::memcpy(host_excluded_base + host_excluded * ES_SIZE, entity, ES_SIZE);
					++host_excluded;
				}
			}

			frame_header header{};
			header.magic = FRAME_MAGIC;
			header.server_time = info.server_time;
			header.host_client = host;
			header.snap_flag_server_bit = info.server_bit;
			header.client_count = count;
			header.omnvar_global = info.omnvar_global;
			header.omnvar_client = omnvars;
			header.shared_entities = static_cast<std::uint32_t>(shared_count);
			header.host_excluded_count = host_excluded;
			write_as(data, 0, header);

			recorder.submit(std::move(frame));
		}

		utils::hook::detour sv_send_client_messages_hook;

		void sv_send_client_messages_stub()
		{
			try
			{
				capture_server_frame();
			}
			catch (...)
			{
				// never let an allocation failure escape into the server frame
			}

			sv_send_client_messages_hook.invoke<void>();
		}

		// ======================================================================================================
		// recording: controller (main thread)
		// ======================================================================================================

		std::vector<std::filesystem::path> active_recordings;

		// Writers are armed for every demo file being written; whether this process hosts the match is decided
		// per frame on the server thread (sv.state == SS_GAME and a loopback client in svs.clients). A sidecar
		// file is only created when the first frame is captured, so a client of a remote server writes nothing.
		game::dvar_t* theater_pov_record{};

		void update_recording(const bool playing)
		{
			std::vector<std::filesystem::path> wanted;
			const auto enabled = !theater_pov_record || theater_pov_record->current.enabled;
			if (!playing && enabled && !game::environment::is_dedi())
			{
				for (auto path : {demo_recording::get_user_path(), demo_recording::get_auto_path()})
				{
					if (!path.empty() && std::ranges::find(wanted, path) == wanted.end())
					{
						wanted.emplace_back(std::move(path));
					}
				}
			}

			if (wanted == active_recordings)
			{
				return;
			}

			for (const auto& path : wanted)
			{
				if (std::ranges::find(active_recordings, path) == active_recordings.end())
				{
					recorder.open(path, demo_utils::get_mapname_lowercase(false), demo_utils::get_gametype_lowercase(false));
				}
			}

			for (const auto& path : active_recordings)
			{
				if (std::ranges::find(wanted, path) == wanted.end())
				{
					recorder.close(path);
				}
			}

			active_recordings = std::move(wanted);
			capture_enabled.store(!active_recordings.empty(), std::memory_order_release);
		}

		// ======================================================================================================
		// playback: sidecar loading (main thread)
		// ======================================================================================================

		std::mutex reader_mutex; // the API may be used from the renderer pipeline as well
		reader sidecar;
		std::filesystem::path sidecar_demo_path; // demo the reader state belongs to (also set when loading failed)
		view_selector selector;                  // which record the view shows; keeps the held records (death hold)

		// death hold: the held body in the outgoing snapshot (guard_held_corpse), for pov_info
		struct corpse_check
		{
			std::int32_t index{-1}; // held ps.corpseIndex
			bool found{};           // entity 0x2A + index is in the outgoing snapshot
			std::int32_t type{-1};  // its eType / owner client
			std::int32_t owner{-1};
			bool cleared{};         // corpseIndex cleared: that entity is not the target's body
		};

		// what the latest substituted snapshot showed, for pov_info
		struct shown_view
		{
			corpse_check corpse{}; // held only
			std::int32_t target{-1};
			std::int32_t time{};      // server time of the current record (the world)
			bool dead{};              // live: the target's own dead record (death camera)
			std::int32_t died_at{};   // held: server time of the first dead record
			std::int32_t held_at{};   // held: server time of the shown record
			std::int32_t until{-1};   // held: server time of the record that ends the hold, -1 = end of the sidecar
			std::uint32_t held_record{};
			death_period::end_reason reason{};
		};

		shown_view latest_view{}; // guarded by reader_mutex

		bool load_sidecar(const std::filesystem::path& demo_path, std::string& error)
		{
			if (!sidecar.load(sidecar_path(demo_path), game::VERSION_MARKER_VALUE, error))
			{
				return false;
			}

			// the sidecar must belong to this demo: same map / game type and an overlapping time range
			const auto identity = read_demo_identity(demo_path);
			const auto& header = sidecar.header();
			const auto mapname = fixed_string(header.mapname.data(), header.mapname.size());
			const auto gametype = fixed_string(header.gametype.data(), header.gametype.size());
			if (!identity || utils::string::to_lower(identity->mapname) != mapname || utils::string::to_lower(identity->gametype) != gametype)
			{
				error = std::format("sidecar is for {} / {}, the demo is {} / {}", mapname, gametype,
				                    identity ? identity->mapname : "?", identity ? identity->gametype : "?");
				sidecar.unload();
				return false;
			}

			const auto& index = sidecar.index();
			if (identity->last_time > identity->first_time
				&& (index.back().time < identity->first_time - 5000 || index.front().time > identity->last_time + 5000))
			{
				error = "the sidecar's time range does not overlap the demo";
				sidecar.unload();
				return false;
			}

			return true;
		}

		void update_playback(const bool playing)
		{
			const auto path = playing ? demo_playback::get_path() : std::filesystem::path{};

			std::lock_guard _(reader_mutex);
			if (path == sidecar_demo_path)
			{
				return;
			}

			const auto was_loaded = sidecar.loaded();
			sidecar.unload();
			sidecar_demo_path = path;
			target_client.store(-1);
			latest_substituted.store(false);
			latest_held.store(false);
			latest_held_record.store(-1);
			latest_view = {};
			selector.reset();

			if (path.empty())
			{
				if (was_loaded)
				{
					console::info("pov: sidecar unloaded\n");
				}
				return;
			}

			std::string error;
			if (load_sidecar(path, error))
			{
				const auto& index = sidecar.index();
				console::info("pov: sidecar loaded %s (%zu frames, %zu keyframes, %.1f MB, recorder client %d, %zu life events) - use 'pov <client>'\n",
				              sidecar.path().filename().string().data(), index.size(), sidecar.keyframes(),
				              static_cast<double>(sidecar.file_size()) / (1024.0 * 1024.0), index.back().host_client, sidecar.events().size());
			}
			else if (error != "no sidecar")
			{
				console::warn("pov: sidecar for %s ignored: %s\n", path.filename().string().data(), error.data());
			}
		}

		void main_frame()
		{
			const auto playing = demo_playback::playing();
			playback_active.store(playing, std::memory_order_relaxed);

			update_playback(playing);
			update_recording(playing);
		}

		// ======================================================================================================
		// playback: snapshot substitution (main thread)
		// ======================================================================================================

		// entity list as SV_WriteSnapshotEntities would have sent it to the target: drop what is excluded for
		// the target, add what was excluded for the recorder (unless also excluded for the target). Entity `self`
		// (>= 0) is dropped as well: a client never gets its own entity (es.number == client).
		void rebuild_entities(std::uint8_t* snap, const frame_view& frame, const client_view& pov, const std::int32_t self)
		{
			static std::vector<std::uint8_t> out(static_cast<std::size_t>(MAX_SNAP_ENTITIES) * ES_SIZE);

			std::bitset<MAX_GENTITIES> removed{};
			for (std::uint32_t k = 0; k < pov.header.excluded_count; ++k)
			{
				const auto number = pov.header.excluded[k];
				if (number < MAX_GENTITIES)
				{
					removed.set(number);
				}
			}

			if (self >= 0 && self < MAX_GENTITIES)
			{
				removed.set(static_cast<std::size_t>(self));
			}

			std::array<const std::uint8_t*, MAX_HOST_EXCLUDED> inserts{};
			std::size_t insert_count = 0;
			for (std::uint32_t k = 0; k < frame.header().host_excluded_count; ++k)
			{
				const auto* es = frame.host_excluded(k);
				const auto number = es ? read_as<std::int32_t>(es) : -1;
				if (number < 0 || number >= MAX_GENTITIES || removed.test(static_cast<std::size_t>(number)))
				{
					continue;
				}

				inserts[insert_count++] = es;
			}

			std::sort(inserts.begin(), inserts.begin() + static_cast<std::ptrdiff_t>(insert_count), [](const std::uint8_t* a, const std::uint8_t* b)
			{
				return read_as<std::int32_t>(a) < read_as<std::int32_t>(b);
			});

			const auto count = std::clamp(read_as<std::int32_t>(snap, SNAP_NUM_ENTITIES), 0, MAX_SNAP_ENTITIES);
			const auto* entities = snap + SNAP_ENTITIES;

			std::int32_t out_count = 0;
			std::int32_t last_number = -1;
			std::int32_t i = 0;
			std::size_t j = 0;

			auto emit = [&](const std::uint8_t* es)
			{
				const auto number = read_as<std::int32_t>(es);
				if (number < 0 || number >= MAX_GENTITIES || removed.test(static_cast<std::size_t>(number)) || number == last_number
					|| out_count >= MAX_SNAP_ENTITIES)
				{
					return;
				}

				std::memcpy(out.data() + static_cast<std::size_t>(out_count) * ES_SIZE, es, ES_SIZE);
				last_number = number;
				++out_count;
			};

			// merge two lists sorted by entity number (the parser emits the snapshot's in ascending order)
			while (i < count || j < insert_count)
			{
				const auto* a = i < count ? entities + static_cast<std::size_t>(i) * ES_SIZE : nullptr;
				const auto* b = j < insert_count ? inserts[j] : nullptr;

				if (a && (!b || read_as<std::int32_t>(a) <= read_as<std::int32_t>(b)))
				{
					// equal numbers: keep the demo's version
					if (b && read_as<std::int32_t>(a) == read_as<std::int32_t>(b))
					{
						++j;
					}

					emit(a);
					++i;
				}
				else
				{
					emit(b);
					++j;
				}
			}

			std::memcpy(snap + SNAP_ENTITIES, out.data(), static_cast<std::size_t>(out_count) * ES_SIZE);
			write_as(snap, SNAP_NUM_ENTITIES, out_count);
		}

		// playerState, with the normalization the demo playback applies to the recorder's ps (demo_playback.cpp,
		// get_predicted_player_data): keep 0x4000 only when dead, remote controlling or in a vehicle
		void apply_player_state(std::uint8_t* snap, const std::uint8_t* ps)
		{
			std::memcpy(snap, ps, PS_SIZE);

			auto other_flags = read_as<std::uint32_t>(snap, PS_OTHER_FLAGS);
			const auto pm_type = read_as<std::int32_t>(snap, PS_PM_TYPE);
			const auto remote = read_as<std::int32_t>(snap, PS_REMOTE_CONTROL_ENT);
			const auto vehicle = read_as<std::int32_t>(snap, PS_VEHICLE_ENTITY);
			other_flags = (pm_type == PM_DEAD || remote != ENTITYNUM_NONE || vehicle != ENTITYNUM_NONE)
				              ? other_flags | OTHERFLAG_PLAYING
				              : other_flags & ~OTHERFLAG_PLAYING;
			write_as(snap, PS_OTHER_FLAGS, other_flags);
		}

		// per-client omnvars: snapshot block [global | per client]; only when the layout is the same
		void apply_client_omnvars(std::uint8_t* snap, const std::uint8_t* omnvars, const std::uint32_t global, const std::uint32_t client)
		{
			const auto omnvar_global = read_global<std::int32_t>(OMNVAR_GLOBAL_COUNT);
			const auto omnvar_client = read_global<std::int32_t>(OMNVAR_CLIENT_COUNT);
			if (omnvar_client > 0 && omnvar_global >= 0 && omnvar_global + omnvar_client <= static_cast<std::int32_t>(MAX_OMNVARS)
				&& static_cast<std::uint32_t>(omnvar_global) == global && static_cast<std::uint32_t>(omnvar_client) == client)
			{
				std::memcpy(snap + SNAP_OMNVARS + static_cast<std::size_t>(omnvar_global) * OMNVAR_SIZE, omnvars,
				            static_cast<std::size_t>(omnvar_client) * OMNVAR_SIZE);
			}
		}

		// death hold: the 8 corpse slots are reused round-robin, so after 8 more deaths during a long hold the held
		// corpseIndex names somebody else's newer body and the dead camera would jump to it. Clear corpseIndex then:
		// the engine falls back to ps.origin + viewheight, the death spot. A corpse missing from the snapshot needs
		// nothing (the engine checks that itself).
		corpse_check guard_held_corpse(std::uint8_t* snap, const std::int32_t target)
		{
			corpse_check check{};
			const auto corpse = read_as<std::int32_t>(snap, PS_CORPSE_INDEX);
			check.index = corpse;
			if (corpse < 0 || corpse >= MAX_CORPSES)
			{
				return check;
			}

			const auto number = CORPSE_FIRST_ENTITY + corpse;
			const auto count = std::clamp(read_as<std::int32_t>(snap, SNAP_NUM_ENTITIES), 0, MAX_SNAP_ENTITIES);
			for (std::int32_t i = 0; i < count; ++i)
			{
				const auto* es = snap + SNAP_ENTITIES + static_cast<std::size_t>(i) * ES_SIZE;
				const auto es_number = read_as<std::int32_t>(es);
				if (es_number < number)
				{
					continue; // rebuild_entities leaves the list sorted by entity number
				}

				if (es_number == number)
				{
					check.found = true;
					check.type = read_as<std::int32_t>(es, ES_TYPE);
					check.owner = read_as<std::int32_t>(es, ES_CLIENT_NUM);
					if (check.type != ET_PLAYER_CORPSE || check.owner != target)
					{
						write_as<std::int32_t>(snap, PS_CORPSE_INDEX, -1);
						check.cleared = true;
					}
				}

				break;
			}

			return check;
		}

		// per-client omnvars a death hold keeps from the held record (hold_keeps_omnvar), by per-client index, resolved
		// by name from the game's omnvar definitions for the running layout. Omnvar_GetDef 0x1404F3E80 is `return
		// &defs[index]` (0x38-byte records, name pointer first), indexed like the snapshot
		// block [global | per client] (demo_playback.cpp checks "ui_options_menu" at 103 = 37 + 66). nullptr when the
		// names cannot be read or ui_session_state is missing: the hold then shows the held record's whole block.
		const omnvar_mask* hold_omnvar_mask(const std::uint32_t global, const std::uint32_t client)
		{
			static std::uint32_t cached_global = ~0u;
			static std::uint32_t cached_client = ~0u;
			static std::optional<omnvar_mask> cached{};

			if (global != cached_global || client != cached_client)
			{
				cached_global = global;
				cached_client = client;
				cached.reset();

				if (global + client <= MAX_OMNVARS)
				{
					omnvar_mask mask{};
					auto names_ok = true;
					auto session_state = false;
					for (std::uint32_t i = 0; i < client && names_ok; ++i)
					{
						const auto* def = game::Omnvar_GetDef(static_cast<int>(global + i));
						const auto* name = def && utils::hook::is_readable(def, sizeof(const char*)) ? *static_cast<const char* const*>(def) : nullptr;
						names_ok = name && utils::hook::is_readable(name, 64);
						if (names_ok)
						{
							const std::string_view text(name, strnlen(name, 64));
							mask[i] = hold_keeps_omnvar(text);
							session_state |= text == "ui_session_state";
						}
					}

					if (names_ok && session_state)
					{
						cached = mask;
					}
				}
			}

			return cached ? &*cached : nullptr;
		}

		// per-client omnvars of a held snapshot (see substitute): the current record's, with the killcam / camera scene /
		// session state entries from the held record
		void apply_held_client_omnvars(std::uint8_t* snap, const view_choice& choice)
		{
			const auto& held = *choice.held;
			const auto& header = choice.frame.header();
			const auto runtime_global = read_global<std::int32_t>(OMNVAR_GLOBAL_COUNT);
			const auto runtime_client = read_global<std::int32_t>(OMNVAR_CLIENT_COUNT);
			const auto same_layout = held.omnvar_global == header.omnvar_global && held.omnvar_client == header.omnvar_client
				&& static_cast<std::uint32_t>(runtime_global) == header.omnvar_global && static_cast<std::uint32_t>(runtime_client) == header.omnvar_client;
			const auto* keep = same_layout ? hold_omnvar_mask(header.omnvar_global, header.omnvar_client) : nullptr;
			if (!keep)
			{
				apply_client_omnvars(snap, held.omnvars.data(), held.omnvar_global, held.omnvar_client);
				return;
			}

			static std::array<std::uint8_t, static_cast<std::size_t>(MAX_OMNVARS) * OMNVAR_SIZE> merged{};
			merge_hold_omnvars(merged.data(), choice.pov.omnvars, held.omnvars.data(), header.omnvar_client, *keep);
			apply_client_omnvars(snap, merged.data(), header.omnvar_global, header.omnvar_client);
		}

		// the view for this snapshot (view_selector::choose in demo_pov_format.hpp, shared with the dry run)
		view_kind substitute(std::uint8_t* snap)
		{
			const auto target = target_client.load();
			const auto recorded = read_recorder_snapshot(snap, read_as<std::int32_t>(snap, SNAP_SERVER_TIME));
			const auto choice = selector.choose(sidecar, target, show_death.load(), recorded, MAX_FRAME_AGE);
			if (choice.kind == view_kind::recorded)
			{
				return choice.kind;
			}

			latest_view = {};
			latest_view.target = target;
			latest_view.time = choice.frame.header().server_time;

			if (choice.kind == view_kind::live)
			{
				apply_player_state(snap, choice.pov.ps);
				if (choice.mark_spawn_count)
				{
					mark_spawn_count(snap); // the dead run of a death that is followed by a hold (see PS_FIELD_SPAWN_COUNT)
				}

				rebuild_entities(snap, choice.frame, choice.pov, -1);
				apply_client_omnvars(snap, choice.pov.omnvars, choice.frame.header().omnvar_global, choice.frame.header().omnvar_client);

				latest_view.dead = (choice.pov.header.flags & CF_DEAD) != 0;
				return choice.kind;
			}

			// death hold (killcam, spectating or waiting to respawn after a death): like BO2, the player's own death
			// camera stays. The engine renders it from the held playerState alone (dead camera behind the corpse along
			// ps.stats[1], no time-based logic; prediction is off in demo playback: deathcam report sections 1 and 4).
			// - playerState from the held record, with the spawn count marked like the dead run before it: the dead run
			//   -> hold transition stays identical, and the respawn always takes CG_SetNextSnap's reset path (a clean cut
			//   to the spawn point) even when the killcam left the killer's count + 1 equal to the held one (see
			//   PS_FIELD_SPAWN_COUNT in demo_pov_format.hpp; the 0x4000 flip does not help there: CG_SetNextSnap compares
			//   it only for a new playerState with a body, and demo playback clears 0x4000 on alive ones)
			// - per-client omnvars from the current record except the killcam / camera scene / session state ones, which
			//   stay the held record's (apply_held_client_omnvars): ui_session_state stays "dead", no killcam HUD or
			//   camera scene, but the round-end / match outcome splash and the other per-client HUD messages show
			// - entities from the current record with the target's CURRENT exclusions: the world keeps moving, and
			//   the held record's list would let in entities created later that are hidden from the target (design
			//   report section 3). The target's own entity is never sent to itself.
			const auto& held = *choice.held;
			apply_player_state(snap, held.ps.data());
			if (choice.mark_spawn_count) // always set for a hold
			{
				mark_spawn_count(snap);
			}

			rebuild_entities(snap, choice.frame, choice.pov, target);
			latest_view.corpse = guard_held_corpse(snap, target);
			apply_held_client_omnvars(snap, choice);

			const auto& index = sidecar.index();
			latest_view.died_at = index[choice.period.dead_first].time;
			latest_view.held_at = held.time;
			latest_view.until = choice.period.end < index.size() ? index[choice.period.end].time : -1;
			latest_view.held_record = choice.period.dead_last;
			latest_view.reason = choice.period.reason;
			return choice.kind;
		}

		utils::hook::detour cl_get_snapshot_hook;

		int cl_get_snapshot_stub(const int local_client_num, const int snapshot_number, std::uint8_t* snapshot)
		{
			const auto result = cl_get_snapshot_hook.invoke<int>(local_client_num, snapshot_number, snapshot);
			if (!result || !snapshot || !playback_active.load(std::memory_order_relaxed) || target_client.load() < 0)
			{
				latest_substituted.store(false);
				latest_held.store(false);
				latest_held_record.store(-1);
				return result;
			}

			auto shown = view_kind::recorded;
			std::int64_t held_record = -1;
			try
			{
				std::lock_guard _(reader_mutex);
				shown = substitute(snapshot);
				if (shown == view_kind::held)
				{
					held_record = latest_view.held_record;
				}
			}
			catch (...)
			{
				shown = view_kind::recorded;
				held_record = -1;
			}

			latest_substituted.store(shown != view_kind::recorded);
			latest_held.store(shown == view_kind::held);
			latest_held_record.store(held_record);
			return result;
		}

		// ======================================================================================================
		// helpers for the API / commands (main thread)
		// ======================================================================================================

		// summary of the sidecar record at the current playback time (caller holds reader_mutex)
		std::optional<summary_t> summary_now(std::int32_t* host = nullptr, std::uint32_t* client_count = nullptr, std::size_t* record = nullptr)
		{
			if (!sidecar.loaded())
			{
				return {};
			}

			const auto found = sidecar.find(game::mp::cl->serverTime, 1000);
			if (!found)
			{
				return {};
			}

			const auto& entry = sidecar.index()[*found];
			if (host)
			{
				*host = entry.host_client;
			}
			if (client_count)
			{
				*client_count = entry.client_count;
			}
			if (record)
			{
				*record = *found;
			}

			return entry.summary;
		}

		// "mm:ss.mmm" counted from the sidecar's first record, like pov_events (caller holds reader_mutex)
		std::string film_time(const std::int32_t server_time)
		{
			const auto ms = server_time - (sidecar.loaded() ? sidecar.index().front().time : 0);
			return std::format("{:02}:{:06.3f}", ms / 60000, (ms % 60000) / 1000.0);
		}

		std::string name_of(const int client_num)
		{
			if (client_num < 0 || client_num >= static_cast<int>(MAX_CLIENTS) || !game::CL_IsCgameInitialized())
			{
				return {};
			}

			// cgame snapshot client list (CG_SetNextSnap copies these names into clientinfo)
			const auto* snap = reinterpret_cast<const std::uint8_t*>(game::mp::cg->nextSnap);
			if (!snap)
			{
				snap = reinterpret_cast<const std::uint8_t*>(game::mp::cg->snap);
			}

			if (!snap)
			{
				return {};
			}

			const auto count = std::clamp(read_as<std::int32_t>(snap, SNAP_NUM_CLIENTS), 0, static_cast<std::int32_t>(MAX_CLIENTS));
			for (std::int32_t i = 0; i < count; ++i)
			{
				const auto* cs = snap + SNAP_CLIENTS + static_cast<std::size_t>(i) * CLIENT_STATE_SIZE;
				if (read_as<std::int32_t>(cs) == client_num)
				{
					return utils::string::strip(fixed_string(reinterpret_cast<const char*>(cs + CLIENT_STATE_NAME), 16), true);
				}
			}

			return {};
		}

		void print_info()
		{
			const auto rec = recorder.stats();
			console::info("pov: recording: %s (%u sidecar file(s) open, %llu frames, %.1f MB written, %llu frames dropped)\n",
			              rec.open_files ? "yes" : "no", rec.open_files, static_cast<unsigned long long>(rec.frames),
			              static_cast<double>(rec.bytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(rec.dropped));

			std::lock_guard _(reader_mutex);
			if (!sidecar.loaded())
			{
				console::info("pov: playback: %s\n", demo_playback::playing() ? "no sidecar for this demo" : "no demo playing");
				return;
			}

			const auto& index = sidecar.index();
			const auto& header = sidecar.header();
			console::info("pov: playback: %s\n", sidecar.path().string().data());
			console::info("pov:   %zu frames (%zu keyframes), %.1f MB, server time %d .. %d (%.0f s), map %s / %s, recorder client %d, %llu records decoded\n",
			              index.size(), sidecar.keyframes(), static_cast<double>(sidecar.file_size()) / (1024.0 * 1024.0),
			              index.front().time, index.back().time, (index.back().time - index.front().time) / 1000.0,
			              fixed_string(header.mapname.data(), header.mapname.size()).data(),
			              fixed_string(header.gametype.data(), header.gametype.size()).data(), index.back().host_client,
			              static_cast<unsigned long long>(sidecar.decoded_records()));

			std::int32_t host{};
			std::uint32_t client_count{};
			std::size_t record{};
			if (const auto summary = summary_now(&host, &client_count, &record))
			{
				for (std::uint32_t c = 0; c < client_count; ++c)
				{
					const auto bit = 1u << c;
					if (!(summary->connected_mask & bit))
					{
						continue;
					}

					// "after death": killcam / spectating / waiting to respawn after a death (the death hold applies)
					const auto after_death = find_hold(sidecar.death_periods(static_cast<std::int32_t>(c)), record) != nullptr;
					const auto* state = (summary->alive_mask & bit) ? "alive" : (summary->dead_mask & bit) ? "dead" : after_death ? "after death" : "not playing";
					console::info("pov:   client %2u  %-16s %-12s kills %u deaths %u%s\n", c, name_of(static_cast<int>(c)).data(), state,
					              static_cast<unsigned>(summary->kills[c]), static_cast<unsigned>(summary->deaths[c]),
					              static_cast<std::int32_t>(c) == host ? "  (recorder)" : "");
				}
			}

			const auto target = target_client.load();
			console::info("pov:   target %d%s, substituting %s, death camera %s, %zu life events\n", target,
			              target < 0 ? " (recorder)" : "", latest_substituted.load() ? "yes" : "no", show_death.load() ? "on" : "off",
			              sidecar.events().size());

			// what the latest snapshot showed
			const auto& v = latest_view;
			if (!latest_substituted.load())
			{
				console::info("pov:   view: recorded view (not substituted)\n");
			}
			else if (latest_held.load())
			{
				console::info("pov:   view: death hold of client %d - died at %s, showing its last dead frame (record %u @%s) until %s (%s); world from %s\n",
				              v.target, film_time(v.died_at).data(), v.held_record, film_time(v.held_at).data(),
				              v.until >= 0 ? film_time(v.until).data() : "the end", end_reason_name(v.reason), film_time(v.time).data());
				if (v.corpse.index >= 0 && v.corpse.index < MAX_CORPSES)
				{
					console::info("pov:   held body: entity %d %s%s\n", CORPSE_FIRST_ENTITY + v.corpse.index,
					              v.corpse.found ? std::format("in the snapshot (eType {}, client {})", v.corpse.type, v.corpse.owner).data() : "not in the snapshot",
					              v.corpse.cleared ? " - not the player's body: the camera uses the death spot" : "");
				}
				else
				{
					console::info("pov:   held body: none (corpseIndex %d)\n", v.corpse.index);
				}
			}
			else
			{
				console::info("pov:   view: live, client %d %s @%s\n", v.target, v.dead ? "dead (death camera)" : "alive", film_time(v.time).data());
			}

			console::info("pov:   death hold: %llu held frames decoded on demand, %llu kept while playing through them\n",
			              static_cast<unsigned long long>(selector.held_loads()), static_cast<unsigned long long>(selector.held_kept()));
		}

		// whole argument must be a number ("pov abc" must not silently select client 0)
		std::optional<int> parse_int(const std::string_view text)
		{
			int value{};
			const auto* end = text.data() + text.size();
			const auto [ptr, ec] = std::from_chars(text.data(), end, value);
			if (ec != std::errc{} || ptr != end || text.empty())
			{
				return {};
			}

			return value;
		}

		void add_commands()
		{
			command::add("pov_info", []()
			{
				print_info();
			});

			command::add("pov", [](const command::params& params)
			{
				const auto arg = params.size() >= 2 ? utils::string::to_lower(params[1]) : std::string{};
				const auto number = parse_int(arg);
				if (params.size() < 2 || (arg != "next" && arg != "prev" && !number))
				{
					console::info("usage: pov <client number | -1 | next | prev>   (current target %d; see pov_info)\n", target_client.load());
					return;
				}

				if (number)
				{
					set_target(*number);
				}
				else
				{
					cycle_target(arg == "next" ? 1 : -1);
				}

				const auto target = target_client.load();
				const auto name = name_of(target);
				console::info("pov: target %d%s%s%s\n", target, target < 0 ? " (recorder)" : "", name.empty() ? "" : " ", name.data());

				if (target >= 0 && !available())
				{
					console::warn("pov: no sidecar loaded for this demo; the recorded view stays active\n");
				}
			});

			command::add("pov_death", [](const command::params& params)
			{
				if (params.size() >= 2)
				{
					set_show_death(std::atoi(params[1]) != 0);
				}

				console::info("pov: death camera %s\n", get_show_death() ? "on" : "off");
			});

			command::add("pov_events", [](const command::params& params)
			{
				const auto parsed = params.size() >= 2 ? parse_int(params[1]) : std::optional<int>(-1);
				if (!parsed)
				{
					console::info("usage: pov_events [client number]\n");
					return;
				}

				const auto client = *parsed;
				const auto events = get_life_events(client);

				std::int32_t first_time{};
				{
					std::lock_guard _(reader_mutex);
					if (!sidecar.loaded())
					{
						console::info("pov: no sidecar loaded\n");
						return;
					}

					first_time = sidecar.index().front().time;
				}

				static constexpr std::array names{"spawn", "death", "kill"};
				for (const auto& event : events)
				{
					const auto seconds = (event.server_time - first_time) / 1000;
					console::info("pov:   %02d:%02d  client %2d  %s\n", seconds / 60, seconds % 60, event.client_num,
					              names[static_cast<std::size_t>(event.kind)]);
				}

				console::info("pov: %zu life events%s\n", events.size(), client >= 0 ? std::format(" for client {}", client).data() : "");
			});
		}
	}

	// ==========================================================================================================
	// public API
	// ==========================================================================================================

	bool available()
	{
		std::lock_guard _(reader_mutex);
		return sidecar.loaded() && playback_active.load();
	}

	void set_target(const int client_num)
	{
		target_client.store(client_num >= 0 && client_num < static_cast<int>(MAX_CLIENTS) ? client_num : -1);
	}

	int get_target()
	{
		return target_client.load();
	}

	bool substituting()
	{
		return latest_substituted.load();
	}

	std::int64_t held_record()
	{
		return latest_held_record.load();
	}

	bool client_alive(const int client_num)
	{
		if (client_num < 0 || client_num >= static_cast<int>(MAX_CLIENTS))
		{
			return false;
		}

		std::lock_guard _(reader_mutex);
		const auto summary = summary_now();
		return summary && (summary->alive_mask >> client_num) & 1;
	}

	std::vector<life_event> get_life_events(const int client_num)
	{
		std::lock_guard _(reader_mutex);
		std::vector<life_event> result;
		for (const auto& event : sidecar.events())
		{
			if (client_num < 0 || event.client_num == client_num)
			{
				result.push_back(event);
			}
		}

		return result;
	}

	bool recording_sidecar()
	{
		return recorder.stats().open_files > 0;
	}

	int get_recorder()
	{
		std::lock_guard _(reader_mutex);
		return sidecar.loaded() ? sidecar.index().back().host_client : -1;
	}

	std::vector<int> get_clients()
	{
		std::lock_guard _(reader_mutex);
		std::vector<int> result;
		std::uint32_t client_count{};
		if (const auto summary = summary_now(nullptr, &client_count))
		{
			for (std::uint32_t c = 0; c < client_count; ++c)
			{
				if ((summary->connected_mask >> c) & 1)
				{
					result.push_back(static_cast<int>(c));
				}
			}
		}

		return result;
	}

	std::string get_client_name(const int client_num)
	{
		return name_of(client_num);
	}

	void cycle_target(const int direction)
	{
		std::int32_t host = -1;
		std::uint32_t client_count{};
		std::optional<summary_t> summary;
		{
			std::lock_guard _(reader_mutex);
			summary = summary_now(&host, &client_count);
		}

		if (!summary || direction == 0)
		{
			return;
		}

		// ring: recorder (-1), then every other connected client; prefer the ones alive right now
		std::vector<int> ring{-1};
		std::vector<int> alive_ring{-1};
		for (std::uint32_t c = 0; c < client_count; ++c)
		{
			if (static_cast<std::int32_t>(c) == host || !((summary->connected_mask >> c) & 1))
			{
				continue;
			}

			ring.push_back(static_cast<int>(c));
			if ((summary->alive_mask >> c) & 1)
			{
				alive_ring.push_back(static_cast<int>(c));
			}
		}

		const auto& candidates = alive_ring.size() > 1 ? alive_ring : ring;
		const auto current = target_client.load();
		const auto size = static_cast<int>(candidates.size());

		auto position = -1;
		for (auto k = 0; k < size; ++k)
		{
			if (candidates[static_cast<std::size_t>(k)] == current)
			{
				position = k;
				break;
			}
		}

		if (position < 0)
		{
			// current target not in the list: continue from the closest lower client number
			position = 0;
			for (auto k = 0; k < size; ++k)
			{
				if (candidates[static_cast<std::size_t>(k)] < current)
				{
					position = k;
				}
			}
		}

		const auto step = direction > 0 ? 1 : -1;
		set_target(candidates[static_cast<std::size_t>(((position + step) % size + size) % size)]);
	}

	void set_show_death(const bool enabled)
	{
		show_death.store(enabled);
	}

	bool get_show_death()
	{
		return show_death.load();
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

			if (!prologue_ok(SV_SEND_CLIENT_MESSAGES, SV_SEND_CLIENT_MESSAGES_PROLOGUE) || !prologue_ok(CL_GET_SNAPSHOT, CL_GET_SNAPSHOT_PROLOGUE))
			{
				console::error("pov: unexpected code at SV_SendClientMessages / CL_GetSnapshot, player POV disabled\n");
				return;
			}

			theater_pov_record = game::Dvar_RegisterBool("theater_pov_record", true, game::DVAR_FLAG_SAVED,
			                                             "Write every player's view next to demos recorded as the host (<demo>.dm_iw6.pov)");

			recorder.start();

			// capture the server state of every client right before the snapshots of a server frame are built
			sv_send_client_messages_hook.create(SV_SEND_CLIENT_MESSAGES, sv_send_client_messages_stub);

			// rebuild the cgame snapshot for the selected player
			cl_get_snapshot_hook.create(CL_GET_SNAPSHOT, cl_get_snapshot_stub);

			scheduler::loop(main_frame, scheduler::main);
			add_commands();
		}
	};
}

REGISTER_COMPONENT(demo_pov::component)
