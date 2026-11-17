// Water rendering.
//
// The water surface reuses the terrain's chunk mesh: the vertex shader
// places each vertex at terrain height + water depth, read from the float
// textures the simulation writes on the GPU (depth; flow speed, mud: see
// water_gpu.h). Dry chunks are skipped entirely.
//
// Shading: color by depth (clear and greenish when shallow, dark when deep),
// sky reflection that grows at grazing angles (Fresnel), a sun highlight,
// small animated ripples, foam where the flow is fast, brown and opaque
// where it carries sediment, shadows from the terrain, and the same fog as
// the terrain.
//
// Reflections (render.water_reflections): before the water is drawn, the
// scene's color and depth so far are copied (water_renderer_capture), and
// the reflected rays are marched over that copy (screen space reflections):
// the land, the trees, the actors. Where a ray leaves the screen or finds
// nothing, the sky, with the volumetric clouds from their dome.
//
// At half size (render.water_reflections_half_size), the marching is done
// apart, for a quarter of the pixels: the water is drawn with the sky in
// the reflection's place (water_split.frag), writing beside its color what
// each pixel would march (the ray, how much of the pixel its reflection
// is) and the sky it put there; water_reflection.comp marches, for each
// 2 x 2 pixels, the one that mirrors the most; water_reflection_apply.frag
// adds what was found in place of the sky, each pixel taking the nearest
// marched texels at its own distance.

#ifndef WATER_RENDERER_H
#define WATER_RENDERER_H

#include <stdbool.h>

#include "camera.h"
#include "clouds.h"
#include "gpu.h"
#include "settings.h"
#include "shadow_map.h"
#include "terrain.h"
#include "terrain_renderer.h"
#include "water.h"
#include "scene_uniforms.h"

typedef struct
{
	// Pipelines by the debug settings (depth test, depth write, back face
	// culling, wireframe: one bit each), made when first wanted
	// (bit 16: the reflections apart, water_split.frag)
	SDL_GPUGraphicsPipeline *pipelines[32];
	bool pipeline_failed[32];
	// The reflections at half size (see above)
	SDL_GPUComputePipeline *march_pipeline;
	SDL_GPUGraphicsPipeline *apply_pipeline;
	bool split_failed;             // their shaders did not load: every pixel marches its own
	SDL_GPUTexture *ray_texture;   // full size, RGBA16F: the ray's direction, its weight
	SDL_GPUTexture *sky_texture;   // full size, RGBA16F: the sky mirrored, the distance along the view
	SDL_GPUTexture *seen_texture;  // half size, RGBA16F: what the march found, how sure
	SDL_GPUTexture *distance_texture; // half size, R32F: the marched pixel's distance
	int split_width;
	int split_height;
	bool drawing_split;            // water_renderer_draw draws with the split pipelines
	int drawn_chunk_count;
	// The chunks with water in view, their origins (cells, two int32 each):
	// the per instance vertex buffer, one draw for them all
	// (water_renderer_prepare, before the scene pass)
	SDL_GPUBuffer *instance_buffer;
	int32_t *instance_staging;
	int instance_capacity;
	int instance_count;
	// F6: thin water in a loud color, for debugging: bright magenta where it
	// moves faster than moving_speed (what keeps a tile awake), dark purple
	// where it stands still, films too thin to flow in cyan. The game sets
	// the depths and the speed from the settings.
	bool highlight_thin;
	float film_meters;
	float thin_meters;
	float moving_speed;
	// Where the frame is between the last two simulation steps (0: the
	// one before, 1: the last): the surface is drawn in between, so it
	// moves smoothly at any frame rate. The game sets it each frame.
	float step_blend;
	// The scene so far, for the reflections (water_renderer_capture)
	SDL_GPUTexture *scene_color;
	SDL_GPUTexture *scene_depth;
	int scene_width;
	int scene_height;
	bool captured;                 // this frame's copy is there (the draw uses it, then forgets it)
	// The ice on the water (water_ice.h; NULL: none). The game sets it each frame
	SDL_GPUTexture *ice_texture;
	// The small ripples' slopes and their squares (made here at the start,
	// RGBA16 SNORM, its own levels of detail): one tile of steep crested
	// ripples, repeated
	SDL_GPUTexture *ripple_texture;
} water_renderer_t;

bool water_renderer_create(water_renderer_t *renderer);
void water_renderer_destroy(water_renderer_t *renderer);

// True if some water would be drawn this frame (worth capturing the
// scene for)
bool water_renderer_visible(const water_t *water, const terrain_t *terrain, const camera_t *camera);

// Before the scene pass: the chunks with water in view found and their
// origins sent up (a pass cannot send anything); water_renderer_draw draws
// those
void water_renderer_prepare(water_renderer_t *renderer, const water_t *water, const terrain_t *terrain, const camera_t *camera);

// Copies the scene's color and depth for the reflections, between render
// passes (paused). False if it could not (the water then mirrors the sky)
bool water_renderer_capture(water_renderer_t *renderer, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, int width, int height);

// Draws the water over the chunks the terrain renderer found visible this
// frame, in the scene's pass (its scene block pushed), after the terrain.
// clouds: their dome mirrored where the reflections find no land (NULL:
// the volumetric clouds are not drawn)
void water_renderer_draw(water_renderer_t *renderer, SDL_GPURenderPass *pass, const water_t *water, const terrain_t *terrain,
	const terrain_renderer_t *terrain_renderer, const camera_t *camera, const render_settings_t *settings,
	const shadow_map_t *shadow, const shadow_map_t *near_shadow, const clouds_t *clouds);

// The water with its reflections at half size (see above), when the
// scene's pass is paused and its copy captured: its own pass over the
// scene's color and depth, the march, the reflections added. False if it
// could not (nothing drawn: draw it the usual way)
bool water_renderer_draw_split(water_renderer_t *renderer, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, int width,
	int height, const scene_uniforms_t *scene, const water_t *water, const terrain_t *terrain,
	const terrain_renderer_t *terrain_renderer, const camera_t *camera, const render_settings_t *settings,
	const shadow_map_t *shadow, const shadow_map_t *near_shadow, const clouds_t *clouds);

#endif
