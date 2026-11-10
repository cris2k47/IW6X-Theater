#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "theater.hpp"
#include "theater_markers.hpp"

#include "command.hpp"
#include "console.hpp"

#include "game/game.hpp"

// BO2-style dolly camera markers, drawn only in Edit Camera Mode (like BO2) and never in screenshots or video renders.
//
// BO2 (t6mp.exe, reverse engineered): every marker is a camera with a coloured glow (first marker green, last
// red, the others yellow; the highlighted one brighter), "#N" above it and its film time "MM:SS" below it (2D text that
// shrinks with the distance), the dolly track as a 1.5 unit wide orange ribbon drawn over everything, and a white
// camera where the dolly is at the current film time. Its picture-in-picture preview is deliberately not copied.
//
// IW6 (iw6mp64_ship.exe 3.15; model, scale and beams checked in game):
//  - camera model: clk_camera_security through a private DObj and R_AddDObjToScene, depth tested, every camera on its
//    own free entity number (the renderer draws one model per entnum). The model is only in 14 map zones
//    (prisonbreak, boneyard_ns, ca_behemoth, ca_rumble, dome_ns, flooded, lonestar, shipment_ns, sovereign, zebra,
//    zerosub, alien_armory, alien_beacon, alien_last); on the other maps a simple camera is drawn in 2D instead (the
//    always loaded camera-like models do not look like a camera).
//  - cone: one tapered laser-style FX beam per camera (FX_Beam_Add) with gfx_tracer_new, in segments so the glow stays
//    centred, tinted by its vertex colours and faded by the end alpha, depth tested.
//  - track, labels and the drawn camera: 2D (R_AddCmdDrawQuadPic / R_AddCmdDrawText) from the theater HUD, on top.
// draw_scene runs inside CG_DrawActiveFrame, where the scene and beam lists are open (the engine adds the entities
// before and the other players' lasers after it); draw_hud runs in R_EndFrame. Both on the main thread.

namespace theater::markers
{
	using namespace math;

	namespace
	{
		// ------------------------------------------------------------------ engine data (iw6mp64_ship.exe 3.15)

		// DObjCreate builds a DObj in caller memory (0xE8 bytes), like the engine's own UI character models
		// (FUN_14014BF60, entnum field 0x7FE), so no clientObjMap handle is used. DObjFree (0x140505E90) frees the
		// model list (script memory tree, MT_Free 0x14042F0F0; the tree lives as long as the game, map loads do not
		// reset it) and releases the duplicate-parts script string at +0x08 when it is not the engine's current shared
		// one (see free_dobj, which clears the borrowed handle first). It never reads the XModel, so it is safe after
		// the model's zone is gone; the render thread must be done with the DObj.
		constexpr std::uintptr_t DOBJ_CREATE = 0x140505920; // DObj* (DObjModel_s* models, uint32 count, XAnimTree* tree, DObj* out, uint16 entnum)
		constexpr std::uintptr_t DOBJ_FREE = 0x140505E90;   // void (DObj* obj)
		constexpr std::size_t DOBJ_SIZE = 0xE8;

		// DObj +0x08: the script string of its duplicate parts. With one model there are none, and DObjCreate's model
		// setup (FUN_140505A50) stores the engine's shared empty string 0x147AD26BC there WITHOUT taking a reference.
		// The engine releases and creates that shared string again on every map load, vid_restart, disconnect and error
		// cleanup (FUN_140414B70 / FUN_140415BB0 -> 0x140507340 / 0x140506D90), after freeing its own DObjs.
		constexpr std::size_t DOBJ_DUPLICATE_PARTS = 0x08; // uint32 script string
		constexpr std::uintptr_t SHARED_NO_DUPLICATE_PARTS = 0x147AD26BC; // uint32 script string (engine's reference)

		// R_AddDObjToScene: a DObj with one model and no anim tree becomes a rigid scene model (no skeleton). The
		// entry copies the pose's origin and angles but keeps a pointer to the pose for its lighting handle, which the
		// renderer writes later, so every drawn camera has its own persistent pose. Render flag 0x800 keeps it out of
		// the shadow views.
		constexpr std::uintptr_t R_ADD_DOBJ_TO_SCENE = 0x140607F70; // void (DObj*, cpose_t*, uint32 entnum, uint32 renderFlags, const float* lightingOrigin, float materialTime, int hudOutline)
		constexpr std::uintptr_t SCENE_MODEL_COUNT = 0x148FFB124;   // int: rigid scene models of this frame (R_ClearScene 0x14060A6A0 sets 0)
		constexpr std::uintptr_t SCENE_MODELS = 0x148FFB130;        // rigid scene models, 1024 entries of 0x80 bytes
		constexpr std::size_t SCENE_MODEL_SIZE = 0x80;
		constexpr std::size_t SCENE_MODEL_SCALE = 0x34;      // float, 1 when added
		constexpr std::size_t SCENE_MODEL_PREV_SCALE = 0x54; // float, the previous frame's scale (per-object motion blur), 1 when added
		constexpr std::size_t SCENE_MODEL_RADIUS = 0x60;     // float, the model's radius
		constexpr std::uint32_t CAMERA_RENDER_FLAGS = 0x800;

		// The renderer culls and draws scene models per entity number: R_AddDObjToScene stores the model's index in a
		// per-entnum map (rigid models 0x149029388, animated DObjs 0x149029390; 0xFFFF = none, reset by R_ClearScene
		// FUN_1405C1090) and the cull passes (FUN_1405C1B50, FUN_1405C8240) go through it, so only one model per
		// entnum is drawn (checked in game: with every camera on 0x7FE, as the engine's UI models use it, no camera
		// showed once there were several). Every camera gets its own entnum, from the top of the range down (never
		// used by real entities in practice): one that no model of this frame uses yet and that is not linked into
		// the world cells (R_LinkEntity 0x1405C29B0 sets its bit in the per local client set 0x148034DA8, the scene's
		// local client is 0x149029348). An unlinked entnum is never culled (FUN_1405C1B50). 0x7FF (0x1480B1E30) is
		// reserved for FX models.
		constexpr std::uintptr_t SCENE_ENTITY_COUNT = 0x1480B1E2C;    // uint32: entnums (2048), size of the maps
		constexpr std::uintptr_t SCENE_MODEL_ENTITY_MAP = 0x149029388; // uint16*: entnum -> rigid scene model of this frame
		constexpr std::uintptr_t SCENE_DOBJ_ENTITY_MAP = 0x149029390;  // uint16*: entnum -> animated scene DObj of this frame
		constexpr std::uintptr_t SCENE_LOCAL_CLIENT = 0x149029348;     // uint32
		constexpr std::uintptr_t LINKED_ENTITIES = 0x148034DA8;        // uint32* per local client (2): bit 0x80000000 >> (n & 31) of word n >> 5
		constexpr std::uint32_t FIRST_CAMERA_ENTNUM = 0x7FE;          // ENTITYNUM_WORLD, then downwards
		constexpr std::uint32_t LAST_CAMERA_ENTNUM = 0x600;

		// FX_Beam_Add copies the beam into this frame's list (emptied by FX_Beam_Begin at the start of
		// CG_DrawActiveFrame; 48 beams shared with laser sights and bullet tracers, further ones are dropped). The FX
		// worker turns it into a camera facing quad later in the frame and dereferences the material.
		constexpr std::uintptr_t FX_BEAM_ADD = 0x140334100; // void (const FxBeam*)

		// R_AddCmdDrawQuadPic: any quad into the 2D command list (cmd 0x0E, 0x38 bytes; a NULL material draws $default)
		constexpr std::uintptr_t R_ADD_CMD_DRAW_QUAD_PIC = 0x140600A60; // void (const float verts[4][2], const float* color, Material*)

		struct fx_beam
		{
			std::uint8_t type;        // 1 = laser style: colors[0] at the begin, colors[1] at the end, tapered width
			std::uint8_t pad0[3];
			float begin[3];
			float end[3];
			float colors[5][4];       // RGBA 0..1
			float width;              // half width at the begin
			game::Material* material; // never NULL: the FX worker dereferences it
			std::uint8_t segments;
			std::uint8_t pad1[3];
			float end_width;          // type 1: half width at the end
		};

		static_assert(sizeof(fx_beam) == 0x80);
		static_assert(offsetof(fx_beam, begin) == 0x04);
		static_assert(offsetof(fx_beam, end) == 0x10);
		static_assert(offsetof(fx_beam, colors) == 0x1C);
		static_assert(offsetof(fx_beam, width) == 0x6C);
		static_assert(offsetof(fx_beam, material) == 0x70);
		static_assert(offsetof(fx_beam, segments) == 0x78);
		static_assert(offsetof(fx_beam, end_width) == 0x7C);

		struct dobj_model
		{
			void* model;                   // XModel*
			std::uint32_t bone_name;       // 0 = root
			std::uint8_t ignore_collision;
			std::uint8_t pad[3];
			std::uint64_t zero[3];
		};

		static_assert(sizeof(dobj_model) == 0x28);
		static_assert(offsetof(dobj_model, bone_name) == 0x08);
		static_assert(offsetof(dobj_model, ignore_collision) == 0x0C);

		// cpose_t, only what a rigid scene model uses; everything else stays 0
		struct alignas(16) cpose
		{
			std::uint16_t lighting_handle; // written by the renderer's lighting cache (0x1405F3A80) through the scene entry
			std::uint8_t e_type;           // 0, like the UI models
			std::uint8_t pad0[0x15];
			float origin[3];
			float angles[3];               // pitch, yaw, roll (AnglesToQuat 0x1404E3E70)
			float prev_origin[3];          // = origin: no motion blur
			float prev_angles[3];          // = angles
			std::uint8_t pad1[0x48];
		};

		static_assert(sizeof(cpose) == 0x90);
		static_assert(offsetof(cpose, origin) == 0x18);
		static_assert(offsetof(cpose, angles) == 0x24);
		static_assert(offsetof(cpose, prev_origin) == 0x30);
		static_assert(offsetof(cpose, prev_angles) == 0x3C);

		using dobj_create_t = void* (*)(dobj_model* models, std::uint32_t count, void* tree, void* out, std::uint16_t entnum);
		using dobj_free_t = void (*)(void* obj);
		using add_dobj_t = void (*)(void* obj, cpose* pose, std::uint32_t entnum, std::uint32_t render_flags, const float* lighting_origin,
		                            float material_time, int hud_outline);
		using beam_add_t = void (*)(const fx_beam* beam);
		using quad_pic_t = void (*)(const float (*verts)[2], const float* color, game::Material* material);

		const auto dobj_create = reinterpret_cast<dobj_create_t>(DOBJ_CREATE);
		const auto dobj_free = reinterpret_cast<dobj_free_t>(DOBJ_FREE);
		const auto add_dobj = reinterpret_cast<add_dobj_t>(R_ADD_DOBJ_TO_SCENE);
		const auto beam_add = reinterpret_cast<beam_add_t>(FX_BEAM_ADD);
		const auto quad_pic = reinterpret_cast<quad_pic_t>(R_ADD_CMD_DRAW_QUAD_PIC);

		constexpr auto CAMERA_MODEL = "clk_camera_security";
		constexpr std::array CONE_MATERIALS{"gfx_tracer_new", "gfx_tracer"}; // code_post_gfx_mp (always loaded); _new is brighter

		// ------------------------------------------------------------------ placement and look (theater_dev_marker)

		struct tuning
		{
			float lens_y = 12.0f;  // lens of clk_camera_security in model space (scale 1): the lens axis is model +Y
			float lens_z = 4.0f;
			float tilt = 20.0f;    // the housing points this many degrees down in the model
			float scale = 0.75f;   // about 15 units long, like BO2's camera
			float cone_len = 50.0f;
			float cone_w0 = 1.5f;  // half width at the lens (about the lens)
			float cone_w1 = 15.0f; // half width at the end
			float alpha = 0.85f;   // alpha at the lens (the highlighted marker: 1)
			float clearance = 16.0f; // no camera (nor its cone) whose body is closer to the eye than this
			float segments = 12.0f;  // pieces of the cone's beam (1 .. 32)
			float force_2d = 0.0f;   // not 0: the drawn 2D cameras even where the map has the model (to look at them)
		};

		tuning tune{};

		// ------------------------------------------------------------------ BO2 look

		constexpr float GLOW_GREEN[4]{0.25f, 1.0f, 0.25f, 1.0f};  // first marker (fx_theater_mode_camera_head_glow_grn)
		constexpr float GLOW_YELLOW[4]{1.0f, 0.85f, 0.15f, 1.0f}; // markers in between (_glow_yllw)
		constexpr float GLOW_RED[4]{1.0f, 0.22f, 0.18f, 1.0f};    // last marker (_glow_red)
		constexpr float GLOW_WHITE[4]{1.0f, 1.0f, 1.0f, 1.0f};    // current dolly position (_glow_white)
		constexpr float TRACK_COLOR[4]{250.0f / 255.0f, 148.0f / 255.0f, 28.0f / 255.0f, 127.0f / 255.0f};
		constexpr float LABEL_COLOR[4]{1.0f, 1.0f, 1.0f, 1.0f};
		constexpr float LABEL_HIGHLIGHT_COLOR[4]{1.0f, 1.0f, 0.0f, 1.0f};

		constexpr double HIGHLIGHT_CONE_ALPHA = 1.0;
		constexpr double HIGHLIGHT_CONE_WIDTH = 1.3;
		constexpr double MODEL_BACK_Y = -6.955;          // back of clk_camera_security's housing on the lens axis (model Y, scale 1; bounds)
		constexpr double CONE_EYE_MARGIN = 4.0;          // a cone is not drawn while the eye is inside it or this close to it
		constexpr int MAX_CONES = 24;                    // theater beams per frame, of the 48 shared with lasers and tracers
		constexpr int MAX_MARKER_CAMERAS = 128;          // markers with a camera (BO2 allows 50 markers in all)
		constexpr int CURRENT_SLOT = MAX_MARKER_CAMERAS; // pose of the current dolly position camera

		// track (BO2 FUN_00926400): the dolly path sampled every max(100 ms, 0.2 % of its duration), at most 500 points
		constexpr double TRACK_MIN_STEP = 100.0;
		constexpr double TRACK_STEP_FRACTION = 0.002;
		constexpr int TRACK_MAX_POINTS = 500;
		constexpr double TRACK_WIDTH = 1.5;   // world units (BO2's demo_dollycamTrackWidth 0.75 is a half width)
		constexpr float TRACK_MIN_PX = 1.5f;
		constexpr float TRACK_MAX_PX = 14.0f; // at 900 px screen height
		constexpr double NEAR_PLANE = 4.0;    // 2D geometry is clipped this far in front of the eye before it is projected

		// labels (BO2 demo_dollycamMarkerInformation*: ThreshholdDistance 400, NearDist 100, FarDist 400, FarScale 0.4)
		constexpr double LABEL_MAX_DISTANCE = 400.0;
		constexpr double LABEL_NEAR = 100.0;
		constexpr double LABEL_FAR = 400.0;
		constexpr double LABEL_FAR_SCALE = 0.4;
		constexpr double LABEL_NUMBER_RISE = 4.0; // "#N" baseline above the marker
		constexpr double LABEL_TIME_DROP = 10.0;  // time baseline below the marker: 10 units, 20 at the far distance
		constexpr float LABEL_HEIGHT = 48.0f;     // text height in px at 900 px screen height, full size
		constexpr float LABEL_MARGIN = 84.0f;     // hidden this close to the screen edges (px at 900 px)

		// drawn camera (maps without the model): a 14 x 6 x 6 box housing behind the lens
		constexpr double BOX_LENGTH = 14.0;
		constexpr double BOX_HALF_SIZE = 3.0;
		constexpr double BOX_LENS_HALF_SIZE = 1.7;
		constexpr float BOX_TOP[4]{0.92f, 0.92f, 0.90f, 0.97f};
		constexpr float BOX_ENDS[4]{0.78f, 0.78f, 0.77f, 0.97f};
		constexpr float BOX_SIDES[4]{0.60f, 0.60f, 0.60f, 0.97f};
		constexpr float BOX_BOTTOM[4]{0.42f, 0.42f, 0.42f, 0.97f};
		constexpr float BOX_LENS[4]{0.07f, 0.07f, 0.08f, 0.97f};

		// ------------------------------------------------------------------ state (main thread)

		enum class model_state
		{
			unchecked, // not looked up since the session started
			missing,   // the map has no clk_camera_security: the cameras are drawn in 2D
			ready,     // the DObj is valid
			retired,   // the map was loaded again: the DObj is not used any more, update() frees it
		};

		model_state model = model_state::unchecked;
		void* camera_xmodel = nullptr;
		alignas(16) std::uint8_t dobj_buffer[DOBJ_SIZE]{};
		dobj_model dobj_models[1]{};
		bool dobj_allocated = false; // DObjCreate ran on dobj_buffer and DObjFree did not yet

		bool materials_checked = false;
		game::Material* cone_material = nullptr;


		// one persistent pose per drawn camera (pose slot = marker index), never freed
		std::array<cpose, MAX_MARKER_CAMERAS + 1> poses{};
		std::optional<std::uint32_t> poses_revision;

		// track samples (world), rebuilt when the markers change
		std::array<vec3, TRACK_MAX_POINTS> track{};
		int track_count = 0;
		std::optional<std::uint32_t> track_revision;

		struct camera
		{
			vec3 lens{};      // the marker position
			vec3 axis[3]{};   // forward, left, up of the marker's view without its roll
			double pitch{};
			double yaw{};
			const float* glow{};
			bool highlighted{};
			bool cone{};       // its light cone is drawn (the eye is not inside it)
			int slot{};        // pose
			double distance{}; // from the eye to the lens
		};

		std::array<camera, MAX_MARKER_CAMERAS + 1> cameras{};
		std::array<int, MAX_MARKER_CAMERAS + 1> camera_order{};

		struct track_vertex
		{
			float x;
			float y;
			float half; // half thickness in px
		};

		std::array<track_vertex, TRACK_MAX_POINTS + 2> track_run{};

		// ------------------------------------------------------------------ common

		bool edit_mode()
		{
			return theater::get_view_mode() == theater::view_mode::free_camera && theater::get_free_mode() == theater::free_mode::edit;
		}

		// the rules of the theater HUD: only in Edit Camera Mode, never in screenshots / video renders or under a menu
		bool visible()
		{
			return theater::active() && edit_mode() && !theater::hide_theater_hud() && (*game::keyCatchers & ~1) == 0;
		}

		// the cameras are drawn in 2D (draw_hud) instead of as models (draw_scene)
		bool draw_2d_cameras()
		{
			return model == model_state::missing || tune.force_2d != 0.0f;
		}

		void* find_asset(const game::XAssetType type, const char* name)
		{
			if (!game::DB_XAssetExists(type, name) || game::DB_IsXAssetDefault(type, name))
			{
				return nullptr;
			}

			return game::DB_FindXAssetHeader(type, name, 0).data;
		}

		void free_dobj()
		{
			if (dobj_allocated)
			{
				game::R_SyncRenderThread(); // frames still queued for the render thread may reference the DObj

				// +0x08 is the engine's shared string, only borrowed (DOBJ_DUPLICATE_PARTS). DObjFree releases it whenever it
				// differs from the CURRENT shared string, and our DObj outlives the map loads / disconnects that replace it:
				// released again by us, the old string's refcount would hit a string or memory tree node that belongs to
				// someone else by then (premature frees, or SL_RemoveRefToStringOfSize 0x140431EE0 looping in its hash
				// chain). A one-model DObj never owns a reference, so the handle is cleared and DObjFree skips it.
				*reinterpret_cast<std::uint32_t*>(dobj_buffer + DOBJ_DUPLICATE_PARTS) = 0;
				dobj_free(dobj_buffer);
				dobj_allocated = false;
			}

			std::memset(dobj_buffer, 0, sizeof(dobj_buffer));
			camera_xmodel = nullptr;
		}

		void clear_lighting()
		{
			for (auto& pose : poses)
			{
				pose.lighting_handle = 0;
			}
		}

		// the cone material (once per session) and the camera model of the map. The model belongs to the map's zone:
		// a demo started again from the menu loads the map again without a new theater session, so the model the DObj
		// uses is compared with the loaded one every frame. A map load (or vid_restart) that put the model at the same
		// address usually shows in the engine's shared string, which it replaced: the DObj is made again then too, with
		// fresh lighting handles
		void update_assets()
		{
			if (!materials_checked)
			{
				materials_checked = true;
				for (const auto* name : CONE_MATERIALS)
				{
					cone_material = static_cast<game::Material*>(find_asset(game::ASSET_TYPE_MATERIAL, name));
					if (cone_material)
					{
						break;
					}
				}
			}

			switch (model)
			{
			case model_state::unchecked:
				camera_xmodel = find_asset(game::ASSET_TYPE_XMODEL, CAMERA_MODEL);
				if (!camera_xmodel)
				{
					model = model_state::missing;
					break;
				}

				std::memset(dobj_buffer, 0, sizeof(dobj_buffer));
				dobj_models[0] = {};
				dobj_models[0].model = camera_xmodel;
				dobj_models[0].ignore_collision = 1;
				dobj_create(dobj_models, 1, nullptr, dobj_buffer, static_cast<std::uint16_t>(FIRST_CAMERA_ENTNUM));
				dobj_allocated = true;
				clear_lighting();
				model = model_state::ready;
				break;
			case model_state::ready:
				if (find_asset(game::ASSET_TYPE_XMODEL, CAMERA_MODEL) != camera_xmodel ||
				    *reinterpret_cast<const std::uint32_t*>(SHARED_NO_DUPLICATE_PARTS) != *reinterpret_cast<const std::uint32_t*>(dobj_buffer + DOBJ_DUPLICATE_PARTS))
				{
					model = model_state::retired;
				}
				break;
			default:
				break;
			}
		}

		// a new, removed or moved marker: a pose slot may now belong to another camera, its lighting is looked up again
		void update_poses()
		{
			const auto revision = theater::keyframes_revision();
			if (poses_revision != revision)
			{
				poses_revision = revision;
				clear_lighting();
			}
		}

		// the highlighted marker for the cones: the marker whose popup is open or that is being repositioned (BO2 keeps
		// its selection meanwhile), otherwise the one F / R3 would edit (BO2's 40 unit rule, theater.cpp)
		int highlighted_marker()
		{
			const auto& popup = theater::get_popup();
			if (popup.kind == theater::popup_kind::marker)
			{
				return popup.marker;
			}

			if (theater::repositioning_marker())
			{
				return theater::reposition_index();
			}

			return theater::near_marker();
		}

		// BO2: the first marker in time order green, the last red, the others yellow (a single marker is green)
		const float* glow_color(const int index, const int count)
		{
			return index == 0 ? GLOW_GREEN : index == count - 1 ? GLOW_RED : GLOW_YELLOW;
		}

		double segment_distance(const vec3& point, const vec3& a, const vec3& b)
		{
			const auto ab = b - a;
			const auto length2 = dot(ab, ab);
			const auto t = length2 > 0.0 ? std::clamp(dot(point - a, ab) / length2, 0.0, 1.0) : 0.0;
			return length(point - (a + ab * t));
		}

		// how far the housing reaches behind the lens: clk_camera_security's bounds (about 14.2 units at scale 0.75), or
		// the drawn box
		double body_length()
		{
			return draw_2d_cameras() ? BOX_LENGTH : (static_cast<double>(tune.lens_y) - MODEL_BACK_Y) * static_cast<double>(tune.scale);
		}

		// the eye inside a light cone, or close to it. The cone is a camera-facing beam quad (FX_Beam_GenerateVerts
		// 0x1403341D0: begin / end +- half width * side, in world units, the side across the line to the eye), so from
		// inside it the quad passes right in front of the eye and tints a large part of the view (a short push-in placed
		// from marker #1, a marker looking back at the camera). The cone is left out then.
		bool eye_in_cone(const vec3& eye, const camera& c)
		{
			const auto cone_len = static_cast<double>(tune.cone_len);
			const auto to_eye = eye - c.lens;
			const auto along = dot(to_eye, c.axis[0]);
			if (along < -CONE_EYE_MARGIN || along > cone_len + CONE_EYE_MARGIN)
			{
				return false;
			}

			const auto u = cone_len > 0.0 ? std::clamp(along / cone_len, 0.0, 1.0) : 0.0;
			const auto widen = c.highlighted ? HIGHLIGHT_CONE_WIDTH : 1.0;
			const auto half_width = (tune.cone_w0 + (tune.cone_w1 - tune.cone_w0) * u) * widen;
			return length(to_eye - c.axis[0] * along) < half_width + CONE_EYE_MARGIN;
		}

		// the cameras of this frame: the markers (time order) and BO2's current dolly position camera. A camera whose
		// body is closer to the eye than tune.clearance is left out with its cone: it would fill the view. The body
		// matters, not the lens: the housing reaches about 14 units behind the lens, towards an eye that backs away
		// from a marker it just inserted or jumped to, or that the current position camera drives away from along the
		// dolly (Edit Camera Mode entered from a playing Dolly Camera)
		int collect_cameras(const vec3& eye)
		{
			const auto& keys = theater::get_keyframes();
			const auto n = static_cast<int>(keys.size());
			const auto highlight = highlighted_marker();
			const auto body = body_length();
			auto count = 0;

			const auto add = [&](const vec3& pos, const quat& rot, const float* glow, const bool highlighted, const int slot)
			{
				auto& c = cameras[count];
				double roll{};
				to_angles(rot, c.pitch, c.yaw, roll); // like BO2, the camera ignores the roll of the view
				to_axis(from_angles(c.pitch, c.yaw, 0.0), c.axis);
				c.lens = pos;
				if (segment_distance(eye, c.lens - c.axis[0] * body, c.lens) < static_cast<double>(tune.clearance))
				{
					return; // the next camera reuses the entry
				}

				c.glow = glow;
				c.highlighted = highlighted;
				c.slot = slot;
				c.distance = length(pos - eye);
				c.cone = !eye_in_cone(eye, c);
				++count;
			};

			for (auto i = 0; i < std::min(n, MAX_MARKER_CAMERAS); ++i)
			{
				add(keys[i].pos, keys[i].rot, glow_color(i, n), i == highlight, i);
			}

			// where the Dolly Camera is at the current film time (white, no labels)
			if (n >= 2)
			{
				const auto now = theater::get_smooth_time();
				if (now >= keys.front().time && now <= keys.back().time)
				{
					if (const auto pose = theater::evaluate_dolly(now))
					{
						add(pose->pos, pose->rot, GLOW_WHITE, false, CURRENT_SLOT);
					}
				}
			}

			return count;
		}

		// ------------------------------------------------------------------ 3D (scene)

		void set3(float* out, const vec3& v)
		{
			out[0] = static_cast<float>(v.x);
			out[1] = static_cast<float>(v.y);
			out[2] = static_cast<float>(v.z);
		}

		// the next entity number for a camera model in this frame (SCENE_MODEL_ENTITY_MAP), 0 when none is left
		std::uint32_t next_entnum = FIRST_CAMERA_ENTNUM;

		std::uint32_t take_entnum()
		{
			const auto count = *reinterpret_cast<const std::uint32_t*>(SCENE_ENTITY_COUNT);
			const auto* model_map = *reinterpret_cast<const std::uint16_t* const*>(SCENE_MODEL_ENTITY_MAP);
			const auto* dobj_map = *reinterpret_cast<const std::uint16_t* const*>(SCENE_DOBJ_ENTITY_MAP);
			const auto local_client = std::min(*reinterpret_cast<const std::uint32_t*>(SCENE_LOCAL_CLIENT), 1u);
			const auto* linked = *reinterpret_cast<const std::uint32_t* const*>(LINKED_ENTITIES + local_client * sizeof(void*));
			if (!model_map || !dobj_map)
			{
				return 0;
			}

			while (next_entnum >= LAST_CAMERA_ENTNUM)
			{
				const auto entnum = next_entnum--;
				if (entnum >= count || model_map[entnum] != 0xFFFF || dobj_map[entnum] != 0xFFFF ||
				    (linked && (linked[entnum >> 5] & (0x80000000u >> (entnum & 31))) != 0))
				{
					continue;
				}

				return entnum;
			}

			return 0;
		}

		// clk_camera_security looks along model +Y with the housing tilted down by tune.tilt; its origin is the mount
		// under the housing. Angles (0, yaw - 90, tilt - pitch) turn the lens to the marker's view (roll rotates about
		// the model's X axis, +tilt levels the lens; checked in game), and the model is moved so its lens is at the marker
		void add_model(const camera& c)
		{
			const auto entnum = take_entnum();
			if (!entnum)
			{
				return;
			}

			const vec3 angles{0.0, c.yaw - 90.0, static_cast<double>(tune.tilt) - c.pitch};
			const auto lens = rotate(from_angles(angles.x, angles.y, angles.z), vec3{0.0, tune.lens_y, tune.lens_z} * static_cast<double>(tune.scale));
			const auto origin = c.lens - lens;

			auto& pose = poses[c.slot];
			set3(pose.origin, origin);
			set3(pose.angles, angles);
			set3(pose.prev_origin, origin);
			set3(pose.prev_angles, angles);

			auto* count = reinterpret_cast<std::int32_t*>(SCENE_MODEL_COUNT);
			const auto before = *count;
			add_dobj(dobj_buffer, &pose, entnum, CAMERA_RENDER_FLAGS, pose.origin, 0.0f, 0);

			// the rigid scene model entry it just allocated: scale it (and its radius) around the model origin. The previous
			// frame's scale gets it too, like the engine's own scaled models (R_AddXModelToSceneFx 0x1405C30B0 writes
			// both): left at 1, per-object motion blur (r_mbEnable 2 flags every DObj) would blur the camera as if it shrank
			if (*count == before + 1)
			{
				auto* entry = reinterpret_cast<std::uint8_t*>(SCENE_MODELS) + static_cast<std::size_t>(before) * SCENE_MODEL_SIZE;
				*reinterpret_cast<float*>(entry + SCENE_MODEL_SCALE) = tune.scale;
				*reinterpret_cast<float*>(entry + SCENE_MODEL_PREV_SCALE) = tune.scale;
				*reinterpret_cast<float*>(entry + SCENE_MODEL_RADIUS) *= tune.scale;
			}
		}

		// BO2's glow: a light cone from the lens along the view, fading out; the highlighted marker's is brighter and
		// wider. The beam is a camera-facing strip whose texture is interpolated per triangle, so one strongly tapered
		// quad bends the bright middle of the texture towards one corner (the cone looked like a line slanting away
		// from the view direction, checked in game); split into short segments, each quad is nearly a parallelogram
		// and the glow stays centred on the view direction.
		void add_cone(const camera& c)
		{
			const auto widen = c.highlighted ? HIGHLIGHT_CONE_WIDTH : 1.0;
			const auto alpha = c.highlighted ? HIGHLIGHT_CONE_ALPHA : static_cast<double>(tune.alpha);

			fx_beam beam{};
			beam.type = 1;
			set3(beam.begin, c.lens);
			set3(beam.end, c.lens + c.axis[0] * static_cast<double>(tune.cone_len));
			for (auto i = 0; i < 3; ++i)
			{
				beam.colors[0][i] = c.glow[i];
				beam.colors[1][i] = c.glow[i];
			}

			beam.colors[0][3] = static_cast<float>(alpha);
			beam.colors[1][3] = 0.0f;
			beam.width = static_cast<float>(tune.cone_w0 * widen);
			beam.end_width = static_cast<float>(tune.cone_w1 * widen);
			beam.material = cone_material;
			beam.segments = static_cast<std::uint8_t>(std::clamp(static_cast<int>(tune.segments), 1, 32));
			beam_add(&beam);
		}

		// ------------------------------------------------------------------ 2D (HUD)

		struct view
		{
			vec3 origin{};
			vec3 axis[3]{}; // forward, left, up
			double tan_x{};
			double tan_y{};
			double width{};
			double height{};
		};

		// view space: x = depth along the view, y = left, z = up
		vec3 to_view(const view& v, const vec3& point)
		{
			const auto d = point - v.origin;
			return {dot(d, v.axis[0]), dot(d, v.axis[1]), dot(d, v.axis[2])};
		}

		// a view space point in front of the eye to screen pixels
		void to_screen(const view& v, const vec3& p, float& sx, float& sy)
		{
			sx = static_cast<float>(v.width * 0.5 * (1.0 - p.y / (p.x * v.tan_x)));
			sy = static_cast<float>(v.height * 0.5 * (1.0 - p.z / (p.x * v.tan_y)));
		}

		double pixels_per_unit(const view& v, const double depth)
		{
			return v.height * 0.5 / (depth * v.tan_y);
		}

		game::Material* white()
		{
			static auto* material = game::Material_RegisterHandle("white");
			return material;
		}

		// the theater HUD's font (fonts/smallFont, the game's condensed face)
		game::Font_t* font()
		{
			static auto* f = game::R_RegisterFont("fonts/smallFont");
			return f;
		}

		// every quad gets the winding of the HUD's stretch pics ((x, y) (x + w, y) (x + w, y + h) (x, y + h): positive area
		// with y down), so the track and the drawn cameras look the same whichever way they face, even if the 2D
		// material culled back faces
		void quad(const float (&verts)[4][2], const float* color)
		{
			auto area = 0.0f;
			for (auto i = 0; i < 4; ++i)
			{
				const auto& a = verts[i];
				const auto& b = verts[(i + 1) % 4];
				area += a[0] * b[1] - b[0] * a[1];
			}

			if (area >= 0.0f)
			{
				quad_pic(verts, color, white());
				return;
			}

			const float reversed[4][2]{
				{verts[0][0], verts[0][1]},
				{verts[3][0], verts[3][1]},
				{verts[2][0], verts[2][1]},
				{verts[1][0], verts[1][1]},
			};

			quad_pic(reversed, color, white());
		}

		bool off_screen(const view& v, const float (&verts)[4][2])
		{
			auto min_x = verts[0][0], max_x = verts[0][0], min_y = verts[0][1], max_y = verts[0][1];
			for (const auto& p : verts)
			{
				min_x = std::min(min_x, p[0]);
				max_x = std::max(max_x, p[0]);
				min_y = std::min(min_y, p[1]);
				max_y = std::max(max_y, p[1]);
			}

			return max_x < 0.0f || max_y < 0.0f || min_x > static_cast<float>(v.width) || min_y > static_cast<float>(v.height);
		}

		void update_track(const std::vector<keyframe>& keys)
		{
			const auto revision = theater::keyframes_revision();
			if (track_revision == revision)
			{
				return;
			}

			track_revision = revision;
			track_count = 0;
			if (keys.size() < 2)
			{
				return;
			}

			const auto start = keys.front().time;
			const auto duration = std::max(0.0, keys.back().time - start);
			const auto step = std::max(TRACK_MIN_STEP, duration * TRACK_STEP_FRACTION);
			const auto intervals = std::clamp(static_cast<int>(std::ceil(duration / step)), 1, TRACK_MAX_POINTS - 1);
			for (auto i = 0; i <= intervals; ++i)
			{
				if (const auto pose = theater::evaluate_dolly(start + duration * i / intervals))
				{
					track[track_count++] = pose->pos;
				}
			}
		}

		// one continuous piece of the track. Neighbouring quads share their edges (mitred joints), so the translucent
		// ribbon has no darker overlaps or gaps where the path bends.
		void draw_ribbon(const view& v, const int n)
		{
			if (n < 2)
			{
				return;
			}

			const auto* t = track_run.data();
			const auto normal = [&](const int i, float& nx, float& ny)
			{
				const auto dx = t[i + 1].x - t[i].x;
				const auto dy = t[i + 1].y - t[i].y;
				const auto len = std::sqrt(dx * dx + dy * dy); // >= 0.5 px (draw_track skips closer points)
				nx = -dy / len;
				ny = dx / len;
			};

			const auto offset = [&](const int i, float& ox, float& oy)
			{
				float bx{}, by{};
				normal(i == n - 1 ? i - 1 : i, bx, by);
				if (i == 0 || i == n - 1)
				{
					ox = bx * t[i].half;
					oy = by * t[i].half;
					return;
				}

				float ax{}, ay{};
				normal(i - 1, ax, ay);
				auto mx = ax + bx;
				auto my = ay + by;
				const auto len = std::sqrt(mx * mx + my * my);
				if (len < 1e-3f)
				{
					ox = bx * t[i].half; // the path turns back on itself
					oy = by * t[i].half;
					return;
				}

				mx /= len;
				my /= len;
				const auto miter = 1.0f / std::max(0.5f, mx * bx + my * by); // at most twice the width on sharp bends
				ox = mx * t[i].half * miter;
				oy = my * t[i].half * miter;
			};

			float ax{}, ay{};
			offset(0, ax, ay);
			for (auto i = 0; i + 1 < n; ++i)
			{
				float bx{}, by{};
				offset(i + 1, bx, by);
				const float verts[4][2]{
					{t[i].x + ax, t[i].y + ay},
					{t[i + 1].x + bx, t[i + 1].y + by},
					{t[i + 1].x - bx, t[i + 1].y - by},
					{t[i].x - ax, t[i].y - ay},
				};

				if (!off_screen(v, verts))
				{
					quad(verts, TRACK_COLOR);
				}

				ax = bx;
				ay = by;
			}
		}

		// BO2's track: an orange ribbon 1.5 world units wide (1.5 .. 14 px), drawn over everything. Every segment is
		// clipped against the near plane before it is projected, so the track also passes the eye correctly.
		void draw_track(const view& v, const float scale)
		{
			if (track_count < 2)
			{
				return;
			}

			auto n = 0;
			const auto max_px = TRACK_MAX_PX * scale;
			const auto push = [&](const vec3& p)
			{
				track_vertex vertex{};
				to_screen(v, p, vertex.x, vertex.y);
				vertex.half = std::clamp(static_cast<float>(TRACK_WIDTH * pixels_per_unit(v, p.x)), TRACK_MIN_PX, max_px) * 0.5f;
				if (n > 0)
				{
					// less than half a pixel from the previous point: skipped, so every segment has a direction
					const auto dx = vertex.x - track_run[n - 1].x;
					const auto dy = vertex.y - track_run[n - 1].y;
					if (dx * dx + dy * dy < 0.25f)
					{
						return;
					}
				}

				track_run[n++] = vertex;
			};

			auto previous = to_view(v, track[0]);
			if (previous.x >= NEAR_PLANE)
			{
				push(previous);
			}

			for (auto i = 1; i < track_count; ++i)
			{
				const auto current = to_view(v, track[i]);
				const auto previous_in = previous.x >= NEAR_PLANE;
				const auto current_in = current.x >= NEAR_PLANE;
				if (previous_in != current_in)
				{
					push(previous + (current - previous) * ((NEAR_PLANE - previous.x) / (current.x - previous.x)));
					if (!current_in)
					{
						draw_ribbon(v, n);
						n = 0;
					}
				}

				if (current_in)
				{
					push(current);
				}

				previous = current;
			}

			draw_ribbon(v, n);
		}

		// a camera drawn in 2D where the map has no camera model: a box housing behind the lens with flat shaded faces
		// and a dark lens on the front face, back faces culled
		void draw_box_camera(const view& v, const camera& c)
		{
			const auto& forward = c.axis[0];
			const auto& left = c.axis[1];
			const auto& up = c.axis[2];

			// corner bits: 1 = back, 2 = right, 4 = bottom
			vec3 corner[8]{};
			float sx[8]{}, sy[8]{};
			for (auto i = 0; i < 8; ++i)
			{
				corner[i] = c.lens + forward * ((i & 1) ? -BOX_LENGTH : 0.0) + left * ((i & 2) ? -BOX_HALF_SIZE : BOX_HALF_SIZE) +
					up * ((i & 4) ? -BOX_HALF_SIZE : BOX_HALF_SIZE);

				const auto p = to_view(v, corner[i]);
				if (p.x < NEAR_PLANE)
				{
					return;
				}

				to_screen(v, p, sx[i], sy[i]);
			}

			struct face
			{
				std::array<int, 4> corners;
				vec3 normal;
				const float* color;
				double distance;
			};

			std::array<face, 6> faces{{
				{{0, 2, 6, 4}, forward, BOX_ENDS, 0.0},    // front (lens)
				{{1, 3, 7, 5}, forward * -1.0, BOX_ENDS, 0.0},
				{{0, 1, 5, 4}, left, BOX_SIDES, 0.0},
				{{2, 3, 7, 6}, left * -1.0, BOX_SIDES, 0.0},
				{{0, 1, 3, 2}, up, BOX_TOP, 0.0},
				{{4, 5, 7, 6}, up * -1.0, BOX_BOTTOM, 0.0},
			}};

			std::array<int, 6> visible_faces{};
			auto count = 0;
			for (auto i = 0; i < 6; ++i)
			{
				auto& f = faces[i];
				vec3 centre{};
				for (const auto k : f.corners)
				{
					centre += corner[k] * 0.25;
				}

				if (dot(f.normal, centre - v.origin) < 0.0)
				{
					f.distance = length(centre - v.origin);
					visible_faces[count++] = i;
				}
			}

			std::sort(visible_faces.begin(), visible_faces.begin() + count, [&](const int a, const int b) { return faces[a].distance > faces[b].distance; });
			for (auto i = 0; i < count; ++i)
			{
				const auto& f = faces[visible_faces[i]];
				float verts[4][2]{};
				for (auto k = 0; k < 4; ++k)
				{
					verts[k][0] = sx[f.corners[k]];
					verts[k][1] = sy[f.corners[k]];
				}

				quad(verts, f.color);
			}

			// the lens, when the front face is visible
			if (dot(forward, c.lens - v.origin) < 0.0)
			{
				float verts[4][2]{};
				static constexpr std::array<std::pair<double, double>, 4> square{{{1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}, {1.0, -1.0}}};
				for (auto k = 0; k < 4; ++k)
				{
					const auto point = c.lens + left * (square[k].first * BOX_LENS_HALF_SIZE) + up * (square[k].second * BOX_LENS_HALF_SIZE);
					to_screen(v, to_view(v, point), verts[k][0], verts[k][1]);
				}

				quad(verts, BOX_LENS);
			}
		}

		std::string format_time(const double ms)
		{
			const auto total = static_cast<int>(std::max(0.0, ms) / 1000.0);
			return std::format("{:02}:{:02}", total / 60, total % 60);
		}

		// BO2: centred on the projection of a world point, the baseline there; hidden off screen and near the edges. Also
		// hidden where it would overlap the theater panel or its prompt row (drawn later, on top): a 48 px "#N" would
		// cover "F Edit Camera Marker" or show through the panel
		void label(const view& v, const std::string& str, const vec3& anchor, const float text_scale, const float margin, const screen_rect& keep_out,
		           const float* color)
		{
			const auto p = to_view(v, anchor);
			if (p.x < 1.0)
			{
				return;
			}

			float sx{}, sy{};
			to_screen(v, p, sx, sy);
			if (sx < margin || sy < margin || sx > static_cast<float>(v.width) - margin || sy > static_cast<float>(v.height) - margin)
			{
				return;
			}

			auto* f = font();
			const auto w = static_cast<float>(game::R_TextWidth(str.data(), std::numeric_limits<int>::max(), f)) * text_scale;
			const auto x = sx - w * 0.5f;
			const auto top = sy - static_cast<float>(f->pixelHeight) * text_scale; // the glyph cell above the baseline
			if (keep_out.w > 0.0f && x < keep_out.x + keep_out.w && x + w > keep_out.x && top < keep_out.y + keep_out.h && sy > keep_out.y)
			{
				return;
			}

			game::R_AddCmdDrawText(str.data(), std::numeric_limits<int>::max(), f, x, sy, text_scale, text_scale, 0.0f, color, 6);
		}

		// "#N" above every marker and its film time below it, full size up to 100 units, 0.4 at 400, none further away;
		// yellow for the highlighted marker. BO2 hides them while a menu is open or a marker is being repositioned.
		void draw_labels(const view& v, const std::vector<keyframe>& keys, const screen_rect& keep_out)
		{
			if (theater::get_popup().kind != theater::popup_kind::none || theater::repositioning_marker())
			{
				return;
			}

			const auto* f = font();
			if (!f || f->pixelHeight <= 0)
			{
				return;
			}

			const auto screen = static_cast<float>(v.height / 900.0);
			const auto margin = LABEL_MARGIN * screen;
			const auto first_time = static_cast<double>(theater::get_demo_info().first_time);
			const auto highlight = theater::near_marker();

			for (auto i = 0; i < static_cast<int>(keys.size()); ++i)
			{
				const auto& k = keys[i];
				const auto distance = length(k.pos - v.origin);
				if (distance > LABEL_MAX_DISTANCE)
				{
					continue;
				}

				const auto t = std::clamp((distance - LABEL_NEAR) / (LABEL_FAR - LABEL_NEAR), 0.0, 1.0);
				const auto size = (1.0 - t) + LABEL_FAR_SCALE * t;
				const auto text_scale = static_cast<float>(LABEL_HEIGHT * size * screen / f->pixelHeight);
				const auto* color = i == highlight ? LABEL_HIGHLIGHT_COLOR : LABEL_COLOR;
				label(v, std::format("#{}", i + 1), k.pos + vec3{0.0, 0.0, LABEL_NUMBER_RISE}, text_scale, margin, keep_out, color);
				label(v, format_time(k.time - first_time), k.pos - vec3{0.0, 0.0, LABEL_TIME_DROP * (1.0 + t)}, text_scale, margin, keep_out, color);
			}
		}

		void print_tuning()
		{
			console::info("theater_dev_marker: lens_y %.2f lens_z %.2f tilt %.1f scale %.3f cone_len %.1f cone_w0 %.2f cone_w1 %.2f alpha %.2f clearance %.1f segments %.0f\n",
			              tune.lens_y, tune.lens_z, tune.tilt, tune.scale, tune.cone_len, tune.cone_w0, tune.cone_w1, tune.alpha, tune.clearance, tune.segments);
			static constexpr std::array state_names{"unchecked", "missing (2D cameras)", "ready", "retired"};
			console::info("theater_dev_marker: camera model %s%s, last frame's models on entities 0x%X .. 0x%X\n", state_names[static_cast<int>(model)],
			              tune.force_2d != 0.0f ? ", 2D cameras forced" : "", FIRST_CAMERA_ENTNUM, next_entnum + 1);
		}
	}

	void draw_scene()
	{
		vec3 eye{};
		vec3 axis[3]{};
		float tan_x{}, tan_y{};
		if (!visible() || theater::get_keyframes().empty() || !theater::get_render_camera(eye, axis, tan_x, tan_y))
		{
			return;
		}

		update_assets();
		update_poses();

		const auto count = collect_cameras(eye);
		if (!draw_2d_cameras() && model == model_state::ready)
		{
			next_entnum = FIRST_CAMERA_ENTNUM;
			for (auto i = 0; i < count; ++i)
			{
				add_model(cameras[i]);
			}
		}

		if (!cone_material)
		{
			return; // FX_Beam_Add must never get a NULL material
		}

		// the closest cameras first, never more than MAX_CONES beams (none for an eye inside the cone)
		auto cones = 0;
		for (auto i = 0; i < count; ++i)
		{
			if (cameras[i].cone)
			{
				camera_order[cones++] = i;
			}
		}

		const auto drawn = std::min(cones, MAX_CONES);
		std::partial_sort(camera_order.begin(), camera_order.begin() + drawn, camera_order.begin() + cones,
		                  [](const int a, const int b) { return cameras[a].distance < cameras[b].distance; });
		for (auto i = 0; i < drawn; ++i)
		{
			add_cone(cameras[camera_order[i]]);
		}
	}

	void draw_hud(const float width, const float height, const float scale, const screen_rect& keep_out)
	{
		view v{};
		float tan_x{}, tan_y{};
		if (!visible() || !theater::get_render_camera(v.origin, v.axis, tan_x, tan_y))
		{
			return;
		}

		const auto& keys = theater::get_keyframes();
		if (keys.empty())
		{
			return;
		}

		v.tan_x = tan_x;
		v.tan_y = tan_y;
		v.width = width;
		v.height = height;

		// BO2: the track only with 2 markers or more
		if (keys.size() >= 2)
		{
			update_track(keys);
			draw_track(v, scale);
		}

		// no camera model on this map: the cameras are drawn here, far to near
		if (draw_2d_cameras())
		{
			const auto count = collect_cameras(v.origin);
			for (auto i = 0; i < count; ++i)
			{
				camera_order[i] = i;
			}

			std::sort(camera_order.begin(), camera_order.begin() + count, [](const int a, const int b) { return cameras[a].distance > cameras[b].distance; });
			for (auto i = 0; i < count; ++i)
			{
				draw_box_camera(v, cameras[camera_order[i]]);
			}
		}

		draw_labels(v, keys, keep_out);
	}

	void update()
	{
		if (model == model_state::retired)
		{
			free_dobj();
			clear_lighting();
			model = model_state::unchecked;
		}
	}

	void reset()
	{
		free_dobj();
		clear_lighting(); // the next map has its own lighting cache
		model = model_state::unchecked;
		materials_checked = false;
		cone_material = nullptr;
		poses_revision.reset();
		track_revision.reset();
		track_count = 0;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			// theater_dev_marker: placement and look of the cameras, tuned in game (developer tools only)
			std::error_code ec;
			if (!std::filesystem::exists(console::get_theater_dir() / "dev" / "enable", ec))
			{
				return;
			}

			command::add("theater_dev_marker", [](const command::params& params)
			{
				static constexpr std::array<std::pair<const char*, float tuning::*>, 11> fields{{
					{"lens_y", &tuning::lens_y},
					{"lens_z", &tuning::lens_z},
					{"tilt", &tuning::tilt},
					{"scale", &tuning::scale},
					{"cone_len", &tuning::cone_len},
					{"cone_w0", &tuning::cone_w0},
					{"cone_w1", &tuning::cone_w1},
					{"alpha", &tuning::alpha},
					{"clearance", &tuning::clearance},
					{"segments", &tuning::segments},
					{"force_2d", &tuning::force_2d},
				}};

				if (params.size() >= 3)
				{
					const std::string name = params[1];
					const auto field = std::ranges::find_if(fields, [&](const auto& f) { return name == f.first; });
					if (field == fields.end())
					{
						console::info("usage: theater_dev_marker [lens_y|lens_z|tilt|scale|cone_len|cone_w0|cone_w1|alpha|clearance|segments|force_2d <value>]\n");
						return;
					}

					tune.*(field->second) = static_cast<float>(std::atof(params[2]));
					tune.scale = std::clamp(tune.scale, 0.05f, 10.0f);
					tune.alpha = std::clamp(tune.alpha, 0.0f, 1.0f);
					tune.cone_len = std::max(0.0f, tune.cone_len);
					tune.cone_w0 = std::max(0.0f, tune.cone_w0);
					tune.cone_w1 = std::max(0.0f, tune.cone_w1);
					tune.clearance = std::max(0.0f, tune.clearance);
				}
				else if (params.size() == 2)
				{
					console::info("usage: theater_dev_marker [lens_y|lens_z|tilt|scale|cone_len|cone_w0|cone_w1|alpha|clearance|segments|force_2d <value>]\n");
				}

				print_tuning();
			});
		}
	};
}

REGISTER_COMPONENT(theater::markers::component)
