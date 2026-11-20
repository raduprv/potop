// A new map's sketch: what the maker wants where, painted roughly over the
// whole map from above (the editor's sketch window) before the terrain is
// made from it (terrain_gen.h).
//
// Painted on a blank sheet in regions, flat like a map's legend: a plain
// here, hills there, mountains, a valley, a lake. The generator turns them
// into land (terrain_gen.c): each kind its own height and roughness, the
// seed's noise giving the shapes within it (the mountains' crests and
// peaks, the hills' rolls), the kinds blended into each other across their
// borders. Nothing painted, the land is the seed's alone; once anything is,
// the unpainted is low, gently rolling land.
//
// TERRAIN_SKETCH_SIZE texels a side over the whole map, whatever its size,
// two planes of a byte each: the region (terrain_sketch_region_t) and the
// water (255 in a lake, else 0; follows the region). Kept in a map
// project's sketch folder as regions.png (gray: the region times
// TERRAIN_SKETCH_PNG_STEP), so it can be painted in an image editor too.

#ifndef TERRAIN_SKETCH_H
#define TERRAIN_SKETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERRAIN_SKETCH_SIZE 1024
#define TERRAIN_SKETCH_LAKES_MAX 256
// Painted this much (of 255) or more, a texel is in a lake
#define TERRAIN_SKETCH_LAKE_THRESHOLD 90
#define TERRAIN_SKETCH_PNG_STEP 40

typedef enum
{
	TERRAIN_SKETCH_NONE = 0,       // not painted
	TERRAIN_SKETCH_PLAIN,
	TERRAIN_SKETCH_HILLS,
	TERRAIN_SKETCH_MOUNTAINS,
	TERRAIN_SKETCH_VALLEY,
	TERRAIN_SKETCH_LAKE,
	TERRAIN_SKETCH_REGION_COUNT
} terrain_sketch_region_t;

typedef enum
{
	TERRAIN_SKETCH_REGION = 0,
	TERRAIN_SKETCH_WATER,
	TERRAIN_SKETCH_PLANES
} terrain_sketch_plane_t;

// What the regions make (terrain_gen.c), the sketch window's Land: each
// region's height (a share of the highest ground), its rolling land (the
// seed's hills' noise: 1 some 8% of the highest ground either way), its
// crests (the seed's ridged noise: how high they rise, a share of the
// highest ground); how far the regions blend across their borders (a share
// of the map's side), how big the crests are (how many across the map, as
// the land's feature count says, times this), how deep a lake is at most
// (meters under its surface). Saved with the sketch (land.ini)
typedef struct
{
	float height[TERRAIN_SKETCH_REGION_COUNT];
	float roll[TERRAIN_SKETCH_REGION_COUNT];
	float crests[TERRAIN_SKETCH_REGION_COUNT];
	float blend_share;
	float crest_scale;
	float lake_depth_meters;
} terrain_sketch_land_t;

typedef struct
{
	terrain_sketch_land_t land;
	uint8_t *planes;               // TERRAIN_SKETCH_PLANES planes of TERRAIN_SKETCH_SIZE squared, one after the other
	uint32_t revision;             // grows with every change
	// The lakes the last terrain made from it found (terrain_gen.c): each
	// texel's lake (0: none, else 1 + its index), each lake's surface
	// (meters); the world fills them with water after (world.c)
	uint16_t *lake_labels;
	float lake_surfaces[TERRAIN_SKETCH_LAKES_MAX];
	int lake_count;
} terrain_sketch_t;

bool terrain_sketch_create(terrain_sketch_t *sketch);
void terrain_sketch_destroy(terrain_sketch_t *sketch);

// A blank sheet (what the regions make left as it is)
void terrain_sketch_clear(terrain_sketch_t *sketch);

// What the regions make, as it starts
void terrain_sketch_land_defaults(terrain_sketch_land_t *land);

// True if any region is painted
bool terrain_sketch_painted(const terrain_sketch_t *sketch);

static inline uint8_t *terrain_sketch_plane(const terrain_sketch_t *sketch, terrain_sketch_plane_t plane)
{
	return sketch->planes + (size_t)plane * TERRAIN_SKETCH_SIZE * TERRAIN_SKETCH_SIZE;
}

// A disc of the region round (u, v) (0..1 over the map), radius a share of
// its side (TERRAIN_SKETCH_NONE: rubbed out)
void terrain_sketch_paint(terrain_sketch_t *sketch, terrain_sketch_region_t region, float u, float v, float radius);

// The same along a stroke, from one point to the next (no gaps between)
void terrain_sketch_paint_line(terrain_sketch_t *sketch, terrain_sketch_region_t region, float from_u, float from_v, float to_u, float to_v,
	float radius);

// The region at (u, v)
terrain_sketch_region_t terrain_sketch_region_at(const terrain_sketch_t *sketch, float u, float v);

// The water plane at (u, v), between the texels (0..1)
float terrain_sketch_water(const terrain_sketch_t *sketch, float u, float v);

// The lake at (u, v): its index (-1: none)
int terrain_sketch_lake_at(const terrain_sketch_t *sketch, float u, float v);

// In a folder (ending in a slash): regions.png and land.ini. Load: false
// if there is no regions.png (the sketch then blank, the land as it starts)
bool terrain_sketch_save(const terrain_sketch_t *sketch, const char *folder);
bool terrain_sketch_load(terrain_sketch_t *sketch, const char *folder);

#endif
