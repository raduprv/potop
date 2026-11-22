// The world following its settings (world.h: world_follow_settings,
// world_apply_render_settings, world_regrow)

#include "world.h"

#include <SDL3/SDL.h>

#include "material_map.h"
#include "populate.h"
#include "season.h"

// How far the light moves as the day goes before the shadows are drawn
// again (degrees)
#define DAY_SHADOW_STEP_DEGREES 0.2f

// The snow (update_snow_amount below, ground_overlay.comp): how fast it
// covers things at full snowfall, how fast it melts after the winter
// (things load slower than the ground, a minute: ground_overlay.comp's;
// the branches step by step through their winter pictures)
static const float SNOW_THINGS_FILL_SECONDS = 300.0f;
static const float SNOW_MELT_SECONDS = 240.0f;
// Through the winter it melts this much slower (some mild days), not at all
// never: snow comes and goes all winter
static const float SNOW_WINTER_MELT_SHARE = 0.15f;
// A map started in the winter, or a jump in the month: this much of it to
// start with, times how far into the winter (as from the snow before)
static const float SNOW_START_SHARE = 0.5f;

static void update_snow_amount(world_t *world)
{
	objects_t *objects = &world->objects;
	float month = objects->month;
	float month_jump = SDL_fabsf(month - world->snow_month);
	float winter = season_snow(month, objects->snow_first_month, objects->snow_last_month);
	float seconds = (float)SDL_clamp(world->elapsed_seconds - world->snow_seconds, 0.0, 5.0);
	float snowfall = rain_intensity(&world->rain, &world->settings->render) * SDL_min(4.0f * winter, 1.0f);

	if (!world->snow_started || SDL_min(month_jump, 12.0f - month_jump) > 0.25f)
	{
		objects->snow_amount = SNOW_START_SHARE * winter;
		world->snow_started = true;
	}
	else
	{
		objects->snow_amount += snowfall * seconds / SNOW_THINGS_FILL_SECONDS
			- (1.0f - (1.0f - SNOW_WINTER_MELT_SHARE) * winter) * seconds / SNOW_MELT_SECONDS;
		objects->snow_amount = SDL_clamp(objects->snow_amount, 0.0f, 1.0f);
	}
	world->snow_seconds = world->elapsed_seconds;
	world->snow_month = month;
	// (the branches sag under it: their shadows follow now and then)
	if (SDL_fabsf(objects->snow_amount - world->snow_shadowed) > 0.05f)
	{
		world->snow_shadowed = objects->snow_amount;
		objects->far_shadow_dirty = true;
		objects->near_shadow_dirty = true;
	}
}

// Settings the interface changes that are used outside the renderers' own
// per frame reads
void world_follow_settings(world_t *world)
{
	const render_settings_t *render = &world->settings->render;

	world->objects.draw_distance_scale = SDL_max(world->settings->objects.draw_distance_scale, 0.01f);
	world->objects.impostors_enabled = world->settings->objects.impostors;
	// (the plants' impostors made, or let go)
	if (world->objects.plant_impostors != world->settings->objects.plant_impostors)
	{
		world->objects.plant_impostors = world->settings->objects.plant_impostors;
		world->objects.impostors_pending = true;
	}
	world->objects.impostors_tinted = world->settings->objects.show_impostors;
	world->objects.lod_enabled = world->settings->objects.lod;
	// The month (season.h): the shadows follow the foliage and the snow
	{
		float month = season_month_wrapped(world->settings->render.month);
		float snow_first = SDL_clamp(SDL_roundf(world->settings->render.snow_first_month), 1.0f, 12.0f);
		float snow_last = SDL_clamp(SDL_roundf(world->settings->render.snow_last_month), 1.0f, 12.0f);

		world->objects.tips_first_month = SDL_clamp(SDL_roundf(world->settings->render.tips_first_month), 1.0f, 12.0f);
		world->objects.tips_last_month = SDL_clamp(SDL_roundf(world->settings->render.tips_last_month), 1.0f, 12.0f);
		if (world->objects.month != month || world->objects.snow_first_month != snow_first || world->objects.snow_last_month != snow_last)
		{
			world->objects.month = month;
			world->objects.snow_first_month = snow_first;
			world->objects.snow_last_month = snow_last;
			world->objects.far_shadow_dirty = true;
			world->objects.near_shadow_dirty = true;
		}
	}
	update_snow_amount(world);
	objects_update_seasons(&world->objects);
	world->objects.lod_distance_scale = world->settings->objects.lod_distance_scale;
	world->objects.lod_foliage_kept = world->settings->objects.lod_foliage_kept;
	world->objects.prop_lod_pixels = world->settings->objects.prop_lod_pixels;
	// The shadows follow the light: at once when it is moved by hand, in
	// steps as the day goes (each step redraws the shadow maps)
	{
		const render_settings_t *lit = &world->lit_render;
		float step = render->day_cycle ? DAY_SHADOW_STEP_DEGREES : 0.0f;
		float azimuth_change = SDL_fabsf(lit->sun_azimuth_degrees - world->drawn_sun_azimuth);

		azimuth_change = SDL_min(azimuth_change, 360.0f - azimuth_change);
		if (azimuth_change > step || SDL_fabsf(lit->sun_elevation_degrees - world->drawn_sun_elevation) > step)
		{
			world->drawn_sun_azimuth = lit->sun_azimuth_degrees;
			world->drawn_sun_elevation = lit->sun_elevation_degrees;
			world->terrain.shadow_dirty = true;
		}
	}
}

// Rebuilds what the render settings size at creation: the terrain
// textures and the shadow maps
void world_apply_render_settings(world_t *world)
{
	materials_destroy(&world->renderer.materials);
	if (!materials_load(&world->renderer.materials, &world->settings->render))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Reloading the terrain materials failed");
	}
	shadow_map_destroy(&world->near_shadow);
	shadow_map_destroy(&world->shadow);
	shadow_map_create(&world->shadow, world->settings->render.shadow_map_size);
	shadow_map_create(&world->near_shadow, world->settings->render.shadow_map_size > 0 ? world->settings->render.near_shadow_map_size : 0);
	world->shadow.needs_render = true;
	world->near_shadow.needs_render = true;
	world->last_shadow_render_seconds = 0.0;
	world->last_near_shadow_render_seconds = 0.0;
	SDL_Log("Graphics rebuilt: textures %d, anisotropy %.0f, shadow maps %d and %d", world->settings->render.texture_size,
		(double)world->settings->render.anisotropy, world->shadow.size, world->near_shadow.size);
}

void world_regrow(world_t *world)
{
	objects_clear(&world->objects, true);
	populate_map(&world->objects, &world->terrain, &world->water, &world->settings->objects);
	material_map_restamp(&world->terrain, &world->objects);
}
