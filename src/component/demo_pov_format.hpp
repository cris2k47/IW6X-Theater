#pragma once

// Sidecar format of demo_pov ("<demo>.dm_iw6.pov"). No game dependencies, so it can be tested standalone.
//
// file   = file_header | record*
// record = record_header | zlib(payload)
//          payload = raw frame (keyframe) or raw frame XOR previous raw frame (delta)
// raw frame = frame_header | client block * client_count | host-excluded entityState * MAX_HOST_EXCLUDED
// client block = client_header | playerState (0x3328) | entityState (0x108) | per-client omnvars (8 bytes each)
//
// Every record header also carries an uncompressed summary (alive / dead / connected masks, kills, deaths) so
// the timeline and client_alive() work without decompressing anything. Records are self-delimiting: a file cut
// short by a crash is readable up to its last complete record.
//
// The playback's view selection (which record the 1st-person view of a player shows, including the death hold)
// is here as well, so a standalone dry run outside the game can run the same code.

#include "demo_pov.hpp"

#include <zlib.h>

namespace demo_pov::format
{
	constexpr std::uint32_t MAX_CLIENTS = 18;
	constexpr std::uint32_t MAX_OMNVARS = 256;        // snapshot_t::omnvars[256]
	constexpr std::uint32_t MAX_EXCLUDED = 127;       // entity numbers excluded per client
	constexpr std::uint32_t MAX_HOST_EXCLUDED = 128;  // entityStates the host's snapshot leaves out
	constexpr std::size_t PS_SIZE = 0x3328;           // playerState_t
	constexpr std::size_t ES_SIZE = 0x108;            // entityState_t
	constexpr std::size_t OMNVAR_SIZE = 8;            // OmnvarData
	constexpr std::size_t SCORE_SIZE = 18;            // ScoreInfo
	constexpr std::size_t SCORE_KILLS = 6;
	constexpr std::size_t SCORE_DEATHS = 8;
	constexpr std::uint32_t KEYFRAME_INTERVAL = 100;  // frames (5 s at sv_fps 20)

	constexpr std::array<char, 8> FILE_MAGIC{'I', 'W', '6', 'P', 'O', 'V', '\x1A', '\0'};
	constexpr std::uint32_t FILE_VERSION = 1;
	constexpr std::uint32_t RECORD_TAG = 0x52564F50;  // "POVR"
	constexpr std::uint32_t FRAME_MAGIC = 0x46564F50; // "POVF"

	constexpr std::uint16_t RECORD_KEYFRAME = 1;
	constexpr std::uint16_t RECORD_SERVER_BIT_CHANGED = 2;

	// client_header::flags (derived at capture time)
	constexpr std::uint8_t CF_CONNECTED = 0x01;
	constexpr std::uint8_t CF_PLAYING = 0x02;   // otherFlags & 0x4000 and not a follow copy
	constexpr std::uint8_t CF_ALIVE = 0x04;
	constexpr std::uint8_t CF_DEAD = 0x08;      // playing, pm_type 7/8
	constexpr std::uint8_t CF_FOLLOWING = 0x10; // otherFlags & 0x2000
	constexpr std::uint8_t CF_KILLCAM = 0x20;   // ps killcam time (ps+0x13CC) != 0
	constexpr std::uint8_t CF_HOST = 0x40;      // loopback client (the recorder)
	constexpr std::uint8_t CF_BOT = 0x80;

#pragma pack(push, 1)
	struct file_header
	{
		std::array<char, 8> magic;
		std::uint32_t version;
		std::uint32_t header_size;
		std::uint32_t build_marker;   // game::VERSION_MARKER_VALUE of the recording build
		std::uint32_t ps_size;
		std::uint32_t es_size;
		std::uint32_t client_header_size;
		std::uint32_t frame_header_size;
		std::uint32_t max_clients;
		std::uint32_t max_excluded;
		std::uint32_t max_host_excluded;
		std::int32_t first_server_time;
		std::int32_t host_client;
		std::array<char, 64> mapname;
		std::array<char, 32> gametype;
		std::array<char, 128> demo_name;
		std::array<std::uint8_t, 64> reserved;
	};

	struct record_header
	{
		std::uint32_t tag;
		std::uint16_t header_size;
		std::uint16_t flags;
		std::int32_t server_time;
		std::uint32_t raw_size;
		std::uint32_t comp_size;
		std::uint32_t crc;            // crc32 of the compressed payload
		std::uint8_t client_count;
		std::uint8_t host_client;
		std::uint16_t omnvar_global;
		std::uint16_t omnvar_client;
		std::uint16_t reserved;
		std::uint32_t alive_mask;
		std::uint32_t dead_mask;
		std::uint32_t connected_mask;
		std::array<std::uint16_t, MAX_CLIENTS> kills;
		std::array<std::uint16_t, MAX_CLIENTS> deaths;
	};

	struct frame_header
	{
		std::uint32_t magic;
		std::int32_t server_time;
		std::int32_t host_client;
		std::uint32_t snap_flag_server_bit;
		std::uint32_t client_count;
		std::uint32_t omnvar_global;
		std::uint32_t omnvar_client;
		std::uint32_t shared_entities;
		std::uint32_t host_excluded_count;
		std::array<std::uint32_t, 7> reserved;
	};

	struct client_header
	{
		std::uint8_t slot_state;  // client_t::header.state
		std::uint8_t flags;       // CF_*
		std::uint8_t linked;      // gentity r.linked
		std::uint8_t sv_flags;    // gentity r.svFlags
		std::int32_t session_state;
		std::int32_t in_killcam;
		std::int32_t archive_time;
		std::int32_t spectator_client;
		std::array<std::uint8_t, 20> score; // ScoreInfo (18 bytes) + 2 padding
		std::uint16_t excluded_count;
		std::array<std::uint16_t, MAX_EXCLUDED> excluded;
	};
#pragma pack(pop)

	static_assert(sizeof(file_header) == 344);
	static_assert(sizeof(record_header) == 116);
	static_assert(sizeof(frame_header) == 64);
	static_assert(sizeof(client_header) == 296);

	constexpr std::size_t client_block_size(const std::uint32_t omnvar_client)
	{
		return sizeof(client_header) + PS_SIZE + ES_SIZE + static_cast<std::size_t>(omnvar_client) * OMNVAR_SIZE;
	}

	constexpr std::size_t frame_size(const std::uint32_t client_count, const std::uint32_t omnvar_client)
	{
		return sizeof(frame_header) + client_count * client_block_size(omnvar_client) + MAX_HOST_EXCLUDED * ES_SIZE;
	}

	constexpr std::size_t MAX_FRAME_SIZE = frame_size(MAX_CLIENTS, MAX_OMNVARS);

	template <typename T>
	T read_as(const void* base, const std::size_t offset = 0)
	{
		T value{};
		std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
		return value;
	}

	template <typename T>
	void write_as(void* base, const std::size_t offset, const T& value)
	{
		std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
	}

	inline std::string fixed_string(const char* data, const std::size_t max_size)
	{
		return std::string(data, strnlen(data, max_size));
	}

	template <std::size_t N>
	void copy_fixed_string(std::array<char, N>& dst, const std::string_view src)
	{
		dst.fill('\0');
		std::memcpy(dst.data(), src.data(), std::min(src.size(), N - 1));
	}

	inline std::filesystem::path sidecar_path(const std::filesystem::path& demo_path)
	{
		auto path = demo_path;
		path += ".pov";
		return path;
	}

	// ==========================================================================================================
	// .dm_iw6 identity: map header (record type 1) and general footer (type 6). Record format (demo_utils.cpp
	// write_id_and_size): id byte (id & 0x1F = type, id & 0x80 = one-byte size), size (1 or 4 bytes), data.
	// The map header is one of the first records (gen_header, mod_header, map_header) and the footer is the
	// record right before the 2-byte eof record (0x9F 0x00), so only both ends of the file are read: scanning
	// every record of a long demo stalls the main thread for a noticeable time when playback starts.
	// ==========================================================================================================

	struct demo_identity
	{
		std::string mapname{};
		std::string gametype{};
		std::int32_t first_time{}; // both 0 when the demo has no footer (recording cut short)
		std::int32_t last_time{};
	};

	inline std::optional<demo_identity> read_demo_identity(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open())
		{
			return {};
		}

		std::optional<demo_identity> identity{};
		for (auto records = 0; records < 16 && !identity; ++records)
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
			if (!file || type == 31 || size > 0x1000000)
			{
				break;
			}

			if (type != 1)
			{
				file.seekg(size, std::ios::cur);
				continue;
			}

			std::array<char, 128> buffer{};
			if (size < 4 || size > 127 || !file.read(buffer.data(), size))
			{
				break;
			}

			demo_identity result{};
			result.mapname = fixed_string(buffer.data(), size);
			const auto gametype_offset = result.mapname.size() + 1;
			if (gametype_offset < size)
			{
				result.gametype = fixed_string(buffer.data() + gametype_offset, size - gametype_offset);
			}

			identity = std::move(result);
		}

		if (!identity)
		{
			return {};
		}

		// footer = 0x86 | size | first time (4) | last time (4) | "[hh:mm:ss]" | '\0', followed by eof = 0x9F 0x00
		constexpr std::size_t MIN_FOOTER = 8 + 3; // times + "[]" + '\0'
		std::error_code ec;
		const auto file_size = std::filesystem::file_size(path, ec);
		if (ec || file_size < 2 + MIN_FOOTER + 2)
		{
			return identity;
		}

		std::array<std::uint8_t, 2 + 255 + 2> tail{};
		const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(tail.size(), file_size));
		file.clear();
		file.seekg(static_cast<std::streamoff>(file_size - n));
		if (!file.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(n)) || tail[n - 2] != (31 | 0x80) || tail[n - 1] != 0)
		{
			return identity;
		}

		const auto eof_at = n - 2;
		for (auto size = MIN_FOOTER; size <= 255 && size + 2 <= eof_at; ++size)
		{
			const auto at = eof_at - size - 2;
			const auto* data = tail.data() + at + 2;
			if (tail[at] == (6 | 0x80) && tail[at + 1] == size && data[8] == '[' && data[size - 2] == ']' && data[size - 1] == '\0')
			{
				identity->first_time = read_as<std::int32_t>(data, 0);
				identity->last_time = read_as<std::int32_t>(data, 4);
				break;
			}
		}

		return identity;
	}

	struct summary_t
	{
		std::uint32_t alive_mask{};
		std::uint32_t dead_mask{};
		std::uint32_t connected_mask{};
		std::array<std::uint16_t, MAX_CLIENTS> kills{};
		std::array<std::uint16_t, MAX_CLIENTS> deaths{};
	};

	struct client_view
	{
		client_header header{};
		const std::uint8_t* ps{};
		const std::uint8_t* es{};
		const std::uint8_t* omnvars{};

		[[nodiscard]] bool valid() const { return ps != nullptr; }
	};

	// bounds-checked access to a decoded raw frame
	class frame_view
	{
	public:
		frame_view(const std::uint8_t* data, const std::size_t size)
			: data_(data), size_(size)
		{
			if (!data_ || size_ < sizeof(frame_header))
			{
				return;
			}

			header_ = read_as<frame_header>(data_);
			valid_ = header_.magic == FRAME_MAGIC
				&& header_.client_count >= 1 && header_.client_count <= MAX_CLIENTS
				&& header_.omnvar_client <= MAX_OMNVARS && header_.omnvar_global <= MAX_OMNVARS
				&& header_.host_client >= 0 && static_cast<std::uint32_t>(header_.host_client) < header_.client_count
				&& header_.host_excluded_count <= MAX_HOST_EXCLUDED
				&& frame_size(header_.client_count, header_.omnvar_client) == size_;
		}

		[[nodiscard]] bool valid() const { return valid_; }
		[[nodiscard]] const frame_header& header() const { return header_; }

		[[nodiscard]] client_view client(const std::int32_t index) const
		{
			client_view view{};
			if (!valid_ || index < 0 || static_cast<std::uint32_t>(index) >= header_.client_count)
			{
				return view;
			}

			const auto* block = data_ + sizeof(frame_header) + static_cast<std::size_t>(index) * client_block_size(header_.omnvar_client);
			view.header = read_as<client_header>(block);
			view.header.excluded_count = std::min<std::uint16_t>(view.header.excluded_count, MAX_EXCLUDED);
			view.ps = block + sizeof(client_header);
			view.es = view.ps + PS_SIZE;
			view.omnvars = view.es + ES_SIZE;
			return view;
		}

		[[nodiscard]] const std::uint8_t* host_excluded(const std::uint32_t index) const
		{
			if (!valid_ || index >= header_.host_excluded_count)
			{
				return nullptr;
			}

			return data_ + sizeof(frame_header) + header_.client_count * client_block_size(header_.omnvar_client) + index * ES_SIZE;
		}

	private:
		const std::uint8_t* data_{};
		std::size_t size_{};
		frame_header header_{};
		bool valid_{};
	};

	inline file_header make_file_header(const std::uint32_t build_marker, const std::int32_t first_server_time, const std::int32_t host_client,
	                                    const std::string_view mapname, const std::string_view gametype, const std::string_view demo_name)
	{
		file_header header{};
		header.magic = FILE_MAGIC;
		header.version = FILE_VERSION;
		header.header_size = sizeof(file_header);
		header.build_marker = build_marker;
		header.ps_size = static_cast<std::uint32_t>(PS_SIZE);
		header.es_size = static_cast<std::uint32_t>(ES_SIZE);
		header.client_header_size = sizeof(client_header);
		header.frame_header_size = sizeof(frame_header);
		header.max_clients = MAX_CLIENTS;
		header.max_excluded = MAX_EXCLUDED;
		header.max_host_excluded = MAX_HOST_EXCLUDED;
		header.first_server_time = first_server_time;
		header.host_client = host_client;
		copy_fixed_string(header.mapname, mapname);
		copy_fixed_string(header.gametype, gametype);
		copy_fixed_string(header.demo_name, demo_name);
		return header;
	}

	// ==========================================================================================================
	// encoder: XOR against the previous frame + deflate (one instance per stream of frames)
	// ==========================================================================================================

	struct frame_info
	{
		std::int32_t server_time{};
		std::uint32_t client_count{};
		std::int32_t host_client{};
		std::uint32_t omnvar_global{};
		std::uint32_t omnvar_client{};
		std::uint32_t server_bit{};
		summary_t summary{};
	};

	class encoder
	{
	public:
		explicit encoder(const int level)
		{
			ready_ = deflateInit(&stream_, level) == Z_OK;
			prev_.resize(MAX_FRAME_SIZE);
			delta_.resize(MAX_FRAME_SIZE);
			comp_.resize(compressBound(static_cast<uLong>(MAX_FRAME_SIZE)) + 64);
		}

		~encoder()
		{
			if (ready_)
			{
				deflateEnd(&stream_);
			}
		}

		encoder(const encoder&) = delete;
		encoder& operator=(const encoder&) = delete;

		[[nodiscard]] bool ready() const { return ready_; }

		// the next frame is written as a keyframe
		void reset() { prev_valid_ = false; }

		// encodes one raw frame; the payload stays valid until the next call
		bool encode(const std::uint8_t* raw, const std::size_t size, const frame_info& info, const bool force_key,
		            record_header& record, std::span<const std::uint8_t>& payload)
		{
			if (!ready_ || !raw || size < sizeof(frame_header) || size > MAX_FRAME_SIZE)
			{
				return false;
			}

			const auto server_bit_changed = prev_valid_ && prev_server_bit_ != info.server_bit;
			const auto key = force_key || !prev_valid_ || prev_size_ != size || frames_since_key_ >= KEYFRAME_INTERVAL || server_bit_changed;

			const std::uint8_t* input = raw;
			if (!key)
			{
				for (std::size_t i = 0; i < size; ++i)
				{
					delta_[i] = static_cast<std::uint8_t>(raw[i] ^ prev_[i]);
				}
				input = delta_.data();
			}

			deflateReset(&stream_);
			stream_.next_in = input;
			stream_.avail_in = static_cast<uInt>(size);
			stream_.next_out = comp_.data();
			stream_.avail_out = static_cast<uInt>(comp_.size());
			if (deflate(&stream_, Z_FINISH) != Z_STREAM_END)
			{
				prev_valid_ = false;
				return false;
			}

			const auto comp_size = static_cast<std::uint32_t>(stream_.total_out);

			record = {};
			record.tag = RECORD_TAG;
			record.header_size = sizeof(record_header);
			record.flags = static_cast<std::uint16_t>((key ? RECORD_KEYFRAME : 0) | (server_bit_changed ? RECORD_SERVER_BIT_CHANGED : 0));
			record.server_time = info.server_time;
			record.raw_size = static_cast<std::uint32_t>(size);
			record.comp_size = comp_size;
			record.crc = static_cast<std::uint32_t>(crc32(0, comp_.data(), comp_size));
			record.client_count = static_cast<std::uint8_t>(info.client_count);
			record.host_client = static_cast<std::uint8_t>(info.host_client);
			record.omnvar_global = static_cast<std::uint16_t>(info.omnvar_global);
			record.omnvar_client = static_cast<std::uint16_t>(info.omnvar_client);
			record.alive_mask = info.summary.alive_mask;
			record.dead_mask = info.summary.dead_mask;
			record.connected_mask = info.summary.connected_mask;
			record.kills = info.summary.kills;
			record.deaths = info.summary.deaths;

			payload = std::span<const std::uint8_t>(comp_.data(), comp_size);

			std::memcpy(prev_.data(), raw, size);
			prev_size_ = size;
			prev_valid_ = true;
			prev_server_bit_ = info.server_bit;
			frames_since_key_ = key ? 1 : frames_since_key_ + 1;
			return true;
		}

	private:
		z_stream stream_{};
		bool ready_{};
		std::vector<std::uint8_t> prev_{};
		std::vector<std::uint8_t> delta_{};
		std::vector<std::uint8_t> comp_{};
		std::size_t prev_size_{};
		bool prev_valid_{};
		std::uint32_t prev_server_bit_{};
		std::uint32_t frames_since_key_{};
	};

	// ==========================================================================================================
	// reader: index, random access decoding, life events
	// ==========================================================================================================

	struct index_entry
	{
		std::int32_t time{};
		std::uint16_t flags{};
		std::uint8_t client_count{};
		std::uint8_t host_client{};
		std::uint16_t omnvar_global{};
		std::uint16_t omnvar_client{};
		std::uint32_t raw_size{};
		std::uint32_t comp_size{};
		std::uint32_t crc{};
		std::uint64_t offset{}; // payload offset in the file
		std::int32_t key{-1};   // record index of the keyframe this one depends on (-1 = undecodable)
		summary_t summary{};
	};

	// ==========================================================================================================
	// death periods (death hold): like BO2, the 1st-person view of a player who died keeps that player's own death
	// camera until it spawns again, also while the match had it watching its killcam or spectating. Those records
	// show the LAST dead record of that life (the "held record"); the world still comes from the current record.
	//
	// Sample ejemplo_bots.dm_iw6.pov: a death without killcam
	// is 20 dead records (1.00 s) and then the respawn; a death with killcam is 21 dead records (1.05 s), 139
	// killcam records (flags C---FK-B, session 2: a follow copy of the killer's archived playerState) and then the
	// respawn. The dead playerState does not change during the dead run (commandTime frozen) and the engine's dead
	// camera has no time-based logic (CG_OffsetThirdPersonView 0x14029B7B0), so the last
	// dead playerState can be shown for the whole period.
	// ==========================================================================================================

	constexpr std::int32_t MAX_PERIOD_GAP = 1000; // ms between two records; a longer gap (lost frames) ends a period

	// One death of one client and what follows it until the client is alive again. Records [dead_first, dead_last]
	// have the client's dead bit (one uninterrupted run); records (dead_last, end) have it connected, neither alive
	// nor dead (killcam, spectating, waiting to respawn): the hold, which shows record dead_last. Only deaths that
	// are followed by such records are kept: a death followed directly by the respawn needs nothing.
	struct death_period
	{
		enum class end_reason : std::uint8_t
		{
			respawn,    // record `end` has the alive bit
			disconnect, // record `end` lacks the connected bit (or the slot)
			death,      // record `end` starts another dead run without a spawn in between
			gap,        // more than MAX_PERIOD_GAP ms between records end - 1 and end
			restart,    // record `end` starts a new server state (map_restart: RECORD_SERVER_BIT_CHANGED)
			end,        // the sidecar ends inside the period (end == record count)
		};

		std::uint32_t dead_first{};
		std::uint32_t dead_last{}; // the held record
		std::uint32_t end{};       // first record after the period
		end_reason reason{};
	};

	inline const char* end_reason_name(const death_period::end_reason reason)
	{
		switch (reason)
		{
		case death_period::end_reason::respawn: return "respawn";
		case death_period::end_reason::disconnect: return "disconnect";
		case death_period::end_reason::death: return "another death";
		case death_period::end_reason::gap: return "gap in the sidecar";
		case death_period::end_reason::restart: return "map restart";
		default: return "end of the sidecar";
		}
	}

	// pure function of the uncompressed record summaries, computed once when a sidecar is loaded: one pass over the
	// index per client, sorted and disjoint periods (a client never dead in a life gets none: the recorder,
	// spectators waiting for their first spawn, a team change without a death)
	inline std::array<std::vector<death_period>, MAX_CLIENTS> compute_death_periods(const std::vector<index_entry>& index)
	{
		std::array<std::vector<death_period>, MAX_CLIENTS> result{};
		const auto count = static_cast<std::uint32_t>(index.size());

		for (std::uint32_t c = 0; c < MAX_CLIENTS; ++c)
		{
			const auto bit = 1u << c;
			enum class phase
			{
				none,
				dead,
				hold,
			} state = phase::none;
			death_period current{};

			const auto close = [&](const std::uint32_t at, const death_period::end_reason reason)
			{
				if (state == phase::hold)
				{
					current.end = at;
					current.reason = reason;
					result[c].push_back(current);
				}

				state = phase::none;
			};

			for (std::uint32_t i = 0; i < count; ++i)
			{
				const auto& entry = index[i];
				const auto in_slot = c < entry.client_count;
				const auto connected = in_slot && (entry.summary.connected_mask & bit) != 0;
				const auto alive = in_slot && (entry.summary.alive_mask & bit) != 0;
				const auto dead = in_slot && (entry.summary.dead_mask & bit) != 0;

				// a new server state or lost frames end any period: what happened in between is unknown
				if (state != phase::none && i > 0)
				{
					if (entry.flags & RECORD_SERVER_BIT_CHANGED)
					{
						close(i, death_period::end_reason::restart);
					}
					else if (entry.time - index[i - 1].time > MAX_PERIOD_GAP)
					{
						close(i, death_period::end_reason::gap);
					}
				}

				if (state == phase::dead)
				{
					if (dead)
					{
						current.dead_last = i;
						continue;
					}

					// the record after the dead run: a hold when the client is still connected and not alive yet
					// (killcam, spectating), nothing when it respawned at once or left
					state = connected && !alive ? phase::hold : phase::none;
					if (state == phase::hold)
					{
						continue;
					}
				}
				else if (state == phase::hold)
				{
					if (connected && !alive && !dead)
					{
						continue;
					}

					close(i, alive ? death_period::end_reason::respawn
					               : !connected ? death_period::end_reason::disconnect : death_period::end_reason::death);
				}

				if (dead && connected)
				{
					state = phase::dead;
					current = {};
					current.dead_first = i;
					current.dead_last = i;
				}
			}

			close(count, death_period::end_reason::end);
		}

		return result;
	}

	// the period whose hold part (dead_last, end) contains record i, or nullptr (binary search)
	inline const death_period* find_hold(const std::vector<death_period>& periods, const std::size_t i)
	{
		// sorted and disjoint: the candidate is the last period with dead_last < i
		const auto it = std::ranges::upper_bound(periods, static_cast<std::uint32_t>(i), {}, &death_period::dead_last);
		if (it == periods.begin())
		{
			return nullptr;
		}

		const auto& period = *(it - 1);
		return i > period.dead_last && i < period.end ? &period : nullptr;
	}

	// record i is the held record of one of the periods
	inline bool is_held_record(const std::vector<death_period>& periods, const std::size_t i)
	{
		const auto it = std::ranges::lower_bound(periods, static_cast<std::uint32_t>(i), {}, &death_period::dead_last);
		return it != periods.end() && it->dead_last == i;
	}

	// the period whose dead run [dead_first, dead_last] contains record i, or nullptr (binary search): a death that is
	// followed by a hold
	inline const death_period* find_dead_run(const std::vector<death_period>& periods, const std::size_t i)
	{
		const auto it = std::ranges::upper_bound(periods, static_cast<std::uint32_t>(i), {}, &death_period::dead_first);
		if (it == periods.begin())
		{
			return nullptr;
		}

		const auto& period = *(it - 1);
		return i >= period.dead_first && i <= period.dead_last ? &period : nullptr;
	}

	class reader
	{
	public:
		reader() = default;
		reader(const reader&) = delete;
		reader& operator=(const reader&) = delete;

		~reader()
		{
			if (inflate_ready_)
			{
				inflateEnd(&inflate_);
			}
		}

		// opens and indexes a sidecar; build_marker must match the one stored in the file
		bool load(const std::filesystem::path& path, const std::uint32_t build_marker, std::string& error)
		{
			unload();

			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
			{
				error = "no sidecar";
				return false;
			}

			const auto file_size = std::filesystem::file_size(path, ec);
			if (ec || file_size < sizeof(file_header))
			{
				error = "sidecar too small";
				return false;
			}

			file_.open(path, std::ios::binary);
			if (!file_.is_open())
			{
				error = "cannot open sidecar";
				return false;
			}

			file_.read(reinterpret_cast<char*>(&header_), sizeof(header_));
			if (!file_ || header_.magic != FILE_MAGIC || header_.version != FILE_VERSION || header_.header_size != sizeof(file_header)
				|| header_.build_marker != build_marker || header_.ps_size != PS_SIZE || header_.es_size != ES_SIZE
				|| header_.client_header_size != sizeof(client_header) || header_.frame_header_size != sizeof(frame_header)
				|| header_.max_clients != MAX_CLIENTS || header_.max_excluded != MAX_EXCLUDED || header_.max_host_excluded != MAX_HOST_EXCLUDED)
			{
				error = "sidecar header does not match this game build / format";
				unload();
				return false;
			}

			build_index(file_size);
			if (index_.empty())
			{
				error = "sidecar has no frames";
				unload();
				return false;
			}

			if (!inflate_ready_)
			{
				inflate_ready_ = inflateInit(&inflate_) == Z_OK;
				if (!inflate_ready_)
				{
					error = "zlib inflateInit failed";
					unload();
					return false;
				}
			}

			path_ = path;
			file_size_ = file_size;
			chain_bad_at_.assign(index_.size(), -1);
			compute_life_events();
			periods_ = compute_death_periods(index_);
			return true;
		}

		void unload()
		{
			file_.close();
			file_.clear();
			path_.clear();
			header_ = {};
			index_.clear();
			events_.clear();
			periods_ = {};
			chain_bad_at_.clear();
			current_.clear();
			current_index_ = -1;
			file_size_ = 0;
			keyframes_ = 0;
			decoded_records_ = 0;
		}

		[[nodiscard]] bool loaded() const { return !index_.empty() && file_.is_open(); }
		[[nodiscard]] const std::filesystem::path& path() const { return path_; }
		[[nodiscard]] const file_header& header() const { return header_; }
		[[nodiscard]] const std::vector<index_entry>& index() const { return index_; }
		[[nodiscard]] const std::vector<life_event>& events() const { return events_; }
		[[nodiscard]] std::uint64_t file_size() const { return file_size_; }

		// death periods of one client (sorted; empty for slots outside the sidecar)
		[[nodiscard]] const std::vector<death_period>& death_periods(const std::int32_t client) const
		{
			static const std::vector<death_period> none{};
			return client >= 0 && static_cast<std::uint32_t>(client) < MAX_CLIENTS ? periods_[static_cast<std::size_t>(client)] : none;
		}
		[[nodiscard]] std::size_t keyframes() const { return keyframes_; }
		[[nodiscard]] std::uint64_t decoded_records() const { return decoded_records_; }

		// record with the given time, or the latest one before it that is at most max_age ms older
		[[nodiscard]] std::optional<std::size_t> find(const std::int32_t time, const std::int32_t max_age) const
		{
			const auto it = std::ranges::upper_bound(index_, time, {}, &index_entry::time);
			if (it == index_.begin())
			{
				return {};
			}

			const auto index = static_cast<std::size_t>(std::distance(index_.begin(), it) - 1);
			if (static_cast<std::int64_t>(time) - index_[index].time > max_age)
			{
				return {};
			}

			return index;
		}

		// decoded raw frame of record i (index()[i].raw_size bytes) or nullptr
		[[nodiscard]] const std::uint8_t* decode(const std::size_t i)
		{
			if (i >= index_.size())
			{
				return nullptr;
			}

			const auto key = index_[i].key;
			if (key < 0)
			{
				return nullptr;
			}

			const auto bad_at = chain_bad_at_[static_cast<std::size_t>(key)];
			if (bad_at >= 0 && static_cast<std::int64_t>(i) >= bad_at)
			{
				return nullptr;
			}

			if (current_index_ == static_cast<std::int64_t>(i))
			{
				return current_.data();
			}

			// continue the current chain when moving forward, otherwise restart from the keyframe
			auto start = static_cast<std::size_t>(key);
			if (current_index_ >= 0 && current_index_ < static_cast<std::int64_t>(i) && index_[static_cast<std::size_t>(current_index_)].key == key)
			{
				start = static_cast<std::size_t>(current_index_) + 1;
			}

			for (auto j = start; j <= i; ++j)
			{
				if (!apply_record(j))
				{
					auto& bad = chain_bad_at_[static_cast<std::size_t>(key)];
					bad = bad < 0 ? static_cast<std::int64_t>(j) : std::min(bad, static_cast<std::int64_t>(j));
					current_index_ = -1;
					return nullptr;
				}

				current_index_ = static_cast<std::int64_t>(j);
			}

			return current_.data();
		}

	private:
		void build_index(const std::uint64_t file_size)
		{
			std::uint64_t position = sizeof(file_header);
			while (position + sizeof(record_header) <= file_size)
			{
				record_header record{};
				file_.seekg(static_cast<std::streamoff>(position));
				if (!file_.read(reinterpret_cast<char*>(&record), sizeof(record)))
				{
					break;
				}

				const auto payload = position + sizeof(record_header);
				const auto expected_size = record.client_count >= 1 && record.client_count <= MAX_CLIENTS && record.omnvar_client <= MAX_OMNVARS
					                           ? frame_size(record.client_count, record.omnvar_client)
					                           : 0;

				if (record.tag != RECORD_TAG || record.header_size != sizeof(record_header) || !expected_size || record.raw_size != expected_size
					|| record.host_client >= record.client_count || record.omnvar_global + record.omnvar_client > MAX_OMNVARS
					|| record.comp_size == 0 || record.comp_size > compressBound(record.raw_size) + 1024 || payload + record.comp_size > file_size)
				{
					break; // truncated or corrupt from here on
				}

				if (!index_.empty() && record.server_time <= index_.back().time)
				{
					break; // server time must increase within one sidecar
				}

				index_entry entry{};
				entry.time = record.server_time;
				entry.flags = record.flags;
				entry.client_count = record.client_count;
				entry.host_client = record.host_client;
				entry.omnvar_global = record.omnvar_global;
				entry.omnvar_client = record.omnvar_client;
				entry.raw_size = record.raw_size;
				entry.comp_size = record.comp_size;
				entry.crc = record.crc;
				entry.offset = payload;
				entry.summary.alive_mask = record.alive_mask;
				entry.summary.dead_mask = record.dead_mask;
				entry.summary.connected_mask = record.connected_mask;
				entry.summary.kills = record.kills;
				entry.summary.deaths = record.deaths;

				if (record.flags & RECORD_KEYFRAME)
				{
					entry.key = static_cast<std::int32_t>(index_.size());
					++keyframes_;
				}
				else if (!index_.empty() && index_.back().raw_size == record.raw_size)
				{
					entry.key = index_.back().key;
				}

				index_.emplace_back(entry);
				position = payload + record.comp_size;
			}

			file_.clear();
		}

		bool apply_record(const std::size_t i)
		{
			const auto& entry = index_[i];

			payload_.resize(entry.comp_size);
			file_.clear();
			file_.seekg(static_cast<std::streamoff>(entry.offset));
			if (!file_.read(reinterpret_cast<char*>(payload_.data()), entry.comp_size))
			{
				file_.clear();
				return false;
			}

			if (static_cast<std::uint32_t>(crc32(0, payload_.data(), entry.comp_size)) != entry.crc)
			{
				return false;
			}

			const auto keyframe = (entry.flags & RECORD_KEYFRAME) != 0;
			if (!keyframe && current_.size() != entry.raw_size)
			{
				return false;
			}

			auto& output = keyframe ? current_ : scratch_;
			output.resize(entry.raw_size);

			inflateReset(&inflate_);
			inflate_.next_in = payload_.data();
			inflate_.avail_in = entry.comp_size;
			inflate_.next_out = output.data();
			inflate_.avail_out = entry.raw_size;
			if (inflate(&inflate_, Z_FINISH) != Z_STREAM_END || inflate_.total_out != entry.raw_size)
			{
				return false;
			}

			if (!keyframe)
			{
				for (std::size_t k = 0; k < entry.raw_size; ++k)
				{
					current_[k] ^= scratch_[k];
				}
			}

			++decoded_records_;
			return true;
		}

		void compute_life_events()
		{
			events_.clear();
			if (index_.empty())
			{
				return;
			}

			auto previous = index_.front().summary;
			for (std::size_t i = 1; i < index_.size(); ++i)
			{
				const auto& entry = index_[i];
				const auto& current = entry.summary;

				for (std::uint32_t c = 0; c < MAX_CLIENTS; ++c)
				{
					const auto bit = 1u << c;
					const auto was_alive = (previous.alive_mask & bit) != 0;
					const auto is_alive = (current.alive_mask & bit) != 0;
					const auto is_dead = (current.dead_mask & bit) != 0;
					const auto client = static_cast<int>(c);

					if (!was_alive && is_alive)
					{
						events_.push_back({entry.time, client, life_event::type::spawn});
					}
					else if (was_alive && !is_alive && (is_dead || current.deaths[c] > previous.deaths[c]))
					{
						events_.push_back({entry.time, client, life_event::type::death});
					}

					if ((current.connected_mask & bit) && (previous.connected_mask & bit) && current.kills[c] > previous.kills[c])
					{
						const auto kills = std::min(current.kills[c] - previous.kills[c], 8);
						for (auto k = 0; k < kills; ++k)
						{
							events_.push_back({entry.time, client, life_event::type::kill});
						}
					}
				}

				previous = current;
			}
		}

		std::ifstream file_{};
		std::filesystem::path path_{};
		file_header header_{};
		std::vector<index_entry> index_{};
		std::vector<life_event> events_{};
		std::array<std::vector<death_period>, MAX_CLIENTS> periods_{};
		std::vector<std::int64_t> chain_bad_at_{};
		std::vector<std::uint8_t> current_{};
		std::vector<std::uint8_t> scratch_{};
		std::vector<std::uint8_t> payload_{};
		std::int64_t current_index_{-1};
		std::uint64_t file_size_{};
		std::size_t keyframes_{};
		std::uint64_t decoded_records_{};
		z_stream inflate_{};
		bool inflate_ready_{};
	};

	// ==========================================================================================================
	// view selection (playback, main thread): which record the 1st-person view of the target shows for one cgame
	// snapshot. demo_pov.cpp substitute() applies the choice to the snapshot; the dry run calls the same code.
	// ==========================================================================================================

	// client_header::slot_state of a client in the game (client_t::header.state CS_ACTIVE)
	constexpr std::uint8_t SLOT_ACTIVE = 5;

	// client_header::session_state (gclient sessionState): 0 playing, 1 dead, 2 spectator (also the killcam), 3 intermission
	constexpr std::int32_t SESSION_INTERMISSION = 3;

	// playerState_t fields read here (demo_pov.cpp asserts them against game::mp::playerState_t and its own constants)
	constexpr std::size_t PS_FIELD_PM_TYPE = 0x4;
	constexpr std::size_t PS_FIELD_OTHER_FLAGS = 0x10;
	constexpr std::size_t PS_FIELD_CLIENT_NUM = 0x17C;
	constexpr std::size_t PS_FIELD_KILLCAM_TIME = 0x13CC;
	constexpr std::uint32_t OTHER_FLAGS_FOLLOWING = 0x2000;
	constexpr std::int32_t PM_TYPE_INTERMISSION = 6;

	// ps.stats[3], the spawn count. CG_SetNextSnap 0x14028CA20 resets cgame for a new snapshot (cg.snap->ps = new ps:
	// no interpolation, plus the respawn reset FUN_1402813d0) only when this field or ps.clientNum changes, or, for a
	// playerState with a body ((otherFlags & 0x6000) != 0), when otherFlags bit 0x4000 flips.
	// Demo playback clears 0x4000 on every alive playerState (demo_pov.cpp
	// apply_player_state), so a respawn is reset only through the spawn count or clientNum. After a killcam that is
	// not enough: the killcam follow copy 0x140386550 memcpys the killer's playerState over the victim's, stats
	// included, and the respawn then increments the KILLER's count, which can equal the victim's held one (sample:
	// client 6 holds 3 from 00:36.800 and respawns with 3 at 00:43.750). Without a reset, CG_InterpolatePlayerState
	// 0x140284220 lerps the first-person camera 2404 units across the map from the death spot to the spawn point
	// during that snapshot. So every snapshot of a death that is followed by a hold (its dead run and the hold) goes
	// out with this field marked: dead run -> hold stays identical (no reset, the steady death camera) and the respawn
	// always differs (reset, as at any other respawn). The death itself is reset as before for pm 7 (alive -> pm 7
	// flips 0x4000); a pm 8 death (otherFlags 0x4000 cleared like an alive one) is now reset through the count as well.
	// Deaths without a hold are not marked. Nothing else in cgame reads the field: no reference to
	// cg.predictedPlayerState+0x1D4 (0x14176EDD4), and the readers of stats[0] / stats[2] read fixed fields
	// (checked in the decompiled code).
	constexpr std::size_t PS_FIELD_SPAWN_COUNT = 0x1D4;
	constexpr std::int32_t SPAWN_COUNT_MARK = 0x40000000; // xor: never equal to a real (small, non-negative) count

	inline void mark_spawn_count(std::uint8_t* ps)
	{
		write_as(ps, PS_FIELD_SPAWN_COUNT, read_as<std::int32_t>(ps, PS_FIELD_SPAWN_COUNT) ^ SPAWN_COUNT_MARK);
	}

	// per-client omnvars a death hold takes from the held record: the killcam HUD data, the camera scene names (a new
	// cam_scene_name timestamp starts a camera scene that would replace the dead camera, FUN_1401512D0) and
	// ui_session_state, which stays "dead" (7) instead of "killcam" (62). Every other per-client omnvar comes from the
	// current record, so the target's HUD keeps working during the hold: the round-end / match outcome splash
	// (ui_round_end*, per client [77]-[83]), splashes, challenge messages. On this build the kept ones are per-client
	// [1]-[17] and [92]; in the sample those are the only ones that change
	// during a hold.
	inline bool hold_keeps_omnvar(const std::string_view name)
	{
		return name.starts_with("ui_killcam_") || name == "killcam_scene" || name.starts_with("cam_scene_") || name == "ui_session_state";
	}

	using omnvar_mask = std::array<bool, MAX_OMNVARS>; // by per-client omnvar index

	// per-client omnvar block of a held snapshot: `current` with the entries of `keep` taken from `held` (count entries,
	// same layout)
	inline void merge_hold_omnvars(std::uint8_t* out, const std::uint8_t* current, const std::uint8_t* held, const std::uint32_t count,
	                               const omnvar_mask& keep)
	{
		const auto n = std::min<std::uint32_t>(count, MAX_OMNVARS);
		std::memcpy(out, current, static_cast<std::size_t>(n) * OMNVAR_SIZE);
		for (std::uint32_t i = 0; i < n; ++i)
		{
			if (keep[i])
			{
				std::memcpy(out + static_cast<std::size_t>(i) * OMNVAR_SIZE, held + static_cast<std::size_t>(i) * OMNVAR_SIZE, OMNVAR_SIZE);
			}
		}
	}

	// what the selection needs to know about the demo's own snapshot (the recorder's view). demo_pov.cpp reads it
	// from the cgame snapshot (playerState at offset 0); the dry run from the recorder's sidecar playerState, which
	// is what the server sent the host while the host's archiveTime was 0
	struct recorder_snapshot
	{
		std::int32_t server_time{};
		std::int32_t client_num{};
		std::int32_t pm_type{};
		std::uint32_t other_flags{};
		std::int32_t killcam_time{};
	};

	inline recorder_snapshot read_recorder_snapshot(const std::uint8_t* ps, const std::int32_t server_time)
	{
		recorder_snapshot snap{};
		snap.server_time = server_time;
		snap.client_num = read_as<std::int32_t>(ps, PS_FIELD_CLIENT_NUM);
		snap.pm_type = read_as<std::int32_t>(ps, PS_FIELD_PM_TYPE);
		snap.other_flags = read_as<std::uint32_t>(ps, PS_FIELD_OTHER_FLAGS);
		snap.killcam_time = read_as<std::int32_t>(ps, PS_FIELD_KILLCAM_TIME);
		return snap;
	}

	// the target's own record can be shown: an active slot, its own playerState (not a follow / killcam copy of
	// somebody else's), playing, and alive (or dead when the death camera is on)
	inline bool target_valid(const client_view& pov, const std::int32_t target, const bool allow_dead)
	{
		if (!pov.valid() || pov.header.slot_state != SLOT_ACTIVE || read_as<std::int32_t>(pov.ps, PS_FIELD_CLIENT_NUM) != target)
		{
			return false;
		}

		const auto flags = pov.header.flags;
		if ((flags & (CF_FOLLOWING | CF_KILLCAM)) || !(flags & CF_PLAYING))
		{
			return false;
		}

		return (flags & CF_ALIVE) || (allow_dead && (flags & CF_DEAD));
	}

	// a record that can be held: the client's own dead playerState, as target_valid accepts it with the death camera
	// on. CF_DEAD = otherFlags 0x4000 without 0x2000 and pm_type 7/8; target_valid adds no killcam time
	// (ps+0x13CC == 0), ps.clientNum == client and the active slot
	inline bool dead_record_ok(const client_view& view, const std::int32_t client)
	{
		return target_valid(view, client, true) && (view.header.flags & CF_DEAD) != 0;
	}

	// the target is still in the post-death part of its period in the current record: same active slot, neither
	// alive nor dead (killcam, spectating, waiting to respawn) and not at intermission, where the engine's own view
	// is shown (CG_SetNextSnap 0x14028CA20 starts the intermission UI only when the snapshot's pm_type turns 6)
	inline bool hold_record_ok(const client_view& now)
	{
		if (!now.valid() || now.header.slot_state != SLOT_ACTIVE || !(now.header.flags & CF_CONNECTED) || (now.header.flags & (CF_ALIVE | CF_DEAD)))
		{
			return false;
		}

		return now.header.session_state != SESSION_INTERMISSION && read_as<std::int32_t>(now.ps, PS_FIELD_PM_TYPE) != PM_TYPE_INTERMISSION;
	}

	// what the hold needs from the held record, copied once: the reader keeps a single decoded frame, so decoding the
	// held record again for every snapshot would restart a delta chain twice per snapshot (10-16 ms per snapshot on
	// the sample instead of ~0.2 ms)
	struct held_record
	{
		std::int32_t client{-1};
		std::int64_t record{-1};
		bool failed{}; // not decodable / not a dead record of the client: not retried for every snapshot
		std::int32_t time{};
		std::uint32_t omnvar_global{};
		std::uint32_t omnvar_client{};
		std::vector<std::uint8_t> ps{};      // PS_SIZE bytes
		std::vector<std::uint8_t> omnvars{}; // the client's per-client omnvars, omnvar_client * OMNVAR_SIZE bytes

		[[nodiscard]] bool matches(const std::int32_t c, const std::size_t r) const
		{
			return client == c && record == static_cast<std::int64_t>(r);
		}

		void store(const std::int32_t c, const std::size_t r, const frame_view& frame, const client_view& view)
		{
			// identity last: if an allocation throws, the copy matches nothing and is loaded again
			client = -1;
			record = -1;
			ps.assign(view.ps, view.ps + PS_SIZE);
			omnvars.assign(view.omnvars, view.omnvars + static_cast<std::size_t>(frame.header().omnvar_client) * OMNVAR_SIZE);
			time = frame.header().server_time;
			omnvar_global = frame.header().omnvar_global;
			omnvar_client = frame.header().omnvar_client;
			failed = false;
			client = c;
			record = static_cast<std::int64_t>(r);
		}

		void store_failure(const std::int32_t c, const std::size_t r)
		{
			reset();
			client = c;
			record = static_cast<std::int64_t>(r);
			failed = true;
		}

		void reset()
		{
			client = -1;
			record = -1;
			failed = false;
			time = 0;
			omnvar_global = 0;
			omnvar_client = 0;
			ps.clear();
			omnvars.clear();
		}
	};

	enum class view_kind : std::uint8_t
	{
		recorded, // no substitution: the demo's own snapshot (the recorder's view)
		live,     // the target's own record: alive, or its death camera while dead
		held,     // death hold: the target's held record, the world from the current record
	};

	struct view_choice
	{
		view_kind kind{view_kind::recorded};
		std::size_t record{};         // current record (the world)
		frame_view frame{nullptr, 0}; // decoded current record (valid until the reader decodes again)
		client_view pov{};            // the target in the current record
		const held_record* held{};    // held: the copy of the shown record
		death_period period{};        // held: the period being held
		bool mark_spawn_count{};      // the outgoing ps gets mark_spawn_count(): held, or the dead run of a death with a hold
	};

	class view_selector
	{
	public:
		// forget the held copies (another sidecar)
		void reset()
		{
			for (auto& held : held_)
			{
				held.reset();
			}
		}

		[[nodiscard]] std::uint64_t held_loads() const { return held_loads_; } // held records decoded on a cache miss
		[[nodiscard]] std::uint64_t held_kept() const { return held_kept_; }   // copied while playback passed through them

		// the view of `target` for the snapshot described by `snap`. Same checks, in the same order, as before the
		// death hold; the hold only adds the `held` kind for records that used to fall back to the recorder.
		view_choice choose(reader& sidecar, const std::int32_t target, const bool show_death, const recorder_snapshot& snap,
		                   const std::int32_t max_age)
		{
			view_choice choice{};
			if (target < 0 || static_cast<std::uint32_t>(target) >= MAX_CLIENTS || !sidecar.loaded())
			{
				return choice;
			}

			const auto found = sidecar.find(snap.server_time, max_age);
			if (!found)
			{
				return choice;
			}

			const auto& entry = sidecar.index()[*found];
			if (target >= entry.client_count || target == entry.host_client)
			{
				return choice; // the recorder itself, or a slot the sidecar does not have
			}

			// cheap pre-check before decoding: the target's own record (alive, or dead with the death camera on), or
			// with the death camera on the post-death part of a death period (index summaries, binary search)
			const auto bit = 1u << target;
			const auto own = (entry.summary.alive_mask & bit) || (show_death && (entry.summary.dead_mask & bit));
			const auto* period = !own && show_death ? find_hold(sidecar.death_periods(target), *found) : nullptr;
			if (!own && !period)
			{
				return choice;
			}

			// the recorder's own snapshot must be live: during its killcam / follow periods the demo's entity list comes
			// from the killcam archive (another time) and the ps is a follow copy
			if (snap.killcam_time != 0 || (snap.other_flags & OTHER_FLAGS_FOLLOWING) || snap.client_num != entry.host_client)
			{
				return choice;
			}

			// a hold never covers the intermission: the engine's own view (scoreboard, intermission UI) gets through
			if (period && snap.pm_type == PM_TYPE_INTERMISSION)
			{
				return choice;
			}

			// the held record before the current one: the reader keeps one decoded frame and dead_last < found, so the
			// current record then continues the delta chain forward (or restarts its own chain once). Usually the copy
			// was already kept on the way through the dead run (keep_held_records) and nothing is decoded here.
			const held_record* held{};
			if (period)
			{
				held = load_held(sidecar, target, period->dead_last);
				if (!held)
				{
					return choice;
				}
			}

			const auto* data = sidecar.decode(*found);
			const frame_view frame(data, data ? entry.raw_size : 0);
			if (!frame.valid() || frame.header().server_time != entry.time || frame.header().host_client != entry.host_client)
			{
				return choice;
			}

			keep_held_records(sidecar, frame, *found);

			// SV_BuildClientSnapshot 0x1404824A0 builds the host's snapshot from the killcam archive when the host's
			// archiveTime (gclient+0x333C) is non-zero
			const auto host = frame.client(entry.host_client);
			if (!host.valid() || host.header.archive_time != 0)
			{
				return choice;
			}

			const auto pov = frame.client(target);
			if (period ? !hold_record_ok(pov) : !target_valid(pov, target, show_death))
			{
				return choice;
			}

			choice.kind = period ? view_kind::held : view_kind::live;
			choice.record = *found;
			choice.frame = frame;
			choice.pov = pov;
			choice.held = held;
			if (period)
			{
				choice.period = *period;
			}

			// the respawn after a hold must reset cgame (PS_FIELD_SPAWN_COUNT): the hold and the dead run before it
			choice.mark_spawn_count = period != nullptr
				|| ((pov.header.flags & CF_DEAD) != 0 && find_dead_run(sidecar.death_periods(target), *found) != nullptr);
			return choice;
		}

	private:
		const held_record* load_held(reader& sidecar, const std::int32_t client, const std::size_t record)
		{
			auto& held = held_[static_cast<std::size_t>(client)];
			if (!held.matches(client, record))
			{
				++held_loads_;
				const auto& entry = sidecar.index()[record];
				const auto* data = sidecar.decode(record);
				const frame_view frame(data, data ? entry.raw_size : 0);
				const auto view = frame.client(client);
				if (frame.valid() && frame.header().server_time == entry.time && dead_record_ok(view, client))
				{
					held.store(client, record, frame, view);
				}
				else
				{
					held.store_failure(client, record);
				}
			}

			return !held.failed && held.ps.size() == PS_SIZE ? &held : nullptr;
		}

		// the current record is the held record of every client whose dead run ends in it and is followed by a hold:
		// copy those now (no extra decode), so a later switch to them or a seek into their hold finds the copy
		void keep_held_records(const reader& sidecar, const frame_view& frame, const std::size_t found)
		{
			const auto& entry = sidecar.index()[found];
			for (std::uint32_t c = 0; c < entry.client_count; ++c)
			{
				const auto client = static_cast<std::int32_t>(c);
				if (!((entry.summary.dead_mask >> c) & 1) || held_[c].matches(client, found) || !is_held_record(sidecar.death_periods(client), found))
				{
					continue;
				}

				const auto view = frame.client(client);
				if (dead_record_ok(view, client))
				{
					held_[c].store(client, found, frame, view);
					++held_kept_;
				}
			}
		}

		std::array<held_record, MAX_CLIENTS> held_{}; // one copy per client: ~15 KB each once used
		std::uint64_t held_loads_{};
		std::uint64_t held_kept_{};
	};
}
