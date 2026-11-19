// Game settings: compiled-in defaults, overridden by data/settings.ini and
// then by command line options. Everything that we expect to tweak while
// tuning the look and feel lives here instead of being hardcoded.

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The clouds start no lower than this: well above every mountain, so the
// land is never inside them (clouds.h relies on it)
#define CLOUD_MINIMUM_HEIGHT_METERS 400.0f
// The planet's surface under the eye at most this far down (the dome holds
// the sky down to 0.2 radians under the horizon: sqrt(2 * drop / radius)
// stays within it)
#define CLOUD_MAXIMUM_HORIZON_DROP_METERS 3000.0f
// Fewer steps than this leave the volumetric clouds more noise than cloud
#define CLOUD_MINIMUM_STEPS 32

typedef enum
{
	CLOUD_TYPE_CUSTOM = 0,
	CLOUD_TYPE_SMALL,      // fair weather: small white puffs
	CLOUD_TYPE_MEDIUM,     // heaps with swelling tops
	CLOUD_TYPE_STORM,      // tall towers with dark bases
	CLOUD_TYPE_OVERCAST,   // a grey blanket
	CLOUD_TYPE_COUNT
} cloud_type_t;

typedef struct
{
	// Terrain size in cells per side; rounded to a multiple of the chunk size
	int32_t size;
	// 0 means "pick a random seed at startup"
	uint32_t seed;
	float cell_size_meters;
	float max_height_meters;
	// Number of large features (hills, valleys) across the whole map
	float feature_count;
	// Smallest noise wavelength; the octave count follows from it, so finer
	// maps do not get cliffs made of single-cell noise
	float minimum_feature_meters;
	// Amplitude ratio between consecutive octaves (lower = smoother)
	float persistence;
	// Domain warp strength, as a fraction of the feature size
	float warp_amount;
	// 0 = only rolling hills, 1 = strong ridged mountains
	float mountain_amount;
	// Fraction of the map covered by mountains
	float mountain_coverage;
	// Thermal erosion: material slides while the drop to a neighbor exceeds
	// talus_slope (meters per meter)
	int32_t thermal_iterations;
	float talus_slope;
	// Steep slopes' gullies and buttresses, carved when the map is made
	// (meters deep; 0: none)
	float gully_depth_meters;
	// How much soil stays on the rock in pockets (ledges, the tops of
	// buttresses, gentler bits up a mountain: earth, so grass and trees grow
	// there; 0: bare rock as the slope and height say)
	float mountain_soil_pockets;
	// Soil classification thresholds
	float rock_slope;             // steeper than this (meters/meter) becomes rock
	float rock_height_fraction;   // above this fraction of max height becomes rock
	float sand_height_fraction;   // below this fraction of max height becomes sand
	float clay_height_fraction;   // between sand and this, in flat areas, becomes clay
} terrain_settings_t;

typedef struct
{
	// Generated terrains are saved and reused when the same seed and
	// settings come up again
	bool use_cache;
	int32_t cache_maximum_files;
} cache_settings_t;

typedef struct
{
	float field_of_view_degrees;
	float minimum_pitch_degrees;
	float maximum_pitch_degrees;
	float default_pitch_degrees;
	float minimum_distance_meters;
	float maximum_distance_meters;
	float rotate_degrees_per_pixel;
	// Keyboard pan speed in screen heights per second
	float keyboard_pan_speed;
	float zoom_step_factor;
	// First person on foot (C)
	float first_person_field_of_view_degrees;
	float first_person_near_meters;       // the near plane on foot (0.1): farther, the depth finer far off (less flicker), closer walls cut
	float jump_height_scale;              // how much higher the walker jumps than its own (1; 4: up onto the houses, to look round)
	float first_person_look_degrees_per_pixel;
	bool first_person_invert_mouse;   // moving the mouse forward looks down
	float walk_meters_per_second;
	float run_meters_per_second;
} camera_settings_t;

typedef struct
{
	bool vsync;
	float max_fps;                // the frames a second at most (the loop waits out the rest of each); 0: no limit
	bool start_fullscreen;        // the window starts as fullscreen (no change of size after start)
	bool start_maximized;         // ... or maximized (a window still)
	float sky_color[3];           // at the horizon, and what distant land fades into
	float sky_zenith_color[3];    // straight up
	// Fog density per meter at height 0; the fog uses exp(-(distance * density)^2),
	// the density thinning upwards: e times thinner every fog_height_meters
	// (the clouds, high up, are seen through less of it)
	float fog_density;
	float fog_height_meters;
	float sun_azimuth_degrees;    // 0 = from +X, 90 = from +Z (south)
	float sun_elevation_degrees;
	float sun_color[3];
	float sun_intensity;
	float ambient_intensity;
	// The sky's occlusion (sky_occlusion.h): the ground and the grass see
	// less of the sky in hollows, under trees, round rocks
	bool sky_occlusion;                    // drawn with (it is worked out either way: the snow, the game go by it)
	float sky_occlusion_intensity;         // 0: none, 1: all of it
	float bounce_light;                    // the light the land round sends back where it hides the sky (0: none)
	float sky_occlusion_least_height;      // things lower than this (m) cast none (a fern would shade itself)
	int32_t sky_occlusion_directions;      // from how many directions of the sky (4..256)
	float sky_occlusion_texel_meters;      // the depth maps' texels (smaller: finer shade of small things, slower)
	float sky_occlusion_lowest_degrees;    // the lowest direction, above the horizon
	int32_t sky_occlusion_directions_per_frame; // while it is worked out
	int32_t texture_size;         // all terrain textures are resampled to this
	float anisotropy;
	float normal_strength;        // scales the relief derived from height maps
	float snow_height_fraction;   // snow line as a fraction of max height
	float snow_aspect_shift;      // how much lower the snow line is on shady slopes
	// Fine texture detail fades out between these distances, leaving the
	// large scale (macro) texture
	float detail_fade_start_meters;
	float detail_fade_end_meters;
	int32_t shadow_map_size;      // 0 disables shadows
	// The fine shadow map around the camera's target (for the shadows of
	// branches): texels per side (0 disables it) and width in meters
	int32_t near_shadow_map_size;
	float near_shadow_meters;
	float shadow_strength;        // 0 = no shadows, 1 = full
	float contact_shadow_meters;  // the contact shadows (with the ambient occlusion, ambient_occlusion.h): how far toward the sun they look
	float contact_shadow_strength; // how dark, of the maps' shadows (1: as dark; up to 1.5)
	bool ambient_occlusion;       // the small scale ambient occlusion from the scene's depth (ambient_occlusion.h)
	float ambient_occlusion_meters;   // how far round a point it looks (its radius)
	float ambient_occlusion_strength; // how much of the sky's light it takes away (1: all it finds; up to 2)
	float ambient_occlusion_distance; // how far from the eye it is drawn (faded out toward it)
	float water_shallow_color[3];
	float water_deep_color[3];
	float water_clarity_meters;   // depth at which the deep color takes over
	float water_opacity_per_meter;
	bool water_reflections;       // the water mirrors the land and the trees (on screen) and the volumetric clouds; off: the sky alone
	bool puddles;                 // the ground holds puddles after rain (and the thin water on it): flat, dark, mirroring the scene (puddles.h)
	bool water_reflections_half_size; // the reflections marched for a quarter of the pixels, apart (faster, a little softer)
	float water_choppiness;       // how steep the small ripples over the waves are (0: a mirror between the waves)
	float water_refraction;       // how far the ripples bend what is seen under the water (with the reflections: the scene's copy); 0: not at all
	float water_reflection_meters; // the land mirrored out to this far from the eye (times the scope's zoom); beyond, the sky alone (0: everywhere)
	bool shadow_depth_16_bit;     // the shadow maps' depth in 16 bits (half the memory and bandwidth; from the next start)
	int moss_shells;              // the moss under water on the rocks drawn in this many shells, near the eye (0: none, its look painted on alone)
	float moss_shell_meters;      // how tall that moss stands
	float moss_shell_distance_meters; // how far from the eye its shells are drawn
	bool water_ice;               // the water freezes in the winter, and holds still under the ice (water_ice.h)
	bool ground_overlay;          // snow and petals settle on the ground and go again (ground_overlay.h; off: none but the snow line's)
	bool material_map;            // the ground shows the material map (material_map.h; off: works its materials out per pixel, as before)
	bool terrain_from_gpu;        // the terrain's heights, normals and surface made on the GPU from the simulation's ground (off: by the CPU from its copy)
	float underwater_visibility_meters; // how far the eye sees under clear water
	float underwater_brightness;        // how bright the water around the eye is
	float foam_speed_meters_per_second;

	// Material transitions
	float soil_blend_meters;      // width of the blend between soil types
	float material_normal_meters; // slope used for materials is measured over this distance
	float grass_slope_start;      // slope (0 flat .. 1 vertical) where grass starts thinning
	float grass_slope_end;        // ...and where it is gone
	float cliff_slope_start;      // where bare rock starts showing on any soil
	float cliff_slope_end;        // ...and where it covers everything

	// Global variation texture over all materials (brightness and hue)
	float variation_tiling_meters;
	float variation_strength;
	float variation_hue_strength;

	// Moving cloud shadows
	float cloud_coverage;         // 0 clear sky .. 1 overcast
	float cloud_softness;
	float cloud_shadow_strength;  // 0 disables cloud shadows
	float cloud_tiling_meters;
	float cloud_height_meters;   // at least CLOUD_MINIMUM_HEIGHT_METERS
	// Volumetric clouds (clouds.h) instead of the flat sheet in the sky:
	// how thick the layer is over cloud_height_meters, how dense, and how
	// many steps the march takes (cost and quality)
	bool volumetric_clouds;
	bool pbr_shading;            // objects and actors lit physically (normal maps, roughness, metal); off: plain diffuse, as before
	float cloud_thickness_meters;
	float cloud_density;
	int32_t cloud_steps;
	float cloud_size;             // the billows' size (1: a 1.4 km noise tile)
	float cloud_towers;           // 0 a flat layer .. 1 towers up to the top of the layer where the cover is thick
	float cloud_base_darkness;    // 0 .. 1: how much a thick layer darkens its bottom
	int32_t cloud_dome_size;      // texels a side of the clouds' dome (0: from the screen's height)
	float cloud_refresh_seconds;  // the dome is marched all over again in this long (when the eye is still)
	float cloud_memory_seconds;   // how long what a dome texel held lingers in it (smooth .. trails behind moving clouds)
	float cloud_density_variation; // 0 .. 1: how much the clouds differ in density (white and puffy .. heavy and dark)
	float cloud_lifetime_minutes; // a cloud forms, lives and dissolves over about this long (0: they stay)
	// Flocks: a thin layer of small white clouds high up (altocumulus,
	// cirrocumulus), all procedural, only where the sky window painted
	// them (cloud_paint.h)
	float cloud_horizon_drop_meters; // the curved planet the clouds go round is let down this far under the eye: clouds a few degrees under the horizon too
	float flock_size_meters;     // how big each one is (100: grains, 1500: big heaps)
	float flock_amount;          // 0 .. 1: how many (a few strays .. packed in rows)
	float flock_trend;           // -1 .. 1: all of them dissolving .. steady .. growing (FLOCK_TREND_SECONDS from none to all at 1)
	float flock_height_meters;   // the bottom of their layer
	float flock_density;         // how opaque they are (1: about a third of a thin big cloud's; less: a veil)
	float flock_shade;           // 0 .. 1: how gray their middles get in their own shadow (0: all white)
	// A kind of clouds (cloud_type_t): choosing one sets the cloud values
	// above (settings_apply_cloud_type); changing any of them makes it custom
	int32_t cloud_type;
	float wind_direction_degrees; // direction the clouds move towards, 0 = +X, 90 = +Z
	float wind_speed_meters_per_second;

	// The sun shadow map is redrawn after terrain changes at most this often
	float shadow_update_interval_seconds;
	// Objects lower than this (meters: stones, bushes, fences) cast no
	// shadow in the far shadow map, only in the near one (0: all of them)
	float far_shadow_least_meters;
	// While actors move near the camera's target, the near shadow map's
	// moving layer (them and loose objects, over a copy of what stands
	// still) is drawn this often so their shadows follow (0: every frame)
	float actor_shadow_update_seconds;
	// Erosion changes the ground a little at a time; the near shadow map is
	// redrawn once it has changed by this much in its square (the far map
	// ignores erosion)
	float shadow_erosion_meters;
	// Terrain level of detail: chunks farther than this are drawn with half
	// the vertices, twice as far with a quarter, and so on; 0 disables it
	float lod_distance_meters;
	// Changed terrain beyond full detail or off screen (erosion far away)
	// goes to the GPU at most this often, seconds (0: at once, always)
	float far_terrain_upload_seconds;
	// The same for the water drawn there (0: every step)
	float far_water_upload_seconds;
	// "Far" for those two: at least this far from the eye (meters), besides
	// beyond full detail; off screen always counts as far
	float far_upload_meters;
	// Fraction of the window resolution the 3D scene is rendered at (then
	// scaled up); below 1 trades sharpness for speed on big screens
	float render_scale;
	// Temporal anti-aliasing (post_temporal): the scene a fraction of a pixel
	// off each frame, each pixel averaged with what it was; how much of the
	// average is the past (0.9: about ten frames), how sharp the past is taken
	// (0 bilinear, softer; 1 Catmull-Rom)
	bool taa;
	// Multisampling (render_target.h): the land, the things on it and the
	// sky drawn with 1 (off), 2, 4 or 8 samples a pixel; at the next start
	int msaa;
	bool taa_when_moving;              // only while the eye goes forward or back (standing, turning, sideways: off; its jitter and its softness gone)
	float taa_history_weight;
	float taa_sharpness;
	// With it on, the buildings' textures read this many levels sharper
	// (negative; the averaging softens them, and steadies the sharper)
	float taa_mip_bias;
	// Day and night (daylight.h): the sun and the sky follow the hour; the
	// sun and sky settings above are noon's
	bool day_cycle;
	float day_length_minutes;     // a whole day, in real minutes (0: the hour stays)
	float daylight_hours;         // of the 24, the sun above the horizon (about noon); an hour of dawn and one of dusk, the rest night
	float moon_cycle_days;        // from new moon to new moon (29.5)
	float golden_hour;            // the low sun's deep gold, the shadows cooler then (0: none, 1: as made)
	float blue_hour;              // the deep blue light after sunset and before sunrise (0: none, 1: as made)
	bool blue_hour_on;            // dusk and dawn with a blue hour (off: the dusky twilight with its warm glow)
	float moon_size_degrees;      // the moon's width on the sky (the real one: about half a degree)
	float aurora_strength;        // how bright the northern lights are, when the game has them (aurora.h)
	// The rain as it is seen (rain.h): how hard (0..1: the drops, the
	// dimming, the rings on the water), how long a drop's streak is, how
	// many splashes; and how bright a rainbow is
	float rain_intensity;
	float rain_streak_meters;
	float rain_splashes;
	float rainbow_strength;
	// The wind in the plants (object.vert): how hard it blows (0 still ..
	// 1 a gale), where from, and how far apart its gusts are
	// (not the clouds' wind)
	float plant_wind_strength;
	float plant_wind_direction_degrees; // where it blows towards: 0 = +X, 90 = +Z
	float plant_wind_gust_meters;
	float grass_gust_sheen;               // how pale a gust's wave makes the grass it bows (0: none)
	// The year in the plants (tools/potop_plant's; season.h): 1 the start
	// of January .. 13 the end of December. Each month shows its pictures
	// (or the latest before it); through a plant's autumn months its
	// foliage turns and falls, a tree and a card at a time
	float month;
	// The winter's snow on the bark and the branches sagging under it: from
	// the start of the first month to the end of the last (round the year)
	float snow_first_month;
	float snow_last_month;
	// The foliage's new growth (its lighter tips): from the start of the
	// first month (growing through it) to the end of the last
	float tips_first_month;
	float tips_last_month;
	// The blooming trees' petals on the ground (cover = petals: objects.ini):
	// falling from the start of the first month, gone at the end of the last
	float bloom_first_month;
	float bloom_last_month;
	// The light into the picture (post.h)
	bool hdr;                     // exposure, bloom and tone mapping; off: the light cut at 1
	bool auto_exposure;           // the exposure follows the picture, as the eye adapts
	float exposure_ev;            // stops up (+) or down (-), on the automatic one or alone
	float night_amount;           // not a setting: 0 by day .. 1 at night (daylight_apply; the fog)
	float exposure_adapt_seconds; // how long the eye takes to adapt (most of the way)
	bool bloom;
	float bloom_strength;         // how much of the blurred picture is mixed in (0..1)
	int32_t tone_mapper;          // 0: AgX, 1: AgX punchy, 2: ACES, 3: Reinhard
	// The color grading (post.h): a look laid over the finished picture
	int32_t color_grade;          // 0: none (as tone mapped), 1: neutral, 2: gloomy
	float color_grade_strength;   // 0..1: how much of it
	// Seeing in the dark (post.h): where the picture is dim, colors fade
	// and turn blue, as the eye's rods take over (the Purkinje shift)
	float night_vision;           // 0: off .. 1: full
	float night_vision_luminance; // the scene's light (linear) under which it starts; all of it a tenth of that
	// Local lights (lights.h): fires, torches, candles
	bool local_lights;
	bool decals;                       // marks on things: bullet holes, scorches, mud (decals.h)
	// The buildings' look (prop_body.glsl): each roof's own lightness, fading
	// and warmth (fractions either way), its weathered patches, runs down the
	// slope, mended stretches; how much old wood dulls where the sky does not
	// reach it
	float roof_lightness;
	float roof_fade;
	float roof_warmth;
	float roof_patches;
	float roof_runs;
	float roof_mended;
	float wood_dulled;
	float local_light_intensity;  // all of them, times this
	bool window_lights;           // the houses' lit windows light what is in front of them (lights.h)
	float window_light_intensity; // each window's light (as a light source's intensity)
	bool lamps_with_the_day;
	// The valley fog (fog.h): how it is drawn (0 off, 1 simple, 2
	// volumetric) and how finely (0..2: the froxels), how thick a full
	// valley's is (per meter), how high it rises at most, how wide a
	// valley is reckoned, how soft its top, how much it glows towards the
	// sun (forward scattering, 0..0.9), when it forms and when it has lifted
	// (hours), how far it is drawn; always: whatever the hour (tests)
	int32_t fog_mode;
	int32_t fog_quality;
	float fog_valley_density;
	float fog_valley_height_meters;
	float fog_valley_reach_meters;
	float fog_softness_meters;
	float fog_glow;
	float fog_shafts;                      // how much the shadows darken the fog (0..1)
	float fog_night_brightness;            // the fog's light at night, of what it would be (the eye sees moonlit fog dimmer than the ground's darkness makes it)
	float fog_rays;                        // the sun's rays through it, from the screen (0: none)
	float fog_form_hour;
	float fog_clear_hour;
	float fog_distance_meters;
	bool fog_always;
	float fireflies;              // on dark nights, 0: none .. 1: many (fireflies.h)      // the lamps are lit at dusk and put out at dawn (else the game, or the console, says)
	// Debugging how the water is drawn: hidden by what stands in front of
	// it (depth test), hiding what is drawn after it (depth write), and its
	// faces turned away from the camera skipped (back face culling)
	bool water_depth_test;
	bool debug_far_shadow_off;     // the far shadow map neither drawn nor used (the Debug tab: where the frame's time goes)
	bool debug_near_shadow_off;    // ... the near one
	bool debug_far_shadow_no_plants; // the far shadow map drawn without the plants (trees, bushes, grass, crops: the Debug tab)
	int32_t debug_water_ripples;   // the water's ripples shown in colors (the Debug tab): 0 off, 1 slope, 2 normal, 3 variance, 4 glint alone
	bool gpu_subgroups;           // the compute shaders' subgroup variants where the device has Vulkan 1.1 (gpu.h; the Debug tab, to compare)
	bool debug_grass_freeze;       // the grass's scatter not run: the last frame's blades drawn again (the Debug tab: what the scatter costs)
	bool water_depth_write;
	bool water_cull_back_faces;
} render_settings_t;

typedef struct
{
	float brush_radius_meters;
	float brush_minimum_radius_meters;
	float brush_maximum_radius_meters;
	float interact_meters;        // on foot: what the walker looks at within this reach glows (if the game can do something with it)
	bool interact_highlight;      // whether it glows at all
	float raise_meters_per_second;
	float smooth_strength_per_second;
	float water_meters_per_second;   // add water / sponge rate at the brush center
	float stamp_height_meters;       // height (or depth) of terrain stamps, changed with - and =
} tool_settings_t;

typedef struct
{
	float gravity;
	// Bed friction: slows flow more where water is shallow
	float friction;
	// Manning roughness of the bed (s/m^(1/3)): friction growing with the
	// square of the speed, which sets the top speed of fast water. 0.03 is
	// a clean natural channel, 0.05 a weedy, stony one; 0 turns it off
	float manning_roughness;
	// Small overall damping so waves eventually settle
	float linear_damping;
	// A tile goes to sleep when the water in it moves slower than this
	// everywhere (and its level hardly changes) for about a second
	float sleep_speed_meters_per_second;
	// ...and no cell's water level changes faster than this
	float sleep_change_millimeters_per_second;
	// Water flowing into a sleeping tile wakes it only if a border cell gets
	// at least this much in one substep; smaller trickles are held back (the
	// level difference grows until they get through)
	float border_wake_millimeters;
	// A tile goes to sleep after this many substeps in a row of still water
	int32_t sleep_after_substeps;
	// A sleeping tile is only woken by water pouring into one of its cells
	// at least this fast, or by a neighbor's level dropping this far below
	// its own (a dam breaking); smaller inflow is thrown away and the tile
	// sleeps on
	float asleep_wake_millimeters_per_second;
	float asleep_level_wake_millimeters;
	// The whole water simulation (and erosion, springs, soaking) stops
	// after this many seconds without the player touching terrain or
	// water, or with F7; the water is still drawn flowing and the physics
	// keeps running. Tools, rain and F7 start it again. 0: never by itself.
	float freeze_after_seconds;
	// Water thinner than this sticks to the ground as a wet film: it does not
	// flow and does not wake neighbors, and it slowly evaporates
	float film_millimeters;
	float film_evaporation_millimeters_per_second;
	// Fake surface tension: water only spills onto dry ground when it stands
	// this much above it, so full ponds stop draining over their lip
	float meniscus_millimeters;
	// The lip: deep water (more than thin_water_millimeters) only spills
	// into thin water or onto dry ground when it stands this much above
	// it, so ponds stop draining through endless trickles (0: off)
	float lip_millimeters;
	bool drain_edges;
	// true: pipes sized by the water depth up to pipe_depth_limit_meters
	// (thin water flows slowly, deeper water behaves as with classic pipes);
	// false: the classic fixed pipes everywhere. M toggles.
	bool depth_scaled_pipes;
	float pipe_depth_limit_meters;
	int32_t maximum_substeps;
	// Water faster than this (deep water, m/s) asks for no more substeps:
	// it is held back by the cell-per-substep limit instead (a waterfall off
	// a high cliff would otherwise run every active tile 8 times a step)
	float substep_speed_meters_per_second;
	// Thin water (a game, not a hydrology tool): trickles and sheets of
	// water up to this deep drain away fast instead of crawling for half
	// an hour. 0 turns all of the thin water rules below off.
	float thin_water_millimeters;
	// Thin water flows as if it were that deep: no extra friction for
	// being shallow, full size pipes
	bool thin_water_superfluid;
	// Unused since the ground's moisture stopped taking water (step 7b2)
	float thin_water_infiltration_factor;
	// Mud in water slower than this settles out quickly, at this fraction
	// of what is left per second, instead of keeping the tile busy
	float still_water_speed_meters_per_second;
	float still_water_deposition_per_second;
	// Water shallower than this erodes less (nothing below half of it), so
	// trickles do not dig (0: everything erodes)
	float erosion_minimum_depth_millimeters;
	// Thin water left behind (no deeper neighbor) evaporates this fast,
	// except while it rains
	float thin_water_evaporation_millimeters_per_second;
	// Static friction: water that just came in through a pipe does not
	// go back through it for a level difference below this, so lakes stop
	// rocking (0: off). Flow in one direction is never held.
	float still_level_millimeters;
	// Slow water shallower than this is braked harder towards the shore,
	// up to shore_damping_per_second at the waterline, so waves die on the
	// beach instead of bouncing back (0: off)
	float shore_damping_depth_meters;
	float shore_damping_per_second;
	float rain_millimeters_per_second;
	float spring_cubic_meters_per_second;
} water_settings_t;

typedef struct
{
	int32_t steps_per_second;
	int32_t maximum_steps_per_frame;
	// Simulation stops for the frame once it has used this much time, and
	// the game runs slower instead of falling ever further behind
	float maximum_milliseconds_per_frame;
	// While the window is in the background the game draws at most
	// background_frames_per_second, and the simulation pauses if this is set
	bool pause_in_background;
	// Threads for the simulation; 0 = all the CPU offers. Fewer leaves room
	// for the graphics driver and other programs.
	int32_t threads;
	float background_frames_per_second;
	// Every this many simulated seconds, a line about what keeps the water
	// awake and what the simulation costs is logged (0 = never)
	float diagnostics_interval_seconds;
	// A path search (actors) gives up after an eighth of this many of the
	// navigation mesh's rectangles (it was so many grid cells of 1 m once)
	// and takes the reached one nearest the goal
	int32_t path_search_cells;
	// Objects smaller than this across are left out of the navigation:
	// actors walk through them (pebbles). Bigger ones are in the way or
	// stepped on, as each actor can (a bear steps on a stone a rabbit goes
	// around)
	float navigation_object_meters;
	// An object on its own whose footprint's longest reach (in the height
	// those that walk take) is at most this is a small thing: not in the
	// map of where they can go, gone round on the way (a trunk, a boulder);
	// a longer one is a wall, as a module's parts always are. A small body
	// lighter than navigation_soft_kilograms is soft (a bucket): no surface
	// to step on, gone round if that is at most small_object_detour_meters
	// longer, else walked through. objects.ini: navigation = wall|round|soft
	float navigation_wall_meters;
	float navigation_soft_kilograms;
	// Small things gone round are left out of the navigation's rectangles
	// (a forest: a few rectangles, not one per tree); the ways found are
	// led round them (navigation.c: mesh_lead_round). Off: they are in
	// the rectangles, as walls are
	bool navigation_small_out;
	// A walker on its way goes by the navigation's rectangles alone (where
	// it may step, its body's room, the ground's height), not by the
	// quarter meters they were made from. Off: as before, by those, with
	// fine ways over them round what is in its way
	bool navigation_follow_mesh;
	// A region's cells' surfaces (what its objects' tops are, quarter meter
	// by quarter meter: what the rectangles are built from) are let go
	// once its rectangles are built, and built again when they are to be
	// (a wall there new or gone; not for small things). With
	// navigation_follow_mesh only. Off: kept, as before
	bool navigation_shed_cells;
	// Ways are led round small things only this near the player; further
	// off they go straight through them (no one sees it)
	float navigation_detail_meters;
	float small_object_detour_meters;
	// What walking on a slope costs (paths go round hills): a meter of
	// ground path_slope_degrees steep or steeper costs 1 + path_slope_cost
	// meters of flat ground, gentler ones less, whichever way the path goes
	float path_slope_degrees;
	float path_slope_cost;
} simulation_settings_t;

typedef struct
{
	// Populate every new map with vegetation (data/objects.ini says what
	// grows where)
	bool populate;
	// Multiplies every type's number per hectare (and the brush's)
	float density_scale;
	// Multiplies every type's draw distance: lower is faster
	float draw_distance_scale;
	// Far away, the bigger models are drawn as pictures of themselves
	// (impostors.h): much faster, the same at that size
	bool impostors;
	// An object becomes its impostor once its bounding sphere is smaller
	// than this on screen (pixels across, 64; the pictures are
	// IMPOSTOR_CELL_PIXELS: above that, a picture is seen enlarged)
	float impostor_pixels;
	// Levels of detail (the trees tools/potop_plant makes): the smaller a
	// tree is on screen, the fewer of its branches are drawn, the finest
	// first, then the next; lod_distance_scale moves where (higher: farther
	// away, finer); lod_foliage_kept: the share of its foliage drawn at the
	// coarsest level (halfway there at the one before; 1: all of it)
	bool lod;
	float lod_distance_scale;
	float lod_foliage_kept;
	// The props' (tools/potop_prop: rocks, walls) levels: each drawn once
	// how far off it is from the full one is less than this on screen
	// (pixels; lower: finer, farther away)
	float prop_lod_pixels;
	// Grass blades made on the GPU (grass.h): out to grass_distance_meters
	// from the eye; grass_density blades a square meter near the eye
	// (farther they thin out, wider); their height and width (meters, give
	// or take); the ground's grass color times grass_tint; the share of
	// dry, yellowed blades
	bool grass;
	float grass_distance_meters;
	float grass_density;
	float grass_height_meters;
	float grass_width_meters;
	float grass_tint[3];
	float grass_dry_share;
	// Past grass_coarse_meters (0: nowhere) a blade is one triangle,
	// grass_coarse_width times as wide (cheaper: its segments are not seen)
	float grass_coarse_meters;
	float grass_coarse_width;
	// The particle effects buildings carry (a forge's fire, a chimney's
	// smoke: object_effects.h) play within this many meters of the eye (0:
	// nowhere)
	float effects_meters;
	// The planting brush fills its circle in about this many seconds
	float brush_fill_seconds;
} objects_settings_t;

typedef struct
{
	// Objects take part in the physics (bodies, floating, floods)
	bool enabled;
	// Threads of the physics engine (it has its own; the water uses the rest)
	int32_t threads;
	int32_t substeps;              // per simulation step; more is steadier
	// Water pushing on bodies: drag = 1/2 water density * this * area * speed^2
	float drag_coefficient;
	// A hit's impulse is spread over this long to give the force that tears
	// anchored objects loose or damages them
	float hit_seconds;
	// Water flowing at least this fast wakes the bodies in it
	float wake_flow_meters_per_second;
	// The physics' copy of the ground follows erosion only where it changed
	// more than this, checked every terrain_check_seconds
	float terrain_resync_meters;
	// Loose bodies smaller than this (their largest side, meters) are pushed
	// by the water at their middle only, bigger ones at many points (they
	// turn, roll, tip in the current)
	float small_body_meters;
	float terrain_check_seconds;
	// Grass and such react to the water this often
	float flood_check_seconds;
	// Floating bodies: up and down motion in the water is braked this much
	// per second (fully submerged), so they stop bobbing
	float float_damping_per_second;
	// Water shallower than this does not push bodies at all
	float water_minimum_meters;
} physics_settings_t;

typedef struct
{
	terrain_settings_t terrain;
	cache_settings_t cache;
	camera_settings_t camera;
	render_settings_t render;
	tool_settings_t tools;
	water_settings_t water;
	simulation_settings_t simulation;
	objects_settings_t objects;
	physics_settings_t physics;
} settings_t;

typedef enum
{
	SETTING_FLOAT,
	SETTING_INT,
	SETTING_UINT,
	SETTING_BOOL,
	SETTING_COLOR
} setting_type_t;

// Sets the cloud values of render to those of its cloud_type (nothing for
// CLOUD_TYPE_CUSTOM)
void settings_apply_cloud_type(render_settings_t *render);

// True if a render setting (its ini key) is one of those a cloud type sets
bool settings_is_cloud_type_key(const char *key);

// Fills in the compiled-in defaults.
void settings_set_defaults(settings_t *settings);

// Applies data/settings.ini on top of whatever is in *settings.
void settings_load(settings_t *settings);

// The setting stored under section/key: a pointer to it inside settings
// and its type. NULL if there is no such key.
void *settings_find(settings_t *settings, const char *section, const char *key, setting_type_t *type_out);

// The table of keys, for listing them (the console): false past the end
size_t settings_field_count(void);
bool settings_field_name(size_t index, const char **section_out, const char **key_out);

// A value as the ini file writes it / from text as the ini file reads it
// (a cloud type brings its values, a changed cloud value makes the type
// custom, as in the settings window). False for an unknown key or a value
// that does not read.
bool settings_get_text(const settings_t *settings, const char *section, const char *key, char *text_out, size_t text_out_size);
bool settings_set_text(settings_t *settings, const char *section, const char *key, const char *value);

// Settings changed in the game are kept apart from data/settings.ini, in
// settings_changed.ini in the preference directory: only the keys whose
// value differs from file_settings (the values from settings.ini) are
// written. Loading applies them on top of whatever is in *settings.
void settings_load_overrides(settings_t *settings);
// Another file for them (the editor's: editor_settings_changed.ini, so
// what it changes, the fog off, stays out of the game), before loading
void settings_set_override_file(const char *file_name);
// A file of the data directory read on top of *settings (the editor's
// own layer over settings.ini: editor_settings.ini); false if missing
bool settings_load_data_file(settings_t *settings, const char *relative_path);
// One setting set from "section.key=value" (the command line's --set);
// false if it is not one
bool settings_set_from_text(settings_t *settings, const char *assignment);
bool settings_save_overrides(const settings_t *settings, const settings_t *file_settings);

#endif
