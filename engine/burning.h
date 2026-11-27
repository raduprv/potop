// Objects on fire (data/burning.ini): a log lit at a point burns from
// there outwards through its body, chars, glows in its cracks, burns
// down to ash and crumbles away.
//
// The fire is a front: a sphere growing from where it was lit, in the
// object's own (model) space, spread_meters_per_second; every point of
// the object caught when the front reached it, and how long ago that was
// says what it looks like (object.frag: scorched ahead of the front,
// then charred over char_seconds, embers glowing in the cracks, ash
// after burn_seconds, crumbling away over ash_seconds). The shader is
// given each fire's front and the object's placement (objects_t's
// burn_uniforms, the instance's burn_slot): nothing per vertex, nothing
// baked.
//
// The flames are particles (particles.ini's flame_effect) at sites
// along the object's length, each lit as the front reaches it, flaring
// up, then dying down to embers; one smoke and one embers emitter over
// the whole of it, as strong as how much of it burns. They follow the
// object if it moves (a log the physics rolls).
//
// Not yet: fire going over to what is near (only the object lit burns).

#ifndef BURNING_H
#define BURNING_H

#include <stdbool.h>
#include <stdint.h>

#include "objects.h"
#include "particles.h"

#define BURNING_SITES_MAX 12
#define BURNING_EFFECT_NAME_LENGTH 32

typedef struct
{
	float spread_meters_per_second;    // how fast the front goes through it
	float char_seconds;                // a point caught: black all through after this
	float burn_seconds;                // a point caught: its flames and embers out, ash, after this
	float ash_seconds;                 // then it crumbles away over this
	float site_spacing_meters;         // the flames' sites along it, this far apart
	char flame_effect[BURNING_EFFECT_NAME_LENGTH];
	char smoke_effect[BURNING_EFFECT_NAME_LENGTH];
	char embers_effect[BURNING_EFFECT_NAME_LENGTH];
} burning_settings_t;

typedef struct
{
	bool used;
	uint32_t object_id;                // the object's id (object_instance_t.id)
	float ignite_model[3];             // where it was lit, in its model's space (meters)
	float elapsed_seconds;
	float reach_meters;                // the farthest of the model from where it was lit: the front past it, all of it has caught
	// Its model's space turned so that its longest side lies along X (the
	// shader's: the char's cracks run along the grain)
	float to_grain[4];
	int site_count;
	float site_model[BURNING_SITES_MAX][3];
	float site_distance[BURNING_SITES_MAX];      // from where it was lit (model meters)
	uint32_t flame_emitter[BURNING_SITES_MAX];   // 0: none playing
	uint32_t smoke_emitter;
	uint32_t embers_emitter;
} burning_fire_t;

typedef struct
{
	burning_settings_t settings;
	burning_fire_t fires[OBJECTS_BURN_SLOTS];
	int flame_effect;                  // particles.ini's (-1: none)
	int smoke_effect;
	int embers_effect;
} burning_t;

// Reads data/burning.ini (defaults where it says nothing) and finds its
// effects in particles.ini
void burning_initialize(burning_t *burning, const particles_t *particles);

// Sets the object (an instance's index) on fire at a point of it (world,
// meters); false if it is burning already or every fire's slot is taken
bool burning_ignite(burning_t *burning, objects_t *objects, particles_t *particles, int instance_index, const float point[3]);

// Once a frame: the fronts go on, the flames start, follow and die down,
// what has crumbled away is taken off the map; the shader's numbers
// (objects_t.burn_uniforms) for this frame
void burning_update(burning_t *burning, objects_t *objects, particles_t *particles, float seconds);

// Puts every fire out (their objects as they are now, unburnt again)
void burning_clear(burning_t *burning, objects_t *objects, particles_t *particles);

int burning_count(const burning_t *burning);

#endif
