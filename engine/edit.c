// Changing the world by hand (edit.h)

#include "edit.h"

#include <SDL3/SDL.h>

#include "material_map.h"
#include "populate.h"

// Cheap integer hash, used to dither the soft edge of the soil brush
static uint32_t hash_cell(int32_t x, int32_t y)
{
	uint32_t value = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u;

	value ^= value >> 13;
	value *= 0x5BD1E995u;
	value ^= value >> 15;
	return value;
}

// Smooth bell from 1 at the center to 0 at the brush edge
static float brush_falloff(float distance, float radius)
{
	if (distance >= radius)
	{
		return 0.0f;
	}
	return 0.5f * (1.0f + SDL_cosf(MATH3D_PI * distance / radius));
}

// The water wakes over the cells a brush covered (a cell beyond its edge
// too): it reacts to the ground's new shape
static void wake_water(world_t *world, float x, float z, float radius_meters)
{
	float cell_size = world->terrain.cell_size_meters;
	float radius_cells = radius_meters / cell_size + 1.0f;
	float center_x = x / cell_size;
	float center_y = z / cell_size;
	int32_t last = world->terrain.size - 1;

	water_wake_cells(&world->water, clamp_int((int32_t)SDL_floorf(center_x - radius_cells), 0, last),
		clamp_int((int32_t)SDL_floorf(center_y - radius_cells), 0, last), clamp_int((int32_t)SDL_ceilf(center_x + radius_cells), 0, last),
		clamp_int((int32_t)SDL_ceilf(center_y + radius_cells), 0, last));
}

bool edit_terrain(world_t *world, edit_brush_t brush, float x, float z, float radius_meters, float rate, soil_type_t soil, float seconds)
{
	terrain_t *terrain = &world->terrain;
	float cell_size = terrain->cell_size_meters;
	float radius_cells = radius_meters / cell_size;
	float center_x = x / cell_size;
	float center_y = z / cell_size;
	int32_t minimum_x = clamp_int((int32_t)SDL_floorf(center_x - radius_cells), 0, terrain->size - 1);
	int32_t minimum_y = clamp_int((int32_t)SDL_floorf(center_y - radius_cells), 0, terrain->size - 1);
	int32_t maximum_x = clamp_int((int32_t)SDL_ceilf(center_x + radius_cells), 0, terrain->size - 1);
	int32_t maximum_y = clamp_int((int32_t)SDL_ceilf(center_y + radius_cells), 0, terrain->size - 1);
	int32_t region_width = maximum_x - minimum_x + 1;
	int32_t region_height = maximum_y - minimum_y + 1;
	float raise_units = rate * seconds * (float)TERRAIN_HEIGHT_UNITS_PER_METER;

	if (terrain->baked)
	{
		return false;
	}
	if (region_width <= 0 || region_height <= 0)
	{
		return true;
	}
	world_touch_water(world);
	if (brush == EDIT_SMOOTH)
	{
		// Smoothing reads neighbors, so it works from a copy of the region
		// (plus a one cell border) to stay independent of visiting order
		int32_t needed = (region_width + 2) * (region_height + 2);

		if (needed > world->edit_scratch_capacity)
		{
			int32_t *grown = SDL_realloc(world->edit_scratch, (size_t)needed * sizeof(int32_t));

			if (grown == NULL)
			{
				return true;
			}
			world->edit_scratch = grown;
			world->edit_scratch_capacity = needed;
		}
		for (int32_t y = 0; y < region_height + 2; y++)
		{
			for (int32_t cell_x = 0; cell_x < region_width + 2; cell_x++)
			{
				world->edit_scratch[y * (region_width + 2) + cell_x] = terrain_height_units_clamped(terrain, minimum_x + cell_x - 1, minimum_y + y - 1);
			}
		}
	}
	for (int32_t y = minimum_y; y <= maximum_y; y++)
	{
		for (int32_t cell_x = minimum_x; cell_x <= maximum_x; cell_x++)
		{
			float distance = SDL_sqrtf(((float)cell_x - center_x) * ((float)cell_x - center_x) + ((float)y - center_y) * ((float)y - center_y));
			float falloff = brush_falloff(distance, radius_cells);
			int32_t index = terrain_index(terrain, cell_x, y);

			if (falloff <= 0.0f)
			{
				continue;
			}
			switch (brush)
			{
				case EDIT_RAISE:
					terrain->heights[index] += (int32_t)(raise_units * falloff);
					// New material on top is of the soil given, and buries
					// the silt there
					if (falloff > 0.3f)
					{
						terrain->soil_types[index] = (uint8_t)soil;
						terrain->silt_thickness[index] = 0;
					}
					break;
				case EDIT_DIG:
					terrain->heights[index] = SDL_max(0, terrain->heights[index] - (int32_t)(raise_units * falloff));
					break;
				case EDIT_PAINT_SOIL:
					// Dithered edge: cells near the rim are painted with a
					// probability that follows the falloff
					if (falloff * 1.6f > (float)(hash_cell(cell_x, y) & 0xFFFF) / 65535.0f)
					{
						terrain->soil_types[index] = (uint8_t)soil;
						terrain->silt_thickness[index] = 0;
					}
					break;
				case EDIT_SMOOTH:
				{
					int32_t stride = region_width + 2;
					int32_t local_x = cell_x - minimum_x + 1;
					int32_t local_y = y - minimum_y + 1;
					const int32_t *scratch = world->edit_scratch;
					int64_t neighbor_sum = (int64_t)scratch[local_y * stride + local_x - 1] + scratch[local_y * stride + local_x + 1]
						+ scratch[(local_y - 1) * stride + local_x] + scratch[(local_y + 1) * stride + local_x];
					float average = (float)neighbor_sum * 0.25f;
					float current = (float)scratch[local_y * stride + local_x];
					float amount = clamp_float(rate * seconds * falloff, 0.0f, 1.0f);

					terrain->heights[index] = (int32_t)SDL_lroundf(current + (average - current) * amount);
					break;
				}
			}
		}
	}
	// Digging below the bedrock lowers it; silt never outgrows its column
	terrain_fix_layers(terrain, minimum_x, minimum_y, maximum_x, maximum_y);
	terrain_mark_cells_changed(terrain, minimum_x, minimum_y, maximum_x, maximum_y);
	wake_water(world, x, z, radius_meters);
	return true;
}

bool edit_water(world_t *world, float x, float z, float radius_meters, float meters_per_second, float seconds)
{
	if (world->terrain.baked)
	{
		return false;
	}
	world_touch_water(world);
	water_apply_brush(&world->water, x, z, radius_meters, meters_per_second, seconds);
	return true;
}

bool edit_add_water(world_t *world, float x, float z, float cubic_meters, float radius_meters)
{
	// The brush's soft shape, 0.5 (1 + cos(pi r / R)), holds this share of a
	// cylinder as high as its middle: (1/2 - 2/pi^2)
	static const float SHAPE_SHARE = 0.29736f;
	float middle_meters;

	if (world->terrain.baked || cubic_meters <= 0.0f || radius_meters <= 0.0f)
	{
		return false;
	}
	middle_meters = cubic_meters / (SHAPE_SHARE * MATH3D_PI * radius_meters * radius_meters);
	world_touch_water(world);
	water_apply_brush(&world->water, x, z, radius_meters, middle_meters, 1.0f);
	return true;
}

bool edit_stamp(world_t *world, const stamp_t *stamp)
{
	if (world->terrain.baked)
	{
		return false;
	}
	world_touch_water(world);
	stamps_apply(&world->terrain, &world->water, stamp);
	return true;
}

bool edit_toggle_spring(world_t *world, float x, float z)
{
	if (world->terrain.baked)
	{
		return false;
	}
	world_touch_water(world);
	water_toggle_spring(&world->water, x, z);
	return true;
}

int edit_plant(world_t *world, int category, float x, float z, float radius_meters, float seconds, random_t *random)
{
	int planted = populate_brush(&world->objects, &world->terrain, &world->water, &world->settings->objects, category, x, z, radius_meters, seconds,
		random);

	if (planted > 0)
	{
		material_map_stamp_objects(&world->terrain, &world->objects, x, z, radius_meters);
	}
	return planted;
}

int edit_clear(world_t *world, float x, float z, float radius_meters)
{
	return objects_remove_in_radius(&world->objects, x, z, radius_meters, -1);
}

uint32_t edit_place_object(world_t *world, int type, float x, float z, float yaw_radians, float scale, bool awake)
{
	const object_types_t *types = &world->objects.types;

	if (type < 0 || type >= types->type_count || types->types[type].model < 0
		|| !objects_add(&world->objects, &world->terrain, type, x, z, yaw_radians, scale, 0))
	{
		return 0;
	}
	if (types->types[type].physics == OBJECT_PHYSICS_BODY && awake)
	{
		object_instance_t *instance = &world->objects.instances[world->objects.instance_count - 1];

		// Loose, on the ground: the physics settles it (down a slope it rolls)
		instance->state |= OBJECT_STATE_FREE;
		instance->y = instance->ground_height + 0.02f;
		// -2: its body starts awake (see physics.c)
		instance->body = -2;
	}
	return world->objects.instances[world->objects.instance_count - 1].id;
}

bool edit_place_layout(world_t *world, int layout, float x, float z, random_t *random)
{
	if (layout < 0 || layout >= world->layouts.count)
	{
		return false;
	}
	world_touch_water(world);
	layouts_place(&world->layouts, &world->objects, &world->terrain, &world->water, layout, x, z, random);
	material_map_stamp_objects(&world->terrain, &world->objects, x, z, world->layouts.layouts[layout].footprint_meters + 4.0f);
	return true;
}

int edit_remove_layout(world_t *world, float x, float z)
{
	int group = objects_nearest_group(&world->objects, x, z, 30.0f);

	if (group == 0)
	{
		return 0;
	}
	world_touch_water(world);
	return objects_remove_group(&world->objects, group);
}

bool edit_place_light(world_t *world, const float position[3], const light_source_t *source)
{
	return lights_place(&world->lights, position, source);
}

bool edit_remove_light(world_t *world, const float position[3], float radius_meters)
{
	return lights_remove_near(&world->lights, position, radius_meters);
}
