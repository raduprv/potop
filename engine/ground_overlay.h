// What settles on the ground and goes again: the material map's overlays
// in amounts (material_map.h). The CPU says where (the overlay ids); this
// works out how much, on the GPU (ground_overlay.comp):
//
//   snow     falls where the weather snows, as much as the cell sees the
//            sky (the sky's occlusion, sky_occlusion.h: little under a
//            crown, none under a roof; not on steep faces nor on water), lies through the winter, melts
//            after it (faster on sunny slopes) and at once under water
//   petals   where the map put them (under blooming trees), through the
//            bloom, carried off by water
//
// One RGBA16F texel per cell: snow, petals (0..1 each), unused. The terrain
// shows an overlay where its mask (the alpha of its normal map) is below
// its amount: it covers the hollows first, then all of it.

#ifndef GROUND_OVERLAY_H
#define GROUND_OVERLAY_H

#include <stdbool.h>

#include "gpu.h"

typedef struct
{
	SDL_GPUComputePipeline *pipeline;
	SDL_GPUTexture *texture;       // RGBA16F, one texel per cell
	int size;
	bool cleared;                  // the texture holds amounts (not garbage)
	float pending_seconds;         // not worked in yet (it runs a few times a second)
	// Nothing falls and nothing lies (no winter, no bloom, all of it gone
	// since): it rests. idle_seconds: since anything last fell
	float idle_seconds;
	bool resting;
	int pass_count;                // statistics: passes run, of them resets
	int reset_count;
} ground_overlay_t;

// What it goes by
typedef struct
{
	SDL_GPUTexture *material_texture;  // the material map (terrain_renderer_t)
	SDL_GPUTexture *normal_texture;    // the terrain's normals (the material normal's slope)
	SDL_GPUTexture *water_texture;     // RG32F: the water's depth (water_gpu_depth_texture; NULL: none)
	SDL_GPUTexture *sky_texture;       // R8: how much sky each cell sees (sky_occlusion_t; NULL: not yet, all of it)
	int map_size;
	float cell_size_meters;
	float seconds;                 // game time since the last call
	float snowfall;                // how hard it snows (0..1)
	float winter;                  // how cold (0: snow melts .. 1: it stays)
	float bloom;                   // the blooming trees' petals: 0 none .. 1 all of them
	float sun_x;                   // towards the sun, horizontal
	float sun_z;
	float sun_height;              // its elevation's sine (melting on sunny slopes)
	float snow_amount;             // the snow on things (objects_t.snow_amount): the ground's on a reset
	bool reset;                    // everything as the season says at once (a new map, a jump in the month)
} ground_overlay_inputs_t;

bool ground_overlay_create(ground_overlay_t *overlay);
void ground_overlay_destroy(ground_overlay_t *overlay);

// Works the time since the last call in (a few times a second: the amounts
// change slowly). Before the terrain is drawn, outside any pass.
void ground_overlay_update(ground_overlay_t *overlay, const ground_overlay_inputs_t *inputs);

#endif
