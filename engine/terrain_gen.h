// Procedural terrain generation.
//
// Pipeline:
//   1. domain warped fBm for rolling hills, plus ridged noise for
//      mountains, mixed by a low frequency "where are the mountains" mask;
//   2. a power curve that flattens the lowlands (good places for water);
//      then, if a sketch is painted (terrain_sketch.h), the land's shape
//      the sketch's (unpainted: low land), the seed's detail over it more
//      or less as its relief says;
//      then the rain's erosion (droplets running downhill: gullies, valleys,
//      hollows filled), worked out on a grid of at most 1024 a side; then
//      the sketch's lakes sunk in;
//   3. thermal erosion: material slides down slopes steeper than the talus
//      angle, which removes noise spikes and gives natural scree slopes;
//   4. soil classification from height, slope and noise;
//   5. bedrock: soil is deep in flat valleys and thin on steep slopes and
//      peaks; rock cells are bare bedrock.

#ifndef TERRAIN_GEN_H
#define TERRAIN_GEN_H

#include <stdint.h>

#include "settings.h"
#include "terrain_sketch.h"
#include "soils.h"
#include "terrain.h"

// Bump whenever the generator's output for the same settings changes, so
// cached terrains from older versions are not reused
#define TERRAIN_GENERATOR_VERSION 7

// terrain_generate_heights' steps besides the land and the sketch
#define TERRAIN_HEIGHTS_LAND 4u        // the seed's land (without: heights holds it already, as this made it)
#define TERRAIN_HEIGHTS_EROSION 1u     // the rain's erosion
#define TERRAIN_HEIGHTS_FINISH 2u      // the thermal erosion, the steep faces' gullies (fine detail: not for a small preview)

// Generates heights, soil types and bedrock for an already created terrain
// and marks everything as changed. The soil settings give the soil depth.
// sketch: what is painted of it over the seed's land (NULL or nothing
// painted: the seed's alone); its lakes are found (terrain_sketch_t's
// lake_labels, lake_surfaces) for the world to fill
void terrain_generate(terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings, uint32_t seed,
	terrain_sketch_t *sketch);

// The heights alone (meters, size squared, row by row) for a map of that
// many cells of cell_size_meters: the same land as terrain_generate's at
// any size (a small one for a picture of it, a quarter for a preview)
bool terrain_generate_heights(float *heights, int32_t size, float cell_size_meters, const terrain_settings_t *settings, uint32_t seed,
	terrain_sketch_t *sketch, unsigned int steps);

#endif
