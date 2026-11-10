#pragma once

namespace demo_timescale
{
	// iw6-theater (video rendering): while > 0 and a demo is playing, every game frame advances the film by exactly
	// timescale * 1000 / fps milliseconds, independent of how long the frame took to render (0 = real time)
	void set_fixed_fps(double fps);
	[[nodiscard]] double get_fixed_fps();
}
