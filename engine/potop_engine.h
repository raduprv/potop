// The engine's instance as the engine sees it (potop.h: potop_t): its
// settings, the world, the player's session, the HUD, the camera. A game
// goes through potop.h; the editor, the tools and the tests may reach in
// here (it is open: what is here may change from one version to the next).

#ifndef POTOP_ENGINE_H
#define POTOP_ENGINE_H

#include "potop.h"

#include "camera.h"
#include "console.h"
#include "edit.h"
#include "hud.h"
#include "input.h"
#include "light_probe.h"
#include "senses.h"
#include "ui_backend.h"
#include "ui_style.h"
#include "settings_window.h"
#include "session.h"
#include "settings.h"
#include "soils.h"
#include "world.h"

#define POTOP_TAKE_MAX 16                      // --take: objects a test takes away as it runs
#define POTOP_TAKE_AFTER_SECONDS 30.0          // and this long after the last, the log says how many tiles are built

struct potop_s
{
	settings_t settings;
	soil_settings_t soils;
	world_t world;
	session_t session;
	hud_t hud;
	camera_t camera;
	random_t edit_random;           // the edits' (where plants go, how they turn)

	// A game's application (potop_start; an editor makes its own)
	bool sdl_ready;
	bool console_ready;
	bool gpu_ready;
	bool ui_ready;                 // the HUD can be drawn
	SDL_Window *window;
	ui_backend_t backend;
	ui_style_t style;
	settings_window_t settings_window;
	settings_t file_settings;      // as data/settings.ini has them (Save keeps only what differs, Restore goes back)
	input_state_t input;
	bool mouse_free;               // Tab on foot: the pointer is out for the HUD
	bool player_held;              // potop_player_set_held
	int look_pick_instance;        // Alt: the object named last (-2: Alt not held)
	console_t console;             // ` (or ~): the log and the commands
	bool map_loaded;               // the console loaded a map: the game is told at the next frame
	bool screenshot_requested;
	int bake_paths_frames;             // --bake_paths: frames till the navigation is all built (0: not asked, or done)
	// --take: the objects a test takes away as it runs (where, when; done)
	float take_points[POTOP_TAKE_MAX][2];
	float take_seconds[POTOP_TAKE_MAX];
	int take_count;
	int take_done;
	bool take_told;
	int navmesh_overlay_class;         // F4: the navigation mesh shown, of this class of walkers (by width); -1: not
	char title[64];                // the window's (the loading screen's too)
	const char *stage_name;        // what is being made (the loading screen)
	uint64_t stage_counter;
	uint64_t previous_counter;
	uint64_t next_frame_ns;            // when the next frame may begin (render.max_fps; 0: not known)
	uint64_t game_counter;             // when the game's part of the frame began (the frame profile's FRAME_GAME)
	float frame_seconds;
	// A test without the picture (potop_options_t.sim_seconds)
	float sim_seconds;
	float sim_step;
	double sim_elapsed;
	// The player on foot: how it goes, its steps' noise, how visible
	int gait;                      // potop_gait_t
	float step_meters_walked;      // since its last step
	potop_stealth_t stealth;
	bool stealth_set;              // (else the defaults)
};

// Who came in and went out of the areas this frame (the game's frame:
// potop_frame_end; the editor has no one to tell)
void potop_update_areas(potop_t *potop);
// The player's gait and its steps' noises, after its walk
void potop_update_steps(potop_t *potop, float seconds);

#endif

