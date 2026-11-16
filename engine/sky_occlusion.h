// The sky's occlusion ("ambient occlusion", baked): how much of the sky
// each cell of the ground sees (0..1). The ground and the grass light
// themselves from the sky that much (the sun has its shadows).
//
// Worked out once (a map loaded without it, the key U, the button in the
// Settings, a new map), on the GPU, over the whole map: from directions
// spread evenly over the sky (render.sky_occlusion_directions, from the
// zenith down to render.sky_occlusion_lowest_degrees above the horizon)
// the terrain and every object are drawn into a depth map (as the sun's
// far shadow map is: the plants' leaves let some through), and each cell
// adds how visible it is from there, weighted by how much that direction
// faces its ground; at the end, against what it would add if nothing were
// in the way. So a crater's floor, a narrow valley, the ground under a
// tree, round a rock's foot, in a crevice see less sky.
//
// Kept with the map (both kinds; map_file.h, sky_occlusion.bin: a byte a
// cell). What moves after (a log floating off) leaves its shade until it
// is worked out again.

#ifndef SKY_OCCLUSION_H
#define SKY_OCCLUSION_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
#include "map_file.h"
#include "objects.h"
#include "settings.h"
#include "shadow_map.h"
#include "terrain.h"

#define SKY_OCCLUSION_MOST_DIRECTIONS 256

// The layers in the air: how many, how high above the ground (meters; the
// objects' shaders have the same, ground_water.glsl)
enum { SKY_OCCLUSION_LAYERS = 4 };
extern const float sky_occlusion_layer_heights[SKY_OCCLUSION_LAYERS];
// The farthest the bounce texture's distances go (meters; sky_occlusion.glsl
// has it too)
extern const float SKY_OCCLUSION_BOUNCE_FARTHEST;

typedef struct
{
	SDL_GPUTexture *texture;           // R8, a texel a cell (NULL: none yet)
	int map_size;
	// The air above the ground: SKY_OCCLUSION_LAYERS layers that follow it
	// (sky_occlusion_layer_heights above it), layer_size texels per side
	// over the map (about a meter), the four in one texel (RGBA8, r the
	// lowest; layer_size x layer_size; map_file_sky_layer_size. The map
	// file and sky_occlusion_download/upload keep them one under the other,
	// R8, as before)
	SDL_GPUTexture *layers_texture;
	int layer_size;
	// The light the land round sends back: per texel of the ground (about a
	// meter, bounce_size per side; map_file_sky_bounce_size) the way to what
	// blocks its sky, on the whole, and how far (RGBA8: rgb the way * 0.5 +
	// 0.5, a sqrt(distance / SKY_OCCLUSION_BOUNCE_FARTHEST), 0: nothing)
	SDL_GPUTexture *bounce_texture;
	int bounce_size;
	bool ready;                        // worked out (or loaded) for this map
	// Its cells on the CPU too (map_size x map_size bytes; NULL: none yet),
	// for asking how much sky a place sees (sky_occlusion_open_at); fetched
	// from the GPU between frames once worked out (cpu_pending)
	uint8_t *cells_cpu;
	bool cpu_pending;
	// Their sums over the rectangles from the map's corner ((map_size + 1)
	// squared; a summed-area table): a box's average at once
	uint32_t *cell_sums;
	bool requested;                    // to be worked out when the terrain's textures are there
	SDL_GPUComputePipeline *accumulate_pipeline;
	SDL_GPUComputePipeline *finish_pipeline;
	SDL_GPUComputePipeline *finish_layers_pipeline;  // the layers' end: all four into one RGBA8 texel
	SDL_GPUComputePipeline *layers_pipeline;
	SDL_GPUComputePipeline *bounce_pipeline;
	SDL_GPUComputePipeline *bounce_finish_pipeline;
	bool failed;                       // the shaders did not load, or no R8 storage: none
	bool log_pending;                  // just worked out: its numbers to the log (sky_occlusion_log, between frames)
	bool given_up;                     // no room on the GPU for working it out (until asked again, or a new map)
	// While it is worked out: the directions, how many are done, the sums
	// (R32F a cell), the depth map of one direction
	bool working;
	float directions[SKY_OCCLUSION_MOST_DIRECTIONS][4];
	int direction_count;
	int directions_done;
	SDL_GPUTexture *sum_texture;
	SDL_GPUTexture *weight_texture;    // the cosines' sums (R32F)
	SDL_GPUTexture *next_texture;      // the result, until it is done
	SDL_GPUTexture *layer_sum_texture;
	SDL_GPUTexture *layer_weight_texture;
	SDL_GPUTexture *next_layers_texture;
	SDL_GPUTexture *bounce_sums_texture;   // R32F, five sums one under the other
	SDL_GPUTexture *next_bounce_texture;
	shadow_map_t view;
	uint64_t start_counter;
} sky_occlusion_t;

bool sky_occlusion_create(sky_occlusion_t *occlusion);
void sky_occlusion_destroy(sky_occlusion_t *occlusion);

// A new map: none (and to be worked out if requested)
void sky_occlusion_forget(sky_occlusion_t *occlusion, bool request);

// Each frame, inside it (before the shadow maps; the objects' shadow
// pipelines draw the depth maps): when requested, a few more directions of
// it (render.sky_occlusion_directions_per_frame), and at the last the
// result. The terrain's textures must be up (height_texture; its chunk
// buffers draw it). True while it is working on it
bool sky_occlusion_step(sky_occlusion_t *occlusion, const terrain_t *terrain, objects_t *objects, const render_settings_t *settings,
	SDL_GPUTexture *height_texture, SDL_GPUBuffer *chunk_vertex_buffer, SDL_GPUBuffer *chunk_index_buffer, int chunk_index_count);

// From a saved map: map_size x map_size bytes, and the layers (layer_size x
// SKY_OCCLUSION_LAYERS * layer_size bytes)
bool sky_occlusion_upload(sky_occlusion_t *occlusion, int map_size, const uint8_t *cells, int layer_size, const uint8_t *layers, int bounce_size,
	const uint8_t *bounce);

// Between frames: what came out (how much sky the cells see: the least,
// the mean, tenths), if it was just worked out
void sky_occlusion_log(sky_occlusion_t *occlusion);
// Between frames (the game's and the editor's): its cells to the CPU if
// just worked out, and its numbers to the log
void sky_occlusion_between_frames(sky_occlusion_t *occlusion);
// How much of the sky the ground at (x, z) sees (meters; between the
// cells, as the texture filters), 0..1; 1 if it is not known yet
float sky_occlusion_open_at(const sky_occlusion_t *occlusion, float cell_size_meters, float x, float z);
// The same on average over a square radius_meters round (x, z): a place
// at the edge of a wood is neither the wood's dark nor the field's light
float sky_occlusion_open_around(const sky_occlusion_t *occlusion, float cell_size_meters, float x, float z, float radius_meters);

// For saving: map_size x map_size bytes (SDL_free them), NULL if there is
// none; with layers_out and bounce_out, the layers and the bounce texture
// too (bounce_size x bounce_size x 4 bytes; SDL_free them; NULL: none)
uint8_t *sky_occlusion_download(sky_occlusion_t *occlusion, uint8_t **layers_out, uint8_t **bounce_out);

#endif
