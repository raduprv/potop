// Areas: rectangles on the map, turned any way, put there in the editor
// and kept with the map (a city's gate, the enchanted forest). Each says
// who it is for (its triggers: the player, the actors, no one); when one
// of them comes in or goes out it tells (areas_poll_event), and what that
// does is the game's.
//
// Who is in an area is followed whatever its triggers: one turned off says
// nothing of who comes and goes, and turned on again it takes those inside
// as already there (no one is told of coming in unseen). An actor that
// dies or is taken away leaves without a word.
//
// An area's id is its own for good: kept with the map, never another's,
// whatever is added or taken away.

#ifndef AREAS_H
#define AREAS_H

#include <stdbool.h>
#include <stdint.h>

#include "actors.h"

enum
{
	AREAS_MAX = 1024,
	AREA_NAME_LENGTH = 32,
	AREA_OCCUPANTS_MAX = 8192,
	AREA_EVENTS_MAX = 256
};

// Who it is for (bits; the rest are free for later)
typedef enum
{
	AREA_TRIGGER_NONE = 0,
	AREA_TRIGGER_PLAYER = 1,
	AREA_TRIGGER_ACTORS = 2
} area_trigger_t;

typedef struct
{
	uint32_t id;
	char name[AREA_NAME_LENGTH];
	float x;                           // its middle, meters
	float z;
	float half_width_meters;           // along its own x
	float half_depth_meters;           // along its own z
	float yaw_radians;                 // turned this much (as the actors: 0 = its depth along +z)
	float bottom_meters;               // how high it goes; top <= bottom: from the ground to the sky
	float top_meters;
	uint8_t triggers;                  // area_trigger_t bits
	uint8_t blocks;                    // the actors' path groups it stops (bits, actors.ini path_groups; 0: none): a locked gate
	bool player_inside;                // (not kept: as of the last update)
} area_t;

typedef enum
{
	AREA_EVENT_ENTER = 1,
	AREA_EVENT_EXIT
} area_event_type_t;

typedef struct
{
	area_event_type_t type;
	uint32_t area_id;
	bool player;                       // the player; else the actor
	uint32_t actor_id;
} area_event_t;

// Who is in which area (sorted by area, then actor)
typedef struct
{
	uint32_t area_id;
	uint32_t actor_id;
} area_occupant_t;

typedef struct
{
	area_t areas[AREAS_MAX];
	int count;
	area_occupant_t inside[AREA_OCCUPANTS_MAX];
	int inside_count;
	area_occupant_t found[AREA_OCCUPANTS_MAX];  // (the update's)
	area_event_t events[AREA_EVENTS_MAX];
	int event_first;
	int event_count;
	uint32_t id_random;
} areas_t;

// None, no one inside, nothing to tell (a new map, a map loaded)
void areas_clear(areas_t *areas);

// An area, the size it is (meters), with a new id; 0 if there is no room
uint32_t areas_add(areas_t *areas, const char *name, float x, float z, float width_meters, float depth_meters, float yaw_radians,
	uint8_t triggers);
// The area with this id (its place in areas_t.areas: it moves as others
// go); -1 if none
int areas_find(const areas_t *areas, uint32_t id);
// Gives an area the id it had (a map loaded); false if another has it
bool areas_set_id(areas_t *areas, int index, uint32_t id);
// False if there was none
bool areas_remove(areas_t *areas, uint32_t id);

// Whether a point is in it (y: only if it has a top)
bool areas_contains(const area_t *area, float x, float y, float z);
// Its corners (x, z each), going round
void areas_corners(const area_t *area, float corners_out[4][2]);
// The topmost area (the last added) at (x, z); -1 if none
int areas_at(const areas_t *areas, float x, float z);

// Once a frame: who came in and went out. player: the player's middle
// (NULL: not on foot, as it was kept)
void areas_update(areas_t *areas, const actors_t *actors, const float player[3]);
bool areas_poll_event(areas_t *areas, area_event_t *event_out);

// Who is in it now (where they are, whatever its triggers; the living
// actors): the actors' ids (at most maximum), how many; player_inside: the
// player too (player NULL: not)
int areas_occupants(const areas_t *areas, const actors_t *actors, uint32_t id, const float player[3], uint32_t *ids_out, int maximum,
	bool *player_inside_out);

#endif
