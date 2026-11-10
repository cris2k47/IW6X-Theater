#pragma once

// BO2-style camera markers of Edit Camera Mode (Black Ops II "Film Editor"): a camera at every marker with a
// coloured light cone, "#N" above and the film time "MM:SS" below, the dolly track between the markers and a white
// camera where the Dolly Camera is at the current film time. Details and engine evidence: theater_markers.cpp.

namespace theater::markers
{
	// 3D part (camera models and light cones). Called at the end of the CG_CalcViewValues post-hook, while
	// CG_DrawActiveFrame builds the scene (main thread); the refdef holds the final camera of the frame.
	void draw_scene();

	// screen pixels; w <= 0: none
	struct screen_rect
	{
		float x{};
		float y{};
		float w{};
		float h{};
	};

	// 2D part (track, labels, and the drawn camera on maps without the camera model). Called by the theater HUD
	// from R_EndFrame (main thread) in Edit Camera Mode, after draw_scene of the same frame. The labels stay out of
	// keep_out (the theater panel and its prompt row, drawn on top later).
	void draw_hud(float width, float height, float scale, const screen_rect& keep_out);

	// once per frame at the start of the frame (main thread): frees a camera model that belongs to a map that was
	// loaded again (after syncing with the render thread)
	void update();

	// theater session start / end (demo change): frees the camera model and forgets the assets of the map
	void reset();
}
