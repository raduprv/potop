// The player on foot (first person): the walker moved by the keys over the
// ground, the objects and the bodies; the weapon in hand, its rounds out of
// the pack, its shots and their projectiles; what the eye is on, for E.
//
// The player reports, the game decides: player_find_target says what the
// eye is on (something to pick, something lying there, an actor, alive or
// dead), the game says what E does with it and what the prompt reads, and
// asks the engine's part of it (player_pick, player_pick_up). The hits the
// projectiles make wait in projectiles_take_hits for the game.
//
// The walker itself (where it stands, which way it looks) is the camera's
// (camera.h: walker_*): the eye is where the player is.

#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"
#include "harvest.h"
#include "input.h"
#include "light_source.h"
#include "world.h"

struct session_s;

// What the eye is on
typedef enum
{
	PLAYER_TARGET_NONE = 0,
	PLAYER_TARGET_HARVEST,         // an object to pick from (harvest.h): instance, item, any_left
	PLAYER_TARGET_GROUND_ITEM,     // something lying there (ground_items.h): ground_item, its container
	PLAYER_TARGET_ACTOR,           // an actor: actor_id, actor_dead
	PLAYER_TARGET_OBJECT,          // something the game can do something with (OBJECT_STATE_INTERACTIVE): instance
	PLAYER_TARGET_DOOR             // a module's door or gate: object_id (the module's), door (its part)
} player_target_kind_t;

typedef struct
{
	player_target_kind_t kind;
	float distance;                // meters from the eye
	int instance;                  // HARVEST, GROUND_ITEM, OBJECT: the object (objects_t: its index now)
	uint32_t object_id;            // ...and its id
	int item;                      // HARVEST: what it gives (items_t)
	bool any_left;                 // HARVEST: something to pick now
	int ground_item;               // GROUND_ITEM: its place in ground_items_t.list
	uint32_t container;            // GROUND_ITEM: what it holds (inventories_t)
	uint32_t actor_id;             // ACTOR
	bool actor_dead;
	int door;                      // DOOR: the module's part
	bool door_open;                // DOOR: open (or opening)
} player_target_t;

typedef struct
{
	// The walker's body (player_init's; the game may change them)
	float height_meters;           // the room it needs (it ducks a little)
	float step_meters;             // what it steps onto
	float kilograms;               // how hard it pushes loose things
	// How far the eye reaches, for player_find_target
	float reach_meters;            // things to pick, things lying there, the dead
	float talk_reach_meters;       // the living
	// The weapon's buttons, set by the game every frame (false: let go)
	bool trigger;
	bool aim;
	float weapon_seconds;          // the last weapon update's (the viewmodel's bob)
	player_target_t target;        // player_find_target's
	// What the player carries that lights round (a torch, a lantern):
	// at the hand left of the eye, on foot (player_update_light)
	bool light_on;
	light_source_t light;
	float light_position[3];       // where it is now (player_update_light)
} player_t;

// The lights' key of the player's carried light (lights_set_carried);
// actors' are PLAYER_ACTOR_LIGHT_KEYS + their id
enum { PLAYER_LIGHT_KEY = 1 };
#define PLAYER_ACTOR_LIGHT_KEYS 0x40000000u

void player_init(player_t *player);

// The carried light where the hand is (on foot), or put out; once a frame,
// after the camera's matrices
void player_update_light(player_t *player, world_t *world, const camera_t *camera);

// First person: moves the walker by this frame's actions (camera.h's
// camera_update_first_person, with what stands on the map in the way), and
// what it walks into is pushed (barrels, logs, boats, stones)
void player_walk(player_t *player, world_t *world, camera_t *camera, const input_state_t *input, const camera_settings_t *settings,
	float seconds);

// The weapon: brought up, fired by the trigger (first person only), its
// rounds taken from the player's pack, the muzzle's flash, the view's kick,
// camera.zoom by the aim; then the projectiles flying (with the plants'
// wind). Out of first person nothing fires and the zoom is 1.
void player_update_weapons(player_t *player, struct session_s *session, world_t *world, camera_t *camera, float seconds);

// Throws a grenade (an item of the pack that is an explosive), from the
// hand along the eye; false if the pack has none (or it cannot be thrown)
bool player_throw(player_t *player, struct session_s *session, world_t *world, const camera_t *camera, int item);
// Puts one of an item that is an explosive (a mine) down where the eye
// looks on the ground within reach, else at the feet; its object's id (0:
// none in the pack, or no room)
uint32_t player_place(player_t *player, struct session_s *session, world_t *world, const camera_t *camera, int item);

// Takes a weapon of weapons.ini (worn in the hands if it is an item);
// false if it is an item the pack does not have (nothing changes)
bool player_select_weapon(player_t *player, struct session_s *session, int weapon);

// What the eye is on (player->target): the nearest of what can be picked,
// lies there or the game can do something with, unless an actor is nearer; the living within
// talk_reach_meters, the rest within reach_meters
void player_find_target(player_t *player, struct session_s *session, world_t *world, const camera_t *camera);

// Picks from the target (PLAYER_TARGET_HARVEST) into the player's pack
harvest_outcome_t player_pick(player_t *player, struct session_s *session, world_t *world);

// Picks up what lies there (PLAYER_TARGET_GROUND_ITEM) into the pack;
// false if some of it is left (too heavy) or there was nothing
bool player_pick_up(player_t *player, struct session_s *session, world_t *world);

#endif
