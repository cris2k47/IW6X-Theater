#pragma once

// demo_pov: BO2-style true first-person view of any player during demo playback.
//
// Works for client demos recorded by the private-match HOST (listen server in the same process, bots
// included): while such a demo is being recorded, a sidecar "<demo>.dm_iw6.pov" with the authoritative
// state of every client is written next to it. During playback the cgame snapshots are rebuilt from the
// sidecar for the selected player (playerState, entity list, per-client omnvars).
//
// All functions must be called from the game's main thread (console commands, scheduler main/renderer).

namespace demo_pov
{
	// sidecar loaded (and validated) for the demo being played
	[[nodiscard]] bool available();

	// -1 = recorder (no substitution). Values outside [-1, 17] select the recorder.
	// The change applies from the next snapshot the game processes (while paused: when playback resumes).
	void set_target(int client_num);
	[[nodiscard]] int get_target();

	// substitution applied to the latest snapshot handed to cgame (also while a death hold shows the target's death camera)
	[[nodiscard]] bool substituting();

	// death hold shown in the latest snapshot: the sidecar record whose dead playerState it shows (one per death and
	// player), -1 when the snapshot is not a death hold
	[[nodiscard]] std::int64_t held_record();

	// from the sidecar at the current playback time (cl.serverTime); false if unknown
	[[nodiscard]] bool client_alive(int client_num);

	struct life_event
	{
		int server_time;
		int client_num;

		enum class type
		{
			spawn,
			death,
			kill,
		} kind;
	};

	// whole demo, for the BO2 timeline (deaths red, kills green); empty if unknown.
	// client_num -1 returns the events of every client. Sorted by time.
	[[nodiscard]] std::vector<life_event> get_life_events(int client_num);

	// true while a sidecar is being written
	[[nodiscard]] bool recording_sidecar();

	// ---- extras

	// client number of the host that recorded the demo (from the sidecar), -1 if unknown
	[[nodiscard]] int get_recorder();

	// client slots that are connected at the current playback time (from the sidecar), ascending
	[[nodiscard]] std::vector<int> get_clients();

	// player name from the current cgame snapshot (empty if unknown)
	[[nodiscard]] std::string get_client_name(int client_num);

	// select the next / previous player (direction > 0 / < 0); cycles recorder -> players -> recorder,
	// preferring players that are alive at the current playback time
	void cycle_target(int direction);

	// also show the target while it is dead (its own death camera) instead of falling back to the recorder. With it on,
	// the death camera also stays, frozen at the last dead frame of that life, through the killcam / spectating period
	// that may follow the death until the target spawns again ("death hold", like BO2). Off by default: dead, killcam
	// and spectating periods then fall back to the recorder.
	void set_show_death(bool enabled);
	[[nodiscard]] bool get_show_death();
}
