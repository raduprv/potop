// The inspector: everything the simulation knows about one cell of the
// map, for finding out why the water does what it does.
//
// The inspect tool shows the cell under the pointer; a click pins it (it
// stays shown while the camera moves), a click on the pinned cell lets it
// go. The window lists the cell's water, ground and mud, what each of its
// four pipes does and which rule decides it (meniscus, static friction,
// superfluid...), what the tile it is in does and what keeps it awake,
// erosion, soaking and drying, the levels around it and the nearest body.
// A graph shows the cell's level and flow over the last ten seconds; Log
// writes it all to the log, to paste somewhere.
//
// The pipe rules come from the same code the simulation runs (see
// water_inspect_cell), so what the inspector says is what happens.

#ifndef INSPECTOR_H
#define INSPECTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "erosion.h"
#include "objects.h"
#include "physics.h"
#include "settings.h"
#include "soils.h"
#include "terrain.h"
#include "water.h"

#define INSPECTOR_LINES 56
#define INSPECTOR_LINE_LENGTH 200
#define INSPECTOR_HISTORY 300
#define INSPECTOR_GRID 5

typedef enum
{
	INSPECTOR_CELL_OUTSIDE = 0,
	INSPECTOR_CELL_DRY,
	INSPECTOR_CELL_FILM,           // too thin to flow
	INSPECTOR_CELL_THIN,           // up to thin_water_millimeters
	INSPECTOR_CELL_DEEP,
	INSPECTOR_CELL_WALL            // a house or wall stands there
} inspector_cell_kind_t;

typedef struct
{
	bool pinned;
	bool valid;                    // a cell is shown
	int32_t cell_x;
	int32_t cell_y;

	// The cell over time, one sample per simulation step: its level in mm
	// (against the first sample) and the speed of its busiest pipe
	int32_t history_cell_x;
	int32_t history_cell_y;
	int32_t history_base_level;
	float level_history[INSPECTOR_HISTORY];
	float speed_history[INSPECTOR_HISTORY];
	int history_count;
	int history_next;

	// Filled in by inspector_describe; lines starting with # are headings
	char lines[INSPECTOR_LINES][INSPECTOR_LINE_LENGTH];
	int line_count;
	// Levels around the cell in mm, against the cell's own
	float grid[INSPECTOR_GRID][INSPECTOR_GRID];
	uint8_t grid_kind[INSPECTOR_GRID][INSPECTOR_GRID]; // inspector_cell_kind_t

	bool log_requested;            // set by the interface
	bool unpin_requested;
} inspector_t;

typedef struct
{
	const water_t *water;
	const terrain_t *terrain;
	const erosion_t *erosion;
	const physics_t *physics;
	const objects_t *objects;
	const settings_t *settings;
	const soil_settings_t *soils;
	float step_seconds;            // one simulation step
} inspector_world_t;

// Shows this cell from now on (-1: none); the history starts over when the
// cell changes
void inspector_set_cell(inspector_t *inspector, int32_t cell_x, int32_t cell_y);

// Once per simulation step: adds the cell's level and flow to the history
void inspector_record(inspector_t *inspector, const water_t *water, const terrain_t *terrain);

// Fills in the lines and the grid for the current cell
void inspector_describe(inspector_t *inspector, const inspector_world_t *world);

// Writes the lines and the grid to the log
void inspector_log(const inspector_t *inspector);

#endif
