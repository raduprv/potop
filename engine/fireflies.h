// Fireflies: on dark nights, small swarms of blinking lights near the eye,
// on grassy ground and most of all near water. Each swarm is a continuous
// particle effect ("firefly" in particles.ini) that lasts a while and is
// replaced by another somewhere else; none by day, in the rain, on rock,
// sand or water. render.fireflies says how many (0: none).

#ifndef FIREFLIES_H
#define FIREFLIES_H

#include <stdbool.h>
#include <stdint.h>

#include "particles.h"
#include "terrain.h"
#include "water.h"

#define FIREFLY_SWARMS_MAX 12

typedef struct
{
	bool used;
	uint32_t emitter;              // particles' emitter id
	float seconds_left;
	float x;
	float z;
} firefly_swarm_t;

typedef struct
{
	firefly_swarm_t swarms[FIREFLY_SWARMS_MAX];
	uint32_t random_state;
	float next_change_seconds;     // a swarm is added or taken away at most this often
	int effect;                    // -1: not looked up yet, -2: none in particles.ini
} fireflies_t;

void fireflies_initialize(fireflies_t *fireflies);

// Once a frame: density 0..1 (render.fireflies), darkness 0..1 (lights_darkness)
void fireflies_update(fireflies_t *fireflies, particles_t *particles, const terrain_t *terrain, const water_t *water, const float eye[3],
	float density, float darkness, float frame_seconds);

// All gone at once (a new map, the effects reloaded)
void fireflies_clear(fireflies_t *fireflies, particles_t *particles);

#endif
