// Growing vegetation: populating a whole map, and the planting brush.
//
// Populating goes through the types in the order of objects.ini (big
// things first) and lays a jittered grid over the map for each, with the
// type's spacing. Every grid cell gets one candidate at a random spot,
// kept with a probability set by the type's number per hectare and by
// where it is: in or out of the type's patches (forests, flower meadows),
// in a valley or on a ridge, on the right soil, slope and height. Solid
// things (trunks, rocks) keep a footprint free around them, so trees do
// not grow through rocks and grass does not grow out of a trunk. The
// result depends only on the terrain and its seed: the same map always
// gets the same forest.
//
// The brush plants one category (trees, grass, ...) around the cursor
// until the circle holds as many as the types' brush densities ask for;
// it follows the footprints, the slope and the water rules, but not the
// soils, heights and patches: where to plant is the player's choice.

#ifndef POPULATE_H
#define POPULATE_H

#include "noise.h"
#include "objects.h"
#include "settings.h"
#include "terrain.h"
#include "water.h"

// Adds the vegetation of the whole map. Objects already there (houses)
// keep their footprints free. water may be NULL.
void populate_map(objects_t *objects, const terrain_t *terrain, const water_t *water, const objects_settings_t *settings);
// The same for the types of the categories whose bits are set in
// category_mask (what is already there keeps its footprint); variation 0
// places them as populate_map does (by the map's seed), any other number
// elsewhere (the forests' patches stay where they are)
void populate_map_categories(objects_t *objects, const terrain_t *terrain, const water_t *water, const objects_settings_t *settings,
	uint32_t category_mask, uint32_t variation);

// One application of the planting brush lasting 'seconds'; returns how
// many objects it planted
int populate_brush(objects_t *objects, const terrain_t *terrain, const water_t *water, const objects_settings_t *settings,
	int category, float center_x, float center_z, float radius, float seconds, random_t *random);

#endif
