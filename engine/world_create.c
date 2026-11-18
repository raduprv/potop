// A world made and let go (world.h: world_init, world_create_land,
// world_create, world_populate, world_destroy)

#include "world.h"

#include <SDL3/SDL.h>

#include "material_map.h"
#include "object_model.h"
#include "populate.h"
#include "profile.h"
#include "water_gpu.h"

// Particles the GPU has room for (about 30 MB)
static const int WORLD_PARTICLE_CAPACITY = 262144;

static void world_progress(world_t *world, const char *stage, float fraction)
{
	if (world->progress != NULL)
	{
		world->progress(world->progress_user, stage, fraction);
	}
}

void world_init(world_t *world, settings_t *settings, const soil_settings_t *soils)
{
	world->settings = settings;
	world->soils = soils;
	world->sky_time_scale = 1.0f;
	// (no load yet: no frames after one to time)
	world->frames_since_load = 4;
	// The sky as the settings make it (a loaded map brings its own)
	cloud_paint_reset(&world->cloud_paint);
	world->harvest_random = 0x68e31da4u;
	// Every map starts in the morning (the hour is not saved with it)
	world->day_hour = 10.0;
	// A full moon the first night
	world->moon_age_days = 0.5 * (double)settings->render.moon_cycle_days - 0.4;
	daylight_apply(&settings->render, world->day_hour, world->moon_age_days, &world->lit_render, &world->daylight);
}

bool world_create_land(world_t *world, int32_t size, float cell_size_meters, bool baked)
{
	if (baked ? !terrain_create_baked(&world->terrain, size, cell_size_meters) : !terrain_create(&world->terrain, size, cell_size_meters))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory creating the terrain");
		return false;
	}
	if (baked ? !water_create_baked(&world->water, world->terrain.size, world->terrain.cell_size_meters)
		: !water_create(&world->water, world->terrain.size, world->terrain.cell_size_meters, world->settings->water.drain_edges,
			world->settings->water.depth_scaled_pipes))
	{
		return false;
	}
	return erosion_create(&world->erosion, &world->water);
}

bool world_create(world_t *world)
{
	world->renderers_created = true;
	world_progress(world, "Loading the ground's textures", 0.08f);
	if (!terrain_renderer_create(&world->renderer, &world->settings->render))
	{
		return false;
	}
	world_progress(world, "The sky, the water and the shadows", 0.66f);
	if (!sky_renderer_create(&world->sky_renderer))
	{
		return false;
	}
	if (!rain_create(&world->rain))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Rain: the drops and the rainbow cannot be drawn");
	}
	grass_create(&world->grass);
	ground_overlay_create(&world->ground_overlay);
	water_ice_create(&world->ice);
	if (!water_renderer_create(&world->water_renderer))
	{
		return false;
	}
	// Without them the game goes on (the shaders are logged)
	particles_create(&world->particles, WORLD_PARTICLE_CAPACITY);
	lights_create(&world->lights);
	decals_create(&world->decals);
	waterfalls_create(&world->waterfalls);
	sky_occlusion_create(&world->sky_occlusion);
	aurora_set(&world->aurora, false, 1.0f);
	fireflies_initialize(&world->fireflies);
	object_effects_initialize(&world->object_effects);
	ground_footprints_initialize(&world->ground_footprints);
	if (!shadow_map_create(&world->shadow, world->settings->render.shadow_map_size))
	{
		return false;
	}
	if (!shadow_map_create(&world->near_shadow, world->settings->render.shadow_map_size > 0 ? world->settings->render.near_shadow_map_size : 0))
	{
		return false;
	}
	gpu_timer_create(&world->gpu_timer);
	world_progress(world, "Loading the objects", 0.67f);
	trace_begin("Startup: the objects, step by step (ms)");
	if (!objects_create(&world->objects, &world->terrain, world->settings))
	{
		return false;
	}
	trace_end();
	world_progress(world, "The picture's effects", 0.94f);
	if (!post_create(&world->post))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No exposure, bloom or tone mapping: the scene is shown as it is drawn");
	}
	if (!hiz_create(&world->hiz))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No depth pyramid: objects hidden by the terrain are drawn anyway");
	}
	world->objects.occlusion = &world->hiz;
	world_progress(world, "People and paths", 0.95f);
	areas_clear(&world->areas);
	if (!actors_create(&world->actors, world->settings))
	{
		return false;
	}
	if (!navigation_create(&world->navigation, &world->terrain))
	{
		return false;
	}
	world->navigation.maximum_expansions = SDL_max(world->settings->simulation.path_search_cells, 1000);
	world->navigation.object_minimum_meters = world->settings->simulation.navigation_object_meters;
	world->navigation.wall_reach_meters = world->settings->simulation.navigation_wall_meters;
	world->navigation.soft_kilograms = world->settings->simulation.navigation_soft_kilograms;
	world->navigation.small_out = world->settings->simulation.navigation_small_out;
	world->navigation.small_detour_meters = world->settings->simulation.small_object_detour_meters;
	navigation_set_slope_cost(&world->navigation, world->settings->simulation.path_slope_degrees, world->settings->simulation.path_slope_cost);
	// Path finding tells the log what it does (debugging)
	world->actors.log_paths = true;
	world->actors.objects = &world->objects;
	world->actors.physics = &world->physics;
	world_progress(world, "Physics", 0.96f);
	if (!physics_create(&world->physics, &world->objects, &world->settings->physics))
	{
		return false;
	}
	layouts_load(&world->layouts, &world->objects.types);
	world->navigation_overlay_seconds = -1e9;
	return true;
}

void world_populate(world_t *world, bool plant)
{
	if (plant)
	{
		world_progress(world, "Planting the trees, building the villages", 0.96f);
		populate_map(&world->objects, &world->terrain, &world->water, &world->settings->objects);
		layouts_populate_map(&world->layouts, &world->objects, &world->terrain, &world->water, &world->settings->objects);
		material_map_restamp(&world->terrain, &world->objects);
	}
	world_progress(world, "Every object into the physics", 0.98f);
	physics_reset(&world->physics, &world->objects, &world->terrain);
	world_progress(world, "Finding the paths", 0.98f);
	// (each region built when a search or an actor first goes through it,
	// as on a loaded map: not all of them on the loading screen, 5 s on
	// a map of 4 km)
	navigation_forget(&world->navigation, &world->terrain, &world->objects);
}

void world_destroy(world_t *world)
{
	ground_footprints_destroy(&world->ground_footprints);
	if (world->renderers_created)
	{
		physics_destroy(&world->physics);
		actors_destroy(&world->actors);
		navigation_destroy(&world->navigation);
		SDL_free(world->navigation_overlay);
		objects_destroy(&world->objects);
		object_default_textures_release();
		hiz_destroy(&world->hiz);
		clouds_destroy(&world->clouds);
		cloud_paint_destroy(&world->cloud_paint);
		post_destroy(&world->post);
		render_target_destroy(&world->scene_target);
		sky_renderer_destroy(&world->sky_renderer);
		rain_destroy(&world->rain);
		fog_destroy(&world->fog);
		ambient_occlusion_destroy(&world->ambient_occlusion);
		puddles_destroy(&world->puddles);
		grass_destroy(&world->grass);
		ground_overlay_destroy(&world->ground_overlay);
		water_ice_destroy(&world->ice);
		gpu_timer_destroy(&world->gpu_timer);
		shadow_map_destroy(&world->near_shadow);
		shadow_map_destroy(&world->shadow);
		water_renderer_destroy(&world->water_renderer);
		waterfalls_destroy(&world->waterfalls, &world->particles);
		sky_occlusion_destroy(&world->sky_occlusion);
		particles_destroy(&world->particles);
		lights_destroy(&world->lights);
		decals_destroy(&world->decals);
		terrain_renderer_destroy(&world->renderer);
		world->renderers_created = false;
	}
	SDL_free(world->edit_scratch);
	world->edit_scratch = NULL;
	world->edit_scratch_capacity = 0;
	water_gpu_release(&world->water);
	erosion_destroy(&world->erosion);
	water_destroy(&world->water);
	terrain_destroy(&world->terrain);
}
