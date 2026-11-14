// Waterfalls: where the water falls (a crater's lip, a cliff), found in the
// water as it is, and played as particles (data/particles.ini:
// waterfall_curtain, waterfall_mist, waterfall_splash).
//
// A waterfall is where the water's surface drops steeply (more than
// WATERFALLS_STEEP_RISE times a cell's width from one cell to the next,
// downstream) after it did not: its lip. From there the way down is
// followed (always the steepest, while it still slopes) to where the water
// lands: its base. Lips of the same fall, the same way, within a couple of
// meters of each other make one segment, which plays three effects:
//
//   curtain  along the lip, pushed out as fast as the water runs, living
//            as long as the fall takes; the particles slide down what the
//            water runs over and spread on the pool below (collide = slide)
//   mist     at the base, more for a higher fall
//   splash   at the base
//
// Each scaled by how wide the segment is and how much water it carries.
//
// On an active map they are looked for again every few seconds, around the
// player; a baked map has them in its file (found when it was baked: they
// do not change). Only those near the player play.

#ifndef WATERFALLS_H
#define WATERFALLS_H

#include <stdbool.h>
#include <stdint.h>

#include "particles.h"
#include "terrain.h"
#include "water.h"

typedef struct
{
	float lip[3];                      // the middle of the lip, on the water's surface (meters)
	float direction[2];                // the way the water runs over it (x, z; unit length)
	float width_meters;                // along the lip
	float fall_meters;                 // from the lip down to the base
	float flow;                        // cubic meters a second over it
	float speed;                       // the water's speed at the lip (m/s)
	float base[3];                     // where it lands, on the water there
} waterfall_segment_t;

#define WATERFALLS_EFFECTS 3           // curtain, mist, splash

typedef struct
{
	waterfall_segment_t *segments;
	int count;
	int capacity;
	// Their emitters (0: not playing), WATERFALLS_EFFECTS a segment
	uint32_t *emitters;
	int effects[WATERFALLS_EFFECTS];   // particles.ini's, -1 where it has none
	bool fixed;                        // from a baked map: not looked for again
	double next_search_seconds;
	bool warned;                       // the effects missing: said once
} waterfalls_t;

void waterfalls_create(waterfalls_t *waterfalls);
void waterfalls_destroy(waterfalls_t *waterfalls, particles_t *particles);

// Looks for them in the water (all of it, radius_meters < 0; else the tiles
// within radius of x, z) and keeps what it finds. True if that is a new
// list (its emitters cleared: the caller stops those of the old one)
bool waterfalls_search(waterfalls_t *waterfalls, const terrain_t *terrain, const water_t *water, float x, float z, float radius_meters);

// The ones a baked map brings (fixed: not looked for again); none (count
// 0, fixed false): looked for (an active map)
void waterfalls_set(waterfalls_t *waterfalls, particles_t *particles, const waterfall_segment_t *segments, int count, bool fixed);

// Each frame: looked for again now and then (an active map: not while the
// water stands still), the emitters of those near x, z started, of those
// far stopped; all stopped while frozen (the ice holds the water)
void waterfalls_update(waterfalls_t *waterfalls, particles_t *particles, const terrain_t *terrain, const water_t *water, float x, float z,
	double now_seconds, bool frozen);

#endif
