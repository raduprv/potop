// What water does to the ground: erosion, deposition and bank collapse on
// the GPU (water_erode.comp, water_collapse.comp), the ground's moisture on
// the CPU.
//
//   erosion       flow faster than a soil's critical speed picks material
//                 up, until the water carries as much as its speed and
//                 depth allow. Under flowing water the ground is soaked, so
//                 the wet critical speed applies. Silt (earlier deposits)
//                 goes first, then the soil; the bedrock never erodes.
//   deposition    water carrying more than it can (because it slowed
//                 down) drops the excess as silt.
//   collapse      banks steeper than the soil's wet talus slope slide into
//                 the water where water stands at their foot, which is what
//                 really breaks a dam: the overflow cuts a notch, the notch
//                 sides slump in. Dry silt slides too.
//   moisture      ground under water, or in the rain, gets wet up to what it
//                 holds; elsewhere it dries. It is not taken from the water
//                 (a coarse number for the farms, the dark wet look and the
//                 game's rules), and is updated every few seconds, a share
//                 of the tiles each step.
//
// Every amount of material is an integer in TERRAIN_HEIGHT_UNITS_PER_METER
// units and moves between exactly two places (ground and sediment, or one
// cell and its neighbor), so material is conserved exactly.

#ifndef EROSION_H
#define EROSION_H

#include "soils.h"
#include "terrain.h"
#include "water.h"

typedef struct
{
	int32_t tile_count;
	int32_t wet_phase;             // which share of the tiles gets wet this step
	int32_t step_count;
	// Totals (from the GPU, a frame or two behind)
	int64_t eroded_units_total;    // ever picked up
	int64_t deposited_units_total; // ever dropped
	int64_t collapsed_units_total; // ever moved by bank collapse
} erosion_t;

bool erosion_create(erosion_t *erosion, const water_t *water);
void erosion_destroy(erosion_t *erosion);
void erosion_reset(erosion_t *erosion);

// Runs after water_step. Changes heights, silt and sediment on the GPU,
// moisture here. shadow_threshold_units: once the accumulated terrain
// change exceeds this, the shadow map is flagged for a redraw.
void erosion_step(erosion_t *erosion, terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings,
	const water_settings_t *water_settings, float step_seconds, int64_t shadow_threshold_units);

// Ground moisture a cell can hold, in water units (depends on the top
// layer and on how much soil lies over the bedrock).
int32_t erosion_moisture_capacity_units(const terrain_t *terrain, const soil_settings_t *soil_settings, int32_t index);

#endif
