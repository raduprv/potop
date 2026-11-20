// A new map's sketch: what the maker wants where, painted roughly over the
// whole map from above (the editor's sketch window) before the terrain is
// made from it (terrain_gen.h).
//
// TERRAIN_SKETCH_SIZE texels a side over the whole map, whatever its size,
// five planes of a byte each:
//
//   height          how high the land is there: 0 the lowest .. 255 the
//                   map's highest (a share of max_height_meters)
//   height weight   how much is painted: 0 none (the unpainted height) ..
//                   255 the height above
//   relief          how rough: 0 smooth (a plain) .. 128 as the seed makes it
//                   .. 255 crags (more of the seed's detail, ridges besides)
//   relief weight   as the height's
//   water           a lake: 0 none .. 255 a deep one (the land under it
//                   sunk, filled to below its lowest shore)
//
// Painted on a blank sheet: once anything is painted the land's shape is
// the sketch's alone (the seed gives only its detail), the unpainted
// height TERRAIN_SKETCH_UNPAINTED_HEIGHT; nothing painted, the seed's land.
//
// The brushes paint several planes at once (a mountain: high and rough; a
// plain: low and smooth); erasing takes the weights back to the seed's.
// Kept in a map project's sketch folder as PNGs (height.png and relief.png
// gray with the weight in alpha, water.png gray), so they can be painted in
// an image editor too.

#ifndef TERRAIN_SKETCH_H
#define TERRAIN_SKETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERRAIN_SKETCH_SIZE 1024
#define TERRAIN_SKETCH_LAKES_MAX 256
// Painted this much (of 255) or more, a texel is in a lake
#define TERRAIN_SKETCH_LAKE_THRESHOLD 90
// Where nothing is painted (of the highest ground): low land
#define TERRAIN_SKETCH_UNPAINTED_HEIGHT 0.12f
// ...and its relief (under the middle: the seed's detail gentler there)
#define TERRAIN_SKETCH_UNPAINTED_RELIEF 0.35f

typedef enum
{
	TERRAIN_SKETCH_HEIGHT = 0,
	TERRAIN_SKETCH_HEIGHT_WEIGHT,
	TERRAIN_SKETCH_RELIEF,
	TERRAIN_SKETCH_RELIEF_WEIGHT,
	TERRAIN_SKETCH_WATER,
	TERRAIN_SKETCH_PLANES
} terrain_sketch_plane_t;

// What a brush paints: each target 0..1, or below 0 to leave that plane be
typedef struct
{
	float height;
	float relief;
	float water;
	bool erase;                    // back to the blank sheet (the weights and the water towards 0)
	// The height a peak: highest at the brush's middle, down to the
	// unpainted towards its edge, and only raising (a mountain, a hill;
	// else flat, either way: a plain, a valley)
	bool peaked;
} terrain_sketch_brush_t;

typedef struct
{
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

// Nothing painted: the seed's alone
void terrain_sketch_clear(terrain_sketch_t *sketch);

// True if anything is painted (a weight or water anywhere)
bool terrain_sketch_painted(const terrain_sketch_t *sketch);

static inline uint8_t *terrain_sketch_plane(const terrain_sketch_t *sketch, terrain_sketch_plane_t plane)
{
	return sketch->planes + (size_t)plane * TERRAIN_SKETCH_SIZE * TERRAIN_SKETCH_SIZE;
}

// Paints round (u, v) (0..1 over the map), radius a share of its side:
// amount (0..1, how far towards the targets at the middle) fading to
// nothing at the edge
void terrain_sketch_paint(terrain_sketch_t *sketch, const terrain_sketch_brush_t *brush, float u, float v, float radius, float amount);

// A plane at (u, v), between the texels (0..1)
float terrain_sketch_sample(const terrain_sketch_t *sketch, terrain_sketch_plane_t plane, float u, float v);

// The lake at (u, v): its index (-1: none)
int terrain_sketch_lake_at(const terrain_sketch_t *sketch, float u, float v);

// In a folder (ending in a slash): height.png, relief.png, water.png.
// Load: false if there is none (the sketch then cleared)
bool terrain_sketch_save(const terrain_sketch_t *sketch, const char *folder);
bool terrain_sketch_load(terrain_sketch_t *sketch, const char *folder);

#endif
