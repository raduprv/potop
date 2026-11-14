// Where the volumetric clouds are and what shape each one has (see
// clouds.h).
//
// Each cloud is a shape from an atlas, stamped somewhere on a map of the
// sky CLOUD_WEATHER_METERS a side (it repeats, and the wind carries it),
// turned and scaled. The atlas is made once, on the CPU, out of
// overlapping blobs, the way real clouds heap up: fair weather puffs,
// cauliflowers, towers, storm clouds spreading into an anvil, flat sheets.
// Each shape is kept as a small 3D distance field (how far each point is
// from the cloud's surface), so the clouds are solid 3D forms with bulging
// sides and overhangs; the clouds' shader only roughens their surfaces with
// noise.
//
// On the GPU:
//   atlas_texture   the shapes' distance fields, in one 3D texture
//   stamp_buffer    the stamps: where, how big, turned how, which shape
//   map_texture     for each patch of the sky (CLOUD_WEATHER_MAP_SIZE a
//                   side) the stamps over it, up to four (0: none)
//
// The stamps are scattered at random by the cloud settings: how many
// (coverage), how far apart (cloud_tiling_meters), how big (cloud_size)
// and how tall (cloud_towers; storm clouds at the top of it, flat sheets
// for an overcast sky). They are made again when those change.

#ifndef CLOUD_WEATHER_H
#define CLOUD_WEATHER_H

#include <stdbool.h>
#include <stdint.h>

#include "cloud_paint.h"
#include "gpu.h"
#include "settings.h"

// The map's side (meters; the wind's offset wraps at a multiple of it, see
// terrain_renderer_cloud_uniforms), and its patches a side
#define CLOUD_WEATHER_METERS 25000.0f
#define CLOUD_WEATHER_MAP_SIZE 512
// Each shape's distance field: texels across, up, and how far up it
// reaches (of its width); the atlas holds the shapes in a grid of
// CLOUD_ATLAS_COLUMNS x CLOUD_ATLAS_ROWS (x, z)
#define CLOUD_SHAPE_TEXELS 64
#define CLOUD_SHAPE_TEXELS_UP 48
#define CLOUD_SHAPE_REACH_UP 0.75f
#define CLOUD_ATLAS_COLUMNS 8
#define CLOUD_ATLAS_ROWS 10
// Shapes of each type of cloud (the five types fill the atlas); simulated
// ones (data/clouds/<type>_<n>.cloud, n from 1) take their places
#define CLOUD_SHAPES_PER_TYPE 16
// The distance fields' range (shape widths each way from the surface)
#define CLOUD_SHAPE_DISTANCE_RANGE 0.1f
// Clouds made elsewhere (data/clouds/cumulus_<n>.cloud, from VDB files by
// tools/cloud_bake --vdb): their real density, in an atlas of their own
// (BC4 where the GPU has it), each at its own size. When there are any, the
// cumulus stamps use them instead of the shapes made of blobs. The stamp
// buffer starts with their table: two vec4 each (atlas texel origin x, z,
// size x, z; size y)
#define CLOUD_DENSITY_SHAPES_MAX 64
#define CLOUD_DENSITY_TABLE_VEC4S (CLOUD_DENSITY_SHAPES_MAX * 2)

typedef struct
{
	int32_t origin[3];        // in the atlas (texels)
	int32_t size[3];
	float meters;             // a texel's size in the file (its natural size)
} cloud_density_shape_t;

typedef struct
{
	float shape_tops[CLOUD_ATLAS_COLUMNS * CLOUD_ATLAS_ROWS]; // each shape's highest point (of its width)
	bool atlas_made;
	SDL_GPUTexture *atlas_texture;
	SDL_GPUTexture *density_atlas_texture;   // never NULL once the atlas is made (1 texel if no shapes)
	cloud_density_shape_t density_shapes[CLOUD_DENSITY_SHAPES_MAX];
	int32_t density_shape_count;
	SDL_GPUTexture *map_texture;
	SDL_GPUBuffer *stamp_buffer;
	uint32_t stamp_capacity;
	int32_t stamp_count;
	float made_for[8];          // the settings the stamps were made for
	bool made;
	uint32_t made_revision;     // the painted sky's revision they were made for
	uint64_t made_ticks;
} cloud_weather_t;

// Makes the stamps again if the cloud settings or the painted sky (paint,
// may be NULL: as the settings say) changed (not more often than a few
// times a second while a slider is dragged or the sky painted);
// rebuilt_out: whether it did (the uploads are waiting then). False if
// there are none.
bool cloud_weather_update(cloud_weather_t *weather, const render_settings_t *settings, const cloud_paint_t *paint, bool *rebuilt_out);

void cloud_weather_destroy(cloud_weather_t *weather);

#endif
