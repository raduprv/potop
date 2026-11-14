// Rain and rainbows, as they are seen (the water the rain brings is the
// simulation's: water.raining, erosion.c).
//
// The drops: thin streaks falling through a box around the eye, slanted by
// the clouds' wind, stopping at the ground or the water (data/shaders/
// rain.vert). Nothing is simulated: where each drop is follows from its
// number and the time, so the box costs no more than drawing it. Where
// drops land, small rings spread on the ground and the water (splashes,
// rain_splash.vert), and the water's surface gets rings of its own
// (water.frag). While it rains the light dims, the haze thickens and the
// objects darken as they get wet (the ground's own wetness is the
// simulation's moisture); they dry slowly once it stops.
//
// In the snow's months (render.snow_first_month .. snow_last_month) it
// snows instead: slow flakes, swaying and drifting with the wind, no
// splashes, no rainbow; sleet as the snow comes and goes.
//
// The rainbow: a ring 42 degrees around the point opposite the sun, red
// outside, violet inside, a fainter second one at 51 degrees with its
// colors the other way round, the sky darker between them (Alexander's
// band) and brighter inside the first (rainbow.frag). It hangs in the rain
// some way off: it is drawn over the sky and over land farther than that,
// never over what is near. So it shows only with the sun low (under 42
// degrees: mornings and evenings), and never at night.
//
// Both are the game's say (rain_set, rainbow_set: the World window, the
// console), eased in and out over a few seconds, and shown as strong as
// the settings say (render.rain_*, render.rainbow_strength).

#ifndef RAIN_H
#define RAIN_H

#include <stdbool.h>

#include "camera.h"
#include "gpu.h"
#include "settings.h"

typedef struct
{
	bool raining;                  // the game's say (kept equal to water.raining)
	float shown;                   // 0..1, eased towards raining
	float wetness;                 // 0..1: how wet things are (rises fast in rain, dries slowly)
	bool rainbow_on;
	float rainbow_strength;        // the game's (1: a good one)
	float rainbow_shown;           // 0..1, eased towards rainbow_on
	SDL_GPUGraphicsPipeline *drop_pipeline;
	SDL_GPUGraphicsPipeline *splash_pipeline;
	SDL_GPUGraphicsPipeline *rainbow_pipeline;
	SDL_GPUGraphicsPipeline *snow_pipeline;
} rain_t;

// What the drawing needs of the world: the ground (meters) and the water
// on it (meters, the last two steps), as the renderers have them
typedef struct
{
	SDL_GPUTexture *height_texture;    // R32F
	SDL_GPUTexture *water_texture;     // RG32F (water_gpu_depth_texture)
	int map_size;                      // cells per side
	float cell_size_meters;
	float wind[2];                     // the clouds' wind, meters per second (x, z)
	float seconds;                     // the game's clock
	float screen_height;               // pixels (the drops are never thinner than one)
	float snow;                        // how much of it falls as snow (0 rain .. 1 snow: the snow's months, season.h)
} rain_world_t;

bool rain_create(rain_t *rain);
void rain_destroy(rain_t *rain);

void rainbow_set(rain_t *rain, bool on, float strength);

// Eases the rain and the rainbow in or out, and wets or dries things
void rain_update(rain_t *rain, float seconds);

// How much it rains on screen (0..1): eased, times render.rain_intensity
float rain_intensity(const rain_t *rain, const render_settings_t *settings);
// How much the rainbow shows (0..): eased, the game's strength times
// render.rainbow_strength, and how much sun there is (none at night)
float rainbow_intensity(const rain_t *rain, const render_settings_t *settings, float sun_elevation_degrees);

// Dims the light while it rains: the sun, the sky's light, a thicker haze
// (on the render settings the frame is drawn with)
void rain_dim_light(const rain_t *rain, const render_settings_t *settings, render_settings_t *lit);

// In the scene's pass, after everything else (the volumetric clouds too),
// with the scene's block pushed: the rainbow, then the drops and splashes
void rain_draw(rain_t *rain, SDL_GPURenderPass *pass, const camera_t *camera, const render_settings_t *settings,
	const rain_world_t *world, float sun_elevation_degrees);

#endif
