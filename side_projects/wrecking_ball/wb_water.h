// Water on the ground: a shallow water simulation on the ground's cells
// ("virtual pipes": between each two neighboring cells a pipe whose flow
// grows with the difference of their surfaces and slows with friction).
// No erosion; the ground's edges are walls. Buildings do not stop it.
//
// Depths are at the cells' middles; the ground's heights at their corners
// (wb_world.h), a cell's floor is the mean of its four corners.

#ifndef WB_WATER_H
#define WB_WATER_H

#include <stdbool.h>

#define WB_WATER_DENSITY 1000.0f      // kilograms a cubic meter

typedef struct
{
	int cells;                         // a side
	float cell_meters;
	float origin_x;                    // the first cell's corner
	float origin_z;
	float *floor;                      // cells * cells: the ground under each cell
	float *depth;                      // meters of water on each
	// The flow out of each cell towards each neighbor, cubic meters a
	// second: left (-x), right (+x), up (-z), down (+z)
	float *flow_left;
	float *flow_right;
	float *flow_up;
	float *flow_down;
	double total_volume;               // cubic meters, for the screen
} wb_water_t;

// corner_heights: (cells + 1) a side, row by row along z
bool wb_water_create(wb_water_t *water, const float *corner_heights, int cells, float cell_meters, float origin_x, float origin_z);
void wb_water_destroy(wb_water_t *water);

void wb_water_step(wb_water_t *water, float seconds);

// Water poured over a disc: cubic meters spread over its cells
void wb_water_pour(wb_water_t *water, float x, float z, float radius, float cubic_meters);
// Every cell raised to at least this height of surface (a flood), or
// emptied with a height below the ground
void wb_water_fill_to(wb_water_t *water, float surface_height);
void wb_water_clear(wb_water_t *water);

// At a point: the water's depth, its surface's height, how fast it flows
// (meters a second, along x and z); false where it is dry
bool wb_water_sample(const wb_water_t *water, float x, float z, float *depth, float *surface_height, float velocity[2]);

#endif
