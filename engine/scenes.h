// Ready made test scenes, built instead of a generated terrain.
//
//   dam   a valley closed at its upper end, a sand dam across it with a
//         small notch in the crest, the lake behind it (an old lake: the
//         ground under it is already soaked) filled just over the notch
//         and a spring feeding it: the lake spills through the notch at
//         once and the dam starts to wash away.
//   flat  level ground everywhere, nothing on it (the houses' gallery
//         stands on it)

#ifndef SCENES_H
#define SCENES_H

#include <stdbool.h>

#include "soils.h"
#include "terrain.h"
#include "water.h"

// The scene works best at this size and cell size; the caller creates the
// terrain and water with them.
#define SCENE_DAM_SIZE 256
#define SCENE_DAM_CELL_SIZE_METERS 0.5f

typedef struct
{
	int32_t notch_cell_x;          // middle of the notch in the dam crest
	int32_t notch_cell_y;
	float crest_meters;            // crest height away from the notch
	vec3_t view_target;            // a good point to look at
} scene_dam_info_t;

// Builds the dam scene into an already created terrain and an empty water
// simulation.
void scenes_build_dam(terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings, scene_dam_info_t *info_out);

// The flat scene's size: small, quick to draw and simulate
#define SCENE_FLAT_SIZE 768
#define SCENE_FLAT_CELL_SIZE_METERS 0.25f

// Builds the flat scene into an already created terrain.
void scenes_build_flat(terrain_t *terrain);

#endif
