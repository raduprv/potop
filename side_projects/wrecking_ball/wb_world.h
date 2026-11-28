// The world WreckingBall simulates: a Box3D world with a heightmap ground,
// one breakable building (engine/wreck.h's model, read from a .wreck
// file) and the balls thrown at it. Stepped at a fixed rate.
//
// A .wreck file is the engine's form (engine/wrecks.h): [pieces] box and
// log lines, [links] link lines, in the building's own frame with its
// foot at y 0. The building stands in the middle of the ground.

#ifndef WB_WORLD_H
#define WB_WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "box3d/box3d.h"
#include "wreck.h"

#define WB_WORLD_STEPS_PER_SECOND 60
#define WB_TERRAIN_CELLS 128                 // a side, in cells
#define WB_TERRAIN_CELL_METERS 0.5f
#define WB_BALLS_MAX 8

typedef struct
{
	b3BodyId body;
	float radius;
	bool used;
} wb_ball_t;

typedef struct
{
	b3WorldId world;
	wreck_settings_t settings;
	// The ground: heights at the cells' corners, (WB_TERRAIN_CELLS + 1)
	// a side, row by row along z; its corner at (-half, -half)
	float terrain_heights[(WB_TERRAIN_CELLS + 1) * (WB_TERRAIN_CELLS + 1)];
	b3HeightFieldData *terrain_field;
	b3BodyId terrain_body;
	// The building (on the heap: its bodies point at it)
	wreck_model_t *model;
	bool building_loaded;
	char building_path[256];
	float building_minimum[3];         // where it stood when placed: for the camera
	float building_maximum[3];
	wb_ball_t balls[WB_BALLS_MAX];
	int next_ball;
	int frame_index;
	int broken_links_at_start;
} wb_world_t;

// The world with its ground; settings_path: an ini of the model's numbers
// (data/wreck.ini, say; NULL or missing: the model's own)
bool wb_world_create(wb_world_t *world, const char *settings_path);
void wb_world_destroy(wb_world_t *world);

// The building of a .wreck file, in place of the one standing (and the
// balls cleared); false when it cannot be read
bool wb_world_load_building(wb_world_t *world, const char *path);
// The same building again, as it was
bool wb_world_reset_building(wb_world_t *world);

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
