// Changing the world by hand: the ground (raise, dig, paint, smooth,
// stamps), the water (pour, sponge, springs), the objects (plant, clear,
// one of a type, a village's layout) and the placed lights. Each call is
// one stroke's worth at a point; who holds the brush (an editor's tools, a
// game's spell) keeps its own state and calls these.
//
// A baked map's ground and water stay as they are: the calls that would
// change them return false and do nothing. Whatever changes the ground or
// the water wakes the water there (it reacts to the new shape).

#ifndef EDIT_H
#define EDIT_H

#include <stdbool.h>
#include <stdint.h>

#include "lights.h"
#include "noise.h"
#include "stamps.h"
#include "world.h"

typedef enum
{
	EDIT_RAISE = 0,                // adds soil (of the soil given) under a bell
	EDIT_DIG,
	EDIT_PAINT_SOIL,               // the top's soil, a dithered edge
	EDIT_SMOOTH                    // towards the neighbors' average
} edit_brush_t;

// A brush stroke for seconds at (x, z): rate is meters a second (raise,
// dig) or the share a second it smooths by (smooth); false on a baked map
bool edit_terrain(world_t *world, edit_brush_t brush, float x, float z, float radius_meters, float rate, soil_type_t soil, float seconds);

// Water poured (meters_per_second > 0) or sponged (< 0) under the brush
bool edit_water(world_t *world, float x, float z, float radius_meters, float meters_per_second, float seconds);
// That much water (cubic meters) put at once round (x, z), most in the
// middle, gone at radius_meters (the water brush's shape); false on a
// baked map
bool edit_add_water(world_t *world, float x, float z, float cubic_meters, float radius_meters);

// A stamp (stamps.h: a crater, a dam from start to end...), at once
bool edit_stamp(world_t *world, const stamp_t *stamp);

// A spring there, or the one there taken away
bool edit_toggle_spring(world_t *world, float x, float z);

// Plants of a category (object_types_t.categories) under the brush, as
// densely as the category's brush density (for seconds); how many
int edit_plant(world_t *world, int category, float x, float z, float radius_meters, float seconds, random_t *random);
// Every object under the brush away; how many
int edit_clear(world_t *world, float x, float z, float radius_meters);

// Every object of the categories flagged (one per category, true: away)
// under the brush, or over the whole map with a radius under 0 (the
// ground's cover stamped afresh then: none left under what went); how many
int edit_clear_categories(world_t *world, float x, float z, float radius_meters, const bool *categories);

// One object of a type at (x, z), turned and sized; loose things (with a
// body) dropped from drop_meters above the ground, awake. False if it
// could not be put (no model, no room)
uint32_t edit_place_object(world_t *world, int type, float x, float z, float yaw_radians, float scale, bool awake);

// A village's layout (layouts_t) at (x, z); the one nearest (within 30 m)
// taken away: how many objects went
bool edit_place_layout(world_t *world, int layout, float x, float z, random_t *random);
int edit_remove_layout(world_t *world, float x, float z);

// A light at a point (false: no room for more); the one within radius of
// it taken away (false: none there)
bool edit_place_light(world_t *world, const float position[3], const light_source_t *source);
bool edit_remove_light(world_t *world, const float position[3], float radius_meters);

#endif
