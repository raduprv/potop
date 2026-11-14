// Layouts: groups of objects placed together, read from data/layouts.ini.
//
// A layout is a recipe around a center point: a homestead is a house, a
// fence around its yard, a garden, an outhouse; a camp is a fire with tents
// around it. Its parts are placed in the layout's own frame: +z is its
// front (where the house door looks), +x its right. Part kinds:
//   single    one object (or a whole other layout) at an offset
//   ring      several on a circle, facing its center, away from it, along it
//             or at random
//   scatter   several at random in a ring shaped area (a disk if the inner
//             radius is 0)
//   rows      a rectangle of plants in rows (a garden)
//   fence     fence pieces along the sides of a rectangle, with a gate
//   stack     a pile: several of one model one on top of the other (crates,
//             logs), optionally lying on two supports across its ends
// Wherever a part names its models, it picks one of them by weight; a
// single, ring or scatter part may name a layout instead, so a hamlet is a
// well with homesteads around it.
//
// Placing a layout levels the ground under it (if it asks for that), clears
// the vegetation under what it places (the yard keeps its grass), and puts
// all its objects into one group, so the whole thing can be removed at once.
// New maps get layouts where the ground suits them; the layout tool puts
// them wherever the player clicks.

#ifndef LAYOUTS_H
#define LAYOUTS_H

#include <stdbool.h>

#include "noise.h"
#include "objects.h"
#include "settings.h"
#include "terrain.h"
#include "water.h"

#define LAYOUT_NAME_LENGTH 48
#define LAYOUT_MAX_PARTS 32
#define LAYOUT_MAX_CHOICES 16
#define LAYOUTS_MAX 64

typedef enum
{
	LAYOUT_PART_SINGLE = 0,
	LAYOUT_PART_RING,
	LAYOUT_PART_SCATTER,
	LAYOUT_PART_ROWS,
	LAYOUT_PART_FENCE,
	LAYOUT_PART_STACK              // count copies of one model piled up, on two supports if given
} layout_part_kind_t;

typedef enum
{
	LAYOUT_FACING_FRONT = 0,       // the layout's front, plus the part's yaw
	LAYOUT_FACING_CENTER,          // towards the part's center
	LAYOUT_FACING_OUTWARD,
	LAYOUT_FACING_ALONG,           // along the circle
	LAYOUT_FACING_RANDOM
} layout_facing_t;

typedef enum
{
	LAYOUT_PLACE_GROUND = 0,       // at the offset
	LAYOUT_PLACE_SHORE,            // where the water starts in front of the layout, plus the offset
	LAYOUT_PLACE_WATER             // the same, for things that float (boats)
} layout_place_t;

typedef enum
{
	LAYOUT_ORIENT_RANDOM = 0,
	LAYOUT_ORIENT_DOWNHILL,        // front down the slope (towards the valley)
	LAYOUT_ORIENT_WATER            // front towards the nearest water
} layout_orient_t;

typedef struct
{
	layout_part_kind_t kind;
	int choice_count;
	int choice_types[LAYOUT_MAX_CHOICES];
	float choice_weights[LAYOUT_MAX_CHOICES];
	char layout_name[LAYOUT_NAME_LENGTH]; // a nested layout instead of models
	int layout;                    // its index, -1 if none
	float offset_x;
	float offset_z;
	float yaw_degrees;
	layout_facing_t facing;
	float chance;                  // probability the part is there at all
	float item_chance;             // probability of each single item (ring, fence: gaps)
	int count_minimum;
	int count_maximum;
	float radius_minimum;
	float radius_maximum;
	float size_x;
	float size_z;
	float row_spacing;
	float plant_spacing;
	float jitter_meters;
	bool same_model_per_row;
	int gate_side;                 // -1 none, 0 front, 1 back, 2 left, 3 right
	float gate_width;
	float lift_minimum;
	float lift_maximum;
	float scale_minimum;           // 0: the type's own range
	float scale_maximum;
	layout_place_t place;
	int support_type;              // stack: the type lying across both ends under the pile, -1 none
} layout_part_t;

typedef struct
{
	char name[LAYOUT_NAME_LENGTH];
	float footprint_meters;        // radius: levelled ground, distance to other layouts
	bool flatten;
	layout_orient_t orient;
	// Placement on new maps
	float spawn_per_square_kilometer;
	uint8_t spawn_soils;
	float flatness_meters;         // largest height difference over the footprint
	float spawn_height_minimum;    // fractions of the map's highest point
	float spawn_height_maximum;
	float spacing_meters;          // free room between it and other layouts
	float water_search_meters;     // shore parts look this far for water; needed on new maps if > 0
	bool needs_water;
	layout_part_t parts[LAYOUT_MAX_PARTS];
	int part_count;
} layout_t;

typedef struct
{
	layout_t layouts[LAYOUTS_MAX];
	int count;
} layouts_t;

// Reads data/layouts.ini; the object type names in it are looked up in
// types. Returns false if the file is missing.
bool layouts_load(layouts_t *layouts, const object_types_t *types);

int layouts_find(const layouts_t *layouts, const char *name);

// Places a layout at (x, z), facing as the layout prefers. Levels the
// ground, clears the vegetation, adds the objects in a new group; returns
// the group (0 if nothing was placed). water may be NULL.
int layouts_place(const layouts_t *layouts, objects_t *objects, terrain_t *terrain, water_t *water, int layout, float x, float z,
	random_t *random);

// Places the layouts of a new map where the ground suits them
void layouts_populate_map(const layouts_t *layouts, objects_t *objects, terrain_t *terrain, water_t *water,
	const objects_settings_t *settings);

#endif
