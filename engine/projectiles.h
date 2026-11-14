// Projectiles: bullets (and later arrows, grenades) flying with their own
// physics: gravity, the air slowing them (drag * speed squared, against
// the wind), the water much more (a bullet goes a meter or two in it). Each
// step the way from where it was to where it is now is checked against the
// ground (the heights) and the physics' shapes (houses, trees, rocks, loose
// things), so a fast one does not go through what is thin.
//
// Where one hits: the effect of what it hit (data/impacts.ini: by the
// ground's material, the object's category, the water), a push to a loose
// body (its momentum times the ammo's push), and a hit kept for the game
// (projectiles_take_hits: what, where, how fast, its damage as the energy
// left goes; an actor: which, and where on its body): what the damage does
// is the game's.

#ifndef PROJECTILES_H
#define PROJECTILES_H

#include <stdbool.h>
#include <stdint.h>

#include "actors.h"
#include "damage.h"
#include "decals.h"
#include "objects.h"
#include "particles.h"
#include "physics.h"
#include "terrain.h"
#include "water.h"

#define PROJECTILES_MAX 512
#define PROJECTILE_HITS_MAX 64
#define IMPACT_KINDS_MAX 16

typedef enum
{
	PROJECTILE_HIT_GROUND = 0,
	PROJECTILE_HIT_OBJECT,
	PROJECTILE_HIT_WATER,          // went into the water (it goes on, slowly)
	PROJECTILE_HIT_ACTOR,          // an actor's body (actor_id, zone, bone)
	PROJECTILE_HIT_PLAYER          // the player (an explosion's: explosives.h)
} projectile_hit_kind_t;

typedef struct
{
	projectile_hit_kind_t kind;
	float point[3];
	float normal[3];
	float velocity[3];             // when it hit
	int instance;                  // the object (PROJECTILE_HIT_OBJECT; else -1): its place now
	uint32_t object_id;            // and its id (the place moves as the map changes)
	uint32_t actor_id;             // the actor (PROJECTILE_HIT_ACTOR; else 0)
	actor_zone_t zone;             // where on it (the game: a head shot)
	int bone;
	float damage;                  // its damage at the muzzle, as the energy left goes
	damage_type_t damage_type;     // what kind (damage.h)
	int ammo;                      // weapons.ini's ammo it was (-1: none: a blast's)
	int owner;                     // who fired it (the caller's number: -1 the player)
} projectile_hit_t;

typedef struct
{
	bool used;
	float position[3];
	float velocity[3];
	float mass_kilograms;
	float drag;                    // 1 / meters
	float push;
	float damage;                  // at the muzzle
	float muzzle_speed;
	float age_seconds;
	float life_seconds;
	int owner;
	damage_type_t damage_type;
	int ammo;                      // weapons_t's (-1: none)
	bool in_water;
	uint32_t tracer;               // its tracer's emitter (0: none)
} projectile_t;

typedef struct
{
	char name[32];
	int effect;                    // particles.ini's (-1: none)
	float spray;                   // meters a second its particles go off the surface (along the normal)
	char materials[128];           // the ground's materials it is for (materials.h's names, commas)
	char categories[128];          // the objects' categories (objects.ini's, commas)
	bool water;
	bool actors;                   // the actors' bodies
	bool fallback_ground;          // what the ground's materials not listed take
	bool fallback_object;
	char decal[32];                // the mark it leaves (data/decals.ini; empty: none)
	float decal_size;              // meters across (0: the decal kind's)
} impact_kind_t;

typedef struct
{
	projectile_t list[PROJECTILES_MAX];
	int next;
	projectile_hit_t hits[PROJECTILE_HITS_MAX];
	int hit_count;
	impact_kind_t impacts[IMPACT_KINDS_MAX];
	int impact_count;
} projectiles_t;

// What the projectiles fly through
typedef struct
{
	const terrain_t *terrain;
	const water_t *water;
	physics_t *physics;            // NULL: the ground only
	const objects_t *objects;
	const actors_t *actors;        // NULL: they are not hit
	particles_t *particles;
	decals_t *decals;              // NULL: the hits leave no marks
	double now;                    // the game's clock (the decals' age)
	float wind[2];                 // meters a second (x, z)
} projectiles_world_t;

// Reads data/impacts.ini (its effects looked up in the particles)
void projectiles_create(projectiles_t *projectiles, const particles_t *particles);

// A projectile from origin along direction (normalized) at speed; tracer:
// that effect (particles.ini; NULL: none) flies along with it, from
// tracer_origin (the muzzle as drawn: seen from the eye, a streak going
// straight away would be a dot; NULL: 2 m ahead of origin)
// It, to set more of it (its damage type: ballistic until told)
projectile_t *projectiles_fire(projectiles_t *projectiles, particles_t *particles, const float origin[3], const float direction[3], float speed,
	float mass_kilograms, float drag, float push, float damage, float life_seconds, int owner, const char *tracer_effect,
	const float tracer_origin[3]);

void projectiles_update(projectiles_t *projectiles, float seconds, const projectiles_world_t *world);

// A hit from elsewhere (an explosion's), with the projectiles' own
void projectiles_add_hit(projectiles_t *projectiles, const projectile_hit_t *hit);

// The hits since the last call (at most max: oldest dropped if more came)
int projectiles_take_hits(projectiles_t *projectiles, projectile_hit_t *hits_out, int max);

void projectiles_clear(projectiles_t *projectiles);

#endif
