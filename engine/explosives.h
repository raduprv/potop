// What blows up (data/explosives.ini): grenades thrown (on a timer, or a
// proximity fuse that waits until something comes near) and explosions.
//
// An explosion pushes the loose things round it (the physics), digs a
// crater in the ground if it is near it, flashes (a light that fades) and
// plays its effects (particles.ini); who was caught in it, how far and
// whether behind something, goes to the hits (projectiles_take_hits:
// what it does is the game's).
//
// A grenade is a loose object of the map (its model: an objects.ini type
// with a body, made small), thrown from the hand: the physics bounces and
// rolls it, it goes off where it lies. A mine is the same put down where
// it is to stay (buried, if its type says), on a proximity fuse: armed
// after a while, it goes off when someone comes near (or, a pressure one,
// steps on it), its owner spared if its type says; it can be taken back
// (disarmed) before.
//
// A blast's hits carry its damage type (damage.h); a fire one (burn_*)
// leaves a fire burning a while where it went off, whose hits come every
// half second.

#ifndef EXPLOSIVES_H
#define EXPLOSIVES_H

#include <stdbool.h>
#include <stdint.h>

#include "damage.h"
#include "light_source.h"
#include "world.h"

#define EXPLOSIVES_TYPES_MAX 16
#define EXPLOSIVES_NAME_LENGTH 32
#define EXPLOSIVES_EFFECTS_MAX 4
#define EXPLOSIVES_GRENADES_MAX 64
#define EXPLOSIVES_BURNS_MAX 16

struct session_s;

typedef enum
{
	EXPLOSIVE_FUSE_TIMER = 0,      // goes off fuse_seconds after it is thrown
	EXPLOSIVE_FUSE_PROXIMITY       // armed after arm_seconds; goes off when an actor (or the player) comes within trigger_meters
} explosive_fuse_t;

typedef struct
{
	char name[EXPLOSIVES_NAME_LENGTH];         // the section's
	explosive_fuse_t fuse;
	float fuse_seconds;
	float arm_seconds;
	float trigger_meters;
	float radius_meters;                       // what it reaches (damage and push fade out to here)
	float damage;                              // at its heart (the game decides what it does)
	float push_pascal_seconds;                 // the blast's impulse on a square meter facing it, a meter away (it falls as the distance grows)
	float crater_meters;                       // the hole it leaves in the ground near it (0: none)
	float crater_depth_meters;
	char effects[EXPLOSIVES_EFFECTS_MAX][EXPLOSIVES_NAME_LENGTH]; // particles.ini
	int effect_count;
	light_source_t light;                      // its flash
	float light_seconds;
	float throw_meters_per_second;
	char model[64];                            // objects.ini: a type with a body
	float size_meters;                         // the model made this big
	float mass_kilograms;                      // and this heavy (not the model's, made small)
	damage_type_t damage_type;                 // what its hits say (explosive)
	float noise_meters;                        // heard this far (noises.h; 600)
	// A fire left burning where it went off (burn_seconds 0: none):
	// burn_damage_per_second to whoever is within burn_radius_meters, of
	// its damage type; burn_effect plays there meanwhile (particles.ini)
	float burn_seconds;
	float burn_radius_meters;
	float burn_damage_per_second;
	char burn_effect[EXPLOSIVES_NAME_LENGTH];
	// Put down (a mine): sunk in the ground (buried), stepped on only
	// (pressure: a walker on the ground no shorter than trigger_height_meters,
	// not a bird), its owner spared (spares_owner)
	bool buried;
	bool pressure;
	float trigger_height_meters;
	bool spares_owner;
} explosive_type_t;

typedef struct
{
	bool used;
	int type;                      // explosives_t.types
	uint32_t object_id;            // its object on the map
	float age_seconds;
	bool thrown;                   // the throw's push given (its body is made a step later)
	bool weighed;                  // its body made its own weight
	float velocity[3];             // the throw
	int owner;                     // who threw it (-1: the player)
	bool placed;                   // put down (a mine), not thrown
} grenade_t;

// A fire a blast left
typedef struct
{
	bool used;
	float position[3];
	float radius_meters;
	float seconds_left;
	float damage_per_second;
	damage_type_t damage_type;
	int owner;
	uint32_t emitter;              // its effect (0: none)
	float tick_seconds;            // till its next hits
} burn_t;

typedef struct
{
	explosive_type_t types[EXPLOSIVES_TYPES_MAX];
	int type_count;
	grenade_t grenades[EXPLOSIVES_GRENADES_MAX];
	burn_t burns[EXPLOSIVES_BURNS_MAX];
	// Where the player was at the last update (the proximity fuses, the
	// blasts' hits)
	float player[3];
	bool player_known;
} explosives_t;

// data/explosives.ini
void explosives_load(explosives_t *explosives);
// The type named so (-1: none)
int explosives_find(const explosives_t *explosives, const char *name);

// An explosion of a type at a point (owner: who set it off, -1 the player)
void explosives_explode(explosives_t *explosives, struct session_s *session, world_t *world, int type, const float position[3], int owner);

// A grenade of a type thrown from a point along a direction (unit length;
// the type's speed); false if it could not be put on the map
bool explosives_throw(explosives_t *explosives, world_t *world, int type, const float from[3], const float direction[3], int owner);

// A mine of a type put down at a point (on the ground; sunk if its type
// is buried), turned yaw; its object's id (0: not put down)
uint32_t explosives_place(explosives_t *explosives, world_t *world, int type, float x, float z, float yaw_radians, int owner);

// The grenade or mine that is this object: its type, whether it is armed
// (a mine; a grenade on a timer: never), who it belongs to; false if the
// object is none
bool explosives_find_object(const explosives_t *explosives, uint32_t object_id, int *type_out, bool *armed_out, int *owner_out);
// The mine (or grenade) that is this object taken off the map unexploded:
// its type (-1 if the object is none)
int explosives_take_back(explosives_t *explosives, world_t *world, uint32_t object_id);

// Once a frame: the throws' pushes, the fuses; what goes off explodes
void explosives_update(explosives_t *explosives, struct session_s *session, world_t *world, const float player[3], float seconds);

// The grenade that is this object (a shot hit it, say: the game's to
// decide) goes off now; false if the object is no grenade
bool explosives_set_off(explosives_t *explosives, struct session_s *session, world_t *world, uint32_t object_id);

// The grenades taken off the map (a map loaded: they were the old one's)
void explosives_forget(explosives_t *explosives);

#endif
