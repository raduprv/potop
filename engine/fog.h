// The valley fog: low milky fog lying in the valleys and over the water,
// its top soft and torn into wisps that drift with the wind; it forms in
// the evening, is thickest at dawn and lifts in the morning
// (render.fog_form_hour, fog_clear_hour).
//
// Where it lies: the valley map (fog_update), made on the CPU from the
// terrain now and then: for each place, how far below its surroundings it
// is (the ground smoothed over render.fog_valley_reach_meters), which
// decides how high the fog's top is there (up to fog_valley_height_meters)
// and how much fog there is; over water, some always. The top is smoothed
// too, so it lies level as a lake's. (Later the terrain's zones will say
// more: none in the deserts, more by the rivers.)
//
// How it is drawn (render.fog_mode):
//   volumetric  the camera's view cut into froxels (cells across, down and
//               in slices of distance, farther apart the farther out), the
//               fog and its light in each (fog_inject.comp: the sun's,
//               through the shadow maps, so trees cast shafts into it, and
//               the sky's), gathered from the eye outwards
//               (fog_integrate.comp), and each pixel takes what lies
//               between it and the eye (fog_apply.frag); past the froxels'
//               reach (render.fog_distance_meters), out to what it sees or
//               the map's edge (the sky), a few steps of its own: the far
//               valleys, the milky horizon when the eye is in it
//   simple      each pixel marches its own ray through it in a few steps,
//               out to the map's edge, no shadows (fog_simple.frag): for
//               weaker GPUs
//   off         none
// The sun's rays through it, in both: from the screen itself (the sky round
// the sun, what stands in front of it, gathered towards the sun on the
// screen: fog_rays_mask.comp, fog_rays.comp), as much as there is fog
// before what each pixel sees (render.fog_rays): the soft shafts between
// the trees when looking into the sun.
// Drawn over the scene once the water and the clouds are in (the clouds
// fog over as the land does; the particles come after).

#ifndef FOG_H
#define FOG_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
#include "scene_uniforms.h"
#include "settings.h"
#include "terrain.h"
#include "water.h"

#define FOG_MODE_OFF 0
#define FOG_MODE_SIMPLE 1
#define FOG_MODE_VOLUMETRIC 2

typedef struct
{
	// The valley map (RG32F: the top's height in meters, how much fog 0..1)
	SDL_GPUTexture *valley_texture;
	float *valley;                         // its texels on the CPU
	int valley_size;                       // texels a side
	double next_valley_seconds;            // when it is made again
	float highest;                         // the highest top anywhere (m): rays above it have no fog
	bool valley_ready;
	// Its making, a slice a frame (four milliseconds at once, every few
	// seconds, was a stutter): the buffers it is worked out in, and how
	// far it is (valley_step: FOG_VALLEY_ steps, valley_row within one)
	float *valley_work[4];                 // the ground, what lies around, the top, the amount
	double *valley_table;                  // the blurs' summed table
	int valley_step;
	int valley_row;
	// The froxels (RGBA16F 3D): the light and the density in each (two,
	// in turn: this frame's is built on the frame before's, the same place
	// in the world), and gathered towards the eye
	SDL_GPUTexture *scatter_textures[2];
	SDL_GPUTexture *integrated_texture;
	bool history_valid;                    // the frame before's froxels can be taken
	float previous_view_projection[16];    // where they were (OpenGL style)
	float previous_eye[3];
	int grid[3];                           // their size: across, down, slices (0: none yet)
	// The sun's rays (half the screen's size, R16F): how much of its glow
	// each place lets through, and that gathered towards the sun
	SDL_GPUTexture *rays_mask_texture;
	SDL_GPUTexture *rays_texture;
	uint32_t rays_size[2];
	SDL_GPUComputePipeline *rays_mask_pipeline;
	SDL_GPUComputePipeline *rays_pipeline;
	SDL_GPUComputePipeline *rays_wide_pipeline;    // the third pass (fog_rays_wide.comp)
	SDL_GPUComputePipeline *inject_pipeline;
	SDL_GPUComputePipeline *integrate_pipeline;
	SDL_GPUGraphicsPipeline *apply_pipeline;
	SDL_GPUGraphicsPipeline *simple_pipeline;
	uint32_t frame;
	bool created;
	bool failed;
} fog_t;

void fog_destroy(fog_t *fog);

// How much fog the hour brings (0..1): it forms at fog_form_hour, thickest
// through the night and at dawn, gone by fog_clear_hour
float fog_amount_at_hour(const render_settings_t *settings, float hour);

// Makes the valley map again if it is due (every few seconds: the water
// moves, the land is dug)
void fog_update(fog_t *fog, const terrain_t *terrain, const water_t *water, const render_settings_t *settings, double seconds);

// Draws the fog over the scene's color, by its depth (a pass of its own,
// ended here); the scene's block is the frame's. wind: meters per second
// (x, z)
void fog_draw(fog_t *fog, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width, uint32_t height,
	const scene_uniforms_t *scene, const render_settings_t *settings, SDL_GPUTexture *far_shadow, SDL_GPUTexture *near_shadow,
	float hour, const float wind[2], double seconds, float map_extent_meters);

#endif
