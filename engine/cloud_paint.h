// The sky as the player paints it: a map over the clouds' map
// (CLOUD_WEATHER_METERS a side, repeating with it), three values a patch,
// each 0..255 with 128 as "what the cloud settings say":
//
//   coverage  how many clouds: 0 clear sky, 128 the Coverage setting,
//             255 twice that (overcast at most)
//   density   how heavy they are: 0 white and puffy (a quarter as dense),
//             128 as they come, 255 dark and heavy (four times as dense)
//   kind      which clouds: 0 fair weather puffs, 128 as the Towers
//             setting says, 255 towers
//   flocks    where the high layer of small clouds is (settings.h,
//             flock_*): 0 none (as it starts), 255 all the Amount setting
//             gives
//
// cloud_weather.c places the clouds after it; the sky window (ui.c) paints
// it; saved maps keep it (map_file.c). It is also drawn into a small
// texture for the window, in the colors of the value being painted.

#ifndef CLOUD_PAINT_H
#define CLOUD_PAINT_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#define CLOUD_PAINT_SIZE 256
#define CLOUD_PAINT_NEUTRAL 128

typedef enum
{
	CLOUD_PAINT_COVERAGE = 0,
	CLOUD_PAINT_DENSITY,
	CLOUD_PAINT_KIND,
	CLOUD_PAINT_FLOCKS,
	CLOUD_PAINT_CHANNELS
} cloud_paint_channel_t;

typedef struct
{
	uint8_t values[CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE * 4];   // RGBA: coverage, density, kind, flocks
	uint32_t revision;              // grows with every change
	uint32_t stamp_revision;        // grows with every change of what places the big clouds (not the flocks)
	uint64_t changed_ticks;         // when it last changed (ms)
	// The window's picture of it
	SDL_GPUTexture *texture;
	int32_t shown_channel;          // what the picture shows (-1: not drawn yet)
	uint32_t shown_revision;
	int32_t shown_offset[2];        // the patch at the picture's corner
} cloud_paint_t;

// All neutral: the sky as the cloud settings make it
void cloud_paint_reset(cloud_paint_t *paint);

// Paints around (u, v) (0..1 over the map, repeating), radius of the map's
// side: amount (-1..1, a share of the whole range) of the channel, full in
// the middle and fading to nothing at the edge. neutral: towards 128 (the
// flocks: towards none) instead (amount's size only)
void cloud_paint_brush(cloud_paint_t *paint, cloud_paint_channel_t channel, float u, float v, float radius, float amount, bool neutral);

// A channel at a point of the clouds' map (meters, any; it repeats), 0..1
// with 0.5 neutral, smoothly between the patches
float cloud_paint_sample(const cloud_paint_t *paint, cloud_paint_channel_t channel, float x_meters, float z_meters);

// The window's picture, centered on (center_u, center_v) (0..1 over the map:
// the map goes round, so the eye can stay in the middle), drawn again if the
// paint, the channel shown or the center's patch changed (uploaded with the
// frame's other uploads). corner_out: the map's u, v at the picture's top
// left corner. NULL until made
SDL_GPUTexture *cloud_paint_picture(cloud_paint_t *paint, cloud_paint_channel_t channel, float center_u, float center_v, float corner_out[2]);

void cloud_paint_destroy(cloud_paint_t *paint);

#endif
