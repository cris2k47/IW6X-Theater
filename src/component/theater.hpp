#pragma once

#include "theater_math.hpp"

// BO2-style theater (Black Ops II film editor) on top of the demo system.
//
// Camera (F3 / Y):     1ST PERSON -> 3RD PERSON -> FREE CAMERA
// Viewed player:       Mouse1 / Mouse2, LB / RB (1st and 3rd person)
// Free camera modes:   Mouse1 / Mouse2, LB / RB: Free Roam Mode -> Dolly Camera -> Edit Camera Mode
// Markers:             F / R3 in Edit Camera Mode (insert, or edit the highlighted marker); drawn like BO2 by
//                      theater_markers.cpp
// Speed:               Up / Down (0.2x steps), D-pad up / down (0.1x steps), 0.1x .. 6.0x

namespace theater
{
	enum class view_mode
	{
		first_person,
		third_person,
		free_camera,
	};

	enum class free_mode
	{
		free_roam,
		dolly, // "Dolly Camera": the camera runs along the markers
		edit,  // "Edit Camera Mode": place and edit markers
	};

	enum class input_device
	{
		keyboard,
		gamepad,
	};

	// BO2 marker "Speed Mode": the playback speed while the dolly camera runs from this marker to the next
	enum class speed_mode
	{
		manual, // whatever speed the user picked
		linear, // from this marker's speed to the next marker's speed
		fixed,  // this marker's speed
	};

	struct keyframe
	{
		double time{}; // server time in milliseconds
		math::vec3 pos{};
		math::quat rot{};
		double fov{65.0};
		speed_mode mode{speed_mode::manual};
		double speed{1.0};
	};

	struct camera_pose
	{
		math::vec3 pos{};
		math::quat rot{};
		double fov{65.0};
	};

	struct demo_info
	{
		std::filesystem::path path{};
		std::int32_t first_time{};
		std::int32_t last_time{};

		[[nodiscard]] bool has_length() const { return last_time > first_time; }
	};

	enum class popup_kind
	{
		none,
		dolly_notice, // "You have switched to the Dolly Camera, but you have not placed any markers..."
		marker,       // "CAMERA MARKER # N"
		keep_clip,    // after RECORD: Autoname and Save Clip / Name and Save Clip / Preview Clip / Discard Clip
		name_clip,    // text entry for "Name and Save Clip"
	};

	struct popup_state
	{
		popup_kind kind{popup_kind::none};
		int marker{-1};    // marker popup: keyframe index
		int selection{};   // highlighted row
		bool confirm{};    // "Remove All Markers" waits for a second press
		std::string text;  // name_clip input
	};

	// rows of the marker popup (Speed is only shown for Linear / Fixed)
	enum class marker_row
	{
		speed_mode,
		speed,
		reposition,
		remove,
		remove_all,
	};

	// state shared between the theater component and its HUD
	[[nodiscard]] bool active();
	[[nodiscard]] view_mode get_view_mode();
	[[nodiscard]] free_mode get_free_mode();
	[[nodiscard]] const std::vector<keyframe>& get_keyframes();
	[[nodiscard]] const demo_info& get_demo_info();
	[[nodiscard]] double get_smooth_time();
	[[nodiscard]] bool is_paused();
	[[nodiscard]] float get_speed();             // speed without RT, also while paused: the selected one (0.1 - 6.0), or
	                                             // the one a clip replay or a Dolly Camera marker segment sets
	[[nodiscard]] bool speed_label_visible();    // BO2 shows the speed text after Up / Down
	[[nodiscard]] bool fast_forwarding();        // RT held: temporary speed, drawn in yellow
	[[nodiscard]] float fast_forward_speed();
	[[nodiscard]] input_device last_input_device();
	[[nodiscard]] float camera_speed();
	[[nodiscard]] std::optional<camera_pose> evaluate_dolly(double time);
	[[nodiscard]] std::string selector_text();   // viewed player's name, or the free camera mode
	[[nodiscard]] int near_marker();             // Edit Camera Mode: highlighted marker (BO2: within 40 units), -1 if none
	[[nodiscard]] bool repositioning_marker();
	[[nodiscard]] int reposition_index();        // marker being repositioned, -1 if none
	[[nodiscard]] std::uint32_t keyframes_revision(); // changes whenever the markers change (inserted, moved, edited, ...)
	[[nodiscard]] int attach_target();          // Free Roam: player the camera is attached to, -1 if none
	[[nodiscard]] int attach_candidate();       // Free Roam: player under the crosshair that F / R3 attaches to
	[[nodiscard]] bool previewing_clip();       // "Preview Clip" is playing
	[[nodiscard]] bool playing_clip();          // a saved clip is playing (theater_clip)
	[[nodiscard]] bool recording_clip();
	[[nodiscard]] bool can_jump_back();
	[[nodiscard]] bool can_jump_forward();
	[[nodiscard]] const popup_state& get_popup();
	[[nodiscard]] std::vector<marker_row> marker_rows(int marker);
	[[nodiscard]] bool hide_theater_hud();       // a screenshot is being taken or a video rendered
	[[nodiscard]] bool controls_visible();       // F4 / L3 "Toggle Controls"

	// timeline marks for the viewed player (BO2: kills green, deaths red); server times
	struct life_marks
	{
		std::vector<int> kills;
		std::vector<int> deaths;
		int first_spawn{-1};
	};
	[[nodiscard]] life_marks get_life_marks();

	// read the camera actually used for this frame (after the theater override)
	[[nodiscard]] bool get_render_camera(math::vec3& origin, math::vec3 axis[3], float& tan_half_fov_x, float& tan_half_fov_y);
}

namespace theater_hud
{
	void draw();
	void show_message(const std::string& message);
}
