#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "theater_world.hpp"

#include "game/game.hpp"
#include "utils/hook.hpp"

// Engine data for following other players (iw6mp64_ship.exe 3.15). Evidence:
// (Ghidra decompiles of CG_DrawActiveFrame 0x140299760, CG_Player 0x14027E2B0, CG_UpdatePlayerAngles 0x140280200,
// CG_ShouldDrawLocalPlayer 0x1402803C0, CG_Trace 0x1402B7580 -> 0x1402B7260, CG_DObjGetWorldTagPos 0x140263F50).

namespace theater::world
{
	using namespace math;

	namespace
	{
		// cg_t (cg array at 0x14176EC00)
		constexpr std::uintptr_t CG_PS = 0x14176EC00;                // playerState_t cg.ps
		constexpr std::uintptr_t CG_PS_PM_TYPE = CG_PS + 0x4;
		constexpr std::uintptr_t CG_PS_OTHER_FLAGS = CG_PS + 0x10;   // 0x4000 playing client, 0x2000 following
		constexpr std::uintptr_t CG_PS_CLIENT_NUM = CG_PS + 0x17C;   // 0x14176ED7C
		constexpr std::uintptr_t CG_PS_VIEW_ANGLES = CG_PS + 0x184;  // interpolated by CG_InterpolatePlayerState (0x140284220)
		constexpr std::uintptr_t CG_CLIENT_NUM = 0x141771F60;        // the local client's own slot
		constexpr std::uintptr_t CG_LOCAL_PLAYER_CENT = 0x141771F28; // centity_t* cg.localPlayerCent
		constexpr std::uintptr_t CG_SNAP = 0x141771F88;              // snapshot_t* cg.snap
		constexpr std::uintptr_t CG_NEXT_SNAP = 0x141771F90;         // snapshot_t* cg.nextSnap

		// snapshot_t
		constexpr std::uintptr_t SNAP_NUM_ENTITIES = 0x3C80;
		constexpr std::uintptr_t SNAP_NUM_CLIENTS = 0x3C84;
		constexpr std::uintptr_t SNAP_ENTITIES = 0x3C88; // entityState_t[768], 0x108 each
		constexpr std::uintptr_t SNAP_ENTITY_SIZE = 0x108;
		constexpr std::uintptr_t SNAP_CLIENTS = 0x35488; // clientState_t[18], 0xD8 each
		constexpr std::uintptr_t SNAP_CLIENT_SIZE = 0xD8;

		// centity_t centities[2048] at 0x141887350, 0x238 each
		constexpr std::uintptr_t CENTITIES = 0x141887350;
		constexpr std::uintptr_t CENTITY_SIZE = 0x238;
		constexpr std::uintptr_t CENT_ORIGIN = 0x18;        // pose.origin (interpolated)
		constexpr std::uintptr_t CENT_NUMBER = 0xF8;        // nextState.number
		constexpr std::uintptr_t CENT_ETYPE = 0xFC;         // nextState.eType
		constexpr std::uintptr_t CENT_EFLAGS = 0x100;       // nextState.lerp.eFlags
		constexpr std::uintptr_t CENT_FLAGS = 0x200;        // bit 0: in the newest snapshot

		// entityState_t
		constexpr std::uintptr_t ENT_NUMBER = 0x0;
		constexpr std::uintptr_t ENT_ETYPE = 0x4;
		constexpr std::uintptr_t ENT_EFLAGS = 0x8;
		constexpr std::uintptr_t ENT_ORIGIN = 0x18;          // lerp.pos.trBase
		constexpr std::uintptr_t ENT_CLIENT_NUM = 0x90;      // owner client (corpses)

		// characterInfo_t characterinfo[] at 0x141869AB8, 0x5D8 each: +0xC team, +0x460 view angles
		constexpr std::uintptr_t CHARACTERINFO = 0x141869AB8;
		constexpr std::uintptr_t CHARACTERINFO_SIZE = 0x5D8;
		constexpr std::uintptr_t CI_VIEW_ANGLES = 0x460;

		// clientInfo_t clientinfo[18] at 0x141879028, 0x70 each: +0x4 name
		constexpr std::uintptr_t CLIENTINFO = 0x141879028;
		constexpr std::uintptr_t CLIENTINFO_SIZE = 0x70;

		constexpr std::uintptr_t SCR_CONST_TAG_EYE = 0x14450CDB4;

		constexpr int ET_PLAYER = 1;
		constexpr int ET_PLAYER_CORPSE = 2;
		constexpr std::uint32_t EF_CROUCHING = 0x4;
		constexpr std::uint32_t EF_PRONE = 0x8;
		constexpr std::uint32_t EF_NODRAW = 0x20;
		constexpr std::uint32_t EF_DEAD = 0x40000;

		constexpr int MAX_CLIENTS = 18;

		using com_get_client_dobj_t = void*(*)(int entnum, int local_client_num);
		using cg_dobj_get_world_tag_pos_t = int(*)(const void* cent, void* obj, unsigned int tag, float* out);
		using cg_trace_t = void(*)(void* trace, const float* start, const float* end, const float* bounds, int pass_entity,
		                           int content_mask, std::uint8_t unknown7, char unknown8, void* unknown9);

		const auto com_get_client_dobj = reinterpret_cast<com_get_client_dobj_t>(0x140416490);
		const auto cg_dobj_get_world_tag_pos = reinterpret_cast<cg_dobj_get_world_tag_pos_t>(0x140263F50);
		const auto cg_trace = reinterpret_cast<cg_trace_t>(0x1402B7580);

		utils::hook::detour should_draw_player_dobj_hook; // 0x14014F100 int(int lc, int entnum): 0 = skip R_AddDObjToScene
		utils::hook::detour should_draw_local_player_hook; // 0x1402803C0 int(int lc)
		utils::hook::detour add_local_player_entity_hook;  // 0x14027FA80 void(int lc)
		// 0x1402A1230 int CG_CalcRenderingThirdPerson(int lc, cg_t* cg): reads cg.nextSnap->ps (+0x3390) and returns 1 only when
		// dead / remote eyes / linked / killcam / camera path / following; camera_thirdPerson plays no part. Its result
		// (cg.renderingThirdPerson 0x1417E04AC, written in CG_DrawActiveFrame) decides whether CG_Player draws the local player.
		utils::hook::detour calc_rendering_third_person_hook;

		std::atomic<int> hidden_player{-1};
		std::atomic<bool> force_local_body{false};

		template <typename T>
		T read(const std::uintptr_t address)
		{
			return *reinterpret_cast<const T*>(address);
		}

		std::uintptr_t snapshot()
		{
			auto snap = read<std::uintptr_t>(CG_NEXT_SNAP);
			if (!snap)
			{
				snap = read<std::uintptr_t>(CG_SNAP);
			}

			return snap;
		}

		std::uintptr_t centity(const int entnum)
		{
			return CENTITIES + static_cast<std::uintptr_t>(entnum) * CENTITY_SIZE;
		}

		bool valid_client(const int client)
		{
			return client >= 0 && client < MAX_CLIENTS;
		}

		int should_draw_player_dobj_stub(const int local_client_num, const int entnum)
		{
			if (entnum >= 0 && entnum == hidden_player.load())
			{
				return 0;
			}

			return should_draw_player_dobj_hook.invoke<int>(local_client_num, entnum);
		}

		bool local_body_drawable(const int local_client_num)
		{
			const auto client = read<int>(CG_PS_CLIENT_NUM);
			const auto pm_type = read<int>(CG_PS_PM_TYPE);
			const auto cent = read<std::uintptr_t>(CG_LOCAL_PLAYER_CENT);
			if (!valid_client(client) || !cent || pm_type >= 5) // spectator, intermission, dead
			{
				return false;
			}

			if (read<std::uint32_t>(cent + CENT_EFLAGS) & (EF_DEAD | EF_NODRAW))
			{
				return false;
			}

			return com_get_client_dobj(client, local_client_num) != nullptr;
		}

		int should_draw_local_player_stub(const int local_client_num)
		{
			if (force_local_body && local_body_drawable(local_client_num))
			{
				return 1;
			}

			return should_draw_local_player_hook.invoke<int>(local_client_num);
		}

		int calc_rendering_third_person_stub(const int local_client_num, void* cg)
		{
			if (force_local_body && local_body_drawable(local_client_num))
			{
				return 1;
			}

			return calc_rendering_third_person_hook.invoke<int>(local_client_num, cg);
		}

		void add_local_player_entity_stub(const int local_client_num)
		{
			if (!force_local_body)
			{
				add_local_player_entity_hook.invoke<void>(local_client_num);
				return;
			}

			// the demo baseline clears the "playing client" bit while the recorder is alive; CG_Player needs it
			// to draw the local player through its own branch
			auto* other_flags = reinterpret_cast<std::uint32_t*>(CG_PS_OTHER_FLAGS);
			const auto saved = *other_flags;
			*other_flags |= 0x4000;
			add_local_player_entity_hook.invoke<void>(local_client_num);
			*other_flags = saved;
		}
	}

	std::vector<player> players()
	{
		std::vector<player> result;
		const auto snap = snapshot();
		if (!snap)
		{
			return result;
		}

		const auto count = std::clamp(read<int>(snap + SNAP_NUM_CLIENTS), 0, MAX_CLIENTS);
		for (auto i = 0; i < count; ++i)
		{
			const auto cs = snap + SNAP_CLIENTS + static_cast<std::uintptr_t>(i) * SNAP_CLIENT_SIZE;
			const auto client = read<int>(cs);
			const auto team = read<int>(cs + 4);
			if (!valid_client(client) || team == 3)
			{
				continue;
			}

			char name[17]{};
			std::memcpy(name, reinterpret_cast<const char*>(cs + 0x40), 16);
			result.push_back({client, name, team});
		}

		std::ranges::sort(result, {}, &player::client);
		return result;
	}

	int recorder_client()
	{
		const auto client = read<int>(CG_CLIENT_NUM);
		return valid_client(client) ? client : read<int>(CG_PS_CLIENT_NUM);
	}

	int recorded_view_client()
	{
		return read<int>(CG_PS_CLIENT_NUM);
	}

	std::string client_name(const int client)
	{
		if (!valid_client(client))
		{
			return {};
		}

		for (const auto& p : players())
		{
			if (p.client == client)
			{
				return p.name;
			}
		}

		char name[17]{};
		std::memcpy(name, reinterpret_cast<const char*>(CLIENTINFO + static_cast<std::uintptr_t>(client) * CLIENTINFO_SIZE + 4), 16);
		return name;
	}

	bool alive(const int client)
	{
		if (!valid_client(client))
		{
			return false;
		}

		const auto cent = centity(client);
		if (!(read<std::uint32_t>(cent + CENT_FLAGS) & 1) || read<int>(cent + CENT_NUMBER) != client ||
			read<int>(cent + CENT_ETYPE) != ET_PLAYER)
		{
			return false;
		}

		if (read<std::uint32_t>(cent + CENT_EFLAGS) & (EF_DEAD | EF_NODRAW))
		{
			return false;
		}

		return com_get_client_dobj(client, 0) != nullptr;
	}

	std::optional<vec3> origin(const int client)
	{
		if (!valid_client(client))
		{
			return {};
		}

		// the player whose view the snapshot shows is the local player: its centity is rebuilt from cg.ps every
		// frame (CG_UpdatePredictedPlayerEntity) and is not part of the snapshot entity list
		if (client == read<int>(CG_PS_CLIENT_NUM))
		{
			const auto local = read<std::uintptr_t>(CG_LOCAL_PLAYER_CENT);
			if (local)
			{
				const auto* o = reinterpret_cast<const float*>(local + CENT_ORIGIN);
				return vec3{o[0], o[1], o[2]};
			}
		}

		const auto cent = centity(client);
		if (!(read<std::uint32_t>(cent + CENT_FLAGS) & 1) || read<int>(cent + CENT_NUMBER) != client)
		{
			return {};
		}

		const auto* o = reinterpret_cast<const float*>(cent + CENT_ORIGIN);
		return vec3{o[0], o[1], o[2]};
	}

	std::optional<vec3> eye(const int client)
	{
		const auto feet = origin(client);
		if (!feet)
		{
			return {};
		}

		auto cent = centity(client);
		if (client == read<int>(CG_PS_CLIENT_NUM) && read<std::uintptr_t>(CG_LOCAL_PLAYER_CENT))
		{
			cent = read<std::uintptr_t>(CG_LOCAL_PLAYER_CENT);
		}

		if (auto* obj = com_get_client_dobj(client, 0))
		{
			float out[3]{};
			if (cg_dobj_get_world_tag_pos(reinterpret_cast<const void*>(cent), obj, read<std::uint32_t>(SCR_CONST_TAG_EYE), out))
			{
				const vec3 tag{out[0], out[1], out[2]};
				if (length(tag - *feet) < 120.0) // sanity: the eye is close to the body
				{
					return tag;
				}
			}
		}

		const auto flags = read<std::uint32_t>(cent + CENT_EFLAGS);
		const auto height = (flags & EF_PRONE) ? 11.0 : (flags & EF_CROUCHING) ? 40.0 : 60.0;
		return *feet + vec3{0.0, 0.0, height};
	}

	std::optional<vec3> corpse(const int client)
	{
		const auto snap = snapshot();
		if (!snap || !valid_client(client))
		{
			return {};
		}

		const auto count = std::clamp(read<int>(snap + SNAP_NUM_ENTITIES), 0, 768);
		for (auto i = 0; i < count; ++i)
		{
			const auto es = snap + SNAP_ENTITIES + static_cast<std::uintptr_t>(i) * SNAP_ENTITY_SIZE;
			if (read<int>(es + ENT_ETYPE) == ET_PLAYER_CORPSE && read<int>(es + ENT_CLIENT_NUM) == client)
			{
				const auto number = read<int>(es + ENT_NUMBER);
				if (number > 0 && number < 2048)
				{
					const auto* o = reinterpret_cast<const float*>(centity(number) + CENT_ORIGIN);
					return vec3{o[0], o[1], o[2]};
				}

				const auto* o = reinterpret_cast<const float*>(es + ENT_ORIGIN);
				return vec3{o[0], o[1], o[2]};
			}
		}

		return {};
	}

	vec3 view_angles(const int client)
	{
		if (!valid_client(client))
		{
			return {};
		}

		// the local player (the recorder, or the player the sidecar view shows): its playerState has the view
		if (client == read<int>(CG_PS_CLIENT_NUM))
		{
			const auto* a = reinterpret_cast<const float*>(CG_PS_VIEW_ANGLES);
			return {a[0], a[1], a[2]};
		}

		const auto* a = reinterpret_cast<const float*>(CHARACTERINFO + static_cast<std::uintptr_t>(client) * CHARACTERINFO_SIZE + CI_VIEW_ANGLES);
		return {a[0], a[1], a[2]};
	}

	vec3 trace(const vec3& from, const vec3& to, const int pass_entity)
	{
		// trace_t: fraction at +0, "start solid" byte at +0x27 (the engine's 3rd person camera FUN_14029BEC0 checks both)
		const auto run = [pass_entity](const vec3& a, const vec3& b, bool& start_solid)
		{
			alignas(16) std::uint8_t result[0x100]{};
			const float start[3]{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)};
			const float end[3]{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)};
			const float bounds[6]{0.0f, 0.0f, 0.0f, 4.0f, 4.0f, 4.0f}; // midpoint, half size

			cg_trace(result, start, end, bounds, pass_entity, 0x2814011, 0, 0, nullptr);

			start_solid = result[0x27] != 0;
			return std::clamp(static_cast<double>(*reinterpret_cast<float*>(result)), 0.0, 1.0);
		};

		auto start_solid = false;
		const auto fraction = run(from, to, start_solid);
		if (!start_solid)
		{
			return from + (to - from) * fraction;
		}

		// the pivot is inside something (often the viewed player's own collision): start a bit further out
		const auto nudged = from + (to - from) * 0.25;
		const auto second = run(nudged, to, start_solid);
		return start_solid ? to : nudged + (to - nudged) * second;
	}

	void set_hidden_player(const int client)
	{
		hidden_player = client;
	}

	void set_force_local_body(const bool force)
	{
		force_local_body = force;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			should_draw_player_dobj_hook.create(0x14014F100, should_draw_player_dobj_stub);
			should_draw_local_player_hook.create(0x1402803C0, should_draw_local_player_stub);
			add_local_player_entity_hook.create(0x14027FA80, add_local_player_entity_stub);
			calc_rendering_third_person_hook.create(0x1402A1230, calc_rendering_third_person_stub);
		}
	};
}

REGISTER_COMPONENT(theater::world::component)
