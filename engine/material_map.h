// The material map: what the ground shows, per cell.
//
// The CPU decides WHERE and WHAT, the GPU decides HOW MUCH. Per cell the
// terrain keeps TERRAIN_MATERIAL_BYTES (terrain->materials):
//   base     the material the ground itself shows (grass, dirt, rock, sand,
//            clay), worked out once from the soils, the slope, how it faces
//            the noon sun and the noises (material_map_bake), and again
//            only where the shape or the soils are edited. Erosion does not
//            change it: the bare bedrock and the silt it leaves come from
//            the surface state, every frame, as before.
//   cover    what lies over the base where something put it there (fallen
//            needles under a pine, leaves under a beech, trampled ground
//            round a house; MATERIAL_NONE: nothing). A flood takes it away
//            (material_map_clear_flooded_cover).
//   overlay  what can settle over both in amounts the GPU works out (snow;
//            petals under a blooming tree; MATERIAL_NONE: whatever the
//            weather brings, snow)
//   ground   what stands on it, from the objects' footprints at run time
//            (MATERIAL_GROUND_*, ground_footprints.h): no grass under a
//            building, the terrain not drawn under its floor; never saved.
//            (The shelter once: how covered from above a cell is comes from
//            the sky's occlusion now, sky_occlusion.h)
//
// The shaders (terrain_materials.glsl) read the four cells around a pixel
// and break the borders between their materials into patches, as they did
// with the materials they worked out themselves before.
//
// Priority, highest first: snow, silt, the bedrock laid bare, cover, base.

#ifndef MATERIAL_MAP_H
#define MATERIAL_MAP_H

#include <stdint.h>

#include "materials.h"
#include "objects.h"
#include "terrain.h"
#include "water.h"

typedef enum
{
	MATERIAL_MAP_BASE = 0,
	MATERIAL_MAP_COVER,
	MATERIAL_MAP_OVERLAY,
	MATERIAL_MAP_GROUND,           // what stands on it (MATERIAL_GROUND_*: ground_footprints.h; the shelter once)
} material_map_byte_t;

// The ground byte's bits: worked out at run time from the objects'
// footprints (ground_footprints.h), never saved
#define MATERIAL_GROUND_NO_GRASS 1u        // no grass grows (under a building)
#define MATERIAL_GROUND_HIDDEN 2u          // covered whole by a building's floor: the terrain not drawn

// What the bake goes by: the terrain shader's rules
typedef struct
{
	float slope_ranges[4];         // grass from, grass to, cliff from, cliff to
	float sun_x;                   // towards the noon sun, horizontal, unit length
	float sun_z;
	float soil_blend_cells;        // how wide soil borders are, in cells
	int normal_reach;              // the material normal's reach, in cells
	float patch_repeats_per_meter[MATERIAL_COUNT];
	float patch_strength[MATERIAL_COUNT];
	// The variation texture (R8, square, repeating) and its repeats per meter
	const uint8_t *variation;
	int variation_size;
	float variation_repeats_per_meter;
} material_rules_t;

// The rules from the render settings and the loaded materials
void material_rules_make(material_rules_t *rules, const render_settings_t *settings, const material_set_t *materials, float cell_size_meters);

// Works out the base material of every cell in a rectangle (inclusive),
// and marks the chunks there changed. Cover and overlay stay.
void material_map_bake(terrain_t *terrain, const material_rules_t *rules, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x,
	int32_t maximum_y);

// Bakes what terrain->materials_dirty asks for (widened by how far the
// rules look). Returns true if it did anything.
bool material_map_update(terrain_t *terrain, const material_rules_t *rules);

// A new map: no cover, no overlay, open sky everywhere (the base is baked
// by the next update)
void material_map_clear(terrain_t *terrain);

// A disk of cover round (x, z) (meters; its edge ragged): material a cover
// (needles, leaves, trampled) or the petals (an overlay); MATERIAL_NONE,
// or a radius of 0: nothing
void material_map_stamp_disk(terrain_t *terrain, float x, float z, float cover_radius, uint8_t material);

// The cover and petals of every object standing within radius of
// (x, z) (radius < 0: all of them). Moving objects (OBJECT_STATE_FREE)
// leave nothing.
void material_map_stamp_objects(terrain_t *terrain, const objects_t *objects, float x, float z, float radius);

// Every cover and overlay cleared, and stamped again from the
// objects (a newly populated map)
void material_map_restamp(terrain_t *terrain, const objects_t *objects);

// Where the water ran fast, or left silt, the cover is gone (it was
// carried off, or buried). A few chunks at a time, round the map: call
// every frame. Returns true if it took any.
bool material_map_clear_flooded_cover(terrain_t *terrain, const water_t *water, float silt_visible_units);

// The cover of a cell, MATERIAL_NONE for none
static inline uint8_t material_map_cover(const terrain_t *terrain, int32_t index)
{
	return terrain->materials[(size_t)index * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_COVER];
}

#endif
