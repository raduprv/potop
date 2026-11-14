// How lit a place is. See light_probe.h.

#include "light_probe.h"

#include <SDL3/SDL.h>

// The sun from this far under the horizon (dusk) to this far over it:
// night to full day
static const float SUN_DARK_DEGREES = -8.0f;
static const float SUN_FULL_DEGREES = 12.0f;
// A full moon high up lights this much of what the day does (a game's
// moon: far more than the real one's, so a night is not pitch black), the
// stars this much always
static const float FULL_MOON_SHARE = 0.18f;
static const float STARLIGHT_SHARE = 0.02f;
// The sky seen on average this far round (a wood's edge is neither the
// wood's dark nor the field's light)
static const float SKY_AVERAGE_METERS = 3.0f;

static float smooth_between(float edge_0, float edge_1, float value)
{
	float t = SDL_clamp((value - edge_0) / (edge_1 - edge_0), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

float light_probe_daylight(const world_t *world)
{
	const daylight_t *daylight = &world->daylight;
	float sun = smooth_between(SUN_DARK_DEGREES, SUN_FULL_DEGREES, daylight->sun_elevation_degrees);
	float moon = FULL_MOON_SHARE * daylight->moon_lit * smooth_between(-2.0f, 20.0f, daylight->moon_elevation_degrees);

	return SDL_clamp(SDL_max(sun, moon + STARLIGHT_SHARE), 0.0f, 1.0f);
}

void light_probe_at(const world_t *world, float x, float y, float z, light_probe_t *probe_out)
{
	float point[3] = { x, y, z };

	probe_out->sky_open = sky_occlusion_open_around(&world->sky_occlusion, world->terrain.cell_size_meters, x, z, SKY_AVERAGE_METERS);
	probe_out->daylight = light_probe_daylight(world);
	probe_out->local = lights_light_at(&world->lights, point);
	probe_out->total = SDL_min(probe_out->sky_open * probe_out->daylight + probe_out->local, 1.0f);
}
