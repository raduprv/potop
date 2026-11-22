// Impostors: an object far away drawn as a picture of itself. A model worth
// one (enough triangles, big enough) gets an atlas of pictures of it taken
// from all round and from above (a hemisphere, unfolded on a square grid,
// see data/shaders/impostor.glsl): its colors, and its normals with their
// depth. Drawn, a square faces the eye and shows the three pictures taken
// from nearest where the eye is, blended, lit like the object would be
// (impostor.frag).
//
// The objects' culling picks, for each object, the model or its impostor:
// the impostor once the object is smaller on screen than a picture of the
// atlas (IMPOSTOR_CELL_PIXELS across), so that it never shows less detail
// than the model would there. The shadow maps keep drawing the models.
//
// The atlases are baked on the GPU when a model is loaded, and kept in
// data/cache/impostors/ (compressed), under a key made of the model's and
// its textures' files and when they were written, and of how the pictures
// are made: a model or texture changed on disk is baked again, the rest
// loads from the cache.

#ifndef IMPOSTORS_H
#define IMPOSTORS_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
#include "object_model.h"

#define IMPOSTOR_GRID 8                // pictures across the grid (impostor.glsl)
#define IMPOSTOR_CELL_PIXELS 64        // each picture's size
#define IMPOSTOR_ATLAS_PIXELS (IMPOSTOR_GRID * IMPOSTOR_CELL_PIXELS)
// A plant's (a model with a plant buffer): pictures larger, from round it
// at a few heights only (impostor.glsl): a row a height, a column a side
#define IMPOSTOR_PLANT_AZIMUTHS 8      // impostor.glsl's
#define IMPOSTOR_PLANT_ELEVATIONS 3
#define IMPOSTOR_PLANT_CELL_PIXELS 128
// The baker's targets: the largest atlas of either
#define IMPOSTOR_BAKE_WIDTH (IMPOSTOR_PLANT_AZIMUTHS * IMPOSTOR_PLANT_CELL_PIXELS)
#define IMPOSTOR_BAKE_HEIGHT IMPOSTOR_ATLAS_PIXELS
// Models with fewer triangles than this, or smaller (bounding sphere
// radius), are cheaper drawn as they are
#define IMPOSTOR_MINIMUM_TRIANGLES 150
#define IMPOSTOR_MINIMUM_RADIUS_METERS 1.0f
// Raise when the pictures are made differently: every cached atlas is
// baked again
#define IMPOSTOR_VERSION 2
// impostor.frag's samplers: the shadow maps (0, 1), the variation (2), the
// color atlas (3), the clouds' shadows (4), the normal atlas (5)
#define IMPOSTOR_FRAGMENT_SAMPLERS 6

typedef struct
{
	SDL_GPUTexture *albedo;        // RGBA8, mipmapped: sRGB color, alpha covered
	SDL_GPUTexture *normal;        // RGBA8, mipmapped: normal in the model's axes * 0.5 + 0.5 (half as long: leaves), alpha depth
	float sphere[4];               // the model's bounding sphere: center, radius (model axes, meters at size 1)
	float look[4];                 // x: how leafy, 0..1 (its triangles' share in cut out materials): lit like leaves; y: a plant's (1); z, w: pictures across, down its atlas
	int atlas_width;               // its atlases' size
	int atlas_height;
	int month_index;               // a plant's: the month its foliage's pictures are of (0 January; -1 not a plant)
} impostor_t;

typedef struct
{
	SDL_GPUGraphicsPipeline *bake_pipeline;
	SDL_GPUGraphicsPipeline *plant_bake_pipeline; // a plant's: its plant data too (impostor_bake_plant.vert)
	SDL_GPUTexture *color_target;  // a whole atlas, baked into
	SDL_GPUTexture *depth_target;
	SDL_GPUTransferBuffer *download;
	// Statistics, since created
	int baked;
	int loaded;                    // from the cache
	double seconds;
} impostor_baker_t;

bool impostor_baker_create(impostor_baker_t *baker);
void impostor_baker_destroy(impostor_baker_t *baker);

// Whether a model is worth an impostor
bool impostor_worth_one(const object_model_t *model);

// The model's impostor, from the cache or baked (which waits for the GPU).
// Outside a frame (between gpu_end_frame and gpu_begin_frame). A plant's
// foliage as it is in month (season.h: its autumn, its pictures). False if it could not be
// made (logged)
bool impostor_make(impostor_baker_t *baker, impostor_t *impostor, const object_model_t *model, const object_texture_cache_t *textures,
	float month);
void impostor_release(impostor_t *impostor);

#endif
