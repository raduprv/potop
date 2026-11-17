// Grass: blades, made on the GPU every frame around the eye (no objects,
// nothing kept), on the ground the terrain shows as grass.
//
// grass.comp scatters them: a grid fixed to the world, so the blades stay
// put as the eye moves, fine near the eye and coarser ring after ring
// farther out, the blades thinning with the square of the distance (and
// growing wider), out to objects.grass_distance where they sink into the
// ground. It keeps those in view, on grass (as terrain.frag decides grass:
// soil, slope, sun, silt, bare rock, cliffs, snow), not drowned, and writes
// them with the indirect draw's arguments. grass.vert makes each one, a
// strip of seven vertices bowed the way it leans: a little its own way,
// with the wind (the gusts that roll through the trees) and laid down the
// way the water flows over it, or flat in the silt it left; colored as the
// ground's grass under it. grass.frag lights it as the terrain is lit, the
// sun shining through it from behind.
//
// The blades receive the shadows but cast none (a blade's shadow is lost in
// the grass's own shade, which grass.frag darkens low down).

#ifndef GRASS_H
#define GRASS_H

#include <stdbool.h>

#include "camera.h"
#include "gpu.h"
#include "hiz.h"
#include "materials.h"
#include "settings.h"
#include "scene_uniforms.h"
#include "shadow_map.h"

typedef struct
{
	SDL_GPUComputePipeline *scatter_pipeline;
	SDL_GPUComputePipeline *map_scatter_pipeline; // the same from the material map (grass_map.comp)
	// Their subgroup variants (gpu.vulkan_11; NULL: none)
	SDL_GPUComputePipeline *scatter_subgroups_pipeline;
	SDL_GPUComputePipeline *map_scatter_subgroups_pipeline;
	bool use_subgroups;            // this frame's say (render.gpu_subgroups)
	SDL_GPUGraphicsPipeline *pipeline;
	SDL_GPUBuffer *blade_buffer;           // grass.vert's instances: 8 floats a blade
	uint32_t blade_capacity;               // blades it holds
	SDL_GPUBuffer *count_buffer;           // the count, then the draw's arguments
	SDL_GPUBuffer *argument_buffer;        // a copy of those, for the indirect draw
	SDL_GPUBuffer *no_occlusion_buffer;    // bound in the pyramid's place where there is none (a float)
	bool prepared;                         // grass_prepare filled them for this frame
	bool failed;                           // the shaders did not load: no grass
	uint32_t candidate_count;              // statistics: grid spots looked at, last frame
	uint32_t blade_estimate;               // blades expected at most, last frame
} grass_t;

// What the grass needs of the world, as the renderers have it
typedef struct
{
	SDL_GPUTexture *height_texture;        // R32F, meters
	SDL_GPUTexture *water_texture;         // RG32F (water_gpu_depth_texture)
	SDL_GPUTexture *flow_texture;          // RGBA16F (water_gpu_flow_texture)
	SDL_GPUTexture *soil_weight_texture;   // the terrain renderer's
	SDL_GPUTexture *normal_texture;
	SDL_GPUTexture *surface_texture;
	SDL_GPUTexture *material_texture;      // RGBA8: the material map (material_map.h)
	bool material_map;                     // read it (render.material_map)
	SDL_GPUTexture *overlay_texture;       // RGBA16F: snow, petals (ground_overlay.h; NULL: none)
	SDL_GPUTexture *variation_texture;
	SDL_GPUTexture *material_color_array;
	SDL_GPUTexture *cloud_shadow_texture;  // NULL: the variation texture stands in
	SDL_GPUTexture *sky_occlusion_texture; // R8 a cell (sky_occlusion.h; NULL: none)
	SDL_GPUTexture *sky_layers_texture;    // its layers, the light the land round sends back (NULL with it)
	SDL_GPUTexture *sky_bounce_texture;
	int grass_layer;                       // the grass material's first layer in the arrays
	float grass_macro_repeats_per_meter;
	float soil_blend_cells;
	int map_size;                          // cells per side
	float cell_size_meters;
	float max_height_meters;
	float sun_direction[3];                // towards the sun
	float material_patches[MATERIAL_COUNT][4]; // terrain.frag's (repeats per meter, strength, edge meters, -)
	const float *wind_uniform;             // objects_t.wind_uniform (8 floats: the plants' wind)
	// The depth pyramid of the frame before (hiz.h; NULL: none): a blade
	// behind what was drawn then (a house, a hill) is left out
	const hiz_t *occlusion;
	// What the scatter works out once a blade (its color, the sky's light,
	// the sun the shadow maps and the clouds leave it) wants the scene's
	// block and the two shadow maps (the near one: the far one, unused)
	const scene_uniforms_t *scene;
	SDL_GPUTexture *far_shadow_texture;
	SDL_GPUTexture *near_shadow_texture;
} grass_world_t;

bool grass_create(grass_t *grass);
void grass_destroy(grass_t *grass);

// Scatters the blades in view, before the scene's pass begins
void grass_prepare(grass_t *grass, const camera_t *camera, const render_settings_t *render, const objects_settings_t *settings,
	const grass_world_t *world);

// Draws them, in the scene's pass (its block pushed)
void grass_draw(grass_t *grass, SDL_GPURenderPass *pass, const render_settings_t *render, const objects_settings_t *settings,
	const grass_world_t *world);

#endif
