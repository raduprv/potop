// Terrain data: the authoritative height field, soil types and the chunk
// bookkeeping used by culling and GPU uploads.
//
// Heights are stored as integers in TERRAIN_HEIGHT_UNITS_PER_METER units
// (0.01 mm). The water simulation will use the same unit for water depth and
// sediment, so that erosion can move material between terrain, sediment and
// water without any rounding loss: every unit taken from one place is added
// to exactly one other place.
//
// Each cell is a column of layers, from the bottom up:
//   bedrock    below bedrock_heights: does not erode, does not absorb water
//   soil       of the cell's soil type, up to heights - silt_thickness
//   silt       material the water deposited, silt_thickness thick
// heights is the top of the column (what you walk on).
//
// Layout: size x size cells, row major, index = y * size + x. World X runs
// along the grid x axis, world Z along the grid y axis, world Y is up. A
// cell's height is sampled at its center, which sits at
// (x * cell_size, height, y * cell_size).

#ifndef TERRAIN_H
#define TERRAIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "math3d.h"

#define TERRAIN_CHUNK_SIZE 64
#define TERRAIN_HEIGHT_UNITS_PER_METER 100000
// Bytes per cell of the material map: base, cover, overlay, shelter
#define TERRAIN_MATERIAL_BYTES 4
#define TERRAIN_METERS_PER_HEIGHT_UNIT (1.0f / (float)TERRAIN_HEIGHT_UNITS_PER_METER)
// A baked map's surface per cell (terrain_t.baked_surface): the silt shown,
// the ground's wetness, the bare bedrock shown (0..255 each: the terrain
// renderer's surface texture, as the active map drew it)
#define TERRAIN_BAKED_SURFACE_BYTES 3
#define TERRAIN_BAKED_CHUNK_CELLS (TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE)

// The order matches the soil selection keys (5 6 7 8) and the soil indices
// used by the terrain shader. Silt is not a soil type a cell can have: it
// is the deposit layer on top. It has an index here only so that its
// properties sit in the same table as the others (see soils.h).
typedef enum
{
	SOIL_SAND = 0,
	SOIL_EARTH,
	SOIL_CLAY,
	SOIL_ROCK,
	SOIL_TYPE_COUNT,
	SOIL_SILT = SOIL_TYPE_COUNT,
	SOIL_PROPERTY_COUNT
} soil_type_t;

typedef struct
{
	// Height range of all samples the chunk's mesh touches, in meters. The
	// mesh of a chunk spans TERRAIN_CHUNK_SIZE + 1 samples per side, so this
	// includes the first row and column of the neighboring chunks.
	float minimum_height_meters;
	float maximum_height_meters;
	// Set when anything the renderer shows changed (heights, soil, silt,
	// ground wetness) and the GPU copy needs refreshing. The renderer clears
	// it after uploading.
	bool needs_upload;
	// Set (together with needs_upload) when the shape or the soil changed,
	// not just the wetness: then normals and soil weights are redone too
	bool needs_shape_upload;
	// Set when erosion changed heights: the height range above is
	// recomputed by terrain_update_chunk_bounds (once per frame)
	bool bounds_dirty;
	// For the physics' copy of the ground (see physics.h): the shape was
	// changed by tools or stamps (redone at once), or worn by erosion
	// (checked now and then, redone only if it changed a lot)
	bool physics_dirty;
	bool physics_eroded;
	// Edited (terrain_mark_cells_changed): the GPU's copy for the water
	// simulation must follow (water_gpu.c clears it)
	bool sim_dirty;
	// The objects standing on it must be put back on the ground: edited
	// (at once), or worn by erosion by more than objects sink into the
	// ground (objects_follow_terrain clears it). erosion_units adds up the
	// erosion since they were last put down.
	bool objects_dirty;
	bool objects_pass;             // being gone through (objects.c)
	// Edited since the objects were last gone through (a tool, a stamp, a
	// crater: terrain_mark_cells_changed), not only worn or silted by the
	// water: the objects follow the ground up too. Under what the water
	// lays down they stay where they stand, buried
	bool objects_edited;
	bool objects_pass_edited;      // ...as it was when the pass began
	int32_t objects_erosion_units;
	// Its part of the material map changed (material_map.c): the renderer
	// uploads it and clears this
	bool materials_changed;
} terrain_chunk_t;

typedef struct
{
	int32_t size;                  // cells per side, multiple of TERRAIN_CHUNK_SIZE
	float cell_size_meters;
	// A baked map (map_file.h): its ground stays as it is (no tools, no
	// erosion); no layers under the surface (bedrock_heights and
	// silt_thickness are NULL), what showed of them in baked_surface:
	// per chunk, NULL where nothing does, else TERRAIN_BAKED_CHUNK_CELLS
	// cells of TERRAIN_BAKED_SURFACE_BYTES, row by row
	bool baked;
	uint8_t **baked_surface;
	// ...and its heights as they were stored: base + a multiple of step
	// (units), so that saving it again keeps them exactly
	int32_t baked_height_base_units;
	int32_t baked_height_step_units;
	uint32_t seed;
	float max_height_meters;       // from generation, used for snow line etc.

	int32_t *heights;              // top of the column, TERRAIN_HEIGHT_UNITS_PER_METER units
	int32_t *bedrock_heights;      // top of the bedrock, same units
	int32_t *silt_thickness;       // deposited silt on top, same units
	uint8_t *soil_types;           // soil_type_t values (never SOIL_SILT)
	// The material map (material_map.h): per cell TERRAIN_MATERIAL_BYTES,
	// what the ground shows there (its base material, the cover over it,
	// the overlay over that, how sheltered from above it is)
	uint8_t *materials;
	// Its base must be worked out again here (inclusive rectangle, valid
	// while materials_dirty): the shape or the soils changed
	bool materials_dirty;
	int32_t materials_dirty_rectangle[4];
	bool any_chunk_materials_changed;
	// Moves whenever a chunk's height range is made again (its
	// minimum_height_meters, maximum_height_meters): who keeps a copy of
	// those (the renderer's, side by side for its culling) knows it is old
	uint32_t chunk_bounds_serial;
	// The material map's ground bytes were cleared (a restamp, a load): the
	// objects' footprints are to be worked into it again (ground_footprints.h)
	bool ground_footprints_stale;

	// Shadow bookkeeping: tools and stamps set shadow_dirty (both shadow
	// maps). Erosion only matters to the fine map near the camera (the far
	// one does not show centimeters): it adds the largest change of each
	// copy back to shadow_change_units, and once that passes a threshold
	// sets eroded_shadow_pending with the rectangle (cells: minimum x, z,
	// maximum x, z) where the ground changed meanwhile
	bool shadow_dirty;
	int64_t shadow_change_units;
	bool eroded_shadow_pending;
	int32_t eroded_rectangle[4];
	bool eroded_rectangle_valid;

	int32_t chunks_per_side;
	terrain_chunk_t *chunks;
	bool any_chunk_needs_upload;
	bool any_chunk_bounds_dirty;
	bool any_chunk_physics_dirty;
	bool any_chunk_sim_dirty;
	bool any_chunk_objects_dirty;
	// The cells tools and stamps changed since the physics last looked
	// (inclusive rectangle; valid while any_chunk_physics_dirty)
	int32_t physics_dirty_rectangle[4];
} terrain_t;

// Allocates a flat terrain. size is rounded up to a multiple of the chunk
// size. Returns false on allocation failure.
bool terrain_create(terrain_t *terrain, int32_t size, float cell_size_meters);
// A baked map's (see baked above)
bool terrain_create_baked(terrain_t *terrain, int32_t size, float cell_size_meters);
void terrain_destroy(terrain_t *terrain);

static inline int32_t terrain_index(const terrain_t *terrain, int32_t x, int32_t y)
{
	return y * terrain->size + x;
}

static inline int32_t terrain_height_units_clamped(const terrain_t *terrain, int32_t x, int32_t y)
{
	x = clamp_int(x, 0, terrain->size - 1);
	y = clamp_int(y, 0, terrain->size - 1);
	return terrain->heights[terrain_index(terrain, x, y)];
}

static inline float terrain_height_meters_clamped(const terrain_t *terrain, int32_t x, int32_t y)
{
	return (float)terrain_height_units_clamped(terrain, x, y) * TERRAIN_METERS_PER_HEIGHT_UNIT;
}

// Bilinear height at a world position (meters). Positions outside the map
// are clamped to the edge.
float terrain_sample_height_meters(const terrain_t *terrain, float world_x, float world_z);

// Converts a world position to the nearest cell. Returns false if outside.
bool terrain_world_to_cell(const terrain_t *terrain, float world_x, float world_z, int32_t *cell_x_out, int32_t *cell_y_out);

// Keeps the layers consistent after heights changed in a rectangle: the
// bedrock is lowered where the surface went below it, and silt is clamped
// to the column. Tools and stamps call this; erosion keeps them right itself.
void terrain_fix_layers(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y);

// Marks an inclusive rectangle of cells as modified: recomputes the height
// bounds of every chunk whose mesh touches it and flags them for upload.
void terrain_mark_cells_changed(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y);

// Same, for small gradual changes (erosion), which happen every step: the
// chunks are only flagged, and their height ranges recomputed later by
// terrain_update_chunk_bounds. The shadow map is not flagged either; the
// caller accounts for the change in shadow_change_units instead.
void terrain_mark_cells_eroded(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y);

// Recomputes the height range of every chunk flagged by
// terrain_mark_cells_eroded. Call before anything uses the ranges
// (culling, shadows), once per frame is enough.
void terrain_update_chunk_bounds(terrain_t *terrain);

// Same for the whole map (after generation).
void terrain_mark_all_changed(terrain_t *terrain);

// The material map's bases must be worked out again in a rectangle (and as
// far around it as the rules look: material_map.c widens it)
void terrain_mark_materials_dirty(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y);

// Its bytes changed in a rectangle (cover, overlay or shelter written
// directly): the chunks there go up again
void terrain_mark_materials_changed(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y);

// What its cells take (bytes), for the log
size_t terrain_memory_bytes(const terrain_t *terrain);

// Intersects a ray with the height field. Returns true and the hit point if
// the ray hits the terrain within max_distance meters.
bool terrain_raycast(const terrain_t *terrain, vec3_t origin, vec3_t direction, float max_distance, vec3_t *hit_out);

#endif
