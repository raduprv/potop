// The settings window (settings_window.h)

#include "settings_window.h"

#include <SDL3/SDL.h>

#include "ui_widgets.h"

#define SETTINGS_WINDOW "Settings"

typedef enum
{
	TAB_SKY = 0,
	TAB_CLOUDS,
	TAB_GROUND,
	TAB_WATER,
	TAB_OBJECTS,
	TAB_RENDER,
	TAB_TERRAIN,
	TAB_CAMERA,
	TAB_INTERFACE,
	TAB_DEBUG,
	TAB_COUNT
} settings_tab_t;

static const char *tab_names[TAB_COUNT] = { "Sky & light", "Clouds", "Ground", "Water", "Objects", "Graphics", "New map", "Camera", "Interface",
	"Debug" };

typedef enum
{
	EFFECT_LIVE = 0,               // used as it changes
	EFFECT_RENDER,                 // rebuilds textures or shadow maps: Apply
	EFFECT_OBJECTS,                // grows the vegetation anew: Apply
	EFFECT_GENERATE                // used by the next new map
} setting_effect_t;

typedef struct
{
	settings_tab_t tab;
	const char *section;           // NULL: a heading
	const char *key;
	const char *label;
	float minimum;
	float maximum;
	float step;
	setting_effect_t effect;
	const char *format;            // for floats; NULL: "%.2f"
	const char *choices;           // integers chosen from a list: "512|1024|2048", or named: "0:Off|1:On"
	const char *tooltip;
	bool editor_only;              // not in a game's settings (only the editor's)
} ui_setting_t;

#define SETTINGS_TABS_PER_ROW 5

#define HEADING(tab, text) { tab, NULL, NULL, text, 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL, false }

static const ui_setting_t ui_settings[] = {
	HEADING(TAB_SKY, "Sun and sky"),
	{ TAB_SKY, "render", "sun_azimuth_degrees", "Sun direction", 0, 360, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, "0 = from +X, 90 = from +Z (with the day cycle: at noon)", false },
	{ TAB_SKY, "render", "sun_elevation_degrees", "Sun height", 5, 85, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL, false },
	{ TAB_SKY, "render", "sun_intensity", "Sunlight", 0, 3, 0.05f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_SKY, "render", "sun_color", "Sun color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_SKY, "render", "ambient_intensity", "Sky light", 0, 1.5f, 0.01f, EFFECT_LIVE, NULL, NULL, "Light from the sky in the shadows", false },
	{ TAB_SKY, "render", "sky_color", "Sky color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "At the horizon; distant land fades into it", false },
	{ TAB_SKY, "render", "sky_zenith_color", "Sky overhead", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "The sky straight up", false },
	{ TAB_SKY, "render", "fog_density", "Haze", 0, 0.003f, 0.00005f, EFFECT_LIVE, "%.5f", NULL, "Fog density per meter, at the ground", false },
	{ TAB_SKY, "render", "fog_height_meters", "Haze height", 100, 5000, 50, EFFECT_LIVE, "%.0f m", NULL,
		"The haze thins upwards: this much higher it is a third as thick (low: clear mountain tops and clouds)", false },
	HEADING(TAB_SKY, "Day and night"),
	{ TAB_SKY, "render", "day_cycle", "Day cycle", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The sun crosses the sky, evening, night with the moon and the stars (off: the sun stays where it is set). The sun settings are noon's", false },
	{ TAB_SKY, "render", "daylight_hours", "Daylight", 2, 21, 0.5f, EFFECT_LIVE, "%.1f h", NULL,
		"Hours of the 24 with the sun up, about noon (14: a summer's day); an hour of dawn, one of dusk, the rest night", false },
	{ TAB_SKY, "render", "day_length_minutes", "Day length", 0, 720, 5, EFFECT_LIVE, "%.0f min", NULL,
		"A whole day in real minutes (0: the hour stays; the console's hour sets it, freeze stops it)", false },
	{ TAB_SKY, "render", "golden_hour", "Golden hour", 0, 2, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The low sun's deep gold (the last and first degrees above the horizon), the shadows cooler against it", false },
	{ TAB_SKY, "render", "blue_hour_on", "Blue hour", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Dusk and dawn with a blue hour: all the sky deep blue, its light stronger (off: the dusky twilight with its warm glow)", false },
	{ TAB_SKY, "render", "blue_hour", "Blue hour strength", 0, 2, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The deep blue light while the sun is a few degrees under the horizon, before the night", false },
	{ TAB_SKY, "render", "moon_cycle_days", "Moon cycle", 2, 60, 0.5f, EFFECT_LIVE, "%.1f days", NULL,
		"From new moon to new moon, in days of the game (29.5: as the real one; the console's moon sets its age)", false },
	{ TAB_SKY, "render", "moon_size_degrees", "Moon size", 0, 4.5f, 0.05f, EFFECT_LIVE, "%.2f\xc2\xb0", NULL,
		"How wide the moon is on the sky (0: none; the real one is half a degree, 1.5 the default)", false },
	{ TAB_SKY, "render", "aurora_strength", "Aurora", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright the northern lights are when there are some (World window, the console's aurora)", false },
	HEADING(TAB_SKY, "Rain"),
	{ TAB_SKY, "render", "rain_intensity", "Rain", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How hard it rains when it rains (World window, R, the console's rain): the drops, the dimmer light, the rings on the water", false },
	{ TAB_SKY, "render", "rain_streak_meters", "Drop streaks", 0.05f, 1.5f, 0.05f, EFFECT_LIVE, "%.2f m", NULL,
		"How long a falling drop looks", false },
	{ TAB_SKY, "render", "rain_splashes", "Splashes", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How many splashes where the drops land (0: none)", false },
	{ TAB_SKY, "render", "rainbow_strength", "Rainbow", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright a rainbow is (World window, the console's rainbow); it shows only with the sun low, opposite it", false },
	HEADING(TAB_SKY, "Valley fog"),
	{ TAB_SKY, "render", "fog_mode", "Valley fog", 0, 0, 0, EFFECT_LIVE, NULL, "0:Off|1:Simple|2:Volumetric",
		"Low fog in the valleys and over the water, forming in the evening, lifting in the morning. Volumetric: lit through "
		"the shadow maps (shafts between the trees); simple: cheaper, no shadows", false },
	{ TAB_SKY, "render", "fog_quality", "Fog detail", 0, 0, 0, EFFECT_LIVE, NULL, "0:Low|1:Medium|2:High",
		"The volumetric fog's cells (froxels): 96 x 54 x 64, 160 x 90 x 96, 240 x 135 x 128", false },
	{ TAB_SKY, "render", "fog_valley_density", "Fog thickness", 0, 0.5f, 0.005f, EFFECT_LIVE, "%.3f", NULL,
		"How thick a full valley's fog is (per meter)", false },
	{ TAB_SKY, "render", "fog_valley_height_meters", "Fog height", 2, 100, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How high it rises in the deepest valleys (and two thirds of that over water)", false },
	{ TAB_SKY, "render", "fog_valley_reach_meters", "Valley width", 20, 1000, 10, EFFECT_LIVE, "%.0f m", NULL,
		"How far around a place is looked at to tell whether it lies in a valley", false },
	{ TAB_SKY, "render", "fog_softness_meters", "Fog top", 0.5f, 30, 0.5f, EFFECT_LIVE, "%.1f m", NULL, "How soft its top is", false },
	{ TAB_SKY, "render", "fog_glow", "Fog glow", 0, 0.9f, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How much brighter it is looking towards the sun (0: alike every way)", false },
	{ TAB_SKY, "render", "fog_shafts", "Fog shafts", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How much the shadows darken the fog: the sun's shafts through the trees (0: none)", false },
	{ TAB_SKY, "render", "fog_night_brightness", "Fog at night", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright the fog is at night, of its full light (lit evenly all round, it outshines the dark ground)", false },
	{ TAB_SKY, "render", "fog_rays", "Fog sun rays", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The soft rays round the sun, through the fog, between what stands in front of it (0: none)", false },
	{ TAB_SKY, "render", "fog_form_hour", "Fog forms", 0, 24, 0.25f, EFFECT_LIVE, "%.2f h", NULL,
		"The hour it begins to form (thick four hours later)", false },
	{ TAB_SKY, "render", "fog_clear_hour", "Fog lifted", 0, 24, 0.25f, EFFECT_LIVE, "%.2f h", NULL,
		"The hour it has gone (lifting over the two hours before)", false },
	{ TAB_SKY, "render", "fog_distance_meters", "Fog distance", 50, 2000, 10, EFFECT_LIVE, "%.0f m", NULL, "How far out it is drawn", false },
	{ TAB_SKY, "render", "fog_always", "Fog always", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Whatever the hour (to look at it)", false },
	HEADING(TAB_SKY, "Light (HDR)"),
	{ TAB_SKY, "render", "hdr", "HDR", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Exposure, bloom and tone mapping: light brighter than white keeps its shape (off: cut at white, as before)", false },
	{ TAB_SKY, "render", "tone_mapper", "Tone mapping", 0, 0, 0, EFFECT_LIVE, NULL, "0:AgX|1:AgX punchy|2:ACES|3:Reinhard",
		"AgX: bright colors go towards white (punchy: more contrast and color); ACES: saturated, filmic; Reinhard: the plainest", false },
	{ TAB_SKY, "render", "auto_exposure", "Auto exposure", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The exposure follows the picture, as the eye adapts (off: Exposure alone)", false },
	{ TAB_SKY, "render", "exposure_ev", "Exposure", -4, 4, 0.1f, EFFECT_LIVE, "%+.1f EV", NULL,
		"Stops brighter (+) or darker (-)", false },
	{ TAB_SKY, "render", "exposure_adapt_seconds", "Adaptation", 0, 10, 0.1f, EFFECT_LIVE, "%.1f s", NULL,
		"How long the eye takes to get used to brighter or darker (0: at once)", false },
	{ TAB_SKY, "render", "bloom", "Bloom", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Bright light glows", false },
	{ TAB_SKY, "render", "bloom_strength", "Bloom strength", 0, 0.3f, 0.005f, EFFECT_LIVE, "%.3f", NULL, NULL, false },
	{ TAB_SKY, "render", "night_vision", "Night vision", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"Where the picture is dim, colors fade and turn blue, as the eye sees in the dark (0: off)", false },
	{ TAB_SKY, "render", "night_vision_luminance", "Night vision under", 0.002f, 0.5f, 0.002f, EFFECT_LIVE, "%.3f", NULL,
		"How dim (the scene's light) before colors start to fade; all gone at a tenth of it", false },
	HEADING(TAB_SKY, "Local lights"),
	{ TAB_SKY, "render", "local_lights", "Fires and torches", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Fires, torches and candles light what is around them (particles.ini and objects.ini: light)", false },
	{ TAB_SKY, "render", "local_light_intensity", "Brightness", 0, 4, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"All the local lights, times this", false },
	{ TAB_SKY, "render", "window_lights", "Window light", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The houses' lit windows light the ground and the walls in front of them", false },
	{ TAB_SKY, "render", "window_light_intensity", "Window brightness", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright each lit window's light is", false },
	{ TAB_SKY, "render", "lamps_with_the_day", "Lamps with the day", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Lanterns and candles are lit at dusk and put out at dawn, each at its own moment (off: the console's lights on/off)", false },
	HEADING(TAB_CLOUDS, "Clouds"),
	{ TAB_CLOUDS, "render", "cloud_type", "Type", 0, 0, 0, EFFECT_LIVE, NULL, "1:Small|2:Medium|3:Storm|4:Overcast|0:Custom",
		"Sets the values below; changing any of them makes the clouds custom", false },
	{ TAB_CLOUDS, "render", "cloud_coverage", "Coverage", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "0 clear sky, 1 overcast", false },
	{ TAB_CLOUDS, "render", "cloud_tiling_meters", "Spread", 300, 8000, 50, EFFECT_LIVE, "%.0f m", NULL, "How broad the clouds are and how far apart", false },
	{ TAB_CLOUDS, "render", "cloud_height_meters", "Height", CLOUD_MINIMUM_HEIGHT_METERS, 3000, 10, EFFECT_LIVE, "%.0f m", NULL, "Bottom of the clouds", false },
	{ TAB_CLOUDS, "render", "cloud_thickness_meters", "Thickness", 50, 2000, 10, EFFECT_LIVE, "%.0f m", NULL, "Volumetric clouds only", false },
	{ TAB_CLOUDS, "render", "cloud_density", "Density", 0.1f, 5, 0.05f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds only", false },
	{ TAB_CLOUDS, "render", "cloud_size", "Size", 0.5f, 4, 0.05f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds: size of the billows", false },
	{ TAB_CLOUDS, "render", "cloud_towers", "Towers", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds: how tall the thick parts grow", false },
	{ TAB_CLOUDS, "render", "cloud_base_darkness", "Dark bases", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds: how dark the bottoms of thick clouds get", false },
	{ TAB_CLOUDS, "render", "cloud_softness", "Softness", 0.01f, 0.5f, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_CLOUDS, "render", "cloud_shadow_strength", "Shadows", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	HEADING(TAB_CLOUDS, "Life and wind"),
	{ TAB_CLOUDS, "render", "cloud_density_variation", "Variety", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"Volumetric clouds: how much they differ, white and puffy to heavy and dark", false },
	{ TAB_CLOUDS, "render", "cloud_lifetime_minutes", "Lifetime", 0, 60, 0.5f, EFFECT_LIVE, "%.1f min", NULL,
		"Volumetric clouds: each forms, lives and dissolves over about this long (0: they stay)", false },
	{ TAB_CLOUDS, "render", "wind_direction_degrees", "Wind direction", 0, 360, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL, false },
	{ TAB_CLOUDS, "render", "wind_speed_meters_per_second", "Wind speed", 0, 30, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, NULL, false },
	{ TAB_CLOUDS, "render", "cloud_horizon_drop_meters", "Horizon drop", 0, CLOUD_MAXIMUM_HORIZON_DROP_METERS, 50, EFFECT_LIVE, "%.0f m", NULL,
		"The planet the clouds go round, let down under you: clouds a few degrees under the horizon too (0: they stop at its line)", false },
	HEADING(TAB_CLOUDS, "Quality"),
	{ TAB_CLOUDS, "render", "volumetric_clouds", "Volumetric", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Real 3D clouds (off: a flat sheet, for weak devices)", false },
	{ TAB_CLOUDS, "render", "cloud_steps", "Quality", CLOUD_MINIMUM_STEPS, 192, 8, EFFECT_LIVE, "%.0f steps", NULL, "Volumetric clouds: steps along each ray (cost)", false },
	{ TAB_CLOUDS, "render", "cloud_dome_size", "Sharpness", 0, 0, 0, EFFECT_LIVE, NULL, "0:Auto|1024:1024|1536:1536|2048:2048|3072:3072",
		"Volumetric clouds: texels a side of the sky they are drawn into (memory and cost; Auto: from the screen)", false },
	{ TAB_CLOUDS, "render", "cloud_refresh_seconds", "Refresh", 0.02f, 1, 0.01f, EFFECT_LIVE, "%.2f s", NULL,
		"Volumetric clouds: the sky is drawn all over again in this long (longer: cheaper, moving clouds step)", false },
	{ TAB_CLOUDS, "render", "cloud_memory_seconds", "Smoothing", 0.02f, 2, 0.01f, EFFECT_LIVE, "%.2f s", NULL,
		"Volumetric clouds: how long the old sky lingers (longer: smoother, trails behind moving clouds)", false },
	HEADING(TAB_CLOUDS, "Flocks: small clouds high up (painted in Sky)"),
	{ TAB_CLOUDS, "render", "flock_size_meters", "Size", 100, 1500, 10, EFFECT_LIVE, "%.0f m", NULL,
		"How big each one is: grains (100 m) to big heaps (1500 m)", false },
	{ TAB_CLOUDS, "render", "flock_amount", "Amount", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"How many: a few strays to packed in rows", false },
	{ TAB_CLOUDS, "render", "flock_trend", "Trend", -1, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"All of them at once: dissolving (-1), steady (0), growing (1); at 1, from none to all in five minutes of the sky's clock", false },
	{ TAB_CLOUDS, "render", "flock_height_meters", "Height", 2000, 10000, 50, EFFECT_LIVE, "%.0f m", NULL,
		"The bottom of their layer (altocumulus 2-6 km, cirrocumulus higher)", false },
	{ TAB_CLOUDS, "render", "flock_density", "Density", 0.05f, 2, 0.01f, EFFECT_LIVE, NULL, NULL,
		"How opaque they are: a veil (low) to solid little heaps", false },
	{ TAB_CLOUDS, "render", "flock_shade", "Shade", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"How gray their middles get in their own shadow (0: all white)", false },
	HEADING(TAB_GROUND, "Look"),
	{ TAB_GROUND, "render", "normal_strength", "Relief", 0, 5, 0.1f, EFFECT_RENDER, NULL, NULL, "Depth of the texture relief", false },
	{ TAB_GROUND, "render", "snow_height_fraction", "Snow line", 0, 1.2f, 0.01f, EFFECT_LIVE, NULL, NULL, "Fraction of the highest point", false },
	{ TAB_GROUND, "render", "variation_strength", "Variation", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_GROUND, "render", "variation_hue_strength", "Hue variation", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_GROUND, "render", "detail_fade_start_meters", "Detail from", 10, 1000, 5, EFFECT_LIVE, "%.0f m", NULL, NULL, false },
	{ TAB_GROUND, "render", "detail_fade_end_meters", "Detail to", 20, 2000, 10, EFFECT_LIVE, "%.0f m", NULL, NULL, false },
	HEADING(TAB_GROUND, "Textures"),
	{ TAB_GROUND, "render", "texture_size", "Texture size", 0, 0, 0, EFFECT_RENDER, NULL, "256|512|1024|2048", NULL, false },
	{ TAB_GROUND, "render", "anisotropy", "Anisotropy", 1, 16, 1, EFFECT_LIVE, "%.0f", NULL, "Sharpness of textures seen at a slant", false },
	HEADING(TAB_GROUND, "Material map"),
	{ TAB_GROUND, "render", "material_map", "Material map", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The ground shows the material map: the materials worked out once, the covers under the trees, the snow and the petals (off: worked out per pixel, as before)", false },
	{ TAB_GROUND, "render", "ground_overlay", "Snow and petals", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Snow settles where it snows and melts after the winter, petals fall under blooming trees (with the material map; off: only the snow line's)", false },
	HEADING(TAB_WATER, "Look"),
	{ TAB_WATER, "render", "water_shallow_color", "Shallow color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_WATER, "render", "water_deep_color", "Deep color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_WATER, "render", "water_clarity_meters", "Clarity", 0.1f, 20, 0.1f, EFFECT_LIVE, "%.1f m", NULL, NULL, false },
	{ TAB_WATER, "render", "water_opacity_per_meter", "Opacity", 0, 5, 0.05f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_WATER, "render", "water_reflections", "Reflections", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The water mirrors what is on screen (the land, the trees) and the volumetric clouds (off: the sky alone)", false },
	{ TAB_WATER, "render", "water_reflections_half_size", "Reflections at half size", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The reflections found for a quarter of the pixels and spread over the rest (faster, a little softer; off: every pixel)", false },
	{ TAB_WATER, "render", "terrain_from_gpu", "Terrain from the GPU", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The ground's heights, normals and surface made on the GPU from the simulation (off: by the CPU from its copy, slower; to compare)", false },
	{ TAB_WATER, "render", "underwater_visibility_meters", "Underwater sight", 5, 300, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How far you see with the camera under clear water (mud cuts it down)", false },
	{ TAB_WATER, "render", "underwater_brightness", "Underwater light", 0.2f, 5, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright the water around the camera is", false },
	{ TAB_WATER, "render", "foam_speed_meters_per_second", "Foam from", 0, 10, 0.1f, EFFECT_LIVE, "%.1f m/s", NULL, "Flow speed where foam starts", false },
	HEADING(TAB_WATER, "Water on things"),
	{ TAB_WATER, "render", "water_ice", "Ice", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The water freezes through the winter, from the shore out (not where it runs fast), and holds still under the ice; a flood breaks it", false },
	{ TAB_WATER, "render", "moss_shells", "Moss shells", 0, 16, 1, EFFECT_LIVE, NULL, NULL,
		"The moss under water on the rocks stands up in this many layers near the camera (0: painted on alone)", false },
	{ TAB_WATER, "render", "moss_shell_meters", "Moss height", 0.005f, 0.05f, 0.001f, EFFECT_LIVE, "%.3f m", NULL, NULL, false },
	{ TAB_WATER, "render", "moss_shell_distance_meters", "Moss shell distance", 0, 40, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How far from the camera the moss's layers are drawn", false },
	HEADING(TAB_WATER, "Sources"),
	{ TAB_WATER, "water", "rain_millimeters_per_second", "Rain", 0, 5, 0.05f, EFFECT_LIVE, "%.2f mm/s", NULL, NULL, false },
	{ TAB_WATER, "water", "spring_cubic_meters_per_second", "Spring flow", 0, 20, 0.1f, EFFECT_LIVE, "%.1f m3/s", NULL, NULL, false },
	{ TAB_WATER, "tools", "water_meters_per_second", "Brush rate", 0.1f, 10, 0.1f, EFFECT_LIVE, "%.1f m/s", NULL, NULL, false },
	HEADING(TAB_WATER, "Flow"),
	{ TAB_WATER, "water", "friction", "Bed friction", 0, 0.2f, 0.001f, EFFECT_LIVE, "%.3f", NULL, "Slows shallow water most", false },
	{ TAB_WATER, "water", "manning_roughness", "Roughness", 0, 0.1f, 0.001f, EFFECT_LIVE, "%.3f", NULL, "Manning: sets the top speed of fast water", false },
	{ TAB_WATER, "water", "linear_damping", "Damping", 0, 0.5f, 0.005f, EFFECT_LIVE, "%.3f", NULL, NULL, false },
	{ TAB_WATER, "water", "maximum_substeps", "Most substeps", 1, 32, 1, EFFECT_LIVE, NULL, NULL, "For fast water; more is steadier and slower", false },
	{ TAB_WATER, "water", "substep_speed_meters_per_second", "Substeps up to", 2, 40, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL,
		"Faster water adds no more substeps; it is held back instead (cheaper, waterfalls a little slower)", false },
	HEADING(TAB_WATER, "Thin water"),
	{ TAB_WATER, "water", "film_millimeters", "Wet film", 0, 5, 0.05f, EFFECT_LIVE, "%.2f mm", NULL, "Thinner water clings to the ground", false },
	{ TAB_WATER, "water", "film_evaporation_millimeters_per_second", "Drying", 0, 0.5f, 0.005f, EFFECT_LIVE, "%.3f mm/s", NULL, NULL, false },
	{ TAB_WATER, "water", "meniscus_millimeters", "Surface tension", 0, 10, 0.1f, EFFECT_LIVE, "%.1f mm", NULL, NULL, false },
	{ TAB_WATER, "water", "lip_millimeters", "Lip", 0, 100, 1, EFFECT_LIVE, "%.0f mm", NULL,
		"Deep water spills into thin water only this much above it; stops ponds draining through trickles", false },
	{ TAB_WATER, "water", "thin_water_millimeters", "Thin water", 0, 100, 1, EFFECT_LIVE, "%.0f mm", NULL, "Up to this deep it drains fast; 0: off", false },
	{ TAB_WATER, "water", "thin_water_superfluid", "Superfluid", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Thin water flows without the shallow friction", false },
	{ TAB_WATER, "water", "still_water_speed_meters_per_second", "Mud settles below", 0, 0.5f, 0.01f, EFFECT_LIVE, "%.2f m/s", NULL, NULL, false },
	{ TAB_WATER, "water", "thin_water_evaporation_millimeters_per_second", "Thin drying", 0, 5, 0.05f, EFFECT_LIVE, "%.2f mm/s", NULL,
		"Thin water left behind dries this fast (not while it rains)", false },
	{ TAB_WATER, "water", "still_level_millimeters", "Still below", 0, 20, 0.5f, EFFECT_LIVE, "%.1f mm", NULL,
		"Water does not turn back for a smaller level difference (stops rocking)", false },
	{ TAB_WATER, "water", "shore_damping_depth_meters", "Shore depth", 0, 1, 0.01f, EFFECT_LIVE, "%.2f m", NULL, "Slow water shallower than this is braked", false },
	{ TAB_WATER, "water", "shore_damping_per_second", "Shore brake", 0, 10, 0.1f, EFFECT_LIVE, "%.1f /s", NULL, "So waves die on the shore", false },
	{ TAB_WATER, "water", "erosion_minimum_depth_millimeters", "Erodes from", 0, 100, 1, EFFECT_LIVE, "%.0f mm", NULL,
		"Shallower water digs less, nothing below half of this", false },
	{ TAB_WATER, "water", "still_water_deposition_per_second", "Mud settling", 0, 10, 0.1f, EFFECT_LIVE, "%.1f /s", NULL, "Fraction of the mud dropped per second in still water", false },
	HEADING(TAB_WATER, "Freeze"),
	{ TAB_WATER, "water", "freeze_after_seconds", "Freezes after", 0, 600, 5, EFFECT_LIVE, "%.0f s", NULL,
		"The water stops this long after you last touched terrain or water (F7 too); drawn flowing, not simulated (0: only F7)", false },
	HEADING(TAB_WATER, "Simulation"),
	{ TAB_WATER, "simulation", "maximum_milliseconds_per_frame", "Time per frame", 5, 60, 1, EFFECT_LIVE, "%.0f ms",
		NULL, "Most simulation time in a frame; beyond it the game slows down", false },
	{ TAB_WATER, "simulation", "pause_in_background", "Pause in background", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL, false },
	HEADING(TAB_OBJECTS, "Vegetation"),
	{ TAB_OBJECTS, "objects", "density_scale", "Density", 0, 3, 0.05f, EFFECT_OBJECTS, NULL, NULL, "Multiplies every plant's number per hectare", false },
	{ TAB_OBJECTS, "objects", "brush_fill_seconds", "Brush fill time", 0.1f, 5, 0.1f, EFFECT_LIVE, "%.1f s", NULL, NULL, false },
	HEADING(TAB_OBJECTS, "Wind in the plants"),
	{ TAB_OBJECTS, "render", "plant_wind_strength", "Wind", 0, 1, 0.01f, EFFECT_LIVE, "%.2f", NULL,
		"How hard the wind blows in the grass and the trees (0: still, 1: a gale); objects.ini says how much each kind sways", false },
	{ TAB_OBJECTS, "render", "plant_wind_direction_degrees", "Wind direction", 0, 360, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL,
		"Where it blows towards in the plants (not the clouds' wind): 0 = +X, 90 = +Z", false },
	{ TAB_OBJECTS, "render", "plant_wind_gust_meters", "Gusts apart", 5, 200, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How far apart the gusts are, rolling over the grass and through the woods", false },
	{ TAB_OBJECTS, "render", "grass_gust_sheen", "Gust waves", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How pale the grass a gust bows turns: the gusts seen rolling over a meadow (0: none)", false },
	HEADING(TAB_OBJECTS, "Distance"),
	{ TAB_OBJECTS, "objects", "draw_distance_scale", "Object distance", 0.2f, 3, 0.05f, EFFECT_LIVE, NULL, NULL, "Multiplies every object's draw distance", false },
	{ TAB_OBJECTS, "objects", "impostors", "Far objects as pictures", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Trees and houses far away are drawn as pictures of themselves (impostors): faster, the same at that size", false },
	{ TAB_OBJECTS, "objects", "impostor_pixels", "Pictures from", 16, 512, 4, EFFECT_LIVE, "%.0f px", NULL,
		"Objects smaller than this on screen are drawn as pictures (their pictures are 64 px: above that they are a little soft)", false },
	{ TAB_OBJECTS, "objects", "lod", "Fewer branches far away", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Trees made by potop_plant lose their finest branches, then the next, the smaller they are on screen (only the trunk and the foliage stay)", false },
	{ TAB_OBJECTS, "objects", "lod_distance_scale", "Branches as far as", 0.25f, 4, 0.05f, EFFECT_LIVE, "x%.2f", NULL,
		"Higher: the branches stay to farther away (finer, slower); lower: they go sooner", false },
	{ TAB_OBJECTS, "objects", "lod_foliage_kept", "Far foliage kept", 0.1f, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The share of a far tree's twigs and leaves drawn (1: all); thinned out evenly", false },
	{ TAB_OBJECTS, "objects", "prop_lod_pixels", "Rocks' detail off by", 0.1f, 4, 0.05f, EFFECT_LIVE, "%.2f px", NULL,
		"Rocks and walls made by potop_prop switch to a simpler level once it would be off by less than this on screen (lower: finer, the simpler ones only farther away)", false },
	HEADING(TAB_OBJECTS, "Grass"),
	{ TAB_OBJECTS, "objects", "grass", "Grass blades", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Blades of grass made on the graphics card every frame, on the ground that shows grass", false },
	{ TAB_OBJECTS, "objects", "grass_distance_meters", "Grass as far as", 10, 300, 5, EFFECT_LIVE, "%.0f m", NULL,
		"How far from the eye the blades go (farther they thin out and widen; past this the ground's texture alone)", false },
	{ TAB_OBJECTS, "objects", "grass_density", "Grass density", 10, 1500, 10, EFFECT_LIVE, "%.0f /m\xc2\xb2", NULL,
		"Blades a square meter near the eye", false },
	{ TAB_OBJECTS, "objects", "grass_height_meters", "Grass height", 0.05f, 1.5f, 0.01f, EFFECT_LIVE, "%.2f m", NULL,
		"How tall the blades are, give or take (shorter where less grows)", false },
	{ TAB_OBJECTS, "objects", "grass_width_meters", "Blade width", 0.004f, 0.08f, 0.001f, EFFECT_LIVE, "%.3f m", NULL,
		"How wide a blade is at its foot, near the eye", false },
	{ TAB_OBJECTS, "objects", "grass_tint", "Grass tint", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Multiplies the ground's grass color, which the blades take", false },
	{ TAB_OBJECTS, "objects", "grass_dry_share", "Dry blades", 0, 0.6f, 0.01f, EFFECT_LIVE, "%.2f", NULL,
		"The share of yellowed, dry blades", false },
	{ TAB_OBJECTS, "objects", "grass_coarse_meters", "Simple blades from", 0, 50, 1, EFFECT_LIVE, "%.0f m", NULL,
		"Farther blades one triangle each, not four (cheaper; 0: never)", false },
	{ TAB_OBJECTS, "objects", "grass_coarse_width", "Simple blades' width", 0.5f, 3, 0.05f, EFFECT_LIVE, "x%.2f", NULL,
		"How much wider the simple blades are (the ground as covered)", false },
	{ TAB_OBJECTS, "objects", "effects_meters", "Buildings' fire and smoke", 0, 400, 10, EFFECT_LIVE, "%.0f m", NULL,
		"How far from the eye the forges burn and the chimneys smoke (0: nowhere)", false },
	HEADING(TAB_OBJECTS, "Physics"),
	{ TAB_OBJECTS, "physics", "enabled", "Physics on", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Off: objects stay where they are", false },
	{ TAB_OBJECTS, "physics", "drag_coefficient", "Water drag", 0.1f, 3, 0.05f, EFFECT_LIVE, NULL, NULL, "How hard flowing water pushes things", false },
	{ TAB_OBJECTS, "physics", "water_minimum_meters", "Water from", 0, 0.2f, 0.005f, EFFECT_LIVE, "%.3f m", NULL, "Shallower water does not push things", false },
	{ TAB_OBJECTS, "physics", "float_damping_per_second", "Bobbing brake", 0, 10, 0.1f, EFFECT_LIVE, "%.1f /s", NULL, "Stops floating things bobbing", false },
	{ TAB_OBJECTS, "physics", "wake_flow_meters_per_second", "Wakes at", 0.02f, 1, 0.01f, EFFECT_LIVE, "%.2f m/s", NULL,
		"Water flowing this fast wakes sleeping objects", false },
	{ TAB_OBJECTS, "physics", "hit_seconds", "Hit length", 0.02f, 0.5f, 0.01f, EFFECT_LIVE, "%.2f s", NULL,
		"Shorter hits are harder: they tear trees loose sooner", false },
	{ TAB_OBJECTS, "physics", "terrain_resync_meters", "Ground follows", 0.05f, 2, 0.05f, EFFECT_LIVE, "%.2f m", NULL,
		"Erosion this deep makes the physics ground follow", false },
	HEADING(TAB_OBJECTS, "Terrain brushes"),
	{ TAB_OBJECTS, "tools", "raise_meters_per_second", "Raise speed", 0.5f, 30, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, NULL, false },
	{ TAB_OBJECTS, "tools", "smooth_strength_per_second", "Smoothing", 0.5f, 10, 0.1f, EFFECT_LIVE, "%.1f", NULL, NULL, false },
	{ TAB_OBJECTS, "tools", "brush_maximum_radius_meters", "Largest brush", 20, 300, 5, EFFECT_LIVE, "%.0f m", NULL, NULL, false },
	HEADING(TAB_RENDER, "Shading"),
	{ TAB_RENDER, "render", "pbr_shading", "PBR shading", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Objects and actors lit physically: normal maps, roughness, metal, their own light (off: plain diffuse, as before)", false },
	HEADING(TAB_RENDER, "Shadows"),
	{ TAB_RENDER, "render", "shadow_strength", "Strength", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_RENDER, "render", "shadow_map_size", "Map shadows", 0, 0, 0, EFFECT_RENDER, NULL, "0|1024|2048|4096|8192",
		"Texels per side of the shadow map over the whole map (0: no shadows)", false },
	{ TAB_RENDER, "render", "near_shadow_map_size", "Near shadows", 0, 0, 0, EFFECT_RENDER, NULL, "0|1024|2048|4096",
		"The sharp shadows around the camera's target", false },
	{ TAB_RENDER, "render", "shadow_depth_16_bit", "16 bit shadows", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The shadow maps' depth in 16 bits: half their memory and bandwidth (takes effect at the next start)", false },
	{ TAB_RENDER, "render", "near_shadow_meters", "Near area", 20, 400, 5, EFFECT_LIVE, "%.0f m", NULL, NULL, false },
	{ TAB_RENDER, "render", "contact_shadow_meters", "Contact reach", 0.1f, 1.5f, 0.05f, EFFECT_LIVE, "%.2f m", NULL, NULL, false },
	{ TAB_RENDER, "render", "contact_shadow_strength", "Contact strength", 0, 1.5f, 0.05f, EFFECT_LIVE, "%.2f", NULL, NULL, false },
	{ TAB_RENDER, "render", "ambient_occlusion", "Ambient occlusion", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The small scale shade of the sky's light (a house's foot, under the eaves, round a barrel, GTAO) and the small shadows the maps are too coarse for (stones on the ground, under a door), from the scene's depth", false },
	{ TAB_RENDER, "render", "ambient_occlusion_meters", "Occlusion radius", 0.25f, 3.0f, 0.05f, EFFECT_LIVE, "%.2f m", NULL, NULL, false },
	{ TAB_RENDER, "render", "ambient_occlusion_strength", "Occlusion strength", 0, 2.0f, 0.05f, EFFECT_LIVE, "%.2f", NULL, NULL, false },
	{ TAB_RENDER, "render", "ambient_occlusion_distance", "Occlusion distance", 20, 1000, 10, EFFECT_LIVE, "%.0f m", NULL, NULL, false },
	HEADING(TAB_RENDER, "Sky occlusion"),
	{ TAB_RENDER, "render", "sky_occlusion", "Sky occlusion", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Hollows, the ground under trees and round rocks see less of the sky (worked out once per map, kept with it)", false },
	{ TAB_RENDER, "render", "sky_occlusion_intensity", "Strength", 0, 1.5f, 0.05f, EFFECT_LIVE, "%.2f", NULL, NULL, false },
	{ TAB_RENDER, "render", "sky_occlusion_directions", "Directions", 0, 0, 0, EFFECT_LIVE, NULL, "16|32|64|128|256",
		"From how many directions of the sky (more: smoother, slower to work out; Work it out)", false },
	{ TAB_RENDER, "render", "sky_occlusion_texel_meters", "Detail", 0.1f, 4, 0.05f, EFFECT_LIVE, "%.2f m", NULL,
		"Texel of each direction's depth map (smaller: small things shade finer; at most 8192 texels over the map; Work it out)", false },
	{ TAB_RENDER, "render", "bounce_light", "Bounce light", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The light a sunlit slope or wall sends back onto what faces it (a crater's shady side, a cliff's foot)", false },
	{ TAB_RENDER, "render", "sky_occlusion_least_height", "Smallest caster", 0, 10, 0.1f, EFFECT_LIVE, "%.1f m", NULL,
		"Things lower than this cast none: ferns, flowers, small bushes would shade themselves (Work it out)", false },
	{ TAB_RENDER, "render", "sky_occlusion_lowest_degrees", "Lowest", 0, 45, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL,
		"The lowest direction above the horizon (Work it out)", false },
	{ TAB_RENDER, "render", "sky_occlusion_directions_per_frame", "Per frame", 1, 32, 1, EFFECT_LIVE, "%.0f", NULL,
		"Directions worked out each frame while it is worked out (more: done sooner, the frames slower meanwhile)", false },
	HEADING(TAB_RENDER, "Anti-aliasing"),
	{ TAB_RENDER, "render", "msaa", "Multisampling (MSAA)", 0, 0, 0, EFFECT_LIVE, NULL, "1:Off|2:2x|4:4x|8:8x",
		"Smooth edges on the land, the things on it and the leaves' cut outs, several samples a pixel (Save, then start the game again: the log says how many it runs with)", false },
	{ TAB_RENDER, "render", "taa", "Temporal (TAA)", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Edges and thin things steady as you move: each pixel averaged over the last frames", false },
	{ TAB_RENDER, "render", "taa_when_moving", "Only going forward", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The temporal anti-aliasing only while you go forward or back (standing, turning round, going sideways: off, no jitter, no softness)", false },
	{ TAB_RENDER, "render", "taa_history_weight", "Frames kept", 0.5f, 0.97f, 0.01f, EFFECT_LIVE, "%.2f", NULL,
		"How much of each pixel is the frames before (more: smoother, more trails on what moves)", false },
	{ TAB_RENDER, "render", "taa_sharpness", "Sharpness", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How sharp the frames before are taken (0: softer)", false },
	{ TAB_RENDER, "render", "taa_mip_bias", "Texture sharpening", -2, 0, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"With TAA on, the buildings' textures read this many detail levels sharper (it softens them)", false },
	HEADING(TAB_RENDER, "Speed"),
	{ TAB_RENDER, "render", "render_scale", "Resolution", 0.25f, 2, 0.05f, EFFECT_LIVE, NULL, NULL, "The scene drawn at this times the window's resolution (2: 2 x 2 supersampling, smooth edges, four times the pixels)", false },
	{ TAB_RENDER, "render", "lod_distance_meters", "Terrain detail", 0, 400, 5, EFFECT_LIVE, "%.0f m", NULL, "Full detail up to here; 0: everywhere", false },
	{ TAB_RENDER, "render", "far_terrain_upload_seconds", "Far ground updates", 0, 5, 0.1f, EFFECT_LIVE, "%.1f s", NULL,
		"Ground changed beyond full detail or off screen is redrawn at most this often (0: at once)", false },
	{ TAB_RENDER, "render", "far_water_upload_seconds", "Far water updates", 0, 2, 0.05f, EFFECT_LIVE, "%.2f s", NULL,
		"Water beyond full detail or off screen is redrawn at most this often (0: every step)", false },
	{ TAB_RENDER, "render", "far_upload_meters", "Far from", 0, 2000, 10, EFFECT_LIVE, "%.0f m", NULL,
		"Ground and water nearer than this are always redrawn at once (if on screen)", false },
	{ TAB_RENDER, "render", "vsync", "Vertical sync", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_RENDER, "render", "max_fps", "Frame rate limit", 0, 360, 1, EFFECT_LIVE, "%.0f fps", NULL,
		"The game draws at most this many frames a second (less heat, less power; with vsync too); 0: no limit", false },
	{ TAB_RENDER, "render", "start_fullscreen", "Start fullscreen", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The game starts fullscreen (from the next start; F11 still switches)", false },
	{ TAB_RENDER, "render", "start_maximized", "Start maximized", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The game starts in a maximized window (from the next start; fullscreen wins if both are on)", false },
	HEADING(TAB_TERRAIN, "Map"),
	{ TAB_TERRAIN, "terrain", "size", "Cells per side", 0, 0, 0, EFFECT_GENERATE, NULL, "512|1024|2048|4096", NULL, false },
	{ TAB_TERRAIN, "terrain", "cell_size_meters", "Cell size", 0.25f, 4, 0.25f, EFFECT_GENERATE, "%.2f m", NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "max_height_meters", "Highest point", 10, 400, 5, EFFECT_GENERATE, "%.0f m", NULL, NULL, false },
	HEADING(TAB_TERRAIN, "Shape"),
	{ TAB_TERRAIN, "terrain", "feature_count", "Big features", 1, 10, 0.1f, EFFECT_GENERATE, "%.1f", NULL, "Hills and valleys across the map", false },
	{ TAB_TERRAIN, "terrain", "minimum_feature_meters", "Finest detail", 1, 64, 1, EFFECT_GENERATE, "%.0f m", NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "persistence", "Roughness", 0.2f, 0.7f, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "warp_amount", "Warp", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "mountain_amount", "Mountains", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, "0 rolling hills, 1 ridged mountains", false },
	{ TAB_TERRAIN, "terrain", "mountain_coverage", "Mountain area", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "thermal_iterations", "Slides", 0, 200, 1, EFFECT_GENERATE, NULL, NULL, "Thermal erosion passes", false },
	{ TAB_TERRAIN, "terrain", "talus_slope", "Steepest slope", 0.2f, 2, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL, false },
	HEADING(TAB_TERRAIN, "Soils"),
	{ TAB_TERRAIN, "terrain", "rock_slope", "Rock from slope", 0.3f, 2, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "rock_height_fraction", "Rock above", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "sand_height_fraction", "Sand below", 0, 0.3f, 0.005f, EFFECT_GENERATE, "%.3f", NULL, NULL, false },
	{ TAB_TERRAIN, "terrain", "clay_height_fraction", "Clay below", 0, 0.5f, 0.005f, EFFECT_GENERATE, "%.3f", NULL, NULL, false },
	{ TAB_TERRAIN, "objects", "populate", "Grow vegetation", 0, 0, 0, EFFECT_GENERATE, NULL, NULL, "New maps get trees, grass and villages", false },
	HEADING(TAB_CAMERA, "Camera"),
	{ TAB_CAMERA, "camera", "field_of_view_degrees", "Field of view (overhead)", 15, 90, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL, false },
	{ TAB_CAMERA, "camera", "rotate_degrees_per_pixel", "Turn speed", 0.05f, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_CAMERA, "camera", "keyboard_pan_speed", "Key pan speed", 0.1f, 3, 0.05f, EFFECT_LIVE, NULL, NULL, "Screen heights per second", false },
	{ TAB_CAMERA, "camera", "zoom_step_factor", "Zoom step", 1.02f, 1.5f, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_CAMERA, "camera", "maximum_distance_meters", "Farthest zoom", 200, 6000, 50, EFFECT_LIVE, "%.0f m", NULL, NULL, false },
	{ TAB_CAMERA, "camera", "maximum_pitch_degrees", "Steepest view", 30, 89, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL, false },
	HEADING(TAB_CAMERA, "On foot (C)"),
	{ TAB_CAMERA, "camera", "first_person_field_of_view_degrees", "Field of view (on foot)", 15, 110, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL, false },
	{ TAB_CAMERA, "camera", "first_person_near_meters", "Near plane", 0.02f, 1, 0.01f, EFFECT_LIVE, "%.2f m", NULL,
		"Farther: less flicker far off; walls close by cut away", false },
	{ TAB_CAMERA, "camera", "first_person_look_degrees_per_pixel", "Mouse speed", 0.02f, 0.5f, 0.01f, EFFECT_LIVE, NULL, NULL, NULL, false },
	{ TAB_CAMERA, "camera", "first_person_invert_mouse", "Invert mouse", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Mouse forward looks down, like a flight stick", false },
	{ TAB_CAMERA, "camera", "walk_meters_per_second", "Walk speed", 1, 10, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, NULL, false },
	{ TAB_CAMERA, "camera", "run_meters_per_second", "Run speed", 2, 30, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, "With shift", false },
	{ TAB_CAMERA, "tools", "interact_highlight", "Glow what is in reach", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"What you look at, within reach, glows if the game can do something with it", false },
	HEADING(TAB_DEBUG, "Buildings' look"),
	{ TAB_DEBUG, "render", "roof_lightness", "Roof lightness", 0, 0.6f, 0.01f, EFFECT_LIVE, NULL, NULL, "How much lighter or darker each roof is than the next", false },
	{ TAB_DEBUG, "render", "roof_fade", "Roof fading", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "How much more or less faded each roof is", false },
	{ TAB_DEBUG, "render", "roof_warmth", "Roof warmth", 0, 0.2f, 0.005f, EFFECT_LIVE, NULL, NULL, "How much warmer or cooler each roof is", false },
	{ TAB_DEBUG, "render", "roof_patches", "Roof patches", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "Weathered patches meters across, grayer and lighter", false },
	{ TAB_DEBUG, "render", "roof_runs", "Roof runs", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "Runs down the slopes where the rain goes", false },
	{ TAB_DEBUG, "render", "roof_mended", "Roof mended", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "Stretches laid new, in the fresh color", false },
	{ TAB_DEBUG, "render", "wood_dulled", "Old wood dulled", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"How much old wood the sky does not reach (under logs, under the eaves) dulls to brown", false },
	HEADING(TAB_DEBUG, "Shadows"),
	{ TAB_DEBUG, "render", "debug_far_shadow_off", "Far shadow map off", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The big shadow map (the terrain, everything standing) neither drawn nor used: what it costs", false },
	{ TAB_DEBUG, "render", "debug_near_shadow_off", "Near shadow map off", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The near shadow map (round you: the small things, the actors) neither drawn nor used: what it costs", false },
	{ TAB_DEBUG, "render", "debug_far_shadow_no_plants", "Far shadow map without plants", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The big shadow map drawn without the trees, bushes, grass and crops: how their far shadows look and what they cost", false },
	HEADING(TAB_DEBUG, "Water drawing"),
	{ TAB_DEBUG, "render", "water_depth_test", "Water depth test", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Off: the water is drawn over everything, hills in front of it included (debugging)", false },
	{ TAB_DEBUG, "render", "water_depth_write", "Water depth write", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"On: the water hides water behind it (debugging)", false },
	{ TAB_DEBUG, "render", "water_cull_back_faces", "Water back face culling", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Off: the faces of the water turned away from the camera are drawn too (debugging)", false },
	HEADING(TAB_DEBUG, "Water sleep"),
	{ TAB_DEBUG, "water", "sleep_speed_meters_per_second", "Sleeps below", 0.005f, 0.5f, 0.005f, EFFECT_LIVE, "%.3f m/s", NULL,
		"A tile sleeps once all its water is slower than this", false },
	{ TAB_DEBUG, "water", "sleep_change_millimeters_per_second", "Level still below", 0.5f, 10000, 1, EFFECT_LIVE, "%.1f mm/s", NULL,
		"A tile sleeps once no cell's level changes faster than this (small waves keep it awake below it)", false },
	{ TAB_DEBUG, "water", "sleep_after_substeps", "Sleeps after", 1, 60, 1, EFFECT_LIVE, NULL, NULL, "Substeps of still water in a row", false },
	{ TAB_DEBUG, "water", "asleep_wake_millimeters_per_second", "Wakes for", 1, 300, 1, EFFECT_LIVE, "%.0f mm/s", NULL,
		"Water pouring into a sleeping tile faster wakes it; slower is thrown away", false },
	{ TAB_DEBUG, "water", "asleep_level_wake_millimeters", "Wakes for drop", 5, 500, 5, EFFECT_LIVE, "%.0f mm", NULL,
		"A neighbor this much lower wakes a sleeping tile (a breaking dam)", false },
	HEADING(TAB_DEBUG, "Physics"),
	{ TAB_DEBUG, "physics", "substeps", "Substeps", 1, 8, 1, EFFECT_LIVE, NULL, NULL, "More is steadier and slower", false },
};

#undef HEADING



// One row of the table; returns true if the value changed
static bool setting_row(settings_window_t *window, settings_t *settings, const ui_setting_t *entry)
{
	setting_type_t type;
	void *value = settings_find(settings, entry->section, entry->key, &type);

	if (value == NULL)
	{
		return false;
	}
	switch (type)
	{
		case SETTING_FLOAT:
			return ui_widget_slider(window->context, entry->label, value, entry->minimum, entry->maximum, entry->step,
				entry->format != NULL ? entry->format : "%.2f", entry->tooltip);
		case SETTING_INT:
			if (entry->choices != NULL)
			{
				return ui_widget_choice(window->context, entry->label, value, entry->choices, entry->tooltip);
			}
			return ui_widget_slider_int(window->context, entry->label, value, (int)entry->minimum, (int)entry->maximum, SDL_max((int)entry->step, 1),
				entry->tooltip);
		case SETTING_BOOL:
			return ui_widget_checkbox(window->context, entry->label, value, entry->tooltip);
		case SETTING_COLOR:
			return ui_widget_color(window->context, entry->label, value, entry->tooltip);
		case SETTING_UINT:
		default:
			return false;
	}
}

// What follows a setting changed in the window
static void setting_changed(settings_window_t *window, settings_t *settings, const ui_setting_t *entry)
{
	// A cloud type sets the clouds' values; changing one of them makes the
	// clouds custom
	if (SDL_strcmp(entry->key, "cloud_type") == 0)
	{
		settings_apply_cloud_type(&settings->render);
	}
	else if (SDL_strcmp(entry->section, "render") == 0 && settings_is_cloud_type_key(entry->key))
	{
		settings->render.cloud_type = CLOUD_TYPE_CUSTOM;
	}
	if (entry->effect == EFFECT_RENDER)
	{
		window->render_pending = true;
	}
	else if (entry->effect == EFFECT_OBJECTS)
	{
		window->objects_pending = true;
	}
}

static void heading_row(settings_window_t *window, const char *text, bool first)
{
	struct nk_context *context = window->context;

	if (!first)
	{
		nk_layout_row_dynamic(context, 4.0f, 1);
		nk_spacer(context);
	}
	ui_widget_section(window->context, text);
}

static void new_map_controls(settings_window_t *window, const settings_window_view_t *view)
{
	struct nk_context *context = window->context;
	nk_flags state;

	nk_layout_row(context, NK_DYNAMIC, UI_ROW_HEIGHT, 3, (const float[]){ 0.38f, 0.32f, 0.30f });
	ui_widget_tooltip(context, "Empty: a random seed");
	nk_label(context, "Seed", NK_TEXT_LEFT);
	state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, window->seed_input, sizeof(window->seed_input), nk_filter_decimal);
	if (state & NK_EDIT_ACTIVE)
	{
		window->backend->wants_text_input = true;
	}
	if (nk_button_label(context, "This map's"))
	{
		SDL_snprintf(window->seed_input, sizeof(window->seed_input), "%u", view->seed);
	}
	nk_layout_row_dynamic(context, UI_ROW_HEIGHT + 2.0f, 1);
	if (nk_button_label(context, "Generate a new map") || (state & NK_EDIT_COMMITED))
	{
		window->requests.new_map = true;
		window->requests.new_map_seed = (uint32_t)SDL_strtoul(window->seed_input, NULL, 10);
	}
}

static void interface_tab(settings_window_t *window, const settings_window_view_t *view)
{
	ui_widget_slider(window->context, "Size", &window->pending_scale, 0.75f, 2.5f, 0.05f, "%.2f", "Scale of the whole interface (Apply)");
	ui_widget_slider(window->context, "Font size", &window->pending_font_size, 10.0f, 28.0f, 1.0f, "%.0f", "In interface units (Apply)");
	ui_widget_slider(window->context, "Dialog text", &window->pending_dialog_text_scale, 1.0f, 3.0f, 0.05f, "%.2f",
		"The conversations' text, times the font size (a television across the room: 2 or more; Apply)");
	window->interface_pending = window->pending_scale != window->style->scale || window->pending_font_size != window->style->font_size
		|| window->pending_dialog_text_scale != window->style->dialog_text_scale;
	ui_widget_slider(window->context, "Panels", &window->style->panel_opacity, 0.0f, 1.0f, 0.05f, "%.2f",
		"How see-through the panels are while the pointer is elsewhere");
	if (view->interface_extra != NULL)
	{
		view->interface_extra(view->user, window->context);
	}
}

void settings_window_draw(settings_window_t *window, ui_backend_t *backend, ui_style_t *style, settings_t *settings,
	const settings_window_view_t *view)
{
	struct nk_context *context = &backend->context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;

	SDL_zero(window->requests);
	window->context = context;
	window->backend = backend;
	window->style = style;
	if (!window->open)
	{
		return;
	}
	if (nk_window_is_hidden(context, SETTINGS_WINDOW) && nk_window_find(context, SETTINGS_WINDOW) != NULL)
	{
		nk_window_show(context, SETTINGS_WINDOW, NK_SHOWN);
	}
	if (nk_begin(context, SETTINGS_WINDOW, window->bounds, flags))
	{
		struct nk_rect region;
		float footer = 2.0f * (UI_ROW_HEIGHT + context->style.window.spacing.y) + 8.0f;

		window->bounds = nk_window_get_bounds(context);
		window->tab = SDL_clamp(window->tab, 0, TAB_COUNT - 1);
		// The tabs in two rows, then the search
		for (int tab = 0; tab < TAB_COUNT; tab++)
		{
			if (tab % SETTINGS_TABS_PER_ROW == 0)
			{
				nk_layout_row_dynamic(context, UI_ROW_HEIGHT, SETTINGS_TABS_PER_ROW);
			}
			if (ui_widget_toggle_button(context, tab_names[tab], NULL, window->tab == tab && window->search[0] == '\0'))
			{
				window->tab = tab;
				window->search[0] = '\0';
			}
		}
		nk_layout_row_begin(context, NK_DYNAMIC, UI_ROW_HEIGHT, 2);
		nk_layout_row_push(context, 0.15f);
		nk_label(context, "Search", NK_TEXT_LEFT);
		nk_layout_row_push(context, 0.85f);
		ui_widget_tooltip(context, "Shows the settings of every tab whose name (or key) has this");
		nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, window->search, (int)sizeof(window->search), nk_filter_default);
		nk_layout_row_end(context);
		region = nk_window_get_content_region(context);
		nk_layout_row_dynamic(context, SDL_max(region.h - 3.0f * UI_ROW_HEIGHT - footer - 16.0f, 60.0f), 1);
		if (window->search[0] != '\0')
		{
			// Searching: the rows of every tab that match, under their tab's name
			if (nk_group_begin(context, "Search results", NK_WINDOW_BORDER))
			{
				int last_tab = -1;
				int found = 0;

				for (size_t i = 0; i < sizeof(ui_settings) / sizeof(ui_settings[0]); i++)
				{
					const ui_setting_t *entry = &ui_settings[i];

					if (entry->section == NULL || (entry->editor_only && !view->editor) || (SDL_strcasestr(entry->label, window->search) == NULL
						&& SDL_strcasestr(entry->key, window->search) == NULL))
					{
						continue;
					}
					if ((int)entry->tab != last_tab)
					{
						heading_row(window, tab_names[entry->tab], last_tab < 0);
						last_tab = (int)entry->tab;
					}
					found++;
					if (setting_row(window, settings, entry))
					{
						setting_changed(window, settings, entry);
					}
				}
				if (found == 0)
				{
					nk_layout_row_dynamic(context, UI_ROW_HEIGHT, 1);
					nk_label(context, "Nothing has that in its name", NK_TEXT_LEFT);
				}
				nk_group_end(context);
			}
		}
		else if (nk_group_begin(context, tab_names[window->tab], NK_WINDOW_BORDER))
		{
			bool first = true;

			if (window->tab == TAB_WATER)
			{
				nk_bool pipes = view->depth_scaled_pipes;

				heading_row(window, "Now", true);
				first = false;
				nk_layout_row_dynamic(context, UI_ROW_HEIGHT - 4.0f, 1);
				ui_widget_tooltip(context, "Thin water flows slowly; off: the classic fixed pipes (M)");
				if (nk_checkbox_label(context, "Depth scaled pipes", &pipes))
				{
					window->requests.toggle_water_model = true;
				}
			}
			if (window->tab == TAB_INTERFACE)
			{
				interface_tab(window, view);
			}
			for (size_t i = 0; i < sizeof(ui_settings) / sizeof(ui_settings[0]); i++)
			{
				const ui_setting_t *entry = &ui_settings[i];

				if ((int)entry->tab != window->tab || (entry->editor_only && !view->editor))
				{
					continue;
				}
				if (entry->section == NULL)
				{
					heading_row(window, entry->label, first);
					first = false;
					continue;
				}
				first = false;
				if (setting_row(window, settings, entry))
				{
					setting_changed(window, settings, entry);
				}
			}
			if (window->tab == TAB_RENDER)
			{
				heading_row(window, "Sky occlusion now", false);
				nk_layout_row_dynamic(context, UI_ROW_HEIGHT, 1);
				ui_widget_tooltip(context, "Over the whole map, with the values above, a few directions a frame (U)");
				if (nk_button_label(context, "Work it out"))
				{
					window->requests.bake_sky_occlusion = true;
				}
			}
			if (window->tab == TAB_TERRAIN)
			{
				heading_row(window, "Generate", false);
				new_map_controls(window, view);
			}
			if (window->tab == TAB_OBJECTS)
			{
				heading_row(window, "Whole map", false);
				nk_layout_row_dynamic(context, UI_ROW_HEIGHT, 2);
				ui_widget_tooltip(context, "Removes the vegetation and grows it again (V)");
				if (nk_button_label(context, "Grow anew"))
				{
					window->requests.regrow = true;
					window->objects_pending = false;
				}
				ui_widget_tooltip(context, "Every object, villages too (shift + V)");
				if (nk_button_label(context, "Remove all"))
				{
					window->requests.remove_objects = true;
				}
			}
			nk_group_end(context);
		}

		// Footer: what waits for Apply, and the buttons
		{
			bool pending = window->render_pending || window->objects_pending || window->interface_pending;
			char note[128] = "";
			struct nk_color dim = window->style->colors[NK_COLOR_TEXT];

			dim.a = 160;
			if (pending)
			{
				SDL_snprintf(note, sizeof(note), "Apply: %s%s%s", window->render_pending ? "graphics " : "",
					window->objects_pending ? "vegetation " : "", window->interface_pending ? "interface" : "");
			}
			else if (window->tab == TAB_TERRAIN)
			{
				SDL_strlcpy(note, "These shape the next new map.", sizeof(note));
			}
			nk_layout_row_dynamic(context, UI_ROW_HEIGHT - 6.0f, 1);
			nk_label_colored(context, note, NK_TEXT_LEFT, dim);
			nk_layout_row_dynamic(context, UI_ROW_HEIGHT, 3);
			if (!pending)
			{
				nk_widget_disable_begin(context);
			}
			if (nk_button_label(context, "Apply"))
			{
				window->requests.apply_render = window->render_pending;
				window->requests.apply_objects = window->objects_pending;
				window->render_pending = false;
				window->objects_pending = false;
				if (window->interface_pending)
				{
					window->style->scale = window->pending_scale;
					window->style->font_size = window->pending_font_size;
					window->style->dialog_text_scale = window->pending_dialog_text_scale;
					window->interface_pending = false;
					// Rebuilt after this frame is drawn (see ui_render)
				}
			}
			if (!pending)
			{
				nk_widget_disable_end(context);
			}
			ui_widget_tooltip(context, "Keep these settings for the next start (settings_changed.ini)");
			if (nk_button_label(context, "Save"))
			{
				window->requests.save_settings = true;
			}
			ui_widget_tooltip(context, "Back to the values of data/settings.ini");
			if (nk_button_label(context, "Restore"))
			{
				window->requests.restore_settings = true;
				window->render_pending = false;
				window->objects_pending = false;
			}
		}
	}
	else
	{
		// Closed with its X
		window->open = false;
	}
	nk_end(context);
	if (!window->open)
	{
		nk_window_close(context, SETTINGS_WINDOW);
	}
}

void settings_window_init(settings_window_t *window, const ui_style_t *style)
{
	SDL_zerop(window);
	window->bounds = nk_rect(320.0f, 60.0f, 520.0f, 560.0f);
	window->pending_scale = style->scale;
	window->pending_font_size = style->font_size;
	window->pending_dialog_text_scale = style->dialog_text_scale;
}
