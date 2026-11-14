// Items: the kinds of things that can be carried, from data/items.ini, one
// section each: its name, its category (shown as a small sign in the
// lists), weight, value, how many go in one row of a list (stack), the
// body's slot it is worn or held in, and what it is to the weapons (a
// weapon of weapons.ini, the ammo of ammo.ini). What an item does beyond
// that (eating, a quest's) is the game's.
//
// The body's slots (where things are worn or held) are items.ini's [slots]
// section: names = hands, head, body, legs, feet, back.

#ifndef ITEMS_H
#define ITEMS_H

#include <stdbool.h>

#include "weapons.h"

#define ITEM_TYPES_MAX 256
#define ITEM_NAME_LENGTH 32
#define ITEM_SLOTS_MAX 12

typedef enum
{
	ITEM_CATEGORY_MISC = 0,
	ITEM_CATEGORY_WEAPON,
	ITEM_CATEGORY_AMMO,
	ITEM_CATEGORY_FOOD,
	ITEM_CATEGORY_MATERIAL,                // pelts, feathers, wood
	ITEM_CATEGORY_CLOTHING,
	ITEM_CATEGORY_TOOL,
	ITEM_CATEGORY_QUEST,
	ITEM_CATEGORY_MONEY,
	ITEM_CATEGORY_COUNT
} item_category_t;

typedef struct
{
	char name[ITEM_NAME_LENGTH];           // the section's
	char shown_name[48];
	char description[160];
	item_category_t category;
	float weight_kilograms;                // one
	float value;                           // one
	int stack;                             // at most this many in one row (1: each its own)
	bool usable;                           // it can be used (eaten, read...: the game says what it does)
	int slot;                              // the body's slot it goes in (items_t.slot_names), -1: none
	char weapon_name[ITEM_NAME_LENGTH];
	int weapon;                            // weapons.ini's (index in weapons_t.types), -1: not a weapon
	char ammo_name[ITEM_NAME_LENGTH];
	int ammo;                              // ammo.ini's (index in weapons_t.ammo), -1: not ammo
	char explosive_name[ITEM_NAME_LENGTH];
	int explosive;                         // explosives.ini's (a grenade: thrown), -1: not one
	char model[64];                        // lying on the ground: an objects.ini type (ground_items.h)
	float size_meters;                     // its size there (the model made that big)
} item_type_t;

typedef struct
{
	item_type_t types[ITEM_TYPES_MAX];
	int type_count;
	char slot_names[ITEM_SLOTS_MAX][24];
	int slot_count;
} items_t;

// Reads data/items.ini; weapons: for the weapon and ammo names (NULL:
// none are). False if there is none
bool items_load(items_t *items, const weapons_t *weapons);

// The item with that name, -1 if none
int items_find(const items_t *items, const char *name);
// The item that is that weapon, that ammo; -1 if none
int items_of_weapon(const items_t *items, int weapon);
int items_of_ammo(const items_t *items, int ammo);
// The slot with that name, -1 if none
int items_find_slot(const items_t *items, const char *name);

const char *items_category_name(item_category_t category);

#endif
