// The player's side of the game, as the engine keeps it: what they carry
// and wield, the weapon's projectiles, the quest log the game fills, the
// markers, the conversation, the world map's picture, what the eye is on.
// It is not the map's: the pack goes with the player from one map to the
// next (a world_t is the map).

#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "dialog.h"
#include "inventory.h"
#include "items.h"
#include "journal.h"
#include "markers.h"
#include "player.h"
#include "projectiles.h"
#include "explosives.h"
#include "viewmodel.h"
#include "weapons.h"
#include "world_map.h"
#include "world.h"

typedef struct session_s
{
	// The weapon in hand (first person), what it fires, how it is drawn
	weapons_t weapons;
	projectiles_t projectiles;
	explosives_t explosives;       // grenades thrown, explosions (explosives.h)
	viewmodel_t viewmodel;
	// The player on foot: the walker, the weapon's buttons, what the eye
	// is on (player.h)
	player_t player;
	// What can be carried, who carries what (the player's own container)
	items_t items;
	inventories_t inventories;
	uint32_t player_inventory;
	// The compass's and the map's markers (the journal's tracked
	// objectives, the player's own waypoints); the map's picture, made
	// again when it is stale
	markers_t markers;
	world_map_t world_map;
	bool world_map_stale;
	// The quest log (the game fills it)
	journal_t journal;
	bool map_was_open;             // (the map's picture made when it opens)
	// Talking (the engine shows it, the game holds it)
	dialog_t dialog;
	// On foot: what E does with what the eye is on (the game says; the
	// HUD shows it; empty: nothing)
	char interaction_prompt[96];
} session_t;

// Loads the weapons, the items (data/weapons.ini, items.ini) and makes
// the rest: the inventories (the player's pack is up to the game), the
// markers, the journal, the dialog, the projectiles, the viewmodel, the
// player
void session_create(session_t *session, world_t *world);
// Lets go of it all (before world_destroy)
void session_destroy(session_t *session, world_t *world);

// The items in a map's file: the player's (in the pack and worn), and what
// lies on the ground (world: its ground items, their containers in the
// session's inventories)
void session_gather_items(session_t *session, world_t *world, map_features_t *features);
// A loaded map's: what lies on the ground, and the player's pack if the map
// has it
void session_apply_items(session_t *session, world_t *world, const map_features_t *features);

// Once a frame: now and then, what lies on the ground is looked over for
// objects gone, and what was picked grows again by the game hours gone by
// (world_t.harvest_hours_pending)
void session_update(session_t *session, world_t *world, float seconds);

// The world map's picture made again if the land changed (world_map_stale),
// the last is half a minute old (the water, the tools), or it was made for
// another map (shown_name: the map's name as the picture shows it)
void session_update_world_map(session_t *session, world_t *world, const char *shown_name);

#endif
