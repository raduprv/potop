// Terrain data. See terrain.h.

#include "terrain.h"
#include "parallel.h"

#include "math3d.h"
#include <SDL3/SDL.h>

// Chunks whose bounds are worked out again per thread (fewer: one thread)
#define TERRAIN_BOUNDS_CHUNKS_PER_THREAD 64

static bool create(terrain_t *terrain, int32_t size, float cell_size_meters, bool baked)
{
	int32_t cell_count;

	SDL_zerop(terrain);
	size = ((size + TERRAIN_CHUNK_SIZE - 1) / TERRAIN_CHUNK_SIZE) * TERRAIN_CHUNK_SIZE;
	if (size < TERRAIN_CHUNK_SIZE)
	{
		size = TERRAIN_CHUNK_SIZE;
	}

	terrain->size = size;
	terrain->cell_size_meters = cell_size_meters;
	terrain->chunks_per_side = size / TERRAIN_CHUNK_SIZE;
	cell_count = size * size;

	terrain->baked = baked;
	terrain->heights = SDL_calloc((size_t)cell_count, sizeof(int32_t));
	if (baked)
	{
		terrain->baked_surface = SDL_calloc((size_t)(terrain->chunks_per_side * terrain->chunks_per_side), sizeof(uint8_t *));
	}
	else
	{
		terrain->bedrock_heights = SDL_calloc((size_t)cell_count, sizeof(int32_t));
		terrain->silt_thickness = SDL_calloc((size_t)cell_count, sizeof(int32_t));
	}
	terrain->soil_types = SDL_calloc((size_t)cell_count, sizeof(uint8_t));
	terrain->materials = SDL_calloc((size_t)cell_count, TERRAIN_MATERIAL_BYTES);
	terrain->chunks = SDL_calloc((size_t)(terrain->chunks_per_side * terrain->chunks_per_side), sizeof(terrain_chunk_t));
	if (terrain->heights == NULL || (baked ? terrain->baked_surface == NULL : terrain->bedrock_heights == NULL || terrain->silt_thickness == NULL)
		|| terrain->soil_types == NULL || terrain->materials == NULL || terrain->chunks == NULL)
	{
		terrain_destroy(terrain);
		return false;
	}
	// No cover, no overlay (MATERIAL_NONE), open sky: material_map.h
	for (int32_t i = 0; i < cell_count; i++)
	{
		terrain->materials[(size_t)i * TERRAIN_MATERIAL_BYTES + 1] = 255;
		terrain->materials[(size_t)i * TERRAIN_MATERIAL_BYTES + 2] = 255;
	}
	terrain->materials_dirty = true;
	terrain->materials_dirty_rectangle[2] = size - 1;
	terrain->materials_dirty_rectangle[3] = size - 1;
	return true;
}

bool terrain_create(terrain_t *terrain, int32_t size, float cell_size_meters)
{
	return create(terrain, size, cell_size_meters, false);
}

bool terrain_create_baked(terrain_t *terrain, int32_t size, float cell_size_meters)
{
	return create(terrain, size, cell_size_meters, true);
}

void terrain_destroy(terrain_t *terrain)
{
	if (terrain->baked_surface != NULL)
	{
		for (int32_t i = 0; i < terrain->chunks_per_side * terrain->chunks_per_side; i++)
		{
			SDL_free(terrain->baked_surface[i]);
		}
		SDL_free(terrain->baked_surface);
	}
	SDL_free(terrain->heights);
	SDL_free(terrain->bedrock_heights);
	SDL_free(terrain->silt_thickness);
	SDL_free(terrain->soil_types);
	SDL_free(terrain->materials);
	SDL_free(terrain->chunks);
	SDL_zerop(terrain);
}

float terrain_sample_height_meters(const terrain_t *terrain, float world_x, float world_z)
{
	float grid_x = clamp_float(world_x / terrain->cell_size_meters, 0.0f, (float)(terrain->size - 1));
	float grid_y = clamp_float(world_z / terrain->cell_size_meters, 0.0f, (float)(terrain->size - 1));
	int32_t cell_x = (int32_t)grid_x;
	int32_t cell_y = (int32_t)grid_y;
	float fraction_x = grid_x - (float)cell_x;
	float fraction_y = grid_y - (float)cell_y;
	float height_00 = terrain_height_meters_clamped(terrain, cell_x, cell_y);
	float height_10 = terrain_height_meters_clamped(terrain, cell_x + 1, cell_y);
	float height_01 = terrain_height_meters_clamped(terrain, cell_x, cell_y + 1);
	float height_11 = terrain_height_meters_clamped(terrain, cell_x + 1, cell_y + 1);
	float top = lerp_float(height_00, height_10, fraction_x);
	float bottom = lerp_float(height_01, height_11, fraction_x);

	return lerp_float(top, bottom, fraction_y);
}

bool terrain_world_to_cell(const terrain_t *terrain, float world_x, float world_z, int32_t *cell_x_out, int32_t *cell_y_out)
{
	int32_t cell_x = floor_to_int(world_x / terrain->cell_size_meters + 0.5f);
	int32_t cell_y = floor_to_int(world_z / terrain->cell_size_meters + 0.5f);

	if (cell_x < 0 || cell_y < 0 || cell_x >= terrain->size || cell_y >= terrain->size)
	{
		return false;
	}
	*cell_x_out = cell_x;
	*cell_y_out = cell_y;
	return true;
}

static void recompute_chunk_bounds(terrain_t *terrain, int32_t chunk_x, int32_t chunk_y)
{
	terrain_chunk_t *chunk = &terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x];
	int32_t start_x = chunk_x * TERRAIN_CHUNK_SIZE;
	int32_t start_y = chunk_y * TERRAIN_CHUNK_SIZE;
	int32_t end_x = SDL_min(start_x + TERRAIN_CHUNK_SIZE, terrain->size - 1);
	int32_t end_y = SDL_min(start_y + TERRAIN_CHUNK_SIZE, terrain->size - 1);
	int32_t minimum_units = INT32_MAX;
	int32_t maximum_units = INT32_MIN;

	for (int32_t y = start_y; y <= end_y; y++)
	{
		const int32_t *row = &terrain->heights[terrain_index(terrain, 0, y)];

		for (int32_t x = start_x; x <= end_x; x++)
		{
			minimum_units = SDL_min(minimum_units, row[x]);
			maximum_units = SDL_max(maximum_units, row[x]);
		}
	}
	chunk->minimum_height_meters = (float)minimum_units * TERRAIN_METERS_PER_HEIGHT_UNIT;
	chunk->maximum_height_meters = (float)maximum_units * TERRAIN_METERS_PER_HEIGHT_UNIT;
#pragma omp atomic
	terrain->chunk_bounds_serial++;
	chunk->needs_upload = true;
	chunk->needs_shape_upload = true;
}

void terrain_fix_layers(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y)
{
	minimum_x = clamp_int(minimum_x, 0, terrain->size - 1);
	minimum_y = clamp_int(minimum_y, 0, terrain->size - 1);
	maximum_x = clamp_int(maximum_x, 0, terrain->size - 1);
	maximum_y = clamp_int(maximum_y, 0, terrain->size - 1);
	if (terrain->baked)
	{
		return;
	}
	for (int32_t y = minimum_y; y <= maximum_y; y++)
	{
		for (int32_t x = minimum_x; x <= maximum_x; x++)
		{
			int32_t index = terrain_index(terrain, x, y);

			if (terrain->bedrock_heights[index] > terrain->heights[index])
			{
				terrain->bedrock_heights[index] = terrain->heights[index];
			}
			terrain->silt_thickness[index] = clamp_int(terrain->silt_thickness[index], 0,
				terrain->heights[index] - terrain->bedrock_heights[index]);
		}
	}
}

// Calls the visitor for every chunk whose mesh touches an inclusive
// rectangle of cells. A chunk's mesh also covers the first sample of the
// next chunk, so a changed cell on a chunk's left/top border affects the
// previous chunk as well; widening the rectangle by one cell catches that.
static void chunk_range(const terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y,
	int32_t range_out[4])
{
	range_out[0] = clamp_int((minimum_x - 1) / TERRAIN_CHUNK_SIZE, 0, terrain->chunks_per_side - 1);
	range_out[1] = clamp_int((minimum_y - 1) / TERRAIN_CHUNK_SIZE, 0, terrain->chunks_per_side - 1);
	range_out[2] = clamp_int(maximum_x / TERRAIN_CHUNK_SIZE, 0, terrain->chunks_per_side - 1);
	range_out[3] = clamp_int(maximum_y / TERRAIN_CHUNK_SIZE, 0, terrain->chunks_per_side - 1);
	if (minimum_x <= 0)
	{
		range_out[0] = 0;
	}
	if (minimum_y <= 0)
	{
		range_out[1] = 0;
	}
}

void terrain_mark_cells_changed(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y)
{
	int32_t range[4];

	chunk_range(terrain, minimum_x, minimum_y, maximum_x, maximum_y, range);
	for (int32_t chunk_y = range[1]; chunk_y <= range[3]; chunk_y++)
	{
		for (int32_t chunk_x = range[0]; chunk_x <= range[2]; chunk_x++)
		{
			recompute_chunk_bounds(terrain, chunk_x, chunk_y);
			terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x].physics_dirty = true;
			terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x].sim_dirty = true;
			terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x].objects_dirty = true;
			terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x].objects_edited = true;
		}
	}
	if (!terrain->any_chunk_physics_dirty)
	{
		terrain->physics_dirty_rectangle[0] = minimum_x;
		terrain->physics_dirty_rectangle[1] = minimum_y;
		terrain->physics_dirty_rectangle[2] = maximum_x;
		terrain->physics_dirty_rectangle[3] = maximum_y;
	}
	else
	{
		terrain->physics_dirty_rectangle[0] = SDL_min(terrain->physics_dirty_rectangle[0], minimum_x);
		terrain->physics_dirty_rectangle[1] = SDL_min(terrain->physics_dirty_rectangle[1], minimum_y);
		terrain->physics_dirty_rectangle[2] = SDL_max(terrain->physics_dirty_rectangle[2], maximum_x);
		terrain->physics_dirty_rectangle[3] = SDL_max(terrain->physics_dirty_rectangle[3], maximum_y);
	}
	terrain->any_chunk_needs_upload = true;
	terrain->any_chunk_physics_dirty = true;
	terrain->any_chunk_sim_dirty = true;
	terrain->any_chunk_objects_dirty = true;
	terrain->shadow_dirty = true;
	terrain_mark_materials_dirty(terrain, minimum_x, minimum_y, maximum_x, maximum_y);
}

void terrain_mark_materials_dirty(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y)
{
	int32_t *rectangle = terrain->materials_dirty_rectangle;

	if (!terrain->materials_dirty)
	{
		rectangle[0] = minimum_x;
		rectangle[1] = minimum_y;
		rectangle[2] = maximum_x;
		rectangle[3] = maximum_y;
	}
	else
	{
		rectangle[0] = SDL_min(rectangle[0], minimum_x);
		rectangle[1] = SDL_min(rectangle[1], minimum_y);
		rectangle[2] = SDL_max(rectangle[2], maximum_x);
		rectangle[3] = SDL_max(rectangle[3], maximum_y);
	}
	terrain->materials_dirty = true;
}

void terrain_mark_materials_changed(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y)
{
	int32_t first_x = SDL_clamp(minimum_x, 0, terrain->size - 1) / TERRAIN_CHUNK_SIZE;
	int32_t first_y = SDL_clamp(minimum_y, 0, terrain->size - 1) / TERRAIN_CHUNK_SIZE;
	int32_t last_x = SDL_clamp(maximum_x, 0, terrain->size - 1) / TERRAIN_CHUNK_SIZE;
	int32_t last_y = SDL_clamp(maximum_y, 0, terrain->size - 1) / TERRAIN_CHUNK_SIZE;

	for (int32_t chunk_y = first_y; chunk_y <= last_y; chunk_y++)
	{
		for (int32_t chunk_x = first_x; chunk_x <= last_x; chunk_x++)
		{
			terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x].materials_changed = true;
		}
	}
	terrain->any_chunk_materials_changed = true;
}

void terrain_mark_cells_eroded(terrain_t *terrain, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y)
{
	int32_t range[4];

	chunk_range(terrain, minimum_x, minimum_y, maximum_x, maximum_y, range);
	for (int32_t chunk_y = range[1]; chunk_y <= range[3]; chunk_y++)
	{
		for (int32_t chunk_x = range[0]; chunk_x <= range[2]; chunk_x++)
		{
			terrain_chunk_t *chunk = &terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x];

			chunk->bounds_dirty = true;
			chunk->needs_upload = true;
			chunk->needs_shape_upload = true;
			chunk->physics_eroded = true;
		}
	}
	terrain->any_chunk_needs_upload = true;
	terrain->any_chunk_bounds_dirty = true;
}

void terrain_update_chunk_bounds(terrain_t *terrain)
{
	int32_t chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;

	if (!terrain->any_chunk_bounds_dirty)
	{
		return;
	}
	int32_t dirty_count = 0;

	for (int32_t chunk = 0; chunk < chunk_count; chunk++)
	{
		dirty_count += terrain->chunks[chunk].bounds_dirty ? 1 : 0;
	}
	// Each chunk writes only its own range. A few (the water wearing the
	// ground here and there, every frame) on this thread alone: one takes
	// microseconds, waking the other threads often took milliseconds (the
	// scheduler's tick); a whole new map on all of them
#pragma omp parallel for schedule(dynamic, 4) num_threads(parallel_threads_for_items(dirty_count, TERRAIN_BOUNDS_CHUNKS_PER_THREAD))
	for (int32_t chunk = 0; chunk < chunk_count; chunk++)
	{
		if (terrain->chunks[chunk].bounds_dirty)
		{
			recompute_chunk_bounds(terrain, chunk % terrain->chunks_per_side, chunk / terrain->chunks_per_side);
			terrain->chunks[chunk].bounds_dirty = false;
		}
	}
	terrain->any_chunk_bounds_dirty = false;
}

void terrain_mark_all_changed(terrain_t *terrain)
{
	terrain_mark_cells_changed(terrain, 0, 0, terrain->size - 1, terrain->size - 1);
}

bool terrain_raycast(const terrain_t *terrain, vec3_t origin, vec3_t direction, float max_distance, vec3_t *hit_out)
{
	// March in steps of half a cell until the ray goes below the surface,
	// then refine the crossing with a binary search. Heights are smooth
	// enough at cell scale that this never misses anything we care about.
	float step = terrain->cell_size_meters * 0.5f;
	float previous_distance = 0.0f;
	float world_extent = (float)terrain->size * terrain->cell_size_meters;

	direction = vec3_normalize(direction);
	for (float distance = 0.0f; distance <= max_distance; distance += step)
	{
		vec3_t point = vec3_add(origin, vec3_scale(direction, distance));

		if (point.x < 0.0f || point.z < 0.0f || point.x > world_extent || point.z > world_extent)
		{
			previous_distance = distance;
			continue;
		}
		if (point.y <= terrain_sample_height_meters(terrain, point.x, point.z))
		{
			float low = previous_distance;
			float high = distance;

			for (int i = 0; i < 12; i++)
			{
				float middle = 0.5f * (low + high);
				vec3_t probe = vec3_add(origin, vec3_scale(direction, middle));

				if (probe.y <= terrain_sample_height_meters(terrain, probe.x, probe.z))
				{
					high = middle;
				}
				else
				{
					low = middle;
				}
			}
			*hit_out = vec3_add(origin, vec3_scale(direction, high));
			return true;
		}
		previous_distance = distance;
	}
	return false;
}

size_t terrain_memory_bytes(const terrain_t *terrain)
{
	size_t cells = (size_t)terrain->size * (size_t)terrain->size;
	size_t bytes = cells * (sizeof(int32_t) + 1 + TERRAIN_MATERIAL_BYTES);

	if (terrain->bedrock_heights != NULL)
	{
		bytes += cells * sizeof(int32_t);
	}
	if (terrain->silt_thickness != NULL)
	{
		bytes += cells * sizeof(int32_t);
	}
	if (terrain->baked_surface != NULL)
	{
		for (int32_t i = 0; i < terrain->chunks_per_side * terrain->chunks_per_side; i++)
		{
			bytes += terrain->baked_surface[i] != NULL ? TERRAIN_BAKED_CHUNK_CELLS * TERRAIN_BAKED_SURFACE_BYTES : 0;
		}
	}
	return bytes;
}
