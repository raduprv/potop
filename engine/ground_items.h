// Things lying on the ground: dropped from an inventory (or put there by
// the game), each an object of the map (its item's model: an objects.ini
// type, a sack or a crate until there are models) with its own container
// holding what lies there. The object is a loose body: it falls, rolls,
// floats or sinks like the rest. Picking it up empties its container into
// another and takes the object away.
//
// Each is known by its object's group (objects.h: a group of its own), so
// it is found again whatever the objects' order, and saved with the map
// (map_file.h: the items).

#ifndef GROUND_ITEMS_H
#define GROUND_ITEMS_H

#include <stdbool.h>
#include <stdint.h>

#include "inventory.h"
#include "objects.h"
#include "terrain.h"

typedef struct
{
	int group;                             // its object's
	uint32_t container;                    // what lies there
	uint32_t object_id;                    // its object's id (0: not known yet; found by its group once)
} ground_item_t;

typedef struct
{
	ground_item_t *list;
	int count;
	int capacity;
} ground_items_t;

void ground_items_destroy(ground_items_t *ground, inventories_t *inventories);
// Every one forgotten, their containers gone (a new map: the objects went
// with it)
void ground_items_clear(ground_items_t *ground, inventories_t *inventories);

// Up to count of the item out of from, laid at (x, z) (a little above the
// ground: it drops); how many went (0: no model for it, or none there)
int ground_items_drop(ground_items_t *ground, objects_t *objects, const terrain_t *terrain, inventories_t *inventories, uint32_t from,
	int item, int count, float x, float z, float yaw_radians);
// The same with no container to take from (the game puts things down);
// group: its object's (0: a new one)
int ground_items_place(ground_items_t *ground, objects_t *objects, const terrain_t *terrain, inventories_t *inventories, int item, int count,
	float x, float z, float yaw_radians, int group);

// A map being loaded: its objects are there already; what lies as the
// object of that group (added to what is known of it). False if no room
bool ground_items_adopt(ground_items_t *ground, inventories_t *inventories, int group, int item, int count);

// The one whose object is that instance, -1 if none
int ground_items_of_instance(const ground_items_t *ground, const objects_t *objects, int instance);
// The instance of one (-1: its object is gone)
int ground_items_instance(ground_items_t *ground, objects_t *objects, int index);

// All of it into to (what fits); its object goes once nothing is left.
// Returns how many things went
int ground_items_pick_up(ground_items_t *ground, objects_t *objects, inventories_t *inventories, int index, uint32_t to);

// Those whose object is gone (washed off the map, cleared by a tool) go too
void ground_items_forget_missing(ground_items_t *ground, objects_t *objects, inventories_t *inventories);

#endif
