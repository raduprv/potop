// What water does to the ground. See erosion.h.

#include "erosion.h"
#include "parallel.h"
#include "profile.h"
#include "water_gpu.h"

#include <SDL3/SDL.h>

// Banks are checked for collapse every this many steps
#define EROSION_COLLAPSE_INTERVAL_STEPS 4
// Every tile's moisture follows the water once every this many seconds.
// The work is spread over the steps in between (each step does its share
// of the tiles), so there is no spike.
#define EROSION_WET_INTERVAL_SECONDS 10.0f
// Soil this thick (meters) holds the full capacity_millimeters of water;
// thinner soil over the bedrock holds proportionally less
#define EROSION_FULL_CAPACITY_SOIL_METERS 1.0f

static const float meters_per_unit = TERRAIN_METERS_PER_HEIGHT_UNIT;
static const float units_per_millimeter = (float)TERRAIN_HEIGHT_UNITS_PER_METER * 0.001f;

typedef struct
{
	int64_t moisture;
	int64_t wetted;
	int64_t dried;
	bool moisture_changed;
} wet_result_t;

bool erosion_create(erosion_t *erosion, const water_t *water)
{
	SDL_zerop(erosion);
	erosion->tile_count = water->tiles_per_side * water->tiles_per_side;
	return true;
}

void erosion_destroy(erosion_t *erosion)
{
	SDL_zerop(erosion);
}

void erosion_reset(erosion_t *erosion)
{
	erosion->wet_phase = 0;
	erosion->step_count = 0;
	erosion->eroded_units_total = 0;
	erosion->deposited_units_total = 0;
	erosion->collapsed_units_total = 0;
}

// The layer that decides how the surface behaves: silt once it is thick
// enough to show, the cell's soil otherwise
static inline int surface_layer(const terrain_t *terrain, const soil_settings_t *soil_settings, int32_t index)
{
	int32_t visible_units = (int32_t)(soil_settings->silt_visible_millimeters * units_per_millimeter);

	return terrain->silt_thickness[index] >= SDL_max(visible_units, 1) ? SOIL_SILT : terrain->soil_types[index];
}

int32_t erosion_moisture_capacity_units(const terrain_t *terrain, const soil_settings_t *soil_settings, int32_t index)
{
	const soil_properties_t *properties = &soil_settings->soils[surface_layer(terrain, soil_settings, index)];
	float soil_meters = (float)(terrain->heights[index] - terrain->bedrock_heights[index]) * meters_per_unit;
	float fraction = clamp_float(soil_meters / EROSION_FULL_CAPACITY_SOIL_METERS, 0.0f, 1.0f);

	// Moisture is stored in 16 bits
	return clamp_int((int32_t)(properties->capacity_millimeters * units_per_millimeter * fraction), 0, 65000);
}

// ---------------------------------------------------------------------------
// The ground's moisture

// Ground under water (or in the rain) gets wet at the soil's infiltration
// rate up to what it holds (in the rain, no faster than it falls); ground
// with nothing on it dries
static void wet_tile(terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings, float rain_units_per_second,
	float elapsed_seconds, int32_t tile, wet_result_t *result)
{
	int32_t size = water->size;
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t drying_units = (int32_t)(soil_settings->drying_millimeters_per_second * units_per_millimeter * elapsed_seconds);

	SDL_zerop(result);
	for (int32_t y = start_y; y < start_y + WATER_TILE_SIZE; y++)
	{
		for (int32_t x = start_x; x < start_x + WATER_TILE_SIZE; x++)
		{
			int32_t index = y * size + x;
			int32_t moisture_units = water->moisture[index];

			if (water->depth[index] > 0 || rain_units_per_second > 0.0f)
			{
				int32_t capacity_units = erosion_moisture_capacity_units(terrain, soil_settings, index);

				if (moisture_units < capacity_units)
				{
					const soil_properties_t *properties = &soil_settings->soils[surface_layer(terrain, soil_settings, index)];
					float rate = properties->infiltration_millimeters_per_second * units_per_millimeter;
					int32_t amount;

					if (water->depth[index] <= 0)
					{
						rate = SDL_min(rate, rain_units_per_second);
					}
					amount = SDL_min((int32_t)(rate * elapsed_seconds), capacity_units - moisture_units);
					if (amount > 0)
					{
						moisture_units += amount;
						result->wetted += amount;
						result->moisture_changed = true;
					}
				}
			}
			else if (moisture_units > 0 && drying_units > 0)
			{
				int32_t amount = SDL_min(moisture_units, drying_units);

				moisture_units -= amount;
				result->dried += amount;
				result->moisture_changed = true;
			}
			water->moisture[index] = (uint16_t)moisture_units;
			result->moisture += moisture_units;
		}
	}
}

// The tiles whose index is phase modulo phase_count
static void wet(terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings, const water_settings_t *water_settings,
	float elapsed_seconds, int32_t phase, int32_t phase_count)
{
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;
	float rain_units_per_second = water->raining ? water_settings->rain_millimeters_per_second * units_per_millimeter : 0.0f;
	int32_t list_count = 0;
	wet_result_t *results;

	// The water's update list is free between steps
	for (int32_t tile = phase; tile < tile_count; tile += phase_count)
	{
		if (rain_units_per_second > 0.0f || water->tile_state[tile] != WATER_TILE_DRY || water->tile_moisture[tile] > 0)
		{
			water->update_tiles[list_count++] = tile;
		}
	}
	if (list_count == 0)
	{
		return;
	}
	frame_profile_count(FRAME_WET_TILES, list_count);
	results = SDL_malloc((size_t)list_count * sizeof(wet_result_t));
	if (results == NULL)
	{
		return;
	}

#pragma omp parallel for schedule(dynamic, 1) num_threads(parallel_threads_for_tiles(list_count))
	for (int32_t i = 0; i < list_count; i++)
	{
		wet_tile(terrain, water, soil_settings, rain_units_per_second, elapsed_seconds, water->update_tiles[i], &results[i]);
	}

	for (int32_t i = 0; i < list_count; i++)
	{
		int32_t tile = water->update_tiles[i];
		const wet_result_t *result = &results[i];

		water->moisture_units += result->wetted - result->dried;
		water->dried_units += result->dried;
		water->tile_moisture[tile] = result->moisture;
		if (result->moisture_changed)
		{
			// The renderer shows wet ground darker
			terrain->chunks[tile].needs_upload = true;
			terrain->any_chunk_needs_upload = true;
		}
	}
	SDL_free(results);
}

// ---------------------------------------------------------------------------

void erosion_step(erosion_t *erosion, terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings,
	const water_settings_t *water_settings, float step_seconds, int64_t shadow_threshold_units)
{
	uint64_t phase_start = profile_begin();
	uint64_t frame_start = phase_start;
	int32_t phase_count = SDL_max(1, (int32_t)SDL_lroundf(EROSION_WET_INTERVAL_SECONDS / step_seconds));

	if (water->baked)
	{
		return;
	}

	// Erosion, deposition and, every few steps, bank collapse, all on the
	// GPU; the changed ground comes back with the water's copies
	erosion->step_count++;
	water_gpu_erosion_step(water, soil_settings, water_settings, step_seconds, erosion->step_count % EROSION_COLLAPSE_INTERVAL_STEPS == 0,
		shadow_threshold_units);
	erosion->eroded_units_total = water->eroded_units;
	erosion->deposited_units_total = water->deposited_units;
	erosion->collapsed_units_total = water->collapsed_units;
	profile_end(PROFILE_EROSION_ERODE, phase_start);
	frame_profile_end(FRAME_EROSION, frame_start);
	if (erosion->step_count % EROSION_COLLAPSE_INTERVAL_STEPS == 0)
	{
		frame_profile_count(FRAME_COLLAPSES, 1);
	}

	phase_start = profile_begin();
	erosion->wet_phase = (erosion->wet_phase + 1) % phase_count;
	wet(terrain, water, soil_settings, water_settings, (float)phase_count * step_seconds, erosion->wet_phase, phase_count);
	profile_end(PROFILE_EROSION_SOAK, phase_start);
	frame_profile_end(FRAME_WETTING, phase_start);
}
