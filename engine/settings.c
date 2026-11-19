// Game settings. See settings.h.

#include "settings.h"
#include "file_io.h"
#include "ini_parser.h"

#include <stddef.h>
#include <SDL3/SDL.h>

// One row per ini key. Driving the parser from a table keeps the list of
// keys, their types and their location in one place.
typedef struct
{
	const char *section;
	const char *key;
	setting_type_t type;
	size_t offset;
} setting_field_t;

#define FIELD(section, key, type, member) { section, key, type, offsetof(settings_t, member) }

static const setting_field_t setting_fields[] =
{
	FIELD("terrain", "size", SETTING_INT, terrain.size),
	FIELD("terrain", "seed", SETTING_UINT, terrain.seed),
	FIELD("terrain", "cell_size_meters", SETTING_FLOAT, terrain.cell_size_meters),
	FIELD("terrain", "max_height_meters", SETTING_FLOAT, terrain.max_height_meters),
	FIELD("terrain", "feature_count", SETTING_FLOAT, terrain.feature_count),
	FIELD("terrain", "minimum_feature_meters", SETTING_FLOAT, terrain.minimum_feature_meters),
	FIELD("terrain", "persistence", SETTING_FLOAT, terrain.persistence),
	FIELD("terrain", "warp_amount", SETTING_FLOAT, terrain.warp_amount),
	FIELD("terrain", "mountain_amount", SETTING_FLOAT, terrain.mountain_amount),
	FIELD("terrain", "mountain_coverage", SETTING_FLOAT, terrain.mountain_coverage),
	FIELD("terrain", "thermal_iterations", SETTING_INT, terrain.thermal_iterations),
	FIELD("terrain", "talus_slope", SETTING_FLOAT, terrain.talus_slope),
	FIELD("terrain", "gully_depth_meters", SETTING_FLOAT, terrain.gully_depth_meters),
	FIELD("terrain", "mountain_soil_pockets", SETTING_FLOAT, terrain.mountain_soil_pockets),
	FIELD("terrain", "rock_slope", SETTING_FLOAT, terrain.rock_slope),
	FIELD("terrain", "rock_height_fraction", SETTING_FLOAT, terrain.rock_height_fraction),
	FIELD("terrain", "sand_height_fraction", SETTING_FLOAT, terrain.sand_height_fraction),
	FIELD("terrain", "clay_height_fraction", SETTING_FLOAT, terrain.clay_height_fraction),

	FIELD("cache", "use_cache", SETTING_BOOL, cache.use_cache),
	FIELD("cache", "maximum_files", SETTING_INT, cache.cache_maximum_files),

	FIELD("camera", "field_of_view_degrees", SETTING_FLOAT, camera.field_of_view_degrees),
	FIELD("camera", "minimum_pitch_degrees", SETTING_FLOAT, camera.minimum_pitch_degrees),
	FIELD("camera", "maximum_pitch_degrees", SETTING_FLOAT, camera.maximum_pitch_degrees),
	FIELD("camera", "default_pitch_degrees", SETTING_FLOAT, camera.default_pitch_degrees),
	FIELD("camera", "minimum_distance_meters", SETTING_FLOAT, camera.minimum_distance_meters),
	FIELD("camera", "maximum_distance_meters", SETTING_FLOAT, camera.maximum_distance_meters),
	FIELD("camera", "rotate_degrees_per_pixel", SETTING_FLOAT, camera.rotate_degrees_per_pixel),
	FIELD("camera", "keyboard_pan_speed", SETTING_FLOAT, camera.keyboard_pan_speed),
	FIELD("camera", "zoom_step_factor", SETTING_FLOAT, camera.zoom_step_factor),
	FIELD("camera", "first_person_field_of_view_degrees", SETTING_FLOAT, camera.first_person_field_of_view_degrees),
	FIELD("camera", "first_person_near_meters", SETTING_FLOAT, camera.first_person_near_meters),
	FIELD("camera", "jump_height_scale", SETTING_FLOAT, camera.jump_height_scale),
	FIELD("camera", "first_person_look_degrees_per_pixel", SETTING_FLOAT, camera.first_person_look_degrees_per_pixel),
	FIELD("camera", "first_person_invert_mouse", SETTING_BOOL, camera.first_person_invert_mouse),
	FIELD("camera", "walk_meters_per_second", SETTING_FLOAT, camera.walk_meters_per_second),
	FIELD("camera", "run_meters_per_second", SETTING_FLOAT, camera.run_meters_per_second),

	FIELD("render", "vsync", SETTING_BOOL, render.vsync),
	FIELD("render", "max_fps", SETTING_FLOAT, render.max_fps),
	FIELD("render", "start_fullscreen", SETTING_BOOL, render.start_fullscreen),
	FIELD("render", "start_maximized", SETTING_BOOL, render.start_maximized),
	FIELD("render", "sky_color", SETTING_COLOR, render.sky_color),
	FIELD("render", "sky_zenith_color", SETTING_COLOR, render.sky_zenith_color),
	FIELD("render", "fog_density", SETTING_FLOAT, render.fog_density),
	FIELD("render", "fog_height_meters", SETTING_FLOAT, render.fog_height_meters),
	FIELD("render", "pbr_shading", SETTING_BOOL, render.pbr_shading),
	FIELD("render", "sun_azimuth_degrees", SETTING_FLOAT, render.sun_azimuth_degrees),
	FIELD("render", "sun_elevation_degrees", SETTING_FLOAT, render.sun_elevation_degrees),
	FIELD("render", "sun_color", SETTING_COLOR, render.sun_color),
	FIELD("render", "sun_intensity", SETTING_FLOAT, render.sun_intensity),
	FIELD("render", "ambient_intensity", SETTING_FLOAT, render.ambient_intensity),
	FIELD("render", "sky_occlusion", SETTING_BOOL, render.sky_occlusion),
	FIELD("render", "sky_occlusion_intensity", SETTING_FLOAT, render.sky_occlusion_intensity),
	FIELD("render", "bounce_light", SETTING_FLOAT, render.bounce_light),
	FIELD("render", "sky_occlusion_least_height", SETTING_FLOAT, render.sky_occlusion_least_height),
	FIELD("render", "sky_occlusion_directions", SETTING_INT, render.sky_occlusion_directions),
	FIELD("render", "sky_occlusion_texel_meters", SETTING_FLOAT, render.sky_occlusion_texel_meters),
	FIELD("render", "sky_occlusion_lowest_degrees", SETTING_FLOAT, render.sky_occlusion_lowest_degrees),
	FIELD("render", "sky_occlusion_directions_per_frame", SETTING_INT, render.sky_occlusion_directions_per_frame),
	FIELD("render", "texture_size", SETTING_INT, render.texture_size),
	FIELD("render", "anisotropy", SETTING_FLOAT, render.anisotropy),
	FIELD("render", "normal_strength", SETTING_FLOAT, render.normal_strength),
	FIELD("render", "snow_height_fraction", SETTING_FLOAT, render.snow_height_fraction),
	FIELD("render", "snow_aspect_shift", SETTING_FLOAT, render.snow_aspect_shift),
	FIELD("render", "detail_fade_start_meters", SETTING_FLOAT, render.detail_fade_start_meters),
	FIELD("render", "detail_fade_end_meters", SETTING_FLOAT, render.detail_fade_end_meters),
	FIELD("render", "shadow_map_size", SETTING_INT, render.shadow_map_size),
	FIELD("render", "shadow_strength", SETTING_FLOAT, render.shadow_strength),
	FIELD("render", "near_shadow_map_size", SETTING_INT, render.near_shadow_map_size),
	FIELD("render", "shadow_depth_16_bit", SETTING_BOOL, render.shadow_depth_16_bit),
	FIELD("render", "near_shadow_meters", SETTING_FLOAT, render.near_shadow_meters),
	FIELD("render", "contact_shadow_meters", SETTING_FLOAT, render.contact_shadow_meters),
	FIELD("render", "contact_shadow_strength", SETTING_FLOAT, render.contact_shadow_strength),
	FIELD("render", "ambient_occlusion", SETTING_BOOL, render.ambient_occlusion),
	FIELD("render", "ambient_occlusion_meters", SETTING_FLOAT, render.ambient_occlusion_meters),
	FIELD("render", "ambient_occlusion_strength", SETTING_FLOAT, render.ambient_occlusion_strength),
	FIELD("render", "ambient_occlusion_distance", SETTING_FLOAT, render.ambient_occlusion_distance),
	FIELD("render", "water_shallow_color", SETTING_COLOR, render.water_shallow_color),
	FIELD("render", "water_deep_color", SETTING_COLOR, render.water_deep_color),
	FIELD("render", "water_clarity_meters", SETTING_FLOAT, render.water_clarity_meters),
	FIELD("render", "water_reflections", SETTING_BOOL, render.water_reflections),
	FIELD("render", "water_reflections_half_size", SETTING_BOOL, render.water_reflections_half_size),
	FIELD("render", "puddles", SETTING_BOOL, render.puddles),
	FIELD("render", "water_refraction", SETTING_FLOAT, render.water_refraction),
	FIELD("render", "water_choppiness", SETTING_FLOAT, render.water_choppiness),
	FIELD("render", "water_reflection_meters", SETTING_FLOAT, render.water_reflection_meters),
	FIELD("render", "terrain_from_gpu", SETTING_BOOL, render.terrain_from_gpu),
	FIELD("render", "material_map", SETTING_BOOL, render.material_map),
	FIELD("render", "ground_overlay", SETTING_BOOL, render.ground_overlay),
	FIELD("render", "water_ice", SETTING_BOOL, render.water_ice),
	FIELD("render", "moss_shells", SETTING_INT, render.moss_shells),
	FIELD("render", "moss_shell_meters", SETTING_FLOAT, render.moss_shell_meters),
	FIELD("render", "moss_shell_distance_meters", SETTING_FLOAT, render.moss_shell_distance_meters),
	FIELD("render", "water_opacity_per_meter", SETTING_FLOAT, render.water_opacity_per_meter),
	FIELD("render", "foam_speed_meters_per_second", SETTING_FLOAT, render.foam_speed_meters_per_second),
	FIELD("render", "soil_blend_meters", SETTING_FLOAT, render.soil_blend_meters),
	FIELD("render", "material_normal_meters", SETTING_FLOAT, render.material_normal_meters),
	FIELD("render", "grass_slope_start", SETTING_FLOAT, render.grass_slope_start),
	FIELD("render", "grass_slope_end", SETTING_FLOAT, render.grass_slope_end),
	FIELD("render", "cliff_slope_start", SETTING_FLOAT, render.cliff_slope_start),
	FIELD("render", "cliff_slope_end", SETTING_FLOAT, render.cliff_slope_end),
	FIELD("render", "variation_tiling_meters", SETTING_FLOAT, render.variation_tiling_meters),
	FIELD("render", "variation_strength", SETTING_FLOAT, render.variation_strength),
	FIELD("render", "variation_hue_strength", SETTING_FLOAT, render.variation_hue_strength),
	FIELD("render", "cloud_coverage", SETTING_FLOAT, render.cloud_coverage),
	FIELD("render", "cloud_softness", SETTING_FLOAT, render.cloud_softness),
	FIELD("render", "cloud_shadow_strength", SETTING_FLOAT, render.cloud_shadow_strength),
	FIELD("render", "cloud_tiling_meters", SETTING_FLOAT, render.cloud_tiling_meters),
	FIELD("render", "cloud_height_meters", SETTING_FLOAT, render.cloud_height_meters),
	FIELD("render", "volumetric_clouds", SETTING_BOOL, render.volumetric_clouds),
	FIELD("render", "cloud_thickness_meters", SETTING_FLOAT, render.cloud_thickness_meters),
	FIELD("render", "cloud_density", SETTING_FLOAT, render.cloud_density),
	FIELD("render", "cloud_steps", SETTING_INT, render.cloud_steps),
	FIELD("render", "cloud_size", SETTING_FLOAT, render.cloud_size),
	FIELD("render", "cloud_towers", SETTING_FLOAT, render.cloud_towers),
	FIELD("render", "cloud_base_darkness", SETTING_FLOAT, render.cloud_base_darkness),
	FIELD("render", "cloud_dome_size", SETTING_INT, render.cloud_dome_size),
	FIELD("render", "cloud_refresh_seconds", SETTING_FLOAT, render.cloud_refresh_seconds),
	FIELD("render", "cloud_memory_seconds", SETTING_FLOAT, render.cloud_memory_seconds),
	FIELD("render", "cloud_density_variation", SETTING_FLOAT, render.cloud_density_variation),
	FIELD("render", "cloud_lifetime_minutes", SETTING_FLOAT, render.cloud_lifetime_minutes),
	FIELD("render", "cloud_horizon_drop_meters", SETTING_FLOAT, render.cloud_horizon_drop_meters),
	FIELD("render", "flock_size_meters", SETTING_FLOAT, render.flock_size_meters),
	FIELD("render", "flock_amount", SETTING_FLOAT, render.flock_amount),
	FIELD("render", "flock_trend", SETTING_FLOAT, render.flock_trend),
	FIELD("render", "flock_height_meters", SETTING_FLOAT, render.flock_height_meters),
	FIELD("render", "flock_density", SETTING_FLOAT, render.flock_density),
	FIELD("render", "flock_shade", SETTING_FLOAT, render.flock_shade),
	FIELD("render", "cloud_type", SETTING_INT, render.cloud_type),
	FIELD("render", "wind_direction_degrees", SETTING_FLOAT, render.wind_direction_degrees),
	FIELD("render", "wind_speed_meters_per_second", SETTING_FLOAT, render.wind_speed_meters_per_second),
	FIELD("render", "shadow_update_interval_seconds", SETTING_FLOAT, render.shadow_update_interval_seconds),
	FIELD("render", "far_shadow_least_meters", SETTING_FLOAT, render.far_shadow_least_meters),
	FIELD("render", "actor_shadow_update_seconds", SETTING_FLOAT, render.actor_shadow_update_seconds),
	FIELD("render", "shadow_erosion_meters", SETTING_FLOAT, render.shadow_erosion_meters),
	FIELD("render", "lod_distance_meters", SETTING_FLOAT, render.lod_distance_meters),
	FIELD("render", "far_terrain_upload_seconds", SETTING_FLOAT, render.far_terrain_upload_seconds),
	FIELD("render", "far_water_upload_seconds", SETTING_FLOAT, render.far_water_upload_seconds),
	FIELD("render", "far_upload_meters", SETTING_FLOAT, render.far_upload_meters),
	FIELD("render", "render_scale", SETTING_FLOAT, render.render_scale),
	FIELD("render", "taa", SETTING_BOOL, render.taa),
	FIELD("render", "msaa", SETTING_INT, render.msaa),
	FIELD("render", "taa_when_moving", SETTING_BOOL, render.taa_when_moving),
	FIELD("render", "taa_history_weight", SETTING_FLOAT, render.taa_history_weight),
	FIELD("render", "taa_sharpness", SETTING_FLOAT, render.taa_sharpness),
	FIELD("render", "taa_mip_bias", SETTING_FLOAT, render.taa_mip_bias),
	FIELD("render", "day_cycle", SETTING_BOOL, render.day_cycle),
	FIELD("render", "day_length_minutes", SETTING_FLOAT, render.day_length_minutes),
	FIELD("render", "daylight_hours", SETTING_FLOAT, render.daylight_hours),
	FIELD("render", "moon_cycle_days", SETTING_FLOAT, render.moon_cycle_days),
	FIELD("render", "golden_hour", SETTING_FLOAT, render.golden_hour),
	FIELD("render", "blue_hour", SETTING_FLOAT, render.blue_hour),
	FIELD("render", "blue_hour_on", SETTING_BOOL, render.blue_hour_on),
	FIELD("render", "moon_size_degrees", SETTING_FLOAT, render.moon_size_degrees),
	FIELD("render", "aurora_strength", SETTING_FLOAT, render.aurora_strength),
	FIELD("render", "rain_intensity", SETTING_FLOAT, render.rain_intensity),
	FIELD("render", "rain_streak_meters", SETTING_FLOAT, render.rain_streak_meters),
	FIELD("render", "rain_splashes", SETTING_FLOAT, render.rain_splashes),
	FIELD("render", "rainbow_strength", SETTING_FLOAT, render.rainbow_strength),
	FIELD("render", "plant_wind_strength", SETTING_FLOAT, render.plant_wind_strength),
	FIELD("render", "plant_wind_direction_degrees", SETTING_FLOAT, render.plant_wind_direction_degrees),
	FIELD("render", "plant_wind_gust_meters", SETTING_FLOAT, render.plant_wind_gust_meters),
	FIELD("render", "grass_gust_sheen", SETTING_FLOAT, render.grass_gust_sheen),
	FIELD("render", "month", SETTING_FLOAT, render.month),
	FIELD("render", "snow_first_month", SETTING_FLOAT, render.snow_first_month),
	FIELD("render", "snow_last_month", SETTING_FLOAT, render.snow_last_month),
	FIELD("render", "tips_first_month", SETTING_FLOAT, render.tips_first_month),
	FIELD("render", "tips_last_month", SETTING_FLOAT, render.tips_last_month),
	FIELD("render", "bloom_first_month", SETTING_FLOAT, render.bloom_first_month),
	FIELD("render", "bloom_last_month", SETTING_FLOAT, render.bloom_last_month),
	FIELD("render", "hdr", SETTING_BOOL, render.hdr),
	FIELD("render", "auto_exposure", SETTING_BOOL, render.auto_exposure),
	FIELD("render", "exposure_ev", SETTING_FLOAT, render.exposure_ev),
	FIELD("render", "exposure_adapt_seconds", SETTING_FLOAT, render.exposure_adapt_seconds),
	FIELD("render", "bloom", SETTING_BOOL, render.bloom),
	FIELD("render", "bloom_strength", SETTING_FLOAT, render.bloom_strength),
	FIELD("render", "tone_mapper", SETTING_INT, render.tone_mapper),
	FIELD("render", "color_grade", SETTING_INT, render.color_grade),
	FIELD("render", "color_grade_strength", SETTING_FLOAT, render.color_grade_strength),
	FIELD("render", "night_vision", SETTING_FLOAT, render.night_vision),
	FIELD("render", "night_vision_luminance", SETTING_FLOAT, render.night_vision_luminance),
	FIELD("render", "local_lights", SETTING_BOOL, render.local_lights),
	FIELD("render", "decals", SETTING_BOOL, render.decals),
	FIELD("render", "roof_lightness", SETTING_FLOAT, render.roof_lightness),
	FIELD("render", "roof_fade", SETTING_FLOAT, render.roof_fade),
	FIELD("render", "roof_warmth", SETTING_FLOAT, render.roof_warmth),
	FIELD("render", "roof_patches", SETTING_FLOAT, render.roof_patches),
	FIELD("render", "roof_runs", SETTING_FLOAT, render.roof_runs),
	FIELD("render", "roof_mended", SETTING_FLOAT, render.roof_mended),
	FIELD("render", "wood_dulled", SETTING_FLOAT, render.wood_dulled),
	FIELD("render", "local_light_intensity", SETTING_FLOAT, render.local_light_intensity),
	FIELD("render", "window_lights", SETTING_BOOL, render.window_lights),
	FIELD("render", "window_light_intensity", SETTING_FLOAT, render.window_light_intensity),
	FIELD("render", "lamps_with_the_day", SETTING_BOOL, render.lamps_with_the_day),
	FIELD("render", "fog_mode", SETTING_INT, render.fog_mode),
	FIELD("render", "fog_quality", SETTING_INT, render.fog_quality),
	FIELD("render", "fog_valley_density", SETTING_FLOAT, render.fog_valley_density),
	FIELD("render", "fog_valley_height_meters", SETTING_FLOAT, render.fog_valley_height_meters),
	FIELD("render", "fog_valley_reach_meters", SETTING_FLOAT, render.fog_valley_reach_meters),
	FIELD("render", "fog_softness_meters", SETTING_FLOAT, render.fog_softness_meters),
	FIELD("render", "fog_glow", SETTING_FLOAT, render.fog_glow),
	FIELD("render", "fog_shafts", SETTING_FLOAT, render.fog_shafts),
	FIELD("render", "fog_night_brightness", SETTING_FLOAT, render.fog_night_brightness),
	FIELD("render", "fog_rays", SETTING_FLOAT, render.fog_rays),
	FIELD("render", "fog_form_hour", SETTING_FLOAT, render.fog_form_hour),
	FIELD("render", "fog_clear_hour", SETTING_FLOAT, render.fog_clear_hour),
	FIELD("render", "fog_distance_meters", SETTING_FLOAT, render.fog_distance_meters),
	FIELD("render", "fog_always", SETTING_BOOL, render.fog_always),
	FIELD("render", "fireflies", SETTING_FLOAT, render.fireflies),
	FIELD("render", "water_depth_test", SETTING_BOOL, render.water_depth_test),
	FIELD("render", "debug_far_shadow_off", SETTING_BOOL, render.debug_far_shadow_off),
	FIELD("render", "debug_near_shadow_off", SETTING_BOOL, render.debug_near_shadow_off),
	FIELD("render", "debug_far_shadow_no_plants", SETTING_BOOL, render.debug_far_shadow_no_plants),
	FIELD("render", "debug_grass_freeze", SETTING_BOOL, render.debug_grass_freeze),
	FIELD("render", "gpu_subgroups", SETTING_BOOL, render.gpu_subgroups),
	FIELD("render", "debug_water_ripples", SETTING_INT, render.debug_water_ripples),
	FIELD("render", "water_depth_write", SETTING_BOOL, render.water_depth_write),
	FIELD("render", "water_cull_back_faces", SETTING_BOOL, render.water_cull_back_faces),
	FIELD("render", "underwater_visibility_meters", SETTING_FLOAT, render.underwater_visibility_meters),
	FIELD("render", "underwater_brightness", SETTING_FLOAT, render.underwater_brightness),

	FIELD("tools", "brush_radius_meters", SETTING_FLOAT, tools.brush_radius_meters),
	FIELD("tools", "brush_minimum_radius_meters", SETTING_FLOAT, tools.brush_minimum_radius_meters),
	FIELD("tools", "brush_maximum_radius_meters", SETTING_FLOAT, tools.brush_maximum_radius_meters),
	FIELD("tools", "interact_meters", SETTING_FLOAT, tools.interact_meters),
	FIELD("tools", "interact_highlight", SETTING_BOOL, tools.interact_highlight),
	FIELD("tools", "raise_meters_per_second", SETTING_FLOAT, tools.raise_meters_per_second),
	FIELD("tools", "smooth_strength_per_second", SETTING_FLOAT, tools.smooth_strength_per_second),
	FIELD("tools", "water_meters_per_second", SETTING_FLOAT, tools.water_meters_per_second),
	FIELD("tools", "stamp_height_meters", SETTING_FLOAT, tools.stamp_height_meters),

	FIELD("water", "gravity", SETTING_FLOAT, water.gravity),
	FIELD("water", "friction", SETTING_FLOAT, water.friction),
	FIELD("water", "manning_roughness", SETTING_FLOAT, water.manning_roughness),
	FIELD("water", "linear_damping", SETTING_FLOAT, water.linear_damping),
	FIELD("water", "sleep_speed_meters_per_second", SETTING_FLOAT, water.sleep_speed_meters_per_second),
	FIELD("water", "sleep_change_millimeters_per_second", SETTING_FLOAT, water.sleep_change_millimeters_per_second),
	FIELD("water", "border_wake_millimeters", SETTING_FLOAT, water.border_wake_millimeters),
	FIELD("water", "sleep_after_substeps", SETTING_INT, water.sleep_after_substeps),
	FIELD("water", "asleep_wake_millimeters_per_second", SETTING_FLOAT, water.asleep_wake_millimeters_per_second),
	FIELD("water", "asleep_level_wake_millimeters", SETTING_FLOAT, water.asleep_level_wake_millimeters),
	FIELD("water", "freeze_after_seconds", SETTING_FLOAT, water.freeze_after_seconds),
	FIELD("water", "film_millimeters", SETTING_FLOAT, water.film_millimeters),
	FIELD("water", "film_evaporation_millimeters_per_second", SETTING_FLOAT, water.film_evaporation_millimeters_per_second),
	FIELD("water", "meniscus_millimeters", SETTING_FLOAT, water.meniscus_millimeters),
	FIELD("water", "lip_millimeters", SETTING_FLOAT, water.lip_millimeters),
	FIELD("water", "drain_edges", SETTING_BOOL, water.drain_edges),
	FIELD("water", "depth_scaled_pipes", SETTING_BOOL, water.depth_scaled_pipes),
	FIELD("water", "pipe_depth_limit_meters", SETTING_FLOAT, water.pipe_depth_limit_meters),
	FIELD("water", "maximum_substeps", SETTING_INT, water.maximum_substeps),
	FIELD("water", "substep_speed_meters_per_second", SETTING_FLOAT, water.substep_speed_meters_per_second),
	FIELD("water", "thin_water_millimeters", SETTING_FLOAT, water.thin_water_millimeters),
	FIELD("water", "thin_water_superfluid", SETTING_BOOL, water.thin_water_superfluid),
	FIELD("water", "thin_water_infiltration_factor", SETTING_FLOAT, water.thin_water_infiltration_factor),
	FIELD("water", "still_water_speed_meters_per_second", SETTING_FLOAT, water.still_water_speed_meters_per_second),
	FIELD("water", "still_water_deposition_per_second", SETTING_FLOAT, water.still_water_deposition_per_second),
	FIELD("water", "erosion_minimum_depth_millimeters", SETTING_FLOAT, water.erosion_minimum_depth_millimeters),
	FIELD("water", "thin_water_evaporation_millimeters_per_second", SETTING_FLOAT, water.thin_water_evaporation_millimeters_per_second),
	FIELD("water", "still_level_millimeters", SETTING_FLOAT, water.still_level_millimeters),
	FIELD("water", "shore_damping_depth_meters", SETTING_FLOAT, water.shore_damping_depth_meters),
	FIELD("water", "shore_damping_per_second", SETTING_FLOAT, water.shore_damping_per_second),
	FIELD("water", "rain_millimeters_per_second", SETTING_FLOAT, water.rain_millimeters_per_second),
	FIELD("water", "spring_cubic_meters_per_second", SETTING_FLOAT, water.spring_cubic_meters_per_second),

	FIELD("simulation", "steps_per_second", SETTING_INT, simulation.steps_per_second),
	FIELD("simulation", "maximum_steps_per_frame", SETTING_INT, simulation.maximum_steps_per_frame),
	FIELD("simulation", "maximum_milliseconds_per_frame", SETTING_FLOAT, simulation.maximum_milliseconds_per_frame),
	FIELD("simulation", "pause_in_background", SETTING_BOOL, simulation.pause_in_background),
	FIELD("simulation", "threads", SETTING_INT, simulation.threads),
	FIELD("simulation", "path_search_cells", SETTING_INT, simulation.path_search_cells),
	FIELD("simulation", "navigation_object_meters", SETTING_FLOAT, simulation.navigation_object_meters),
	FIELD("simulation", "navigation_wall_meters", SETTING_FLOAT, simulation.navigation_wall_meters),
	FIELD("simulation", "navigation_soft_kilograms", SETTING_FLOAT, simulation.navigation_soft_kilograms),
	FIELD("simulation", "navigation_small_out", SETTING_BOOL, simulation.navigation_small_out),
	FIELD("simulation", "navigation_follow_mesh", SETTING_BOOL, simulation.navigation_follow_mesh),
	FIELD("simulation", "navigation_detail_meters", SETTING_FLOAT, simulation.navigation_detail_meters),
	FIELD("simulation", "small_object_detour_meters", SETTING_FLOAT, simulation.small_object_detour_meters),
	FIELD("simulation", "path_slope_degrees", SETTING_FLOAT, simulation.path_slope_degrees),
	FIELD("simulation", "path_slope_cost", SETTING_FLOAT, simulation.path_slope_cost),
	FIELD("simulation", "background_frames_per_second", SETTING_FLOAT, simulation.background_frames_per_second),
	FIELD("simulation", "diagnostics_interval_seconds", SETTING_FLOAT, simulation.diagnostics_interval_seconds),

	FIELD("objects", "populate", SETTING_BOOL, objects.populate),
	FIELD("objects", "density_scale", SETTING_FLOAT, objects.density_scale),
	FIELD("objects", "draw_distance_scale", SETTING_FLOAT, objects.draw_distance_scale),
	FIELD("objects", "impostors", SETTING_BOOL, objects.impostors),
	FIELD("objects", "impostor_pixels", SETTING_FLOAT, objects.impostor_pixels),
	FIELD("objects", "lod", SETTING_BOOL, objects.lod),
	FIELD("objects", "lod_distance_scale", SETTING_FLOAT, objects.lod_distance_scale),
	FIELD("objects", "lod_foliage_kept", SETTING_FLOAT, objects.lod_foliage_kept),
	FIELD("objects", "prop_lod_pixels", SETTING_FLOAT, objects.prop_lod_pixels),
	FIELD("objects", "grass", SETTING_BOOL, objects.grass),
	FIELD("objects", "grass_distance_meters", SETTING_FLOAT, objects.grass_distance_meters),
	FIELD("objects", "grass_density", SETTING_FLOAT, objects.grass_density),
	FIELD("objects", "grass_height_meters", SETTING_FLOAT, objects.grass_height_meters),
	FIELD("objects", "grass_width_meters", SETTING_FLOAT, objects.grass_width_meters),
	FIELD("objects", "grass_tint", SETTING_COLOR, objects.grass_tint),
	FIELD("objects", "grass_dry_share", SETTING_FLOAT, objects.grass_dry_share),
	FIELD("objects", "grass_coarse_meters", SETTING_FLOAT, objects.grass_coarse_meters),
	FIELD("objects", "effects_meters", SETTING_FLOAT, objects.effects_meters),
	FIELD("objects", "grass_coarse_width", SETTING_FLOAT, objects.grass_coarse_width),
	FIELD("objects", "brush_fill_seconds", SETTING_FLOAT, objects.brush_fill_seconds),

	FIELD("physics", "enabled", SETTING_BOOL, physics.enabled),
	FIELD("physics", "threads", SETTING_INT, physics.threads),
	FIELD("physics", "substeps", SETTING_INT, physics.substeps),
	FIELD("physics", "drag_coefficient", SETTING_FLOAT, physics.drag_coefficient),
	FIELD("physics", "hit_seconds", SETTING_FLOAT, physics.hit_seconds),
	FIELD("physics", "wake_flow_meters_per_second", SETTING_FLOAT, physics.wake_flow_meters_per_second),
	FIELD("physics", "terrain_resync_meters", SETTING_FLOAT, physics.terrain_resync_meters),
	FIELD("physics", "small_body_meters", SETTING_FLOAT, physics.small_body_meters),
	FIELD("physics", "terrain_check_seconds", SETTING_FLOAT, physics.terrain_check_seconds),
	FIELD("physics", "flood_check_seconds", SETTING_FLOAT, physics.flood_check_seconds),
	FIELD("physics", "float_damping_per_second", SETTING_FLOAT, physics.float_damping_per_second),
	FIELD("physics", "water_minimum_meters", SETTING_FLOAT, physics.water_minimum_meters),
};

#undef FIELD

// The cloud types' values
typedef struct
{
	float coverage;
	float softness;
	float tiling_meters;      // of the cover (the clouds' spacing and breadth)
	float height_meters;
	float thickness_meters;
	float density;
	float size;
	float towers;
	float base_darkness;
	float shadow_strength;
} cloud_type_values_t;

static const cloud_type_values_t cloud_types[CLOUD_TYPE_COUNT] =
{
	[CLOUD_TYPE_SMALL] = { 0.42f, 0.10f, 900.0f, 800.0f, 300.0f, 1.0f, 1.0f, 0.0f, 0.1f, 0.45f },
	[CLOUD_TYPE_MEDIUM] = { 0.45f, 0.12f, 1800.0f, 700.0f, 900.0f, 1.0f, 1.5f, 0.5f, 0.25f, 0.55f },
	[CLOUD_TYPE_STORM] = { 0.68f, 0.15f, 4000.0f, 500.0f, 2500.0f, 1.5f, 1.5f, 1.0f, 0.6f, 0.8f },
	[CLOUD_TYPE_OVERCAST] = { 1.0f, 0.40f, 3000.0f, 500.0f, 400.0f, 1.2f, 2.0f, 0.0f, 0.5f, 0.7f },
};

void settings_apply_cloud_type(render_settings_t *render)
{
	const cloud_type_values_t *values;

	if (render->cloud_type <= CLOUD_TYPE_CUSTOM || render->cloud_type >= CLOUD_TYPE_COUNT)
	{
		return;
	}
	values = &cloud_types[render->cloud_type];
	render->cloud_coverage = values->coverage;
	render->cloud_softness = values->softness;
	render->cloud_tiling_meters = values->tiling_meters;
	render->cloud_height_meters = values->height_meters;
	render->cloud_thickness_meters = values->thickness_meters;
	render->cloud_density = values->density;
	render->cloud_size = values->size;
	render->cloud_towers = values->towers;
	render->cloud_base_darkness = values->base_darkness;
	render->cloud_shadow_strength = values->shadow_strength;
}

// A cloud type whose values were changed (in an ini file) is custom
static void reconcile_cloud_type(render_settings_t *render)
{
	render_settings_t typed = *render;

	settings_apply_cloud_type(&typed);
	if (typed.cloud_coverage != render->cloud_coverage || typed.cloud_softness != render->cloud_softness
		|| typed.cloud_tiling_meters != render->cloud_tiling_meters || typed.cloud_height_meters != render->cloud_height_meters
		|| typed.cloud_thickness_meters != render->cloud_thickness_meters || typed.cloud_density != render->cloud_density
		|| typed.cloud_size != render->cloud_size || typed.cloud_towers != render->cloud_towers
		|| typed.cloud_base_darkness != render->cloud_base_darkness || typed.cloud_shadow_strength != render->cloud_shadow_strength)
	{
		render->cloud_type = CLOUD_TYPE_CUSTOM;
	}
}

bool settings_is_cloud_type_key(const char *key)
{
	static const char *const keys[] =
	{
		"cloud_coverage", "cloud_softness", "cloud_tiling_meters", "cloud_height_meters", "cloud_thickness_meters", "cloud_density",
		"cloud_size", "cloud_towers", "cloud_base_darkness", "cloud_shadow_strength",
	};

	for (size_t i = 0; i < SDL_arraysize(keys); i++)
	{
		if (SDL_strcmp(key, keys[i]) == 0)
		{
			return true;
		}
	}
	return false;
}

void settings_set_defaults(settings_t *settings)
{
	SDL_zerop(settings);

	settings->terrain.size = 1024;
	settings->terrain.seed = 0;
	settings->terrain.cell_size_meters = 1.0f;
	settings->terrain.max_height_meters = 80.0f;
	settings->terrain.feature_count = 3.0f;
	settings->terrain.minimum_feature_meters = 8.0f;
	settings->terrain.persistence = 0.45f;
	settings->terrain.warp_amount = 0.35f;
	settings->terrain.mountain_amount = 0.7f;
	settings->terrain.mountain_coverage = 0.45f;
	settings->terrain.thermal_iterations = 60;
	settings->terrain.talus_slope = 0.8f;
	settings->terrain.gully_depth_meters = 3.0f;
	settings->terrain.mountain_soil_pockets = 0.5f;
	settings->terrain.rock_slope = 0.85f;
	settings->terrain.rock_height_fraction = 0.78f;
	settings->terrain.sand_height_fraction = 0.04f;
	settings->terrain.clay_height_fraction = 0.15f;

	settings->cache.use_cache = true;
	settings->cache.cache_maximum_files = 8;

	settings->camera.field_of_view_degrees = 32.0f;
	settings->camera.minimum_pitch_degrees = 0.0f;
	settings->camera.maximum_pitch_degrees = 80.0f;
	settings->camera.default_pitch_degrees = 45.0f;
	settings->camera.minimum_distance_meters = 5.0f;
	settings->camera.maximum_distance_meters = 2500.0f;
	settings->camera.rotate_degrees_per_pixel = 0.3f;
	settings->camera.keyboard_pan_speed = 0.6f;
	settings->camera.zoom_step_factor = 1.15f;
	settings->camera.first_person_field_of_view_degrees = 70.0f;
	settings->camera.first_person_near_meters = 0.1f;
	settings->camera.jump_height_scale = 1.0f;
	settings->camera.first_person_look_degrees_per_pixel = 0.12f;
	settings->camera.first_person_invert_mouse = false;
	settings->camera.walk_meters_per_second = 4.0f;
	settings->camera.run_meters_per_second = 10.0f;

	settings->render.vsync = true;
	settings->render.max_fps = 0.0f;
	settings->render.sky_color[0] = 0.62f;
	settings->render.sky_color[1] = 0.74f;
	settings->render.sky_color[2] = 0.86f;
	settings->render.sky_zenith_color[0] = 0.30f;
	settings->render.sky_zenith_color[1] = 0.50f;
	settings->render.sky_zenith_color[2] = 0.85f;
	settings->render.fog_density = 0.0004f;
	settings->render.fog_height_meters = 1000.0f;
	settings->render.pbr_shading = true;
	settings->render.sun_azimuth_degrees = 60.0f;
	settings->render.sun_elevation_degrees = 38.0f;
	settings->render.sun_color[0] = 1.0f;
	settings->render.sun_color[1] = 0.95f;
	settings->render.sun_color[2] = 0.85f;
	settings->render.sun_intensity = 1.0f;
	settings->render.ambient_intensity = 0.35f;
	settings->render.sky_occlusion = true;
	settings->render.sky_occlusion_intensity = 1.0f;
	settings->render.bounce_light = 1.0f;
	settings->render.sky_occlusion_least_height = 1.5f;
	settings->render.sky_occlusion_directions = 64;
	settings->render.sky_occlusion_texel_meters = 0.5f;
	settings->render.sky_occlusion_lowest_degrees = 5.0f;
	settings->render.sky_occlusion_directions_per_frame = 4;
	settings->render.texture_size = 512;
	settings->render.anisotropy = 8.0f;
	settings->render.normal_strength = 2.0f;
	settings->render.snow_height_fraction = 0.82f;
	settings->render.snow_aspect_shift = 0.12f;
	settings->render.detail_fade_start_meters = 80.0f;
	settings->render.detail_fade_end_meters = 400.0f;
	settings->render.shadow_map_size = 4096;
	settings->render.shadow_strength = 0.85f;
	settings->render.near_shadow_map_size = 4096;
	settings->render.near_shadow_meters = 100.0f;
	settings->render.contact_shadow_meters = 0.4f;
	settings->render.contact_shadow_strength = 1.0f;
	settings->render.ambient_occlusion = true;
	settings->render.ambient_occlusion_meters = 1.0f;
	settings->render.ambient_occlusion_strength = 1.0f;
	settings->render.ambient_occlusion_distance = 150.0f;
	settings->render.water_shallow_color[0] = 0.30f;
	settings->render.water_shallow_color[1] = 0.55f;
	settings->render.water_shallow_color[2] = 0.55f;
	settings->render.water_deep_color[0] = 0.05f;
	settings->render.water_deep_color[1] = 0.17f;
	settings->render.water_deep_color[2] = 0.25f;
	settings->render.water_clarity_meters = 3.0f;
	settings->render.water_reflections = true;
	settings->render.water_refraction = 1.0f;
	settings->render.water_choppiness = 1.0f;
	settings->render.water_reflections_half_size = false;
	settings->render.puddles = true;
	settings->render.gpu_subgroups = true;
	settings->render.water_reflection_meters = 250.0f;
	settings->render.terrain_from_gpu = true;
	settings->render.material_map = true;
	settings->render.ground_overlay = true;
	settings->render.water_ice = true;
	settings->render.moss_shells = 8;
	settings->render.moss_shell_meters = 0.02f;
	settings->render.moss_shell_distance_meters = 12.0f;
	settings->render.water_opacity_per_meter = 1.2f;
	settings->render.foam_speed_meters_per_second = 3.0f;
	settings->render.soil_blend_meters = 4.0f;
	settings->render.material_normal_meters = 1.5f;
	settings->render.grass_slope_start = 0.18f;
	settings->render.grass_slope_end = 0.45f;
	settings->render.cliff_slope_start = 0.42f;
	settings->render.cliff_slope_end = 0.75f;
	settings->render.variation_tiling_meters = 180.0f;
	settings->render.variation_strength = 0.35f;
	settings->render.variation_hue_strength = 0.25f;
	settings->render.volumetric_clouds = true;
	settings->render.cloud_steps = 64;
	settings->render.cloud_dome_size = 0;
	settings->render.cloud_refresh_seconds = 0.1f;
	settings->render.cloud_memory_seconds = 0.3f;
	settings->render.cloud_density_variation = 0.5f;
	settings->render.cloud_lifetime_minutes = 15.0f;
	settings->render.cloud_horizon_drop_meters = 1000.0f;
	settings->render.flock_size_meters = 400.0f;
	settings->render.flock_amount = 0.5f;
	settings->render.flock_trend = 0.0f;
	settings->render.flock_height_meters = 4500.0f;
	settings->render.flock_density = 0.5f;
	settings->render.flock_shade = 0.3f;
	settings->render.cloud_type = CLOUD_TYPE_MEDIUM;
	settings_apply_cloud_type(&settings->render);
	settings->render.wind_direction_degrees = 30.0f;
	settings->render.wind_speed_meters_per_second = 3.0f;
	settings->render.shadow_update_interval_seconds = 0.5f;
	settings->render.far_shadow_least_meters = 1.5f;
	settings->render.actor_shadow_update_seconds = 0.0f;
	settings->render.shadow_erosion_meters = 0.15f;
	settings->render.lod_distance_meters = 80.0f;
	settings->render.far_terrain_upload_seconds = 1.0f;
	settings->render.far_water_upload_seconds = 0.2f;
	settings->render.far_upload_meters = 300.0f;
	settings->objects.populate = true;
	settings->objects.density_scale = 1.0f;
	settings->objects.draw_distance_scale = 1.0f;
	settings->objects.impostors = true;
	settings->objects.impostor_pixels = 64.0f;
	settings->objects.lod = true;
	settings->objects.lod_distance_scale = 1.0f;
	settings->objects.lod_foliage_kept = 1.0f;
	settings->objects.prop_lod_pixels = 0.5f;
	settings->objects.grass = true;
	settings->objects.grass_distance_meters = 50.0f;
	settings->objects.grass_density = 550.0f;
	settings->objects.grass_height_meters = 0.3f;
	settings->objects.grass_width_meters = 0.018f;
	settings->objects.grass_tint[0] = 1.0f;
	settings->objects.grass_tint[1] = 1.0f;
	settings->objects.grass_tint[2] = 1.0f;
	settings->objects.grass_dry_share = 0.12f;
	settings->objects.grass_coarse_meters = 18.0f;
	settings->objects.effects_meters = 150.0f;
	settings->objects.grass_coarse_width = 1.3f;
	settings->objects.brush_fill_seconds = 0.6f;
	settings->render.render_scale = 1.0f;
	settings->render.taa = true;
	settings->render.msaa = 1;
	settings->render.taa_when_moving = true;
	settings->render.taa_history_weight = 0.9f;
	settings->render.taa_sharpness = 1.0f;
	settings->render.taa_mip_bias = -0.75f;
	settings->render.day_cycle = true;
	settings->render.day_length_minutes = 240.0f;
	settings->render.daylight_hours = 14.0f;
	settings->render.moon_cycle_days = 29.5f;
	settings->render.golden_hour = 1.0f;
	settings->render.blue_hour = 1.0f;
	settings->render.blue_hour_on = true;
	settings->render.moon_size_degrees = 1.5f;
	settings->render.aurora_strength = 1.0f;
	settings->render.rain_intensity = 0.6f;
	settings->render.rain_streak_meters = 0.45f;
	settings->render.rain_splashes = 0.7f;
	settings->render.rainbow_strength = 1.0f;
	settings->render.plant_wind_strength = 0.35f;
	settings->render.plant_wind_direction_degrees = 60.0f;
	settings->render.plant_wind_gust_meters = 30.0f;
	settings->render.grass_gust_sheen = 1.0f;
	settings->render.month = 7.5f;
	settings->render.snow_first_month = 12.0f;
	settings->render.snow_last_month = 2.0f;
	settings->render.tips_first_month = 4.0f;
	settings->render.tips_last_month = 9.0f;
	settings->render.bloom_first_month = 4.0f;
	settings->render.bloom_last_month = 5.0f;
	settings->render.hdr = true;
	settings->render.auto_exposure = true;
	settings->render.exposure_ev = 0.0f;
	settings->render.exposure_adapt_seconds = 1.5f;
	settings->render.bloom = true;
	settings->render.bloom_strength = 0.05f;
	settings->render.tone_mapper = 1;
	settings->render.color_grade = 0;
	settings->render.color_grade_strength = 1.0f;
	settings->render.night_vision = 0.7f;
	settings->render.night_vision_luminance = 0.05f;
	settings->render.local_lights = true;
	settings->render.decals = true;
	settings->render.roof_lightness = 0.2f;
	settings->render.roof_fade = 0.35f;
	settings->render.roof_warmth = 0.05f;
	settings->render.roof_patches = 0.55f;
	settings->render.roof_runs = 0.35f;
	settings->render.roof_mended = 0.9f;
	settings->render.wood_dulled = 0.6f;
	settings->render.local_light_intensity = 1.0f;
	settings->render.window_lights = true;
	settings->render.window_light_intensity = 0.3f;
	settings->render.lamps_with_the_day = true;
	settings->render.fog_mode = 2;
	settings->render.fog_quality = 1;
	settings->render.fog_valley_density = 0.08f;
	settings->render.fog_valley_height_meters = 30.0f;
	settings->render.fog_valley_reach_meters = 250.0f;
	settings->render.fog_softness_meters = 6.0f;
	settings->render.fog_glow = 0.6f;
	settings->render.fog_shafts = 1.0f;
	settings->render.fog_night_brightness = 0.3f;
	settings->render.fog_rays = 1.0f;
	settings->render.fog_form_hour = 19.0f;
	settings->render.fog_clear_hour = 10.0f;
	settings->render.fog_distance_meters = 600.0f;
	settings->render.fog_always = false;
	settings->render.fireflies = 0.5f;
	settings->render.water_depth_test = true;
	settings->render.water_depth_write = false;
	settings->render.water_cull_back_faces = true;
	settings->render.underwater_visibility_meters = 60.0f;
	settings->render.underwater_brightness = 1.5f;

	settings->tools.brush_radius_meters = 12.0f;
	settings->tools.brush_minimum_radius_meters = 2.0f;
	settings->tools.brush_maximum_radius_meters = 80.0f;
	settings->tools.interact_meters = 6.0f;
	settings->tools.interact_highlight = false;
	settings->tools.raise_meters_per_second = 6.0f;
	settings->tools.smooth_strength_per_second = 3.0f;
	settings->tools.water_meters_per_second = 2.0f;
	settings->tools.stamp_height_meters = 4.0f;

	settings->water.gravity = 9.81f;
	settings->water.friction = 0.02f;
	settings->water.manning_roughness = 0.035f;
	settings->water.linear_damping = 0.05f;
	settings->water.sleep_speed_meters_per_second = 0.05f;
	settings->water.sleep_change_millimeters_per_second = 4.0f;
	settings->water.border_wake_millimeters = 0.15f;
	settings->water.sleep_after_substeps = 10;
	settings->water.asleep_wake_millimeters_per_second = 30.0f;
	settings->water.asleep_level_wake_millimeters = 20.0f;
	settings->water.freeze_after_seconds = 30.0f;
	settings->water.film_millimeters = 1.0f;
	settings->water.film_evaporation_millimeters_per_second = 0.02f;
	settings->water.meniscus_millimeters = 3.0f;
	settings->water.lip_millimeters = 20.0f;
	settings->water.drain_edges = true;
	settings->water.depth_scaled_pipes = true;
	settings->water.pipe_depth_limit_meters = 0.02f;
	settings->water.maximum_substeps = 8;
	settings->water.substep_speed_meters_per_second = 10.0f;
	settings->water.thin_water_millimeters = 30.0f;
	settings->water.thin_water_superfluid = true;
	settings->water.thin_water_infiltration_factor = 4.0f;
	settings->water.still_water_speed_meters_per_second = 0.05f;
	settings->water.still_water_deposition_per_second = 2.0f;
	settings->water.erosion_minimum_depth_millimeters = 20.0f;
	settings->water.thin_water_evaporation_millimeters_per_second = 1.0f;
	settings->water.still_level_millimeters = 5.0f;
	settings->water.shore_damping_depth_meters = 0.2f;
	settings->water.shore_damping_per_second = 2.0f;
	settings->water.rain_millimeters_per_second = 0.5f;
	settings->water.spring_cubic_meters_per_second = 2.0f;

	settings->simulation.steps_per_second = 30;
	settings->simulation.maximum_steps_per_frame = 16;
	settings->simulation.maximum_milliseconds_per_frame = 25.0f;
	settings->simulation.pause_in_background = true;
	settings->simulation.background_frames_per_second = 10.0f;
	settings->simulation.diagnostics_interval_seconds = 30.0f;
	settings->simulation.path_search_cells = 100000;
	settings->simulation.navigation_object_meters = 0.3f;
	settings->simulation.navigation_wall_meters = 1.2f;
	settings->simulation.navigation_soft_kilograms = 20.0f;
	settings->simulation.navigation_small_out = true;
	settings->simulation.navigation_follow_mesh = true;
	settings->simulation.navigation_detail_meters = 250.0f;
	settings->simulation.small_object_detour_meters = 10.0f;
	settings->simulation.path_slope_degrees = 40.0f;
	settings->simulation.path_slope_cost = 1.5f;

	settings->physics.enabled = true;
	settings->physics.threads = 2;
	settings->physics.substeps = 4;
	settings->physics.drag_coefficient = 1.0f;
	settings->physics.hit_seconds = 0.1f;
	settings->physics.wake_flow_meters_per_second = 0.15f;
	settings->physics.terrain_resync_meters = 0.3f;
	settings->physics.small_body_meters = 1.0f;
	settings->physics.terrain_check_seconds = 2.0f;
	settings->physics.flood_check_seconds = 0.25f;
	settings->physics.float_damping_per_second = 2.0f;
	settings->physics.water_minimum_meters = 0.03f;
}

// What the file being read said about the clouds (see after_parse)
static bool parse_saw_cloud_type;
static bool parse_saw_cloud_value;

static bool settings_key_callback(void *user_data, const char *section, const char *key, const char *value)
{
	settings_t *settings = user_data;
	size_t field_count = sizeof(setting_fields) / sizeof(setting_fields[0]);

	if (SDL_strcmp(section, "render") == 0)
	{
		parse_saw_cloud_type |= SDL_strcmp(key, "cloud_type") == 0;
		parse_saw_cloud_value |= settings_is_cloud_type_key(key);
	}

	for (size_t i = 0; i < field_count; i++)
	{
		const setting_field_t *field = &setting_fields[i];
		void *target = (char *)settings + field->offset;

		if (SDL_strcmp(field->section, section) != 0 || SDL_strcmp(field->key, key) != 0)
		{
			continue;
		}

		switch (field->type)
		{
			case SETTING_FLOAT:
				return ini_value_to_float(value, target);
			case SETTING_INT:
				return ini_value_to_int(value, target);
			case SETTING_UINT:
			{
				int parsed;

				if (!ini_value_to_int(value, &parsed))
				{
					return false;
				}
				*(uint32_t *)target = (uint32_t)parsed;
				return true;
			}
			case SETTING_BOOL:
				return ini_value_to_bool(value, target);
			case SETTING_COLOR:
				return ini_value_to_color(value, target);
		}
	}
	return false;
}

// A file that names a cloud type and none of its values gets the type's
// values; one that changes the values of a type makes it custom
static void after_parse(settings_t *settings)
{
	if (parse_saw_cloud_type && !parse_saw_cloud_value)
	{
		settings_apply_cloud_type(&settings->render);
	}
	else if (parse_saw_cloud_value)
	{
		reconcile_cloud_type(&settings->render);
	}
	parse_saw_cloud_type = false;
	parse_saw_cloud_value = false;
}

void settings_load(settings_t *settings)
{
	ini_parse_data_file("settings.ini", settings_key_callback, settings);
	after_parse(settings);
}

#define SETTINGS_OVERRIDE_FILE "settings_changed.ini"

void *settings_find(settings_t *settings, const char *section, const char *key, setting_type_t *type_out)
{
	size_t field_count = sizeof(setting_fields) / sizeof(setting_fields[0]);

	for (size_t i = 0; i < field_count; i++)
	{
		if (SDL_strcmp(setting_fields[i].section, section) == 0 && SDL_strcmp(setting_fields[i].key, key) == 0)
		{
			if (type_out != NULL)
			{
				*type_out = setting_fields[i].type;
			}
			return (char *)settings + setting_fields[i].offset;
		}
	}
	return NULL;
}

void settings_load_overrides(settings_t *settings)
{
	char path[1024];
	char *text;

	if (!file_io_preference_path(SETTINGS_OVERRIDE_FILE, path, sizeof(path)))
	{
		return;
	}
	text = file_io_load_file(path, NULL);
	if (text == NULL)
	{
		return;
	}
	SDL_Log("Settings changed in the game: %s", path);
	ini_parse_buffer(text, SETTINGS_OVERRIDE_FILE, settings_key_callback, settings);
	after_parse(settings);
	SDL_free(text);
}

bool settings_set_from_text(settings_t *settings, const char *assignment)
{
	char section[64];
	char key[128];
	const char *dot = SDL_strchr(assignment, '.');
	const char *equals = SDL_strchr(assignment, '=');
	bool done;

	if (dot == NULL || equals == NULL || dot > equals || (size_t)(dot - assignment) >= sizeof(section)
		|| (size_t)(equals - dot - 1) >= sizeof(key))
	{
		return false;
	}
	SDL_strlcpy(section, assignment, (size_t)(dot - assignment) + 1);
	SDL_strlcpy(key, dot + 1, (size_t)(equals - dot - 1) + 1);
	done = settings_key_callback(settings, section, key, equals + 1);
	after_parse(settings);
	return done;
}

// The value as the ini file writes it
static void format_value(const setting_field_t *field, const void *value, char *text_out, size_t text_out_size)
{
	switch (field->type)
	{
		case SETTING_FLOAT:
			SDL_snprintf(text_out, text_out_size, "%g", (double)*(const float *)value);
			break;
		case SETTING_INT:
			SDL_snprintf(text_out, text_out_size, "%d", *(const int32_t *)value);
			break;
		case SETTING_UINT:
			SDL_snprintf(text_out, text_out_size, "%u", *(const uint32_t *)value);
			break;
		case SETTING_BOOL:
			SDL_strlcpy(text_out, *(const bool *)value ? "true" : "false", text_out_size);
			break;
		case SETTING_COLOR:
		{
			const float *color = value;

			SDL_snprintf(text_out, text_out_size, "%d, %d, %d", (int)SDL_lroundf(color[0] * 255.0f), (int)SDL_lroundf(color[1] * 255.0f),
				(int)SDL_lroundf(color[2] * 255.0f));
			break;
		}
	}
}

size_t settings_field_count(void)
{
	return sizeof(setting_fields) / sizeof(setting_fields[0]);
}

bool settings_field_name(size_t index, const char **section_out, const char **key_out)
{
	if (index >= settings_field_count())
	{
		return false;
	}
	*section_out = setting_fields[index].section;
	*key_out = setting_fields[index].key;
	return true;
}

bool settings_get_text(const settings_t *settings, const char *section, const char *key, char *text_out, size_t text_out_size)
{
	for (size_t i = 0; i < settings_field_count(); i++)
	{
		const setting_field_t *field = &setting_fields[i];

		if (SDL_strcmp(field->section, section) == 0 && SDL_strcmp(field->key, key) == 0)
		{
			format_value(field, (const char *)settings + field->offset, text_out, text_out_size);
			return true;
		}
	}
	return false;
}

bool settings_set_text(settings_t *settings, const char *section, const char *key, const char *value)
{
	bool ok = settings_key_callback(settings, section, key, value);

	parse_saw_cloud_type = false;
	parse_saw_cloud_value = false;
	if (ok && SDL_strcmp(section, "render") == 0)
	{
		// As the settings window does: a cloud type brings its values, a
		// changed value makes the type custom
		if (SDL_strcmp(key, "cloud_type") == 0)
		{
			settings_apply_cloud_type(&settings->render);
		}
		else if (settings_is_cloud_type_key(key))
		{
			reconcile_cloud_type(&settings->render);
		}
	}
	return ok;
}

bool settings_save_overrides(const settings_t *settings, const settings_t *file_settings)
{
	size_t field_count = sizeof(setting_fields) / sizeof(setting_fields[0]);
	char path[1024];
	char *text;
	size_t length = 0;
	size_t capacity = 65536;
	const char *section = "";
	int written = 0;
	bool saved;

	if (!file_io_preference_path(SETTINGS_OVERRIDE_FILE, path, sizeof(path)))
	{
		return false;
	}
	text = SDL_malloc(capacity);
	if (text == NULL)
	{
		return false;
	}
	length += (size_t)SDL_snprintf(text + length, capacity - length,
		"; Settings changed in the game, on top of data/settings.ini. Delete this\n; file to go back to the values there.\n");
	for (size_t i = 0; i < field_count; i++)
	{
		const setting_field_t *field = &setting_fields[i];
		char mine[64];
		char theirs[64];

		// Compared as written, so a color that rounds to the same bytes or a
		// float printed the same does not count as changed
		format_value(field, (const char *)settings + field->offset, mine, sizeof(mine));
		format_value(field, (const char *)file_settings + field->offset, theirs, sizeof(theirs));
		if (SDL_strcmp(mine, theirs) == 0)
		{
			continue;
		}
		if (SDL_strcmp(section, field->section) != 0)
		{
			section = field->section;
			length += (size_t)SDL_snprintf(text + length, capacity - length, "\n[%s]\n", section);
		}
		length += (size_t)SDL_snprintf(text + length, capacity - length, "%s = %s\n", field->key, mine);
		written++;
	}
	length = SDL_min(length, capacity - 1);
	if (written == 0)
	{
		// Nothing differs: no file at all
		SDL_RemovePath(path);
		saved = true;
	}
	else
	{
		saved = file_io_save_file(path, text, length);
	}
	SDL_free(text);
	if (saved)
	{
		SDL_Log("Saved %d changed settings to %s", written, path);
	}
	return saved;
}
