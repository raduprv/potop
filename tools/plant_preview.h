// The plant editor's view of the plant (tools/plant_editor.c): the plant
// drawn with the game's own object shaders (object.vert, object.frag), so
// it looks as it will in the game (its atlas from memory, its bark from
// its files), standing on a patch of ground marked in meters, in the sun
// and the wind. No shadows yet.

#ifndef PLANT_PREVIEW_H
#define PLANT_PREVIEW_H

#include <stdbool.h>

#include "gpu.h"
#include "plant_grow.h"

typedef struct
{
	SDL_GPUGraphicsPipeline *pipeline;        // solid: the trunk, the bark, the ground
	SDL_GPUGraphicsPipeline *alpha_pipeline;  // cut out (the ground has none: unused)
	// The plant: the game's plants' shaders (plant.vert, plant.frag)
	SDL_GPUGraphicsPipeline *plant_pipeline;
	SDL_GPUGraphicsPipeline *plant_alpha_pipeline;
	SDL_GPUBuffer *plant_buffer;              // what each vertex is on the plant
	object_file_plant_material_t plant_materials[PLANT_MATERIAL_COUNT];
	SDL_GPUBuffer *vertex_buffer;             // position, normal, uv
	SDL_GPUBuffer *tangent_buffer;
	SDL_GPUBuffer *index_buffer;              // 32 bit, the materials one after the other
	uint32_t first_index[PLANT_MATERIAL_COUNT];
	uint32_t index_count[PLANT_MATERIAL_COUNT];
	bool lods[PLANT_MATERIAL_COUNT];
	uint32_t lod_index_count[PLANT_MATERIAL_COUNT][3];
	float minimum[3];
	float maximum[3];
	float horizontal_radius;                  // its farthest point from the vertical axis (the snow's sag, plant.glsl)
	SDL_GPUBuffer *instance_buffer;           // the one plant (and the ground), where it stands
	SDL_GPUBuffer *light_buffers[2];          // the local lights: none
	SDL_GPUBuffer *ground_vertex_buffer;
	SDL_GPUBuffer *ground_tangent_buffer;
	SDL_GPUBuffer *ground_index_buffer;
	SDL_GPUTexture *ground_texture;
	SDL_GPUTexture *atlas_texture;            // the atlas
	// Its pictures that show others in some months (object_format.h)
	object_file_month_cell_t month_cells[PLANT_MAX_MONTH_CELLS];
	int month_cell_count;
	SDL_GPUTexture *bark_textures[3];         // NULL: plain (bark_plain), the neutral maps
	SDL_GPUTexture *bark_plain;
	SDL_GPUTexture *shadow_dummy;             // the shadow maps' places (no shadows)
	SDL_GPUTexture *neutral;                  // gray: the variation, the clouds' shadows
	bool has_plant;
} plant_preview_t;

// How it is seen
typedef struct
{
	float eye[3];
	float target[3];
	float vertical_fov_degrees;
	float aspect;
	float sun_azimuth_degrees;
	float sun_elevation_degrees;
	float wind;                               // 0: still .. 1: a gale
	float sway;                               // the plant's own (its recipe's: plant_recipe_wind)
	float flutter;
	float stiffness;                          // how hard it must blow before it bends (0..1)
	float ambient;                            // the sky's light (the game's render.ambient_intensity)
	float time_seconds;
	int detail;                               // the level of detail (0: all; 1..3: fewer branches, see object_format.h)
	float month;                              // the game's render.month (1 .. 13; season.h)
	float snow_first_month;                   // the snow's months (render.snow_first_month, snow_last_month)
	float snow_last_month;
	float tips_first_month;                   // the new growth's (render.tips_first_month, tips_last_month)
	float tips_last_month;
	float foliage_kept;                       // of the foliage's cards, the first this many (0..1; shuffled, so evenly thinned)
} plant_view_t;

bool plant_preview_create(plant_preview_t *preview);
void plant_preview_destroy(plant_preview_t *preview);

void plant_preview_set_mesh(plant_preview_t *preview, const plant_mesh_t *mesh);
void plant_preview_set_atlas(plant_preview_t *preview, const plant_atlas_t *atlas);
// The bark from its files (three: color, normal, arm; empty: none, a plain
// color: plain_color, sRGB), as seen from here or under the data directory
void plant_preview_set_bark(plant_preview_t *preview, const char (*bark)[PLANT_PATH_LENGTH], const float plain_color[3],
	const char *data_directory);

// Draws the ground and the plant into the scene's pass (a render target of
// the scene's format and depth); the scene's block is pushed here
void plant_preview_draw(plant_preview_t *preview, SDL_GPURenderPass *pass, const plant_view_t *view);

// The light the scene is cleared to (linear): the sky
void plant_preview_sky(float color_out[4]);

#endif
