# IW6X Theater: the Black Ops 2 theater for Call of Duty: Ghosts (IW6x / CBServers)

An add-on for the **CBServers IW6x client** (iw6-mod) that brings the **Black Ops 2** theater mode to Ghosts, with the
same keyboard and controller controls as the BO2 theater on PC (Plutonium T6):

- **Record** matches to demo files (`.dm_iw6`), online matches included.
- **Play them back** with the BO2 panel: pause (the sound pauses too, like in BO2), speed from 0.1x to 6.0x (fixed 0.1x slow motion for cinematics), jumps to the 10 s marks and fast forward with RT.
- **Watch any player** (bots included) in **1st person** or **3rd person**, like BO2's `◄ name ►` selector.
- **Free camera** with BO2's three modes: **Free Roam** (with "Attach To Object"), **Edit Camera Mode** (markers drawn like in BO2: cameras with their colored cone, number, time and path) and **Dolly Camera**.
- **RECORD**: records clips with the camera you are using and, when you save them, turns them into **MP4 videos** in `Videos\IW6X Theater`, at the fps you choose and perfectly smooth.
- A **THEATER** entry in the main menu with the list of your demos and a **Settings** menu (game recording, videos,
  free camera). Like in BO2, **GAME RECORDING** (on by default) is also in the private match lobby.

The demo system (recording and playback) is **Anomaly's (Caball009)**, published under GPL-3.0 at
[git.alterware.dev/anomaly/iw6-mod](https://git.alterware.dev/anomaly/iw6-mod). Here it is ported to a DLL that loads
into the official CBServers client. The cameras, the dolly, the HUD and the menu are new.

---

## Installation

Download the ZIP from [Releases](https://github.com/cris2k47/IW6X-Theater/releases) and extract it into the game folder
(`ghosts_game_files`, the one with `iw6mp64_ship.exe`). It puts each file in its place:

| File | Destination |
|---|---|
| `XINPUT1_3.dll` | `ghosts_game_files\XINPUT1_3.dll` |
| `iw6\ui_scripts\theater\__init__.lua` (`lui\theater\__init__.lua` in the source code) | `ghosts_game_files\iw6\ui_scripts\theater\__init__.lua` |

From the source code, `build.bat install` builds the DLL and copies both files.

Then start the game **as usual from the CBServers launcher**. In the multiplayer menu you will see **THEATER** below **MODS**.
The console (`~` key) shows `[theater] iw6-theater loaded`.

**How it works:** the game loads `XINPUT1_3.dll` from its own folder. The DLL forwards the controller calls to the real
Windows XInput, waits until the game is decrypted and the CB client has installed its hooks, and then enables the theater.
It does not modify `iw6x.exe` or the `data\` folder: the launcher rewrites both.

**To uninstall:** delete `XINPUT1_3.dll`, the `iw6\ui_scripts\theater` folder and, if you installed from the ZIP, its
three `IW6X-Theater-*` files (README, license and third-party notices). Your demos, cameras and clips stay in `demos\`
and `theater\`.

> If Windows Smart App Control is on, it will block the DLL because it is not signed.

---

## 1. Recording

**GAME RECORDING** is on by default, like in BO2: every match you play is recorded automatically, on any server, not
only in private matches. You can switch it off or on in main menu → **THEATER** → **Settings** and at the bottom of
the **PRIVATE MATCH** lobby. It is the console's `demoautorecord` and it is saved in your config, so
if you used an older version of the theater, your config keeps the OFF it had then: switch it on once in Settings.
Automatic demos pile up in `demos\client\auto\`: delete the ones you do not want.

To record only some matches, switch GAME RECORDING off and use the console (`~`) during a match:

| Command | What it does |
|---|---|
| `demostart` | starts recording with an automatic name |
| `demostart myname` | records as `myname` (add `overwrite` to overwrite it) |
| `demostop` | stops and saves |
| `set demoautorecord 0` / `1` | GAME RECORDING off / on (records **every** match **automatically**, like BO2; on by default). Saved in your config |

You can run `demostart` in the middle of a match: the demo contains the whole match from the moment you joined.

Demos are saved to:
- `ghosts_game_files\demos\client\user\` (the ones you record)
- `ghosts_game_files\demos\client\auto\` (the automatic ones)

In private matches that you host, you can also record another real player's point of view from the server.
Type `set sv_demos 1` **before** they join, then use `sv_demostart <num|name>` and `sv_demostop <num|name>`.
Those demos go to `demos\server\`. Bots cannot be recorded this way.

## 2. Opening the theater

- Main menu → **THEATER** → pick a demo, or
- Console: `theater_list` (lists the demos with their length) and `theater_play <name>`.

The map loads without a server and the demo starts playing. The theater's timeline appears at the bottom.

### Theater settings

In the THEATER menu, **Settings** (X on the controller, `F2` on the keyboard, or click it) opens **THEATER SETTINGS**.
While you watch a film, the game menu (`Esc` / Start) also has **THEATER SETTINGS**, below OPTIONS, so you can change
the free camera or the videos without leaving the film. Up / Down choose an option, Left / Right change it (A, `Enter`
or a click also switch the ON / OFF ones) and B / `Esc` goes back. Every change is saved in your config at once and
applies right away.

| Option | Console | What it does |
|---|---|---|
| GAME RECORDING | `demoautorecord` | records every match automatically (default ON) |
| RECORD ALL PLAYERS | `theater_pov_record` | as the host, also saves every player's view, see *Watching other players* (default ON) |
| SAVE VIDEO WITH CLIPS | `theater_video_on_save` | makes the MP4 when you save a clip (default ON) |
| VIDEO FRAME RATE | `theater_video_fps` | 24 to 240 fps (default 60) |
| VIDEO QUALITY | `theater_video_quality` | LOW, MEDIUM, HIGH (default), ULTRA or MAX. The bitrate follows your window size and the frame rate, and the menu shows it, for example HIGH (26 MBPS) at 1600×900 and 60 fps |
| FREE CAMERA SPEED | `theater_freecam_speed` | 50 to 5000 (default 250) |
| FREE CAMERA SENSITIVITY | `theater_mouse_scale` | mouse sensitivity of the free camera, 0.25x to 3x (default 1x) |

A value typed in the console shows as it is, and Left / Right then jump to the nearest value of the list. The free
camera speed you change while watching (mouse wheel, keys) lasts until you close the game; the one in Settings is the
saved one.

## 3. Controls (the same as in Black Ops 2)

The panel copies the BO2 theater's ("MODE: Film Editor"), measured in Plutonium T6:

```
   F4 Toggle Controls          F Insert Camera Marker          F2 Toggle Hud
  +-------------------------------------------------------------------------+
  |          (M1) StarBerry (M2)                    MODE: Film Editor        |
  |                     01:19                                               |
  |   [====|====|====|===v=|====|====|====|====|====|====|====]   02:08     |
  |   [ 1ST PERSON ] [ |<  [>  ||  >>  >| ] [  RECORD  ] [ SCREENSHOT ]     |
  |        F3          <-   v  SPACE  ^  ->       R           Hold R        |
  +-------------------------------------------------------------------------+
```

- At the top: **F4 Toggle Controls** (hides the whole panel) and **F2 Toggle Hud** (hides the game HUD: in third person
  and in the free camera, the players' names). With the HUD hidden, `Esc` / Start still open the game menu: the HUD
  shows while the menu is open and hides again when you close it. Like in BO2, Ghosts' "SPECTATING" text never shows,
  even if the player who recorded the demo was spectating. In **Edit Camera Mode** the center shows
  **F Insert Camera Marker** / **F Edit Camera Marker** (with a highlighted marker, see section 4), and in Free Roam
  **F Attach To Object**.
- Top row: the **player you are watching** between the left and right click icons (in the free camera, the mode:
  *Free Roam Mode*, *Dolly Camera* or *Edit Camera Mode*) and **MODE: Film Editor**.
- Timeline: the current time above the marker, marks every 10 s and the total length. If the demo has every player's
  view (see below), the **kills in green** and the **deaths in red** of the player you are watching are shown. In the
  free camera, the **camera markers in yellow** are shown.
- Buttons: the camera (`1ST PERSON` / `3RD PERSON` / `FREE CAMERA`), playback (when you change the speed with `↑` / `↓`
  or the D-pad, the speed replaces the slow motion icon below 1x and the fast forward icon from 1x: the selected one or,
  in a *Linear* / *Fixed* segment of the Dolly Camera or while a clip plays, the one in effect at that moment; while you
  hold RT its speed shows in yellow and, when you release it, the previous one shows again), `RECORD` (red while
  recording) and `SCREENSHOT`. Below them, the key or button for each one: with a controller, D-pad ↓ and ↑ sit on both
  sides of A, like `↓` and `↑` on both sides of `SPACE` on the keyboard.

The prompts switch automatically between **keyboard** and **controller** depending on what you used last; with a
controller, the game's own button icons are shown.

**Sound while paused (like in BO2):** pausing stops all the match's sound (gunfire, voices, music and ambience) and, when
you resume, every sound carries on exactly where it stopped. Menu sounds keep working. Using RT from pause plays the sound
at the fast forward speed. If you jump or switch players while the film is paused, sounds do not pile up: on resume you
only hear the new position. At the end of the demo, while it stands still, nothing plays either.

### Keyboard and mouse (BO2 on PC)

| Key | Action |
|---|---|
| `F4` | Toggle Controls: show or hide the panel |
| `F2` | Toggle Hud: show or hide the game HUD (the players' names, among other things) |
| `F3` | camera: **1ST PERSON → 3RD PERSON → FREE CAMERA** |
| Left / right click | previous / next player (in the free camera: previous / next mode) |
| `Space` | pause / play (at the end of the demo it starts over) |
| `↓` / `↑` | speed in 0.2x steps like in BO2, but down to 0.1x: 0.1x, 0.2x, 0.4x, 0.6x … 6.0x. Also while paused (applied on resume) and while recording |
| `←` / `→` | previous / next 10 s mark (forward up to the end of the recording) |
| `R` | RECORD: start / stop a clip |
| Hold `R` | SCREENSHOT: a capture without the panel in `Pictures\IW6X Theater` |
| `F` | Free Roam: Attach To Object (follow the player in front of you) / Detach. Edit Camera Mode: insert a marker, or edit the highlighted marker (see section 4) |
| `W A S D` + mouse | move and look with the free camera (it flies where you look) |
| `E` / `Q` | up / down |
| Mouse (3RD PERSON) | orbit the camera around the player |
| `Esc` | game menu |

Extras that BO2 does not have (they do not get in the way of the BO2 controls):

| Key | Action |
|---|---|
| `Shift` / `Ctrl` | faster / slower free camera |
| Mouse wheel | free camera zoom (FOV) |
| `Z` / `X` | tilt the free camera (roll); `C` resets it together with the zoom |
| `PgUp` / `PgDn` | free camera speed |
| `Shift` + `←` / `→` | −30 s / +30 s |
| `Home` | go to the start |
| `J` / `L` | previous / next marker |
| `Del` | delete the highlighted marker or, if there is none, the closest one in time (Edit Camera Mode) |

### Controller (the BO2 theater's "DEFAULT" layout)

| Button | Action |
|---|---|
| **L3** | Toggle Controls |
| **B** | Toggle HUD |
| **Y** | camera: 1ST PERSON → 3RD PERSON → FREE CAMERA |
| **LB** / **RB** | previous / next player (in the free camera: previous / next mode) |
| **A** | pause / play |
| **LT** | previous 10 s mark |
| **RT** (pressure sensitive) | speed while held: a light pull = slow motion, fully pulled = 6.0x (also from pause); when released, the film goes back to its previous speed (the one selected with the D-pad or `↑` / `↓`) or to pause. Opening a menu or the console, or unplugging the controller, counts as releasing it |
| **D-pad ↓** / **↑** | permanent speed −0.1x / +0.1x (from 0.1x to 6.0x, like BO2's controller layout 2); holding it repeats. Also while paused (applied on resume) and while recording |
| **D-pad →** | next 10 s mark |
| **X** | RECORD; **hold X**: SCREENSHOT |
| **R3** | Free Roam: Attach To Object / Detach. Edit Camera Mode: insert or edit a marker |
| **Left / right stick** | move / look (free camera); the right stick orbits the camera in 3RD PERSON |
| **Back** | game scoreboard |
| **Start** | game menu |

Free camera extras: **hold D-pad ←** and move the **right stick**: up / down = zoom (FOV), left / right = tilt (roll);
while you hold it, the right stick does not turn the camera. **A short tap on D-pad ←** (without moving the stick) resets
the zoom and the tilt. Outside the free camera, D-pad ← does nothing.

While the theater is on screen, the game does not receive the buttons the theater uses (except Start and Back).
PlayStation controllers work if they show up as XInput (Steam Input or DS4Windows); the buttons are shown with Xbox letters.

### Popups (like in BO2)

- **NOTICE**: when switching to *Dolly Camera* without markers ("ENTER / A Accept").
- **CAMERA MARKER # N** (F / R3 with a highlighted marker): *Speed Mode* (Manual / Linear / Fixed), *Speed* (for Linear
  and Fixed, from 0.1x to 6.0x), *Reposition Camera* (move the camera and press F / R3 to place it), *Remove Marker*,
  *Remove All Markers*. The mouse / LB-RB switch to the previous / next marker; `Esc` / B = *Done*. Keyboard: `↑` `↓`
  to choose, `←` `→` to change, `Enter` to accept. With a controller, inside the popups the D-pad chooses and changes the
  options (not the speed).
- **Keep Clip** (when RECORD ends): *Autoname and Save Clip*, *Name and Save Clip*, *Preview Clip*, *Discard Clip*
  (discarding goes back to where the clip started). Saving the clip also **records the video** (see section 5).

### Watching other players

- **3RD PERSON** follows any player or bot from behind, with collision against walls. If they are dead, the camera
  stays on their body until they respawn.
- **1ST PERSON** of another player:
  - in demos recorded **as the host** of the match (a private match or one with bots), a `<demo>.dm_iw6.pov` file is
    recorded next to the demo with the state of **every** player: then the 1st person view is the real one, with their
    weapon, their HUD, their ammo and their kills and deaths on the timeline, just like in BO2 (the HUD and the ammo do
    not show if the recording player was spectating: see *Known limitations*). If the player you are watching dies,
    their death camera stays (like in BO2) until they respawn, even if in the match they were watching the kill-cam or
    spectating: after the first second the camera stays still next to their body, looking toward their killer, while the
    match goes on (`pov_death 0` turns this off);
  - in other demos (for example recorded as a client on a server) the camera sits in the player's eyes, looking where
    they look, but without their weapon or HUD: the demo only stores that data for the recording player.
- **3RD PERSON** and the free camera show the world as it was recorded, also with the `.pov` file; the player's weapon
  and HUD are seen in **1ST PERSON**.
- `theater_list` marks the demos that have that file with `[all players]`. In the THEATER menu they show `[ALL PLAYERS]`.

## 4. Making a dolly shot (like in BO2)

1. Go to the start of the shot (`←` / `→`, or `theater_goto <seconds>`) and pause with `Space` if you want.
2. `F3` until **FREE CAMERA** and right / left click (LB / RB) until **Edit Camera Mode**.
3. Place the camera and press `F` (R3): **Insert Camera Marker**. A yellow mark appears on the timeline and, in the
   world, a camera like BO2's at that point, looking where you were looking (see below).
4. Move forward in time, reposition the camera and press `F` again. Repeat. BO2 does not allow two markers at the same instant.
5. With a **highlighted** marker, `F` opens **CAMERA MARKER # N** to change the speed of that segment (*Linear* /
   *Fixed*), reposition it or delete it. Like in BO2, the highlighted marker is the first one (in time order) that is
   less than 40 units from the camera and in front of it (or at most 15 units behind it); the prompt at the top then
   says **Edit Camera Marker**.
6. Switch to **Dolly Camera**, go back before the first marker and press `Space`: the camera goes through each marker
   exactly at its instant, with the speed of each segment.
7. For the final video: turn on **RECORD** (`R`) at the start, press `R` again at the end and save the clip: the theater
   replays it by itself and records it to `Videos\IW6X Theater`. Hide the game HUD with `F2` first if you don't want it.
   For a fixed slow motion (for example a 0.1x cinematic), lower the speed with `↓` or D-pad ↓ before recording, or use
   0.1x *Fixed* segments on the markers: the clip and the video keep the speed of every moment.

**How the markers look (like in BO2, only in Edit Camera Mode):**

- Each marker has a **camera** looking where the camera was looking when the marker was placed (without the tilt), with
  a translucent **light cone** coming out of the lens: **green** for the first marker, **yellow** for the ones in between
  and **red** for the last one (with a single marker, green). The highlighted marker's cone is brighter and a bit wider.
- Above each camera is **#N** (its number in time order) and below it its **time** in the film (`MM:SS`). The texts get
  smaller with the distance (from 100 units on) and disappear beyond 400 units, near the edges of the screen, where they
  would cover the panel or its prompts, while a popup is open and while you reposition a marker. The highlighted
  marker's texts are yellow.
- With two markers or more, the dolly **path** is drawn in orange, on top of everything (like in BO2), plus a **white
  camera** at the point where the Dolly Camera will be at the film's current instant (if it is between the first and
  the last marker).
- The camera is the game's own security camera model, which only exists in the maps that include it (`mp_prisonbreak`,
  `mp_boneyard_ns`, `mp_ca_behemoth`, `mp_ca_rumble`, `mp_dome_ns`, `mp_flooded`, `mp_lonestar`, `mp_shipment_ns`,
  `mp_sovereign`, `mp_zebra`, `mp_zerosub` and three Extinction maps). On the other maps a simple camera is drawn (a gray
  box with a dark lens). The cone is the same everywhere.
- So that they do not block the view, a camera (and its cone) is not drawn while its body is less than 16 units from
  your camera (for example right after placing a marker or jumping to it, and while you move away from it), nor the cone
  of a camera while your camera is inside it. None of this shows in SCREENSHOT captures or in videos.

Markers are saved automatically per demo in `ghosts_game_files\theater\cameras\<demo>.cam`, and clips in
`ghosts_game_files\theater\clips\`.

**Interpolation:** position and FOV use non-uniform Catmull-Rom with the marker times as knots (the Barry-Goldman
formulation). Rotation uses quaternions with the same scheme and `slerp`, so there is no gimbal lock and roll is free.

## 5. Videos

When you save a clip in **Keep Clip** (*Autoname and Save Clip* or *Name and Save Clip*), the theater turns it into a video:

1. It goes back to the start of the clip, hides the theater panel and plays the clip with the same camera and the same
   speeds you recorded it with.
2. Every frame is saved to an **MP4 (H.264)** in **`Videos\IW6X Theater`** (your Windows Videos folder), named after
   the clip. If a video with that name already exists, it is saved as `_2`, `_3`... A thin progress bar at the top does
   not appear in the video. `Esc` (B on the controller) cancels it and deletes the partial file. Leaving the demo while
   it records (disconnecting, going back to the menu) also cancels it at once.
3. When it finishes, *video saved* shows and the film stays paused at the end of the clip.

**Frames per second:** the video is not recorded in real time. For every frame the film advances exactly 1/fps of a
second, however long your PC takes to draw and save it. That is why the video is **always perfectly smooth**, even if the
game runs slowly while it records (it just takes longer). The default is **60 fps**. Use 120 or more to slow it down
later in an editor, or 30 if you want smaller files.

The fps, the quality and whether clips make a video are also in THEATER → **Settings**.

| Setting | What it does |
|---|---|
| `set theater_video_fps <n>` | fps of the videos (10–240, default 60; saved) |
| `set theater_video_quality <0-4>` | quality preset: 0 LOW, 1 MEDIUM, 2 HIGH (default), 3 ULTRA, 4 MAX. The bitrate is width × height × fps × 0.1 / 0.2 / 0.3 / 0.5 / 0.8 bits (HIGH: about 26 Mbps at 1600×900 and 60 fps, 37 at 1080p), between 8 and 150 Mbps; saved |
| `set theater_video_bitrate <Mbps>` | a fixed bitrate instead of the preset (8–150; Settings shows it as CUSTOM). `0` (default) uses the preset; saved |
| `set theater_video_on_save 0` | save clips without making the video |
| `theater_render <clip>` | make (or remake, for example with other fps) the video of a saved clip |
| `theater_render_cancel` | cancel the video in progress |
| `theater_video_test <s> [fps]` | test: records the screen in real time for `s` seconds (to check that videos work on your PC) |
| `theater_video_stop` / `theater_video_cancel` | stop (and save) / cancel that test |

- The video has the resolution of the game window (for example 1600×900). For 1080p videos, set the game to 1920×1080
  before recording.
- What you see is what you get: with or without the game HUD (`F2`), in 1st or 3rd person, free camera or dolly.
- Videos have **no sound**: the game's sound cannot be recorded frame by frame. While the video is recorded the game is
  muted and runs as fast as your PC allows (a 4 s clip takes less than a second with a current graphics card). When it
  finishes, the sound and your fps limit come back; your saved volume and `com_maxfps` are not touched.
- The graphics card's H.264 encoder is used (NVIDIA, AMD or Intel) and, if there is none, the Windows one. Windows "N"
  editions need the *Media Feature Pack*; without it the theater warns that it cannot make videos, and clips are still saved.

## 6. Console commands

| Command | What it does |
|---|---|
| `theater_help` | list of controls |
| `theater_list` | available demos with their length (`[all players]` = has every player's view) |
| `theater_play <name>` | play a demo (looped) |
| `theater_cam <1st\|3rd\|free\|dolly\|edit>` | camera |
| `theater_player [n]` | without a number: list of players; with a number: watch that player (`-1` = the recorded view) |
| `theater_speed <x>` | speed from 0.1 to 6 (for example `0.5`) |
| `theater_pause` | pause / play |
| `theater_seek <s>` / `theater_goto <s>` | jump `s` seconds / go to second `s` |
| `theater_markers` | list of camera markers |
| `theater_clips` / `theater_clip <name>` | list of saved clips / play one |
| `theater_screenshot [name]` | a capture without the panel |
| `theater_freecam_speed <n>` | free camera speed in units/s (default 250; saved) |
| `theater_mouse_scale <n>` | mouse sensitivity in the free camera (saved) |
| `pov_info` | state of the file with every player's view |
| `pov_death <0\|1>` | `1` (what the theater sets): in 1ST PERSON the player's death camera shows until they respawn; `0`: while they are dead, the 1st person view is as without the `.pov` file (the camera still at the last view, the recording player's HUD) |
| `theater_soundstate` | state of the sound pause (useful if you report a sound bug) |
| `set theater_pov_record 0` | do not write the `.pov` file when recording as the host (it takes about 2-5 MB per minute; saved) |

Anomaly's original commands are still available: `demoplay`, `demoforward`, `demorewind`, `demoreplay` and `demoinstantplay`.

---

## Known limitations

- **A client demo only stores the full view of the recording player.** Ghosts sends every player the position and
  animation of everybody else (not culled by visibility), so the free camera and 3rd person see everyone everywhere on the
  map; what is missing for the others is their 1st person view (weapon, HUD), which only the `.pov` file has.
- **1st person of other players without the `.pov` file.** In demos you did not record as the host, another player's
  1st person view is a camera in their eyes: there is no weapon or HUD, because the demo does not contain them.
- **No preview window (PIP) in Edit Camera Mode.** BO2 draws what the dolly will see in the top left corner; here you
  check it by switching to *Dolly Camera*.
- **The host's team in matches with bots.** In local matches the game does not let you pick a team from the menu if the
  match is started from the console; the recording player ends up spectating. You still see everyone else, but the game
  HUD stays in spectator mode: in 1ST PERSON you see the player's weapon (with the `.pov` file), but not their minimap,
  their ammo or the score, which BO2 does show, so there F2 / B only hides the names.
- **Kill-cams and spectator periods** of the original match show as they are in the recorded view, in 3RD PERSON and
  in the free camera. In another player's 1ST PERSON (with the `.pov` file) their death camera shows instead, except
  during the final kill-cam, if they disconnect or if the recording player was in their own kill-cam: there the 1st
  person view is as without the `.pov` file (the camera in their eyes, or still at the last view if they are dead).
- **Rewinding** restarts the demo internally and fast-forwards to the requested point. In very long demos it takes a
  moment, and some effects or sounds may hang for an instant (a limitation inherited from the original system).
- **Sound while paused:** if, with the film paused, you use `demorewind` or `demoforward` directly in the console (instead
  of the theater controls, `theater_seek` or `theater_goto`), sounds from the previous point may be heard for a moment on
  resume. Looping sounds (ambience, engines) are kept during the pause, so after a jump while paused the loop of
  something that is no longer at the new point may be heard for a fraction of a second on resume.
- **Extreme slow motion** (0.1x) is less smooth: the engine advances the film time in whole milliseconds, so characters
  move in small steps (in a 60 fps video the film advances 1 or 2 ms per frame). The dolly camera is perfectly smooth
  because it uses fractional time.
- **Edit Camera Mode markers:** the path, the texts and, on maps without the camera model, the drawn cameras show on top
  of everything, also through walls (BO2 draws the path that way too). The cones share a maximum of 48 per frame with the
  game's lasers and tracers: the theater uses at most 24, those of the closest cameras.
- **Your stats are protected.** When playback starts, a copy of your local stats is saved, writes are blocked while it
  lasts and the copy is restored when you leave. This way the demo cannot add XP or unlocks from the recorded match.
- **The video's edges are not frame-exact.** The engine draws every frame one or two frames late, so the video may start
  or end 1–2 frames (a few hundredths of a second) before or after the clip.
- **If the game closes while a video is being saved**, an unplayable `.mp4.part` file is left in `Videos\IW6X Theater`:
  delete it and make the video again with `theater_render <clip>`.
- **Tested encoders:** NVIDIA's and Windows'. AMD's and Intel's have not been tested; if one rejects the configuration,
  the Windows one is used automatically.
- **Game version:** the addresses are those of `iw6mp64_ship.exe` 3.15, the version CBServers pins. If the executable
  changes, the add-on does not activate and logs it to `theater\theater.log`.

## Files

```
ghosts_game_files\
  XINPUT1_3.dll                        the add-on
  iw6\ui_scripts\theater\__init__.lua  THEATER menu, its Settings and the lobby's GAME RECORDING
  demos\client\user|auto\*.dm_iw6      demos
  theater\cameras\*.cam                camera markers per demo
  demos\client\user|auto\*.dm_iw6.pov  every player's view (demos recorded as the host)
  theater\clips\*.clip                 RECORD clips (the camera of every moment)
  theater\theater.log                  the add-on's log (useful if something goes wrong)

Videos\IW6X Theater\*.mp4              clip videos (your Windows user's Videos folder)
Pictures\IW6X Theater\*.png            SCREENSHOT captures (your Windows user's Pictures folder)
```

## Building from source

Requirements: Visual Studio 2022 Build Tools (C++ x64, Windows SDK) and premake5. `build.bat` uses `tools\premake5.exe`,
or `..\upstream\cbservers-iw6-mod\tools\premake5.exe` when there is none in `tools\`.

```
build.bat          builds build\bin\x64\Release\XINPUT1_3.dll
build.bat install  builds it and copies the DLL and the menu to the game folder
```

`build.bat install` copies to `..\..\ghosts_game_files`; change `GAME_DIR` in `build.bat` if your game is somewhere else.

Code layout:

- `src\boot.cpp`: startup. Waits until the game is decrypted and CB has installed its hooks.
- `src\dllmain.cpp`: the XInput proxy.
- `src\component\demo_*`: Anomaly's demo system (GPL-3.0).
- `src\component\demo_pov*`: the `.pov` file with every player's view (recording, reading and the snapshot substitution
  in 1ST PERSON), including the death camera that is held until the player respawns. `demo_pov_format.hpp` does not
  depend on the game, so the same logic can be tested outside of it.
- `src\component\theater*.cpp`: camera modes, dolly, input, HUD, stats protection and the sound pause.
- `src\component\theater_markers.cpp`: the Edit Camera Mode markers, like in BO2. The camera model is added to the scene
  with its own DObj (without taking any of the game's entity slots). Each camera is drawn with its own free entity
  number, because the engine only draws one model per number. The cones are effect beams (`FX_Beam_Add`) split into
  segments so the glow stays centered. Both are added from the `CG_CalcViewValues` hook. The path, the texts and the
  drawn camera are 2D in the HUD.
- `src\component\theater_capture.cpp`: the only `IDXGISwapChain::Present` hook, shared by captures and videos. It is
  installed the first time one of them is used, or at the first backward jump. A backward jump restarts the demo
  internally and runs it up to the requested point: meanwhile the hook does not present those frames (one or two from
  the start of the demo would show), so the screen keeps the last frame until the demo gets there.
- `src\component\theater_video.cpp`: MP4 videos. It copies every presented frame (without the panel), converts it to
  NV12 BT.709 and encodes it with Media Foundation on another thread. Media Foundation is loaded when it is used, so the
  DLL does not depend on it.
- `src\component\demo_timescale.cpp`: besides Anomaly's `demotimescale`, the fixed clock of the videos (1/fps per frame).
- `src\component\devtools.cpp`: development tools. Only enabled if `theater\dev\enable` exists.

Engine addresses used by the theater (`iw6mp64_ship.exe` 3.15):

| Item | Address |
|---|---|
| `CG_CalcViewValues` | `0x14029F980` |
| death camera (`CG_OffsetThirdPersonView`: placed behind body `0x2A + ps.corpseIndex`, looking toward `ps.stats[1]`) | `0x14029B7B0` |
| `CG_SetNextSnap` (resets the view without interpolation when `ps.stats[3]` or `ps.clientNum` change) | `0x14028CA20` |
| `CG_CalcFov` | `0x140299220` |
| `CG_GetViewFov` | `0x14029A630` |
| `FOV → tangents` | `0x1404E2A80` |
| `cg.refdef` | `0x1417E04C8` (`vieworg +0x18`, `viewaxis +0x24`, `tanHalfFov +0x10`) |
| `cg.refdefViewAngles` | `0x141818690` |
| `cgs.viewAspect` | `0x14187EB90` |
| `LiveStorage_StatsWriteNeeded` | `0x1404090E0` |
| persistent stats block of controller 0 | `0x14459E874` (size `0x20468`) |
| `Com_TimeScaleMsec` (one call per frame) | `0x140415D50` |
| master sound volume (`fade * snd_volume * scale`) / its update | `0x14984AB5C` / `0x140660ED0` |
| sound pause: the read of `cl_paused` inside `SND_Update` (`0x1406602F0`), redirected while the film is paused | `0x1406603A1` |
| `SND_PauseSounds` / `SND_UnpauseSounds` / `SND_StopVoice` | `0x1406598F0` / `0x140660260` / `0x140491530` |
| engine swap chain | `0x1480B1D70` |
| `Com_WriteConfiguration` (saves each dvar's *latched* value) | `0x140416100` |
| `SEH_StringEd_GetString` (the text of a localization key; the LUI only resolves `LUA_MENU_SPECTATING_CAPS` when the map's HUD is built). While a demo plays, the theater's hook returns `""` for that key. It sits on top of the hook CB's `iw6x.exe` already installs (iw6-mod's `localized_strings` component, the one behind `game:addlocalizedstring`), so it wins even if CB changes that key's text | `0x1404A5F60` |
| `DObjCreate` / `DObjFree` (a DObj in the theater's own memory, like the UI character models) | `0x140505920` / `0x140505E90` |
| the shared "no duplicate parts" string that `DObjCreate` leaves in `DObj+0x08` without a reference (the engine replaces it on every map load; cleared from `+0x08` before `DObjFree`) | `0x147AD26BC` |
| `R_AddDObjToScene` (rigid model; render flags `0x800`, no shadow) | `0x140607F70` |
| entnum → this frame's scene model (rigid / animated; `0xFFFF` = none) and the entities linked into the world, per local client. The engine draws only one model per entnum, so each camera uses a free, unlinked one, from `0x7FE` down | `0x149029388` / `0x149029390` / `0x148034DA8` |
| rigid scene models: count / `0x80`-byte entries (`+0x34` scale, `+0x54` the previous frame's scale, `+0x60` radius) | `0x148FFB124` / `0x148FFB130` |
| `R_SyncRenderThread` (before the DObj is freed) | `0x140602D30` |
| `FX_Beam_Add` (effect beam; 48 per frame, shared with lasers and tracers; `+0x78` segment count) | `0x140334100` |
| `R_AddCmdDrawQuadPic` (any 2D quad) | `0x140600A60` |

## License and credits

GPL-3.0 (see `LICENSE`; `IW6X-Theater-LICENSE.txt` in the release ZIP). Includes code from **iw6-mod** (X Labs /
AlterWare / CBServers) and **Anomaly's (Caball009) demo system**. The DLL also includes MinHook (BSD-2) and zlib (zlib):
their licenses are in `THIRD-PARTY-NOTICES.txt` (`IW6X-Theater-THIRD-PARTY-NOTICES.txt` in the ZIP).
