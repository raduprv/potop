// Sun shadows with a shadow map.
//
// Two maps ("cascades"): the far one covers the whole map and is redrawn
// only when the terrain or the objects change; the near one covers a square
// around the point the camera looks at, finely enough for the shadows of
// branches, and is redrawn when the camera has moved a fair bit, too.
//
// The terrain is drawn from the sun's point of view into a depth texture;
// when shading, each pixel compares its own distance to the sun with the
// stored one to know whether something stands in between.
//
// The sun does not move, so the far map only has to be redrawn when
// something on the map changes. With 4096 texels on a 1024 m map that is
// 25 cm per texel: fine for hills, useless for branches, hence the near map
// (4096 texels over 100 m is about 2.5 cm).

#ifndef SHADOW_MAP_H
#define SHADOW_MAP_H

#include <stdbool.h>

#include "gpu.h"
#include "math3d.h"
#include "terrain.h"

// The moving layer's marks (below): at most this many a frame, more and the
// whole still layer is copied in
#define SHADOW_MAP_MARKS_MAX 128

// A rectangle of the map, in texels
typedef struct
{
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
} shadow_rectangle_t;

typedef struct
{
	SDL_GPUTexture *depth_texture; // 1x1 when shadows are off (shaders still bind one)
	// The near map in two layers: what stands still is drawn here (now and
	// then: shadow_map_begin_still), and each frame something moves it is
	// copied into depth_texture and the moving things drawn over it
	// (shadow_map_begin_moving). NULL until first asked for
	SDL_GPUTexture *still_texture;
	SDL_GPUGraphicsPipeline *terrain_pipeline;
	int size;                      // texels per side; 0 = shadows disabled
	mat4_t light_view_projection;  // world -> light clip space
	mat4_t light_view;             // world -> light view space
	float depth_near;              // the orthographic depth range
	float depth_far;
	// Near cascade only: the square it covers, where it was centered
	vec3_t center;
	float extent_meters;
	bool needs_render;
	// The near map's moving layer: where what moves is drawn this frame
	// (marked: shadow_map_mark_moving) and was drawn the last time
	// (drawn). Only there is the still layer copied in again, not the whole
	// map (16 million texels for a hen); drawn_known false: the last time
	// is not known (the whole map then)
	shadow_rectangle_t marked[SHADOW_MAP_MARKS_MAX];
	int marked_count;
	bool marked_overflow;
	shadow_rectangle_t drawn[SHADOW_MAP_MARKS_MAX];
	int drawn_count;
	bool drawn_known;
} shadow_map_t;

// Creates the depth texture and the terrain's pipeline. size 0 disables
// shadows (the call still succeeds). Returns false if the GPU refuses.
bool shadow_map_create(shadow_map_t *shadow, int size);
void shadow_map_destroy(shadow_map_t *shadow);

// Fits an orthographic light projection around the whole terrain.
void shadow_map_update_projection(shadow_map_t *shadow, const terrain_t *terrain, vec3_t sun_direction);

// The near cascade: a finer map of a square extent_meters wide around
// center (seen from the sun), with the far map's view and depth range.
// Needs the far map's projection to be up to date.
void shadow_map_update_near_projection(shadow_map_t *near_shadow, const shadow_map_t *far_shadow, vec3_t center, float extent_meters);

// Opens the pass that draws into the map (the scene's terrain, objects and
// actors follow, each with its own shadow pipeline); NULL if shadows are off.
// The sun's matrix is pushed for the vertex shaders (shadow_pass.glsl)
SDL_GPURenderPass *shadow_map_begin(shadow_map_t *shadow);

// The still layer (made the first time): cleared, ready for the terrain
// and what stands still. NULL if shadows are off or it cannot be made
SDL_GPURenderPass *shadow_map_begin_still(shadow_map_t *shadow);

// A box (world, meters) something moving is drawn within this frame, for
// the moving layer: its texels get the still layer again before it is drawn
// (and next frame too, where it was). Everything drawn over the still layer
// that moved since the last time must be marked (or the whole asked for);
// what is drawn again where it was need not be
void shadow_map_mark_moving(shadow_map_t *shadow, vec3_t minimum, vec3_t maximum);

// The map the shading reads: the still layer copied in, ready for what
// moves to be drawn over it (cleared instead if there is no still layer).
// Copied in only where the marks are, this frame's and the last time's,
// unless whole (the still layer drawn again, something moved that was not
// marked) or the marks are not enough; the marks are then cleared
SDL_GPURenderPass *shadow_map_begin_moving(shadow_map_t *shadow, bool whole);

// The terrain's chunks, chunk_index_count indices from first_index in the
// chunk index buffer each (a level of detail: terrain_renderer.h)
void shadow_map_render_terrain(shadow_map_t *shadow, SDL_GPURenderPass *pass, const terrain_t *terrain, SDL_GPUBuffer *chunk_vertex_buffer,
	SDL_GPUBuffer *chunk_index_buffer, int chunk_index_count, int first_index, SDL_GPUTexture *height_texture, bool cull);

void shadow_map_end(shadow_map_t *shadow, SDL_GPURenderPass *pass);

// The two maps as fragment samplers 0 and 1 (shadows.glsl); the far one
// stands in for a missing near one
void shadow_map_bind_for_shading(SDL_GPURenderPass *pass, const shadow_map_t *far_shadow, const shadow_map_t *near_shadow);

#endif
