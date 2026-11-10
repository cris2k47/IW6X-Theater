#pragma once

#include "theater_math.hpp"

// Read-only access to the client game state the theater needs to look at other players during demo
// playback (iw6mp64_ship.exe 3.15). Everything here is what the engine itself uses to draw other players:
// centities, characterinfo (interpolated view angles), clientinfo (names) and their animated models.
// Addresses: see theater_world.cpp.

namespace theater::world
{
	struct player
	{
		int client{-1};
		std::string name;
		int team{}; // 1 axis, 2 allies, 3 spectator
	};

	// connected players from the current snapshot (spectators excluded), ordered by client number
	[[nodiscard]] std::vector<player> players();

	// the client slot that recorded the demo, and the client whose view the recorded snapshot shows
	[[nodiscard]] int recorder_client();
	[[nodiscard]] int recorded_view_client();

	[[nodiscard]] std::string client_name(int client);

	// the player's entity is in the current snapshot, alive and drawable
	[[nodiscard]] bool alive(int client);

	// interpolated world position of the player's feet, and of the eyes (tag_eye, or a stance based estimate)
	[[nodiscard]] std::optional<math::vec3> origin(int client);
	[[nodiscard]] std::optional<math::vec3> eye(int client);

	// position of the player's corpse while dead (ET_PLAYER_CORPSE owned by the client)
	[[nodiscard]] std::optional<math::vec3> corpse(int client);

	// interpolated view angles (pitch, yaw, roll) the engine uses to animate the player
	[[nodiscard]] math::vec3 view_angles(int client);

	// third person camera collision: point where a small box moving from 'from' to 'to' stops
	[[nodiscard]] math::vec3 trace(const math::vec3& from, const math::vec3& to, int pass_entity);

	// hides one player's model (1st person view from inside another player); -1 = none
	void set_hidden_player(int client);

	// draws the recorder's own body even though the recorded view is first person (free / 3rd person cameras)
	void set_force_local_body(bool force);
}
