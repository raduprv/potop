// Terrain stamps: whole landforms in one click, because building a dam
// with the brush takes forever.
//
//   crater   click: a bowl dug into the ground, with a raised rim
//   dam      drag from one bank to the other: a wall with a flat,
//            horizontal crest (stamp height above the higher end), a crest
//            about 2 m wide and side slopes of 1:2, made of the selected soil
//   hill     click: a round hill of the selected soil
//   channel  drag: a ditch whose bottom slopes evenly from one end to the
//            other, stamp height deep, with 1:1 sides
//   lake     click: a flat bottomed basin, stamp height below its lowest
//            rim point, filled with water up to that point
//   plateau  click: a flat topped mesa of the selected soil
//
// The brush radius sets the size of round stamps and the channel width;
// the stamp height (- and = keys) sets how tall or deep they are. Raising
// stamps take the max of the terrain and the shape, digging ones the min,
// so stamping twice in the same place does nothing new.

#ifndef STAMPS_H
#define STAMPS_H

#include <stdbool.h>

#include "math3d.h"
#include "terrain.h"
#include "water.h"

typedef enum
{
	STAMP_CRATER = 0,
	STAMP_DAM,
	STAMP_HILL,
	STAMP_CHANNEL,
	STAMP_LAKE,
	STAMP_PLATEAU,
	STAMP_COUNT
} stamp_type_t;

typedef struct
{
	stamp_type_t type;
	soil_type_t soil;              // material of raised stamps
	float radius_meters;
	float height_meters;
	vec3_t start;                  // click point, or start of the drag
	vec3_t end;                    // end of the drag (same as start for clicks)
} stamp_t;

const char *stamps_name(stamp_type_t type);

// True for stamps placed by dragging a line (dam, channel).
bool stamps_is_line(stamp_type_t type);

// Half width of the stamp's footprint around its point or line, for the
// preview outline.
float stamps_footprint_meters(const terrain_t *terrain, const stamp_t *stamp);

// Applies the stamp: changes the terrain (and for a lake, adds water),
// fixes the soil layers, flags the chunks and wakes the water around it.
void stamps_apply(terrain_t *terrain, water_t *water, const stamp_t *stamp);

#endif
