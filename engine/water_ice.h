// The ice on the water (data/shaders/water_ice.comp): one RGBA16F texel per
// cell, its thickness (m) and the snow on it (0..1), worked out on the GPU
// a few times a second from the water's depth and flow and how cold it is.
// Still water freezes through the winter, from the shore out, running
// water stays open; it melts after the winter, a flood breaks it.
//
// Where it is thick enough (WATER_ICE_LOCK_METERS) the water under it does
// not move: the simulation's pipes in and out of those cells are shut
// (water_fluxes.comp, water_gpu_set_ice). The water renderer draws it over
// the water (water_surface.glsl).

#ifndef WATER_ICE_H
#define WATER_ICE_H

#include <stdbool.h>

#include "gpu.h"

// Ice this thick holds the water under it still (water_fluxes.comp's)
static const float WATER_ICE_LOCK_METERS = 0.03f;
// This cold (the winter, 0..1: season_snow) all of the water holds still,
// running or not (the game stops its simulation), and freezes over
static const float WATER_ICE_HOLDS_WINTER = 0.35f;

typedef struct
{
	SDL_GPUComputePipeline *pipeline;
	SDL_GPUTexture *texture;       // RGBA16F, one texel per cell (NULL: none yet, or let go)
	int size;
	bool cleared;                  // the texture holds ice (not garbage)
	float pending_seconds;         // not worked in yet (it runs a few times a second)
	// No winter and none of it left for long: it rests (and its texture is
	// all clear). idle_seconds: since the winter's end
	float idle_seconds;
	bool resting;
	int pass_count;                // statistics
	int reset_count;
} water_ice_t;

// What it goes by
typedef struct
{
	SDL_GPUTexture *water_texture; // RG32F: the water's depth (water_gpu_depth_texture)
	SDL_GPUTexture *flow_texture;  // RGBA16F: its velocity (water_gpu_flow_texture)
	SDL_GPUTexture *height_texture; // R32F: the ground (m)
	int map_size;
	float seconds;                 // game time since the last call
	float winter;                  // how cold (0: it melts .. 1: deep winter)
	float snowfall;                // how hard it snows (0..1)
	bool reset;                    // everything as the season says at once (a new map, a jump in the month)
	bool frozen;                   // all of the water held still (WATER_ICE_HOLDS_WINTER): it freezes everywhere, running or not
} water_ice_inputs_t;

bool water_ice_create(water_ice_t *ice);
void water_ice_destroy(water_ice_t *ice);

// Works the time since the last call in. Outside any pass
void water_ice_update(water_ice_t *ice, const water_ice_inputs_t *inputs);

// A new map: as the season says, at the next update
void water_ice_clear(water_ice_t *ice);

// Turned off: its texture let go (made anew, as the season says, when it is
// on again)
void water_ice_release(water_ice_t *ice);

// The texture to draw and to hold the water by: NULL while there is none
// (resting, off)
SDL_GPUTexture *water_ice_texture(const water_ice_t *ice);

#endif
