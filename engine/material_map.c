// The material map. See material_map.h.
//
// The bake follows terrain_materials.glsl's terrain_material_weights and
// terrain_material_patches (the part that does not change by the frame:
// the soils, the slope, the noon sun, the noises), and keeps the material
// with the strongest claim. Its noise is the shaders' (value_noise: the
// lattice, noise_lattice.h), so the patches land where they did.

#include "material_map.h"
#include "noise_lattice.h"

#include <SDL3/SDL.h>

#include "math3d.h"

_Static_assert(MATERIAL_NONE == 255 && MATERIAL_MAP_COVER == 1 && MATERIAL_MAP_OVERLAY == 2, "terrain_create sets them so");

// Materials a base can be (the rest come from the surface, the weather or
// a cover)
static const int BASE_MATERIALS[] = { MATERIAL_GRASS, MATERIAL_DIRT, MATERIAL_ROCK, MATERIAL_SAND, MATERIAL_CLAY };
#define BASE_MATERIAL_COUNT 5

void material_rules_make(material_rules_t *rules, const render_settings_t *settings, const material_set_t *materials, float cell_size_meters)
{
	float azimuth = degrees_to_radians(settings->sun_azimuth_degrees);

	SDL_zerop(rules);
	rules->slope_ranges[0] = settings->grass_slope_start;
	rules->slope_ranges[1] = SDL_max(settings->grass_slope_end, settings->grass_slope_start + 0.01f);
	rules->slope_ranges[2] = settings->cliff_slope_start;
	rules->slope_ranges[3] = SDL_max(settings->cliff_slope_end, settings->cliff_slope_start + 0.01f);
	// (terrain_renderer_sun_direction's, horizontal: the settings' azimuth
	// is the sun's at noon, daylight.c turns it from there)
	rules->sun_x = SDL_cosf(azimuth);
	rules->sun_z = SDL_sinf(azimuth);
	rules->soil_blend_cells = settings->soil_blend_meters / SDL_max(cell_size_meters, 1e-3f);
	rules->normal_reach = SDL_clamp((int)SDL_lroundf(settings->material_normal_meters / SDL_max(cell_size_meters, 1e-3f)), 1, 16);
	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		rules->patch_repeats_per_meter[i] = materials->patch_repeats_per_meter[i];
		rules->patch_strength[i] = materials->patch_strength[i];
	}
	rules->variation = materials->variation_pixels;
	rules->variation_size = materials->variation_size;
	rules->variation_repeats_per_meter = 1.0f / SDL_max(settings->variation_tiling_meters, 1.0f);
}

void material_map_clear(terrain_t *terrain)
{
	size_t cell_count = (size_t)terrain->size * (size_t)terrain->size;

	for (size_t i = 0; i < cell_count; i++)
	{
		uint8_t *cell = &terrain->materials[i * TERRAIN_MATERIAL_BYTES];

		cell[MATERIAL_MAP_BASE] = MATERIAL_DIRT;
		cell[MATERIAL_MAP_COVER] = MATERIAL_NONE;
		cell[MATERIAL_MAP_OVERLAY] = MATERIAL_NONE;
		cell[MATERIAL_MAP_GROUND] = 0;
	}
	terrain_mark_materials_changed(terrain, 0, 0, terrain->size - 1, terrain->size - 1);
	terrain->ground_footprints_stale = true;
}

// --- The shaders' noise ------------------------------------------------------

static inline float fraction_of(float value)
{
	return value - SDL_floorf(value);
}

// The shaders' value_noise (the lattice in the variation texture's green)
static inline float value_noise(float x, float y)
{
	return noise_lattice_value(x, y);
}

// The variation texture, filtered, repeating (u, v in repeats)
static float variation_at(const material_rules_t *rules, float u, float v)
{
	int size = rules->variation_size;
	float x;
	float y;
	int x0;
	int y0;
	float fx;
	float fy;
	const uint8_t *pixels = rules->variation;

	if (pixels == NULL || size <= 0)
	{
		return 0.5f;
	}
	x = u * (float)size - 0.5f;
	y = v * (float)size - 0.5f;
	x0 = (int)SDL_floorf(x);
	y0 = (int)SDL_floorf(y);
	fx = x - (float)x0;
	fy = y - (float)y0;
	{
		int ax = ((x0 % size) + size) % size;
		int ay = ((y0 % size) + size) % size;
		int bx = (ax + 1) % size;
		int by = (ay + 1) % size;
		float low = lerp_float((float)pixels[ay * size + ax], (float)pixels[ay * size + bx], fx);
		float high = lerp_float((float)pixels[by * size + ax], (float)pixels[by * size + bx], fx);

		return lerp_float(low, high, fy) * (1.0f / 255.0f);
	}
}

// --- The bake ----------------------------------------------------------------

// Per soil, how many cells of it lie in every box of the region: a summed
// area table (one row and column of zeros first)
typedef struct
{
	int32_t first_x;
	int32_t first_y;
	int32_t width;
	int32_t height;
	uint32_t *sums;                // (width + 1) * (height + 1) * SOIL_TYPE_COUNT
} soil_sums_t;

static bool soil_sums_make(soil_sums_t *table, const terrain_t *terrain, int32_t first_x, int32_t first_y, int32_t last_x, int32_t last_y)
{
	int32_t stride;

	table->first_x = first_x;
	table->first_y = first_y;
	table->width = last_x - first_x + 1;
	table->height = last_y - first_y + 1;
	stride = table->width + 1;
	table->sums = SDL_calloc((size_t)stride * (size_t)(table->height + 1) * SOIL_TYPE_COUNT, sizeof(uint32_t));
	if (table->sums == NULL)
	{
		return false;
	}
	// Rows first (each on its own), then down the columns
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < table->height; y++)
	{
		const uint8_t *soils = &terrain->soil_types[(size_t)(first_y + y) * (size_t)terrain->size + (size_t)first_x];
		uint32_t *row = &table->sums[((size_t)(y + 1) * (size_t)stride) * SOIL_TYPE_COUNT];
		uint32_t running[SOIL_TYPE_COUNT] = { 0 };

		for (int32_t x = 0; x < table->width; x++)
		{
			running[SDL_min(soils[x], SOIL_TYPE_COUNT - 1)]++;
			for (int s = 0; s < SOIL_TYPE_COUNT; s++)
			{
				row[(size_t)(x + 1) * SOIL_TYPE_COUNT + (size_t)s] = running[s];
			}
		}
	}
	for (int32_t y = 1; y <= table->height; y++)
	{
		uint32_t *row = &table->sums[(size_t)y * (size_t)stride * SOIL_TYPE_COUNT];
		const uint32_t *above = row - (size_t)stride * SOIL_TYPE_COUNT;

		for (int32_t i = 0; i < stride * SOIL_TYPE_COUNT; i++)
		{
			row[i] += above[i];
		}
	}
	return true;
}

// The soils' shares in the box of radius cells round a cell (clamped to
// the table)
static void soil_shares(const soil_sums_t *table, int32_t x, int32_t y, int32_t radius, float shares_out[SOIL_TYPE_COUNT])
{
	int32_t stride = table->width + 1;
	int32_t x0 = SDL_clamp(x - radius - table->first_x, 0, table->width);
	int32_t y0 = SDL_clamp(y - radius - table->first_y, 0, table->height);
	int32_t x1 = SDL_clamp(x + radius + 1 - table->first_x, 0, table->width);
	int32_t y1 = SDL_clamp(y + radius + 1 - table->first_y, 0, table->height);
	const uint32_t *a = &table->sums[((size_t)y0 * (size_t)stride + (size_t)x0) * SOIL_TYPE_COUNT];
	const uint32_t *b = &table->sums[((size_t)y0 * (size_t)stride + (size_t)x1) * SOIL_TYPE_COUNT];
	const uint32_t *c = &table->sums[((size_t)y1 * (size_t)stride + (size_t)x0) * SOIL_TYPE_COUNT];
	const uint32_t *d = &table->sums[((size_t)y1 * (size_t)stride + (size_t)x1) * SOIL_TYPE_COUNT];
	float total = 0.0f;

	for (int s = 0; s < SOIL_TYPE_COUNT; s++)
	{
		shares_out[s] = (float)(d[s] + a[s] - b[s] - c[s]);
		total += shares_out[s];
	}
	for (int s = 0; s < SOIL_TYPE_COUNT; s++)
	{
		shares_out[s] = total > 0.0f ? shares_out[s] / total : (s == SOIL_EARTH ? 1.0f : 0.0f);
	}
}

// How far round a cell the bake looks, in cells: the soils' blur and warp,
// the slope's reach
static int32_t bake_reach(const material_rules_t *rules)
{
	float width = SDL_max(rules->soil_blend_cells, 1.0f);

	return (int32_t)SDL_ceilf(width * 0.5f + width * 1.2f * 0.75f) + rules->normal_reach + 2;
}

// One cell's base material
static uint8_t bake_cell(const terrain_t *terrain, const material_rules_t *rules, const soil_sums_t *table, int32_t x, int32_t y)
{
	float cell_size = terrain->cell_size_meters;
	float world_x = (float)x * cell_size;
	float world_z = (float)y * cell_size;
	float shares[SOIL_TYPE_COUNT];
	float weights[MATERIAL_COUNT] = { 0 };
	float slope;
	float sun_facing;
	float detail_noise = value_noise(world_x * 0.11f, world_z * 0.11f);
	float broad_noise = value_noise(world_x * 0.013f, world_z * 0.013f);
	float variation_large = variation_at(rules, world_x * rules->variation_repeats_per_meter, world_z * rules->variation_repeats_per_meter);
	float variation_small = variation_at(rules, world_x * rules->variation_repeats_per_meter * 3.83f + 0.61f,
		world_z * rules->variation_repeats_per_meter * 3.83f + 0.61f);

	// The soils: blurred (soil_blend_cells wide) at a position the noise
	// moves about (terrain_materials.glsl's soil_warped_position)
	{
		float width = SDL_max(rules->soil_blend_cells, 1.0f);
		float px = (float)x / (width * 2.5f);
		float py = (float)y / (width * 2.5f);
		float warp_x = value_noise(px, py) - 0.5f + 0.5f * (value_noise(px * 2.7f + 5.1f, py * 2.7f + 5.1f) - 0.5f);
		float warp_y = value_noise(px + 19.7f, py + 19.7f) - 0.5f + 0.5f * (value_noise(px * 2.7f + 33.3f, py * 2.7f + 33.3f) - 0.5f);
		int32_t at_x = (int32_t)SDL_lroundf((float)x + warp_x * width * 1.2f);
		int32_t at_y = (int32_t)SDL_lroundf((float)y + warp_y * width * 1.2f);

		soil_shares(table, at_x, at_y, SDL_max((int32_t)SDL_lroundf(width * 0.5f), 1), shares);
	}

	// The slope and how it faces the sun: the wide "material" normal, as
	// the renderer's (fill_normals)
	{
		int32_t reach = rules->normal_reach;
		int32_t last = terrain->size - 1;
		float dx = terrain_height_meters_clamped(terrain, SDL_max(x - reach, 0), y) - terrain_height_meters_clamped(terrain, SDL_min(x + reach, last), y);
		float dz = terrain_height_meters_clamped(terrain, x, SDL_max(y - reach, 0)) - terrain_height_meters_clamped(terrain, x, SDL_min(y + reach, last));
		float dy = 2.0f * (float)reach * cell_size;
		float scale = 1.0f / SDL_sqrtf(dx * dx + dy * dy + dz * dz);
		// (the texture's 8 bits: as the shaders see it)
		float nx = SDL_roundf(dx * scale * 127.5f) / 127.5f;
		float nz = SDL_roundf(dz * scale * 127.5f) / 127.5f;
		float ny = SDL_sqrtf(SDL_max(1.0f - nx * nx - nz * nz, 0.0f));

		slope = 1.0f - ny;
		sun_facing = nx * rules->sun_x + nz * rules->sun_z;
	}

	// terrain_material_weights, without what changes by the frame
	{
		float grass_limit = slope + sun_facing * 0.15f + (detail_noise - 0.5f) * 0.15f + (variation_large - 0.5f) * 0.12f;
		float grass = 1.0f - smoothstep_float(rules->slope_ranges[0], rules->slope_ranges[1], grass_limit);

		weights[MATERIAL_GRASS] += shares[SOIL_EARTH] * grass;
		weights[MATERIAL_DIRT] += shares[SOIL_EARTH] * (1.0f - grass);
	}
	{
		float grass = (1.0f - smoothstep_float(0.08f, 0.25f, slope)) * smoothstep_float(0.45f, 0.75f, broad_noise) * 0.8f;

		weights[MATERIAL_GRASS] += shares[SOIL_CLAY] * grass;
		weights[MATERIAL_CLAY] += shares[SOIL_CLAY] * (1.0f - grass);
	}
	weights[MATERIAL_SAND] += shares[SOIL_SAND];
	weights[MATERIAL_ROCK] += shares[SOIL_ROCK];
	{
		float cliff = smoothstep_float(rules->slope_ranges[2], rules->slope_ranges[3],
			slope + (detail_noise - 0.5f) * 0.1f + (variation_small - 0.5f) * 0.1f);

		for (int i = 0; i < MATERIAL_COUNT; i++)
		{
			weights[i] *= 1.0f - cliff;
		}
		weights[MATERIAL_ROCK] += cliff;
	}
	for (int k = 0; k < BASE_MATERIAL_COUNT; k++)
	{
		int i = BASE_MATERIALS[k];

		if (weights[i] > 0.001f)
		{
			weights[i] *= 0.75f + 0.5f * value_noise(world_x * 0.35f + (float)i * 7.3f, world_z * 0.35f + (float)i * 7.3f);
		}
	}

	// terrain_material_patches: the strongest claim
	{
		float total = 0.0f;
		float best_score = -1e9f;
		int best = MATERIAL_DIRT;

		for (int k = 0; k < BASE_MATERIAL_COUNT; k++)
		{
			total += weights[BASE_MATERIALS[k]];
		}
		total = SDL_max(total, 1e-4f);
		for (int k = 0; k < BASE_MATERIAL_COUNT; k++)
		{
			int i = BASE_MATERIALS[k];
			float at_x;
			float at_z;
			float score;

			if (weights[i] <= 0.001f)
			{
				continue;
			}
			at_x = world_x * rules->patch_repeats_per_meter[i] + (float)i * 17.31f;
			at_z = world_z * rules->patch_repeats_per_meter[i] + (float)i * 17.31f;
			score = weights[i] / total + rules->patch_strength[i]
				* (0.7f * value_noise(at_x, at_z) + 0.3f * value_noise(at_x * 8.0f + 41.7f, at_z * 8.0f + 41.7f) - 0.5f);
			if (score > best_score)
			{
				best_score = score;
				best = i;
			}
		}
		return (uint8_t)best;
	}
}

void material_map_bake(terrain_t *terrain, const material_rules_t *rules, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x,
	int32_t maximum_y)
{
	int32_t reach = bake_reach(rules);
	soil_sums_t table;

	minimum_x = SDL_clamp(minimum_x, 0, terrain->size - 1);
	minimum_y = SDL_clamp(minimum_y, 0, terrain->size - 1);
	maximum_x = SDL_clamp(maximum_x, 0, terrain->size - 1);
	maximum_y = SDL_clamp(maximum_y, 0, terrain->size - 1);
	if (maximum_x < minimum_x || maximum_y < minimum_y)
	{
		return;
	}
	if (!soil_sums_make(&table, terrain, SDL_max(minimum_x - reach, 0), SDL_max(minimum_y - reach, 0),
		SDL_min(maximum_x + reach, terrain->size - 1), SDL_min(maximum_y + reach, terrain->size - 1)))
	{
		return;
	}
#pragma omp parallel for schedule(dynamic, 8)
	for (int32_t y = minimum_y; y <= maximum_y; y++)
	{
		for (int32_t x = minimum_x; x <= maximum_x; x++)
		{
			terrain->materials[(size_t)terrain_index(terrain, x, y) * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_BASE]
				= bake_cell(terrain, rules, &table, x, y);
		}
	}
	SDL_free(table.sums);
	terrain_mark_materials_changed(terrain, minimum_x, minimum_y, maximum_x, maximum_y);
}

bool material_map_update(terrain_t *terrain, const material_rules_t *rules)
{
	int32_t reach;
	const int32_t *rectangle = terrain->materials_dirty_rectangle;
	uint64_t start;

	if (!terrain->materials_dirty)
	{
		return false;
	}
	// (a cell's base looks this far round it: what changed there changes
	// the bases this far off)
	reach = bake_reach(rules);
	start = SDL_GetPerformanceCounter();
	material_map_bake(terrain, rules, rectangle[0] - reach, rectangle[1] - reach, rectangle[2] + reach, rectangle[3] + reach);
	terrain->materials_dirty = false;
	{
		int64_t cells = (int64_t)(rectangle[2] - rectangle[0] + 1 + 2 * reach) * (int64_t)(rectangle[3] - rectangle[1] + 1 + 2 * reach);

		if (cells > 512 * 512)
		{
			SDL_Log("Material map: baked in %.1f ms", 1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency());
		}
	}
	return true;
}

// --- Cover, petals ----------------------------------------------------------

// The ragged edge's reach as a share of the cover radius: the noise moves
// it between these (0.6 + 0.55 * [0, 1) + 0.25 * ([0, 1) - 0.5)), a little
// widened: nearer than the least it is always in, farther than the most
// never, the noise not needed
static const float RAGGED_LEAST = 0.47f;
static const float RAGGED_MOST = 1.28f;

// A disk of cover on the rows first_row..last_row only (a band of them: the
// restamp's threads each have their own)
static void stamp_disk_rows(terrain_t *terrain, float x, float z, float cover_radius, uint8_t material, int32_t first_row, int32_t last_row)
{
	float cell_size = terrain->cell_size_meters;
	float reach = cover_radius * 1.2f;
	float sure_squared = (RAGGED_LEAST * cover_radius) * (RAGGED_LEAST * cover_radius);
	float unsure_squared = (RAGGED_MOST * cover_radius) * (RAGGED_MOST * cover_radius);
	float margin = 1e-3f * cover_radius;
	int32_t first_x = SDL_max((int32_t)SDL_floorf((x - reach) / cell_size), 0);
	int32_t first_y = SDL_max(SDL_max((int32_t)SDL_floorf((z - reach) / cell_size), 0), first_row);
	int32_t last_x = SDL_min((int32_t)SDL_ceilf((x + reach) / cell_size), terrain->size - 1);
	int32_t last_y = SDL_min(SDL_min((int32_t)SDL_ceilf((z + reach) / cell_size), terrain->size - 1), last_row);
	int byte = material == MATERIAL_PETALS ? MATERIAL_MAP_OVERLAY : MATERIAL_MAP_COVER;

	for (int32_t cy = first_y; cy <= last_y; cy++)
	{
		float world_z = (float)cy * cell_size;
		uint8_t *row = &terrain->materials[(size_t)terrain_index(terrain, 0, cy) * TERRAIN_MATERIAL_BYTES];

		for (int32_t cx = first_x; cx <= last_x; cx++)
		{
			float world_x = (float)cx * cell_size;
			float distance_squared = (world_x - x) * (world_x - x) + (world_z - z) * (world_z - z);

			bool inside;

			// (the edge ragged: about a meter's noise, and finer. Well inside
			// it no noise takes the cell out, far out none brings it in; the
			// coarse noise alone mostly decides, the fine one only near its
			// edge: the same cells as the whole sum would give)
			if (distance_squared < sure_squared)
			{
				inside = true;
			}
			else if (distance_squared >= unsure_squared)
			{
				inside = false;
			}
			else
			{
				float distance = SDL_sqrtf(distance_squared);
				float coarse = value_noise(world_x * 0.6f + 3.1f, world_z * 0.6f + 7.7f);

				if (distance < cover_radius * (0.475f + 0.55f * coarse) - margin)
				{
					inside = true;
				}
				else if (distance > cover_radius * (0.725f + 0.55f * coarse) + margin)
				{
					inside = false;
				}
				else
				{
					inside = distance < cover_radius * (0.6f + 0.55f * coarse + 0.25f * (value_noise(world_x * 2.3f, world_z * 2.3f) - 0.5f));
				}
			}
			if (inside)
			{
				row[(size_t)cx * TERRAIN_MATERIAL_BYTES + byte] = material;
			}
		}
	}
}

void material_map_stamp_disk(terrain_t *terrain, float x, float z, float cover_radius, uint8_t material)
{
	float cell_size = terrain->cell_size_meters;
	float reach = cover_radius * 1.2f;
	int32_t first_x = SDL_max((int32_t)SDL_floorf((x - reach) / cell_size), 0);
	int32_t first_y = SDL_max((int32_t)SDL_floorf((z - reach) / cell_size), 0);
	int32_t last_x = SDL_min((int32_t)SDL_ceilf((x + reach) / cell_size), terrain->size - 1);
	int32_t last_y = SDL_min((int32_t)SDL_ceilf((z + reach) / cell_size), terrain->size - 1);

	if (material == MATERIAL_NONE || cover_radius <= 0.0f || first_x > last_x || first_y > last_y)
	{
		return;
	}
	stamp_disk_rows(terrain, x, z, cover_radius, material, first_y, last_y);
	terrain_mark_materials_changed(terrain, first_x, first_y, last_x, last_y);
}

// The cover an object leaves (MATERIAL_NONE: none), and how far
static uint8_t object_cover(const objects_t *objects, const object_instance_t *instance, float *cover_radius_out)
{
	const object_type_t *type;
	uint8_t material;
	float model_radius;

	*cover_radius_out = 0.0f;
	if (instance->state & OBJECT_STATE_FREE || instance->type < 0 || instance->type >= objects->types.type_count)
	{
		return MATERIAL_NONE;
	}
	type = &objects->types.types[instance->type];
	material = type->cover_material;
	// (what stands in a layout tramples the ground round it, unless
	// its type says otherwise)
	if (material == MATERIAL_NONE && instance->group != 0 && type->cover_meters <= 0.0f)
	{
		material = MATERIAL_TRAMPLED;
	}
	if (material == MATERIAL_NONE)
	{
		return MATERIAL_NONE;
	}
	model_radius = type->model >= 0 && type->model < objects->model_count ? objects->models[type->model].horizontal_radius : 0.0f;
	if (type->cover_material == MATERIAL_NONE)
	{
		*cover_radius_out = model_radius * instance->scale + 1.5f;
	}
	else
	{
		// (no size given: under its crown)
		*cover_radius_out = (type->cover_meters > 0.0f ? type->cover_meters : 0.9f * model_radius) * instance->scale;
	}
	return material;
}

void material_map_stamp_objects(terrain_t *terrain, const objects_t *objects, float x, float z, float radius)
{
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		float cover_radius;
		uint8_t material = object_cover(objects, instance, &cover_radius);

		if (material == MATERIAL_NONE)
		{
			continue;
		}
		if (radius >= 0.0f)
		{
			float reach = radius + cover_radius * 1.2f;

			if ((instance->x - x) * (instance->x - x) + (instance->z - z) * (instance->z - z) > reach * reach)
			{
				continue;
			}
		}
		material_map_stamp_disk(terrain, instance->x, instance->z, cover_radius, material);
	}
}

// A disk to stamp (the restamp's list)
typedef struct
{
	float x;
	float z;
	float radius;
	float first_row;               // the rows it reaches (meters / cell size), for the bands
	float last_row;
	uint8_t material;
} cover_disk_t;

// Rows per band of the restamp (each band a thread's, the disks in their
// order: the same map as one thread would make)
static const int32_t RESTAMP_BAND_ROWS = 32;

void material_map_restamp(terrain_t *terrain, const objects_t *objects)
{
	size_t cell_count = (size_t)terrain->size * (size_t)terrain->size;
	uint64_t start = SDL_GetPerformanceCounter();
	cover_disk_t *disks = SDL_malloc(sizeof(cover_disk_t) * (size_t)SDL_max(objects->instance_count, 1));
	int disk_count = 0;
	int32_t band_count = (terrain->size + RESTAMP_BAND_ROWS - 1) / RESTAMP_BAND_ROWS;

	for (size_t i = 0; i < cell_count; i++)
	{
		uint8_t *cell = &terrain->materials[i * TERRAIN_MATERIAL_BYTES];

		cell[MATERIAL_MAP_COVER] = MATERIAL_NONE;
		cell[MATERIAL_MAP_OVERLAY] = MATERIAL_NONE;
		cell[MATERIAL_MAP_GROUND] = 0;
	}
	terrain_mark_materials_changed(terrain, 0, 0, terrain->size - 1, terrain->size - 1);
	terrain->ground_footprints_stale = true;
	if (disks == NULL)
	{
		material_map_stamp_objects(terrain, objects, 0.0f, 0.0f, -1.0f);
		return;
	}
	// The disks, in the objects' order
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		float cover_radius;
		uint8_t material = object_cover(objects, instance, &cover_radius);
		cover_disk_t *disk;

		if (material == MATERIAL_NONE || cover_radius <= 0.0f)
		{
			continue;
		}
		disk = &disks[disk_count++];
		disk->x = instance->x;
		disk->z = instance->z;
		disk->radius = cover_radius;
		disk->first_row = SDL_floorf((instance->z - cover_radius * 1.2f) / terrain->cell_size_meters);
		disk->last_row = SDL_ceilf((instance->z + cover_radius * 1.2f) / terrain->cell_size_meters);
		disk->material = material;
	}
	// Band by band, at once
#pragma omp parallel for schedule(dynamic, 1)
	for (int32_t band = 0; band < band_count; band++)
	{
		int32_t first_row = band * RESTAMP_BAND_ROWS;
		int32_t last_row = SDL_min(first_row + RESTAMP_BAND_ROWS - 1, terrain->size - 1);

		for (int d = 0; d < disk_count; d++)
		{
			const cover_disk_t *disk = &disks[d];

			if (disk->last_row < (float)first_row || disk->first_row > (float)last_row)
			{
				continue;
			}
			stamp_disk_rows(terrain, disk->x, disk->z, disk->radius, disk->material, first_row, last_row);
		}
	}
	SDL_free(disks);
	SDL_Log("Material map: the objects' cover in %.1f ms (%d disks)",
		1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency(), disk_count);
}

// Water this deep takes the cover away (it floats off), and silt this
// thick buries it (silt_visible_units: as thick as it shows)
static const float FLOOD_COVER_DEPTH_METERS = 0.05f;
// Chunks looked at per call
static const int FLOOD_CHUNKS_PER_CALL = 4;

bool material_map_clear_flooded_cover(terrain_t *terrain, const water_t *water, float silt_visible_units)
{
	static int next_chunk = 0;
	int chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;
	int32_t deep_units = (int32_t)(FLOOD_COVER_DEPTH_METERS * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	int32_t silt_units = (int32_t)SDL_max(silt_visible_units, 1.0f);
	bool took = false;

	// (a baked map's water does not move: it was done when it was baked)
	if (water == NULL || water->depth == NULL || water->size != terrain->size || terrain->baked)
	{
		return false;
	}
	for (int n = 0; n < FLOOD_CHUNKS_PER_CALL && n < chunk_count; n++)
	{
		int chunk = next_chunk;
		int start_x = (chunk % terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE;
		int start_y = (chunk / terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE;
		bool changed = false;

		next_chunk = (next_chunk + 1) % chunk_count;
		// A tile the water says is dry (its tiles are the terrain's
		// chunks): none of its cells read. Silt left by water that came
		// and went between two rounds stays under its grass
		if (water->tile_state != NULL && water->tile_total_depth != NULL && water->tiles_per_side == terrain->chunks_per_side
			&& water->tile_state[chunk] == WATER_TILE_DRY && water->tile_total_depth[chunk] <= 0)
		{
			continue;
		}
		for (int y = start_y; y < start_y + TERRAIN_CHUNK_SIZE; y++)
		{
			for (int x = start_x; x < start_x + TERRAIN_CHUNK_SIZE; x++)
			{
				int32_t index = terrain_index(terrain, x, y);
				uint8_t *cell = &terrain->materials[(size_t)index * TERRAIN_MATERIAL_BYTES];

				if (cell[MATERIAL_MAP_COVER] == MATERIAL_NONE && cell[MATERIAL_MAP_OVERLAY] == MATERIAL_NONE)
				{
					continue;
				}
				if (water->depth[index] > deep_units || terrain->silt_thickness[index] >= silt_units)
				{
					cell[MATERIAL_MAP_COVER] = MATERIAL_NONE;
					cell[MATERIAL_MAP_OVERLAY] = MATERIAL_NONE;
					changed = true;
				}
			}
		}
		if (changed)
		{
			terrain->chunks[chunk].materials_changed = true;
			terrain->any_chunk_materials_changed = true;
			took = true;
		}
	}
	return took;
}
