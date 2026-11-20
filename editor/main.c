// Potop - a sandbox for floods.
//
// Main loop: input, camera, fixed step simulation (terrain tools, water),
// shadows and rendering.
//
// Command line (mostly for automated testing):
//   --seed N           terrain seed (default: random, or settings.ini)
//   --size N           terrain size in cells (multiple of 64)
//   --width N --height N   window size
//   --yaw D --pitch D --distance M   initial camera
//   --first-person     start on foot
//   --go-to X,Y,Z      put the camera there (Y '-' : on the ground; on foot:
//                      the feet), see camera_go_to
//   --look-at X,Y,Z    turn the camera towards that point (after --go-to)
//   --heading YAW,PITCH  turn the camera (degrees, pitch up positive)
//   --debug-view N     0 normal, 1 soil types
//   --brush-test       raise a hill at the map center during the first steps
//   --flood-test       pour water at the map center during the first steps
//   --water-test       run the water mass conservation test (no window) and exit
//   --scene dam        start in the dam scene instead of a generated terrain
//   --dam-test         run the dam scene without a window, check that the dam
//                      washes out and that water and material are conserved
//   --dam-seconds S    simulated seconds for --dam-test (default 180)
//   --screenshot FILE  render --frames frames (default 3), save, quit
//   --warmup S         simulate S seconds before the first frame (for screenshots;
//                      actors sent with --actor-goal move meanwhile)
//   --pour-test S      no window: pour water at the map center for 15 s, then
//                      simulate S seconds with diagnostics every 30 s
//   --no-erosion       water only, without soaking and erosion (to compare)
//   --fixed-pipes      start with the classic fixed pipes (M toggles in game)
//   --tile-overlay     start with the water tile overlay on (F3 toggles)
//   --density X        vegetation density scale (overrides objects.density_scale)
//   --plant-test       plant a few circles with the brush after start (screenshots)
//   --gallery [CATEGORY]  no vegetation; every object type (or those of one
//                      category) in rows around the camera's target, to look at them
//   --actor-test [ACTION[:SECONDS]]  every actor type side by side at the
//                      camera's target, side on; optionally playing
//                      an action, that many seconds into it (screenshots)
//   --actor-goal DX,DZ with --actor-test: send them to the camera's target
//                      plus (DX, DZ) meters (screenshots: a thirtieth of a
//                      second per frame, with --frames)
//   --bridge-test      with --gallery none and --actor-test: a trench 3 m deep
//                      across the view at the target, bridged by bridge1,
//                      and the actors 10 m to its side (with --actor-goal)
//   --obstacle TYPE[:SCALE]  with --actor-test: an object of that type at the
//                      camera's target, the actors 10 m to its side
//   --walk-test [DZ]   with --obstacle and --warmup: in first person, 6 m from
//                      the obstacle, walking into it (W held) the whole warmup;
//                      the walker's and the nearest loose object's places are
//                      logged twice a second
//   --walk-start DX,DZ,YAW  with --walk-test: start there instead (meters from
//                      the obstacle, the heading in degrees: 180 = +Z, -90 = +X),
//                      dropped from a meter over the terrain onto whatever is there; logged
//                      ten times a second
//   --effect NAME      a particle effect (data/particles.ini) on the obstacle,
//                      or at the camera's target; with --warmup it plays that long
//   --pick             as if alt were held: highlight and log what the ray
//                      through the middle of the screen goes through
//   --navigation-overlay  start with the navigation overlay on (F8)
//   --ramp DEGREES     with --gallery: the floor slopes up along +X by that
//                      much (actors on slopes)
//   --layout NAME      place a layout where the camera looks (after the rest)
//   --no-ui            no interface (keys only; the statistics go to the title)
//   --load NAME        start with a saved map (from the saves folder)
//   --save NAME        save the map as NAME just before quitting (tests): an
//                      active map as NAME.active, a baked one as NAME.baked
//   --bake NAME        ...or bake it, as NAME.baked (for play)
//   --open WINDOW      open an interface window at start: settings, diagnostics,
//                      save, load, new, sky, console (for screenshots)
//   --console "A; B"   run console commands at start (see console.h), one
//                      after the other
//   --tool N           start with tool N (see tools.h; for screenshots)
//   --sky-test         paint the sky clear but for heavy clouds over the map (screenshots)
//   --thin-highlight   start with thin water highlighted (F6)
//   --inspect          pin the inspector on the cell the camera looks at;
//                      with --screenshot its report is logged at the end
//   --ui-test          click a tool button and the map with made-up mouse
//                      events, check who got the clicks, then quit
//   --physics-test     (with --scene dam) logs, barrels and a boat in the
//                      lake, trees, a house and fences below the dam
//   --map-test         save a flooded map, load it back over another one and
//                      check that everything came back, then quit
// F4 logs the diagnostics at once.

#include <SDL3/SDL.h>
#include <math.h>
#include <SDL3/SDL_main.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "camera.h"
#include "console.h"
#include "erosion.h"
#include "hiz.h"
#include "clouds.h"
#include "file_io.h"
#include "gpu.h"
#include "water_gpu.h"
#include "scene_uniforms.h"
#include "input.h"
#include "inspector.h"
#include "profile.h"
#include "gpu_timer.h"
#include "daylight.h"
#include "post.h"
#include "render_target.h"
#include "sky_renderer.h"
#include "objects.h"
#include "actors.h"
#include "navigation.h"
#include "particles.h"
#include "light_probe.h"
#include "senses.h"
#include "aurora.h"
#include "rain.h"
#include "grass.h"
#include "ground_overlay.h"
#include "waterfalls.h"
#include "weapons.h"
#include "projectiles.h"
#include "viewmodel.h"
#include "items.h"
#include "inventory.h"
#include "ground_items.h"
#include "markers.h"
#include "world_map.h"
#include "harvest.h"
#include "journal.h"
#include "dialog.h"
#include "sky_occlusion.h"
#include "water_ice.h"
#include "lights.h"
#include "fireflies.h"
#include "fog.h"
#include "physics.h"
#include "populate.h"
#include "layouts.h"
#include "map_file.h"
#include "materials.h"
#include "scenes.h"
#include "screenshot.h"
#include "season.h"
#include "settings.h"
#include "shadow_map.h"
#include "soils.h"
#include "stamps.h"
#include "terrain.h"
#include "terrain_cache.h"
#include "terrain_gen.h"
#include "terrain_renderer.h"
#include "material_map.h"
#include "tools.h"
#include "project.h"
#include "sketch_view.h"
#include "terrain_sketch.h"
#include "ui.h"
#include "water.h"
#include "water_renderer.h"
#include "world.h"
#include "session.h"
#include "potop_engine.h"
#include "ui_loading.h"
#include "player.h"

// Simulation speed choices for , and .

static const float time_scales[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
#define TIME_SCALE_COUNT ((int)(sizeof(time_scales) / sizeof(time_scales[0])))
#define TIME_SCALE_NORMAL 2

typedef struct
{
	uint32_t seed;
	bool seed_given;
	int size;
	int window_width;
	int window_height;
	float yaw_degrees;
	float pitch_degrees;
	float distance_meters;
	bool yaw_given;
	bool pitch_given;
	bool distance_given;
	bool first_person;
	bool go_to_given;
	float go_to[3];                // y NAN: on the ground
	bool look_at_given;
	float look_at[3];
	bool heading_given;
	float heading[2];
	int debug_view;
	bool brush_test;
	bool flood_test;
	bool water_test;
	bool dam_scene;
	bool dam_test;
	float dam_test_seconds;
	float warmup_seconds;
	float pour_test_seconds;
	bool no_erosion;
	bool fixed_pipes;
	bool tile_overlay;
	const char *screenshot_path;
	float spin_degrees;
	int screenshot_frames;
	float density_scale;           // --density, 0 if not given
	bool plant_test;
	bool gallery;
	bool actor_test;
	float ramp_degrees;            // --ramp: the gallery's floor slopes along +X
	bool bridge_test;
	bool walk_test;                // --walk-test: the walker walks into the obstacle during --warmup
	float walk_offset;             // meters to the side of the obstacle's middle
	bool walk_start_given;         // --walk-start: where from, and which way
	float walk_start[3];
	bool navigation_overlay;
	const char *obstacle;          // --obstacle TYPE[:SCALE]
	const char *effect;            // --effect NAME: played on the obstacle (or at the target)
	bool pick;                     // --pick: as if alt were held, the ray through the middle of the screen
	const char *actor_test_action; // NULL: their idle action
	bool actor_test_goal;          // --actor-goal: the actors were sent somewhere
	float actor_goal_x;            // meters from the camera's target
	float actor_goal_z;
	const char *layout_name;       // --layout
	bool inspect;                  // --inspect
	bool thin_highlight;           // --thin-highlight
	const char *gallery_category;  // NULL: all of them
	bool no_ui;
	const char *load_name;         // --load
	const char *save_name;         // --save
	const char *bake_name;         // --bake
	const char *open_window;       // --open
	const char *console_commands;  // --console: separated by ;
	bool sky_test;                 // --sky-test: clear sky but for a painted patch of clouds over the map
	bool map_test;
	bool physics_test;
	int tool;                      // --tool, -1 if not given
	bool ui_test;
	int ui_test_result;            // 0 running, 1 passed, 2 failed
} command_line_t;

typedef struct
{
	potop_t *potop;                // the engine (potop_engine.h: its settings, world, session, HUD, camera)
	settings_t file_settings;      // as data/settings.ini has them, for saving only what changed
	command_line_t command_line;
	SDL_Window *window;
	bool gpu_ready;                // the GPU device was made (what shutdown releases)

	// Alt: what the ray under the pointer goes through (shown, the nearest highlighted)
	int pick_line_count;
	char pick_lines[UI_PICK_LINES][96];
	bool pick_was_held;
	float pick_microseconds;
	uint32_t selected_actor;       // clicked on (tests): the next clicks send it somewhere; 0 = none
	int actor_type_to_place;       // K puts one of these (ctrl + K: the next type)
	char actor_name_to_place[32];  // ...with this name, game number, enemy or not (the World window)
	int actor_game_id_to_place;
	bool actor_hostile_to_place;
	bool pointer_ray_valid;        // the ray under the pointer (picking actors in the air)
	vec3_t pointer_ray_origin;
	vec3_t pointer_ray_direction;
	bool actor_click_held;         // the left button went down on an actor command: the tools do not see it
	inspector_t inspector;
	random_t object_random;        // for the planting brush, single objects and layouts
	// On foot, the tool put away (0): no brush, nothing applied, until 0
	// again or another tool is chosen
	bool hands_empty;
	int hands_empty_tool;          // the tool that was put away
	console_t console;             // ` or ~ (see console.h)
	bool mouse_free;               // Tab on foot: the pointer is out for the interface, the keys still walk
	tool_state_t tools;
	input_state_t input;
	ui_t ui;
	float reached_time_scale;      // the simulation speed reached lately

	bool paused;
	int time_scale_index;

	// Statistics shown in the window title
	double title_timer_seconds;
	int frames_since_title;
	double frame_seconds_sum;
	double simulation_seconds_sum;
	double simulated_seconds_sum;  // game time advanced, for the real speed
	int thread_count;              // OpenMP threads available
	double render_seconds_sum;
	// Where the simulation time went, for the status line
	double water_upload_milliseconds_sum; // since the last render diagnostics
	// The start: when it began, the stage now (startup_stage)
	uint64_t startup_counter;
	uint64_t stage_counter;
	const char *stage_name;
	bool first_frame_logged;
	// The map project open (project.h; "" none): saves go into it
	char project_name[PROJECT_NAME_LENGTH];
	// The new map's sketch (the sketch window, terrain_sketch.h) and its picture
	terrain_sketch_t sketch;
	sketch_view_t sketch_view;
	ui_requests_t console_requests;    // the console's "project" asks (as the interface does), done with the interface's
} testbed_t;

// The player changed terrain or water: the world's water runs again
static void touch_water(testbed_t *game)
{
	world_touch_water(&game->potop->world);
}

// A map of another size: the world's, the tools' hover let go (false: out
// of memory, the game cannot go on)
static bool resize_world(testbed_t *game, int32_t size, float cell_size_meters, bool baked)
{
	if (!world_resize(&game->potop->world, size, cell_size_meters, baked))
	{
		return false;
	}
	game->tools.hover_valid = false;
	game->tools.stamp_dragging = false;
	return true;
}

// A new map's land (and what grows on it): nothing lies about any more
static void regenerate_terrain(testbed_t *game, uint32_t seed)
{
	if (!world_generate(&game->potop->world, seed))
	{
		game->input.request_quit = true;
		return;
	}
	ground_items_clear(&game->potop->world.ground_items, &game->potop->session.inventories);
}

// The empty map the editor starts with: cells a side
#define EMPTY_START_SIZE 256

// An empty map's land: flat, low (a sketch painted all plain, no rain's
// erosion): to start from without waiting for a big one
static void make_empty_land(testbed_t *game, uint32_t seed)
{
	terrain_sketch_t flat;
	float erosion = game->potop->settings.terrain.erosion_droplets;

	if (!terrain_sketch_create(&flat))
	{
		regenerate_terrain(game, seed);
		return;
	}
	SDL_memset(terrain_sketch_plane(&flat, TERRAIN_SKETCH_REGION), TERRAIN_SKETCH_PLAIN, (size_t)TERRAIN_SKETCH_SIZE * TERRAIN_SKETCH_SIZE);
	flat.land.roll[TERRAIN_SKETCH_PLAIN] = 0.0f;
	flat.revision++;
	game->potop->settings.terrain.erosion_droplets = 0.0f;
	game->potop->world.generation_sketch = &flat;
	regenerate_terrain(game, seed);
	game->potop->world.generation_sketch = NULL;
	game->potop->settings.terrain.erosion_droplets = erosion;
	terrain_sketch_destroy(&flat);
}

static void parse_command_line(command_line_t *command_line, int argument_count, char **arguments)
{
	SDL_zerop(command_line);
	command_line->window_width = 1280;
	command_line->window_height = 800;
	command_line->screenshot_frames = 3;
	command_line->dam_test_seconds = 180.0f;
	command_line->tool = -1;

	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		const char *value = i + 1 < argument_count ? arguments[i + 1] : NULL;

		if (SDL_strcmp(argument, "--brush-test") == 0)
		{
			command_line->brush_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--flood-test") == 0)
		{
			command_line->flood_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--water-test") == 0)
		{
			command_line->water_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--tile-overlay") == 0)
		{
			command_line->tile_overlay = true;
			continue;
		}
		if (SDL_strcmp(argument, "--fixed-pipes") == 0)
		{
			command_line->fixed_pipes = true;
			continue;
		}
		if (SDL_strcmp(argument, "--actor-goal") == 0 && value != NULL)
		{
			command_line->actor_test_goal = SDL_sscanf(value, "%f,%f", &command_line->actor_goal_x, &command_line->actor_goal_z) == 2;
			i++;
			continue;
		}
		if (SDL_strcmp(argument, "--obstacle") == 0 && value != NULL)
		{
			command_line->obstacle = value;
			i++;
			continue;
		}
		if (SDL_strcmp(argument, "--pick") == 0)
		{
			command_line->pick = true;
			continue;
		}
		if (SDL_strcmp(argument, "--effect") == 0 && value != NULL)
		{
			command_line->effect = value;
			i++;
			continue;
		}
		if (SDL_strcmp(argument, "--navigation-overlay") == 0)
		{
			command_line->navigation_overlay = true;
			continue;
		}
		if (SDL_strcmp(argument, "--walk-start") == 0 && value != NULL)
		{
			command_line->walk_start_given = SDL_sscanf(value, "%f,%f,%f", &command_line->walk_start[0], &command_line->walk_start[1],
				&command_line->walk_start[2]) == 3;
			i++;
			continue;
		}
		if (SDL_strcmp(argument, "--walk-test") == 0)
		{
			command_line->walk_test = true;
			// An optional sideways offset: walking past the middle
			if (value != NULL && (SDL_isdigit((unsigned char)value[0]) || value[0] == '-') && SDL_strncmp(value, "--", 2) != 0)
			{
				command_line->walk_offset = (float)SDL_atof(value);
				i++;
			}
			continue;
		}
		if (SDL_strcmp(argument, "--bridge-test") == 0)
		{
			command_line->bridge_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--ramp") == 0 && value != NULL)
		{
			command_line->ramp_degrees = (float)SDL_atof(value);
			i++;
			continue;
		}
		if (SDL_strcmp(argument, "--actor-test") == 0)
		{
			command_line->actor_test = true;
			if (value != NULL && value[0] != '-')
			{
				command_line->actor_test_action = value;
				i++;
			}
			continue;
		}
		if (SDL_strcmp(argument, "--gallery") == 0)
		{
			command_line->gallery = true;
			if (value != NULL && value[0] != '-')
			{
				command_line->gallery_category = value;
				i++;
			}
			continue;
		}
		if (SDL_strcmp(argument, "--thin-highlight") == 0)
		{
			command_line->thin_highlight = true;
			continue;
		}
		if (SDL_strcmp(argument, "--inspect") == 0)
		{
			command_line->inspect = true;
			continue;
		}
		if (SDL_strcmp(argument, "--ui-test") == 0)
		{
			command_line->ui_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--physics-test") == 0)
		{
			command_line->physics_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--map-test") == 0)
		{
			command_line->map_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--no-ui") == 0)
		{
			command_line->no_ui = true;
			continue;
		}
		if (SDL_strcmp(argument, "--plant-test") == 0)
		{
			command_line->plant_test = true;
			continue;
		}
		if (SDL_strcmp(argument, "--no-erosion") == 0)
		{
			command_line->no_erosion = true;
			continue;
		}
		if (SDL_strcmp(argument, "--dam-test") == 0)
		{
			command_line->dam_test = true;
			command_line->dam_scene = true;
			continue;
		}
		if (SDL_strcmp(argument, "--first-person") == 0)
		{
			command_line->first_person = true;
			continue;
		}
		if (SDL_strcmp(argument, "--sky-test") == 0)
		{
			command_line->sky_test = true;
			continue;
		}
		if (value == NULL)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Option %s needs a value", argument);
			continue;
		}
		if (SDL_strcmp(argument, "--seed") == 0)
		{
			command_line->seed = (uint32_t)SDL_strtoul(value, NULL, 0);
			command_line->seed_given = true;
		}
		else if (SDL_strcmp(argument, "--size") == 0)
		{
			command_line->size = SDL_atoi(value);
		}
		else if (SDL_strcmp(argument, "--width") == 0)
		{
			command_line->window_width = SDL_atoi(value);
		}
		else if (SDL_strcmp(argument, "--height") == 0)
		{
			command_line->window_height = SDL_atoi(value);
		}
		else if (SDL_strcmp(argument, "--spin") == 0)
		{
			// Degrees a frame the camera turns (the tests: things in view change)
			command_line->spin_degrees = (float)SDL_atof(value);
		}
		else if (SDL_strcmp(argument, "--yaw") == 0)
		{
			command_line->yaw_degrees = (float)SDL_atof(value);
			command_line->yaw_given = true;
		}
		else if (SDL_strcmp(argument, "--pitch") == 0)
		{
			command_line->pitch_degrees = (float)SDL_atof(value);
			command_line->pitch_given = true;
		}
		else if (SDL_strcmp(argument, "--go-to") == 0)
		{
			char y_text[32] = "";

			command_line->go_to_given = SDL_sscanf(value, "%f,%31[^,],%f", &command_line->go_to[0], y_text, &command_line->go_to[2]) == 3;
			command_line->go_to[1] = y_text[0] == '-' && y_text[1] == '\0' ? NAN : (float)SDL_atof(y_text);
		}
		else if (SDL_strcmp(argument, "--look-at") == 0)
		{
			command_line->look_at_given = SDL_sscanf(value, "%f,%f,%f", &command_line->look_at[0], &command_line->look_at[1],
				&command_line->look_at[2]) == 3;
		}
		else if (SDL_strcmp(argument, "--heading") == 0)
		{
			command_line->heading_given = SDL_sscanf(value, "%f,%f", &command_line->heading[0], &command_line->heading[1]) == 2;
		}
		else if (SDL_strcmp(argument, "--distance") == 0)
		{
			command_line->distance_meters = (float)SDL_atof(value);
			command_line->distance_given = true;
		}
		else if (SDL_strcmp(argument, "--debug-view") == 0)
		{
			command_line->debug_view = SDL_atoi(value);
		}
		else if (SDL_strcmp(argument, "--screenshot") == 0)
		{
			command_line->screenshot_path = value;
		}
		else if (SDL_strcmp(argument, "--scene") == 0)
		{
			if (SDL_strcmp(value, "dam") == 0)
			{
				command_line->dam_scene = true;
			}
			else
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unknown scene %s (known: dam)", value);
			}
		}
		else if (SDL_strcmp(argument, "--tool") == 0)
		{
			command_line->tool = SDL_atoi(value);
		}
		else if (SDL_strcmp(argument, "--load") == 0)
		{
			command_line->load_name = value;
		}
		else if (SDL_strcmp(argument, "--save") == 0)
		{
			command_line->save_name = value;
		}
		else if (SDL_strcmp(argument, "--bake") == 0)
		{
			command_line->bake_name = value;
		}
		else if (SDL_strcmp(argument, "--open") == 0)
		{
			command_line->open_window = value;
		}
		else if (SDL_strcmp(argument, "--console") == 0)
		{
			command_line->console_commands = value;
		}
		else if (SDL_strcmp(argument, "--layout") == 0)
		{
			command_line->layout_name = value;
		}
		else if (SDL_strcmp(argument, "--density") == 0)
		{
			command_line->density_scale = SDL_max(0.0f, (float)SDL_atof(value));
		}
		else if (SDL_strcmp(argument, "--pour-test") == 0)
		{
			command_line->pour_test_seconds = (float)SDL_atof(value);
		}
		else if (SDL_strcmp(argument, "--warmup") == 0)
		{
			command_line->warmup_seconds = (float)SDL_atof(value);
		}
		else if (SDL_strcmp(argument, "--dam-seconds") == 0)
		{
			command_line->dam_test_seconds = (float)SDL_atof(value);
		}
		else if (SDL_strcmp(argument, "--frames") == 0)
		{
			command_line->screenshot_frames = SDL_max(1, SDL_atoi(value));
		}
		else
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unknown option %s", argument);
			continue;
		}
		i++;
	}
}

static uint32_t pick_random_seed(void)
{
	uint64_t mixed = SDL_GetPerformanceCounter() * 0x9E3779B97F4A7C15ULL;

	return (uint32_t)(mixed >> 32) | 1u;
}

// The start, stage by stage: the stage before logged with its time, and
// the loading screen showing the next (what, how far along: 0..1), once
// there is a window. next NULL: the start is over
static void startup_stage(testbed_t *game, const char *next, float fraction)
{
	uint64_t now = SDL_GetPerformanceCounter();
	double frequency = (double)SDL_GetPerformanceFrequency();

	if (game->startup_counter == 0)
	{
		game->startup_counter = now;
	}
	if (game->stage_name != NULL)
	{
		SDL_Log("Startup: %s: %.2f s (%.2f s so far)", game->stage_name, (double)(now - game->stage_counter) / frequency,
			(double)(now - game->startup_counter) / frequency);
	}
	game->stage_name = next;
	if (next != NULL && game->window != NULL && game->ui.available)
	{
		ui_loading_frame(&game->ui.backend, "Potop editor", next, fraction);
	}
	// (the screen's drawing not counted in the stage)
	game->stage_counter = SDL_GetPerformanceCounter();
}

// The world's progress while it is made (world_t.progress)
static void world_startup_progress(void *user, const char *stage, float fraction)
{
	startup_stage(user, stage, fraction);
}

static bool create_window(testbed_t *game)
{
	bool fullscreen = game->potop->settings.render.start_fullscreen && game->command_line.screenshot_path == NULL;
	bool maximized = !fullscreen && game->potop->settings.render.start_maximized && game->command_line.screenshot_path == NULL;
	int width = game->command_line.window_width;
	int height = game->command_line.window_height;

	// Fullscreen from the start: the window made as big as the screen right
	// away, so that it never changes size once the rendering has begun
	if (fullscreen)
	{
		const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());

		if (mode != NULL)
		{
			width = mode->w;
			height = mode->h;
		}
	}
	game->window = SDL_CreateWindow("Potop", width, height,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0) | (maximized ? SDL_WINDOW_MAXIMIZED : 0));
	if (game->window == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	if (fullscreen || maximized)
	{
		// The window manager makes it fullscreen when it gets to it: wait
		// for that before anything is drawn
		SDL_SyncWindow(game->window);
	}
	if (!gpu_create(game->window, game->potop->settings.render.vsync && game->command_line.screenshot_path == NULL, game->potop->settings.render.anisotropy))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No GPU driver can run the game (Vulkan, Metal or Direct3D 12 needed)");
		return false;
	}
	game->gpu_ready = true;
	return true;
}

// Loads the terrain from the cache, or generates (and caches) it
static bool resize_world(testbed_t *game, int32_t size, float cell_size_meters, bool baked);

// Logs what the GPU spends per frame on each pass (averaged since the last
// log) and how much terrain the last frame drew at each level of detail.
static void log_render_diagnostics(testbed_t *game)
{
	const terrain_renderer_t *renderer = &game->potop->world.renderer;
	char passes_text[256] = "";
	char levels_text[192] = "";
	double total = 0.0;

	if (game->potop->world.gpu_timer.available)
	{
		for (int pass = 0; pass < GPU_PASS_COUNT; pass++)
		{
			char entry[64];
			double milliseconds = gpu_timer_average(&game->potop->world.gpu_timer, (gpu_pass_t)pass);

			SDL_snprintf(entry, sizeof(entry), "%s%s %.2f", pass > 0 ? ", " : "", gpu_timer_pass_name((gpu_pass_t)pass), milliseconds);
			SDL_strlcat(passes_text, entry, sizeof(passes_text));
			total += milliseconds;
		}
	}
	else
	{
		SDL_strlcpy(passes_text, "not measurable here", sizeof(passes_text));
	}
	for (int level = 0; level < TERRAIN_LOD_LEVEL_COUNT; level++)
	{
		char entry[32];

		SDL_snprintf(entry, sizeof(entry), "%s%d", level > 0 ? "/" : "", renderer->chunks_per_lod_level[level]);
		SDL_strlcat(levels_text, entry, sizeof(levels_text));
	}
	SDL_Log("Diagnostics, GPU ms per frame: %s, total %.2f | terrain chunks per detail level (step 1/2/4/8/16/32): %s, %.2f M triangles "
		"| render scale %.2f",
		passes_text, total, levels_text, (double)renderer->triangles_drawn * 1e-6,
		(double)SDL_clamp(game->potop->settings.render.render_scale, 0.25f, 1.0f));
	{
		double frames = (double)SDL_max(renderer->upload_frame_count, 1);

		SDL_Log("Diagnostics, CPU ms per frame for uploads: terrain %.2f (%.1f chunks per frame), water %.2f | objects: %d drawn of %d in %d draw calls",
			renderer->upload_milliseconds_sum / frames, (double)renderer->uploaded_chunk_count / frames,
			game->water_upload_milliseconds_sum / frames, game->potop->world.objects.drawn_instance_count, game->potop->world.objects.instance_count,
			game->potop->world.objects.draw_call_count);
		game->potop->world.renderer.upload_milliseconds_sum = 0.0;
		game->potop->world.renderer.uploaded_chunk_count = 0;
		game->potop->world.renderer.deferred_chunk_count = 0;
		game->potop->world.renderer.upload_frame_count = 0;
		game->water_upload_milliseconds_sum = 0.0;
	}
}

// Logs what keeps the water awake and what the simulation costs, every
// diagnostics_interval_seconds of simulated time (or now, if forced), then
// starts counting again. Only logs while some water is awake.
static void log_diagnostics(testbed_t *game, bool force)
{
	water_t *water = &game->potop->world.water;
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;
	float interval = game->potop->settings.simulation.diagnostics_interval_seconds;
	int32_t active;
	int32_t sleeping;
	int32_t dry;
	int32_t active_with_mud = 0;
	int64_t suspended_units = 0;
	int32_t busiest[3] = { -1, -1, -1 };
	char busiest_text[384] = "";
	double cell_area = (double)water->cell_size_meters * water->cell_size_meters;
	double steps = (double)SDL_max(game->potop->world.diagnostics.step_count, 1);

	if (!force && (interval <= 0.0f || game->potop->world.diagnostics.simulated_seconds < interval))
	{
		return;
	}
	water_count_tiles(water, &active, &sleeping, &dry);
	for (int32_t tile = 0; tile < tile_count; tile++)
	{
		suspended_units += water->tile_sediment[tile];
		if (water->tile_state[tile] != WATER_TILE_ACTIVE)
		{
			continue;
		}
		if (water->tile_sediment[tile] > 0)
		{
			active_with_mud++;
		}
		// The three active tiles with the fastest flow
		for (int k = 0; k < 3; k++)
		{
			if (busiest[k] < 0 || water->tile_max_speed[tile] > water->tile_max_speed[busiest[k]])
			{
				for (int j = 2; j > k; j--)
				{
					busiest[j] = busiest[j - 1];
				}
				busiest[k] = tile;
				break;
			}
		}
	}
	for (int k = 0; k < 3 && busiest[k] >= 0; k++)
	{
		int32_t tile = busiest[k];
		char entry[128];

		SDL_snprintf(entry, sizeof(entry), "%s(%d,%d) %.3f m/s, change %d, deepest %.2f m, mud %.2f m3", k > 0 ? "; " : "",
			tile % water->tiles_per_side, tile / water->tiles_per_side, (double)water->tile_max_speed[tile], water->tile_max_change[tile],
			(double)water->tile_max_depth[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT,
			(double)water->tile_sediment[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT * cell_area);
		SDL_strlcat(busiest_text, entry, sizeof(busiest_text));
	}

	if (active > 0 || force)
	{
		SDL_Log("Diagnostics, last %.0f s: tiles %d active (%d with mud), %d still, %d dry | woken by source %lld, neighbor %lld, edit %lld, "
			"soak %lld, erosion %lld | kept awake (tile substeps) by flow %lld, by level change %lld | mud in suspension %.1f m3 | "
			"water %.2f ms/step (max %.2f), erosion %.2f ms/step (max %.2f)",
			game->potop->world.diagnostics.simulated_seconds, active, active_with_mud, sleeping, dry,
			(long long)water->wake_counts[WATER_WAKE_SOURCE], (long long)water->wake_counts[WATER_WAKE_NEIGHBOR],
			(long long)water->wake_counts[WATER_WAKE_EDIT], (long long)water->wake_counts[WATER_WAKE_SOAK],
			(long long)water->wake_counts[WATER_WAKE_EROSION],
			(long long)water->kept_awake_by_flow, (long long)water->kept_awake_by_level_change,
			(double)suspended_units * TERRAIN_METERS_PER_HEIGHT_UNIT * cell_area,
			game->potop->world.diagnostics.water_milliseconds_sum / steps, game->potop->world.diagnostics.water_milliseconds_max,
			game->potop->world.diagnostics.erosion_milliseconds_sum / steps, game->potop->world.diagnostics.erosion_milliseconds_max);
		if (busiest[0] >= 0)
		{
			SDL_Log("Diagnostics, fastest active tiles (x,y): %s", busiest_text);
		}
		{
			char phases_text[512] = "";
			double milliseconds_per_tick = 1000.0 / (double)SDL_GetPerformanceFrequency();

			for (int phase = 0; phase < PROFILE_PHASE_COUNT; phase++)
			{
				char entry[64];

				SDL_snprintf(entry, sizeof(entry), "%s%s %.3f", phase > 0 ? ", " : "", profile_phase_name((profile_phase_t)phase),
					(double)profile.ticks[phase] * milliseconds_per_tick / steps);
				SDL_strlcat(phases_text, entry, sizeof(phases_text));
			}
			SDL_Log("Diagnostics, ms per step: %s | %.2f substeps and %.1f simulated tiles per step, %d threads",
				phases_text, (double)profile.substeps / steps, (double)profile.tile_substeps / steps, game->thread_count);
		}
	}
	if (force)
	{
		log_render_diagnostics(game);
	}
	gpu_timer_reset(&game->potop->world.gpu_timer);
	SDL_zero(water->wake_counts);
	water->kept_awake_by_flow = 0;
	water->kept_awake_by_level_change = 0;
	SDL_zero(game->potop->world.diagnostics);
	SDL_zero(profile);
}

// What a held click does to the water: shift and ctrl pour and sponge with
// any tool, the water tools without them
static water_action_t current_water_action(const testbed_t *game)
{
	if (game->input.water_action != WATER_ACTION_NONE)
	{
		return game->input.water_action;
	}
	if (game->tools.current_tool == TOOL_WATER_ADD)
	{
		return WATER_ACTION_ADD;
	}
	if (game->tools.current_tool == TOOL_WATER_REMOVE)
	{
		return WATER_ACTION_REMOVE;
	}
	return WATER_ACTION_NONE;
}

static void show_message(testbed_t *game, const char *message);

// A baked map's ground and water stay as they are (map_file.h): true (and
// a word now and then) on one
static bool baked_refuses(testbed_t *game)
{
	if (!game->potop->world.terrain.baked)
	{
		return false;
	}
	if (game->potop->world.elapsed_seconds - game->potop->world.baked_refused_seconds > 3.0)
	{
		game->potop->world.baked_refused_seconds = game->potop->world.elapsed_seconds;
		show_message(game, "A baked map: its ground and water stay as they are (load the active one to change them)");
	}
	return true;
}

// The brushes' tools are in the order of the engine's brushes
_Static_assert((int)TOOL_RAISE == (int)POTOP_BRUSH_RAISE && (int)TOOL_SMOOTH == (int)POTOP_BRUSH_SMOOTH, "brush tools");

static void simulation_step(testbed_t *game, float step_seconds)
{
	float map_center = (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters * 0.5f;

	if (game->command_line.brush_test && game->potop->world.simulation_step_count < 30)
	{
		// Automated test of the edit -> bounds -> upload path
		potop_edit_terrain(game->potop, POTOP_BRUSH_RAISE, map_center, map_center, 40.0f, game->potop->settings.tools.raise_meters_per_second,
			POTOP_SOIL_SAND, step_seconds * 10.0f);
	}
	else if (game->command_line.flood_test && game->potop->world.simulation_step_count < 120)
	{
		water_apply_brush(&game->potop->world.water, map_center, map_center, 30.0f, 4.0f, step_seconds);
	}
	else if (game->input.apply_tool_held && game->tools.hover_valid)
	{
		water_action_t water_action = current_water_action(game);

		const tool_settings_t *rates = &game->potop->settings.tools;
		float x = game->tools.hover_point.x;
		float z = game->tools.hover_point.z;
		float radius = game->tools.brush_radius_meters;

		if ((water_action != WATER_ACTION_NONE || game->tools.current_tool < TOOL_STAMP) && baked_refuses(game))
		{
			return;
		}
		if (water_action == WATER_ACTION_ADD || water_action == WATER_ACTION_REMOVE)
		{
			potop_edit_water(game->potop, x, z, radius, water_action == WATER_ACTION_ADD ? rates->water_meters_per_second : -rates->water_meters_per_second,
				step_seconds);
		}
		else if (game->tools.current_tool < TOOL_STAMP)
		{
			// (the brushes' tools are in the order of potop_brush_t)
			potop_brush_t brush = (potop_brush_t)game->tools.current_tool;

			potop_edit_terrain(game->potop, brush, x, z, radius, brush == POTOP_BRUSH_SMOOTH ? rates->smooth_strength_per_second : rates->raise_meters_per_second,
				(potop_soil_t)game->tools.current_soil, step_seconds);
		}
	}

	world_step(&game->potop->world, step_seconds);
	if (game->potop->world.water_stepped)
	{
		inspector_record(&game->inspector, &game->potop->world.water, &game->potop->world.terrain);
	}
	log_diagnostics(game, false);
}

// The stamp the current tool settings describe, from start to end
static potop_stamp_t current_stamp(const testbed_t *game, vec3_t start, vec3_t end)
{
	potop_stamp_t stamp;

	SDL_zero(stamp);
	stamp.type = (potop_stamp_type_t)game->tools.current_stamp;
	stamp.soil = (potop_soil_t)game->tools.current_soil;
	stamp.radius_meters = game->tools.brush_radius_meters;
	stamp.height_meters = game->tools.stamp_height_meters;
	stamp.start_x = start.x;
	stamp.start_y = start.y;
	stamp.start_z = start.z;
	stamp.end_x = end.x;
	stamp.end_y = end.y;
	stamp.end_z = end.z;
	return stamp;
}

// The area tool: a click in an area picks it; a drag elsewhere draws a new
// one (along the map's sides: turned in the panel), picked once drawn
static void handle_area_tool(testbed_t *game)
{
	tool_state_t *tools = &game->tools;
	input_state_t *input = &game->input;

	if (tools->current_tool != TOOL_AREA)
	{
		tools->area_dragging = false;
		return;
	}
	if (areas_find(&game->potop->world.areas, tools->area_selected) < 0)
	{
		tools->area_selected = 0;
	}
	if (input->apply_tool_pressed && input->water_action == WATER_ACTION_NONE && tools->hover_valid)
	{
		int index = areas_at(&game->potop->world.areas, tools->hover_point.x, tools->hover_point.z);

		if (index >= 0)
		{
			tools->area_selected = game->potop->world.areas.areas[index].id;
		}
		else
		{
			tools->area_dragging = true;
			tools->area_drag_start = tools->hover_point;
		}
	}
	if (input->apply_tool_released && tools->area_dragging)
	{
		tools->area_dragging = false;
		if (tools->hover_valid)
		{
			float width = SDL_fabsf(tools->hover_point.x - tools->area_drag_start.x);
			float depth = SDL_fabsf(tools->hover_point.z - tools->area_drag_start.z);

			if (width >= 1.0f && depth >= 1.0f)
			{
				char name[AREA_NAME_LENGTH];
				uint32_t id;

				SDL_snprintf(name, sizeof(name), "area_%d", game->potop->world.areas.count + 1);
				id = potop_area_add(game->potop, name, 0.5f * (tools->hover_point.x + tools->area_drag_start.x),
					0.5f * (tools->hover_point.z + tools->area_drag_start.z), width, depth, 0.0f, POTOP_AREA_TRIGGER_PLAYER);
				if (id != 0)
				{
					tools->area_selected = id;
				}
				else
				{
					ui_show_message(&game->ui, "No room for more areas", game->potop->world.elapsed_seconds);
				}
			}
		}
	}
}

// Stamps act on a click (or, for dams and channels, on the release at the
// end of a drag), once, not every step like the brushes
static void handle_stamp_tool(testbed_t *game)
{
	tool_state_t *tools = &game->tools;
	input_state_t *input = &game->input;

	if (tools->current_tool != TOOL_STAMP || (input->apply_tool_pressed && baked_refuses(game)))
	{
		tools->stamp_dragging = false;
		return;
	}
	if (input->apply_tool_pressed && input->water_action == WATER_ACTION_NONE && tools->hover_valid)
	{
		if (stamps_is_line(tools->current_stamp))
		{
			tools->stamp_dragging = true;
			tools->stamp_drag_start = tools->hover_point;
		}
		else
		{
			potop_stamp_t stamp = current_stamp(game, tools->hover_point, tools->hover_point);

			potop_edit_stamp(game->potop, &stamp);
		}
	}
	if (input->apply_tool_released && tools->stamp_dragging)
	{
		tools->stamp_dragging = false;
		if (tools->hover_valid)
		{
			potop_stamp_t stamp = current_stamp(game, tools->stamp_drag_start, tools->hover_point);

			potop_edit_stamp(game->potop, &stamp);
		}
	}
}

// The next category after 'category' that the brush can plant, or the
// same one if there is no other
static int next_plant_category(const testbed_t *game, int category)
{
	const object_types_t *types = &game->potop->world.objects.types;

	for (int step = 1; step <= types->category_count; step++)
	{
		int candidate = (category + step) % SDL_max(types->category_count, 1);

		if (object_types_category_plantable(types, candidate))
		{
			return candidate;
		}
	}
	return category;
}

// O: one object of the plant category, of a type picked by brush density
static void place_single_object(testbed_t *game, vec3_t point)
{
	const object_types_t *types = &game->potop->world.objects.types;
	float total = 0.0f;
	float pick;

	for (int t = 0; t < types->type_count; t++)
	{
		if (types->types[t].category == game->tools.plant_category && types->types[t].model >= 0)
		{
			total += SDL_max(types->types[t].brush_per_hectare, 1.0f);
		}
	}
	pick = random_float(&game->object_random) * total;
	for (int t = 0; t < types->type_count; t++)
	{
		if (types->types[t].category != game->tools.plant_category || types->types[t].model < 0)
		{
			continue;
		}
		pick -= SDL_max(types->types[t].brush_per_hectare, 1.0f);
		if (pick < 0.0f)
		{
			float yaw;
			float scale;

			objects_random_placement(&game->potop->world.objects, t, random_float(&game->object_random), random_float(&game->object_random), &yaw, &scale);
			potop_object_place(game->potop, t, point.x, point.z, yaw, scale, false);
			return;
		}
	}
}

// The gallery's rows: how deep they all are together, laid as
// place_gallery lays them (meters along +z)
static float gallery_depth(const testbed_t *game, int category, bool none, float row_width)
{
	const object_types_t *types = &game->potop->world.objects.types;
	float x = 0.0f;
	float depth = 0.0f;
	float row_depth = 0.0f;

	for (int t = 0; t < types->type_count && !none; t++)
	{
		const object_type_t *type = &types->types[t];
		float radius;

		if (type->model < 0 || (category >= 0 && type->category != category))
		{
			continue;
		}
		radius = game->potop->world.objects.models[type->model].horizontal_radius * 0.5f * (type->scale_minimum + type->scale_maximum);
		if (x + 2.0f * radius > row_width && x > 0.0f)
		{
			x = 0.0f;
			depth += row_depth + 1.5f;
			row_depth = 0.0f;
		}
		x += 2.0f * radius + 1.5f;
		row_depth = SDL_max(row_depth, 2.0f * radius);
	}
	return depth + row_depth;
}

// --gallery: every type (of one category) in rows, at its average size,
// round the camera's target (the rows centered on it, and kept inside the
// map: the last ones fell off its edge)
static void place_gallery(testbed_t *game)
{
	const object_types_t *types = &game->potop->world.objects.types;
	const float row_width = 170.0f;
	float start_x = game->potop->camera.target_x - row_width * 0.5f;
	float x = start_x;
	float row_depth = 0.0f;
	float extent_x = start_x;
	int placed = 0;
	int category = game->command_line.gallery_category != NULL ? object_types_find_category(types, game->command_line.gallery_category) : -1;
	// An unknown category: just the flat floor (for the plant test)
	bool none = game->command_line.gallery_category != NULL && category < 0;
	float depth = gallery_depth(game, category, none, row_width);
	float map_meters = (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters;
	float z = game->potop->camera.target_z - 0.5f * depth;
	float start_z;

	// (a margin of the floor's round it, inside the map)
	z = SDL_min(z, map_meters - 12.0f - depth);
	z = SDL_max(z, 12.0f);
	start_z = z;
	objects_clear(&game->potop->world.objects, false);
	// A flat floor to stand them on
	{
		terrain_t *terrain = &game->potop->world.terrain;
		float cell = terrain->cell_size_meters;
		int32_t level = terrain_height_units_clamped(terrain, (int32_t)(game->potop->camera.target_x / cell), (int32_t)(game->potop->camera.target_z / cell));
		int32_t bounds[4] = {
			clamp_int((int32_t)((start_x - 10.0f) / cell), 0, terrain->size - 1),
			clamp_int((int32_t)((z - 10.0f) / cell), 0, terrain->size - 1),
			clamp_int((int32_t)((start_x + row_width + 10.0f) / cell), 0, terrain->size - 1),
			clamp_int((int32_t)((z + depth + 10.0f) / cell), 0, terrain->size - 1) };

		for (int32_t y = bounds[1]; y <= bounds[3]; y++)
		{
			for (int32_t cell_x = bounds[0]; cell_x <= bounds[2]; cell_x++)
			{
				// --ramp: rising along +X through the target
				float rise = SDL_tanf(degrees_to_radians(game->command_line.ramp_degrees)) * ((float)cell_x * cell - game->potop->camera.target_x);

				terrain->heights[terrain_index(terrain, cell_x, y)] = level + (int32_t)SDL_lroundf(rise / TERRAIN_METERS_PER_HEIGHT_UNIT);
				terrain->soil_types[terrain_index(terrain, cell_x, y)] = SOIL_EARTH;
			}
		}
		terrain_fix_layers(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
		terrain_mark_cells_changed(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
		terrain->shadow_dirty = true;
		game->potop->camera.target_height = (float)level * TERRAIN_METERS_PER_HEIGHT_UNIT;
	}
	for (int t = 0; t < types->type_count; t++)
	{
		const object_type_t *type = &types->types[t];
		const object_model_t *model;
		float scale;
		float radius;

		if (none || type->model < 0 || (category >= 0 && type->category != category))
		{
			continue;
		}
		model = &game->potop->world.objects.models[type->model];
		scale = 0.5f * (type->scale_minimum + type->scale_maximum);
		radius = model->horizontal_radius * scale;
		if (x + 2.0f * radius > start_x + row_width && x > start_x)
		{
			x = start_x;
			z += row_depth + 1.5f;
			row_depth = 0.0f;
		}
		objects_add(&game->potop->world.objects, &game->potop->world.terrain, t, x + radius, z + radius, 0.0f, scale, 0);
		SDL_Log("Gallery %3d: %-18s at (%.0f, %.0f)", placed, type->name, (double)(x + radius - game->potop->camera.target_x),
			(double)(z + radius - game->potop->camera.target_z));
		x += 2.0f * radius + 1.5f;
		extent_x = SDL_max(extent_x, x);
		row_depth = SDL_max(row_depth, 2.0f * radius);
		placed++;
	}
	SDL_Log("Gallery: %d types%s%s", placed, category >= 0 ? " of " : "", category >= 0 ? types->categories[category] : "");
	// Look at the middle of it
	if (placed == 0)
	{
		return;
	}
	game->potop->camera.target_x = 0.5f * (start_x + extent_x);
	game->potop->camera.target_z = 0.5f * (start_z + z + row_depth);
}

// The object tool: one object of the chosen type; loose things are
// dropped from the chosen height, awake
static void place_chosen_object(testbed_t *game, vec3_t point)
{
	const object_types_t *types = &game->potop->world.objects.types;
	int type = game->tools.object_type;
	float yaw;
	float scale;

	if (type < 0 || type >= types->type_count || types->types[type].model < 0)
	{
		return;
	}
	objects_random_placement(&game->potop->world.objects, type, random_float(&game->object_random), random_float(&game->object_random), &yaw, &scale);
	potop_object_place(game->potop, type, point.x, point.z, yaw, scale, game->tools.object_awake);
}

// Removes the layout (homestead, camp...) nearest to the pointer
static void remove_nearest_layout(testbed_t *game)
{
	int removed = potop_edit_remove_layout(game->potop, game->tools.hover_point.x, game->tools.hover_point.z);

	if (removed > 0)
	{
		SDL_Log("Removed a layout: %d objects", removed);
	}
}

// What the ray under the pointer goes through, nearest first: objects
// (their triangles) and actors (their boxes), mixed, up to the ground or a
// distance
typedef struct
{
	bool is_actor;
	int instance;                  // an object: its index (valid this frame)
	uint32_t actor_id;             // an actor
	float distance;
} pick_hit_t;

typedef struct
{
	int count;
	pick_hit_t hits[UI_PICK_LINES];
	float ground_distance;         // -1: the ray misses the ground (or stops before it)
} pick_t;

// mode: OBJECTS_RAY_ALL (the editor, Alt) or OBJECTS_RAY_INTERACTIVE (what
// the game can do something with); maximum_distance <= 0: down to the ground
static void pick_along_ray(testbed_t *game, objects_ray_mode_t mode, float maximum_distance, pick_t *pick)
{
	vec3_t origin = game->pointer_ray_origin;
	vec3_t direction = game->pointer_ray_direction;
	float maximum = game->potop->camera.far_distance;
	object_ray_hit_t object_hits[UI_PICK_LINES];
	uint32_t actor_ids[UI_PICK_LINES];
	float actor_distances[UI_PICK_LINES];
	int object_count;
	int actor_count;
	int object_next = 0;
	int actor_next = 0;

	SDL_zerop(pick);
	pick->ground_distance = -1.0f;
	if (!game->pointer_ray_valid)
	{
		return;
	}
	if (game->tools.hover_valid)
	{
		pick->ground_distance = vec3_length(vec3_subtract(game->tools.hover_point, origin));
		maximum = pick->ground_distance + 0.05f;
	}
	if (maximum_distance > 0.0f && maximum_distance < maximum)
	{
		maximum = maximum_distance;
		pick->ground_distance = -1.0f;
	}
	object_count = objects_raycast(&game->potop->world.objects, origin, direction, maximum, mode, object_hits, UI_PICK_LINES);
	actor_count = actors_raycast(&game->potop->world.actors, origin, direction, maximum, mode == OBJECTS_RAY_INTERACTIVE, actor_ids, actor_distances,
		UI_PICK_LINES);
	// The two lists, merged by distance
	while (pick->count < UI_PICK_LINES && (object_next < object_count || actor_next < actor_count))
	{
		pick_hit_t *hit = &pick->hits[pick->count++];

		if (actor_next < actor_count && (object_next >= object_count || actor_distances[actor_next] <= object_hits[object_next].distance))
		{
			hit->is_actor = true;
			hit->actor_id = actor_ids[actor_next];
			hit->distance = actor_distances[actor_next++];
		}
		else
		{
			hit->is_actor = false;
			hit->instance = object_hits[object_next].instance;
			hit->distance = object_hits[object_next++].distance;
		}
	}
}

// Draws the nearest hit highlighted
static void highlight_hit(testbed_t *game, const pick_hit_t *hit, const float color[3])
{
	if (hit->is_actor)
	{
		game->potop->world.actors.highlight_id = hit->actor_id;
		SDL_memcpy(game->potop->world.actors.highlight_color, color, sizeof(float) * 3);
	}
	else
	{
		objects_set_highlight(&game->potop->world.objects, hit->instance, color);
	}
}

// Alt held (or --pick): the nearest thing under the pointer glows, and the
// list of everything the ray goes through goes to the interface (and the
// log, once a press). Else, on foot: what the walker looks at and could
// do something with, within reach, glows softly
static void update_pick(testbed_t *game)
{
	static const float debug_color[3] = { 1.0f, 0.85f, 0.35f };
	static const float interact_color[3] = { 0.55f, 0.75f, 1.0f };
	bool held = (game->input.pick_held && !game->input.pointer_over_ui) || game->command_line.pick;
	pick_t pick;

	game->pick_line_count = 0;
	objects_set_highlight(&game->potop->world.objects, -1, NULL);
	game->potop->world.actors.highlight_id = 0;
	if (!held)
	{
		game->pick_was_held = false;
		if (game->potop->camera.first_person && game->potop->settings.tools.interact_highlight)
		{
			pick_along_ray(game, OBJECTS_RAY_INTERACTIVE, game->potop->settings.tools.interact_meters, &pick);
			if (pick.count > 0)
			{
				highlight_hit(game, &pick.hits[0], interact_color);
			}
		}
		return;
	}
	{
		uint64_t start = SDL_GetPerformanceCounter();

		pick_along_ray(game, OBJECTS_RAY_ALL, 0.0f, &pick);
		game->pick_microseconds = (float)((double)(SDL_GetPerformanceCounter() - start) * 1e6 / (double)SDL_GetPerformanceFrequency());
	}
	if (pick.count > 0)
	{
		highlight_hit(game, &pick.hits[0], debug_color);
	}
	for (int i = 0; i < pick.count && game->pick_line_count < UI_PICK_LINES; i++)
	{
		char *line = game->pick_lines[game->pick_line_count++];

		if (pick.hits[i].is_actor)
		{
			int index = actors_find(&game->potop->world.actors, pick.hits[i].actor_id);
			const actor_t *actor = index >= 0 ? &game->potop->world.actors.actors[index] : NULL;

			SDL_snprintf(line, sizeof(game->pick_lines[0]), "%5.1f m  actor %s #%u%s", (double)pick.hits[i].distance,
				actor != NULL ? game->potop->world.actors.types[actor->type].name : "?", pick.hits[i].actor_id, actor != NULL && actor->interactive ? ", interactive" : "");
		}
		else
		{
			const object_instance_t *instance = &game->potop->world.objects.instances[pick.hits[i].instance];

			SDL_snprintf(line, sizeof(game->pick_lines[0]), "%5.1f m  %s #%u%s%s%s", (double)pick.hits[i].distance,
				game->potop->world.objects.types.types[instance->type].name, instance->id,
				instance->body >= 0 ? (instance->state & OBJECT_STATE_FREE ? ", loose" : ", body") : "",
				instance->damage == OBJECT_DAMAGE_DESTROYED ? ", destroyed" : "",
				instance->state & OBJECT_STATE_INTERACTIVE ? ", interactive" : "");
		}
	}
	if (game->pick_line_count < UI_PICK_LINES)
	{
		if (pick.ground_distance >= 0.0f)
		{
			SDL_snprintf(game->pick_lines[game->pick_line_count++], sizeof(game->pick_lines[0]), "%5.1f m  ground", (double)pick.ground_distance);
		}
		else
		{
			SDL_strlcpy(game->pick_lines[game->pick_line_count++], "no ground (sky)", sizeof(game->pick_lines[0]));
		}
	}
	if (!game->pick_was_held)
	{
		SDL_Log("Pick: %d things under the pointer (the ray took %.0f microseconds)", game->pick_line_count, (double)game->pick_microseconds);
		for (int i = 0; i < game->pick_line_count; i++)
		{
			SDL_Log("  %s", game->pick_lines[i]);
		}
	}
	game->pick_was_held = true;
}

// What an effect is put on: an object's box (its model's bounds, turned
// and scaled as it stands)
static void object_box(const testbed_t *game, const object_instance_t *instance, float position[3], float rotation[4], float extents[3])
{
	const object_type_t *type = &game->potop->world.objects.types.types[instance->type];
	const object_model_t *model = &game->potop->world.objects.models[type->model];
	const float *q = instance->rotation;
	vec3_t middle = vec3_scale(vec3_add(model->bounds_minimum, model->bounds_maximum), 0.5f * instance->scale);
	// The middle turned with the object: v + 2 w (q x v) + 2 q x (q x v)
	vec3_t cross_1 = vec3_make(q[1] * middle.z - q[2] * middle.y, q[2] * middle.x - q[0] * middle.z, q[0] * middle.y - q[1] * middle.x);
	vec3_t cross_2 = vec3_make(q[1] * cross_1.z - q[2] * cross_1.y, q[2] * cross_1.x - q[0] * cross_1.z, q[0] * cross_1.y - q[1] * cross_1.x);
	vec3_t turned = vec3_add(middle, vec3_add(vec3_scale(cross_1, 2.0f * q[3]), vec3_scale(cross_2, 2.0f)));

	position[0] = instance->x + turned.x;
	position[1] = objects_instance_base_y(instance) + turned.y;
	position[2] = instance->z + turned.z;
	SDL_memcpy(rotation, instance->rotation, sizeof(float) * 4);
	extents[0] = 0.5f * (model->bounds_maximum.x - model->bounds_minimum.x) * instance->scale;
	extents[1] = 0.5f * (model->bounds_maximum.y - model->bounds_minimum.y) * instance->scale;
	extents[2] = 0.5f * (model->bounds_maximum.z - model->bounds_minimum.z) * instance->scale;
}

// An actor's box: its height and width as the navigation sees them
static void actor_box(const testbed_t *game, const actor_t *actor, float position[3], float rotation[4], float extents[3])
{
	navigation_agent_t agent = actors_agent(&game->potop->world.actors, actor->type);

	position[0] = actor->x;
	position[1] = actor->y + 0.5f * agent.height_meters;
	position[2] = actor->z;
	SDL_memcpy(rotation, actor->rotation, sizeof(float) * 4);
	extents[0] = agent.radius_meters;
	extents[1] = 0.5f * agent.height_meters;
	extents[2] = agent.radius_meters;
}

// The object whose foot is nearest to (x, z), if the point is on it
static int object_at(const testbed_t *game, float x, float z)
{
	int nearest = -1;
	float nearest_squared = 1e30f;

	for (int i = 0; i < game->potop->world.objects.instance_count; i++)
	{
		const object_instance_t *instance = &game->potop->world.objects.instances[i];
		const object_type_t *type = &game->potop->world.objects.types.types[instance->type];
		float reach;
		float dx = instance->x - x;
		float dz = instance->z - z;
		float squared = dx * dx + dz * dz;

		if (type->model < 0 || squared >= nearest_squared)
		{
			continue;
		}
		reach = SDL_max(0.6f * game->potop->world.objects.models[type->model].horizontal_radius * instance->scale, 0.4f);
		if (squared <= reach * reach)
		{
			nearest_squared = squared;
			nearest = i;
		}
	}
	return nearest;
}

// Plays the effect tool's effect: on the actor under the pointer, else on
// the object at the point, else at the point
static void play_effect(testbed_t *game, int effect, vec3_t point, bool use_ray)
{
	float position[3];
	float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float extents[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t actor_id = 0;
	int object = -1;

	if (use_ray && game->pointer_ray_valid)
	{
		// The first thing the ray meets: an actor or an object (a click on
		// a roof is the house, not the ground behind it)
		pick_t pick;

		pick_along_ray(game, OBJECTS_RAY_ALL, 0.0f, &pick);
		if (pick.count > 0 && pick.hits[0].is_actor)
		{
			actor_id = pick.hits[0].actor_id;
		}
		else if (pick.count > 0)
		{
			object = pick.hits[0].instance;
		}
	}
	else
	{
		object = object_at(game, point.x, point.z);
	}
	particles_emitter_t *emitter;
	uint32_t id;

	if (object >= 0 && game->potop->world.objects.types.types[game->potop->world.objects.instances[object].type].model < 0)
	{
		object = -1;
	}
	if (effect < 0 || effect >= game->potop->world.particles.effect_count)
	{
		return;
	}
	if (actor_id != 0 && actors_find(&game->potop->world.actors, actor_id) >= 0)
	{
		actor_box(game, &game->potop->world.actors.actors[actors_find(&game->potop->world.actors, actor_id)], position, rotation, extents);
	}
	else if (object >= 0)
	{
		object_box(game, &game->potop->world.objects.instances[object], position, rotation, extents);
	}
	else
	{
		position[0] = point.x;
		position[1] = point.y;
		position[2] = point.z;
	}
	id = particles_play_on(&game->potop->world.particles, effect, position, rotation, extents);
	emitter = particles_find_emitter(&game->potop->world.particles, id);
	if (emitter == NULL)
	{
		return;
	}
	// (a continuous one not on an actor stays with the map)
	emitter->kept = !game->potop->world.particles.effects[effect].burst && actor_id == 0;
	if (actor_id != 0)
	{
		emitter->attach = PARTICLES_ATTACH_ACTOR;
		emitter->actor_id = actor_id;
	}
	else if (object >= 0)
	{
		emitter->attach = PARTICLES_ATTACH_OBJECT;
		emitter->object_body = game->potop->world.objects.instances[object].body;
	}
	SDL_Log("Effect %s %s", game->potop->world.particles.effects[effect].name,
		actor_id != 0 ? "on an actor" : object >= 0 ? game->potop->world.objects.types.types[game->potop->world.objects.instances[object].type].name : "at a point");
}

// Effects on things go where the things went (an actor walking, a barrel
// the water carries); ones whose actor is gone stop
static void follow_effects(testbed_t *game)
{
	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		particles_emitter_t *emitter = &game->potop->world.particles.emitters[i];

		if (!emitter->used)
		{
			continue;
		}
		if (emitter->attach == PARTICLES_ATTACH_ACTOR)
		{
			int index = actors_find(&game->potop->world.actors, emitter->actor_id);

			if (index < 0)
			{
				emitter->used = false;
				continue;
			}
			actor_box(game, &game->potop->world.actors.actors[index], emitter->position, emitter->rotation, emitter->extents);
		}
		else if (emitter->attach == PARTICLES_ATTACH_OBJECT && emitter->object_body >= 0
			&& emitter->object_body < game->potop->world.physics.body_capacity && game->potop->world.physics.bodies[emitter->object_body].used)
		{
			int instance = game->potop->world.physics.bodies[emitter->object_body].instance;

			if (instance >= 0 && instance < game->potop->world.objects.instance_count && game->potop->world.objects.instances[instance].body == emitter->object_body)
			{
				object_box(game, &game->potop->world.objects.instances[instance], emitter->position, emitter->rotation, emitter->extents);
			}
		}
	}
}

// The light tool: a light of the chosen kind over the point, or the one
// placed next to it (within 2 m, on the ground or at its height) gone
static void place_or_remove_light(testbed_t *game, vec3_t point)
{
	const tool_state_t *tools = &game->tools;
	float raised = point.y + tools->light_height_meters;
	potop_light_t light;

	if (potop_edit_remove_light(game->potop, point.x, raised, point.z, 2.0f) || potop_edit_remove_light(game->potop, point.x, point.y, point.z, 2.0f))
	{
		return;
	}
	SDL_zero(light);
	light.intensity = tools->light_intensity;
	light.radius_meters = tools->light_radius_meters;
	light.switched = true;
	switch (tools->light_kind)
	{
		case TOOL_LIGHT_FIRE:
			light.color[0] = 1.0f;
			light.color[1] = 0.55f;
			light.color[2] = 0.22f;
			light.flicker = 0.6f;
			light.core_meters = 0.15f;
			light.switched = false;
			break;
		case TOOL_LIGHT_COLD:
			light.color[0] = 0.6f;
			light.color[1] = 0.75f;
			light.color[2] = 1.0f;
			light.core_meters = 0.08f;
			break;
		case TOOL_LIGHT_WHITE:
			light.color[0] = 1.0f;
			light.color[1] = 1.0f;
			light.color[2] = 1.0f;
			light.core_meters = 0.06f;
			break;
		case TOOL_LIGHT_LAMP:
		default:
			light.color[0] = 1.0f;
			light.color[1] = 0.75f;
			light.color[2] = 0.45f;
			light.flicker = 0.15f;
			light.core_meters = 0.08f;
			break;
	}
	if (!potop_edit_place_light(game->potop, point.x, raised, point.z, &light))
	{
		SDL_Log("No room for more placed lights (%d)", LIGHTS_PLACED_MAX);
	}
}

// The plant and clear tools work per rendered frame, not per simulation
// step, so that they also work while the game is paused
static void apply_object_tools(testbed_t *game, float frame_seconds)
{
	tool_state_t *tools = &game->tools;

	if (!game->input.apply_tool_held || !tools->hover_valid || game->input.water_action != WATER_ACTION_NONE)
	{
		return;
	}
	if (tools->current_tool == TOOL_SPRING)
	{
		if (game->input.apply_tool_pressed && !baked_refuses(game))
		{
			potop_edit_toggle_spring(game->potop, tools->hover_point.x, tools->hover_point.z);
		}
		return;
	}
	if (tools->current_tool == TOOL_OBJECT)
	{
		if (game->input.apply_tool_pressed)
		{
			place_chosen_object(game, tools->hover_point);
		}
		return;
	}
	if (tools->current_tool == TOOL_EFFECT)
	{
		if (game->input.apply_tool_pressed)
		{
			play_effect(game, tools->effect, tools->hover_point, true);
		}
		return;
	}
	if (tools->current_tool == TOOL_LIGHT)
	{
		if (game->input.apply_tool_pressed)
		{
			place_or_remove_light(game, tools->hover_point);
		}
		return;
	}
	if (tools->current_tool == TOOL_REMOVE_LAYOUT)
	{
		if (game->input.apply_tool_pressed)
		{
			remove_nearest_layout(game);
		}
		return;
	}
	if (tools->current_tool == TOOL_LAYOUT)
	{
		// One per click
		if (game->input.apply_tool_pressed)
		{
			potop_edit_place_layout(game->potop, tools->layout, tools->hover_point.x, tools->hover_point.z);
		}
		return;
	}
	if (tools->current_tool == TOOL_PLANT)
	{
		potop_edit_plant(game->potop, tools->plant_category, tools->hover_point.x, tools->hover_point.z, tools->brush_radius_meters, frame_seconds);
	}
	else if (tools->current_tool == TOOL_CLEAR)
	{
		potop_edit_clear(game->potop, tools->hover_point.x, tools->hover_point.z, tools->brush_radius_meters);
	}
}

// The inspect tool: the cell under the pointer, or the pinned one
static void update_inspector(testbed_t *game)
{
	inspector_t *inspector = &game->inspector;
	const tool_state_t *tools = &game->tools;
	bool inspecting = tools->current_tool == TOOL_INSPECT;
	int32_t hover_x = -1;
	int32_t hover_y = -1;

	if (tools->hover_valid)
	{
		hover_x = clamp_int((int32_t)SDL_floorf(tools->hover_point.x / game->potop->world.water.cell_size_meters + 0.5f), 0, game->potop->world.water.size - 1);
		hover_y = clamp_int((int32_t)SDL_floorf(tools->hover_point.z / game->potop->world.water.cell_size_meters + 0.5f), 0, game->potop->world.water.size - 1);
	}
	if (inspector->unpin_requested)
	{
		inspector->pinned = false;
		inspector->unpin_requested = false;
	}
	if (inspecting && game->input.apply_tool_pressed && hover_x >= 0 && game->input.water_action == WATER_ACTION_NONE)
	{
		if (inspector->pinned && hover_x == inspector->cell_x && hover_y == inspector->cell_y)
		{
			inspector->pinned = false;
		}
		else
		{
			inspector->pinned = true;
			inspector_set_cell(inspector, hover_x, hover_y);
		}
	}
	if (!inspector->pinned)
	{
		inspector_set_cell(inspector, inspecting ? hover_x : -1, inspecting ? hover_y : -1);
	}
	if (inspecting || inspector->pinned)
	{
		inspector_world_t world;

		world.water = &game->potop->world.water;
		world.terrain = &game->potop->world.terrain;
		world.erosion = &game->potop->world.erosion;
		world.physics = &game->potop->world.physics;
		world.objects = &game->potop->world.objects;
		world.settings = &game->potop->settings;
		world.soils = &game->potop->soils;
		world.step_seconds = 1.0f / (float)SDL_max(1, game->potop->settings.simulation.steps_per_second);
		inspector_describe(inspector, &world);
	}
	if (inspector->log_requested)
	{
		inspector_log(inspector);
		inspector->log_requested = false;
	}
}

static void show_message(testbed_t *game, const char *message);


// Test controls for actors: a left click on an actor (or near one) selects
// it, the following left clicks send it there, escape lets go. Those
// clicks never reach the tools. Shift and ctrl clicks stay the water's.
static void handle_actor_clicks(testbed_t *game)
{
	input_state_t *input = &game->input;

	if (game->selected_actor != 0 && actors_find(&game->potop->world.actors, game->selected_actor) < 0)
	{
		game->selected_actor = 0;
	}
	if (input->apply_tool_pressed && game->tools.hover_valid && input->water_action == WATER_ACTION_NONE && !input->pointer_over_ui)
	{
		vec3_t point = game->tools.hover_point;

		if (game->selected_actor != 0)
		{
			actors_move_to(&game->potop->world.actors, &game->potop->world.navigation, &game->potop->world.terrain, &game->potop->world.water, game->selected_actor, point.x, point.z);
			game->actor_click_held = true;
		}
		else
		{
			// On the actor (a bird up in the air too), else near where the
			// click meets the ground
			uint32_t nearest = game->pointer_ray_valid
				? actors_pick(&game->potop->world.actors, game->pointer_ray_origin, game->pointer_ray_direction) : 0;

			if (nearest == 0)
			{
				nearest = actors_nearest(&game->potop->world.actors, point.x, point.z, 2.0f);
			}

			if (nearest != 0)
			{
				game->selected_actor = nearest;
				game->actor_click_held = true;
			}
		}
	}
	if (game->actor_click_held)
	{
		if (!input->apply_tool_held && !input->apply_tool_pressed)
		{
			game->actor_click_held = false;
		}
		input->apply_tool_pressed = false;
		input->apply_tool_held = false;
		input->apply_tool_released = false;
	}
}




// What the actors report goes to the screen (tests)
static void report_actor_events(testbed_t *game)
{
	actor_event_t event;

	while (actors_poll_event(&game->potop->world.actors, &event))
	{
		int index = actors_find(&game->potop->world.actors, event.id);
		const char *name = index >= 0 ? game->potop->world.actors.types[game->potop->world.actors.actors[index].type].name : "actor";
		char message[128];

		switch (event.type)
		{
			case ACTOR_EVENT_ARRIVED:
				SDL_snprintf(message, sizeof(message), "%s arrived", name);
				break;
			case ACTOR_EVENT_GOT_NEAR:
				SDL_snprintf(message, sizeof(message), "%s cannot get there: as near as it goes", name);
				break;
			default:
				SDL_snprintf(message, sizeof(message), "%s finds no way there", name);
				break;
		}
		SDL_Log("%s at (%.1f, %.1f)", message, (double)event.x, (double)event.z);
		show_message(game, message);
	}
}

static void handle_requests(testbed_t *game)
{
	input_state_t *input = &game->input;

	if (input->select_tool >= 0 && input->select_tool < TOOL_COUNT)
	{
		game->tools.current_tool = (tool_type_t)input->select_tool;
	}
	if (input->select_soil >= 0 && input->select_soil < SOIL_TYPE_COUNT)
	{
		game->tools.current_soil = (soil_type_t)input->select_soil;
	}
	if ((input->place_object || input->remove_object) && game->tools.hover_valid)
	{
		vec3_t point = game->tools.hover_point;

		if (input->remove_object)
		{
			objects_remove_nearest(&game->potop->world.objects, point.x, point.z, SDL_max(game->tools.brush_radius_meters, 2.0f));
		}
		else
		{
			place_single_object(game, point);
		}
	}
	if ((input->place_actor || input->remove_actor) && game->tools.hover_valid)
	{
		vec3_t point = game->tools.hover_point;

		if (input->remove_actor)
		{
			actors_remove_nearest(&game->potop->world.actors, point.x, point.z, SDL_max(game->tools.brush_radius_meters, 3.0f));
		}
		else
		{
			// The chosen type (ctrl + K), or the next one that has a model,
			// facing the camera
			for (int k = 0; k < game->potop->world.actors.type_count; k++)
			{
				int t = (game->actor_type_to_place + k) % game->potop->world.actors.type_count;

				if (game->potop->world.actors.types[t].model >= 0)
				{
					float yaw = SDL_atan2f(game->potop->camera.eye_position.x - point.x, game->potop->camera.eye_position.z - point.z);

					uint32_t id = actors_add(&game->potop->world.actors, &game->potop->world.terrain, t, point.x, point.z, yaw);

					actors_set_name(&game->potop->world.actors, id, game->actor_name_to_place, game->actor_hostile_to_place);
					actors_set_game_id(&game->potop->world.actors, id, (uint32_t)SDL_max(game->actor_game_id_to_place, 0));
					break;
				}
			}
		}
	}
	if (input->cycle_actor_type && game->potop->world.actors.type_count > 0)
	{
		char message[96];

		// The next type that has a model
		for (int k = 1; k <= game->potop->world.actors.type_count; k++)
		{
			int t = (game->actor_type_to_place + k) % game->potop->world.actors.type_count;

			if (game->potop->world.actors.types[t].model >= 0)
			{
				game->actor_type_to_place = t;
				break;
			}
		}
		SDL_snprintf(message, sizeof(message), "K places: %s", game->potop->world.actors.types[game->actor_type_to_place].name);
		show_message(game, message);
	}
	if (input->cycle_actor_action != 0 && !game->potop->camera.first_person)
	{
		const char *action = actors_cycle_actions(&game->potop->world.actors, input->cycle_actor_action);

		if (action != NULL)
		{
			char message[96];

			SDL_snprintf(message, sizeof(message), "Actors: %s", action);
			show_message(game, message);
		}
	}
	if (input->cycle_plant)
	{
		// The first G picks the plant tool, the next ones cycle the
		// categories that have something to plant
		if (game->tools.current_tool == TOOL_PLANT)
		{
			game->tools.plant_category = next_plant_category(game, game->tools.plant_category);
		}
		else if (!object_types_category_plantable(&game->potop->world.objects.types, game->tools.plant_category))
		{
			game->tools.plant_category = next_plant_category(game, game->tools.plant_category);
		}
		game->tools.current_tool = TOOL_PLANT;
	}
	if (input->select_clear)
	{
		game->tools.current_tool = TOOL_CLEAR;
	}
	if (input->cycle_layout && game->potop->world.layouts.count > 0)
	{
		// The first L picks the layout tool, the next ones cycle the layouts
		if (game->tools.current_tool == TOOL_LAYOUT)
		{
			game->tools.layout = (game->tools.layout + 1) % game->potop->world.layouts.count;
		}
		game->tools.layout = clamp_int(game->tools.layout, 0, game->potop->world.layouts.count - 1);
		game->tools.current_tool = TOOL_LAYOUT;
	}
	if (input->request_remove_layout && game->tools.hover_valid)
	{
		remove_nearest_layout(game);
	}
	if (input->request_repopulate)
	{
		world_regrow(&game->potop->world);
	}
	if (input->request_remove_objects)
	{
		objects_clear(&game->potop->world.objects, false);
	}
	if (input->cycle_stamp)
	{
		// The first T picks the stamp tool, the next ones cycle the stamps
		if (game->tools.current_tool == TOOL_STAMP)
		{
			game->tools.current_stamp = (stamp_type_t)((game->tools.current_stamp + 1) % STAMP_COUNT);
		}
		game->tools.current_tool = TOOL_STAMP;
		game->tools.stamp_dragging = false;
	}
	if (input->stamp_height_steps != 0)
	{
		game->tools.stamp_height_meters = clamp_float(game->tools.stamp_height_meters + 0.5f * (float)input->stamp_height_steps, 0.5f, 30.0f);
	}
	handle_stamp_tool(game);
	handle_area_tool(game);
	if (input->brush_size_steps != 0)
	{
		game->tools.brush_radius_meters *= SDL_powf(1.2f, (float)input->brush_size_steps);
		game->tools.brush_radius_meters = clamp_float(game->tools.brush_radius_meters,
			game->potop->settings.tools.brush_minimum_radius_meters, game->potop->settings.tools.brush_maximum_radius_meters);
	}
	if (input->request_regenerate)
	{
		// (a new map at once: no longer the project's)
		game->project_name[0] = '\0';
		ui_set_map_name(&game->ui, "");
		touch_water(game);
		regenerate_terrain(game, pick_random_seed());
	}
	if (input->toggle_spring && game->tools.hover_valid && !baked_refuses(game))
	{
		touch_water(game);
		water_toggle_spring(&game->potop->world.water, game->tools.hover_point.x, game->tools.hover_point.z);
	}
	if (input->toggle_rain)
	{
		touch_water(game);
		game->potop->world.water.raining = !game->potop->world.water.raining;
		SDL_Log("Rain %s", game->potop->world.water.raining ? "on" : "off");
	}
	if (input->bake_sky_occlusion)
	{
		// (the one there is drawn until the new one is done)
		game->potop->world.sky_occlusion.requested = true;
		game->potop->world.sky_occlusion.given_up = false;
		show_message(game, "Working out the sky occlusion");
	}
	if (input->toggle_borders)
	{
		touch_water(game);
		game->potop->world.water.drain_edges = !game->potop->world.water.drain_edges;
		SDL_Log("Map borders: %s", game->potop->world.water.drain_edges ? "drain" : "wall");
		// Water resting against the border has to notice the change
		water_wake_cells(&game->potop->world.water, 0, 0, game->potop->world.water.size - 1, game->potop->world.water.size - 1);
	}
	if (input->toggle_water_model && !game->potop->camera.first_person)
	{
		touch_water(game);
		game->potop->world.water.depth_scaled_pipes = !game->potop->world.water.depth_scaled_pipes;
		SDL_Log("Water model: %s", game->potop->world.water.depth_scaled_pipes ? "depth scaled pipes" : "classic fixed pipes");
		water_wake_cells(&game->potop->world.water, 0, 0, game->potop->world.water.size - 1, game->potop->world.water.size - 1);
	}
	if (input->toggle_pause)
	{
		game->paused = !game->paused;
	}
	if (input->toggle_water_freeze)
	{
		if (game->potop->world.water_frozen)
		{
			touch_water(game);
		}
		else
		{
			game->potop->world.water_frozen = true;
			SDL_Log("Water simulation frozen (F7 or any tool starts it again)");
		}
	}
	if (input->time_scale_steps != 0)
	{
		game->time_scale_index = clamp_int(game->time_scale_index + input->time_scale_steps, 0, TIME_SCALE_COUNT - 1);
	}
	if (input->toggle_wireframe)
	{
		game->potop->world.renderer.wireframe = !game->potop->world.renderer.wireframe;
	}
	if (input->request_diagnostics)
	{
		log_diagnostics(game, true);
	}
	if (input->toggle_navigation_overlay)
	{
		game->potop->world.renderer.navigation_overlay = !game->potop->world.renderer.navigation_overlay;
		game->potop->world.navigation_overlay_seconds = -1e9;
		show_message(game, game->potop->world.renderer.navigation_overlay
			? "Navigation overlay: red objects, orange too low, yellow too steep, blue object tops, violet water; paths green"
			: "Navigation overlay off");
	}
	if (input->toggle_tile_overlay)
	{
		game->potop->world.renderer.tile_overlay = !game->potop->world.renderer.tile_overlay;
	}
	if (input->toggle_thin_highlight)
	{
		game->potop->world.water_renderer.highlight_thin = !game->potop->world.water_renderer.highlight_thin;
	}
	if (input->toggle_debug_view)
	{
		game->potop->world.renderer.debug_view = (terrain_debug_view_t)((game->potop->world.renderer.debug_view + 1) % TERRAIN_DEBUG_VIEW_COUNT);
	}
}

static void update_hover(testbed_t *game, int pixel_width, int pixel_height)
{
	float pixel_density = SDL_GetWindowPixelDensity(game->window);
	vec3_t origin;
	vec3_t direction;

	game->tools.hover_valid = false;
	game->pointer_ray_valid = false;
	if (game->potop->camera.first_person || game->command_line.pick)
	{
		// On foot the tools work where the walker looks: the middle of the
		// screen (and --pick looks there too)
		camera_pixel_ray(&game->potop->camera, 0.5f * (float)pixel_width, 0.5f * (float)pixel_height, pixel_width, pixel_height, &origin,
			&direction);
		game->tools.hover_valid = terrain_raycast(&game->potop->world.terrain, origin, direction, game->potop->camera.far_distance, &game->tools.hover_point);
		game->pointer_ray_valid = true;
		game->pointer_ray_origin = origin;
		game->pointer_ray_direction = direction;
		// The tool put away: choosing another takes it out again
		if (game->hands_empty && (int)game->tools.current_tool != game->hands_empty_tool)
		{
			game->hands_empty = false;
		}
		if (game->hands_empty || weapons_in_hand(&game->potop->session.weapons))
		{
			game->tools.hover_valid = false;
		}
		return;
	}
	if (!game->input.pointer_in_window || game->input.pointer_over_ui)
	{
		return;
	}
	// Mouse coordinates are in window points; the framebuffer may have more
	// pixels than that on high density displays
	camera_pixel_ray(&game->potop->camera, game->input.pointer_x * pixel_density, game->input.pointer_y * pixel_density,
		pixel_width, pixel_height, &origin, &direction);
	game->tools.hover_valid = terrain_raycast(&game->potop->world.terrain, origin, direction, game->potop->camera.far_distance, &game->tools.hover_point);
	game->pointer_ray_valid = true;
	game->pointer_ray_origin = origin;
	game->pointer_ray_direction = direction;
}

// What the current tool is, for the status line (and the title without
// the interface)
static void describe_tool(const testbed_t *game, char *text_out, size_t text_out_size)
{
	const tool_state_t *tools = &game->tools;

	switch (tools->current_tool)
	{
		case TOOL_PLANT:
			SDL_snprintf(text_out, text_out_size, "plant %s, r = %.0f m",
				game->potop->world.objects.types.category_count > 0 ? game->potop->world.objects.types.categories[tools->plant_category] : "nothing",
				(double)tools->brush_radius_meters);
			break;
		case TOOL_LAYOUT:
			SDL_snprintf(text_out, text_out_size, "place %s", game->potop->world.layouts.count > 0 ? game->potop->world.layouts.layouts[tools->layout].name : "nothing");
			break;
		case TOOL_STAMP:
			SDL_snprintf(text_out, text_out_size, "stamp %s %.1f m (%s), r = %.0f m", stamps_name(tools->current_stamp),
				(double)tools->stamp_height_meters, tools_soil_name(tools->current_soil), (double)tools->brush_radius_meters);
			break;
		case TOOL_RAISE:
		case TOOL_PAINT_SOIL:
			SDL_snprintf(text_out, text_out_size, "%s (%s), r = %.0f m", tools_tool_name(tools->current_tool), tools_soil_name(tools->current_soil),
				(double)tools->brush_radius_meters);
			break;
		case TOOL_SPRING:
		case TOOL_REMOVE_LAYOUT:
		case TOOL_INSPECT:
		case TOOL_NONE:
			SDL_strlcpy(text_out, tools_tool_name(tools->current_tool), text_out_size);
			break;
		case TOOL_LIGHT:
			SDL_snprintf(text_out, text_out_size, "light, %d placed", game->potop->world.lights.placed_count);
			break;
		case TOOL_AREA:
			SDL_snprintf(text_out, text_out_size, "area, %d on the map", game->potop->world.areas.count);
			break;
		case TOOL_EFFECT:
			SDL_snprintf(text_out, text_out_size, "effect %s", tools->effect >= 0 && tools->effect < game->potop->world.particles.effect_count
				? game->potop->world.particles.effects[tools->effect].name : "none");
			break;
		case TOOL_OBJECT:
			SDL_snprintf(text_out, text_out_size, "object %s", tools->object_type >= 0 && tools->object_type < game->potop->world.objects.types.type_count
				? game->potop->world.objects.types.types[tools->object_type].name : "none");
			break;
		default:
			SDL_snprintf(text_out, text_out_size, "%s, r = %.0f m", tools_tool_name(tools->current_tool), (double)tools->brush_radius_meters);
			break;
	}
}

static void add_diagnostic(testbed_t *game, const char *format, ...)
{
	va_list arguments;

	if (game->ui.diagnostic_count >= UI_DIAGNOSTIC_LINES)
	{
		return;
	}
	va_start(arguments, format);
	SDL_vsnprintf(game->ui.diagnostics[game->ui.diagnostic_count], UI_DIAGNOSTIC_LINE_LENGTH, format, arguments);
	va_end(arguments);
	game->ui.diagnostic_count++;
}

// The diagnostics window's lines, from the same numbers the log uses
static void fill_diagnostics(testbed_t *game, double frames, double water_cubic_meters, double mud_cubic_meters, int32_t active_tiles,
	int32_t sleeping_tiles, int32_t dry_tiles)
{
	const terrain_renderer_t *renderer = &game->potop->world.renderer;
	double steps = (double)SDL_max(game->potop->world.diagnostics.step_count, 1);
	char text[UI_DIAGNOSTIC_LINE_LENGTH];

	game->ui.diagnostic_count = 0;
	add_diagnostic(game, "#FRAME");
	add_diagnostic(game, "%.0f fps: frame %.1f ms, simulation %.2f ms (%.1f steps), drawing %.2f ms", frames / game->title_timer_seconds,
		1000.0 * game->frame_seconds_sum / frames, 1000.0 * game->simulation_seconds_sum / frames,
		game->simulated_seconds_sum * (double)SDL_max(1, game->potop->settings.simulation.steps_per_second) / frames,
		1000.0 * game->render_seconds_sum / frames);
	// Where the frame's time went on the CPU (profile.h), averaged: a big
	// "acquire" is the CPU waiting for a busy GPU (or vsync); shadows,
	// objects cull and terrain upload are inside render
	{
		char sections[512];
		size_t length;
		size_t start = 0;
		bool first = true;

		if (frame_profile_take_averages(sections, sizeof(sections)) > 0)
		{
			length = SDL_strlen(sections);
			// (lines of about 90 characters, broken after a comma: the
			// window cuts longer ones off)
			while (start < length)
			{
				size_t end = SDL_min(start + 90, length);

				while (end < length && sections[end] != ',')
				{
					end++;
				}
				add_diagnostic(game, "%s%.*s", first ? "cpu ms: " : "        ", (int)(end - start), sections + start);
				first = false;
				start = end + 2;
			}
		}
	}
	add_diagnostic(game, "#GPU, MS PER FRAME");
	if (game->potop->world.gpu_timer.available)
	{
		double total = 0.0;

		text[0] = '\0';
		for (int pass = 0; pass < GPU_PASS_COUNT; pass++)
		{
			char entry[48];
			// Over the last half second, like the rest of the window
			double milliseconds = gpu_timer_recent_average(&game->potop->world.gpu_timer, (gpu_pass_t)pass);

			SDL_snprintf(entry, sizeof(entry), "%s%s %.2f", pass > 0 ? ", " : "", gpu_timer_pass_name((gpu_pass_t)pass), milliseconds);
			SDL_strlcat(text, entry, sizeof(text));
			total += milliseconds;
		}
		add_diagnostic(game, "%s", text);
		add_diagnostic(game, "total %.2f", total);
	}
	else
	{
		add_diagnostic(game, "not measurable here");
	}
	add_diagnostic(game, "#TERRAIN");
	text[0] = '\0';
	for (int level = 0; level < TERRAIN_LOD_LEVEL_COUNT; level++)
	{
		char entry[16];

		SDL_snprintf(entry, sizeof(entry), "%s%d", level > 0 ? "/" : "", renderer->chunks_per_lod_level[level]);
		SDL_strlcat(text, entry, sizeof(text));
	}
	add_diagnostic(game, "chunks per detail level (step 1/2/4/8/16/32): %s, %.2f M triangles", text, (double)renderer->triangles_drawn * 1e-6);
	add_diagnostic(game, "uploads: %.2f ms per frame (%.1f chunks, %.1f far ones put off), water to and from the GPU %.2f ms",
		renderer->upload_milliseconds_sum / (double)SDL_max(renderer->upload_frame_count, 1),
		(double)renderer->uploaded_chunk_count / (double)SDL_max(renderer->upload_frame_count, 1),
		(double)renderer->deferred_chunk_count / (double)SDL_max(renderer->upload_frame_count, 1),
		game->water_upload_milliseconds_sum / (double)SDL_max(renderer->upload_frame_count, 1));
	add_diagnostic(game, "map %dx%d cells of %.2f m, seed %u%s%s", game->potop->world.terrain.size, game->potop->world.terrain.size, (double)game->potop->world.terrain.cell_size_meters,
		game->potop->world.terrain.seed, game->potop->world.map_name[0] != '\0' ? ", saved as " : "", game->potop->world.map_name);
	add_diagnostic(game, "#OBJECTS");
	add_diagnostic(game, "%d drawn of %d, %d draw calls", game->potop->world.objects.drawn_instance_count, game->potop->world.objects.instance_count,
		game->potop->world.objects.draw_call_count);
	add_diagnostic(game, "#PHYSICS");
	add_diagnostic(game, "%d bodies (%d moving, %d fixed), %d awake; %d ground tiles; %.2f ms per step (Box3D %.2f)",
		game->potop->world.physics.body_count, game->potop->world.physics.dynamic_body_count, game->potop->world.physics.static_body_count, game->potop->world.physics.awake_body_count,
		game->potop->world.physics.tile_count_made, game->potop->world.physics.step_milliseconds, game->potop->world.physics.world_step_milliseconds);
	add_diagnostic(game, "torn loose %d, washed away or destroyed %d, laid down by water %d", game->potop->world.physics.uprooted_total,
		game->potop->world.physics.removed_total, game->potop->world.physics.flattened_count);
	add_diagnostic(game, "#WATER");
	add_diagnostic(game, "%.0f m3 of water, %.1f m3 of mud in it", water_cubic_meters, mud_cubic_meters);
	add_diagnostic(game, "tiles: %d active, %d still, %d dry; %d substeps, fastest %.1f m/s%s", active_tiles, sleeping_tiles,
		dry_tiles, game->potop->world.water.substeps_last_step, (double)game->potop->world.water.fastest_flow_last_step, game->potop->world.water_frozen ? "; FROZEN" : "");
	add_diagnostic(game, "per step: water %.2f ms (most %.2f), erosion %.2f ms (most %.2f), %d threads",
		game->potop->world.diagnostics.water_milliseconds_sum / steps, game->potop->world.diagnostics.water_milliseconds_max,
		game->potop->world.diagnostics.erosion_milliseconds_sum / steps, game->potop->world.diagnostics.erosion_milliseconds_max, game->thread_count);
	add_diagnostic(game, "rain %s, borders %s, pipes %s, %d springs", game->potop->world.water.raining ? "on" : "off", game->potop->world.water.drain_edges ? "drain" : "wall",
		game->potop->world.water.depth_scaled_pipes ? "depth scaled" : "fixed", game->potop->world.water.spring_count);
	{
		const ground_overlay_t *overlay = &game->potop->world.ground_overlay;
		float winter = season_snow(game->potop->world.objects.month, game->potop->world.objects.snow_first_month, game->potop->world.objects.snow_last_month);

		add_diagnostic(game, "snow: falling %.2f, winter %.2f, on things %.2f; on the ground: %s, %d passes, %d resets",
			rain_intensity(&game->potop->world.rain, &game->potop->settings.render) * SDL_min(4.0f * winter, 1.0f), winter, game->potop->world.objects.snow_amount,
			!game->potop->settings.render.ground_overlay || !game->potop->settings.render.material_map ? "off"
			: (overlay->pipeline == NULL ? "no shader" : (overlay->resting ? "resting" : "running")), overlay->pass_count, overlay->reset_count);
		add_diagnostic(game, "ice: %s, %d passes, %d resets%s", !game->potop->settings.render.water_ice ? "off"
			: (game->potop->world.ice.pipeline == NULL ? "no shader" : (game->potop->world.ice.resting ? "resting" : (game->potop->world.ice.texture != NULL ? "running" : "waiting"))),
			game->potop->world.ice.pass_count, game->potop->world.ice.reset_count,
			game->potop->world.water_iced ? "; all of the water frozen still (it bears)" : (winter > 0.0f ? "; the water still moves (only thick ice holds it)" : ""));
	}
}

static void update_title(testbed_t *game, double frame_seconds)
{
	game->title_timer_seconds += frame_seconds;
	game->frames_since_title++;
	if (game->title_timer_seconds >= 0.5)
	{
		char title[512];
		double frames = (double)game->frames_since_title;
		int32_t active_tiles;
		int32_t sleeping_tiles;
		int32_t dry_tiles;
		double water_cubic_meters = (double)game->potop->world.water.total_units * TERRAIN_METERS_PER_HEIGHT_UNIT
			* game->potop->world.terrain.cell_size_meters * game->potop->world.terrain.cell_size_meters;
		char speed_text[48];
		char tool_text[96];
		double mud_cubic_meters = 0.0;
		int32_t tile_count = game->potop->world.water.tiles_per_side * game->potop->world.water.tiles_per_side;
		double reached = game->simulated_seconds_sum / SDL_max(game->frame_seconds_sum, 1e-6);
		// The slowest frame of the interval: an average hides the stutters
		double worst_frame_milliseconds = frame_profile_take_worst();

		water_count_tiles(&game->potop->world.water, &active_tiles, &sleeping_tiles, &dry_tiles);
		for (int32_t tile = 0; tile < tile_count; tile++)
		{
			mud_cubic_meters += (double)game->potop->world.water.tile_sediment[tile];
		}
		mud_cubic_meters *= TERRAIN_METERS_PER_HEIGHT_UNIT * game->potop->world.terrain.cell_size_meters * game->potop->world.terrain.cell_size_meters;
		describe_tool(game, tool_text, sizeof(tool_text));
		if (game->potop->camera.first_person && weapons_in_hand(&game->potop->session.weapons))
		{
			weapons_describe(&game->potop->session.weapons, tool_text, (int)sizeof(tool_text));
		}
		game->reached_time_scale = (float)reached;
		if (game->paused)
		{
			SDL_strlcpy(speed_text, "PAUSED", sizeof(speed_text));
		}
		else if (game->input.in_background && game->potop->settings.simulation.pause_in_background)
		{
			SDL_strlcpy(speed_text, "PAUSED (in background)", sizeof(speed_text));
		}
		else if (reached < 0.95 * (double)time_scales[game->time_scale_index])
		{
			// The speed asked for, and the one reached if the simulation
			// cannot keep up (see the time budget in run())
			SDL_snprintf(speed_text, sizeof(speed_text), "x%g (slowed to x%.2f)", (double)time_scales[game->time_scale_index], reached);
		}
		else
		{
			SDL_snprintf(speed_text, sizeof(speed_text), "x%g", (double)time_scales[game->time_scale_index]);
		}

		if (game->ui.available)
		{
			// Milliseconds per frame: render is the CPU side of drawing,
			// GPU what the graphics card spent (when it can be measured);
			// the simulation split into water, erosion and physics
			char gpu_text[32] = "";

			if (game->potop->world.gpu_timer.available)
			{
				double gpu_total = 0.0;

				for (int pass = 0; pass < GPU_PASS_COUNT; pass++)
				{
					gpu_total += gpu_timer_recent_average(&game->potop->world.gpu_timer, (gpu_pass_t)pass);
				}
				SDL_snprintf(gpu_text, sizeof(gpu_text), ", gpu %.1f", gpu_total);
			}
			SDL_snprintf(game->ui.status_text, sizeof(game->ui.status_text),
				"%.0f fps (min %.0f), frame %.1f ms: render %.1f%s, water %.1f, erosion %.1f, physics %.1f  |  %s  |  water %.0f m\xc2\xb3, mud %.1f m\xc2\xb3,"
				" %d active tiles%s  |  %02d:%02d%s  |  rain %s  |  %d objects  |  %s",
				frames / game->title_timer_seconds, 1000.0 / SDL_max(worst_frame_milliseconds, 0.1), 1000.0 * game->frame_seconds_sum / frames, 1000.0 * game->render_seconds_sum / frames, gpu_text,
				1000.0 * game->potop->world.water_seconds_sum / frames, 1000.0 * game->potop->world.erosion_seconds_sum / frames, 1000.0 * game->potop->world.physics_seconds_sum / frames,
				speed_text, water_cubic_meters, mud_cubic_meters, active_tiles, game->potop->world.terrain.baked ? " (a BAKED map: still)" : (game->potop->world.water_iced ? " (water ICED: the cold)" : (game->potop->world.water_frozen ? " (water FROZEN, F7)" : "")),
				(int)game->potop->world.day_hour, (int)(SDL_fmod(game->potop->world.day_hour, 1.0) * 60.0), game->potop->world.day_frozen ? " (frozen)" : "",
				game->potop->world.water.raining ? "on" : "off", game->potop->world.objects.instance_count, tool_text);
			fill_diagnostics(game, frames, water_cubic_meters, mud_cubic_meters, active_tiles, sleeping_tiles, dry_tiles);
			if (game->potop->world.map_name[0] != '\0')
			{
				SDL_snprintf(title, sizeof(title), "Potop - %s", game->potop->world.map_name);
			}
			else
			{
				SDL_snprintf(title, sizeof(title), "Potop - seed %u", game->potop->world.terrain.seed);
			}
		}
		else
		{
			SDL_snprintf(title, sizeof(title),
				"Potop | seed %u | %.0f fps | frame %.1f ms, sim %.2f ms (%.1f steps), render %.2f ms | %s | water %.0f m3, mud %.1f m3, tiles %d active %d still, %d substeps"
				" | rain %s, borders %s, pipes %s, springs %d | %s | objects %d",
				game->potop->world.terrain.seed,
				frames / game->title_timer_seconds,
				1000.0 * game->frame_seconds_sum / frames,
				1000.0 * game->simulation_seconds_sum / frames,
				game->simulated_seconds_sum * (double)SDL_max(1, game->potop->settings.simulation.steps_per_second) / frames,
				1000.0 * game->render_seconds_sum / frames,
				speed_text,
				water_cubic_meters, mud_cubic_meters, active_tiles, sleeping_tiles, game->potop->world.water.substeps_last_step,
				game->potop->world.water.raining ? "on" : "off", game->potop->world.water.drain_edges ? "drain" : "wall",
				game->potop->world.water.depth_scaled_pipes ? "depth" : "fixed", game->potop->world.water.spring_count, tool_text, game->potop->world.objects.instance_count);
		}
		SDL_SetWindowTitle(game->window, title);

		game->title_timer_seconds = 0.0;
		game->frames_since_title = 0;
		gpu_timer_reset_recent(&game->potop->world.gpu_timer);
		game->frame_seconds_sum = 0.0;
		game->simulation_seconds_sum = 0.0;
		game->render_seconds_sum = 0.0;
		game->potop->world.water_seconds_sum = 0.0;
		game->potop->world.erosion_seconds_sum = 0.0;
		game->potop->world.physics_seconds_sum = 0.0;
		game->simulated_seconds_sum = 0.0;
	}
}









// The frame: the outline on the ground from the tools, the selected actor
// or the pinned cell, then the world (world_render)
static void render_frame(testbed_t *game, int pixel_width, int pixel_height)
{
	world_render_options_t options;
	brush_outline_t brush = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

	if (game->tools.hover_valid)
	{
		brush.start_x = game->tools.hover_point.x;
		brush.start_z = game->tools.hover_point.z;
		brush.end_x = brush.start_x;
		brush.end_z = brush.start_z;
		brush.radius = game->tools.brush_radius_meters;
		if (game->tools.current_tool == TOOL_LAYOUT && game->potop->world.layouts.count > 0)
		{
			brush.radius = game->potop->world.layouts.layouts[game->tools.layout].footprint_meters;
		}
		if (game->tools.current_tool == TOOL_SPRING || game->tools.current_tool == TOOL_REMOVE_LAYOUT)
		{
			brush.radius = 2.0f;
		}
		if (game->tools.current_tool == TOOL_INSPECT)
		{
			brush.radius = game->potop->world.terrain.cell_size_meters * 0.6f;
		}
		if (game->tools.current_tool == TOOL_AREA)
		{
			brush.radius = 0.5f;
		}
		// (no tool: no brush, unless shift or ctrl pours or sponges)
		if (game->tools.current_tool == TOOL_NONE && game->input.water_action == WATER_ACTION_NONE)
		{
			brush.radius = 0.0f;
		}
		if (game->tools.current_tool == TOOL_OBJECT && game->tools.object_type >= 0 && game->tools.object_type < game->potop->world.objects.types.type_count
			&& game->potop->world.objects.types.types[game->tools.object_type].model >= 0)
		{
			const object_type_t *type = &game->potop->world.objects.types.types[game->tools.object_type];

			brush.radius = SDL_max(game->potop->world.objects.models[type->model].horizontal_radius * 0.5f * (type->scale_minimum + type->scale_maximum), 0.5f);
		}
		if (game->tools.current_tool == TOOL_STAMP && game->input.water_action == WATER_ACTION_NONE)
		{
			// Preview the stamp's footprint; while a dam or channel is being
			// dragged, around the whole line
			vec3_t start = game->tools.stamp_dragging ? game->tools.stamp_drag_start : game->tools.hover_point;
			potop_stamp_t stamp = current_stamp(game, start, game->tools.hover_point);

			brush.start_x = start.x;
			brush.start_z = start.z;
			brush.radius = potop_stamp_footprint_meters(game->potop, &stamp);
		}
	}
	if (game->selected_actor != 0 && actors_find(&game->potop->world.actors, game->selected_actor) >= 0)
	{
		// The selected actor gets a circle around it
		const actor_t *actor = &game->potop->world.actors.actors[actors_find(&game->potop->world.actors, game->selected_actor)];

		brush.start_x = actor->x;
		brush.start_z = actor->z;
		brush.end_x = actor->x;
		brush.end_z = actor->z;
		brush.radius = 1.2f * actor->scale;
	}
	if (game->inspector.pinned)
	{
		// The pinned cell stays marked wherever the pointer goes
		brush.start_x = (float)game->inspector.cell_x * game->potop->world.terrain.cell_size_meters;
		brush.start_z = (float)game->inspector.cell_y * game->potop->world.terrain.cell_size_meters;
		brush.end_x = brush.start_x;
		brush.end_z = brush.start_z;
		brush.radius = game->potop->world.terrain.cell_size_meters * 0.6f;
	}
	SDL_zero(options);
	options.brush = brush;
	options.paused = game->paused;
	options.capture = game->command_line.screenshot_path != NULL;
	world_render(&game->potop->world, &game->potop->camera, &options, pixel_width, pixel_height);
}

// A new map from the terrain settings (the size too)
static bool new_map(testbed_t *game, uint32_t seed)
{
	game->potop->session.world_map_stale = true;
	if (!resize_world(game, game->potop->settings.terrain.size, game->potop->settings.terrain.cell_size_meters, false))
	{
		return false;
	}
	regenerate_terrain(game, seed != 0 ? seed : pick_random_seed());
	{
		// (on foot it stays on foot, in the middle of the new map)
		bool on_foot = game->potop->camera.first_person;

		camera_reset(&game->potop->camera, &game->potop->settings.camera, &game->potop->world.terrain);
		if (on_foot)
		{
			camera_enter_first_person(&game->potop->camera, &game->potop->world.terrain);
		}
	}
	cloud_paint_reset(&game->potop->world.cloud_paint);
	game->potop->world.map_name[0] = '\0';
	game->project_name[0] = '\0';
	game->potop->world.simulation_accumulator_seconds = 0.0;
	return true;
}

static void set_vsync(const testbed_t *game)
{
	gpu_set_vsync(game->potop->settings.render.vsync && game->command_line.screenshot_path == NULL);
}

// The settings that rebuild (Apply): the textures, the shadow maps; vsync
static void apply_render_settings(testbed_t *game)
{
	world_apply_render_settings(&game->potop->world);
	set_vsync(game);
}

// The settings that are followed as they change (the season, the shadows
// with the sun, the levels of detail; vsync, the brush's size)
static void follow_live_settings(testbed_t *game)
{
	world_follow_settings(&game->potop->world);
	set_vsync(game);
	game->tools.brush_radius_meters = clamp_float(game->tools.brush_radius_meters, game->potop->settings.tools.brush_minimum_radius_meters,
		SDL_max(game->potop->settings.tools.brush_maximum_radius_meters, game->potop->settings.tools.brush_minimum_radius_meters));
}


static void show_message(testbed_t *game, const char *message)
{
	SDL_Log("%s", message);
	ui_show_message(&game->ui, message, game->potop->world.elapsed_seconds);
}

// The time since mark (milliseconds), and mark moved on to now
static double milliseconds_since(uint64_t *mark)
{
	uint64_t now = SDL_GetPerformanceCounter();
	double milliseconds = 1000.0 * (double)(now - *mark) / (double)SDL_GetPerformanceFrequency();

	*mark = now;
	return milliseconds;
}



// Saves the map as name (without the kind: map_file_kind_name adds it),
// baked (for play) or active. A baked map can only be saved baked again.
static bool save_map(testbed_t *game, const char *name, bool baked)
{
	char message[160];
	bool saved;
	uint64_t mark = SDL_GetPerformanceCounter();

	if (game->potop->world.terrain.baked && !baked)
	{
		SDL_snprintf(message, sizeof(message), "\"%s\" is a baked map: it can only be saved baked", name);
		show_message(game, message);
		return false;
	}
	saved = potop_map_save(game->potop, name, baked);
	SDL_Log("Save: %.0f ms", milliseconds_since(&mark));

	if (saved)
	{
		ui_set_map_name(&game->ui, potop_map_name(game->potop));
		SDL_snprintf(message, sizeof(message), baked ? "Baked \"%s\" (for play)" : "Saved \"%s\"", name);
	}
	else
	{
		SDL_snprintf(message, sizeof(message), "Could not save \"%s\" (see the log)", name);
	}
	show_message(game, message);
	return saved;
}


static bool load_map(testbed_t *game, const char *name)
{
	char message[160];
	uint64_t mark = SDL_GetPerformanceCounter();
	potop_load_result_t result = potop_map_load(game->potop, name);

	if (result == POTOP_LOAD_FAILED)
	{
		SDL_snprintf(message, sizeof(message), "Could not load \"%s\" (see the log)", name);
		show_message(game, message);
		if (game->potop->world.terrain.heights == NULL)
		{
			game->input.request_quit = true;
		}
		return false;
	}
	// The tools' strokes end; no project's map now
	game->tools.hover_valid = false;
	game->tools.stamp_dragging = false;
	game->project_name[0] = '\0';
	name = potop_map_name(game->potop);
	ui_set_map_name(&game->ui, name);
	SDL_Log("Load: %.0f ms in all (and the frames after it: their uploads, see the next frame's time)", milliseconds_since(&mark));
	if (result == POTOP_LOAD_DONE)
	{
		SDL_snprintf(message, sizeof(message), "Loaded \"%s\"", name);
	}
	else
	{
		SDL_snprintf(message, sizeof(message), "\"%s\" is damaged: parts of it are missing", name);
	}
	show_message(game, message);
	return result == POTOP_LOAD_DONE;
}

// ---------------------------------------------------------------------------
// Map projects (project.h)

// Saves the map into the project name (made if new), its project.ini kept
// up: the project open from now on
static bool save_project(testbed_t *game, const char *name)
{
	char message[192];
	char path[1024];
	project_info_t info;
	const terrain_t *terrain = &game->potop->world.terrain;
	bool baked = terrain->baked;
	uint64_t mark = SDL_GetPerformanceCounter();
	bool saved;

	if (!project_read_info(name, &info) || info.created_seconds == 0)
	{
		SDL_Time now = 0;

		SDL_GetCurrentTime(&now);
		info.created_seconds = (int64_t)(now / SDL_NS_PER_SECOND);
	}
	info.seed = terrain->seed;
	info.size_cells = terrain->size;
	info.cell_size_meters = terrain->cell_size_meters;
	info.baked = baked;
	info.max_height_meters = terrain->max_height_meters;
	info.erosion_droplets = game->potop->settings.terrain.erosion_droplets;
	saved = project_path(name, PROJECT_MAP_FILE, path, sizeof(path)) && potop_map_save_file(game->potop, path, name, baked)
		&& project_write_info(&info);
	// Its sketch, if one is painted
	if (saved && terrain_sketch_painted(&game->sketch) && project_path(name, PROJECT_SKETCH_FOLDER, path, sizeof(path))
		&& !terrain_sketch_save(&game->sketch, path))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Project \"%s\": its sketch could not be saved", name);
	}
	SDL_Log("Project save: %.0f ms", milliseconds_since(&mark));
	if (saved)
	{
		SDL_strlcpy(game->project_name, name, sizeof(game->project_name));
		ui_set_map_name(&game->ui, name);
		SDL_snprintf(message, sizeof(message), "Saved project \"%s\"", name);
	}
	else
	{
		SDL_snprintf(message, sizeof(message), "Could not save project \"%s\" (see the log)", name);
	}
	show_message(game, message);
	return saved;
}

// Bakes the project open: baked.potop in it, and a copy in the saves as
// NAME_baked for the game
static bool bake_project(testbed_t *game)
{
	char message[192];
	char path[1024];
	char saved_path[1024];
	char file_name[MAP_FILE_NAME_LENGTH + 16];
	uint64_t mark = SDL_GetPerformanceCounter();
	bool baked;
	bool copied = false;

	if (game->project_name[0] == '\0')
	{
		show_message(game, "No project open to bake");
		return false;
	}
	baked = project_path(game->project_name, PROJECT_BAKE_FILE, path, sizeof(path))
		&& potop_map_save_file(game->potop, path, game->project_name, true);
	if (baked)
	{
		map_file_kind_name(game->project_name, true, file_name, sizeof(file_name));
		copied = map_file_path(file_name, saved_path, sizeof(saved_path)) && SDL_CopyFile(path, saved_path);
	}
	SDL_Log("Project bake: %.0f ms", milliseconds_since(&mark));
	if (!baked)
	{
		SDL_snprintf(message, sizeof(message), "Could not bake \"%s\" (see the log)", game->project_name);
	}
	else if (!copied)
	{
		SDL_snprintf(message, sizeof(message), "Baked \"%s\", but not copied into the saves: %s", game->project_name, SDL_GetError());
	}
	else
	{
		SDL_snprintf(message, sizeof(message), "Baked \"%s\": in the project, and for the game as \"%s\"", game->project_name, file_name);
	}
	show_message(game, message);
	return baked && copied;
}

// Opens a project: its map; the project open from now on
static bool open_project(testbed_t *game, const char *name)
{
	char message[192];
	char path[1024];
	uint64_t mark = SDL_GetPerformanceCounter();
	potop_load_result_t result = POTOP_LOAD_FAILED;

	if (project_path(name, PROJECT_MAP_FILE, path, sizeof(path)))
	{
		result = potop_map_load_file(game->potop, path, name);
	}
	if (result == POTOP_LOAD_FAILED)
	{
		SDL_snprintf(message, sizeof(message), "Could not open project \"%s\" (see the log)", name);
		show_message(game, message);
		if (game->potop->world.terrain.heights == NULL)
		{
			game->input.request_quit = true;
		}
		return false;
	}
	game->tools.hover_valid = false;
	game->tools.stamp_dragging = false;
	SDL_strlcpy(game->project_name, name, sizeof(game->project_name));
	ui_set_map_name(&game->ui, name);
	// Its sketch (none: a clean one), and the sketch window's fields from it
	if (game->sketch.planes != NULL)
	{
		project_info_t info;

		if (!project_path(name, PROJECT_SKETCH_FOLDER, path, sizeof(path)) || !terrain_sketch_load(&game->sketch, path))
		{
			terrain_sketch_clear(&game->sketch);
		}
		game->ui.sketch_known = false;
		if (project_read_info(name, &info) && info.erosion_droplets > 0.0f)
		{
			game->potop->settings.terrain.erosion_droplets = info.erosion_droplets;
		}
	}
	SDL_Log("Project open: %.0f ms", milliseconds_since(&mark));
	SDL_snprintf(message, sizeof(message), result == POTOP_LOAD_DONE ? "Opened project \"%s\"" : "Project \"%s\": its map is damaged, parts are missing",
		name);
	show_message(game, message);
	return result == POTOP_LOAD_DONE;
}

// The land's settings the sketch window says (its highest ground, its
// erosion), and its seed (none typed: the map's)
static void sketch_settings(const testbed_t *game, terrain_settings_t *settings_out, uint32_t *seed_out)
{
	*settings_out = game->potop->settings.terrain;
	settings_out->max_height_meters = SDL_max(game->ui.sketch_max_height, 1.0f);
	settings_out->erosion_droplets = SDL_max(game->ui.sketch_erosion, 0.0f);
	*seed_out = ui_sketch_seed(&game->ui);
	if (*seed_out == 0)
	{
		*seed_out = game->potop->world.terrain.seed;
	}
}

static void update_sketch_view(testbed_t *game)
{
	terrain_settings_t settings;
	uint32_t seed;

	if (game->sketch.planes == NULL)
	{
		return;
	}
	sketch_settings(game, &settings, &seed);
	sketch_view_update(&game->sketch_view, &game->sketch, &settings, seed, ui_sketch_size(&game->ui), SDL_max(game->ui.sketch_cell_meters, 0.1f),
		SDL_GetTicks());
}

// A map from the sketch: as the sketch window says (its size, cell, highest
// ground, erosion, seed); a preview a quarter as big (outside any project),
// else into the project of that name (its map replaced)
static bool generate_from_sketch(testbed_t *game, bool preview, const char *name)
{
	terrain_settings_t *terrain = &game->potop->settings.terrain;
	int32_t size = ui_sketch_size(&game->ui);
	float cell = SDL_max(game->ui.sketch_cell_meters, 0.1f);
	terrain_settings_t settings;
	uint32_t seed;
	bool made;
	uint64_t mark = SDL_GetPerformanceCounter();
	char message[192];

	sketch_settings(game, &settings, &seed);
	if (preview)
	{
		int32_t small = SDL_max((size / 4 + 63) / 64 * 64, 256);

		cell *= (float)size / (float)small;
		size = small;
	}
	terrain->size = size;
	terrain->cell_size_meters = cell;
	terrain->max_height_meters = settings.max_height_meters;
	terrain->erosion_droplets = settings.erosion_droplets;
	game->potop->world.generation_sketch = &game->sketch;
	{
		// (a preview is for the land: nothing grown on it, which takes the
		// longest and as much as the whole map's)
		bool populate = game->potop->settings.objects.populate;
		int32_t full_size = ui_sketch_size(&game->ui);
		float full_cell = SDL_max(game->ui.sketch_cell_meters, 0.1f);

		game->potop->settings.objects.populate = populate && !preview;
		made = new_map(game, seed);
		game->potop->settings.objects.populate = populate;
		// (the settings keep the map's own size, not the preview's)
		terrain->size = full_size;
		terrain->cell_size_meters = full_cell;
	}
	game->potop->world.generation_sketch = NULL;
	if (!made)
	{
		game->input.request_quit = true;
		return false;
	}
	SDL_Log("Sketch: %s %d x %d (%.2f m cells), seed %u: %.0f ms", preview ? "previewed" : "made", size, size, (double)cell, seed,
		milliseconds_since(&mark));
	if (preview)
	{
		SDL_snprintf(message, sizeof(message), "Preview, a quarter as big (%d cells of %.1f m): not in a project", size, (double)cell);
		show_message(game, message);
		return true;
	}
	return save_project(game, name);
}

// The projects' asks (the interface's, or the console's)
static void handle_project_requests(testbed_t *game, const ui_requests_t *requests)
{
	if (requests->project_new)
	{
		if (!new_map(game, requests->project_seed))
		{
			game->input.request_quit = true;
		}
		else
		{
			save_project(game, requests->project_name);
		}
	}
	if (requests->project_open)
	{
		open_project(game, requests->project_name);
	}
	if (requests->project_save && game->project_name[0] != '\0')
	{
		save_project(game, game->project_name);
	}
	if (requests->project_save_as)
	{
		save_project(game, requests->project_name);
	}
	if (requests->project_bake)
	{
		bake_project(game);
	}
	if (requests->project_import && load_map(game, requests->import_map))
	{
		save_project(game, requests->project_name);
	}
	if (requests->sketch_preview)
	{
		generate_from_sketch(game, true, NULL);
	}
	if (requests->sketch_generate)
	{
		char clean[MAP_FILE_NAME_LENGTH];

		map_file_clean_name(requests == &game->console_requests ? requests->project_name : game->ui.sketch_name_input, clean, sizeof(clean));
		if (clean[0] != '\0')
		{
			generate_from_sketch(game, false, clean);
		}
	}
}

// The console's "sketch": as the sketch window
static void sketch_command(void *user, const char *arguments)
{
	testbed_t *game = user;
	char verb[16] = "";
	char first[MAP_FILE_NAME_LENGTH] = "";
	float u = 0.5f;
	float v = 0.5f;
	float radius = 0.1f;
	float amount = 1.0f;

	if (game->sketch.planes == NULL)
	{
		SDL_Log("sketch: there is none (no memory)");
		return;
	}
	if (!game->ui.sketch_known)
	{
		// (the window's fields: from the map, as when it opens)
		const terrain_t *terrain = &game->potop->world.terrain;

		game->ui.sketch_size_index = terrain->size <= 1024 ? 0 : (terrain->size <= 2048 ? 1 : (terrain->size <= 4096 ? 2 : 3));
		game->ui.sketch_cell_meters = terrain->cell_size_meters;
		game->ui.sketch_max_height = game->potop->settings.terrain.max_height_meters;
		game->ui.sketch_erosion = game->potop->settings.terrain.erosion_droplets;
		SDL_snprintf(game->ui.sketch_seed_input, sizeof(game->ui.sketch_seed_input), "%u", terrain->seed);
		game->ui.sketch_brush_meters = SDL_max((float)ui_sketch_size(&game->ui) * terrain->cell_size_meters * 0.06f, 20.0f);
		if (game->project_name[0] != '\0')
		{
			SDL_strlcpy(game->ui.sketch_name_input, game->project_name, sizeof(game->ui.sketch_name_input));
		}
		else
		{
			SDL_snprintf(game->ui.sketch_name_input, sizeof(game->ui.sketch_name_input), "map %u", terrain->seed);
		}
		game->ui.sketch_known = true;
	}
	SDL_sscanf(arguments, "%15s %127s %f %f %f %f", verb, first, &u, &v, &radius, &amount);
	if (SDL_strcmp(verb, "open") == 0)
	{
		game->ui.sketch_open = true;
	}
	else if (SDL_strcmp(verb, "clear") == 0)
	{
		terrain_sketch_clear(&game->sketch);
	}
	else if (SDL_strcmp(verb, "paint") == 0)
	{
		int preset = sketch_preset_find(first);

		if (preset < 0)
		{
			SDL_Log("sketch paint: no region \"%s\" (plain, hills, mountains, valley, lake, erase)", first);
			return;
		}
		terrain_sketch_paint(&game->sketch, preset < SKETCH_PRESET_COUNT ? sketch_presets[preset].region : TERRAIN_SKETCH_NONE, u, v, radius);
	}
	else if (SDL_strcmp(verb, "seed") == 0)
	{
		SDL_strlcpy(game->ui.sketch_seed_input, first, sizeof(game->ui.sketch_seed_input));
	}
	else if (SDL_strcmp(verb, "size") == 0)
	{
		int32_t cells = SDL_atoi(first);

		int old_size = ui_sketch_size(&game->ui);

		game->ui.sketch_size_index = cells <= 1024 ? 0 : (cells <= 2048 ? 1 : (cells <= 4096 ? 2 : 3));
		game->ui.sketch_brush_meters *= (float)ui_sketch_size(&game->ui) / (float)SDL_max(old_size, 1);
	}
	else if (SDL_strcmp(verb, "show") == 0)
	{
		game->sketch_view.show_result = SDL_strcmp(first, "result") == 0;
	}
	else if (SDL_strcmp(verb, "preview") == 0)
	{
		game->console_requests.sketch_preview = true;
	}
	else if (SDL_strcmp(verb, "make") == 0 && first[0] != '\0')
	{
		game->console_requests.sketch_generate = true;
		SDL_strlcpy(game->console_requests.project_name, first, sizeof(game->console_requests.project_name));
	}
	else if (SDL_strcmp(verb, "picture") == 0 && first[0] != '\0')
	{
		// (worked out whole at once: no waiting for the painting to settle)
		game->sketch_view.changed_ticks = 0;
		game->sketch_view.heights_valid = false;
		update_sketch_view(game);
		game->sketch_view.changed_ticks = 0;
		update_sketch_view(game);
		SDL_Log("sketch picture: %s", sketch_view_save_picture(&game->sketch_view, first) ? "saved" : "not saved");
	}
	else
	{
		SDL_Log("sketch open | clear | paint <region> <u> <v> <radius> | seed <n> | size <cells> | preview | make <project> | picture <file>");
	}
}

// The console's "project": as the projects' window
static void project_command(void *user, const char *arguments)
{
	testbed_t *game = user;
	ui_requests_t *requests = &game->console_requests;
	char verb[16] = "";
	char first[MAP_FILE_NAME_LENGTH] = "";
	char second[PROJECT_NAME_LENGTH] = "";
	project_info_t infos[PROJECT_LIST_MAX];
	int count;

	SDL_sscanf(arguments, "%15s %127s %63s", verb, first, second);
	if (SDL_strcmp(verb, "list") == 0 || verb[0] == '\0')
	{
		count = project_list(infos, PROJECT_LIST_MAX);
		SDL_Log("Projects: %d (open: \"%s\")", count, game->project_name);
		for (int i = 0; i < count; i++)
		{
			SDL_Log("  %s: %d cells of %.2f m, seed %u, %s%s", infos[i].name, infos[i].size_cells, (double)infos[i].cell_size_meters, infos[i].seed,
				infos[i].baked ? "baked" : "active", infos[i].has_bake ? ", baked for play" : "");
		}
	}
	else if (SDL_strcmp(verb, "new") == 0 && first[0] != '\0')
	{
		requests->project_new = true;
		requests->project_seed = (uint32_t)SDL_strtoul(second, NULL, 10);
		SDL_strlcpy(requests->project_name, first, sizeof(requests->project_name));
	}
	else if (SDL_strcmp(verb, "open") == 0 && first[0] != '\0')
	{
		requests->project_open = true;
		SDL_strlcpy(requests->project_name, first, sizeof(requests->project_name));
	}
	else if (SDL_strcmp(verb, "save") == 0)
	{
		requests->project_save = first[0] == '\0';
		requests->project_save_as = first[0] != '\0';
		SDL_strlcpy(requests->project_name, first, sizeof(requests->project_name));
	}
	else if (SDL_strcmp(verb, "bake") == 0)
	{
		requests->project_bake = true;
	}
	else if (SDL_strcmp(verb, "import") == 0 && first[0] != '\0' && second[0] != '\0')
	{
		requests->project_import = true;
		SDL_strlcpy(requests->import_map, first, sizeof(requests->import_map));
		SDL_strlcpy(requests->project_name, second, sizeof(requests->project_name));
	}
	else
	{
		SDL_Log("project list | new <name> [seed] | open <name> | save [as name] | bake | import <saved map> <name>");
	}
}

// What the interface asked for this frame
static void handle_ui_requests(testbed_t *game)
{
	const ui_requests_t *requests = &game->ui.requests;

	if (requests->save_map)
	{
		save_map(game, requests->map_name, requests->map_baked);
	}
	if (requests->load_map)
	{
		load_map(game, requests->map_name);
	}
	if (requests->new_map)
	{
		if (!new_map(game, requests->new_map_seed))
		{
			game->input.request_quit = true;
		}
	}
	handle_project_requests(game, requests);
	handle_project_requests(game, &game->console_requests);
	SDL_zero(game->console_requests);
	if (requests->restore_settings)
	{
		game->potop->settings = game->file_settings;
		apply_render_settings(game);
		show_message(game, "Settings back to data/settings.ini");
	}
	if (requests->apply_render)
	{
		apply_render_settings(game);
	}
	if (requests->apply_objects || requests->regrow)
	{
		objects_clear(&game->potop->world.objects, true);
		populate_map(&game->potop->world.objects, &game->potop->world.terrain, &game->potop->world.water, &game->potop->settings.objects);
		material_map_restamp(&game->potop->world.terrain, &game->potop->world.objects);
	}
	if (requests->remove_objects)
	{
		objects_clear(&game->potop->world.objects, false);
	}
	if (requests->save_settings)
	{
		show_message(game, settings_save_overrides(&game->potop->settings, &game->file_settings) ? "Settings saved" : "Could not save the settings");
	}
	if (game->input.request_quick_save)
	{
		// (into the project open; with none, the quick save as before)
		if (game->project_name[0] != '\0')
		{
			save_project(game, game->project_name);
		}
		else
		{
			save_map(game, "quicksave", game->potop->world.terrain.baked);
		}
	}
	if (game->input.request_quick_load && game->project_name[0] != '\0')
	{
		char name[PROJECT_NAME_LENGTH];

		// (the project as last saved)
		SDL_strlcpy(name, game->project_name, sizeof(name));
		open_project(game, name);
	}
	else if (game->input.request_quick_load)
	{
		// (the kind of map there is now first)
		char file_name[MAP_FILE_NAME_LENGTH + 16];

		map_file_kind_name("quicksave", game->potop->world.terrain.baked, file_name, sizeof(file_name));
		load_map(game, potop_map_find(file_name, file_name, (int)sizeof(file_name)) ? file_name : "quicksave");
	}
	if (game->input.toggle_hands && game->potop->camera.first_person)
	{
		game->hands_empty = !game->hands_empty;
		game->hands_empty_tool = (int)game->tools.current_tool;
		SDL_Log(game->hands_empty ? "Tool put away (0 takes it back)" : "Tool taken back");
	}
	if (game->input.toggle_fullscreen)
	{
		// Borderless at the desktop's resolution (no mode change); the
		// swapchain follows the new size on its own
		bool fullscreen = (SDL_GetWindowFlags(game->window) & SDL_WINDOW_FULLSCREEN) != 0;

		SDL_SetWindowFullscreen(game->window, !fullscreen);
	}
	if (game->input.request_escape && game->selected_actor != 0)
	{
		// Escape lets go of the selected actor before anything else
		game->selected_actor = 0;
		game->input.request_escape = false;
	}
	if (game->input.request_escape)
	{
		// Escape closes what is open first (the editor's windows), the
		// topmost first; with nothing open it leaves first person. Quitting is Ctrl+Q (escape was too easy to
		// hit twice while letting go of actors)
		if (!ui_close_windows(&game->ui) && game->potop->camera.first_person)
		{
			camera_leave_first_person(&game->potop->camera, &game->potop->settings.camera);
		}
	}
	if (game->input.toggle_first_person)
	{
		if (game->potop->camera.first_person)
		{
			camera_leave_first_person(&game->potop->camera, &game->potop->settings.camera);
		}
		else
		{
			camera_enter_first_person(&game->potop->camera, &game->potop->world.terrain);
		}
	}
	if (game->input.toggle_interface)
	{
		game->ui.hidden = !game->ui.hidden;
	}
}

// The snow lying on things (objects_t.snow_amount; the ground's own is
// ground_overlay.comp's, by the same rates): it grows as it snows, stays
// through the winter and melts after it. Starting (a map, a jump in the
// month): a thin layer, as far into the winter as it is




// A console command (console.h: the engine runs it), and what it asks of
// the editor: map loads and saves, Apply, a screenshot, quitting
static void run_console_command(testbed_t *game, const char *line)
{
	console_actions_t actions;

	console_run_command(&game->console, game->potop, line, &actions);
	if (actions.apply_render)
	{
		game->ui.requests.apply_render = true;
	}
	if (actions.load_map)
	{
		game->ui.requests.load_map = true;
		SDL_strlcpy(game->ui.requests.map_name, actions.map_name, sizeof(game->ui.requests.map_name));
	}
	if (actions.save_map)
	{
		game->ui.requests.save_map = true;
		game->ui.requests.map_baked = actions.map_baked;
		SDL_strlcpy(game->ui.requests.map_name, actions.map_name, sizeof(game->ui.requests.map_name));
	}
	if (actions.screenshot)
	{
		game->input.request_screenshot = true;
	}
	if (actions.quit)
	{
		game->input.request_quit = true;
	}
}

















// The weapon's keys, first person: 1 brings it up or puts it away (instead
// of choosing the first tool), R reloads while it is in hand (instead of the
// rain), the left button fires (the tools do not see it then). Out of first
// person it is put away at once


static void update_interface(testbed_t *game, double frame_seconds)
{
	ui_view_t view;

	SDL_zero(view);
	view.settings = &game->potop->settings;
	view.tools = &game->tools;
	view.input = &game->input;
	view.water = &game->potop->world.water;
	view.object_types = &game->potop->world.objects.types;
	view.layouts = &game->potop->world.layouts;
	view.seed = game->potop->world.terrain.seed;
	view.project_name = game->project_name;
	view.map_baked = game->potop->world.terrain.baked;
	view.paused = game->paused;
	view.time_scale = time_scales[game->time_scale_index];
	view.reached_time_scale = game->paused || game->reached_time_scale <= 0.0f ? view.time_scale : game->reached_time_scale;
	view.object_count = game->potop->world.objects.instance_count;
	view.inspector = &game->inspector;
	view.particles = &game->potop->world.particles;
	view.lights = &game->potop->world.lights;
	view.areas = &game->potop->world.areas;
	view.terrain = &game->potop->world.terrain;
	view.view_projection = game->potop->camera.view_projection;
	view.objects_hidden = &game->potop->world.objects.hidden;
	view.fireflies = &game->potop->settings.render.fireflies;
	view.no_terrain_occlusion = &game->potop->world.objects.no_terrain_occlusion;

	view.far_shadow_off = &game->potop->world.far_shadow_off;
	view.physics_off = &game->potop->world.physics_off;
	view.aurora_on = &game->potop->world.aurora.on;
	view.raining = &game->potop->world.water.raining;
	view.rainbow_on = &game->potop->world.rain.rainbow_on;
	view.month = &game->potop->settings.render.month;
	view.snow_first_month = &game->potop->settings.render.snow_first_month;
	view.snow_last_month = &game->potop->settings.render.snow_last_month;
	view.tips_first_month = &game->potop->settings.render.tips_first_month;
	view.tips_last_month = &game->potop->settings.render.tips_last_month;
	view.bloom_first_month = &game->potop->settings.render.bloom_first_month;
	view.bloom_last_month = &game->potop->settings.render.bloom_last_month;
	view.impostors_tinted = &game->potop->world.objects.impostors_tinted;

	view.near_shadow_off = &game->potop->world.near_shadow_off;
	view.cloud_paint = &game->potop->world.cloud_paint;
	view.sketch = game->sketch.planes != NULL ? &game->sketch : NULL;
	view.sketch_view = &game->sketch_view;
	view.day_hour = game->potop->settings.render.day_cycle ? &game->potop->world.day_hour : NULL;
	view.day_frozen = &game->potop->world.day_frozen;
	view.sky_eye[0] = game->potop->world.sky_eye[0];
	view.sky_eye[1] = game->potop->world.sky_eye[1];
	view.console = &game->console;
	view.actor_name = game->actor_name_to_place;
	view.actor_game_id = &game->actor_game_id_to_place;
	view.actor_hostile = &game->actor_hostile_to_place;
	view.pick_line_count = game->pick_line_count;
	SDL_memcpy(view.pick_lines, game->pick_lines, sizeof(view.pick_lines));
	(void)frame_seconds;
	ui_update(&game->ui, &view, game->potop->world.elapsed_seconds);
	// The sketch's picture, while it is painted
	if (game->ui.sketch_open)
	{
		update_sketch_view(game);
	}
	// After the interface cleared its requests: a command may ask too
	{
		char command[CONSOLE_INPUT_LENGTH];

		while (console_take_command(&game->console, command, sizeof(command)))
		{
			run_console_command(game, command);
		}
	}
	game->input.pointer_over_ui = game->ui.available && !game->ui.hidden && game->ui.backend.mouse_captured;
	if (game->input.pointer_over_ui)
	{
		// A click that went to the interface does not start a stroke
		game->input.apply_tool_pressed = false;
	}
}

static void push_mouse_event(testbed_t *game, Uint32 type, float x, float y)
{
	SDL_Event event;

	SDL_zero(event);
	event.type = type;
	if (type == SDL_EVENT_MOUSE_MOTION)
	{
		event.motion.windowID = SDL_GetWindowID(game->window);
		event.motion.x = x;
		event.motion.y = y;
	}
	else if (type == SDL_EVENT_MOUSE_WHEEL)
	{
		event.wheel.windowID = SDL_GetWindowID(game->window);
		event.wheel.y = 1.0f;
		event.wheel.mouse_x = x;
		event.wheel.mouse_y = y;
	}
	else
	{
		event.button.windowID = SDL_GetWindowID(game->window);
		event.button.button = SDL_BUTTON_LEFT;
		event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
		event.button.clicks = 1;
		event.button.x = x;
		event.button.y = y;
	}
	SDL_PushEvent(&event);
}

// --ui-test: made-up mouse events, frame by frame, before the frame's
// events are read. A click on the brush panel's Dig button must select the
// tool and not reach the map; a click on the map must; the wheel over the
// panel must not zoom.
static void push_key_event(testbed_t *game, SDL_Keycode key, SDL_Scancode scancode)
{
	SDL_Event event;

	SDL_zero(event);
	event.type = SDL_EVENT_KEY_DOWN;
	event.key.windowID = SDL_GetWindowID(game->window);
	event.key.key = key;
	event.key.scancode = scancode;
	event.key.down = true;
	SDL_PushEvent(&event);
	event.type = SDL_EVENT_KEY_UP;
	event.key.down = false;
	SDL_PushEvent(&event);
}

static void push_text_event(testbed_t *game, const char *text)
{
	SDL_Event event;

	SDL_zero(event);
	event.type = SDL_EVENT_TEXT_INPUT;
	event.text.windowID = SDL_GetWindowID(game->window);
	// A literal: it outlives the event
	event.text.text = text;
	SDL_PushEvent(&event);
}

static bool ui_test_check(testbed_t *game, bool ok, const char *what)
{
	if (!ok)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI test: %s", what);
		game->command_line.ui_test_result = 2;
	}
	return ok;
}

static void ui_test_frame(testbed_t *game, int frame)
{
	static float distance_before;
	command_line_t *test = &game->command_line;
	struct nk_rect panel = game->ui.brushes_bounds;
	// The Dig button: the middle one of the first row of tools
	float button_x = (panel.x + panel.w * 0.5f) * game->ui.style.scale;
	float button_y = (panel.y + 76.0f) * game->ui.style.scale;
	float map_x = (float)game->command_line.window_width * 0.6f;
	float map_y = (float)game->command_line.window_height * 0.6f;

	if (test->ui_test_result != 0 || !game->ui.available)
	{
		return;
	}
	switch (frame)
	{
		case 2:
			push_mouse_event(game, SDL_EVENT_MOUSE_MOTION, button_x, button_y);
			break;
		case 4:
			push_mouse_event(game, SDL_EVENT_MOUSE_BUTTON_DOWN, button_x, button_y);
			break;
		case 5:
			if (game->input.apply_tool_held)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI test: the click on the panel reached the map");
				test->ui_test_result = 2;
			}
			push_mouse_event(game, SDL_EVENT_MOUSE_BUTTON_UP, button_x, button_y);
			break;
		case 7:
			if (game->tools.current_tool != TOOL_DIG)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI test: the Dig button did not select digging (tool %d)", game->tools.current_tool);
				test->ui_test_result = 2;
				break;
			}
			distance_before = game->potop->camera.distance_meters;
			push_mouse_event(game, SDL_EVENT_MOUSE_WHEEL, button_x, button_y);
			break;
		case 9:
			if (game->potop->camera.distance_meters != distance_before)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI test: the wheel over the panel zoomed");
				test->ui_test_result = 2;
				break;
			}
			push_mouse_event(game, SDL_EVENT_MOUSE_MOTION, map_x, map_y);
			break;
		case 11:
			push_mouse_event(game, SDL_EVENT_MOUSE_BUTTON_DOWN, map_x, map_y);
			break;
		case 12:
			if (!game->input.apply_tool_held || !game->tools.hover_valid)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI test: the click on the map did not reach it (held %d, hover %d)",
					game->input.apply_tool_held, game->tools.hover_valid);
				test->ui_test_result = 2;
				break;
			}
			push_mouse_event(game, SDL_EVENT_MOUSE_BUTTON_UP, map_x, map_y);
			break;
		case 14:
			if (game->input.apply_tool_held)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI test: the button stayed held after the release");
				test->ui_test_result = 2;
				break;
			}
			// What Apply does: graphics rebuilt, the interface baked anew
			game->potop->settings.render.texture_size = 256;
			game->potop->settings.render.shadow_map_size = 2048;
			apply_render_settings(game);
			game->ui.style.scale = 1.25f;
			break;
		case 16:
			if (!game->ui.available || game->ui.backend.scale != 1.25f || game->potop->world.shadow.size != 2048)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI test: rebuilding the graphics or the interface failed");
				test->ui_test_result = 2;
				break;
			}
			game->ui.style.scale = 1.0f;
			// The console: ` opens it, Tab completes, Enter runs
			push_key_event(game, SDLK_GRAVE, SDL_SCANCODE_GRAVE);
			push_text_event(game, "`");
			break;
		case 18:
			if (ui_test_check(game, game->console.open && game->console.input_length == 0, "` did not open the console (or typed into it)"))
			{
				push_text_event(game, "set cloud_cov");
				push_key_event(game, SDLK_TAB, SDL_SCANCODE_TAB);
			}
			break;
		case 20:
			if (ui_test_check(game, SDL_strcmp(game->console.input, "set render.cloud_coverage ") == 0, "Tab did not complete the setting"))
			{
				push_text_event(game, "0.3");
				push_key_event(game, SDLK_RETURN, SDL_SCANCODE_RETURN);
			}
			break;
		case 22:
			if (ui_test_check(game, game->potop->settings.render.cloud_coverage == 0.3f && game->console.input_length == 0,
				"the command did not set the coverage"))
			{
				push_key_event(game, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
			}
			break;
		case 24:
			if (ui_test_check(game, !game->console.open, "Escape did not close the console"))
			{
				// On foot: Tab lets the pointer out and takes it back
				push_key_event(game, SDLK_C, SDL_SCANCODE_C);
			}
			break;
		case 27:
			if (ui_test_check(game, game->potop->camera.first_person && SDL_GetWindowRelativeMouseMode(game->window), "C did not hold the pointer"))
			{
				push_key_event(game, SDLK_TAB, SDL_SCANCODE_TAB);
			}
			break;
		case 30:
			if (ui_test_check(game, game->mouse_free && !SDL_GetWindowRelativeMouseMode(game->window), "Tab did not let the pointer out"))
			{
				push_key_event(game, SDLK_TAB, SDL_SCANCODE_TAB);
			}
			break;
		case 33:
			if (ui_test_check(game, !game->mouse_free && SDL_GetWindowRelativeMouseMode(game->window), "Tab did not take the pointer back"))
			{
				push_key_event(game, SDLK_GRAVE, SDL_SCANCODE_GRAVE);
			}
			break;
		case 36:
			if (ui_test_check(game, game->console.open && !SDL_GetWindowRelativeMouseMode(game->window), "the console did not let the pointer out"))
			{
				push_key_event(game, SDLK_GRAVE, SDL_SCANCODE_GRAVE);
			}
			break;
		case 39:
			if (ui_test_check(game, !game->console.open && game->potop->camera.first_person && SDL_GetWindowRelativeMouseMode(game->window),
				"closing the console did not give the pointer back"))
			{
				// The sky window, on foot with the pointer let out: painting
				// the flocks
				push_key_event(game, SDLK_TAB, SDL_SCANCODE_TAB);
				game->ui.sky_open = true;
				game->ui.sky_channel = CLOUD_PAINT_FLOCKS;
				game->ui.sky_brush_neutral = false;
			}
			break;
		case 42:
		case 44:
		{
			struct nk_rect sky = game->ui.sky_bounds;
			float x = (sky.x + sky.w * 0.5f) * game->ui.style.scale;
			float y = (sky.y + sky.h * 0.62f) * game->ui.style.scale;

			push_mouse_event(game, frame == 42 ? SDL_EVENT_MOUSE_MOTION : SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
			break;
		}
		case 54:
		{
			struct nk_rect sky = game->ui.sky_bounds;
			int painted = 0;

			push_mouse_event(game, SDL_EVENT_MOUSE_BUTTON_UP, (sky.x + sky.w * 0.5f) * game->ui.style.scale, (sky.y + sky.h * 0.62f) * game->ui.style.scale);
			for (size_t t = 0; t < (size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE; t++)
			{
				painted += game->potop->world.cloud_paint.values[t * 4 + CLOUD_PAINT_FLOCKS] != 0;
			}
			SDL_Log("UI test: %d patches of flocks painted", painted);
			if (ui_test_check(game, painted > 0, "the sky window did not paint the flocks"))
			{
				test->ui_test_result = 1;
			}
			break;
		}
		default:
			break;
	}
	if (test->ui_test_result != 0)
	{
		SDL_Log("UI test %s", test->ui_test_result == 1 ? "PASSED" : "FAILED");
		game->input.request_quit = true;
	}
}

static void log_physics(const testbed_t *game)
{
	int free_count = 0;
	int flattened = 0;

	for (int i = 0; i < game->potop->world.objects.instance_count; i++)
	{
		free_count += (game->potop->world.objects.instances[i].state & OBJECT_STATE_FREE) ? 1 : 0;
		flattened += game->potop->world.objects.instances[i].flatten > 0.0f ? 1 : 0;
	}
	SDL_Log("Physics: %d bodies, %d awake, %d objects loose, %d torn loose, %d removed, %d laid down, %d ground tiles, %.2f ms per step "
		"(Box3D %.2f)", game->potop->world.physics.body_count, game->potop->world.physics.awake_body_count, free_count, game->potop->world.physics.uprooted_total,
		game->potop->world.physics.removed_total, flattened, game->potop->world.physics.tile_count_made, game->potop->world.physics.step_milliseconds,
		game->potop->world.physics.world_step_milliseconds);
}

// A fixed step as the testbed takes it (world_simulate): the brushes
// first, then the world's
static void testbed_simulation_step(void *user, float step_seconds)
{
	simulation_step(user, step_seconds);
}

static void run(testbed_t *game)
{
	const double counter_frequency = (double)SDL_GetPerformanceFrequency();
	const float step_seconds = 1.0f / (float)SDL_max(1, game->potop->settings.simulation.steps_per_second);
	uint64_t previous_counter = SDL_GetPerformanceCounter();
	int frame_number = 0;

	while (!game->input.request_quit)
	{
		uint64_t frame_start = SDL_GetPerformanceCounter();
		double frame_seconds = (double)(frame_start - previous_counter) / counter_frequency;
		int pixel_width;
		int pixel_height;
		int steps_this_frame = 0;
		uint64_t simulation_start;
		uint64_t render_start;
		SDL_Event event;
		bool pointer_free;
		float particle_seconds;

		previous_counter = frame_start;
		// (after a load, the first frames' times: what the uploads took)
		// (and this frame's steps, one by one: trace_mark below)
		if (game->potop->world.frames_since_load < 4)
		{
			char label[64];

			SDL_Log("Frame %d after the load: %.0f ms", game->potop->world.frames_since_load, 1000.0 * frame_seconds);
			game->potop->world.frames_since_load++;
			if (game->potop->world.frames_since_load < 4)
			{
				SDL_snprintf(label, sizeof(label), "Frame %d after the load, its steps (ms)", game->potop->world.frames_since_load);
				trace_begin(label);
			}
		}
		// After a stall (window dragged, debugger break) do not try to
		// catch up on seconds of simulation
		frame_seconds = SDL_min(frame_seconds, 0.25);
		world_begin_frame(&game->potop->world, frame_seconds);

		if (game->command_line.ui_test)
		{
			ui_test_frame(game, frame_number);
		}
		// The pointer is hidden and held in place on foot (a new map resets
		// the camera, so this follows it rather than the C key), unless Tab
		// let it out or the console is open
		if (!game->potop->camera.first_person)
		{
			game->mouse_free = false;
		}
		pointer_free = game->potop->camera.first_person && (game->mouse_free || game->console.open);
		if (game->window != NULL && SDL_GetWindowRelativeMouseMode(game->window) != (game->potop->camera.first_person && !pointer_free))
		{
			SDL_SetWindowRelativeMouseMode(game->window, game->potop->camera.first_person && !pointer_free);
		}
		game->input.first_person = game->potop->camera.first_person;
		input_begin_frame(&game->input);
		ui_input_begin(&game->ui);
		while (SDL_PollEvent(&event))
		{
			// The interface first: what it keeps does not reach the game. On
			// foot the mouse is the walker's (the pointer is hidden and
			// held in place), so the interface gets none of it.
			bool mouse_event = event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
				|| event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL;

			// The console before everything: ` (the key left of 1, ~ with
			// shift) opens it, and while open it has the keyboard
			if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_GRAVE && !game->console.open)
			{
				if (!event.key.repeat)
				{
					console_set_open(&game->console, true);
				}
				continue;
			}
			if (console_handle_event(&game->console, &event))
			{
				continue;
			}
			if (event.type == SDL_EVENT_TEXT_INPUT && (SDL_strcmp(event.text.text, "`") == 0 || SDL_strcmp(event.text.text, "~") == 0))
			{
				// What the key that closed the console typed
				continue;
			}
			// Tab on foot lets the pointer out for the interface (the keys
			// still walk) and takes it back
			if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_TAB && game->potop->camera.first_person
				&& !(game->ui.available && game->ui.backend.keyboard_captured))
			{
				if (!event.key.repeat)
				{
					game->mouse_free = !game->mouse_free;
				}
				continue;
			}
			if (game->potop->camera.first_person && mouse_event && !pointer_free)
			{
				input_handle_event(&game->input, &event);
				// A release reaches the interface too: a click that closed a
				// window (the conversation's last answer) let go on foot,
				// and the interface would think the button still held, the
				// next click on it lost
				if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
				{
					ui_handle_event(&game->ui, &event);
				}
			}
			else if (!ui_handle_event(&game->ui, &event))
			{
				if (pointer_free && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
				{
					// A click on the world takes the pointer back (with the
					// console open it does nothing)
					if (!game->console.open)
					{
						game->mouse_free = false;
					}
					continue;
				}
				input_handle_event(&game->input, &event);
			}
			else if (event.type == SDL_EVENT_QUIT)
			{
				game->input.request_quit = true;
			}
		}
		ui_input_end(&game->ui);
		game->input.keyboard_captured = game->console.open
			|| (game->ui.available && !game->ui.hidden && game->ui.backend.keyboard_captured);
		input_end_frame(&game->input);
		trace_mark("events");
		if (pointer_free)
		{
			// The pointer moves over the interface: the eye stays put
			game->input.look_x_pixels = 0.0f;
			game->input.look_y_pixels = 0.0f;
		}
		update_interface(game, frame_seconds);
		trace_mark("interface");
		handle_ui_requests(game);
		trace_mark("its requests (save, load, new)");
		follow_live_settings(game);
		trace_mark("settings followed");

		SDL_GetWindowSizeInPixels(game->window, &pixel_width, &pixel_height);
		pixel_width = SDL_max(pixel_width, 1);
		pixel_height = SDL_max(pixel_height, 1);

		if (game->potop->camera.first_person)
		{
			player_walk(&game->potop->session.player, &game->potop->world, &game->potop->camera, &game->input, &game->potop->settings.camera, (float)frame_seconds);
		}
		else
		{
			camera_update(&game->potop->camera, &game->input, &game->potop->settings.camera, &game->potop->world.terrain, (float)frame_seconds, pixel_height);
		}
		camera_compute_matrices(&game->potop->camera, &game->potop->settings.camera, &game->potop->world.terrain, (float)pixel_width / (float)pixel_height);
		player_update_light(&game->potop->session.player, &game->potop->world, &game->potop->camera);
		update_hover(game, pixel_width, pixel_height);
		trace_mark("camera, pointer");
		handle_actor_clicks(game);
		handle_requests(game);
		update_inspector(game);
		apply_object_tools(game, (float)frame_seconds);
		trace_mark("clicks, tools");
		{
			// Screenshots keep the pose they were set up with, unless the
			// actors were sent somewhere: then a fixed thirtieth per frame
			float actor_seconds = game->paused ? 0.0f : (float)frame_seconds;

			if (game->command_line.screenshot_path != NULL)
			{
				actor_seconds = game->command_line.actor_test_goal ? 1.0f / 30.0f : 0.0f;
			}
			world_update_actors(&game->potop->world, actor_seconds);
			report_actor_events(game);
		}
		trace_mark("actors");

		// Fixed step simulation: run as many steps as (scaled) real time
		// demands, but never more than the configured maximum per frame,
		// and stop once the frame's time budget is used up. If the machine
		// cannot keep up, the game slows down instead of spiraling into ever
		// longer frames: a long frame would ask for even more steps in the
		// next one, which would be longer still. Time that cannot be
		// simulated is dropped, not owed.
		simulation_start = SDL_GetPerformanceCounter();
		frame_profile_end(FRAME_UPDATE, frame_start);
		{
			double game_seconds = 0.0;
			const camera_t *camera = &game->potop->camera;

			if (game->input.focus_gained)
			{
				// Back from another window: whatever piled up meanwhile (some
				// window managers stall the swap of unfocused windows) is not
				// worth catching up on
				game->potop->world.simulation_accumulator_seconds = 0.0;
			}
			if (!game->paused && !(game->input.in_background && game->potop->settings.simulation.pause_in_background))
			{
				game_seconds = frame_seconds * time_scales[game->time_scale_index];
			}
			// (headless test runs do one step per frame, however slow the
			// software renderer is)
			steps_this_frame = world_simulate(&game->potop->world, game_seconds, camera->first_person ? camera->walker_x : camera->target_x,
				camera->first_person ? camera->walker_z : camera->target_z, game->command_line.screenshot_path != NULL, testbed_simulation_step, game);
			game->simulated_seconds_sum += (double)steps_this_frame * step_seconds;
		}

		// The weapon, the effects and the particles move with the game's time
		// (not at all when paused); headless runs a step's worth a frame
		particle_seconds = game->paused ? 0.0f : (float)frame_seconds * time_scales[game->time_scale_index];
		if (game->command_line.screenshot_path != NULL)
		{
			particle_seconds = (float)step_seconds;
		}
		follow_effects(game);
		update_pick(game);
		if (game->command_line.spin_degrees != 0.0f)
		{
			game->potop->camera.yaw_degrees = SDL_fmodf(game->potop->camera.yaw_degrees + game->command_line.spin_degrees, 360.0f);
		}
		world_end_frame(&game->potop->world, &game->potop->camera, particle_seconds, game->paused, game->command_line.screenshot_path != NULL);
		game->water_upload_milliseconds_sum += game->potop->world.water_copy_milliseconds;
		render_start = SDL_GetPerformanceCounter();
		gpu_begin_frame();
		trace_mark("frame begun (swapchain)");
		{
			uint64_t draw_start = SDL_GetPerformanceCounter();

			render_frame(game, pixel_width, pixel_height);
			trace_mark("the scene's last steps (returned early)");
			ui_render(&game->ui, gpu.final_texture != NULL ? (int)gpu.final_width : pixel_width,
				gpu.final_texture != NULL ? (int)gpu.final_height : pixel_height);
			frame_profile_end(FRAME_RENDER, draw_start);
			trace_mark("interface drawn");
		}

		frame_number++;
		// (the first frames bake what was not baked yet: part of the start)
		if (frame_number <= 3)
		{
			SDL_Log("Startup: frame %d drawn, %.2f s since the start", frame_number,
				(double)(SDL_GetPerformanceCounter() - game->startup_counter) / (double)SDL_GetPerformanceFrequency());
		}
		{
			bool test_done = game->command_line.screenshot_path != NULL && frame_number >= game->command_line.screenshot_frames;
			uint8_t *pixels = NULL;
			uint32_t shot_width = 0;
			uint32_t shot_height = 0;

			uint64_t present_start = SDL_GetPerformanceCounter();

			gpu_end_frame(game->input.request_screenshot || test_done ? &pixels : NULL, &shot_width, &shot_height);
			trace_mark("submitted and presented (the GPU's own work waited for here)");
			frame_profile_end(FRAME_PRESENT, present_start);
			if (game->input.request_screenshot && pixels != NULL)
			{
				char name[128];

				screenshot_make_name(name, sizeof(name));
				screenshot_save(name, pixels, (int)shot_width, (int)shot_height);
			}
			if (test_done)
			{
				screenshot_save(game->command_line.screenshot_path, pixels, (int)shot_width, (int)shot_height);
			}
			SDL_free(pixels);
		}
		sky_occlusion_between_frames(&game->potop->world.sky_occlusion);
		if (game->command_line.screenshot_path != NULL && frame_number >= game->command_line.screenshot_frames)
		{
			if (game->command_line.save_name != NULL)
			{
				save_map(game, game->command_line.save_name, game->potop->world.terrain.baked);
			}
			if (game->command_line.bake_name != NULL)
			{
				save_map(game, game->command_line.bake_name, true);
			}
			log_render_diagnostics(game);
			log_physics(game);
			if (game->inspector.pinned)
			{
				inspector_log(&game->inspector);
			}
			game->input.request_quit = true;
		}
		// In the background, vsync often does not hold the loop back any
		// more (hidden windows are not presented), so it would spin at full
		// speed; sleep instead
		if (game->input.in_background && game->command_line.screenshot_path == NULL)
		{
			double frame_used = (double)(SDL_GetPerformanceCounter() - frame_start) / counter_frequency;
			double frame_wanted = 1.0 / (double)SDL_max(game->potop->settings.simulation.background_frames_per_second, 1.0f);

			if (frame_used < frame_wanted)
			{
				SDL_DelayNS((Uint64)((frame_wanted - frame_used) * 1e9));
			}
		}

		game->simulation_seconds_sum += (double)(render_start - simulation_start) / counter_frequency;
		game->render_seconds_sum += (double)(SDL_GetPerformanceCounter() - render_start) / counter_frequency;
		game->frame_seconds_sum += frame_seconds;
		trace_end();
		update_title(game, frame_seconds);
		frame_profile_finish();
	}
}

// One step of the water and the erosion for the tests, with the frame's
// copies back
static void test_step(testbed_t *game, float step_seconds)
{
	water_step(&game->potop->world.water, &game->potop->world.terrain, &game->potop->settings.water, &game->potop->soils, step_seconds);
	if (!game->command_line.no_erosion)
	{
		erosion_step(&game->potop->world.erosion, &game->potop->world.terrain, &game->potop->world.water, &game->potop->soils, &game->potop->settings.water, step_seconds,
			(int64_t)(game->potop->settings.render.shadow_erosion_meters * (float)TERRAIN_HEIGHT_UNITS_PER_METER));
	}
	water_gpu_end_frame(&game->potop->world.water, &game->potop->world.terrain, 0.0f, 0.0f);
	water_gpu_poll(&game->potop->world.water);
}

// After rain stops, the map must settle: water drains, films evaporate and
// tiles fall asleep (or dry out) and stay that way. Returns true if, over
// the last ten seconds of a long quiet period, at most an eighth of the
// tiles are still awake.
static bool run_rain_settle_test(testbed_t *game, float step_seconds)
{
	water_t *water = &game->potop->world.water;
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;
	int32_t most_active_at_end = 0;
	int quiet_steps = (int)(240.0f / step_seconds);
	int64_t counted;
	int64_t expected;

	water_clear(water);
	erosion_reset(&game->potop->world.erosion);
	water->drain_edges = true;
	water->raining = true;
	for (int step = 0; step < (int)(10.0f / step_seconds); step++)
	{
		test_step(game, step_seconds);
	}
	water->raining = false;
	for (int step = 0; step < quiet_steps; step++)
	{
		test_step(game, step_seconds);
		if (step % (int)(30.0f / step_seconds) == 0 || step >= quiet_steps - (int)(10.0f / step_seconds))
		{
			int32_t active;
			int32_t sleeping;
			int32_t dry;

			water_count_tiles(water, &active, &sleeping, &dry);
			if (step % (int)(30.0f / step_seconds) == 0)
			{
				SDL_Log("Rain settle test: %3.0f s after the rain: %d active, %d still, %d dry tiles (eroded %lld, deposited %lld, collapsed %lld)",
					(double)((float)step * step_seconds), active, sleeping, dry, (long long)game->potop->world.erosion.eroded_units_total,
					(long long)game->potop->world.erosion.deposited_units_total, (long long)game->potop->world.erosion.collapsed_units_total);
			}
			if (step >= quiet_steps - (int)(10.0f / step_seconds))
			{
				most_active_at_end = SDL_max(most_active_at_end, active);
			}
		}
	}

	// Rain only wets the ground (it adds no water), and the ground's
	// moisture is not part of the water balance
	water_gpu_sync(water, &game->potop->world.terrain);
	counted = water_count_total_units(water);
	expected = water->added_units - water->removed_units - water->drained_units - water->evaporated_units;
	SDL_Log("Rain settle test: at most %d of %d tiles active in the last 10 s; drained %lld, evaporated %lld, ground moisture %lld, dried %lld, "
		"eroded %lld, balance %s",
		most_active_at_end, tile_count, (long long)water->drained_units, (long long)water->evaporated_units,
		(long long)water->moisture_units, (long long)water->dried_units, (long long)game->potop->world.erosion.eroded_units_total,
		counted == expected ? "exact" : "WRONG");
	{
		// Fingerprint with mud and ground (run with one thread to compare)
		uint64_t hash = 14695981039346656037ull;
		size_t cell_count = (size_t)water->size * (size_t)water->size;

		for (size_t i = 0; i < cell_count; i++)
		{
			hash = (hash ^ (uint64_t)(uint32_t)water->depth[i]) * 1099511628211ull;
			hash = (hash ^ (uint64_t)(uint32_t)water->sediment[i]) * 1099511628211ull;
			hash = (hash ^ (uint64_t)(uint32_t)game->potop->world.terrain.heights[i]) * 1099511628211ull;
		}
		SDL_Log("Rain settle test: depth, sediment and ground hash %016llx", (unsigned long long)hash);
	}
	// Streams still draining the valleys keep a few tiles awake (that is
	// real flow); the rest of the map must be quiet
	return counted == expected && most_active_at_end <= tile_count / 8;
}

// Water mass conservation test, no window needed. Pours water, lets it
// flow (first against walls, then draining at the borders) and checks that
// the water in the grid always equals added - removed - drained -
// evaporated, to the last unit. Then checks that the map settles after rain.
static int run_water_test(testbed_t *game)
{
	water_t *water = &game->potop->world.water;
	const float step_seconds = 1.0f / (float)SDL_max(1, game->potop->settings.simulation.steps_per_second);
	float center = (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters * 0.5f;
	bool passed = true;
	uint64_t start_counter;
	double step_milliseconds;

	for (int phase = 0; phase < 2; phase++)
	{
		int64_t counted;
		int64_t expected;

		water_clear(water);
		water->drain_edges = phase == 1;
		start_counter = SDL_GetPerformanceCounter();
		for (int step = 0; step < 600; step++)
		{
			if (step < 150)
			{
				water_apply_brush(water, center, center, 60.0f, 8.0f, step_seconds);
			}
			if (step >= 200 && step < 230)
			{
				water_apply_brush(water, center + 20.0f, center, 15.0f, -3.0f, step_seconds);
			}
			if (phase == 1 && step == 100)
			{
				water->raining = true;
			}
			water_step(water, &game->potop->world.terrain, &game->potop->settings.water, &game->potop->soils, step_seconds);
		}
		water_gpu_sync(water, &game->potop->world.terrain);
		step_milliseconds = 1000.0 * (double)(SDL_GetPerformanceCounter() - start_counter) / (double)SDL_GetPerformanceFrequency() / 600.0;
		water->raining = false;

		counted = water_count_total_units(water);
		expected = water->added_units - water->removed_units - water->drained_units - water->evaporated_units;
		SDL_Log("Water test (%s): added %lld, removed %lld, drained %lld, evaporated %lld, expected %lld, counted %lld, tracked %lld, corrections %lld, %.2f ms/step",
			phase == 0 ? "walls" : "drain + rain",
			(long long)water->added_units, (long long)water->removed_units, (long long)water->drained_units, (long long)water->evaporated_units,
			(long long)expected, (long long)counted, (long long)water->total_units,
			(long long)water->negative_depth_corrections, step_milliseconds);
		{
			// Fingerprint of the result: optimizations that must not change
			// anything (vectorizing a pass) are checked against it
			uint64_t hash = 14695981039346656037ull;
			size_t cell_count = (size_t)water->size * (size_t)water->size;

			for (size_t i = 0; i < cell_count; i++)
			{
				hash = (hash ^ (uint64_t)(uint32_t)water->depth[i]) * 1099511628211ull;
				hash = (hash ^ (uint64_t)(uint32_t)water->sediment[i]) * 1099511628211ull;
			}
			SDL_Log("Water test (%s): depth and sediment hash %016llx", phase == 0 ? "walls" : "drain + rain", (unsigned long long)hash);
		}
		if (counted != expected || counted != water->total_units || water->negative_depth_corrections != 0)
		{
			passed = false;
		}
		if (phase == 0 && water->drained_units != 0)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Water test: water drained through walls");
			passed = false;
		}
	}
	if (!run_rain_settle_test(game, step_seconds))
	{
		passed = false;
	}
	SDL_Log("Water test %s", passed ? "PASSED" : "FAILED");
	return passed ? 0 : 1;
}

// Pours water at the map center for 15 s (the water brush, with the brush
// radius and rate from settings.ini), then
// keeps simulating, logging diagnostics every 30 s. No window. For finding
// out what keeps water awake after a flood; --no-erosion compares with the
// bare water simulation.
static int run_pour_test(testbed_t *game)
{
	const float step_seconds = 1.0f / (float)SDL_max(1, game->potop->settings.simulation.steps_per_second);
	float center = (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters * 0.5f;
	int step_count = (int)((15.0f + game->command_line.pour_test_seconds) / step_seconds);

	game->potop->settings.simulation.diagnostics_interval_seconds = 30.0f;
	water_clear(&game->potop->world.water);
	game->potop->world.water.drain_edges = true;
	SDL_Log("Pour test: %dx%d cells of %.2f m, erosion %s, %s pipes", game->potop->world.terrain.size, game->potop->world.terrain.size, (double)game->potop->world.terrain.cell_size_meters,
		game->command_line.no_erosion ? "off" : "on", game->potop->world.water.depth_scaled_pipes ? "depth scaled" : "fixed");
	for (int step = 0; step < step_count; step++)
	{
		if ((float)step * step_seconds < 15.0f)
		{
			water_apply_brush(&game->potop->world.water, center, center, game->potop->settings.tools.brush_radius_meters,
				game->potop->settings.tools.water_meters_per_second, step_seconds);
		}
		simulation_step(game, step_seconds);
	}
	log_diagnostics(game, true);
	return 0;
}

static int64_t sum_units(const int32_t *values, int64_t count)
{
	int64_t total = 0;

	for (int64_t i = 0; i < count; i++)
	{
		total += values[i];
	}
	return total;
}

// Dam break test, no window needed. Runs the dam scene and checks that:
//   - the overflow washes the notch in the crest down by at least a meter;
//   - water is conserved to the last unit (wetting the ground takes none):
//     surface = added - removed - drained - evaporated;
//   - material is conserved to the last unit: every unit the water picked
//     up is still in the ground, in suspension, or left the map with the
//     water.
static int run_dam_test(testbed_t *game, const scene_dam_info_t *scene)
{
	terrain_t *terrain = &game->potop->world.terrain;
	water_t *water = &game->potop->world.water;
	const float step_seconds = 1.0f / (float)SDL_max(1, game->potop->settings.simulation.steps_per_second);
	int64_t cell_count = (int64_t)terrain->size * terrain->size;
	int64_t solids_before = sum_units(terrain->heights, cell_count) + sum_units(water->sediment, cell_count);
	int32_t notch_index = terrain_index(terrain, scene->notch_cell_x, scene->notch_cell_y);
	float notch_before = (float)terrain->heights[notch_index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
	int step_count = (int)(game->command_line.dam_test_seconds / step_seconds);
	int log_interval = (int)(10.0f / step_seconds);
	uint64_t start_counter = SDL_GetPerformanceCounter();
	float lowest_crest = notch_before;
	int64_t surface_water;
	int64_t expected_water;
	int64_t solids_after;
	bool passed = true;

	for (int step = 1; step <= step_count; step++)
	{
		test_step(game, step_seconds);

		if (step % log_interval == 0 || step == step_count)
		{
			int32_t active;
			int32_t sleeping;
			int32_t dry;
			float crest = 1e30f;

			// The lowest point of the crest line is where the breach is
			for (int32_t x = 0; x < terrain->size; x++)
			{
				int32_t index = terrain_index(terrain, x, scene->notch_cell_y);

				if (terrain->heights[index] > terrain->heights[notch_index] - (int32_t)(20 * TERRAIN_HEIGHT_UNITS_PER_METER)
					&& SDL_abs(x - scene->notch_cell_x) < (int32_t)(25.0f / terrain->cell_size_meters))
				{
					crest = SDL_min(crest, (float)terrain->heights[index] * TERRAIN_METERS_PER_HEIGHT_UNIT);
				}
			}
			lowest_crest = SDL_min(lowest_crest, crest);
			water_count_tiles(water, &active, &sleeping, &dry);
			SDL_Log("Dam test %4.0f s: crest low point %.2f m (%.2f m down), eroded %.1f m3, collapsed %.1f m3, in suspension %.2f m3, "
				"drained water %.0f m3, %d active tiles, %d substeps (fastest flow %.1f m/s), %.2f ms/step",
				(double)((float)step * step_seconds), (double)crest, (double)(notch_before - crest),
				(double)game->potop->world.erosion.eroded_units_total * TERRAIN_METERS_PER_HEIGHT_UNIT * terrain->cell_size_meters * terrain->cell_size_meters,
				(double)game->potop->world.erosion.collapsed_units_total * TERRAIN_METERS_PER_HEIGHT_UNIT * terrain->cell_size_meters * terrain->cell_size_meters,
				(double)sum_units(water->sediment, cell_count) * TERRAIN_METERS_PER_HEIGHT_UNIT * terrain->cell_size_meters * terrain->cell_size_meters,
				(double)water->drained_units * TERRAIN_METERS_PER_HEIGHT_UNIT * terrain->cell_size_meters * terrain->cell_size_meters,
				active, water->substeps_last_step, (double)water->fastest_flow_last_step,
				1000.0 * (double)(SDL_GetPerformanceCounter() - start_counter) / (double)SDL_GetPerformanceFrequency() / (double)step);
		}
	}

	water_gpu_sync(water, terrain);
	surface_water = water_count_total_units(water);
	expected_water = water->added_units - water->removed_units - water->drained_units - water->evaporated_units;
	solids_after = sum_units(terrain->heights, cell_count) + water_count_sediment_units(water) + water->sediment_drained_units;

	SDL_Log("Dam test water: surface %lld, expected %lld (tracked %lld), corrections %lld: %s", (long long)surface_water,
		(long long)expected_water, (long long)water->total_units, (long long)water->negative_depth_corrections,
		surface_water == expected_water && surface_water == water->total_units && water->negative_depth_corrections == 0 ? "exact" : "WRONG");
	SDL_Log("Dam test material: before %lld, after %lld (ground + suspended + %lld drained), corrections %lld: %s",
		(long long)solids_before, (long long)solids_after, (long long)water->sediment_drained_units, (long long)water->sediment_corrections,
		solids_after == solids_before && water->sediment_corrections == 0 ? "exact" : "WRONG");
	SDL_Log("Dam test breach: crest went down %.2f m", (double)(notch_before - lowest_crest));

	if (surface_water != expected_water || surface_water != water->total_units || water->negative_depth_corrections != 0)
	{
		passed = false;
	}
	if (solids_after != solids_before || water->sediment_corrections != 0)
	{
		passed = false;
	}
	if (notch_before - lowest_crest < 1.0f)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Dam test: the dam did not wash out (less than 1 m)");
		passed = false;
	}
	SDL_Log("Dam test %s", passed ? "PASSED" : "FAILED");
	return passed ? 0 : 1;
}

static bool same_cells(const char *name, const void *before, const void *after, size_t size)
{
	if (SDL_memcmp(before, after, size) != 0)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Map test: %s differs after loading", name);
		return false;
	}
	return true;
}

// The map there is now (the map test's, loaded back) baked, loaded over
// another one, and compared: the heights within half a step, the water's
// level within a step, the material map (the shelter within its last
// bit), the objects; then baked again from the baked one, which must give
// the very same
static bool run_baked_map_test(testbed_t *game)
{
	terrain_t *terrain = &game->potop->world.terrain;
	water_t *water = &game->potop->world.water;
	int32_t size = terrain->size;
	size_t cell_count = (size_t)size * (size_t)size;
	int32_t *heights = SDL_malloc(cell_count * sizeof(int32_t));
	int32_t *depths = SDL_malloc(cell_count * sizeof(int32_t));
	uint8_t *materials = SDL_malloc(cell_count * TERRAIN_MATERIAL_BYTES);
	int32_t *baked_heights = SDL_malloc(cell_count * sizeof(int32_t));
	int32_t *baked_depths = SDL_malloc(cell_count * sizeof(int32_t));
	int instance_count;
	int32_t step;
	int64_t worst_height = 0;
	int64_t worst_level = 0;
	int64_t worse_levels = 0;
	int64_t lost_wet = 0;
	int64_t wet = 0;
	int64_t material_differences = 0;
	bool passed = true;

	if (heights == NULL || depths == NULL || materials == NULL || baked_heights == NULL || baked_depths == NULL)
	{
		return false;
	}
	water_gpu_sync(water, terrain);
	terrain_renderer_bake_materials(&game->potop->world.renderer, terrain, &game->potop->world.lit_render);
	SDL_memcpy(heights, terrain->heights, cell_count * sizeof(int32_t));
	SDL_memcpy(materials, terrain->materials, cell_count * TERRAIN_MATERIAL_BYTES);
	// (the water as it is drawn: on a wall's top)
	for (size_t i = 0; i < cell_count; i++)
	{
		int32_t depth = water->depth[i];

		if (water->obstacle_centimeters[i] > 0)
		{
			depth = (float)depth * TERRAIN_METERS_PER_HEIGHT_UNIT > 0.05f
				? depth + (int32_t)water->obstacle_centimeters[i] * WATER_UNITS_PER_OBSTACLE_CENTIMETER : 0;
		}
		depths[i] = depth;
	}
	instance_count = game->potop->world.objects.instance_count;
	// A sky occlusion (a pattern) goes with it
	{
		int layer_size = map_file_sky_layer_size(size, terrain->cell_size_meters);
		size_t layer_bytes = (size_t)layer_size * (size_t)layer_size * SKY_OCCLUSION_LAYERS;
		uint8_t *pattern = SDL_malloc(cell_count);
		uint8_t *layers = SDL_malloc(layer_bytes);
		int bounce_size = map_file_sky_bounce_size(size, terrain->cell_size_meters);
		size_t bounce_bytes = (size_t)bounce_size * (size_t)bounce_size * 4;
		uint8_t *bounce = SDL_malloc(bounce_bytes);

		for (size_t i = 0; pattern != NULL && i < cell_count; i++)
		{
			pattern[i] = (uint8_t)((i * 7u + i / (size_t)size * 13u) & 255u);
		}
		for (size_t i = 0; layers != NULL && i < layer_bytes; i++)
		{
			layers[i] = (uint8_t)((i * 11u + 5u) & 255u);
		}
		for (size_t i = 0; bounce != NULL && i < bounce_bytes; i++)
		{
			bounce[i] = (uint8_t)((i * 5u + 3u) & 255u);
		}
		if (pattern == NULL || layers == NULL || bounce == NULL
			|| !sky_occlusion_upload(&game->potop->world.sky_occlusion, size, pattern, layer_size, layers, bounce_size, bounce))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Baked map test: no sky occlusion to save (left out of the test)");
		}
		SDL_free(pattern);
		SDL_free(layers);
		SDL_free(bounce);
	}
	if (!save_map(game, "map_test", true))
	{
		passed = false;
		goto done;
	}
	game->potop->settings.terrain.size = size / 2;
	new_map(game, 54321u);
	if (!load_map(game, "map_test.baked") || !game->potop->world.terrain.baked || game->potop->world.terrain.size != size)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Baked map test: it did not load back as a baked map of the same size");
		passed = false;
		goto done;
	}
	terrain = &game->potop->world.terrain;
	water = &game->potop->world.water;
	step = terrain->baked_height_step_units;
	for (size_t i = 0; i < cell_count; i++)
	{
		int64_t height_error = SDL_abs(terrain->heights[i] - heights[i]);
		const uint8_t *before = &materials[i * TERRAIN_MATERIAL_BYTES];
		const uint8_t *after = &terrain->materials[i * TERRAIN_MATERIAL_BYTES];

		worst_height = SDL_max(worst_height, height_error);
		if (depths[i] > 0)
		{
			int64_t level_error = SDL_abs((terrain->heights[i] + water->depth[i]) - (heights[i] + depths[i]));

			wet++;
			if (water->depth[i] > 0)
			{
				worst_level = SDL_max(worst_level, level_error);
				worse_levels += level_error > step ? 1 : 0;
			}
			else if (depths[i] > step)
			{
				// (thinner than half a step it may go: a film)
				lost_wet++;
			}
		}
		if (before[0] != after[0] || before[1] != after[1] || before[2] != after[2] || SDL_abs((int)before[3] - (int)after[3]) > 1)
		{
			material_differences++;
		}
	}
	SDL_Log("Baked map test: heights in %.2f mm steps, at most %.2f mm off; %lld wet cells, the level at most %.2f mm off "
		"(%lld more than a step), %lld deeper than a step lost; %lld cells' materials differ; %d objects (saved %d)",
		(double)step * 1000.0 / (double)TERRAIN_HEIGHT_UNITS_PER_METER, (double)worst_height * 1000.0 / (double)TERRAIN_HEIGHT_UNITS_PER_METER,
		(long long)wet, (double)worst_level * 1000.0 / (double)TERRAIN_HEIGHT_UNITS_PER_METER, (long long)worse_levels, (long long)lost_wet,
		(long long)material_differences, game->potop->world.objects.instance_count, instance_count);
	passed = worst_height <= step / 2 + 1 && worse_levels == 0 && lost_wet == 0 && material_differences == 0
		&& game->potop->world.objects.instance_count == instance_count;
	if (!game->potop->world.sky_occlusion.failed)
	{
		uint8_t *loaded_layers = NULL;
		uint8_t *loaded_bounce = NULL;
		uint8_t *loaded = sky_occlusion_download(&game->potop->world.sky_occlusion, &loaded_layers, &loaded_bounce);
		size_t bounce_bytes = (size_t)game->potop->world.sky_occlusion.bounce_size * (size_t)game->potop->world.sky_occlusion.bounce_size * 4;
		size_t layer_bytes = (size_t)game->potop->world.sky_occlusion.layer_size * (size_t)game->potop->world.sky_occlusion.layer_size * SKY_OCCLUSION_LAYERS;
		size_t wrong = 0;

		for (size_t i = 0; loaded != NULL && i < cell_count; i++)
		{
			wrong += loaded[i] != (uint8_t)((i * 7u + i / (size_t)size * 13u) & 255u) ? 1 : 0;
		}
		for (size_t i = 0; loaded_layers != NULL && i < layer_bytes; i++)
		{
			wrong += loaded_layers[i] != (uint8_t)((i * 11u + 5u) & 255u) ? 1 : 0;
		}
		for (size_t i = 0; loaded_bounce != NULL && i < bounce_bytes; i++)
		{
			wrong += loaded_bounce[i] != (uint8_t)((i * 5u + 3u) & 255u) ? 1 : 0;
		}
		SDL_Log("Baked map test: the sky occlusion %s (%zu texels wrong)",
			loaded == NULL || loaded_layers == NULL || loaded_bounce == NULL ? "did not come back" : "came back", wrong);
		passed = passed && loaded != NULL && loaded_layers != NULL && loaded_bounce != NULL && wrong == 0;
		SDL_free(loaded);
		SDL_free(loaded_layers);
		SDL_free(loaded_bounce);
	}

	// Baked again from the baked map: the same
	SDL_memcpy(baked_heights, terrain->heights, cell_count * sizeof(int32_t));
	SDL_memcpy(baked_depths, water->depth, cell_count * sizeof(int32_t));
	SDL_memcpy(materials, terrain->materials, cell_count * TERRAIN_MATERIAL_BYTES);
	if (!save_map(game, "map_test_again", true) || !load_map(game, "map_test_again.baked"))
	{
		passed = false;
		goto done;
	}
	terrain = &game->potop->world.terrain;
	water = &game->potop->world.water;
	passed = same_cells("baked heights", baked_heights, terrain->heights, cell_count * sizeof(int32_t)) && passed;
	passed = same_cells("baked water", baked_depths, water->depth, cell_count * sizeof(int32_t)) && passed;
	passed = same_cells("baked materials", materials, terrain->materials, cell_count * TERRAIN_MATERIAL_BYTES) && passed;
	SDL_Log("Baked map test: %s", passed ? "PASSED" : "FAILED");

done:
	map_file_delete("map_test.baked");
	map_file_delete("map_test_again.baked");
	SDL_free(heights);
	SDL_free(depths);
	SDL_free(materials);
	SDL_free(baked_heights);
	SDL_free(baked_depths);
	return passed;
}

// Saves a map with water, mud, springs and a layout on it, loads another
// one, loads the saved one back and compares everything that was saved
static bool run_weapon_test(testbed_t *game);
static void run_path_benchmark(testbed_t *game);
static bool run_explosion_test(testbed_t *game);

static bool run_inventory_test(testbed_t *game);

static int run_map_test(testbed_t *game)
{
	terrain_t *terrain = &game->potop->world.terrain;
	water_t *water = &game->potop->world.water;
	const float step_seconds = 1.0f / (float)SDL_max(1, game->potop->settings.simulation.steps_per_second);
	float center = (float)terrain->size * terrain->cell_size_meters * 0.5f;
	size_t cell_count = (size_t)terrain->size * (size_t)terrain->size;
	int32_t *copies[6];
	uint8_t *soils = SDL_malloc(cell_count);
	uint16_t *moisture = SDL_malloc(cell_count * sizeof(uint16_t));
	uint8_t *materials = SDL_malloc(cell_count * TERRAIN_MATERIAL_BYTES);
	object_instance_t *instances;
	int instance_count;
	int spring_count;
	int64_t total_units;
	bool passed = true;

	// The editor has no player of its own: a pack for the tests that carry
	// (40 kg, 150 rounds: what the inventory test looks for)
	if (game->potop->session.player_inventory == 0)
	{
		int rounds = items_find(&game->potop->session.items, "rounds_762x39");

		game->potop->session.player_inventory = inventory_create(&game->potop->session.inventories, "You", 40.0f);
		if (rounds >= 0)
		{
			inventory_add(&game->potop->session.inventories, game->potop->session.player_inventory, rounds, 150);
		}
	}
	if (game->potop->world.layouts.count > 0)
	{
		layouts_place(&game->potop->world.layouts, &game->potop->world.objects, terrain, water, 0, center + 40.0f, center, &game->object_random);
	}
	water_toggle_spring(water, center - 30.0f, center);
	// A light and an effect placed: they come back too
	{
		float position[3] = { center + 5.0f, terrain_sample_height_meters(terrain, center + 5.0f, center) + 1.0f, center };
		light_source_t source;

		light_source_reset(&source);
		source.color[0] = 1.0f;
		source.color[1] = 0.5f;
		source.color[2] = 0.25f;
		source.intensity = 2.0f;
		source.radius_meters = 7.0f;
		lights_clear_placed(&game->potop->world.lights);
		lights_place(&game->potop->world.lights, position, &source);
		for (int e = 0; e < game->potop->world.particles.effect_count; e++)
		{
			if (!game->potop->world.particles.effects[e].burst)
			{
				particles_emitter_t *emitter = particles_find_emitter(&game->potop->world.particles, particles_play(&game->potop->world.particles, e, position[0],
					position[1], position[2]));

				if (emitter != NULL)
				{
					emitter->kept = true;
				}
				break;
			}
		}
	}
	for (int step = 0; step < 150; step++)
	{
		water_apply_brush(water, center, center, 25.0f, 3.0f, step_seconds);
		simulation_step(game, step_seconds);
	}
	// The water as the GPU has it
	water_gpu_sync(water, terrain);
	objects_sort(&game->potop->world.objects);
	copies[0] = SDL_malloc(cell_count * sizeof(int32_t));
	copies[1] = SDL_malloc(cell_count * sizeof(int32_t));
	copies[2] = SDL_malloc(cell_count * sizeof(int32_t));
	copies[3] = SDL_malloc(cell_count * sizeof(int32_t));
	copies[4] = SDL_malloc(cell_count * sizeof(int32_t));
	instances = SDL_malloc(sizeof(object_instance_t) * (size_t)SDL_max(game->potop->world.objects.instance_count, 1));
	SDL_memcpy(copies[0], terrain->heights, cell_count * sizeof(int32_t));
	SDL_memcpy(copies[1], terrain->bedrock_heights, cell_count * sizeof(int32_t));
	SDL_memcpy(copies[2], terrain->silt_thickness, cell_count * sizeof(int32_t));
	SDL_memcpy(copies[3], water->depth, cell_count * sizeof(int32_t));
	SDL_memcpy(copies[4], water->sediment, cell_count * sizeof(int32_t));
	copies[5] = NULL;
	SDL_memcpy(soils, terrain->soil_types, cell_count);
	SDL_memcpy(moisture, water->moisture, cell_count * sizeof(uint16_t));
	terrain_renderer_bake_materials(&game->potop->world.renderer, terrain, &game->potop->world.lit_render);
	SDL_memcpy(materials, terrain->materials, cell_count * TERRAIN_MATERIAL_BYTES);
	// (a few of them someone's: kept with the map)
	for (int i = 0; i < game->potop->world.objects.instance_count; i += 7)
	{
		game->potop->world.objects.instances[i].owner = 1000u + (uint32_t)i;
	}
	SDL_memcpy(instances, game->potop->world.objects.instances, sizeof(object_instance_t) * (size_t)game->potop->world.objects.instance_count);
	instance_count = game->potop->world.objects.instance_count;
	spring_count = water->spring_count;
	total_units = water->total_units;

	if (!save_map(game, "map_test", false))
	{
		return 1;
	}
	// Something else in between, of another size
	game->potop->settings.terrain.size = terrain->size / 2;
	new_map(game, 12345u);
	if (!load_map(game, "map_test"))
	{
		passed = false;
	}
	terrain = &game->potop->world.terrain;
	water = &game->potop->world.water;
	objects_sort(&game->potop->world.objects);
	passed = same_cells("heights", copies[0], terrain->heights, cell_count * sizeof(int32_t)) && passed;
	passed = same_cells("bedrock", copies[1], terrain->bedrock_heights, cell_count * sizeof(int32_t)) && passed;
	passed = same_cells("silt", copies[2], terrain->silt_thickness, cell_count * sizeof(int32_t)) && passed;
	passed = same_cells("water depth", copies[3], water->depth, cell_count * sizeof(int32_t)) && passed;
	passed = same_cells("sediment", copies[4], water->sediment, cell_count * sizeof(int32_t)) && passed;
	passed = same_cells("soils", soils, terrain->soil_types, cell_count) && passed;
	passed = same_cells("moisture", moisture, water->moisture, cell_count * sizeof(uint16_t)) && passed;
	passed = same_cells("materials", materials, terrain->materials, cell_count * TERRAIN_MATERIAL_BYTES) && passed;
	{
		int kept = 0;

		for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
		{
			kept += game->potop->world.particles.emitters[i].used && game->potop->world.particles.emitters[i].kept ? 1 : 0;
		}
		if (game->potop->world.lights.placed_count != 1 || game->potop->world.lights.placed[0].source.radius_meters != 7.0f || (game->potop->world.particles.created && kept != 1))
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Map test: %d placed lights (saved 1), %d kept effects (saved 1)", game->potop->world.lights.placed_count,
				kept);
			passed = false;
		}
	}
	if (game->potop->world.objects.instance_count != instance_count || water->spring_count != spring_count || water->total_units != total_units)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Map test: %d objects (saved %d), %d springs (saved %d), %lld water units (saved %lld)",
			game->potop->world.objects.instance_count, instance_count, water->spring_count, spring_count, (long long)water->total_units, (long long)total_units);
		passed = false;
	}
	else
	{
		for (int i = 0; i < instance_count; i++)
		{
			const object_instance_t *a = &instances[i];
			const object_instance_t *b = &game->potop->world.objects.instances[i];

			if (a->id != b->id || a->type != b->type || a->group != b->group || a->x != b->x || a->z != b->z || a->yaw_radians != b->yaw_radians || a->scale != b->scale
				|| a->lift_meters != b->lift_meters || a->owner != b->owner || a->state != b->state || a->damage != b->damage || a->flatten != b->flatten
				|| SDL_memcmp(a->rotation, b->rotation, sizeof(a->rotation)) != 0 || ((a->state & OBJECT_STATE_FREE) && a->y != b->y))
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Map test: object %d differs", i);
				passed = false;
				break;
			}
		}
		{
			// Every id found again where it is, none twice; one taken away
			// is not found, the others still are
			int found = 0;
			bool ids_work;
			uint32_t gone = instance_count > 2 ? game->potop->world.objects.instances[1].id : 0;
			uint32_t last = instance_count > 2 ? game->potop->world.objects.instances[instance_count - 1].id : 0;

			for (int i = 0; i < instance_count; i++)
			{
				found += objects_find_id(&game->potop->world.objects, game->potop->world.objects.instances[i].id) == i ? 1 : 0;
			}
			ids_work = found == instance_count && potop_object_remove(game->potop, gone) && objects_find_id(&game->potop->world.objects, gone) < 0
				&& objects_find_id(&game->potop->world.objects, last) == game->potop->world.objects.instance_count - 1;
			SDL_Log("Object id test: %d of %d kept with the map (and whose they are) and found again, one taken away and not found: %s", found, instance_count,
				ids_work ? "PASSED" : "FAILED");
			passed = passed && ids_work;
		}
	}
	{
		int loose = 0;
		int flattened = 0;

		for (int i = 0; i < instance_count; i++)
		{
			loose += (instances[i].state & OBJECT_STATE_FREE) ? 1 : 0;
			flattened += instances[i].flatten > 0.0f ? 1 : 0;
		}
		SDL_Log("Map test: %d objects (%d loose, %d laid down), %d springs, %lld water units: %s", instance_count, loose, flattened,
			spring_count, (long long)total_units, passed ? "PASSED" : "FAILED");
	}
	for (int i = 0; i < 5; i++)
	{
		SDL_free(copies[i]);
	}
	SDL_free(soils);
	SDL_free(moisture);
	SDL_free(materials);
	SDL_free(instances);
	map_file_delete("map_test.active");
	run_path_benchmark(game);
	if (passed)
	{
		passed = run_weapon_test(game);
	}
	if (passed)
	{
		passed = run_explosion_test(game);
	}
	if (passed)
	{
		passed = run_inventory_test(game);
	}
	if (passed)
	{
		passed = run_baked_map_test(game);
	}
	return passed ? 0 : 1;
}

// Part of --map-test: containers keep their weight limit, move, wear; a
// thing dropped lies as an object, comes back with the map (and the
// player's pack with it) and is picked up again
static bool run_inventory_test(testbed_t *game)
{
	inventories_t *inventories = &game->potop->session.inventories;
	int meat = items_find(&game->potop->session.items, "bear_meat");
	int rifle = items_find(&game->potop->session.items, "ak47");
	int rounds = items_find(&game->potop->session.items, "rounds_762x39");
	bool passed = true;

	if (meat < 0 || rifle < 0 || rounds < 0)
	{
		SDL_Log("Inventory test: items.ini lacks bear_meat, ak47 or rounds_762x39, skipped");
		return true;
	}
	{
		uint32_t small = inventory_create(inventories, "small", 10.0f);
		uint32_t big = inventory_create(inventories, "big", 0.0f);
		int added = inventory_add(inventories, small, meat, 8);
		int moved = inventory_move(inventories, small, big, meat, 4);
		const container_t *container;
		bool worn;

		inventory_add(inventories, big, rifle, 1);
		container = inventory_get_const(inventories, big);
		worn = false;
		for (int i = 0; container != NULL && i < container->entry_count; i++)
		{
			if (container->entries[i].item == rifle)
			{
				worn = inventory_equip(inventories, big, i);
				break;
			}
		}
		passed = added == 6 && moved == 4 && inventory_count(inventories, small, meat, false) == 2 && inventory_count(inventories, big, meat, false) == 4
			&& worn && inventory_count(inventories, big, rifle, false) == 0 && inventory_count(inventories, big, rifle, true) == 1
			&& SDL_fabsf(inventory_weight(inventories, small) - 3.0f) < 0.01f;
		SDL_Log("Inventory test: 8 meat into 10 kg took %d, moved %d, the rifle worn %s, %.1f kg left: %s", added, moved, worn ? "yes" : "no",
			inventory_weight(inventories, small), passed ? "PASSED" : "FAILED");
		inventory_destroy(inventories, small);
		inventory_destroy(inventories, big);
	}
	{
		float middle = 0.5f * (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters;
		int before = inventory_count(inventories, game->potop->session.player_inventory, rounds, false);
		int placed = ground_items_place(&game->potop->world.ground_items, &game->potop->world.objects, &game->potop->world.terrain, inventories, rounds, 30, middle + 3.0f, middle, 0.0f, 0);
		int back = 0;
		int kept = 0;
		int picked = 0;
		bool round_trip;

		if (placed == 0)
		{
			SDL_Log("Inventory test: nothing to lay things down as (objects.ini: no sack or crate), skipped");
			return passed;
		}
		objects_sort(&game->potop->world.objects);
		if (!save_map(game, "inventory_test", false))
		{
			return false;
		}
		inventory_remove(inventories, game->potop->session.player_inventory, rounds, before);
		new_map(game, 4321u);
		if (load_map(game, "inventory_test.active"))
		{
			objects_sort(&game->potop->world.objects);
			if (game->potop->world.ground_items.count == 1)
			{
				back = inventory_count(inventories, game->potop->world.ground_items.list[0].container, rounds, false);
			}
			kept = inventory_count(inventories, game->potop->session.player_inventory, rounds, false);
			picked = ground_items_pick_up(&game->potop->world.ground_items, &game->potop->world.objects, inventories, 0, game->potop->session.player_inventory);
		}
		round_trip = back == 30 && kept == before && picked == 30 && game->potop->world.ground_items.count == 0
			&& inventory_count(inventories, game->potop->session.player_inventory, rounds, false) == before + 30;
		SDL_Log("Inventory test: 30 rounds laid down came back %d with the map (the pack's %d of %d), picked up %d: %s", back, kept, before, picked,
			round_trip ? "PASSED" : "FAILED");
		passed = passed && round_trip;
		inventory_remove(inventories, game->potop->session.player_inventory, rounds, 30);
		map_file_delete("inventory_test.active");
	}
	{
		// Picking: a type made pickable twice, growing again in an hour,
		// turned into another while picked out; then a weed that goes
		int type = -1;
		int other = -1;
		int instance = -1;
		bool picking_works = false;
		int tomato = items_find(&game->potop->session.items, "tomato");

		for (int i = 0; i < game->potop->world.objects.instance_count && tomato >= 0; i++)
		{
			int candidate = game->potop->world.objects.instances[i].type;

			if (game->potop->world.objects.types.types[candidate].model >= 0 && game->potop->world.objects.types.types[candidate].harvest_item[0] == '\0')
			{
				type = candidate;
				instance = i;
				break;
			}
		}
		for (int t = 0; t < game->potop->world.objects.types.type_count && type >= 0; t++)
		{
			if (t != type && game->potop->world.objects.types.types[t].model >= 0)
			{
				other = t;
				break;
			}
		}
		if (type >= 0 && other >= 0)
		{
			float x = game->potop->world.objects.instances[instance].x;
			float z = game->potop->world.objects.instances[instance].z;
			int before = inventory_count(inventories, game->potop->session.player_inventory, tomato, false);
			harvest_outcome_t first;
			harvest_outcome_t second;
			harvest_outcome_t third;
			int turned = -1;
			int back = -1;
			int count_before;

			uint32_t id = game->potop->world.objects.instances[instance].id;

			game->potop->world.objects.instances[instance].owner = 42;
			harvest_set(&game->potop->world.objects, type, "tomato", 2, 2, false, 2, 1.0f);
			game->potop->world.objects.types.types[type].harvest_change_type = other;
			first = harvest_pick(&game->potop->world.objects, &game->potop->world.terrain, inventories, instance, game->potop->session.player_inventory, &game->potop->world.harvest_random);
			second = harvest_pick(&game->potop->world.objects, &game->potop->world.terrain, inventories, instance, game->potop->session.player_inventory, &game->potop->world.harvest_random);
			// (picked out, it turned: the last object now)
			turned = game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].type;
			third = harvest_pick(&game->potop->world.objects, &game->potop->world.terrain, inventories, game->potop->world.objects.instance_count - 1, game->potop->session.player_inventory,
				&game->potop->world.harvest_random);
			harvest_update(&game->potop->world.objects, &game->potop->world.terrain, 0.5f);
			harvest_update(&game->potop->world.objects, &game->potop->world.terrain, 0.6f);
			back = game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].type;
			picking_works = first.result == HARVEST_PICKED && first.count == 2 && second.result == HARVEST_PICKED && turned == other
				&& third.result == HARVEST_PICKED_OUT && back == type && game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].harvested == 0
				&& SDL_fabsf(game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].x - x) < 0.01f
				&& SDL_fabsf(game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].z - z) < 0.01f
				&& inventory_count(inventories, game->potop->session.player_inventory, tomato, false) == before + 4
				// (the same thing still, turned and back: its id, whose it is)
				&& game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].id == id
				&& game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].owner == 42;
			// What was picked, as a map's file keeps it, comes back
			{
				map_features_t kept;
				object_instance_t *last = &game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1];

				SDL_zero(kept);
				harvest_pick(&game->potop->world.objects, &game->potop->world.terrain, inventories, game->potop->world.objects.instance_count - 1, game->potop->session.player_inventory,
					&game->potop->world.harvest_random);
				world_gather_harvest(&game->potop->world, &kept);
				last = &game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1];
				last->harvested = 0;
				world_apply_harvest(&game->potop->world, &kept);
				picking_works = picking_works && last->harvested == 1;
				SDL_free(kept.harvest);
			}
			// A weed: it goes
			harvest_set(&game->potop->world.objects, type, "tomato", 1, 1, true, 1, 0.0f);
			game->potop->world.objects.types.types[type].harvest_change_type = -1;
			game->potop->world.objects.instances[game->potop->world.objects.instance_count - 1].harvested = 0;
			count_before = game->potop->world.objects.instance_count;
			first = harvest_pick(&game->potop->world.objects, &game->potop->world.terrain, inventories, game->potop->world.objects.instance_count - 1, game->potop->session.player_inventory,
				&game->potop->world.harvest_random);
			picking_works = picking_works && first.removed && game->potop->world.objects.instance_count == count_before - 1;
			SDL_Log("Harvest test: picked %d, %d, then %s; turned into another and back after the hours (the same id, owner); kept as a file keeps it; a weed went: %s", first.count, second.count,
				third.result == HARVEST_PICKED_OUT ? "nothing left" : "more", picking_works ? "PASSED" : "FAILED");
			harvest_set(&game->potop->world.objects, type, "", 1, 1, false, 1, 0.0f);
			inventory_remove(inventories, game->potop->session.player_inventory, tomato, 1000);
			passed = passed && picking_works;
		}
	}
	{
		// The journal: the track limit, tracked objectives as markers, the
		// focus the active one, done ones' markers gone
		journal_t *journal = SDL_calloc(1, sizeof(journal_t));
		markers_t *test_markers = SDL_calloc(1, sizeof(markers_t));
		bool journal_works = false;

		if (journal != NULL && test_markers != NULL)
		{
			bool third;
			int shown_first;
			uint32_t focus_marker;
			int shown_after;
			int shown_untracked;

			journal_create(journal);
			markers_create(test_markers);
			journal_set_track_limit(journal, 2);
			for (int q = 0; q < 3; q++)
			{
				char id[8];

				SDL_snprintf(id, sizeof(id), "q%d", q);
				journal_set_quest(journal, id, id, "", JOURNAL_QUEST_ACTIVE);
				journal_set_objective(journal, id, "a", "first", JOURNAL_OBJECTIVE_OPEN, NULL);
				journal_set_objective(journal, id, "b", "second", JOURNAL_OBJECTIVE_OPEN, "0/3");
				journal_set_place(journal, id, "a", 10.0f * (float)q, 5.0f);
				journal_set_place(journal, id, "b", 10.0f * (float)q, 50.0f);
			}
			journal_track(journal, "q0", true);
			journal_track(journal, "q1", true);
			third = journal_track(journal, "q2", true);
			journal_sync_markers(journal, test_markers);
			shown_first = test_markers->count;
			journal_focus(journal, "q1", "b");
			journal_sync_markers(journal, test_markers);
			focus_marker = journal_find_objective(journal, "q1", "b")->marker;
			journal_set_objective(journal, "q1", "b", NULL, JOURNAL_OBJECTIVE_DONE, "3/3");
			journal_sync_markers(journal, test_markers);
			shown_after = test_markers->count;
			journal_track(journal, "q0", false);
			journal_remove_quest(journal, "q1");
			journal_sync_markers(journal, test_markers);
			shown_untracked = test_markers->count;
			journal_works = !third && shown_first == 4 && focus_marker != 0 && shown_after == 3 && shown_untracked == 0;
			SDL_Log("Journal test: a third tracked %s, %d markers, focus %s, %d after one done, %d once untracked and removed: %s",
				third ? "allowed" : "refused", shown_first, focus_marker != 0 ? "shown" : "not shown", shown_after, shown_untracked,
				journal_works ? "PASSED" : "FAILED");
		}
		SDL_free(journal);
		SDL_free(test_markers);
		passed = passed && journal_works;
	}
	{
		// Actors with the game's numbers: kept with the map (dead or alive,
		// named) and found again by the number
		int bear = actors_find_type(&game->potop->world.actors, "bear");

		if (bear >= 0 && game->potop->world.actors.types[bear].model >= 0)
		{
			float middle = 0.5f * (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters;
			uint32_t wolf = actors_add(&game->potop->world.actors, &game->potop->world.terrain, bear, middle + 5.0f, middle, 0.0f);
			uint32_t other = actors_add(&game->potop->world.actors, &game->potop->world.terrain, bear, middle - 5.0f, middle, 0.0f);
			actor_event_t event;
			map_features_t kept;
			uint32_t wolf_again;
			uint32_t other_again;
			bool kept_well;
			int index;

			actors_set_game_id(&game->potop->world.actors, wolf, 77);
			actors_set_name(&game->potop->world.actors, wolf, "Big Bad", true);
			actors_set_game_id(&game->potop->world.actors, other, 78);
			while (actors_poll_event(&game->potop->world.actors, &event))
			{
			}
			actors_kill(&game->potop->world.actors, wolf);
			SDL_zero(kept);
			world_gather_actors(&game->potop->world, &kept);
			world_apply_actors(&game->potop->world, &kept);
			SDL_free(kept.actors);
			wolf_again = actors_find_by_game_id(&game->potop->world.actors, 77);
			other_again = actors_find_by_game_id(&game->potop->world.actors, 78);
			index = actors_find(&game->potop->world.actors, wolf_again);
			kept_well = wolf_again != 0 && other_again != 0 && actors_is_dead(&game->potop->world.actors, wolf_again) && !actors_is_dead(&game->potop->world.actors, other_again)
				&& index >= 0 && SDL_strcmp(game->potop->world.actors.actors[index].name, "Big Bad") == 0 && game->potop->world.actors.actors[index].hostile;
			SDL_Log("Actor id test: kept with the map (dead, named, an enemy) and found again by the game's number %s: %s",
				kept_well ? "yes" : "no", kept_well ? "PASSED" : "FAILED");
			passed = passed && kept_well;
			actors_clear(&game->potop->world.actors);
		}
	}
	{
		// Areas: who comes in and goes out, told only as the triggers say;
		// one turned off says nothing, turned on again takes those inside
		// as there; who is in it; kept with the map, the same id
		areas_t *areas = &game->potop->world.areas;
		actors_t *actors = &game->potop->world.actors;
		int bear = actors_find_type(actors, "bear");

		if (bear >= 0 && actors->types[bear].model >= 0)
		{
			float middle = 0.5f * (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters;
			float player_out[3] = { middle - 40.0f, 0.0f, middle };
			float player_in[3] = { middle + 2.0f, 0.0f, middle + 1.0f };
			int area_count_before = areas->count;
			uint32_t gate = areas_add(areas, "test_gate", middle, middle, 10.0f, 4.0f, 0.5f, AREA_TRIGGER_PLAYER | AREA_TRIGGER_ACTORS);
			uint32_t walker = actors_add(actors, &game->potop->world.terrain, bear, middle - 30.0f, middle, 0.0f);
			int walker_index;
			area_event_t event;
			int enter_player = 0;
			int enter_actor = 0;
			int exit_actor = 0;
			int told_while_off = 0;
			int told_on_again = 0;
			uint32_t ids[8];
			int inside;
			bool player_inside = false;
			bool kept;
			map_features_t saved;

			player_out[1] = terrain_sample_height_meters(&game->potop->world.terrain, player_out[0], player_out[2]) + 1.0f;
			player_in[1] = terrain_sample_height_meters(&game->potop->world.terrain, player_in[0], player_in[2]) + 1.0f;
			areas_update(areas, actors, player_out);
			while (areas_poll_event(areas, &event))
			{
			}
			walker_index = actors_find(actors, walker);
			// In: the player and the bear
			actors->actors[walker_index].x = middle - 1.0f;
			areas_update(areas, actors, player_in);
			while (areas_poll_event(areas, &event))
			{
				enter_player += event.type == AREA_EVENT_ENTER && event.player && event.area_id == gate ? 1 : 0;
				enter_actor += event.type == AREA_EVENT_ENTER && !event.player && event.actor_id == walker ? 1 : 0;
			}
			inside = areas_occupants(areas, actors, gate, player_in, ids, 8, &player_inside);
			// Out again: the bear
			actors->actors[walker_index].x = middle - 30.0f;
			areas_update(areas, actors, player_in);
			while (areas_poll_event(areas, &event))
			{
				exit_actor += event.type == AREA_EVENT_EXIT && event.actor_id == walker ? 1 : 0;
			}
			// Turned off: the player goes out and comes in, nothing told;
			// turned on with the player in: nothing either
			areas->areas[areas_find(areas, gate)].triggers = AREA_TRIGGER_NONE;
			areas_update(areas, actors, player_out);
			areas_update(areas, actors, player_in);
			while (areas_poll_event(areas, &event))
			{
				told_while_off++;
			}
			areas->areas[areas_find(areas, gate)].triggers = AREA_TRIGGER_PLAYER;
			areas_update(areas, actors, player_in);
			while (areas_poll_event(areas, &event))
			{
				told_on_again++;
			}
			// Kept with the map
			SDL_zero(saved);
			world_gather_areas(&game->potop->world, &saved);
			areas_clear(areas);
			world_apply_areas(&game->potop->world, &saved);
			SDL_free(saved.areas);
			kept = areas_find(areas, gate) >= 0 && SDL_strcmp(areas->areas[areas_find(areas, gate)].name, "test_gate") == 0
				&& areas->count == area_count_before + 1;
			{
				bool areas_work = enter_player == 1 && enter_actor == 1 && exit_actor == 1 && inside == 1 && ids[0] == walker && player_inside
					&& told_while_off == 0 && told_on_again == 0 && kept;

				SDL_Log("Area test: in %d/%d, out %d, %d inside (the player %s), %d told while off, %d turned on, kept with its id %s: %s",
					enter_player, enter_actor, exit_actor, inside, player_inside ? "too" : "not", told_while_off, told_on_again, kept ? "yes" : "no",
					areas_work ? "PASSED" : "FAILED");
				passed = passed && areas_work;
			}
			areas_remove(areas, gate);
			actors_clear(actors);
		}
	}
	{
		// A path blocker: an area that blocks the bear's group leaves no way
		// to a goal inside it; open again, the way is there again, and the
		// paths through it were told to look again
		navigation_t *navigation = &game->potop->world.navigation;
		areas_t *areas = &game->potop->world.areas;
		int bear = actors_find_type(&game->potop->world.actors, "bear");

		if (bear >= 0)
		{
			navigation_agent_t agent = actors_agent(&game->potop->world.actors, bear);
			float size = (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters;
			float waypoints[NAVIGATION_MAX_WAYPOINTS][2];
			int waypoint_count = 0;
			bool complete = false;
			float from[2] = { -1.0f, -1.0f };
			float to[2] = { -1.0f, -1.0f };
			navigation_result_t open_before = NAVIGATION_PATH_NONE;
			navigation_result_t walled;
			navigation_result_t open_after;
			bool told;
			uint32_t generation;
			uint32_t gate;

			// Two points 20 m apart with a way between them
			for (int attempt = 0; attempt < 64 && open_before != NAVIGATION_PATH_FOUND; attempt++)
			{
				from[0] = size * (0.3f + 0.4f * (float)(attempt % 8) / 8.0f);
				from[1] = size * (0.3f + 0.4f * (float)(attempt / 8) / 8.0f);
				to[0] = from[0] + 20.0f;
				to[1] = from[1];
				open_before = navigation_find_path(navigation, &game->potop->world.terrain, NULL, &agent, from[0], from[1],
					terrain_sample_height_meters(&game->potop->world.terrain, from[0], from[1]), to[0], to[1], waypoints, &waypoint_count, &complete);
			}
			generation = navigation->generation;
			gate = areas_add(areas, "test_gate", to[0], to[1], 6.0f, 6.0f, 0.3f, AREA_TRIGGER_NONE);
			areas->areas[areas_find(areas, gate)].blocks = (uint8_t)agent.groups;
			{
				navigation_blocker_t blocker;

				SDL_zero(blocker);
				blocker.x = to[0];
				blocker.z = to[1];
				blocker.half_width_meters = 3.0f;
				blocker.half_depth_meters = 3.0f;
				blocker.yaw_radians = 0.3f;
				blocker.groups = agent.groups;
				navigation_set_blockers(navigation, &blocker, 1);
			}
			told = navigation_changed_since(navigation, NULL, to[0] - 1.0f, to[1] - 1.0f, to[0] + 1.0f, to[1] + 1.0f, generation);
			walled = navigation_find_path(navigation, &game->potop->world.terrain, NULL, &agent, from[0], from[1],
				terrain_sample_height_meters(&game->potop->world.terrain, from[0], from[1]), to[0], to[1], waypoints, &waypoint_count, &complete);
			navigation_set_blockers(navigation, NULL, 0);
			areas_remove(areas, gate);
			open_after = navigation_find_path(navigation, &game->potop->world.terrain, NULL, &agent, from[0], from[1],
				terrain_sample_height_meters(&game->potop->world.terrain, from[0], from[1]), to[0], to[1], waypoints, &waypoint_count, &complete);
			{
				bool blocking_works = open_before == NAVIGATION_PATH_FOUND && walled != NAVIGATION_PATH_FOUND && told
					&& open_after == NAVIGATION_PATH_FOUND;

				SDL_Log("Blocker test: a way %s, walled off %s (told %s), open again %s: %s", open_before == NAVIGATION_PATH_FOUND ? "found" : "not found",
					walled == NAVIGATION_PATH_FOUND ? "still found" : "none", told ? "yes" : "no", open_after == NAVIGATION_PATH_FOUND ? "found" : "not found",
					blocking_works ? "PASSED" : "FAILED");
				passed = passed && blocking_works;
			}
		}
	}
	{
		// The light: the sky's occlusion on the CPU as on the GPU; by day
		// what the sky gives, at night next to nothing, and a carried
		// light's when it was gathered
		sky_occlusion_t *occlusion = &game->potop->world.sky_occlusion;
		world_t *world = &game->potop->world;
		bool made_up = false;

		if (!occlusion->ready)
		{
			// (not worked out yet: one made up, as a map's file brings it)
			int size = world->terrain.size;
			int layer_size = map_file_sky_layer_size(size, world->terrain.cell_size_meters);
			int bounce_size = map_file_sky_bounce_size(size, world->terrain.cell_size_meters);
			uint8_t *cells = SDL_malloc((size_t)size * (size_t)size);
			uint8_t *layers = SDL_calloc((size_t)layer_size * (size_t)layer_size * SKY_OCCLUSION_LAYERS, 1);
			uint8_t *bounce = SDL_calloc((size_t)bounce_size * (size_t)bounce_size * 4, 1);

			for (int i = 0; cells != NULL && i < size * size; i++)
			{
				cells[i] = (uint8_t)((i * 7 + i / size * 13) & 255);
			}
			made_up = cells != NULL && layers != NULL && bounce != NULL
				&& sky_occlusion_upload(occlusion, size, cells, layer_size, layers, bounce_size, bounce);
			SDL_free(cells);
			SDL_free(layers);
			SDL_free(bounce);
		}
		if (occlusion->ready && occlusion->cells_cpu != NULL)
		{
			uint8_t *gpu_cells = sky_occlusion_download(occlusion, NULL, NULL);
			size_t cells = (size_t)occlusion->map_size * (size_t)occlusion->map_size;
			bool same = gpu_cells != NULL && SDL_memcmp(gpu_cells, occlusion->cells_cpu, cells) == 0;
			int cell_x = occlusion->map_size / 3;
			int cell_z = occlusion->map_size / 2;
			float open = sky_occlusion_open_at(occlusion, world->terrain.cell_size_meters, (float)cell_x * world->terrain.cell_size_meters,
				(float)cell_z * world->terrain.cell_size_meters);
			bool exact = SDL_fabsf(open - (float)occlusion->cells_cpu[cell_z * occlusion->map_size + cell_x] / 255.0f) < 0.002f;
			daylight_t kept = world->daylight;
			light_probe_t noon;
			light_probe_t night;
			bool light_works;

			world->daylight.sun_elevation_degrees = 50.0f;
			light_probe_at(world, (float)cell_x * world->terrain.cell_size_meters, 0.0f, (float)cell_z * world->terrain.cell_size_meters, &noon);
			world->daylight.sun_elevation_degrees = -30.0f;
			world->daylight.moon_lit = 0.0f;
			light_probe_at(world, (float)cell_x * world->terrain.cell_size_meters, 0.0f, (float)cell_z * world->terrain.cell_size_meters, &night);
			world->daylight = kept;
			{
				// The average round it, as the cells added up one by one
				float radius = 3.0f;
				float cell = world->terrain.cell_size_meters;
				float around = sky_occlusion_open_around(occlusion, cell, (float)cell_x * cell, (float)cell_z * cell, radius);
				int first_x = (int)SDL_floorf(((float)cell_x * cell - radius) / cell + 0.5f);
				int last_x = (int)SDL_floorf(((float)cell_x * cell + radius) / cell + 0.5f);
				int first_z = (int)SDL_floorf(((float)cell_z * cell - radius) / cell + 0.5f);
				int last_z = (int)SDL_floorf(((float)cell_z * cell + radius) / cell + 0.5f);
				float sum = 0.0f;

				for (int z = first_z; z <= last_z; z++)
				{
					for (int x = first_x; x <= last_x; x++)
					{
						sum += (float)occlusion->cells_cpu[z * occlusion->map_size + x] / 255.0f;
					}
				}
				exact = exact && SDL_fabsf(around - sum / (float)((last_x - first_x + 1) * (last_z - first_z + 1))) < 0.002f;
			}
			light_works = same && exact && noon.daylight > 0.99f && SDL_fabsf(noon.total - SDL_min(noon.sky_open + noon.local, 1.0f)) < 0.001f
				&& night.daylight < 0.05f;
			SDL_Log("Light test: the sky's occlusion on the CPU %s (and on average round), %.2f of the sky there, by day %.2f, at night %.2f: %s", same ? "as on the GPU" : "differs",
				open, noon.total, night.total, light_works ? "PASSED" : "FAILED");
			SDL_free(gpu_cells);
			passed = passed && light_works;
		}
		else
		{
			SDL_Log("Light test: no sky occlusion, skipped");
		}
		if (made_up)
		{
			sky_occlusion_forget(occlusion, true);
		}
	}
	{
		// The senses: a bear sees what is before it in the light, not behind
		// it, less in the dark; a noise is heard within its reach, once
		world_t *world = &game->potop->world;
		int bear = actors_find_type(&world->actors, "bear");

		if (bear >= 0 && world->actors.types[bear].model >= 0)
		{
			float middle = 0.5f * (float)world->terrain.size * world->terrain.cell_size_meters;
			uint32_t watcher;
			int index;
			float ahead[3];
			float behind[3];
			float seen_day;
			float seen_behind;
			float seen_night;
			daylight_t kept = world->daylight;
			noise_t heard[4];
			int near_count;
			int far_count;
			int again_count;
			double now = world->elapsed_seconds;

			actors_clear(&world->actors);
			// (facing +z: yaw 0)
			watcher = actors_add(&world->actors, &world->terrain, bear, middle, middle, 0.0f);
			index = actors_find(&world->actors, watcher);
			ahead[0] = middle;
			ahead[2] = middle + 8.0f;
			ahead[1] = terrain_sample_height_meters(&world->terrain, ahead[0], ahead[2]) + 1.0f;
			behind[0] = middle;
			behind[2] = middle - 8.0f;
			behind[1] = terrain_sample_height_meters(&world->terrain, behind[0], behind[2]) + 1.0f;
			world->daylight.sun_elevation_degrees = 50.0f;
			seen_day = senses_sight(world, index, ahead, 1.0f);
			seen_behind = senses_sight(world, index, behind, 1.0f);
			world->daylight.sun_elevation_degrees = -30.0f;
			world->daylight.moon_lit = 0.0f;
			seen_night = senses_sight(world, index, ahead, 1.0f);
			world->daylight = kept;
			noises_make(&world->noises, ahead, 10.0f, NOISE_STEP, 0, now + 0.5);
			near_count = noises_heard(&world->noises, middle, ahead[1], middle + 4.0f, now, heard, 4);
			far_count = noises_heard(&world->noises, middle + 30.0f, ahead[1], middle, now, heard, 4);
			again_count = noises_heard(&world->noises, middle, ahead[1], middle + 4.0f, now + 0.5, heard, 4);
			{
				// (the terrain may hide the point ahead on a slope: then only the rest is asked)
				bool senses_work = seen_behind == 0.0f && (seen_day == 0.0f || seen_night < seen_day) && near_count == 1 && far_count == 0
					&& again_count == 0;

				SDL_Log("Senses test: seen ahead %.2f by day, %.2f at night, behind %.2f; a step heard near %d, far %d, again %d: %s", seen_day,
					seen_night, seen_behind, near_count, far_count, again_count, senses_work ? "PASSED" : "FAILED");
				passed = passed && senses_work;
			}
			noises_clear(&world->noises);
			actors_clear(&world->actors);
		}
	}
	{
		// Where a bear can stand: not in deep water (no way is searched to
		// there), on dry ground yes
		world_t *world = &game->potop->world;
		int bear = actors_find_type(&world->actors, "bear");

		if (bear >= 0)
		{
			navigation_agent_t agent = actors_agent(&world->actors, bear);
			float extent = (float)world->terrain.size * world->terrain.cell_size_meters;
			float deep[2] = { -1.0f, -1.0f };
			float dry[2] = { -1.0f, -1.0f };
			float deepest = agent.swim_depth_meters;

			for (float z = 8.0f; z < extent - 8.0f; z += 8.0f)
			{
				for (float x = 8.0f; x < extent - 8.0f; x += 8.0f)
				{
					float depth = 0.0f;
					float velocity_x;
					float velocity_z;

					if (!water_sample(&world->water, x, z, &depth, &velocity_x, &velocity_z))
					{
						depth = 0.0f;
					}
					if (depth > deepest)
					{
						deepest = depth;
						deep[0] = x;
						deep[1] = z;
					}
					if (dry[0] < 0.0f && depth <= 0.0f && SDL_fabsf(x - 0.5f * extent) < 64.0f && SDL_fabsf(z - 0.5f * extent) < 64.0f)
					{
						dry[0] = x;
						dry[1] = z;
					}
				}
			}
			if (deep[0] >= 0.0f && dry[0] >= 0.0f)
			{
				bool in_deep = navigation_standable(&world->navigation, &world->terrain, &world->water, &agent, deep[0], deep[1]);
				bool on_dry = navigation_standable(&world->navigation, &world->terrain, &world->water, &agent, dry[0], dry[1]);
				bool works = !in_deep && on_dry;

				SDL_Log("Standing test: a bear in %.1f m of water %s, on dry ground %s: %s", (double)deepest, in_deep ? "yes" : "no",
					on_dry ? "yes" : "no", works ? "PASSED" : "FAILED");
				passed = passed && works;
			}
			else
			{
				SDL_Log("Standing test: no deep water or no dry ground found (skipped)");
			}
		}
	}
	{
		// The terrain moving under a built region (the water wearing it):
		// its cells take the new height, the region is stamped newer, but
		// nothing is built again
		world_t *world = &game->potop->world;
		navigation_t *navigation = &world->navigation;

		if (!world->terrain.baked)
		{
			float middle = 0.5f * (float)world->terrain.size * world->terrain.cell_size_meters;
			int32_t cell_x = (int32_t)(middle / NAVIGATION_CELL_METERS);
			int32_t cell = cell_x * navigation->size + cell_x;
			uint32_t generation;
			int64_t built_before;
			float before;
			bool told;
			stamp_t stamp;

			navigation_update(navigation, &world->terrain, &world->objects, world->elapsed_seconds);
			navigation_prepare_around(navigation, middle, middle);
			generation = navigation->generation;
			built_before = navigation->rebuilt_region_total;
			before = navigation->ground_height[cell];
			SDL_zero(stamp);
			stamp.type = STAMP_CRATER;
			stamp.soil = SOIL_EARTH;
			stamp.radius_meters = 4.0f;
			stamp.height_meters = 1.0f;
			stamp.start = vec3_make(((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS, terrain_sample_height_meters(&world->terrain, middle, middle),
				((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS);
			stamp.end = stamp.start;
			stamps_apply(&world->terrain, &world->water, &stamp);
			navigation_update(navigation, &world->terrain, &world->objects, world->elapsed_seconds);
			told = navigation_changed_since(navigation, NULL, middle - 1.0f, middle - 1.0f, middle + 1.0f, middle + 1.0f, generation);
			{
				bool follows = told && navigation->ground_height[cell] < before - 0.3f && navigation->rebuilt_region_total == built_before;

				SDL_Log("Navigation terrain test: dug %.2f m, the cell followed to %.2f m, told %s, regions built again %lld: %s",
					(double)(before - terrain_sample_height_meters(&world->terrain, ((float)cell_x + 0.5f), ((float)cell_x + 0.5f))),
					(double)(before - navigation->ground_height[cell]), told ? "yes" : "no", (long long)(navigation->rebuilt_region_total - built_before),
					follows ? "PASSED" : "FAILED");
				passed = passed && follows;
			}
		}
	}
	{
		// An object going from a built region: only the cells round it are
		// built again, and they come out as building the whole region would
		world_t *world = &game->potop->world;
		navigation_t *navigation = &world->navigation;
		objects_t *objects = &world->objects;
		int chosen = -1;
		float middle = 0.5f * (float)world->terrain.size * world->terrain.cell_size_meters;
		float best = 1e30f;

		for (int i = 0; i < objects->instance_count; i++)
		{
			const object_instance_t *instance = &objects->instances[i];
			const object_type_t *type = &objects->types.types[instance->type];
			int model = objects_instance_model(objects, instance);
			float distance = SDL_fabsf(instance->x - middle) + SDL_fabsf(instance->z - middle);

			if (type->physics != OBJECT_PHYSICS_STATIC || (instance->state & OBJECT_STATE_FREE) != 0 || model < 0
				|| objects->models[model].collision_index_count == 0 || objects->models[model].horizontal_radius * instance->scale < 0.75f
				|| objects->models[model].horizontal_radius * instance->scale > 4.0f || distance >= best)
			{
				continue;
			}
			best = distance;
			chosen = i;
		}
		if (chosen >= 0)
		{
			float x = objects->instances[chosen].x;
			float z = objects->instances[chosen].z;
			char name[64];
			int32_t region_cells;
			int32_t first_x;
			int32_t first_z;
			int32_t region_x;
			int32_t region_z;
			uint8_t *kept;
			float *kept_heights;
			int64_t partial_before;
			int32_t differences = 0;
			int32_t opened = 0;
			bool partial;

			SDL_strlcpy(name, objects->types.types[objects->instances[chosen].type].name, sizeof(name));
			// (built from nothing first: the terrain test's crater left cells
			// that moved less than the navigation follows)
			navigation_update(navigation, &world->terrain, objects, world->elapsed_seconds);
			navigation_forget(navigation, &world->terrain, objects);
			navigation_prepare_around(navigation, x, z);
			region_x = (int32_t)(x / NAVIGATION_CELL_METERS) / navigation->region_size;
			region_z = (int32_t)(z / NAVIGATION_CELL_METERS) / navigation->region_size;
			first_x = region_x * navigation->region_size;
			first_z = region_z * navigation->region_size;
			region_cells = navigation->region_size * navigation->region_size;
			// Per cell: the room above the ground, the flyers' top, the small
			// ones' height, how many surfaces, then the surfaces' clearances
			kept = SDL_calloc((size_t)region_cells, 4 + NAVIGATION_MAX_EXTRA_SURFACES);
			kept_heights = SDL_calloc((size_t)region_cells * NAVIGATION_MAX_EXTRA_SURFACES, sizeof(float));
			for (int32_t k = 0; k < region_cells; k++)
			{
				int32_t cell = (first_z + k / navigation->region_size) * navigation->size + first_x + k % navigation->region_size;

				opened -= navigation->terrain_clearance[cell] == NAVIGATION_CLEARANCE_OPEN ? 1 : 0;
			}
			partial_before = navigation->partial_region_total;
			objects_remove_nearest(objects, x, z, 0.01f);
			navigation_update(navigation, &world->terrain, objects, world->elapsed_seconds);
			navigation_prepare_around(navigation, x, z);
			partial = navigation->partial_region_total > partial_before;
			for (int pass = 0; pass < 2; pass++)
			{
				const navigation_region_t *region = &navigation->regions[region_z * navigation->regions_per_side + region_x];

				if (pass == 1)
				{
					// The whole region, from nothing
					navigation_forget(navigation, &world->terrain, objects);
					navigation_prepare_around(navigation, x, z);
				}
				for (int32_t k = 0; k < region_cells && kept != NULL && kept_heights != NULL; k++)
				{
					int32_t cell = (first_z + k / navigation->region_size) * navigation->size + first_x + k % navigation->region_size;
					uint8_t *row = &kept[k * (4 + NAVIGATION_MAX_EXTRA_SURFACES)];
					uint8_t now[4 + NAVIGATION_MAX_EXTRA_SURFACES] = { navigation->terrain_clearance[cell], navigation->object_top[cell],
						navigation->small_height[cell], navigation->extra_count[cell] };
					float heights[NAVIGATION_MAX_EXTRA_SURFACES] = { 0 };

					for (int e = 0; e < navigation->extra_count[cell]; e++)
					{
						now[4 + e] = region->surfaces[navigation->extra_first[cell] + e].clearance;
						heights[e] = region->surfaces[navigation->extra_first[cell] + e].height;
					}
					if (pass == 0)
					{
						SDL_memcpy(row, now, sizeof(now));
						SDL_memcpy(&kept_heights[k * NAVIGATION_MAX_EXTRA_SURFACES], heights, sizeof(heights));
						opened += navigation->terrain_clearance[cell] == NAVIGATION_CLEARANCE_OPEN ? 1 : 0;
					}
					else if (SDL_memcmp(row, now, sizeof(now)) != 0
						|| SDL_memcmp(&kept_heights[k * NAVIGATION_MAX_EXTRA_SURFACES], heights, sizeof(heights)) != 0)
					{
						differences++;
					}
				}
			}
			{
				bool works = partial && differences == 0 && opened > 0 && kept != NULL;

				SDL_Log("Navigation part test: %s taken away, built in part %s, %d cells opened, %d cells unlike the whole region built again: %s",
					name, partial ? "yes" : "no", opened, differences, works ? "PASSED" : "FAILED");
				passed = passed && works;
			}
			SDL_free(kept);
			SDL_free(kept_heights);
		}
		else
		{
			SDL_Log("Navigation part test: no standing object to take away (skipped)");
		}
	}
	{
		// A dialog: the answers come back as events, Escape closes it
		dialog_t talk;
		dialog_choice_t answers[2] = { { 10, "yes" }, { 20, "no" } };
		dialog_event_t first;
		dialog_event_t second;
		bool talking;

		dialog_create(&talk);
		dialog_open(&talk, 7, "Someone");
		dialog_say(&talk, "Well?");
		dialog_set_choices(&talk, answers, 2);
		dialog_add_choice(&talk, 30, "maybe");
		dialog_remove_choice(&talk, 10);
		// ("no" first now, "maybe" second: the second place is id 30)
		dialog_choose(&talk, 1);
		dialog_choose(&talk, 5);
		dialog_cancel(&talk);
		talking = dialog_poll_event(&talk, &first) && dialog_poll_event(&talk, &second) && first.type == DIALOG_EVENT_CHOSEN
			&& first.choice == 30 && first.actor == 7 && second.type == DIALOG_EVENT_CLOSED && !talk.open && !dialog_poll_event(&talk, &first);
		SDL_Log("Dialog test: answers by id (added, taken away), a place out of range ignored, closed: %s", talking ? "PASSED" : "FAILED");
		passed = passed && talking;
	}
	{
		// The world map's picture made from the land; the markers
		bool made;
		uint32_t first;
		uint32_t second;
		bool markers_work;

		terrain_renderer_bake_materials(&game->potop->world.renderer, &game->potop->world.terrain, &game->potop->world.lit_render);
		made = world_map_make(&game->potop->session.world_map, &game->potop->world.terrain, &game->potop->world.water, &game->potop->world.objects, "");
		markers_clear(&game->potop->session.markers, false);
		first = markers_add(&game->potop->session.markers, 10.0f, 10.0f, NULL, "a");
		second = markers_add(&game->potop->session.markers, 100.0f, 100.0f, NULL, "b");
		markers_set_active(&game->potop->session.markers, second);
		markers_work = markers_nearest(&game->potop->session.markers, 12.0f, 9.0f, 5.0f) == first && markers_remove(&game->potop->session.markers, second)
			&& game->potop->session.markers.active == 0 && game->potop->session.markers.count == 1;
		SDL_Log("World map test: the picture %s (%dx%d, %s), markers %s: %s", made ? "made" : "not made", game->potop->session.world_map.width,
			game->potop->session.world_map.height, game->potop->session.world_map.from_artist ? "an artist's" : "from the land", markers_work ? "work" : "do not work",
			made && markers_work ? "PASSED" : "FAILED");
		markers_clear(&game->potop->session.markers, false);
		passed = passed && made && markers_work;
	}
	return passed;
}

// Part of --map-test: the weapon comes up and fires (its shots), and a
// bullet fired down from high up hits the ground where the ground is
// A frag grenade dropped 3 m from a bear goes off after its fuse: the
// bear is in the hits, the ground has a hole; a proximity grenade lies
// armed until a bear is put next to it
static bool run_explosion_test(testbed_t *game)
{
	session_t *session = &game->potop->session;
	world_t *world = &game->potop->world;
	int frag = explosives_find(&session->explosives, "frag");
	int proximity = explosives_find(&session->explosives, "proximity");
	int bear = actors_find_type(&world->actors, "bear");
	float size = (float)world->terrain.size * world->terrain.cell_size_meters;
	float spot_x = -1.0f;
	float spot_z = -1.0f;
	projectile_hit_t hits[PROJECTILE_HITS_MAX];
	const float step = 1.0f / 60.0f;
	bool passed;

	if (frag < 0 || proximity < 0 || bear < 0 || world->actors.types[bear].model < 0)
	{
		SDL_Log("Explosion test: no explosives.ini (frag, proximity) or no bear, skipped");
		return true;
	}
	// Dry, fairly flat ground
	for (int ring = 0; ring < 2000 && spot_x < 0.0f; ring++)
	{
		float x = size * (0.3f + 0.4f * (float)(ring % 40) / 40.0f);
		float z = size * (0.3f + 0.4f * (float)(ring / 40) / 50.0f);
		int32_t cell = terrain_index(&world->terrain, (int32_t)(x / world->terrain.cell_size_meters), (int32_t)(z / world->terrain.cell_size_meters));
		float slope = SDL_fabsf(terrain_sample_height_meters(&world->terrain, x + 3.0f, z) - terrain_sample_height_meters(&world->terrain, x - 3.0f, z));

		if (world->water.depth[cell] == 0 && slope < 1.0f)
		{
			spot_x = x;
			spot_z = z;
		}
	}
	if (spot_x < 0.0f)
	{
		SDL_Log("Explosion test: no dry flat ground found, skipped");
		return true;
	}
	projectiles_take_hits(&session->projectiles, NULL, 0);
	actors_clear(&world->actors);
	{
		float ground = terrain_sample_height_meters(&world->terrain, spot_x, spot_z);
		// Dropped at its feet: the throw itself is the game's to aim
		float from[3] = { spot_x, ground + 0.5f, spot_z };
		float direction[3] = { 0.0f, 0.0f, 0.0f };
		uint32_t target = actors_add(&world->actors, &world->terrain, bear, spot_x, spot_z + 3.0f, 0.0f);
		float heights_before[25 * 17];
		float lowest = 0.0f;
		int hit_count;
		bool bear_hit = false;
		bool gone = false;
		float far_away[3] = { -1000.0f, -1000.0f, -1000.0f };

		for (int dz = -8; dz <= 16; dz++)
		{
			for (int dx = -8; dx <= 8; dx++)
			{
				heights_before[(dz + 8) * 17 + dx + 8] = terrain_sample_height_meters(&world->terrain, spot_x + (float)dx * 0.5f, spot_z + (float)dz * 0.5f);
			}
		}
		explosives_throw(&session->explosives, world, frag, from, direction, -1);
		for (int i = 0; i < 6 * 60 && !gone; i++)
		{
			world_step(world, step);
			explosives_update(&session->explosives, session, world, far_away, step);
			gone = !session->explosives.grenades[0].used;
		}
		hit_count = projectiles_take_hits(&session->projectiles, hits, PROJECTILE_HITS_MAX);
		for (int i = 0; i < hit_count; i++)
		{
			bear_hit = bear_hit || (hits[i].kind == PROJECTILE_HIT_ACTOR && hits[i].actor_id == target && hits[i].damage > 0.0f);
		}
		// Where it lay: its object is gone, the hole is round the last place
		// the heights dropped most
		for (int dz = -8; dz <= 16; dz++)
		{
			for (int dx = -8; dx <= 8; dx++)
			{
				float x = spot_x + (float)dx * 0.5f;
				float z = spot_z + (float)dz * 0.5f;

				lowest = SDL_min(lowest, terrain_sample_height_meters(&world->terrain, x, z) - heights_before[(dz + 8) * 17 + dx + 8]);
			}
		}
		passed = gone && bear_hit;
		SDL_Log("Explosion test: the grenade went off %s, the bear %s, the ground %s by %.2f m: %s", gone ? "after its fuse" : "never",
			bear_hit ? "caught in it" : "untouched", world->terrain.baked ? "(baked, kept)" : "dug", -lowest, passed ? "PASSED" : "FAILED");
		actors_clear(&world->actors);
	}
	if (passed)
	{
		float ground = terrain_sample_height_meters(&world->terrain, spot_x + 8.0f, spot_z);
		float from[3] = { spot_x + 8.0f, ground + 0.3f, spot_z };
		float direction[3] = { 0.0f, 0.0f, 0.0f };
		float far_away[3] = { -1000.0f, -1000.0f, -1000.0f };
		bool waited;
		bool gone = false;
		int slot = -1;

		explosives_throw(&session->explosives, world, proximity, from, direction, -1);
		for (int g = 0; g < EXPLOSIVES_GRENADES_MAX; g++)
		{
			slot = session->explosives.grenades[g].used ? g : slot;
		}
		for (int i = 0; i < 4 * 60; i++)
		{
			world_step(world, step);
			explosives_update(&session->explosives, session, world, far_away, step);
		}
		waited = slot >= 0 && session->explosives.grenades[slot].used;
		if (waited)
		{
			int index = objects_find_id(&world->objects, session->explosives.grenades[slot].object_id);

			if (index >= 0)
			{
				actors_add(&world->actors, &world->terrain, bear, world->objects.instances[index].x + 1.0f, world->objects.instances[index].z, 0.0f);
			}
			for (int i = 0; i < 10 && !gone; i++)
			{
				world_step(world, step);
				explosives_update(&session->explosives, session, world, far_away, step);
				gone = !session->explosives.grenades[slot].used;
			}
		}
		passed = waited && gone;
		SDL_Log("Explosion test: the proximity grenade %s, then %s with a bear next to it: %s", waited ? "lay armed" : "did not wait",
			gone ? "went off" : "did not go off", passed ? "PASSED" : "FAILED");
		projectiles_take_hits(&session->projectiles, NULL, 0);
		actors_clear(&world->actors);
	}
	if (passed)
	{
		// Shot: a rifle bullet's push (8 g at 700 m/s) rolls it a few meters,
		// and it goes off when the game says
		float ground = terrain_sample_height_meters(&world->terrain, spot_x, spot_z);
		float from[3] = { spot_x, ground + 0.3f, spot_z };
		float direction[3] = { 0.0f, 0.0f, 0.0f };
		float far_away[3] = { -1000.0f, -1000.0f, -1000.0f };
		float mass = 0.0f;
		float moved = -1.0f;
		bool set_off = false;
		int slot = -1;
		int index = -1;

		explosives_throw(&session->explosives, world, frag, from, direction, -1);
		for (int g = 0; g < EXPLOSIVES_GRENADES_MAX; g++)
		{
			slot = session->explosives.grenades[g].used ? g : slot;
		}
		for (int i = 0; i < 30 && slot >= 0; i++)
		{
			world_step(world, step);
			explosives_update(&session->explosives, session, world, far_away, step);
		}
		index = slot >= 0 ? objects_find_id(&world->objects, session->explosives.grenades[slot].object_id) : -1;
		if (index >= 0 && world->objects.instances[index].body >= 0)
		{
			float impulse[3] = { 0.008f * 700.0f, 0.0f, 0.0f };
			float point[3] = { world->objects.instances[index].x, objects_instance_base_y(&world->objects.instances[index]),
				world->objects.instances[index].z };
			float start_x = point[0];
			float start_z = point[2];

			mass = world->physics.bodies[world->objects.instances[index].body].mass;
			physics_push_body(&world->physics, world->objects.instances[index].body, impulse, point);
			for (int i = 0; i < 60; i++)
			{
				world_step(world, step);
				explosives_update(&session->explosives, session, world, far_away, step);
			}
			index = objects_find_id(&world->objects, session->explosives.grenades[slot].object_id);
			if (index >= 0)
			{
				moved = SDL_sqrtf((world->objects.instances[index].x - start_x) * (world->objects.instances[index].x - start_x)
					+ (world->objects.instances[index].z - start_z) * (world->objects.instances[index].z - start_z));
				set_off = explosives_set_off(&session->explosives, session, world, session->explosives.grenades[slot].object_id);
			}
		}
		passed = SDL_fabsf(mass - session->explosives.types[frag].mass_kilograms) < 0.01f && moved >= 0.0f && moved < 20.0f && set_off;
		SDL_Log("Explosion test: the grenade weighs %.2f kg, a shot rolled it %.1f m, set off by its id %s: %s", mass, moved,
			set_off ? "yes" : "no", passed ? "PASSED" : "FAILED");
		projectiles_take_hits(&session->projectiles, NULL, 0);
	}
	if (passed)
	{
		// A landmine: buried, not set off before it is armed, nor by a bird;
		// armed, a bear stepping by sets it off. A fire mine's blast and its
		// fire say fire. A mine taken back is gone, unexploded
		int landmine = explosives_find(&session->explosives, "landmine");
		int fire_mine = explosives_find(&session->explosives, "fire_mine");
		int falcon = actors_find_type(&world->actors, "falcon");
		float far_away[3] = { -1000.0f, -1000.0f, -1000.0f };

		if (landmine >= 0 && fire_mine >= 0)
		{
			float x = spot_x + 4.0f;
			float z = spot_z + 4.0f;
			uint32_t mine = explosives_place(&session->explosives, world, landmine, x, z, 0.0f, -1);
			int index = objects_find_id(&world->objects, mine);
			bool buried = index >= 0 && world->objects.instances[index].lift_meters < 0.0f;
			bool early = false;
			bool by_bird = false;
			bool went_off = false;
			bool fire_blast = false;
			bool fire_burns = false;
			int hit_count;
			uint32_t taken;
			bool taken_back;
			int kind = -1;
			bool armed = false;
			int owner = 0;

			actors_clear(&world->actors);
			// Not armed yet (5 s): a bear on it does nothing
			actors_add(&world->actors, &world->terrain, bear, x + 0.3f, z, 0.0f);
			for (int i = 0; i < 60; i++)
			{
				world_step(world, step);
				explosives_update(&session->explosives, session, world, far_away, step);
			}
			early = objects_find_id(&world->objects, mine) < 0;
			actors_clear(&world->actors);
			// Armed: a bird by it does nothing
			for (int i = 0; i < 5 * 60; i++)
			{
				explosives_update(&session->explosives, session, world, far_away, step);
			}
			if (falcon >= 0 && world->actors.types[falcon].model >= 0)
			{
				actors_add(&world->actors, &world->terrain, falcon, x + 0.3f, z, 0.0f);
				explosives_update(&session->explosives, session, world, far_away, step);
				by_bird = objects_find_id(&world->objects, mine) < 0;
				actors_clear(&world->actors);
			}
			explosives_find_object(&session->explosives, mine, &kind, &armed, &owner);
			// A bear by it: it goes off
			actors_add(&world->actors, &world->terrain, bear, x + 0.3f, z, 0.0f);
			explosives_update(&session->explosives, session, world, far_away, step);
			went_off = objects_find_id(&world->objects, mine) < 0;
			projectiles_take_hits(&session->projectiles, NULL, 0);
			// A fire mine's blast by the bear, then its fire
			{
				float ground = terrain_sample_height_meters(&world->terrain, x, z);
				float at[3] = { x + 1.0f, ground, z };

				explosives_explode(&session->explosives, session, world, fire_mine, at, -1);
				hit_count = projectiles_take_hits(&session->projectiles, hits, PROJECTILE_HITS_MAX);
				for (int i = 0; i < hit_count; i++)
				{
					fire_blast = fire_blast || (hits[i].kind == PROJECTILE_HIT_ACTOR && hits[i].damage_type == DAMAGE_FIRE);
				}
				for (int i = 0; i < 60; i++)
				{
					explosives_update(&session->explosives, session, world, far_away, step);
				}
				hit_count = projectiles_take_hits(&session->projectiles, hits, PROJECTILE_HITS_MAX);
				for (int i = 0; i < hit_count; i++)
				{
					fire_burns = fire_burns || (hits[i].kind == PROJECTILE_HIT_ACTOR && hits[i].damage_type == DAMAGE_FIRE && hits[i].damage > 0.0f);
				}
				explosives_forget(&session->explosives);
			}
			// Taken back
			taken = explosives_place(&session->explosives, world, landmine, x - 3.0f, z, 0.0f, -1);
			taken_back = explosives_take_back(&session->explosives, world, taken) == landmine && objects_find_id(&world->objects, taken) < 0;
			passed = buried && !early && !by_bird && armed && went_off && fire_blast && fire_burns && taken_back;
			SDL_Log("Mine test: buried %s, before armed %s, a bird %s, armed %s, a bear %s; fire blast %s, burning %s; taken back %s: %s",
				buried ? "yes" : "no", early ? "went off" : "waited", by_bird ? "set it off" : "did not", armed ? "yes" : "no",
				went_off ? "set it off" : "did not", fire_blast ? "fire" : "not fire", fire_burns ? "yes" : "no", taken_back ? "yes" : "no",
				passed ? "PASSED" : "FAILED");
			projectiles_take_hits(&session->projectiles, NULL, 0);
			actors_clear(&world->actors);
		}
		else
		{
			SDL_Log("Mine test: no landmine or fire_mine in explosives.ini, skipped");
		}
	}
	return passed;
}

// Path benchmark (no pass or fail): a bear's ways from round the middle
// to random places up to 80 m off, some of them across water; where the
// time goes (the search, pulling the path tight). Twice: the first pass
// builds the regions it goes through
static void run_path_benchmark(testbed_t *game)
{
	world_t *world = &game->potop->world;
	int bear = actors_find_type(&world->actors, "bear");

	if (bear >= 0)
	{
		navigation_agent_t agent = actors_agent(&world->actors, bear);
		navigation_t *navigation = &world->navigation;
		float middle = 0.5f * (float)world->terrain.size * world->terrain.cell_size_meters;
		uint32_t random = 12345u;
		uint64_t search_ticks = 0;
		int64_t expansions = 0;
		int counts[3] = { 0, 0, 0 };
		double frequency = (double)SDL_GetPerformanceFrequency();
		uint64_t worst = 0;
		// (POTOP_PATH_BENCH: as many as it says, to time them closely)
		const int searches = SDL_getenv("POTOP_PATH_BENCH") != NULL ? SDL_atoi(SDL_getenv("POTOP_PATH_BENCH")) : 300;

		navigation_update(navigation, &world->terrain, &world->objects, world->elapsed_seconds);
		uint64_t best_search_ticks = UINT64_MAX;

		for (int pass = 0; pass < 6; pass++)
		{
			if (pass > 1)
			{
				best_search_ticks = SDL_min(best_search_ticks, search_ticks);
			}
			random = 12345u;
			search_ticks = 0;
			expansions = 0;
			worst = 0;
			SDL_zeroa(counts);
			for (int i = 0; i < searches; i++)
			{
				float waypoints[NAVIGATION_MAX_WAYPOINTS][2];
				int waypoint_count;
				bool complete;
				float from_x;
				float from_z;
				float to_x;
				float to_z;
				navigation_result_t result;

				random = random * 1664525u + 1013904223u;
				from_x = middle - 100.0f + (float)(random >> 8) / 16777216.0f * 200.0f;
				random = random * 1664525u + 1013904223u;
				from_z = middle - 100.0f + (float)(random >> 8) / 16777216.0f * 200.0f;
				random = random * 1664525u + 1013904223u;
				to_x = from_x - 80.0f + (float)(random >> 8) / 16777216.0f * 160.0f;
				random = random * 1664525u + 1013904223u;
				to_z = from_z - 80.0f + (float)(random >> 8) / 16777216.0f * 160.0f;
				result = navigation_find_path(navigation, &world->terrain, &world->water, &agent, from_x, from_z,
					terrain_sample_height_meters(&world->terrain, from_x, from_z), to_x, to_z, waypoints, &waypoint_count, &complete);
				search_ticks += navigation->last_search_ticks;
				expansions += navigation->last_expansions;
				worst = SDL_max(worst, navigation->last_search_ticks);
				counts[result == NAVIGATION_PATH_FOUND ? 0 : (result == NAVIGATION_PATH_PARTIAL ? 1 : 2)]++;
			}
		}
		best_search_ticks = SDL_min(best_search_ticks, search_ticks);
		search_ticks = best_search_ticks;
		SDL_Log("Path benchmark (the best of 5 passes): %d searches (%d found, %d partial, %d none), %lld rectangles: search %.1f ms (%.3f us a rectangle), "
			"the worst %.2f ms", searches, counts[0], counts[1], counts[2], (long long)expansions,
			1000.0 * (double)search_ticks / frequency, 1e6 * (double)search_ticks / frequency / (double)SDL_max(expansions, 1),
			1000.0 * (double)worst / frequency);
	}
}

static bool run_weapon_test(testbed_t *game)
{
	weapon_shot_t shots[16];
	float kick_up;
	float kick_right;
	float eye[3];
	float down[3] = { 0.0f, -1.0f, 0.0f };
	float forward[3] = { 0.0f, 0.0f, 1.0f };
	float right[3] = { 1.0f, 0.0f, 0.0f };
	float up[3] = { 0.0f, 1.0f, 0.0f };
	float middle = 0.5f * (float)game->potop->world.terrain.size * game->potop->world.terrain.cell_size_meters;
	float ground = terrain_sample_height_meters(&game->potop->world.terrain, middle, middle);
	projectile_hit_t hits[PROJECTILE_HITS_MAX];
	projectiles_world_t world;
	int shot_count = 0;
	int hit_count = 0;
	bool passed;

	if (game->potop->session.weapons.type_count == 0)
	{
		SDL_Log("Weapon test: no weapons.ini, skipped");
		return true;
	}
	eye[0] = middle;
	eye[1] = ground + 1.7f;
	eye[2] = middle;
	weapons_toggle(&game->potop->session.weapons);
	for (int i = 0; i < 120; i++)
	{
		shot_count += weapons_update(&game->potop->session.weapons, 1.0f / 60.0f, i >= 60, false, eye, forward, right, up, shots, 16, &kick_up, &kick_right);
	}
	SDL_zero(world);
	world.terrain = &game->potop->world.terrain;
	world.water = &game->potop->world.water;
	world.physics = &game->potop->world.physics;
	world.objects = &game->potop->world.objects;
	world.particles = &game->potop->world.particles;
	projectiles_take_hits(&game->potop->session.projectiles, NULL, 0);
	eye[1] = ground + 300.0f;
	projectiles_fire(&game->potop->session.projectiles, &game->potop->world.particles, eye, down, 715.0f, 0.0079f, 0.0011f, 6.0f, 35.0f, 4.0f, -1, NULL, NULL);
	for (int i = 0; i < 60 && hit_count == 0; i++)
	{
		projectiles_update(&game->potop->session.projectiles, 1.0f / 60.0f, &world);
		hit_count = projectiles_take_hits(&game->potop->session.projectiles, hits, PROJECTILE_HITS_MAX);
	}
	// (on the ground, in the water over it, or on what stands there: not
	// under the ground and not far above it)
	passed = shot_count >= 9 && shot_count <= 11 && hit_count >= 1 && hits[0].point[1] > ground - 0.5f && hits[0].point[1] < ground + 40.0f;
	SDL_Log("Weapon test: %d shots in a second of trigger, the bullet down hit %s at %.2f m (ground %.2f m), %.0f damage: %s", shot_count,
		hit_count == 0 ? "nothing" : (hits[0].kind == PROJECTILE_HIT_GROUND ? "the ground" : (hits[0].kind == PROJECTILE_HIT_WATER ? "the water" : "an object")),
		hit_count > 0 ? hits[0].point[1] : 0.0f, ground, hit_count > 0 ? hits[0].damage : 0.0f, passed ? "PASSED" : "FAILED");
	projectiles_clear(&game->potop->session.projectiles);
	// A bear 4 m off (3 across, 2.5 up), shot at its middle: the bullet stops in it
	{
		int bear = actors_find_type(&game->potop->world.actors, "bear");

		if (bear >= 0 && game->potop->world.actors.types[bear].model >= 0)
		{
			const actor_model_t *model = &game->potop->world.actors.models[game->potop->world.actors.types[bear].model];
			float spot_x = middle;
			float spot_z = middle;
			uint32_t id;
			int index;

			// (on dry ground: the map's middle may be a lake)
			for (int ring = 0; ring < 400; ring++)
			{
				float angle = (float)ring * 2.4f;
				float x = middle + SDL_cosf(angle) * 3.0f * (float)ring;
				float z = middle + SDL_sinf(angle) * 3.0f * (float)ring;
				float depth = 0.0f;
				float flow_x;
				float flow_z;

				if (!water_sample(&game->potop->world.water, x, z, &depth, &flow_x, &flow_z))
				{
					depth = 0.0f;
				}
				if (depth < 0.01f && water_sample(&game->potop->world.water, x + 4.0f, z, &depth, &flow_x, &flow_z) && depth < 0.01f)
				{
					spot_x = x;
					spot_z = z;
					break;
				}
			}
			id = actors_add(&game->potop->world.actors, &game->potop->world.terrain, bear, spot_x, spot_z, 0.0f);
			index = actors_find(&game->potop->world.actors, id);
			float aim[3];
			float from[3];
			float length;
			bool actor_hit = false;
			const char *part = "-";

			if (index >= 0)
			{
				actor_t *actor = &game->potop->world.actors.actors[index];
				float actor_x = actor->x;
				float actor_z = actor->z;
				float middle_height = actor->y + 0.5f * (model->bounds_minimum.y + model->bounds_maximum.y) * actor->scale;

				actors_update(&game->potop->world.actors, 0.0f);
				// (from above: the ground may rise between)
				from[0] = actor->x + 3.0f;
				from[1] = middle_height + 2.5f;
				from[2] = actor->z + 0.05f;
				aim[0] = actor->x - from[0];
				aim[1] = middle_height - from[1];
				aim[2] = actor->z - from[2];
				length = SDL_sqrtf(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]);
				for (int k = 0; k < 3; k++)
				{
					aim[k] /= length;
				}
				projectiles_take_hits(&game->potop->session.projectiles, NULL, 0);
				world.actors = &game->potop->world.actors;
				projectiles_fire(&game->potop->session.projectiles, &game->potop->world.particles, from, aim, 715.0f, 0.0079f, 0.0011f, 6.0f, 35.0f, 4.0f, -1, NULL, NULL);
				hit_count = 0;
				for (int i = 0; i < 10 && hit_count == 0; i++)
				{
					projectiles_update(&game->potop->session.projectiles, 1.0f / 60.0f, &world);
					hit_count = projectiles_take_hits(&game->potop->session.projectiles, hits, PROJECTILE_HITS_MAX);
				}
				actor_hit = hit_count > 0 && hits[0].kind == PROJECTILE_HIT_ACTOR && hits[0].actor_id == id;
				if (actor_hit)
				{
					part = hits[0].zone == ACTOR_ZONE_HEAD ? "head" : (hits[0].zone == ACTOR_ZONE_LIMB ? "limb" : "body");
				}
				actor_hit = actor_hit && actors_kill(&game->potop->world.actors, id) && actors_is_dead(&game->potop->world.actors, id);
				SDL_Log("Weapon test: a bullet at a bear 4 m off hit %s (%s, bone %d) and killed it: %s",
					actor_hit ? "it" : "something else", part, hit_count > 0 ? hits[0].bone : -1, actor_hit ? "PASSED" : "FAILED");
				passed = passed && actor_hit;
				actors_remove_nearest(&game->potop->world.actors, actor_x, actor_z, 1.0f);
				projectiles_clear(&game->potop->session.projectiles);
			}
		}
	}
	game->potop->session.weapons.state = WEAPON_STATE_AWAY;
	return passed;
}

// --physics-test: things for the dam break to carry off
static void place_physics_test(testbed_t *game, const scene_dam_info_t *scene)
{
	float cell = game->potop->world.terrain.cell_size_meters;
	float center_x = (float)scene->notch_cell_x * cell;
	float dam_z = (float)scene->notch_cell_y * cell;
	struct
	{
		const char *type;
		float x;
		float z;
		float yaw_degrees;
	} items[] = {
		{ "log1", -6.0f, -12.0f, 20.0f }, { "log2", 4.0f, -15.0f, 80.0f }, { "log1", 0.0f, -20.0f, 45.0f },
		{ "barrel1", -2.0f, -9.0f, 0.0f }, { "barrel2", 2.0f, -10.0f, 0.0f }, { "barrel1", 6.0f, -18.0f, 0.0f },
		{ "crate", -4.0f, -16.0f, 30.0f }, { "boat1", 0.0f, -28.0f, 90.0f }, { "haystack1", 8.0f, -24.0f, 0.0f },
		{ "tree1", -8.0f, 14.0f, 0.0f }, { "tree1", 8.0f, 24.0f, 0.0f }, { "treeflower2", 0.0f, 28.0f, 0.0f },
		{ "tree6", 6.0f, 34.0f, 0.0f }, { "house_gnome1", -14.0f, 30.0f, 90.0f }, { "house_gnome3", 0.0f, 18.0f, 0.0f }, { "fence1", 10.0f, 12.0f, 90.0f },
		{ "fence1", 10.0f, 15.0f, 90.0f }, { "fence1", 10.0f, 18.0f, 90.0f }, { "rock_small1", -1.0f, 8.0f, 0.0f },
	};
	int placed = 0;

	for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++)
	{
		int type = object_types_find(&game->potop->world.objects.types, items[i].type);

		if (type < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Physics test: no type %s", items[i].type);
			continue;
		}
		if (objects_add(&game->potop->world.objects, &game->potop->world.terrain, type, center_x + items[i].x, dam_z + items[i].z,
			items[i].yaw_degrees * SDL_PI_F / 180.0f, 0.5f * (game->potop->world.objects.types.types[type].scale_minimum + game->potop->world.objects.types.types[type].scale_maximum), 0))
		{
			placed++;
		}
	}
	// Grass on the slope below the dam, to be laid down
	{
		int grass = object_types_find(&game->potop->world.objects.types, "grass4");

		for (int i = 0; grass >= 0 && i < 400; i++)
		{
			float x = center_x - 12.0f + (float)(i % 20) * 1.2f;
			float z = dam_z + 6.0f + (float)(i / 20) * 1.5f;

			placed += objects_add(&game->potop->world.objects, &game->potop->world.terrain, grass, x, z, (float)i, 1.2f, 0) ? 1 : 0;
		}
	}
	SDL_Log("Physics test: %d objects placed", placed);
}

int main(int argument_count, char **arguments)
{
	static testbed_t game;
	uint32_t seed = 0;
	map_file_info_t start_map = { 0 };
	bool load_at_start = false;
	bool empty_start = false;
	scene_dam_info_t dam_scene;
	bool headless;
	int exit_code = 1;

	// SDL shows only errors for most log categories by default; we want every
	// warning (missing textures, unknown settings, fallbacks) to be visible
	SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
	game.potop = potop_create();
	if (game.potop == NULL)
	{
		return 1;
	}
	startup_stage(&game, "Starting", 0.0f);
	// The console keeps the log from the start
	console_create(&game.console);
	console_add_command(&game.console, "project", "project list | new <name> [seed] | open <name> | save [name] | bake | import <map> <name>",
		"Map projects (the Save, Open and New windows): their folders under projects/", project_command, &game);
	console_add_command(&game.console, "sketch",
		"sketch open | clear | paint <region> <u> <v> <radius> | seed <n> | size <cells> | show sketch|result | preview | make <project> | picture <file.png>",
		"The new map's sketch (the Sketch window): regions plain, hills, mountains, valley, lake, erase; u, v and radius shares of the map", sketch_command,
		&game);
	if (!terrain_sketch_create(&game.sketch) || !sketch_view_create(&game.sketch_view))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Sketch: no memory for it; the sketch window is off");
	}
	SDL_SetAppMetadata("Potop", "0.3", "potop");
	parse_command_line(&game.command_line, argument_count, arguments);
	headless = game.command_line.water_test || game.command_line.dam_test || game.command_line.pour_test_seconds > 0.0f;

	// The tests run the water on the GPU too, which needs the video
	// subsystem (for Vulkan), though not a window
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
		return 1;
	}
	if (headless)
	{
		if (!gpu_create(NULL, false, 1.0f))
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The tests need a GPU (Vulkan)");
			return 1;
		}
	}
	file_io_initialize();
	settings_set_defaults(&game.potop->settings);
	settings_load(&game.potop->settings);
	// The editor's own: data/editor_settings.ini over settings.ini (no fog:
	// a map seen from above), and what is changed in it kept apart from
	// the game's
	settings_load_data_file(&game.potop->settings, "editor_settings.ini");
	settings_set_override_file("editor_settings_changed.ini");
	game.file_settings = game.potop->settings;
	settings_load_overrides(&game.potop->settings);
	if (headless)
	{
		// The tests measure the water itself: it must not stop by itself
		game.potop->settings.water.freeze_after_seconds = 0.0f;
	}
	soils_set_defaults(&game.potop->soils);
	soils_load(&game.potop->soils);
	// (the world goes by the application's settings and soils)
	world_init(&game.potop->world, &game.potop->settings, &game.potop->soils);
	game.potop->world.no_erosion = game.command_line.no_erosion;
	game.potop->world.progress = world_startup_progress;
	game.potop->world.progress_user = &game;
#ifdef _OPENMP
	if (game.potop->settings.simulation.threads > 0)
	{
		omp_set_num_threads(game.potop->settings.simulation.threads);
	}
	game.thread_count = omp_get_max_threads();
#else
	game.thread_count = 1;
#endif
	if (game.command_line.size > 0)
	{
		game.potop->settings.terrain.size = game.command_line.size;
	}
	if (game.command_line.fixed_pipes)
	{
		game.potop->settings.water.depth_scaled_pipes = false;
	}
	if (game.command_line.dam_scene)
	{
		game.potop->settings.terrain.size = SCENE_DAM_SIZE;
		game.potop->settings.terrain.cell_size_meters = SCENE_DAM_CELL_SIZE_METERS;
	}
	game.time_scale_index = TIME_SCALE_NORMAL;

	// The window first, so the loading screen shows from the start (the
	// tests need none)
	if (!headless)
	{
		startup_stage(&game, "Opening the window", 0.0f);
		if (!create_window(&game))
		{
			goto shutdown;
		}
		// (before any shadow map or its pipelines: the next start for a change)
		if (game.potop->settings.render.shadow_depth_16_bit
			&& SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_D16_UNORM, SDL_GPU_TEXTURETYPE_2D,
				SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
		{
			gpu.shadow_depth_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
			SDL_Log("Shadow maps: 16 bit depth");
		}
		if (!game.command_line.no_ui && !ui_create(&game.ui, game.window))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No interface: the keys still work (see README.md)");
		}
	}
	startup_stage(&game, "Making the land", 0.04f);
	// Starting with a saved map: the world made at its size and of its
	// kind, empty (no land generated, nothing planted: the map brings it)
	if (game.command_line.load_name != NULL && !game.command_line.dam_scene && !game.command_line.map_test)
	{
		char file_name[MAP_FILE_NAME_LENGTH + 16];
		char path[1024];
		map_file_reader_t *reader;

		if (potop_map_find(game.command_line.load_name, file_name, (int)sizeof(file_name)) && map_file_path(file_name, path, sizeof(path))
			&& (reader = map_file_open(path, &start_map)) != NULL)
		{
			map_file_close(reader);
			load_at_start = true;
		}
	}
	// Started plainly (no map, seed or size asked for): a small, empty map
	// at once, not a big one made only to be thrown away for New
	empty_start = !load_at_start && game.command_line.load_name == NULL && !game.command_line.seed_given && game.command_line.size <= 0
		&& !game.command_line.dam_scene && !game.command_line.map_test && !headless && game.command_line.screenshot_path == NULL;
	if (load_at_start ? !world_create_land(&game.potop->world, start_map.size, start_map.cell_size_meters, start_map.baked)
		: !world_create_land(&game.potop->world, empty_start ? EMPTY_START_SIZE : game.potop->settings.terrain.size,
			SDL_max(game.potop->settings.terrain.cell_size_meters, 0.01f), false))
	{
		goto shutdown;
	}
	if (game.command_line.dam_scene)
	{
		scenes_build_dam(&game.potop->world.terrain, &game.potop->world.water, &game.potop->soils, &dam_scene);
		game.potop->world.shadow.needs_render = true;
	}
	else
	{
		seed = game.command_line.seed_given ? game.command_line.seed : game.potop->settings.terrain.seed;
		if (seed == 0)
		{
			seed = pick_random_seed();
		}
		if (empty_start)
		{
			make_empty_land(&game, seed);
		}
		else if (!load_at_start)
		{
			regenerate_terrain(&game, seed);
		}
	}

	if (game.command_line.water_test)
	{
		exit_code = run_water_test(&game);
		goto shutdown;
	}
	if (game.command_line.dam_test)
	{
		exit_code = run_dam_test(&game, &dam_scene);
		goto shutdown;
	}
	if (game.command_line.pour_test_seconds > 0.0f)
	{
		exit_code = run_pour_test(&game);
		goto shutdown;
	}

	if (game.command_line.density_scale > 0.0f)
	{
		game.potop->settings.objects.density_scale = game.command_line.density_scale;
	}
	if (!world_create(&game.potop->world))
	{
		goto shutdown;
	}
	// (without weapons.ini the testbed has none: 1 does nothing in first person)
	session_create(&game.potop->session, &game.potop->world);
	random_seed(&game.object_random, (uint64_t)game.potop->world.terrain.seed ^ 0x5DEECE66Dull);
	game.tools.plant_category = 0;
	if (!load_at_start)
	{
		world_populate(&game.potop->world, game.potop->settings.objects.populate && !game.command_line.dam_scene && !empty_start);
	}
	startup_stage(&game, "Getting ready", 0.99f);
	if (game.command_line.physics_test && game.command_line.dam_scene)
	{
		place_physics_test(&game, &dam_scene);
	}
	game.potop->world.renderer.tile_overlay = game.command_line.tile_overlay;
	game.potop->world.renderer.navigation_overlay = game.command_line.navigation_overlay;
	// (here whole hills move: the far shadow map follows the edits too)
	game.potop->world.far_shadow_follows_edits = true;
	game.potop->world.navigation_overlay_seconds = -1e9;
	game.potop->world.water_renderer.highlight_thin = game.command_line.thin_highlight;
	game.potop->world.renderer.debug_view = (terrain_debug_view_t)clamp_int(game.command_line.debug_view, 0, TERRAIN_DEBUG_VIEW_COUNT - 1);

	camera_reset(&game.potop->camera, &game.potop->settings.camera, &game.potop->world.terrain);
	if (game.command_line.dam_scene)
	{
		// Looking at the dam over the lake, from a little to the side
		game.potop->camera.target_x = dam_scene.view_target.x;
		game.potop->camera.target_z = dam_scene.view_target.z;
		game.potop->camera.target_height = dam_scene.view_target.y;
		game.potop->camera.distance_meters = 110.0f;
		game.potop->camera.yaw_degrees = -110.0f;
		game.potop->camera.pitch_degrees = 35.0f;
	}
	if (game.command_line.yaw_given)
	{
		game.potop->camera.yaw_degrees = game.command_line.yaw_degrees;
	}
	if (game.command_line.pitch_given)
	{
		game.potop->camera.pitch_degrees = game.command_line.pitch_degrees;
	}
	if (game.command_line.distance_given)
	{
		game.potop->camera.distance_meters = game.command_line.distance_meters;
	}
	if (game.command_line.first_person)
	{
		camera_set_first_person(&game.potop->camera, &game.potop->settings.camera, &game.potop->world.terrain, true);
	}
	if (game.command_line.go_to_given)
	{
		camera_go_to(&game.potop->camera, &game.potop->world.terrain, game.command_line.go_to[0], game.command_line.go_to[1], game.command_line.go_to[2]);
	}
	if (game.command_line.heading_given)
	{
		camera_set_heading(&game.potop->camera, &game.potop->settings.camera, game.command_line.heading[0], game.command_line.heading[1]);
	}
	if (game.command_line.look_at_given)
	{
		// The orbit camera's eye comes from the matrices
		camera_compute_matrices(&game.potop->camera, &game.potop->settings.camera, &game.potop->world.terrain, 16.0f / 9.0f);
		camera_look_at(&game.potop->camera, &game.potop->settings.camera, game.command_line.look_at[0], game.command_line.look_at[1],
			game.command_line.look_at[2]);
	}
	tools_initialize(&game.tools, &game.potop->settings.tools);
	input_initialize(&game.input);
	game.tools.object_awake = true;
	if (game.command_line.tool >= 0 && game.command_line.tool < TOOL_COUNT)
	{
		game.tools.current_tool = (tool_type_t)game.command_line.tool;
	}
	if (game.command_line.gallery)
	{
		place_gallery(&game);
	}
	if (game.command_line.bridge_test)
	{
		// A trench along Z through the target, 6 m wide and 3 m deep, and a
		// bridge across it (along X), its deck at the old ground
		terrain_t *terrain = &game.potop->world.terrain;
		float cell = terrain->cell_size_meters;
		int bridge = object_types_find(&game.potop->world.objects.types, "bridge1");
		int32_t center_x = (int32_t)(game.potop->camera.target_x / cell);
		int32_t center_z = (int32_t)(game.potop->camera.target_z / cell);
		int32_t half_width = (int32_t)(3.0f / cell);
		int32_t half_length = (int32_t)(40.0f / cell);
		int32_t drop = (int32_t)(3.0f * TERRAIN_HEIGHT_UNITS_PER_METER);
		int32_t bounds[4] = {
			clamp_int(center_x - half_width, 0, terrain->size - 1), clamp_int(center_z - half_length, 0, terrain->size - 1),
			clamp_int(center_x + half_width, 0, terrain->size - 1), clamp_int(center_z + half_length, 0, terrain->size - 1) };

		for (int32_t z = bounds[1]; z <= bounds[3]; z++)
		{
			for (int32_t x = bounds[0]; x <= bounds[2]; x++)
			{
				terrain->heights[terrain_index(terrain, x, z)] -= drop;
			}
		}
		terrain_fix_layers(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
		terrain_mark_cells_changed(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
		terrain->shadow_dirty = true;
		if (bridge >= 0 && game.potop->world.objects.types.types[bridge].model >= 0)
		{
			const object_model_t *model = &game.potop->world.objects.models[game.potop->world.objects.types.types[bridge].model];

			SDL_Log("Bridge test: bridge1 is %.1f x %.1f x %.1f m (x, y, z), bottom at %.2f",
				(double)(model->bounds_maximum.x - model->bounds_minimum.x), (double)(model->bounds_maximum.y - model->bounds_minimum.y),
				(double)(model->bounds_maximum.z - model->bounds_minimum.z), (double)model->bounds_minimum.y);
			// Across the trench: the model's longer side along X
			objects_add_lifted(&game.potop->world.objects, terrain, bridge, game.potop->camera.target_x, game.potop->camera.target_z,
				model->bounds_maximum.x - model->bounds_minimum.x >= model->bounds_maximum.z - model->bounds_minimum.z ? 0.0f : 0.5f * MATH3D_PI,
				1.0f, 0, 3.0f - (model->bounds_maximum.y));
		}
	}
	if (game.command_line.obstacle != NULL)
	{
		char name[64];
		char *colon;
		float scale = 1.0f;
		int type;

		SDL_strlcpy(name, game.command_line.obstacle, sizeof(name));
		colon = SDL_strchr(name, ':');
		if (colon != NULL)
		{
			*colon = '\0';
			scale = (float)SDL_atof(colon + 1);
		}
		type = object_types_find(&game.potop->world.objects.types, name);
		if (type >= 0 && game.potop->world.objects.types.types[type].model >= 0)
		{
			const object_model_t *model = &game.potop->world.objects.models[game.potop->world.objects.types.types[type].model];

			objects_add(&game.potop->world.objects, &game.potop->world.terrain, type, game.potop->camera.target_x, game.potop->camera.target_z, 0.0f, scale, 0);
			SDL_Log("Obstacle: %s at size %.2f, %.2f m high", name, (double)scale,
				(double)((model->bounds_maximum.y - model->bounds_minimum.y) * scale));
		}
		else
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Obstacle: no object type '%s'", name);
		}
	}
	if (game.command_line.actor_test)
	{
		// Side by side across the view, 3 m apart
		float spacing = 3.0f;
		float across_x;
		float across_z;
		int count = 0;

		camera_compute_matrices(&game.potop->camera, &game.potop->settings.camera, &game.potop->world.terrain, 16.0f / 9.0f);
		{
			// At right angles to the view, on the ground
			float view_x = game.potop->camera.target_x - game.potop->camera.eye_position.x;
			float view_z = game.potop->camera.target_z - game.potop->camera.eye_position.z;
			float length = SDL_max(SDL_sqrtf(view_x * view_x + view_z * view_z), 1e-3f);

			across_x = -view_z / length;
			across_z = view_x / length;
		}
		for (int t = 0; t < game.potop->world.actors.type_count; t++)
		{
			count += game.potop->world.actors.types[t].model >= 0 ? 1 : 0;
		}
		for (int t = 0, k = 0; t < game.potop->world.actors.type_count; t++)
		{
			float offset = ((float)k - 0.5f * (float)(count - 1)) * spacing;
			float x = game.potop->camera.target_x + across_x * offset;
			float z = game.potop->camera.target_z + across_z * offset;

			if (game.potop->world.actors.types[t].model < 0)
			{
				continue;
			}
			if (game.command_line.bridge_test || game.command_line.obstacle != NULL)
			{
				// On the near side of the trench
				x -= 10.0f;
			}
			// Side on, heads to the right: the poses show best
			actors_add(&game.potop->world.actors, &game.potop->world.terrain, t, x, z,
				SDL_atan2f(game.potop->camera.eye_position.x - x, game.potop->camera.eye_position.z - z) + 0.5f * MATH3D_PI);
			k++;
		}
		if (game.command_line.actor_test_action != NULL)
		{
			char action[64];
			char *colon;
			float seconds = 0.0f;

			SDL_strlcpy(action, game.command_line.actor_test_action, sizeof(action));
			colon = SDL_strchr(action, ':');
			if (colon != NULL)
			{
				*colon = '\0';
				seconds = (float)SDL_atof(colon + 1);
			}
			for (int i = 0; i < game.potop->world.actors.count; i++)
			{
				if (!actors_play(&game.potop->world.actors, game.potop->world.actors.actors[i].id, action))
				{
					SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Actor test: no action '%s'", action);
				}
			}
			// In small steps, as the game would
			for (float done = 0.0f; done < seconds; done += 1.0f / 60.0f)
			{
				actors_update(&game.potop->world.actors, SDL_min(1.0f / 60.0f, seconds - done));
			}
		}
		if (game.command_line.actor_test_goal)
		{
			navigation_update(&game.potop->world.navigation, &game.potop->world.terrain, &game.potop->world.objects, 0.0);
			float goal_x = game.potop->camera.target_x + game.command_line.actor_goal_x;
			float goal_z = game.potop->camera.target_z + game.command_line.actor_goal_z;

			for (int i = 0; i < game.potop->world.actors.count; i++)
			{
				const actor_t *actor = &game.potop->world.actors.actors[i];
				uint64_t start = SDL_GetPerformanceCounter();
				bool sent = actors_move_to(&game.potop->world.actors, &game.potop->world.navigation, &game.potop->world.terrain, &game.potop->world.water, actor->id, goal_x, goal_z);

				SDL_Log("Actor test: sent from (%.1f, %.1f) to (%.1f, %.1f): %s, %d waypoints, %d cells searched in %.2f ms",
					(double)actor->x, (double)actor->z, (double)goal_x, (double)goal_z,
					!sent ? "no path" : actor->path_partial ? "as near as it gets" : "on its way",
					actor->path_count, game.potop->world.navigation.last_expansions,
					(double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency());
				for (int k = 0; k < actor->path_count; k++)
				{
					SDL_Log("  waypoint %d: (%.1f, %.1f)", k, (double)actor->path[k][0], (double)actor->path[k][1]);
				}
			}
		}
		SDL_Log("Actor test: %d actors", count);
	}
	if (game.command_line.plant_test)
	{
		// Every plantable category in its own circle, on a 3 x 3 grid
		// around the camera's target
		int planted = 0;
		int circle = 0;

		for (int category = 0; category < game.potop->world.objects.types.category_count; category++)
		{
			float x;
			float z;

			if (!object_types_category_plantable(&game.potop->world.objects.types, category))
			{
				continue;
			}
			int in_circle = 0;

			x = game.potop->camera.target_x + (float)(circle % 3 - 1) * 18.0f;
			z = game.potop->camera.target_z + (float)(circle / 3 % 3 - 1) * 18.0f;
			// Things that grow on water get the deepest spot nearby
			for (int t = 0; t < game.potop->world.objects.types.type_count; t++)
			{
				const object_type_t *type = &game.potop->world.objects.types.types[t];

				if (type->category == category && type->spawn_water_minimum_meters > 0.0f)
				{
					float deepest = 0.0f;

					for (float search_z = -50.0f; search_z <= 50.0f; search_z += 4.0f)
					{
						for (float search_x = -50.0f; search_x <= 50.0f; search_x += 4.0f)
						{
							int32_t cell_x;
							int32_t cell_y;

							if (terrain_world_to_cell(&game.potop->world.terrain, game.potop->camera.target_x + search_x, game.potop->camera.target_z + search_z, &cell_x, &cell_y)
								&& (float)game.potop->world.water.depth[terrain_index(&game.potop->world.terrain, cell_x, cell_y)] * TERRAIN_METERS_PER_HEIGHT_UNIT > deepest)
							{
								deepest = (float)game.potop->world.water.depth[terrain_index(&game.potop->world.terrain, cell_x, cell_y)] * TERRAIN_METERS_PER_HEIGHT_UNIT;
								x = game.potop->camera.target_x + search_x;
								z = game.potop->camera.target_z + search_z;
							}
						}
					}
					break;
				}
			}
			for (int k = 0; k < 20; k++)
			{
				in_circle += populate_brush(&game.potop->world.objects, &game.potop->world.terrain, &game.potop->world.water, &game.potop->settings.objects, category, x, z, 8.0f, 0.1f,
					&game.object_random);
			}
			SDL_Log("Plant test: %d %s at (%.0f, %.0f)", in_circle, game.potop->world.objects.types.categories[category], (double)x, (double)z);
			planted += in_circle;
			circle++;
		}
		SDL_Log("Plant test: %d objects in %d circles", planted, circle);
	}
	if (game.command_line.inspect)
	{
		game.inspector.pinned = true;
		inspector_set_cell(&game.inspector, clamp_int((int32_t)SDL_lroundf(game.potop->camera.target_x / game.potop->world.terrain.cell_size_meters), 0,
			game.potop->world.terrain.size - 1), clamp_int((int32_t)SDL_lroundf(game.potop->camera.target_z / game.potop->world.terrain.cell_size_meters), 0, game.potop->world.terrain.size - 1));
	}
	if (game.command_line.layout_name != NULL)
	{
		int layout = layouts_find(&game.potop->world.layouts, game.command_line.layout_name);

		if (layout < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unknown layout '%s'", game.command_line.layout_name);
		}
		else
		{
			int group = layouts_place(&game.potop->world.layouts, &game.potop->world.objects, &game.potop->world.terrain, &game.potop->world.water, layout, game.potop->camera.target_x,
				game.potop->camera.target_z, &game.object_random);
			int count = 0;

			for (int i = 0; i < game.potop->world.objects.instance_count; i++)
			{
				count += game.potop->world.objects.instances[i].group == group ? 1 : 0;
			}
			SDL_Log("Layout '%s' placed: %d objects", game.command_line.layout_name, count);
		}
	}
	if (game.command_line.load_name != NULL)
	{
		startup_stage(&game, "Loading the map", 0.98f);
		if (!load_map(&game, game.command_line.load_name) && load_at_start)
		{
			// (nothing to start with: a map from the settings after all)
			if (!resize_world(&game, game.potop->settings.terrain.size, game.potop->settings.terrain.cell_size_meters, false))
			{
				goto shutdown;
			}
			regenerate_terrain(&game, seed);
			navigation_forget(&game.potop->world.navigation, &game.potop->world.terrain, &game.potop->world.objects);
		}
	}
	if (game.command_line.map_test)
	{
		exit_code = run_map_test(&game);
		goto shutdown;
	}
	if (game.ui.available)
	{
		SDL_SetWindowTitle(game.window, "Potop");
		if (game.potop->world.map_name[0] != '\0')
		{
			ui_set_map_name(&game.ui, game.potop->world.map_name);
		}
		if (game.command_line.open_window != NULL)
		{
			const char *window = game.command_line.open_window;

			game.ui.settings.open = SDL_strcmp(window, "settings") == 0 || SDL_strncmp(window, "settings:", 9) == 0;
			if (SDL_strncmp(window, "settings:", 9) == 0)
			{
				game.ui.settings.tab = SDL_atoi(window + 9);
			}
			game.ui.diagnostics_open = SDL_strcmp(window, "diagnostics") == 0;
			game.ui.sky_open = SDL_strcmp(window, "sky") == 0;
			if (SDL_strcmp(window, "console") == 0)
			{
				console_set_open(&game.console, true);
			}
			if (SDL_strcmp(window, "new") == 0)
			{
				game.ui.sketch_open = true;
				game.ui.sketch_new_pending = true;
			}
			else if (SDL_strcmp(window, "save") == 0 || SDL_strcmp(window, "load") == 0)
			{
				game.ui.maps_mode = window[0] == 's' ? UI_MAPS_SAVE : (window[0] == 'l' ? UI_MAPS_LOAD : UI_MAPS_NEW);
				game.ui.map_entry_count = map_file_list(game.ui.map_entries, MAP_FILE_LIST_MAX);
				game.ui.project_count = project_list(game.ui.projects, PROJECT_LIST_MAX);
				game.ui.selected_map = -1;
				game.ui.selected_project = -1;
				SDL_strlcpy(game.ui.project_name_input, "my map", sizeof(game.ui.project_name_input));
			}
		}
	}
	if (game.command_line.sky_test)
	{
		// No clouds but over the map, where they are heavy towers
		float u = game.potop->camera.target_x / CLOUD_WEATHER_METERS;
		float v = game.potop->camera.target_z / CLOUD_WEATHER_METERS;

		for (size_t t = 0; t < (size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE; t++)
		{
			game.potop->world.cloud_paint.values[t * 4 + CLOUD_PAINT_COVERAGE] = 0;
		}
		cloud_paint_brush(&game.potop->world.cloud_paint, CLOUD_PAINT_COVERAGE, u, v, 0.12f, 1.0f, false);
		cloud_paint_brush(&game.potop->world.cloud_paint, CLOUD_PAINT_KIND, u, v, 0.12f, 0.5f, false);
		cloud_paint_brush(&game.potop->world.cloud_paint, CLOUD_PAINT_DENSITY, u, v, 0.12f, 0.4f, false);
	}
	if (game.command_line.console_commands != NULL)
	{
		char commands[1024];
		char *rest = NULL;

		SDL_strlcpy(commands, game.command_line.console_commands, sizeof(commands));
		for (char *piece = SDL_strtok_r(commands, ";", &rest); piece != NULL; piece = SDL_strtok_r(NULL, ";", &rest))
		{
			run_console_command(&game, piece);
		}
	}
	daylight_apply(&game.potop->settings.render, game.potop->world.day_hour, game.potop->world.moon_age_days, &game.potop->world.lit_render, &game.potop->world.daylight);
	game.potop->world.drawn_sun_azimuth = game.potop->world.lit_render.sun_azimuth_degrees;
	game.potop->world.drawn_sun_elevation = game.potop->world.lit_render.sun_elevation_degrees;
	if (game.command_line.effect != NULL)
	{
		int effect = particles_find_effect(&game.potop->world.particles, game.command_line.effect);

		if (effect < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Effect: no effect '%s' in particles.ini", game.command_line.effect);
		}
		else
		{
			vec3_t target = vec3_make(game.potop->camera.target_x,
				terrain_sample_height_meters(&game.potop->world.terrain, game.potop->camera.target_x, game.potop->camera.target_z), game.potop->camera.target_z);

			play_effect(&game, effect, target, false);
		}
	}
	if (game.command_line.warmup_seconds > 0.0f)
	{
		const float step_seconds = 1.0f / (float)SDL_max(1, game.potop->settings.simulation.steps_per_second);

		if (game.command_line.walk_test)
		{
			navigation_update(&game.potop->world.navigation, &game.potop->world.terrain, &game.potop->world.objects, 0.0);
			camera_enter_first_person(&game.potop->camera, &game.potop->world.terrain);
			game.potop->camera.walker_z -= 6.0f;
			game.potop->camera.walker_x += game.command_line.walk_offset;
			game.potop->camera.walker_feet = terrain_sample_height_meters(&game.potop->world.terrain, game.potop->camera.walker_x, game.potop->camera.walker_z);
			game.potop->camera.walker_yaw_degrees = 180.0f;    // facing +Z, the obstacle
			if (game.command_line.walk_start_given)
			{
				float x = game.potop->camera.target_x + game.command_line.walk_start[0];
				float z = game.potop->camera.target_z + game.command_line.walk_start[1];

				game.potop->camera.walker_x = x;
				game.potop->camera.walker_z = z;
				// (a meter over the terrain: it lands on whatever is there)
				game.potop->camera.walker_feet = terrain_sample_height_meters(&game.potop->world.terrain, x, z) + 1.0f;
				game.potop->camera.walker_on_ground = false;
				game.potop->camera.walker_yaw_degrees = game.command_line.walk_start[2];
			}
		}
		for (float simulated = 0.0f; simulated < game.command_line.warmup_seconds; simulated += step_seconds)
		{
			simulation_step(&game, step_seconds);
			water_gpu_end_frame(&game.potop->world.water, &game.potop->world.terrain, game.potop->camera.target_x, game.potop->camera.target_z);
			water_gpu_poll(&game.potop->world.water);
			if (particles_emitter_count(&game.potop->world.particles) > 0)
			{
				// The terrain's textures are uploaded by the first frame only
				if (game.potop->world.renderer.height_texture == 0)
				{
					terrain_renderer_upload(&game.potop->world.renderer, &game.potop->world.terrain, &game.potop->world.water, &game.potop->soils, &game.potop->camera, &game.potop->settings.render,
						game.potop->world.elapsed_seconds);
				}
				follow_effects(&game);
				particles_update(&game.potop->world.particles, step_seconds, game.potop->world.renderer.height_texture, water_gpu_depth_texture(&game.potop->world.water),
					game.potop->world.terrain.size, game.potop->world.terrain.cell_size_meters);
			}
			if (game.command_line.walk_test)
			{
				input_state_t walking;

				SDL_zero(walking);
				walking.pan_keyboard_y = 1.0f;
				player_walk(&game.potop->session.player, &game.potop->world, &game.potop->camera, &walking, &game.potop->settings.camera, step_seconds);
				if (SDL_fmodf(simulated, game.command_line.walk_start_given ? 0.1f : 0.5f) < step_seconds)
				{
					const object_instance_t *nearest = NULL;
					float nearest_squared = 1e30f;

					for (int i = 0; i < game.potop->world.objects.instance_count; i++)
					{
						const object_instance_t *instance = &game.potop->world.objects.instances[i];
						float dx = instance->x - game.potop->camera.walker_x;
						float dz = instance->z - game.potop->camera.walker_z;

						if (game.potop->world.objects.types.types[instance->type].physics == OBJECT_PHYSICS_BODY && dx * dx + dz * dz < nearest_squared)
						{
							nearest_squared = dx * dx + dz * dz;
							nearest = instance;
						}
					}
					SDL_Log("Walk test %.1f s: over the terrain %.2f, pushed %.3f (%d planes)", (double)simulated,
						(double)(game.potop->camera.walker_feet - terrain_sample_height_meters(&game.potop->world.terrain, game.potop->camera.walker_x, game.potop->camera.walker_z)),
						(double)SDL_sqrtf(game.potop->world.physics.walker_push_x * game.potop->world.physics.walker_push_x
						+ game.potop->world.physics.walker_push_z * game.potop->world.physics.walker_push_z), game.potop->world.physics.walker_planes_kept);
					SDL_Log("Walk test %.1f s: walker (%.2f, %.2f) feet %.2f; nearest body %s at (%.2f, %.2f) y %.2f%s", (double)simulated,
						(double)game.potop->camera.walker_x, (double)game.potop->camera.walker_z, (double)game.potop->camera.walker_feet,
						nearest != NULL ? game.potop->world.objects.types.types[nearest->type].name : "none", nearest != NULL ? (double)nearest->x : 0.0,
						nearest != NULL ? (double)nearest->z : 0.0, nearest != NULL ? (double)objects_instance_base_y(nearest) : 0.0,
						nearest != NULL && (nearest->state & OBJECT_STATE_FREE) ? " (loose)" : "");
				}
			}
			if (game.command_line.actor_test_goal)
			{
				// Sent actors go on their way meanwhile
				actors_move(&game.potop->world.actors, &game.potop->world.navigation, &game.potop->world.terrain, &game.potop->world.water, step_seconds);
				actors_update(&game.potop->world.actors, step_seconds);
				report_actor_events(&game);
			}
		}
		game.potop->world.terrain.shadow_dirty = true;
	}

	startup_stage(&game, NULL, 1.0f);
	run(&game);
	exit_code = game.command_line.ui_test && game.command_line.ui_test_result != 1 ? 1 : 0;

shutdown:
	tools_shutdown(&game.tools);
	sketch_view_destroy(&game.sketch_view);
	terrain_sketch_destroy(&game.sketch);
	if (game.gpu_ready)
	{
		ui_destroy(&game.ui);
		session_destroy(&game.potop->session, &game.potop->world);
	}
	// (the water's GPU resources go before the device does)
	world_destroy(&game.potop->world);
	gpu_destroy();
	if (game.window != NULL)
	{
		SDL_DestroyWindow(game.window);
	}
	console_destroy(&game.console);
	potop_destroy(game.potop);
	SDL_Quit();
	return exit_code;
}
