// Water on the ground: a shallow water simulation on the ground's cells
// ("virtual pipes": between each two neighboring cells a pipe whose flow
// grows with the difference of their surfaces and slows with friction).
// No erosion; the ground's edges are walls. Buildings do not stop it.
//
// Depths are at the cells' middles; the ground's heights at their corners
// (the caller's), a cell's floor is the mean of its four corners.

#ifndef SHALLOW_WATER_H
#define SHALLOW_WATER_H

#include <stdbool.h>

#define SHALLOW_WATER_DENSITY 1000.0f      // kilograms a cubic meter

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
} shallow_water_t;

// corner_heights: (cells + 1) a side, row by row along z
bool shallow_water_create(shallow_water_t *water, const float *corner_heights, int cells, float cell_meters, float origin_x, float origin_z);
void shallow_water_destroy(shallow_water_t *water);

void shallow_water_step(shallow_water_t *water, float seconds);

// Water poured over a disc: cubic meters spread over its cells
void shallow_water_pour(shallow_water_t *water, float x, float z, float radius, float cubic_meters);
// Every cell raised to at least this height of surface (a flood), or
// emptied with a height below the ground
void shallow_water_fill_to(shallow_water_t *water, float surface_height);
void shallow_water_clear(shallow_water_t *water);
// A blast at (x, y, z) of kilograms of explosive: the water near it
// thrown out of a crater into a ring round it and set flowing outwards,
// so it runs off as a ring of waves. Less the higher over the surface it
// goes off; nothing past its reach (1.6 m times the cube root of the
// kilograms)
void shallow_water_blast(shallow_water_t *water, float x, float y, float z, float kilograms);

// At a point: the water's depth, its surface's height, how fast it flows
// (meters a second, along x and z); false where it is dry
bool shallow_water_sample(const shallow_water_t *water, float x, float z, float *depth, float *surface_height, float velocity[2]);

#endif
