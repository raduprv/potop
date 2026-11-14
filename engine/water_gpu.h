// The water simulation on the GPU (compute shaders, data/shaders/water_*.comp).
//
// The GPU holds the real water: depth, sediment, the pipes' flow, the
// tiles' states. The water_t the rest of the game reads (the physics, the
// actors, the navigation, the inspector) is a copy, brought back a few
// tiles at a time: each frame the GPU's tile statistics come back (states,
// totals, the counters of the balance), and the cells of the tiles that
// changed, as many as the budget allows, nearest to where the camera looks
// first. That copy is a frame or two behind, which nobody sees.
//
// The other way, what the CPU changes in water_t (a lake stamp, a scene,
// a loaded map, the walls the physics stands up) goes up tile by tile at
// the start of the next step; the brush, the springs and the rain run on
// the GPU itself. The terrain goes up where it was edited
// (terrain_mark_cells_changed).
//
// Everything here records into gpu_commands(): the steps of a frame pile
// up in the work command buffer, which water_gpu_end_frame submits.

#ifndef WATER_GPU_H
#define WATER_GPU_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
#include "settings.h"
#include "soils.h"
#include "terrain.h"

struct water_s;

// The CPU side of the bookkeeping, made with the water (the GPU resources
// come on first use, once there is a device)
bool water_gpu_init(struct water_s *water);
void water_gpu_release(struct water_s *water);

// One simulation step on the GPU (see water_step)
void water_gpu_step(struct water_s *water, const terrain_t *terrain, const water_settings_t *settings, const soil_settings_t *soil_settings,
	float step_seconds, int32_t substeps, int32_t spring_units[], int32_t rain_units, bool evaporate, float evaporation_seconds);

// Erosion, deposition and (with collapse) bank collapse and smoothing on
// the GPU, after water_gpu_step (see erosion.h). The ground it changes
// comes back to the terrain a few tiles a frame (water_gpu_poll), which
// flags the shadows once the change passes shadow_threshold_units.
void water_gpu_erosion_step(struct water_s *water, const soil_settings_t *soil_settings, const water_settings_t *water_settings, float step_seconds,
	bool collapse, int64_t shadow_threshold_units);

// The brush, on the GPU (see water_apply_brush)
void water_gpu_brush(struct water_s *water, float center_x, float center_z, float radius_meters, float meters_per_second, float step_seconds);

// The terrain's textures, made here from the ground the simulation holds
// (water_ground.comp) rather than by the CPU from its copy: where they go
// (the terrain renderer's), and what decides the surface
typedef struct
{
	SDL_GPUTexture *height_texture;        // R32F, compute storage write
	SDL_GPUTexture *normal_texture;        // RGBA8, compute storage write
	SDL_GPUTexture *surface_texture;       // RGBA8, compute storage read and write
	int normal_reach;                      // the material normal's reach, cells
	float bare_units;                      // the bare rock shows through this much soil
	float silt_visible_units;              // silt shows from...
	float silt_full_units;                 // ...fully at
} water_ground_targets_t;

// Each frame (cheap): where the terrain's textures go (NULL: nowhere, the
// CPU makes them). New ones are drawn whole at the next water_gpu_end_frame
void water_gpu_set_ground_targets(struct water_s *water, const water_ground_targets_t *targets);
// True once the terrain's textures are made here: the CPU leaves them alone
bool water_gpu_ground_live(const struct water_s *water);
// A tile's wetness (TILE_CELLS bytes, its rows one after the other, 0..255)
// for the terrain's surface, and the tile drawn again
void water_gpu_ground_wetness(struct water_s *water, int32_t tile, const uint8_t *wetness);
// A tile's terrain textures drawn again (its ground changed on the CPU)
void water_gpu_note_ground(struct water_s *water, int32_t tile);
// What the CPU changed: a tile's water (depth, sediment) to go up,
// a tile to wake, everything cleared, or everything loaded anew
void water_gpu_note_edit(struct water_s *water, int32_t tile);
void water_gpu_note_wake(struct water_s *water, int32_t tile, int reason);
void water_gpu_note_clear(struct water_s *water);
void water_gpu_note_restore(struct water_s *water);

// Once a frame, after the steps: the water textures follow the changes,
// and the copies back to the CPU are asked for. focus_x and focus_z
// (world meters) is where the copy matters most (the camera's target).
void water_gpu_end_frame(struct water_s *water, const terrain_t *terrain, float focus_x, float focus_z);

// Once a frame, before the steps: applies the copies that have arrived
// (the water, and the ground into the terrain last given to the steps)
void water_gpu_poll(struct water_s *water);

// Waits for the GPU and copies everything back (saving a map, the tests)
void water_gpu_sync(struct water_s *water, const terrain_t *terrain);

// The textures the renderer draws the water from (NULL until the first
// step): R32F depth (meters), RGBA16F flow velocity (m/s, x and z),
// sediment concentration, busiest pipe's speed
SDL_GPUTexture *water_gpu_depth_texture(const struct water_s *water);

// Each frame: the ice (water_ice.h; NULL: none), the water under it held
// still where it is thick enough
void water_gpu_set_ice(struct water_s *water, SDL_GPUTexture *ice_texture);
SDL_GPUTexture *water_gpu_flow_texture(const struct water_s *water);

#endif
