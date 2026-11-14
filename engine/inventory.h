// Inventories: what is carried, and where. A container is a list of
// entries (an item of items.h and how many, at most its stack in one
// entry), with what is worn or held in the body's slots, and a weight it
// may not go over (the game sets it: inventory_set_max_weight; 0: none).
// The player's pack, a dead animal's loot, a chest, a basket: all are
// containers. Each has an id that stays while it exists (never 0).
//
// What goes in or comes out is told as events (inventory_poll_event): a
// quest that wants a basket carried to the grandmother watches those.

#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "items.h"

#define INVENTORY_ENTRIES_MAX 96
#define INVENTORY_NAME_LENGTH 48
#define INVENTORY_EVENT_QUEUE 64

typedef struct
{
	int item;                              // items_t's
	int count;
} inventory_entry_t;

typedef struct
{
	uint32_t id;                           // 0: unused
	char name[INVENTORY_NAME_LENGTH];      // shown over its list ("Bear", "Chest")
	inventory_entry_t entries[INVENTORY_ENTRIES_MAX];
	int entry_count;
	int equipped[ITEM_SLOTS_MAX];          // the item in each slot (-1: none)
	float max_weight_kilograms;            // 0: no limit
	bool locked;                           // (the game decides what that keeps out)
	int key_item;                          // the item that opens it (items_t's; -1: none)
} container_t;

typedef enum
{
	INVENTORY_EVENT_ADDED = 1,
	INVENTORY_EVENT_REMOVED,
	INVENTORY_EVENT_EQUIPPED,
	INVENTORY_EVENT_UNEQUIPPED,
	INVENTORY_EVENT_USED                   // the player used it (what that does is the game's; it is still there)
} inventory_event_type_t;

typedef struct
{
	inventory_event_type_t type;
	uint32_t container;
	int item;
	int count;
} inventory_event_t;

typedef struct
{
	const items_t *items;
	container_t *containers;
	int count;
	int capacity;
	uint32_t next_id;
	inventory_event_t events[INVENTORY_EVENT_QUEUE];
	int event_first;
	int event_count;
} inventories_t;

void inventories_create(inventories_t *inventories, const items_t *items);
void inventories_destroy(inventories_t *inventories);
// Every container gone (a new map)
void inventories_clear(inventories_t *inventories);

// A new, empty container; 0 if there is no room
uint32_t inventory_create(inventories_t *inventories, const char *name, float max_weight_kilograms);
void inventory_destroy(inventories_t *inventories, uint32_t id);
container_t *inventory_get(inventories_t *inventories, uint32_t id);
const container_t *inventory_get_const(const inventories_t *inventories, uint32_t id);

void inventory_set_max_weight(inventories_t *inventories, uint32_t id, float kilograms);
// Locked or not, and the item that opens it (-1: none)
void inventory_set_lock(inventories_t *inventories, uint32_t id, bool locked, int key_item);
// What it carries, worn and held too
float inventory_weight(const inventories_t *inventories, uint32_t id);

// Puts in up to count of the item (as many as its weight limit and its
// rows allow); returns how many went in
int inventory_add(inventories_t *inventories, uint32_t id, int item, int count);
// Takes out up to count (from the rows, not what is worn); returns how many
int inventory_remove(inventories_t *inventories, uint32_t id, int item, int count);
// How many of the item it has (in its rows, and worn: worn too)
int inventory_count(const inventories_t *inventories, uint32_t id, int item, bool worn_too);
// From one to the other, up to count; returns how many went
int inventory_move(inventories_t *inventories, uint32_t from, uint32_t to, int item, int count);
// Everything of its rows that fits; returns how many things went
int inventory_move_all(inventories_t *inventories, uint32_t from, uint32_t to);

// The item of that row into its slot (what was there back into the rows);
// false if it has no slot or the row is not there
bool inventory_equip(inventories_t *inventories, uint32_t id, int entry);
// What is in the slot back into the rows
bool inventory_unequip(inventories_t *inventories, uint32_t id, int slot);
// The item of that row used (an event: the game decides what it does, and
// whether it is used up); false if it is not usable
bool inventory_use(inventories_t *inventories, uint32_t id, int entry);

bool inventory_poll_event(inventories_t *inventories, inventory_event_t *event_out);

#endif
