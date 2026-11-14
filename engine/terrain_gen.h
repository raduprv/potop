// Procedural terrain generation.
//
// Pipeline:
//   1. domain warped fBm for rolling hills, plus ridged noise for
//      mountains, mixed by a low frequency "where are the mountains" mask;
//   2. a power curve that flattens the lowlands (good places for water);
//   3. thermal erosion: material slides down slopes steeper than the talus
//      angle, which removes noise spikes and gives natural scree slopes;
//   4. soil classification from height, slope and noise;
//   5. bedrock: soil is deep in flat valleys and thin on steep slopes and
//      peaks; rock cells are bare bedrock.

#ifndef TERRAIN_GEN_H
#define TERRAIN_GEN_H

#include <stdint.h>

#include "settings.h"
#include "soils.h"
#include "terrain.h"

// Bump whenever the generator's output for the same settings changes, so
// cached terrains from older versions are not reused
#define TERRAIN_GENERATOR_VERSION 6

// Generates heights, soil types and bedrock for an already created terrain
// and marks everything as changed. The soil settings give the soil depth.
void terrain_generate(terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings, uint32_t seed);

#endif
