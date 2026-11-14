// Cache of generated terrains.
//
// Generating a 4096x4096 map takes seconds, so every generated terrain is
// saved in the user's preference directory and loaded instead of
// regenerated when the same seed and settings come up again. The file name
// is a hash of the generator version, seed, size and every terrain setting,
// so changing anything (including the generator code, via
// TERRAIN_GENERATOR_VERSION) simply misses the cache.
//
// Only the newest cache_maximum_files files are kept.

#ifndef TERRAIN_CACHE_H
#define TERRAIN_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"
#include "soils.h"
#include "terrain.h"

// Tries to fill an already created terrain from the cache. Returns true on
// a hit; logs and returns false otherwise.
bool terrain_cache_load(terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings, uint32_t seed);

// Saves a freshly generated terrain and prunes old cache files.
void terrain_cache_save(const terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings,
	const cache_settings_t *cache_settings);

#endif
