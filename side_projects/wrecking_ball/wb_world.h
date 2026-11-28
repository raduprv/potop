// The world WreckingBall simulates: a Box3D world with a heightmap ground,
// breakable buildings (engine/wreck.h's model, each read from a .wreck
// file) and the balls thrown at them. Stepped at a fixed rate.
//
// A .wreck file is the engine's form (engine/wrecks.h): [pieces] box and
// log lines, [links] link lines, in the building's own frame with its
// foot at y 0. A building is placed at a point of the ground, turned
// round the vertical.

#ifndef WB_WORLD_H
#define WB_WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "box3d/box3d.h"
#include "shallow_water.h"
#include "wreck.h"

#define WB_WORLD_STEPS_PER_SECOND 60
#define WB_TERRAIN_CELLS 128                 // a side, in cells
#define WB_TERRAIN_CELL_METERS 0.5f
#define WB_BALLS_MAX 8
#define WB_BUILDINGS_MAX 16

typedef struct
{
	b3BodyId body;
	float radius;
	bool used;
} wb_ball_t;

typedef struct
{
	bool used;
	wreck_model_t *model;              // on the heap: its bodies point at it
	char path[256];
	float x;                           // where it was placed, and its turn
	float z;
	float yaw_radians;
	float minimum[3];                  // the box it stood in when placed
	float maximum[3];
	bool knows_water;                  // its model was given the water (wreck_model_set_water)
} wb_building_t;

typedef struct
{
	b3WorldId world;
	wreck_settings_t settings;
	// The ground: heights at the cells' corners, (WB_TERRAIN_CELLS + 1)
	// a side, row by row along z; its corner at (-half, -half)
	float terrain_heights[(WB_TERRAIN_CELLS + 1) * (WB_TERRAIN_CELLS + 1)];
	b3HeightFieldData *terrain_field;
	b3BodyId terrain_body;
	wb_building_t buildings[WB_BUILDINGS_MAX];
	wb_ball_t balls[WB_BALLS_MAX];
	int next_ball;
	int frame_index;
	shallow_water_t water;
} wb_world_t;

// The world with its ground; settings_path: an ini of the model's numbers
// (data/wreck.ini, say; NULL or missing: the model's own)
bool wb_world_create(wb_world_t *world, const char *settings_path);
void wb_world_destroy(wb_world_t *world);

// A building of a .wreck file placed at (x, z) on the ground, turned
// yaw_radians round the vertical; its number, -1 when it cannot be read
// or there are WB_BUILDINGS_MAX already
int wb_world_add_building(wb_world_t *world, const char *path, float x, float z, float yaw_radians);
// Every building and ball taken away
void wb_world_clear(wb_world_t *world);
// Every building again as it was placed (the balls cleared)
void wb_world_reset_buildings(wb_world_t *world);

int wb_world_building_count(const wb_world_t *world);
// The box round every building as placed; false when there is none
bool wb_world_buildings_bounds(const wb_world_t *world, float minimum[3], float maximum[3]);
// Summed over the buildings
int wb_world_piece_count(const wb_world_t *world);
int wb_world_broken_link_count(const wb_world_t *world);

void wb_world_step(wb_world_t *world);

// A ball from a point, flying along a direction at a speed (meters a
// second); the oldest goes when there are WB_BALLS_MAX
void wb_world_throw_ball(wb_world_t *world, const float origin[3], const float direction[3], float speed);
// A blast of kilograms of explosive at a point
void wb_world_blast(wb_world_t *world, const float position[3], float kilograms);
// The first thing along a ray (direction of unit length) within a
// distance; false if nothing
bool wb_world_cast_ray(const wb_world_t *world, const float origin[3], const float direction[3], float distance, float hit_point[3]);

float wb_world_terrain_height(const wb_world_t *world, float x, float z);
// Half the ground's side, in meters
float wb_world_terrain_half_size(void);

#endif
