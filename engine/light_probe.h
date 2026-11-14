// How lit a place is, for the game (stealth: who can see whom; an animal
// that seeks the dark to sleep): how much sky it sees (the sky's
// occlusion, sky_occlusion.h) times how much day it is now (the sun; at
// night the moon, by its phase), and the lights near it (fires, lamps,
// torches, flashes: lights.h). No rays: under the trees it is dark because
// they hide the sky, not because of the sun's shadow.

#ifndef LIGHT_PROBE_H
#define LIGHT_PROBE_H

#include "world.h"

typedef struct
{
	float sky_open;                    // how much of the sky the ground there sees, 0..1 (on average a few meters round)
	float daylight;                    // how much day it is now, 0..1 (night: the moon's, a little)
	float local;                       // the lights near it, as the noon sun would light it (1)
	float total;                       // sky_open * daylight + local, at most 1
} light_probe_t;

// How much day it is now, 0..1: the sun up; at night the moon by its
// phase and height, and the stars
float light_probe_daylight(const world_t *world);
void light_probe_at(const world_t *world, float x, float y, float z, light_probe_t *probe_out);

#endif
