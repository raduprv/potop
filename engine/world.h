// The world: a map and everything on it, as the engine keeps it. The
// terrain and its water, the objects and the actors on them, the physics
// moving what is loose, the particles, the lights, the sky, the time of
// day, the weather, and what draws all of it (the renderers, the shadow
// maps, the frame's targets). The camera is not in it: it is the
// application's, given to the drawing.
//
// No globals: there could be more than one (the GPU's device is one,
// gpu.h, so only one is drawn).

#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "actors.h"
#include "aurora.h"
#include "clouds.h"
#include "cloud_paint.h"
#include "ambient_occlusion.h"
#include "puddles.h"
#include "daylight.h"
#include "erosion.h"
#include "fireflies.h"
#include "object_effects.h"
#include "ground_footprints.h"
#include "fog.h"
#include "gpu_timer.h"
#include "grass.h"
#include "grass_mask.h"
#include "terrain_sketch.h"
#include "ground_items.h"
#include "ground_overlay.h"
#include "hiz.h"
#include "layouts.h"
#include "lights.h"
#include "map_file.h"
#include "navigation.h"
#include "noise.h"
#include "objects.h"
#include "particles.h"
#include "physics.h"
#include "post.h"
#include "rain.h"
#include "render_target.h"
#include "settings.h"
#include "shadow_map.h"
#include "sky_occlusion.h"
#include "sky_renderer.h"
#include "terrain.h"
#include "terrain_renderer.h"
#include "water.h"
#include "water_ice.h"
#include "water_renderer.h"
#include "waterfalls.h"
#include "areas.h"
#include "noises.h"
#include "camera.h"

typedef struct
{
	// What it goes by (the application's, given before anything is done
	// with it): the settings, the soils, whether the ground erodes
	settings_t *settings;
	const soil_settings_t *soils;
	bool no_erosion;
	// What the last steps took (seconds; the application reads and clears
	// them), whether the last one moved the water
	double water_seconds_sum;
	double erosion_seconds_sum;
	double physics_seconds_sum;
	bool water_stepped;
	// The world's clock: real seconds since it began, paused or not (what
	// the renderers animate by; the application moves it on each frame)
	double elapsed_seconds;
	bool objects_were_hidden;
	// The smoothing brush's copy of the ground it reads (edit.h)
	int32_t *edit_scratch;
	int32_t edit_scratch_capacity;
	// The sculpt brush's (edit_sculpt): its noise (from the map's seed, made
	// once), its region's heights and their large shapes
	noise_context_t sculpt_noise;
	noise_context_t sculpt_ridges;
	bool sculpt_noise_ready;
	uint32_t sculpt_noise_seed;
	float *sculpt_scratch;
	int32_t sculpt_scratch_capacity;
	double water_copy_milliseconds; // this frame's water copies to and from the GPU (diagnostics)
	bool renderers_created;        // world_create ran (world_destroy lets go of them)
	// Told what is being made and how far along (0..1), for a loading
	// screen (NULL: nothing told)
	void (*progress)(void *user, const char *stage, float fraction);
	void *progress_user;      // the objects' hidden flag the shadows were last drawn with

	terrain_t terrain;
	water_t water;
	erosion_t erosion;
	terrain_renderer_t renderer;
	water_renderer_t water_renderer;
	shadow_map_t shadow;
	shadow_map_t near_shadow;      // the fine cascade around the camera's target
	double last_near_shadow_render_seconds;
	objects_t objects;
	actors_t actors;
	particles_t particles;
	lights_t lights;               // the local lights (fires, torches), gathered each frame
	decals_t decals;               // marks on things (bullet holes, scorches), clustered with the lights
	waterfalls_t waterfalls;       // where the water falls, played as particles
	sky_occlusion_t sky_occlusion; // how much sky each cell sees (worked out once per map)
	// What a new map is made from besides its seed (the editor's sketch,
	// terrain_sketch.h; NULL: the seed alone); its lakes filled after
	terrain_sketch_t *generation_sketch;
	fireflies_t fireflies;
	object_effects_t object_effects; // the fires and smoke the buildings carry (object_effects.h)
	ground_footprints_t ground_footprints; // the ground under the buildings: no grass, no terrain drawn (ground_footprints.h)
	aurora_t aurora;               // the northern lights (aurora.h): the game's (and the World window's) say
	rain_t rain;                   // the rain as it is seen, and the rainbow (rain.h)
	fog_t fog;                     // the valley fog (fog.h)
	puddles_t puddles;                   // what the puddles mirror (puddles.h)
	ambient_occlusion_t ambient_occlusion; // the small scale ambient occlusion from the scene's depth (ambient_occlusion.h)
	grass_t grass;                 // blades made on the GPU every frame (grass.h)
	grass_mask_t grass_mask;       // where they thin away round the things that stand (grass_mask.h)
	ground_overlay_t ground_overlay; // snow and petals on the ground (ground_overlay.h)
	water_ice_t ice;               // the ice on the water (water_ice.h)
	int frames_since_load;         // the first frames after a map was loaded: their times logged (what the GPU took up)
	double ice_seconds;            // game time it was last given
	float ice_month;               // the month it was last given (a jump: as the new one says at once)
	double ground_overlay_seconds; // game time it was last given
	float ground_overlay_month;    // the month it was last given (a jump: as the new one says at once)
	// The snow on things (objects_t.snow_amount): it grows as it snows, it
	// melts after the winter; the game time and month it last went by, and
	// what the shadows were last drawn with (the branches sag under it)
	double snow_seconds;
	float snow_month;
	float snow_shadowed;
	bool snow_started;
	double aurora_seconds;         // the clock it was last eased at
	bool physics_off;              // the World window (debugging): no physics steps, the bodies stay where they are
	bool far_shadow_off;           // the World window (debugging): the far shadow map neither drawn nor used
	bool near_shadow_off;          // ... the near one
	bool shadows_were_off[2];      // the far and the near map off the frame before (either way): back on, drawn again
	bool far_shadow_had_plants;     // render.debug_far_shadow_no_plants as the far map was last drawn (switched: drawn again)
	// The temporal anti-aliasing (post_temporal): the frames counted (the
	// jitter's pattern), this frame's view and projection unjittered and its
	// inverse, last frame's, whether there was one
	uint32_t temporal_frame;
	float temporal_jitter[2];      // the scene's shift this frame (clip space)
	// (from the eye: camera_t's _relative ones, and the eye's place, so that
	// the reprojection carries only how far the eye moved, not kilometers)
	mat4_t temporal_view_projection;
	mat4_t temporal_inverse;
	mat4_t temporal_previous_view_projection;
	vec3_t temporal_eye;
	vec3_t temporal_previous_eye;
	bool temporal_previous_valid;
	// (render.taa_when_moving: on only while the eye goes somewhere, a
	// little after it stops; standing, turning round, off)
	bool temporal_active;
	vec3_t temporal_last_eye;
	double temporal_moving_until;
	// Where actors can go (navigation.h); F8's overlay of it
	navigation_t navigation;
	uint8_t *navigation_overlay;   // F8: what goes to the terrain renderer
	int navigation_overlay_size;
	double navigation_overlay_seconds; // when it was last filled
	// Not followed at all (the editor, for now: building it for a big map
	// takes minutes and nothing there walks); the actors stand still too,
	// their paths would be looked for on a navigation of another map
	bool navigation_off;
	physics_t physics;             // the objects as rigid bodies (physics.h)
	// What lies on the ground (dropped), looked over now and then for
	// objects gone
	ground_items_t ground_items;
	float ground_items_check_seconds;
	// Game hours gone by not yet given to what was picked (it grows again)
	double harvest_hours_pending;
	uint32_t harvest_random;
	layouts_t layouts;             // villages, camps... (layouts.ini)
	areas_t areas;                 // the map's areas (areas.h): kept with it
	noises_t noises;               // who made a sound where, lately (noises.h)
	// Drawing: what the frame goes through
	gpu_timer_t gpu_timer;
	render_target_t scene_target;  // the scene's light (floats), at render_scale
	post_t post;                   // the scene's light into the picture: exposure, bloom, tone mapping
	hiz_t hiz;                     // the terrain's depth pyramid, for the objects' occlusion culling
	clouds_t clouds;               // volumetric clouds (render.volumetric_clouds)
	cloud_paint_t cloud_paint;     // the sky as the player painted it (the sky window; saved with the map)
	float sky_eye[2];              // where the eye is on the clouds' map (meters, wind taken off), for the sky window
	double sky_seconds;            // the sky's clock: the clouds, their wind and their lives (the console's time)
	double day_hour;               // the time of day, 0..24 (daylight.h; the console's hour and freeze)
	double moon_age_days;          // days since the last new moon (the console's moon)
	bool day_frozen;
	double reload_check_seconds;   // when the object files were last looked at (objects_reload_changed)
	daylight_t daylight;           // what the hour does, this frame
	render_settings_t lit_render;  // the render settings with this hour's sun and sky: what the frame is drawn with
	float sky_time_scale;          // how fast it goes (1: as the game's)
	sky_renderer_t sky_renderer;
	char map_name[MAP_FILE_NAME_LENGTH];  // the map was loaded or saved as this
	float drawn_sun_azimuth;       // the shadows were drawn for this sun
	float drawn_sun_elevation;
	// The water simulation (with erosion, springs and soaking) is stopped:
	// F7, or freeze_after_seconds without touching terrain or water. The
	// physics keeps running on the frozen flow.
	bool water_frozen;
	// The cold holds all of the water still (the ice, water_ice.h: from
	// WATER_ICE_HOLDS_WINTER on, the simulation does not step; F7 and the
	// tools do not start it, the end of the winter does)
	bool water_iced;
	double baked_refused_seconds;  // when the last tool was refused on a baked map
	float water_quiet_seconds;     // since the player last touched terrain or water
	double last_shadow_render_seconds;
	double last_moving_shadow_render_seconds; // the near map's moving layer
	// The far shadow map follows the light (the sun, at night the moon):
	// drawn again once it has turned more than render.far_shadow_turn_degrees from
	// this, the way it was last drawn. Objects and the ground changing leave
	// it be (a stone washed away, a crater: the near map shows those), but
	// in the editor (far_shadow_follows_edits) where whole hills move
	vec3_t far_shadow_light;
	bool far_shadow_follows_edits;
	// Why the far map is to be drawn again (FAR_SHADOW_FOR_ bits:
	// world_render.c), counted when it is (profile.h)
	uint32_t far_shadow_reasons;
	// The square of the ground the far map covers with a view distance
	// (camera.h; half its side 0: the whole map), moved on in steps as the
	// camera goes
	float far_shadow_center_x;
	float far_shadow_center_z;
	float far_shadow_half_side;
	// The editor's layers: the water, the actors, the grass blades not drawn
	// (all of them go on, only unseen)
	bool water_hidden;
	bool actors_hidden;
	bool grass_hidden;
	double simulation_accumulator_seconds;
	uint64_t simulation_step_count;

	// Diagnostics, logged every diagnostics_interval_seconds of simulated time
	struct
	{
		double simulated_seconds;
		double water_milliseconds_sum;
		double water_milliseconds_max;
		double erosion_milliseconds_sum;
		double erosion_milliseconds_max;
		int step_count;
	} diagnostics;
} world_t;

// The player changed terrain or water (a tool, a spring, rain, a new map):
// the water runs again, and the freeze timer starts over
// Before anything else: the settings and soils the world goes by (the
// application's, kept), the morning of a new day, the sky as the settings
// make it
void world_init(world_t *world, settings_t *settings, const soil_settings_t *soils);
// The land and its water at that size (no renderers: the headless tests
// need no more), empty
bool world_create_land(world_t *world, int32_t size, float cell_size_meters, bool baked);
// The rest: renderers, objects (their types and models), actors,
// navigation, physics, the villages' layouts. Nothing planted yet.
bool world_create(world_t *world);
// A new map's: trees and villages planted (plant), every object into the
// physics, the paths found (a loaded map: world_load does its part)
void world_populate(world_t *world, bool plant);
// Lets go of everything (session_destroy first: what lies on the ground is
// in the session's inventories)
void world_destroy(world_t *world);

// Once a frame: what is followed as the settings change (the season in
// the plants, the snow on things, the shadows after the sun, the levels of
// detail)
void world_follow_settings(world_t *world);
// The settings that rebuild things (textures, shadow maps: Apply)
void world_apply_render_settings(world_t *world);
// The vegetation grown anew (what was put by hand stays)
void world_regrow(world_t *world);

void world_touch_water(world_t *world);

// Terrain, water, erosion and the objects' grid made anew at that size,
// empty (false: out of memory, the game cannot go on)
bool world_resize(world_t *world, int32_t size, float cell_size_meters, bool baked);

// A new active map from the terrain settings and the seed: the land, its
// vegetation and villages (objects.populate), nothing else placed (false:
// out of memory)
bool world_generate(world_t *world, uint32_t seed);

// One step of the simulation: the water (unless frozen or iced), the
// erosion, the physics
void world_step(world_t *world, float step_seconds);
// The modules' doors as ways through for the navigation (navigation_set_links):
// each frame, before the actors move
void world_update_door_links(world_t *world);

// A frame, in its order: world_begin_frame (the clocks, the hour, the
// light, the rain, models changed on disk), then the application's input,
// world_update_actors, world_simulate (the fixed steps), what the
// application does with their results, world_end_frame, world_render.
void world_begin_frame(world_t *world, double frame_seconds);
// The actors move and animate (their events wait in actors_poll_event)
void world_update_actors(world_t *world, float seconds);
// Fixed steps for game_seconds of (scaled) time, the bodies near (focus_x,
// focus_z) made first; each step is step(user, seconds) (NULL: world_step;
// an editor puts its brushes in before it). exactly_one: one step whatever
// the time (screenshots). How many steps
int world_simulate(world_t *world, double game_seconds, float focus_x, float focus_z, bool exactly_one, void (*step)(void *user, float step_seconds),
	void *user);
// After the steps: the fireflies and particles (seconds of game time), the
// water's textures, the bodies and the water between the last two steps
// (held: paused, the water as the last step left it; exact: both as it
// left them), the impostors
void world_end_frame(world_t *world, const camera_t *camera, float seconds, bool held, bool exact);

// How a frame is drawn, beside the world and the camera
typedef struct
{
	brush_outline_t brush;         // the outline on the ground (radius 0: none)
	bool paused;                   // the actors stand still: their shadows are not redrawn for moving
	bool capture;                  // the frame goes to a screenshot (post_draw)
	// Called in the scene's pass after everything of the world, before the
	// exposure and tone (the weapon in hand); may be NULL
	void (*draw_in_scene)(void *user, SDL_GPURenderPass *pass, int scene_width, int scene_height);
	void *user;
} world_render_options_t;

// Draws the world as the camera sees it, into the swapchain (gpu.h's frame
// must have begun); marks the camera under water or not
void world_render(world_t *world, camera_t *camera, const world_render_options_t *options, int pixel_width, int pixel_height);

// What a map's file keeps besides the land (map_file.h): the lights, the
// effects, the waterfalls (search: the baked map's, looked for), the sky's
// occlusion, what was picked, the actors. The items are the session's
// (session_gather_items)
void world_gather_features(world_t *world, bool baked, map_features_t *features, waterfalls_t *search);
// A loaded map's (the ones placed before go)
void world_apply_features(world_t *world, const map_features_t *features);
// The map into its file (path: map_file_path's), the session's items with
// it (NULL: none), the camera's place (where it opens). Baked: for play
// (a baked map can only be saved baked)
struct session_s;
bool world_save(world_t *world, struct session_s *session, const char *path, bool baked, const camera_t *camera);
// A map from its file instead of the one there is (the session's items
// too: NULL, none); info_out: what it says (its camera), complete_out:
// false if parts of it were damaged. False if it could not be read (or
// memory ran out: the game cannot go on then)
bool world_load(world_t *world, struct session_s *session, const char *path, map_file_info_t *info_out, bool *complete_out);

// Parts of those on their own (a game's own saves): the actors (a loaded
// map's instead of the ones there were; their ids new, the game's numbers
// kept), what was picked off the objects (found again by type and place)
void world_gather_actors(world_t *world, map_features_t *features);
void world_apply_actors(world_t *world, const map_features_t *features);
void world_gather_areas(world_t *world, map_features_t *features);
void world_apply_areas(world_t *world, const map_features_t *features);
void world_gather_harvest(world_t *world, map_features_t *features);
void world_apply_harvest(world_t *world, const map_features_t *features);

#endif
