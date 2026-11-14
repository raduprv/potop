// A house drawn as the game draws it (tools/house_view.c, the headless
// viewer, and tools/house_editor.c, the house editor): its aged materials
// through the game's building shaders (building.vert, building.frag: moss,
// lichen, the dirt up its walls, snow), its weathered ones through the prop
// shaders, the rest through the object shaders, standing on a patch of
// ground marked in meters, in the sun. No shadows.

#ifndef HOUSE_PREVIEW_H
#define HOUSE_PREVIEW_H

#include <stdbool.h>

#include "gpu.h"
#include "house_grow.h"

#define HOUSE_PREVIEW_TEXTURE_CACHE 64

typedef struct
{
	char name[HOUSE_NAME_LENGTH];
	uint32_t level_first[HOUSE_LEVELS];
	uint32_t level_count[HOUSE_LEVELS];
	uint32_t flags;
	float texture_meters;                         // a repeat (OBJECT_MATERIAL_TRIPLANAR: laid on from the sides)
	SDL_GPUTexture *textures[4];                  // color, normal, arm, emissive (NULL: from the factors; the cache's)
	SDL_GPUTexture *solid[3];                     // color, arm, emissive from the factors (its own)
} house_preview_material_t;

typedef struct
{
	SDL_GPUGraphicsPipeline *object_pipeline;
	SDL_GPUGraphicsPipeline *object_pipeline_two_sided;
	SDL_GPUGraphicsPipeline *prop_pipeline;
	SDL_GPUGraphicsPipeline *building_pipeline;
	SDL_GPUBuffer *vertex_buffer;
	SDL_GPUBuffer *tangent_buffer;
	SDL_GPUBuffer *weathering_buffer;             // the vertices' baked weathering (building.vert)
	SDL_GPUBuffer *index_buffer;
	SDL_GPUBuffer *instance_buffer;
	SDL_GPUBuffer *light_buffers[2];
	SDL_GPUBuffer *ground_vertex_buffer;
	SDL_GPUBuffer *ground_tangent_buffer;
	SDL_GPUBuffer *ground_index_buffer;
	SDL_GPUTexture *ground_texture;
	SDL_GPUTexture *shadow_dummy;
	SDL_GPUTexture *neutral;
	house_preview_material_t materials[HOUSE_MAX_MATERIALS];
	int material_count;
	float moss;
	float lichen;
	float minimum[3];
	float maximum[3];
	bool has_house;
	// The textures read so far, by their names (a house made again with the
	// same textures reads none)
	struct
	{
		char name[HOUSE_PATH_LENGTH];
		SDL_GPUTexture *texture;
	} cache[HOUSE_PREVIEW_TEXTURE_CACHE];
	int cache_count;
} house_preview_t;

typedef struct
{
	float eye[3];
	float target[3];
	float vertical_fov_degrees;                   // 0: 38
	float aspect;
	float sun_azimuth_degrees;
	float sun_elevation_degrees;
	float ambient;                                // the sky's light (0: 0.5)
	float month;                                  // season.h's: the snow's months make snow
	float snow_first_month;
	float snow_last_month;
	float wetness;                                // 0..1 after the rain
	int detail;                                   // the level of detail drawn, 0 (its own) .. 3
	bool show_collision;                          // the collision shell too
	const char *hide;                             // a material not drawn (to see what is behind it; NULL: none)
	int show;                                     // 0: as it is; the baked weathering as gray: 1 how open to the sky, 2 the edges, 3 the shelter
	float age;                                    // how old the house is drawn (0 new .. 1 very old)
	float dirt_meters;                            // how high the dirt splashes up its walls
} house_view_t;

bool house_preview_create(house_preview_t *preview);
void house_preview_destroy(house_preview_t *preview);
// The house to draw (what was drawn before let go; its textures read from
// under the data directory, those read before taken from the cache)
bool house_preview_set_mesh(house_preview_t *preview, const house_mesh_t *mesh, const char *data_directory);
// The textures read again from their files the next time (they were made
// again)
void house_preview_forget_textures(house_preview_t *preview);
void house_preview_draw(house_preview_t *preview, SDL_GPURenderPass *pass, const house_view_t *view);
void house_preview_sky(float color_out[4]);

#endif
