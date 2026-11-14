// Test scenes. See scenes.h.

#include "scenes.h"
#include "erosion.h"
#include "stamps.h"

#include <SDL3/SDL.h>

// Valley shape, meters
#define DAM_VALLEY_FLOOR_START 16.0f      // floor height at the upper end
#define DAM_VALLEY_FLOOR_DROP 6.0f        // how much the floor falls along the map
#define DAM_VALLEY_WALL_CURVE 0.004f      // wall height = curve * distance^2
#define DAM_VALLEY_WALL_MAXIMUM 15.0f
#define DAM_UPPER_WALL_METERS 15.0f       // ridge closing the upper end
#define DAM_UPPER_WALL_WIDTH_METERS 20.0f
#define DAM_SOIL_DEPTH_METERS 3.0f
// The dam: where it stands (fraction of the map along the valley), how far
// its ends reach up the valley walls, how much its crest rises above them
#define DAM_POSITION_FRACTION 0.4f
#define DAM_HALF_LENGTH_METERS 30.0f
#define DAM_HEIGHT_ABOVE_ENDS_METERS 2.0f
#define DAM_NOTCH_DEPTH_METERS 0.15f
#define DAM_NOTCH_HALF_WIDTH_METERS 1.5f
// The lake starts this far above the bottom of the notch
#define DAM_LAKE_OVER_NOTCH_METERS 0.02f

static int32_t meters_to_units(float meters)
{
	return (int32_t)SDL_lroundf(meters * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
}

void scenes_build_dam(terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings, scene_dam_info_t *info_out)
{
	int32_t size = terrain->size;
	float cell_size = terrain->cell_size_meters;
	float extent = (float)size * cell_size;
	float center_x = extent * 0.5f;
	float dam_z = extent * DAM_POSITION_FRACTION;
	stamp_t dam;
	int32_t dam_row = (int32_t)SDL_lroundf(dam_z / cell_size);
	int32_t notch_x = (int32_t)SDL_lroundf(center_x / cell_size);
	int32_t crest_units;
	int32_t lake_level_units;

	terrain->seed = 0;
	// High enough that the snow line stays far above the scene
	terrain->max_height_meters = 200.0f;

	// The valley: a parabolic cross section, a floor falling gently along
	// +Z, and a ridge closing the upper end so the lake cannot leave there
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			int32_t index = terrain_index(terrain, x, y);
			float world_x = (float)x * cell_size;
			float world_z = (float)y * cell_size;
			float across = world_x - center_x;
			float floor = DAM_VALLEY_FLOOR_START - DAM_VALLEY_FLOOR_DROP * world_z / extent;
			float wall = SDL_min(DAM_VALLEY_WALL_CURVE * across * across, DAM_VALLEY_WALL_MAXIMUM);
			float upper_ridge = DAM_UPPER_WALL_METERS * (1.0f - smoothstep_float(0.0f, DAM_UPPER_WALL_WIDTH_METERS, world_z));
			float height = floor + SDL_max(wall, upper_ridge);

			terrain->heights[index] = meters_to_units(height);
			terrain->bedrock_heights[index] = meters_to_units(height - DAM_SOIL_DEPTH_METERS);
			terrain->silt_thickness[index] = 0;
			// Earth on the walls, clay on the valley floor
			terrain->soil_types[index] = (uint8_t)(wall < 0.5f ? SOIL_CLAY : SOIL_EARTH);
		}
	}

	// The dam, stamped like the player would, out of sand
	SDL_zero(dam);
	dam.type = STAMP_DAM;
	dam.soil = SOIL_SAND;
	dam.height_meters = DAM_HEIGHT_ABOVE_ENDS_METERS;
	dam.start = vec3_make(center_x - DAM_HALF_LENGTH_METERS, 0.0f, dam_z);
	dam.end = vec3_make(center_x + DAM_HALF_LENGTH_METERS, 0.0f, dam_z);
	stamps_apply(terrain, NULL, &dam);
	crest_units = terrain->heights[terrain_index(terrain, notch_x + (int32_t)(5.0f / cell_size), dam_row)];

	// A small notch in the middle of the crest, so the overflow starts in
	// one place (on a perfectly level crest it would spill everywhere)
	for (int32_t y = dam_row - (int32_t)(4.0f / cell_size); y <= dam_row + (int32_t)(4.0f / cell_size); y++)
	{
		for (int32_t x = notch_x - (int32_t)(DAM_NOTCH_HALF_WIDTH_METERS / cell_size); x <= notch_x + (int32_t)(DAM_NOTCH_HALF_WIDTH_METERS / cell_size); x++)
		{
			int32_t index = terrain_index(terrain, x, y);

			terrain->heights[index] = SDL_min(terrain->heights[index], crest_units - meters_to_units(DAM_NOTCH_DEPTH_METERS));
		}
	}
	terrain_fix_layers(terrain, 0, 0, size - 1, size - 1);

	// The lake: everything above the dam lower than the level gets water.
	// The valley rises behind the dam, so the lake ends by itself.
	lake_level_units = crest_units - meters_to_units(DAM_NOTCH_DEPTH_METERS - DAM_LAKE_OVER_NOTCH_METERS);
	water_clear(water);
	for (int32_t y = 0; y < dam_row; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			int32_t index = terrain_index(terrain, x, y);

			if (terrain->heights[index] < lake_level_units)
			{
				// The lake has been there for a while: the ground under
				// it is soaked
				int32_t capacity = erosion_moisture_capacity_units(terrain, soil_settings, index);

				water_add_units(water, x, y, lake_level_units - terrain->heights[index]);
				water->moisture[index] = (uint16_t)capacity;
				water->moisture_units += capacity;
			}
		}
	}
	// The lake was written here directly: it goes up to the GPU
	water_mark_cells_edited(water, 0, 0, size - 1, dam_row);
	for (int32_t tile = 0; tile < water->tiles_per_side * water->tiles_per_side; tile++)
	{
		int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
		int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

		water->tile_moisture[tile] = 0;
		for (int32_t y = start_y; y < start_y + WATER_TILE_SIZE; y++)
		{
			for (int32_t x = start_x; x < start_x + WATER_TILE_SIZE; x++)
			{
				water->tile_moisture[tile] += water->moisture[terrain_index(terrain, x, y)];
			}
		}
	}
	// A spring at the upper end of the lake keeps it rising
	water_toggle_spring(water, center_x, DAM_UPPER_WALL_WIDTH_METERS + 5.0f);
	water->drain_edges = true;

	terrain_mark_all_changed(terrain);
	info_out->notch_cell_x = notch_x;
	info_out->notch_cell_y = dam_row;
	info_out->crest_meters = (float)crest_units * TERRAIN_METERS_PER_HEIGHT_UNIT;
	info_out->view_target = vec3_make(center_x, info_out->crest_meters, dam_z);
	SDL_Log("Dam scene: %dx%d cells of %.2f m, crest at %.2f m, lake level %.2f m", size, size, (double)cell_size,
		(double)info_out->crest_meters, (double)((float)lake_level_units * TERRAIN_METERS_PER_HEIGHT_UNIT));
}

// The flat scene's ground: its height, and the soil over the rock
#define FLAT_HEIGHT_METERS 10.0f
#define FLAT_SOIL_DEPTH_METERS 3.0f

void scenes_build_flat(terrain_t *terrain)
{
	int32_t size = terrain->size;

	terrain->seed = 0;
	terrain->max_height_meters = 80.0f;
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			int32_t index = terrain_index(terrain, x, y);

			terrain->heights[index] = meters_to_units(FLAT_HEIGHT_METERS);
			terrain->bedrock_heights[index] = meters_to_units(FLAT_HEIGHT_METERS - FLAT_SOIL_DEPTH_METERS);
			terrain->silt_thickness[index] = 0;
			terrain->soil_types[index] = (uint8_t)SOIL_EARTH;
		}
	}
	terrain_fix_layers(terrain, 0, 0, size - 1, size - 1);
	terrain_mark_all_changed(terrain);
}
