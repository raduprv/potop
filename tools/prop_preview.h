// The prop editor's view of the prop (tools/prop_editor.c): drawn with the
// game's own prop shaders (prop.vert, prop.frag), so it looks as it will in
// the game, triplanar textures, moss, lichen and snow and all, standing on
// a patch of ground marked in meters, in the sun. No shadows.

#ifndef PROP_PREVIEW_H
#define PROP_PREVIEW_H

#include <stdbool.h>

#include "gpu.h"
#include "prop_grow.h"

typedef struct
{
	SDL_GPUGraphicsPipeline *ground_pipeline;     // object.vert, object.frag
	SDL_GPUGraphicsPipeline *prop_pipeline;       // prop.vert, prop.frag
	SDL_GPUBuffer *vertex_buffer;                 // position, normal, uv
	SDL_GPUBuffer *tangent_buffer;                // tangents (a fence's) or the weathering (a rock's)
	SDL_GPUBuffer *index_buffer;
	// Its materials: their indices at each level of detail (0: their own),
	// their look, their textures (NULL: their factors', solid)
	struct
	{
		uint32_t level_first[4];
		uint32_t level_count[4];
		bool triplanar;
		bool weathered;
		float texture_meters;
		SDL_GPUTexture *textures[3];
		char texture_names[3][PROP_PATH_LENGTH];
		SDL_GPUTexture *solid[2];             // its color and arm from its factors
	} materials[PROP_MAX_MATERIALS];
	int material_count;
	float moss;
	float lichen;
	float minimum[3];
	float maximum[3];
	SDL_GPUBuffer *instance_buffer;
	SDL_GPUBuffer *light_buffers[2];
	SDL_GPUBuffer *ground_vertex_buffer;
	SDL_GPUBuffer *ground_tangent_buffer;
	SDL_GPUBuffer *ground_index_buffer;
	SDL_GPUTexture *ground_texture;
	SDL_GPUTexture *shadow_dummy;
	SDL_GPUTexture *neutral;
	bool has_prop;
} prop_preview_t;

typedef struct
{
	float eye[3];
	float target[3];
	float vertical_fov_degrees;
	float aspect;
	float sun_azimuth_degrees;
	float sun_elevation_degrees;
	float ambient;
	float time_seconds;
	float month;                                  // render.month (season.h): the snow
	float snow_first_month;
	float snow_last_month;
	float wetness;                                // 0..1 (after the rain)
	bool pbr;
	int detail;                                   // the level of detail drawn (0: its own .. 3)
	int show;                                     // 0: as it is; 1: the crevices, 2: the edges, 3: the sky (the weathering, as gray)
} prop_view_t;

bool prop_preview_create(prop_preview_t *preview);
void prop_preview_destroy(prop_preview_t *preview);

void prop_preview_set_mesh(prop_preview_t *preview, const prop_mesh_t *mesh);
// Its textures, from their files under the data directory (loaded again
// only when their names change, or if reload)
void prop_preview_set_textures(prop_preview_t *preview, const prop_mesh_t *mesh, const char *data_directory, bool reload);

void prop_preview_draw(prop_preview_t *preview, SDL_GPURenderPass *pass, const prop_view_t *view);
void prop_preview_sky(float color_out[4]);

#endif
