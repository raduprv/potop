// Picking things off the map by hand: an object whose type has a harvest
// (objects.ini: harvest = tomato; the game can change it through
// harvest_set) gives that item into a container. A weed goes
// (harvest_removes); else the object counts how many times it was picked
// and gives nothing more after harvest_times, until harvest_regrow_hours of
// game time have gone by (0: never again). While picked out it may turn
// into another type (harvest_change_object: a bush without its berries),
// and back once grown again.
//
// The objects keep the count (objects.h: harvested, regrow_hours,
// harvest_original_type); a map's file keeps it too (map_file.h).

#ifndef HARVEST_H
#define HARVEST_H

#include <stdbool.h>
#include <stdint.h>

#include "inventory.h"
#include "objects.h"
#include "terrain.h"

typedef enum
{
	HARVEST_NOTHING = 0,                   // not something to pick
	HARVEST_PICKED,                        // picked (some at least: the pack may be full)
	HARVEST_PICKED_OUT,                    // nothing left on it (yet)
	HARVEST_TOO_HEAVY                      // nothing fits in the pack
} harvest_result_t;

typedef struct
{
	harvest_result_t result;
	int item;                              // items_t's (-1: none)
	int count;                             // went into the container
	bool removed;                          // the object went
} harvest_outcome_t;

// What the instance would give (its item, -1 if none) and whether there is
// any left on it now
int harvest_item_of(const objects_t *objects, const items_t *items, int instance, bool *any_left_out);

// Picks it: the item into the container; the object goes, counts, or turns
harvest_outcome_t harvest_pick(objects_t *objects, const terrain_t *terrain, inventories_t *inventories, int instance, uint32_t container,
	uint32_t *random_state);

// Game hours went by: what was picked grows again
void harvest_update(objects_t *objects, const terrain_t *terrain, float hours);

// The game's say over a type's harvest (item NULL or empty: none)
void harvest_set(objects_t *objects, int type, const char *item, int least, int most, bool removes, int times, float regrow_hours);

#endif
