// Weapons: what the player holds and fires, from data/weapons.ini (the
// weapons) and data/ammo.ini (what they fire). The engine keeps the
// weapon's state (put away, coming up, ready, reloading), its timing, its
// magazine, its spread and its recoil; each shot comes out as a
// weapon_shot_t that the caller hands to the projectiles (projectiles.h),
// with the muzzle flash and the recoil to apply to the camera.
//
// The game changes a weapon through its modifiers (weapons_set_modifiers:
// the player's skills, better parts): more damage, less spread, a faster
// reload. Damage itself is only reported (projectiles.h's hits): what it
// does to whom is the game's.
//
// Melee weapons will come here too (a swing, a reach, a short arc instead
// of projectiles); not yet.

#ifndef WEAPONS_H
#define WEAPONS_H

#include <stdbool.h>
#include <stdint.h>

#include "damage.h"

#define WEAPONS_MAX 16
#define WEAPONS_AMMO_MAX 16
#define WEAPONS_NAME_LENGTH 32

typedef enum
{
	WEAPON_FIRE_SEMI = 0,          // a shot a click
	WEAPON_FIRE_BURST,             // burst_count shots a click
	WEAPON_FIRE_AUTO               // while held
} weapon_fire_mode_t;

typedef struct
{
	char name[WEAPONS_NAME_LENGTH];            // the section's
	char shown_name[48];
	char ammo_name[WEAPONS_NAME_LENGTH];
	int ammo;                                  // index into weapons_t.ammo (-1: none)
	weapon_fire_mode_t fire_mode;
	int burst_count;
	float rounds_per_minute;
	int magazine;
	float reload_seconds;
	float ready_seconds;                       // to bring it up (or put it away)
	float spread_degrees;                      // the cone shots leave in, standing still, first shot
	float spread_per_shot_degrees;             // more for each shot in a row
	float spread_most_degrees;
	float spread_recovery_degrees;             // a second, back towards spread_degrees
	float recoil_degrees;                      // the view kicked up a shot
	float recoil_sideways_degrees;             // and to a side, at random, at most
	char muzzle_effect[WEAPONS_NAME_LENGTH];   // particles.ini (its light lights the world)
	char tracer_effect[WEAPONS_NAME_LENGTH];
	int tracer_every;                          // every this many shots one is a tracer (0: none)
	char model[64];                            // "procedural:rifle" or "procedural:scoped_rifle" (until there are models)
	// Aiming (the right button): down the sights, the view narrowed
	// aim_zoom times; with a scope (scope_zoom > 0) through it, that many
	// times; spread and recoil that much less
	float aim_seconds;
	float aim_zoom;
	float scope_zoom;
	float aim_spread;
	float aim_recoil;
	float noise_meters;                        // a shot is heard this far (noises.h; 300)
	bool infinite_ammo;                        // never runs out, never reloads (a spell's)
} weapon_type_t;

typedef struct
{
	char name[WEAPONS_NAME_LENGTH];
	float muzzle_velocity;                     // meters a second
	float mass_grams;
	float drag;                                // 1 / meters: deceleration = drag * speed squared
	float damage;                              // at the muzzle; it goes with the energy left
	int pellets;                               // shot out at once (a shotgun's)
	float pellet_spread_degrees;
	float push;                                // what it gives a body it hits, of its momentum
	float life_seconds;                        // gone after (if it hit nothing)
	damage_type_t damage_type;                 // what its hits say (ballistic; damage.h)
} weapon_ammo_t;

// What the game can change of a weapon (its skills, better parts):
// multipliers (1: as weapons.ini has it) and a magazine bonus
typedef struct
{
	float damage;
	float spread;
	float recoil;
	float reload_time;
	float fire_rate;
	float muzzle_velocity;
	int magazine_bonus;
} weapon_modifiers_t;

typedef enum
{
	WEAPON_STATE_AWAY = 0,
	WEAPON_STATE_RAISING,
	WEAPON_STATE_READY,
	WEAPON_STATE_RELOADING,
	WEAPON_STATE_LOWERING
} weapon_state_t;

// A shot: where from, which way, how fast; what it is (ammo index), its
// share of the ammo's damage (the modifiers), whether it is a tracer
typedef struct
{
	float origin[3];
	float direction[3];
	float speed;
	int ammo;
	float damage_scale;
	bool tracer;
} weapon_shot_t;

typedef struct
{
	weapon_type_t types[WEAPONS_MAX];
	int type_count;
	weapon_ammo_t ammo[WEAPONS_AMMO_MAX];
	int ammo_count;

	// The player's
	int current;                               // the weapon in hand (or to be)
	int next;                                  // to be brought up once the one in hand is down (-1: none)
	float aim;                                 // 0 .. 1: how far it is brought up to the eye
	weapon_state_t state;
	float state_seconds;                       // into the state
	int rounds[WEAPONS_MAX];                   // in each weapon's magazine
	// The rounds there are to load, of each ammo (the game's: what is in
	// the pack; -1: endless), and what reloads took of them since the game
	// last asked (weapons_take_loaded)
	int ammo_reserve[WEAPONS_AMMO_MAX];
	int ammo_loaded[WEAPONS_AMMO_MAX];
	weapon_modifiers_t modifiers[WEAPONS_MAX];
	float cooldown_seconds;                    // until it can fire again
	int burst_left;
	bool trigger_was_held;
	float spread_now_degrees;
	int shots_fired;
	uint32_t random_state;
	// For the drawing (viewmodel.h): the last shot's kick (1 at the shot,
	// dying away), the flash's (shorter)
	float kick;
	float flash;
} weapons_t;

// Reads weapons.ini and ammo.ini (the weapons with a magazine full). False
// if there is no weapon at all
bool weapons_load(weapons_t *weapons);

// The weapon in hand up, or away (it takes its ready_seconds)
void weapons_toggle(weapons_t *weapons);
// That weapon up: the one in hand is put away first; the one in hand
// itself: away
void weapons_select(weapons_t *weapons, int weapon);
// In hand (up, or coming up, or reloading): the tools are put away then
bool weapons_in_hand(const weapons_t *weapons);
// A new magazine, if it is not full (and ready) and there are rounds to
// load
void weapons_reload(weapons_t *weapons);
// The rounds of that ammo there are to load (-1: endless); what reloads
// took of them since the last call (the game takes them out of the pack)
void weapons_set_reserve(weapons_t *weapons, int ammo, int rounds);
int weapons_take_loaded(weapons_t *weapons, int ammo);

// The time going on, the trigger held or not, aiming or not: the shots
// fired (at most max_shots: written to shots_out), and the view's kick
// (degrees up, and to the right) to add to the camera. eye and the three
// axes: the camera's
int weapons_update(weapons_t *weapons, float seconds, bool trigger_held, bool aim_held, const float eye[3], const float forward[3],
	const float right[3],
	const float up[3], weapon_shot_t *shots_out, int max_shots, float *kick_up_degrees_out, float *kick_right_degrees_out);

void weapons_set_modifiers(weapons_t *weapons, int weapon, const weapon_modifiers_t *modifiers);
weapon_modifiers_t weapons_default_modifiers(void);

// For the drawing: how far up the weapon is (0 away .. 1 ready), how far
// through a reload (0..1, 0 when not reloading)
float weapons_raised(const weapons_t *weapons);
float weapons_reload_progress(const weapons_t *weapons);
// How far it is aimed (0..1, eased); how many times the view is narrowed
// now (1: not at all); whether the eye is at its scope (draw the scope,
// not the weapon)
float weapons_aim(const weapons_t *weapons);
float weapons_zoom(const weapons_t *weapons);
bool weapons_through_scope(const weapons_t *weapons);
bool weapons_scoped(const weapons_t *weapons);
// "AK-47 23/30 +60 auto" (nothing when it is away; +: the rounds to load)
void weapons_describe(const weapons_t *weapons, char *text, int text_size);

#endif
