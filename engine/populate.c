// Vegetation: populating the map and the planting brush. See populate.h.

#include "populate.h"

#include <SDL3/SDL.h>

// Footprints are kept in a bitmap of cells this big
#define OCCUPANCY_CELL_METERS 0.5f
// Patches and valleys are computed on a coarse grid of this cell size
#define FIELD_CELL_METERS 4.0f
// A valley is ground lower than its surroundings within this radius ...
#define VALLEY_RADIUS_METERS 24.0f
// ... by this much for the full effect
#define VALLEY_DEPTH_METERS 1.5f
#define MAXIMUM_PATCHES 16
// No footprint is bigger than this (the brush searches this far)
#define MAXIMUM_FOOTPRINT_METERS 4.0f
// Patches cover about half the map; this is the width of their soft edge
// in noise units
#define PATCH_EDGE_LOW 0.45f
#define PATCH_EDGE_HIGH 0.55f
// Water deeper than this counts as water for spawn_water_near/far
#define NEAR_WATER_DEPTH_METERS 0.1f
// Distances to water are kept up to this far
#define WATER_DISTANCE_LIMIT_METERS 1000.0f

typedef struct
{
	int size;                      // samples per side
	float *values;
} field_t;

typedef struct
{
	const terrain_t *terrain;
	const water_t *water;
	float map_meters;
	uint8_t *occupied;             // footprints, one byte per occupancy cell
	int occupancy_size;
	field_t valley;                // -1 ridge .. 1 valley bottom
	field_t water_distance;        // meters to the nearest water (made when a type asks)
	bool water_distance_made;
	uint16_t *near_counts[OBJECT_CATEGORIES_MAX]; // objects placed in each field cell, for the categories a spawn_near names
	int near_size;
	char patch_names[MAXIMUM_PATCHES][OBJECT_CATEGORY_NAME_LENGTH];
	field_t patches[MAXIMUM_PATCHES]; // 0..1, inside a patch above one half
	int patch_count;
} populate_context_t;

static float smoothstep_range(float edge_low, float edge_high, float value)
{
	float t = SDL_clamp((value - edge_low) / (edge_high - edge_low), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

// Bilinear sample of a coarse field at a map position
static float field_sample(const field_t *field, float x, float z)
{
	float fx = SDL_clamp(x / FIELD_CELL_METERS - 0.5f, 0.0f, (float)(field->size - 1));
	float fz = SDL_clamp(z / FIELD_CELL_METERS - 0.5f, 0.0f, (float)(field->size - 1));
	int x0 = SDL_min((int)fx, field->size - 1);
	int z0 = SDL_min((int)fz, field->size - 1);
	int x1 = SDL_min(x0 + 1, field->size - 1);
	int z1 = SDL_min(z0 + 1, field->size - 1);
	float tx = fx - (float)x0;
	float tz = fz - (float)z0;
	float top = field->values[z0 * field->size + x0] * (1.0f - tx) + field->values[z0 * field->size + x1] * tx;
	float bottom = field->values[z1 * field->size + x0] * (1.0f - tx) + field->values[z1 * field->size + x1] * tx;

	return top * (1.0f - tz) + bottom * tz;
}

static float water_depth_at(const water_t *water, float x, float z)
{
	int32_t cell_x;
	int32_t cell_y;

	if (water == NULL || water->depth == NULL)
	{
		return 0.0f;
	}
	cell_x = clamp_int((int32_t)(x / water->cell_size_meters + 0.5f), 0, water->size - 1);
	cell_y = clamp_int((int32_t)(z / water->cell_size_meters + 0.5f), 0, water->size - 1);
	return (float)water->depth[cell_y * water->size + cell_x] * TERRAIN_METERS_PER_HEIGHT_UNIT;
}

// Steepness of the ground over about a meter, in degrees
static float slope_degrees(const terrain_t *terrain, float x, float z)
{
	const float half_span = 0.5f;
	float dx = (terrain_sample_height_meters(terrain, x + half_span, z) - terrain_sample_height_meters(terrain, x - half_span, z)) / (2.0f * half_span);
	float dz = (terrain_sample_height_meters(terrain, x, z + half_span) - terrain_sample_height_meters(terrain, x, z - half_span)) / (2.0f * half_span);

	return SDL_atanf(SDL_sqrtf(dx * dx + dz * dz)) * (180.0f / MATH3D_PI);
}

// The rules of the ground at a spot: water, slope, and (when populating,
// not for the brush) soil and height
static bool ground_allows(const object_type_t *type, const terrain_t *terrain, const water_t *water, float x, float z, bool brush)
{
	float water_depth = water_depth_at(water, x, z);

	if (water_depth < type->spawn_water_minimum_meters || water_depth > type->spawn_water_maximum_meters)
	{
		return false;
	}
	if (slope_degrees(terrain, x, z) > type->spawn_slope_maximum_degrees)
	{
		return false;
	}
	if (!brush)
	{
		int32_t cell_x;
		int32_t cell_y;
		float height_fraction;

		if (!terrain_world_to_cell(terrain, x, z, &cell_x, &cell_y))
		{
			return false;
		}
		if ((type->spawn_soils & (1u << terrain->soil_types[terrain_index(terrain, cell_x, cell_y)])) == 0)
		{
			return false;
		}
		height_fraction = terrain_sample_height_meters(terrain, x, z) / SDL_max(terrain->max_height_meters, 1.0f);
		if (height_fraction < type->spawn_height_minimum || height_fraction > type->spawn_height_maximum)
		{
			return false;
		}
	}
	return true;
}

// --- Footprints when populating -------------------------------------------

static bool occupancy_free(const populate_context_t *context, float x, float z, float radius)
{
	int size = context->occupancy_size;
	float reach = radius / OCCUPANCY_CELL_METERS;
	float center_x = x / OCCUPANCY_CELL_METERS;
	float center_z = z / OCCUPANCY_CELL_METERS;
	int first_x = SDL_max((int)(center_x - reach), 0);
	int first_z = SDL_max((int)(center_z - reach), 0);
	int last_x = SDL_min((int)(center_x + reach), size - 1);
	int last_z = SDL_min((int)(center_z + reach), size - 1);

	for (int j = first_z; j <= last_z; j++)
	{
		for (int i = first_x; i <= last_x; i++)
		{
			float offset_x = (float)i + 0.5f - center_x;
			float offset_z = (float)j + 0.5f - center_z;

			// The cell under the spot always counts; others if their
			// center is inside the circle
			if (context->occupied[j * size + i] && (offset_x * offset_x + offset_z * offset_z <= reach * reach
				|| (i == (int)center_x && j == (int)center_z)))
			{
				return false;
			}
		}
	}
	return true;
}

static void occupancy_mark(populate_context_t *context, float x, float z, float radius)
{
	int size = context->occupancy_size;
	float reach = radius / OCCUPANCY_CELL_METERS;
	float center_x = x / OCCUPANCY_CELL_METERS;
	float center_z = z / OCCUPANCY_CELL_METERS;
	int first_x = SDL_max((int)(center_x - reach), 0);
	int first_z = SDL_max((int)(center_z - reach), 0);
	int last_x = SDL_min((int)(center_x + reach), size - 1);
	int last_z = SDL_min((int)(center_z + reach), size - 1);

	for (int j = first_z; j <= last_z; j++)
	{
		for (int i = first_x; i <= last_x; i++)
		{
			float offset_x = (float)i + 0.5f - center_x;
			float offset_z = (float)j + 0.5f - center_z;

			if (offset_x * offset_x + offset_z * offset_z <= reach * reach || (i == (int)center_x && j == (int)center_z))
			{
				context->occupied[j * size + i] = 1;
			}
		}
	}
}

// --- Coarse fields ----------------------------------------------------------

static bool field_create(field_t *field, float map_meters)
{
	field->size = SDL_max(2, (int)SDL_ceilf(map_meters / FIELD_CELL_METERS));
	field->values = SDL_calloc((size_t)field->size * (size_t)field->size, sizeof(float));
	return field->values != NULL;
}

// Box blur of a square field along rows then columns (in place)
static void box_blur(float *values, float *scratch, int size, int radius)
{
	for (int pass = 0; pass < 2; pass++)
	{
		for (int line = 0; line < size; line++)
		{
			for (int k = 0; k < size; k++)
			{
				float sum = 0.0f;
				int count = 0;

				for (int offset = -radius; offset <= radius; offset++)
				{
					int position = SDL_clamp(k + offset, 0, size - 1);

					sum += pass == 0 ? values[line * size + position] : values[position * size + line];
					count++;
				}
				if (pass == 0)
				{
					scratch[line * size + k] = sum / (float)count;
				}
				else
				{
					scratch[k * size + line] = sum / (float)count;
				}
			}
		}
		SDL_memcpy(values, scratch, sizeof(float) * (size_t)size * (size_t)size);
	}
}

// Valleys: how much lower the ground is than its surroundings
static bool compute_valleys(populate_context_t *context)
{
	field_t *valley = &context->valley;
	int size;
	float *heights;
	float *scratch;
	int radius = SDL_max(1, (int)(VALLEY_RADIUS_METERS / FIELD_CELL_METERS));

	if (!field_create(valley, context->map_meters))
	{
		return false;
	}
	size = valley->size;
	heights = SDL_malloc(sizeof(float) * (size_t)size * (size_t)size);
	scratch = SDL_malloc(sizeof(float) * (size_t)size * (size_t)size);
	if (heights == NULL || scratch == NULL)
	{
		SDL_free(heights);
		SDL_free(scratch);
		return false;
	}
	for (int j = 0; j < size; j++)
	{
		for (int i = 0; i < size; i++)
		{
			heights[j * size + i] = terrain_sample_height_meters(context->terrain, ((float)i + 0.5f) * FIELD_CELL_METERS,
				((float)j + 0.5f) * FIELD_CELL_METERS);
		}
	}
	SDL_memcpy(valley->values, heights, sizeof(float) * (size_t)size * (size_t)size);
	// Two box blurs are close to a gaussian
	box_blur(valley->values, scratch, size, radius);
	box_blur(valley->values, scratch, size, radius);
	for (int k = 0; k < size * size; k++)
	{
		valley->values[k] = SDL_clamp((valley->values[k] - heights[k]) / VALLEY_DEPTH_METERS, -1.0f, 1.0f);
	}
	SDL_free(heights);
	SDL_free(scratch);
	return true;
}

static uint64_t hash_name(const char *name)
{
	uint64_t hash = 1469598103934665603ull;

	for (const char *character = name; *character != '\0'; character++)
	{
		hash = (hash ^ (uint64_t)(unsigned char)SDL_tolower((unsigned char)*character)) * 1099511628211ull;
	}
	return hash;
}

// The patch field with that name, computed on first use with the size the
// first type naming it asks for; NULL if there are too many
static const field_t *patch_field(populate_context_t *context, const object_type_t *type)
{
	noise_context_t noise;
	field_t *field;
	float frequency;

	for (int p = 0; p < context->patch_count; p++)
	{
		if (SDL_strcasecmp(context->patch_names[p], type->spawn_patch) == 0)
		{
			return &context->patches[p];
		}
	}
	if (context->patch_count >= MAXIMUM_PATCHES)
	{
		return NULL;
	}
	field = &context->patches[context->patch_count];
	if (!field_create(field, context->map_meters))
	{
		return NULL;
	}
	SDL_strlcpy(context->patch_names[context->patch_count], type->spawn_patch, OBJECT_CATEGORY_NAME_LENGTH);
	context->patch_count++;
	noise_initialize(&noise, (uint64_t)context->terrain->seed * 0x9E3779B97F4A7C15ull ^ hash_name(type->spawn_patch));
	frequency = 1.0f / type->spawn_patch_meters;
	for (int j = 0; j < field->size; j++)
	{
		for (int i = 0; i < field->size; i++)
		{
			float x = ((float)i + 0.5f) * FIELD_CELL_METERS * frequency;
			float z = ((float)j + 0.5f) * FIELD_CELL_METERS * frequency;

			field->values[j * field->size + i] = SDL_clamp(0.5f + 0.7f * noise_fbm(&noise, x, z, 3, 0.5f), 0.0f, 1.0f);
		}
	}
	return field;
}

// Distance to water: the field cells holding any water deeper than a hand
// at 0, the others by a two pass chamfer distance (in meters)
static bool compute_water_distance(populate_context_t *context)
{
	field_t *field = &context->water_distance;
	const water_t *water = context->water;
	int size;
	const float straight = FIELD_CELL_METERS;
	const float diagonal = FIELD_CELL_METERS * 1.41421356f;

	context->water_distance_made = true;
	if (!field_create(field, context->map_meters))
	{
		return false;
	}
	size = field->size;
	#pragma omp parallel for schedule(dynamic, 8)
	for (int j = 0; j < size; j++)
	{
		for (int i = 0; i < size; i++)
		{
			bool wet = false;

			if (water != NULL && water->depth != NULL)
			{
				int32_t first_x = clamp_int((int32_t)((float)i * FIELD_CELL_METERS / water->cell_size_meters), 0, water->size - 1);
				int32_t first_y = clamp_int((int32_t)((float)j * FIELD_CELL_METERS / water->cell_size_meters), 0, water->size - 1);
				int32_t last_x = clamp_int((int32_t)((float)(i + 1) * FIELD_CELL_METERS / water->cell_size_meters) - 1, first_x, water->size - 1);
				int32_t last_y = clamp_int((int32_t)((float)(j + 1) * FIELD_CELL_METERS / water->cell_size_meters) - 1, first_y, water->size - 1);
				int32_t deep = (int32_t)(NEAR_WATER_DEPTH_METERS * (float)TERRAIN_HEIGHT_UNITS_PER_METER);

				for (int32_t y = first_y; y <= last_y && !wet; y++)
				{
					for (int32_t x = first_x; x <= last_x; x++)
					{
						if (water->depth[y * water->size + x] > deep)
						{
							wet = true;
							break;
						}
					}
				}
			}
			field->values[j * size + i] = wet ? 0.0f : WATER_DISTANCE_LIMIT_METERS;
		}
	}
	for (int j = 0; j < size; j++)
	{
		for (int i = 0; i < size; i++)
		{
			float *value = &field->values[j * size + i];

			if (i > 0)
			{
				*value = SDL_min(*value, field->values[j * size + i - 1] + straight);
			}
			if (j > 0)
			{
				*value = SDL_min(*value, field->values[(j - 1) * size + i] + straight);
				if (i > 0)
				{
					*value = SDL_min(*value, field->values[(j - 1) * size + i - 1] + diagonal);
				}
				if (i < size - 1)
				{
					*value = SDL_min(*value, field->values[(j - 1) * size + i + 1] + diagonal);
				}
			}
		}
	}
	for (int j = size - 1; j >= 0; j--)
	{
		for (int i = size - 1; i >= 0; i--)
		{
			float *value = &field->values[j * size + i];

			if (i < size - 1)
			{
				*value = SDL_min(*value, field->values[j * size + i + 1] + straight);
			}
			if (j < size - 1)
			{
				*value = SDL_min(*value, field->values[(j + 1) * size + i] + straight);
				if (i < size - 1)
				{
					*value = SDL_min(*value, field->values[(j + 1) * size + i + 1] + diagonal);
				}
				if (i > 0)
				{
					*value = SDL_min(*value, field->values[(j + 1) * size + i - 1] + diagonal);
				}
			}
		}
	}
	return true;
}

// The water rules' share at a spot of distance meters from water: 0 outside
// them, up to 2 right by the water for the near ones (about one on average)
static float water_near_factor(const object_type_t *type, float distance)
{
	float factor = 1.0f;

	if (type->spawn_water_far_meters > 0.0f && distance < type->spawn_water_far_meters)
	{
		return 0.0f;
	}
	if (type->spawn_water_near_meters > 0.0f)
	{
		if (distance > type->spawn_water_near_meters)
		{
			return 0.0f;
		}
		factor = 2.0f * (1.0f - distance / type->spawn_water_near_meters);
	}
	return factor;
}

// The neighbors rule's share for count neighbors: 0..1
static float neighbors_factor(const object_type_t *type, float count)
{
	float enough = SDL_clamp(count / SDL_max(type->spawn_near_count, 0.01f), 0.0f, 1.0f);

	return type->spawn_near_inverted ? 1.0f - enough : enough;
}

// Objects of the category within radius of a spot, from the field cells
// whose middles are within it
static float near_count_at(const populate_context_t *context, const uint16_t *counts, float x, float z, float radius)
{
	int size = context->near_size;
	int center_x = (int)(x / FIELD_CELL_METERS);
	int center_z = (int)(z / FIELD_CELL_METERS);
	int reach = (int)SDL_ceilf(radius / FIELD_CELL_METERS);
	float limit = (radius + 0.5f * FIELD_CELL_METERS) * (radius + 0.5f * FIELD_CELL_METERS);
	int count = 0;

	for (int j = SDL_max(center_z - reach, 0); j <= SDL_min(center_z + reach, size - 1); j++)
	{
		for (int i = SDL_max(center_x - reach, 0); i <= SDL_min(center_x + reach, size - 1); i++)
		{
			float offset_x = ((float)i + 0.5f) * FIELD_CELL_METERS - x;
			float offset_z = ((float)j + 0.5f) * FIELD_CELL_METERS - z;

			if (offset_x * offset_x + offset_z * offset_z <= limit)
			{
				count += counts[j * size + i];
			}
		}
	}
	return (float)count;
}

static void near_count_add(populate_context_t *context, int category, float x, float z)
{
	uint16_t *counts = category >= 0 && category < OBJECT_CATEGORIES_MAX ? context->near_counts[category] : NULL;
	int i = (int)(x / FIELD_CELL_METERS);
	int j = (int)(z / FIELD_CELL_METERS);

	if (counts != NULL && i >= 0 && j >= 0 && i < context->near_size && j < context->near_size && counts[j * context->near_size + i] < UINT16_MAX)
	{
		counts[j * context->near_size + i]++;
	}
}

// How much more (or less) likely the type is at the spot than on average;
// 0 if it cannot grow there at all
static float placement_factor(populate_context_t *context, const object_type_t *type, const field_t *patch, float x, float z)
{
	float factor = 1.0f;

	if (type->spawn_water_near_meters > 0.0f || type->spawn_water_far_meters > 0.0f)
	{
		factor *= context->water_distance.values != NULL ? water_near_factor(type, field_sample(&context->water_distance, x, z)) : 0.0f;
		if (factor <= 0.0f)
		{
			return 0.0f;
		}
	}
	if (type->spawn_near[0] != '\0' && type->near_category >= 0 && context->near_counts[type->near_category] != NULL)
	{
		factor *= neighbors_factor(type, near_count_at(context, context->near_counts[type->near_category], x, z, type->spawn_near_meters));
		if (factor <= 0.0f)
		{
			return 0.0f;
		}
	}

	if (patch != NULL && type->spawn_clustering > 0.0f)
	{
		float inside = smoothstep_range(PATCH_EDGE_LOW, PATCH_EDGE_HIGH, field_sample(patch, x, z));

		if (type->spawn_patch_inverted)
		{
			inside = 1.0f - inside;
		}
		// Patches cover about half the map: twice the density inside them
		// keeps the average
		factor *= (1.0f - type->spawn_clustering) + type->spawn_clustering * 2.0f * inside;
	}
	if (type->spawn_valley_preference != 0.0f || type->spawn_valley_minimum > -1.0f)
	{
		float valley = field_sample(&context->valley, x, z);

		if (valley < type->spawn_valley_minimum)
		{
			return 0.0f;
		}
		factor *= SDL_max(0.0f, 1.0f + type->spawn_valley_preference * valley);
	}
	return factor;
}

static uint64_t cell_seed(uint32_t seed, int type, int i, int j)
{
	return ((uint64_t)seed * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(type + 1) << 48) ^ ((uint64_t)(uint32_t)j << 24) ^ (uint64_t)(uint32_t)i;
}

void populate_map(objects_t *objects, const terrain_t *terrain, const water_t *water, const objects_settings_t *settings)
{
	populate_map_categories(objects, terrain, water, settings, UINT32_MAX, 0);
}

// Whether the type's category is one of the mask's
static bool category_wanted(const object_type_t *type, uint32_t category_mask)
{
	return type->category >= 0 && type->category < 32 && (category_mask & (1u << type->category)) != 0;
}

void populate_map_categories(objects_t *objects, const terrain_t *terrain, const water_t *water, const objects_settings_t *settings,
	uint32_t category_mask, uint32_t variation)
{
	populate_context_t context;
	uint64_t start = SDL_GetPerformanceCounter();
	int placed_total = 0;
	int types_used = 0;

	SDL_zero(context);
	context.terrain = terrain;
	context.water = water;
	context.map_meters = (float)terrain->size * terrain->cell_size_meters;
	context.occupancy_size = SDL_max(1, (int)SDL_ceilf(context.map_meters / OCCUPANCY_CELL_METERS));
	context.occupied = SDL_calloc((size_t)context.occupancy_size * (size_t)context.occupancy_size, 1);
	if (context.occupied == NULL || !compute_valleys(&context))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory populating the map");
		goto done;
	}
	// The distances to water and the neighbor counts, if a type asks
	context.near_size = context.valley.size;
	for (int t = 0; t < objects->types.type_count; t++)
	{
		const object_type_t *type = &objects->types.types[t];

		if (type->spawn_per_hectare <= 0.0f || !category_wanted(type, category_mask))
		{
			continue;
		}
		if ((type->spawn_water_near_meters > 0.0f || type->spawn_water_far_meters > 0.0f) && !context.water_distance_made)
		{
			compute_water_distance(&context);
		}
		if (type->near_category >= 0 && type->near_category < OBJECT_CATEGORIES_MAX && context.near_counts[type->near_category] == NULL)
		{
			context.near_counts[type->near_category] = SDL_calloc((size_t)context.near_size * (size_t)context.near_size, sizeof(uint16_t));
		}
	}
	// What is already there keeps its footprint
	for (int k = 0; k < objects->instance_count; k++)
	{
		const object_instance_t *instance = &objects->instances[k];
		const object_type_t *type = &objects->types.types[instance->type];
		float footprint = type->footprint_meters * instance->scale;

		// What layouts placed (houses, gardens, fences) keeps nature out of
		// most of its size
		if (instance->group != 0)
		{
			footprint = SDL_max(footprint, 0.8f * objects->models[type->model].horizontal_radius * instance->scale);
		}
		if (footprint > 0.0f)
		{
			occupancy_mark(&context, instance->x, instance->z, footprint);
		}
		near_count_add(&context, type->category, instance->x, instance->z);
	}

	for (int t = 0; t < objects->types.type_count; t++)
	{
		const object_type_t *type = &objects->types.types[t];
		float spacing = type->spawn_spacing_meters;
		int cells = (int)SDL_ceilf(context.map_meters / spacing);
		float base_probability = type->spawn_per_hectare * 1e-4f * SDL_max(settings->density_scale, 0.0f) * spacing * spacing;
		// No spot is more likely than this: candidates above it are
		// rejected before anything is looked up
		float best_factor = (1.0f + type->spawn_clustering) * (1.0f + SDL_fabsf(type->spawn_valley_preference)) * (type->spawn_water_near_meters > 0.0f ? 2.0f : 1.0f);
		const field_t *patch = NULL;
		int placed = 0;

		if (base_probability <= 0.0f || type->model < 0 || !category_wanted(type, category_mask))
		{
			continue;
		}
		if (type->spawn_patch[0] != '\0' && type->spawn_clustering > 0.0f)
		{
			patch = patch_field(&context, type);
		}
		for (int j = 0; j < cells; j++)
		{
			for (int i = 0; i < cells; i++)
			{
				random_t random;
				float chance;
				float x;
				float z;
				float yaw;
				float scale;
				float footprint;

				random_seed(&random, cell_seed(terrain->seed + variation * 0x6F4A7C15u, t, i, j));
				chance = random_float(&random);
				if (chance >= base_probability * best_factor)
				{
					continue;
				}
				x = ((float)i + random_float(&random)) * spacing;
				z = ((float)j + random_float(&random)) * spacing;
				if (x >= context.map_meters || z >= context.map_meters)
				{
					continue;
				}
				if (chance >= base_probability * placement_factor(&context, type, patch, x, z))
				{
					continue;
				}
				if (!ground_allows(type, terrain, water, x, z, false))
				{
					continue;
				}
				objects_random_placement(objects, t, random_float(&random), random_float(&random), &yaw, &scale);
				footprint = type->footprint_meters * scale;
				if (!occupancy_free(&context, x, z, footprint))
				{
					continue;
				}
				if (objects_add(objects, terrain, t, x, z, yaw, scale, 0))
				{
					if (footprint > 0.0f)
					{
						occupancy_mark(&context, x, z, footprint);
					}
					near_count_add(&context, type->category, x, z);
					placed++;
				}
			}
		}
		placed_total += placed;
		types_used += placed > 0 ? 1 : 0;
	}
	objects_sort(objects);
	SDL_Log("Populated the map: %d objects of %d types in %.0f ms", placed_total, types_used,
		1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency());

done:
	SDL_free(context.occupied);
	SDL_free(context.valley.values);
	SDL_free(context.water_distance.values);
	for (int c = 0; c < OBJECT_CATEGORIES_MAX; c++)
	{
		SDL_free(context.near_counts[c]);
	}
	for (int p = 0; p < context.patch_count; p++)
	{
		SDL_free(context.patches[p].values);
	}
}

// --- The planting brush -----------------------------------------------------

// Calls the visitor for the instances near a spot: those in the grid cells
// the square around it touches, and those added since the last sort. The
// visitor returns false to stop early; so does this function then.
typedef bool (*instance_visitor_t)(void *user_data, const object_instance_t *instance);

static bool visit_neighborhood(const objects_t *objects, float x, float z, float reach, instance_visitor_t visitor, void *user_data)
{
	int last = objects->grid_size - 1;
	int first_cell_x = SDL_clamp((int)((x - reach) / OBJECTS_GRID_CELL_METERS), 0, last);
	int first_cell_z = SDL_clamp((int)((z - reach) / OBJECTS_GRID_CELL_METERS), 0, last);
	int last_cell_x = SDL_clamp((int)((x + reach) / OBJECTS_GRID_CELL_METERS), 0, last);
	int last_cell_z = SDL_clamp((int)((z + reach) / OBJECTS_GRID_CELL_METERS), 0, last);
	// Objects past the sorted ones were added since the sort (the caller
	// sorts before it starts adding)
	int sorted_count = objects->grid_first[objects->grid_size * objects->grid_size];

	sorted_count = SDL_min(sorted_count, objects->instance_count);
	for (int j = first_cell_z; j <= last_cell_z && sorted_count > 0; j++)
	{
		for (int i = first_cell_x; i <= last_cell_x; i++)
		{
			int cell = j * objects->grid_size + i;

			for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1] && k < sorted_count; k++)
			{
				if (!visitor(user_data, &objects->instances[k]))
				{
					return false;
				}
			}
		}
	}
	for (int k = sorted_count; k < objects->instance_count; k++)
	{
		if (!visitor(user_data, &objects->instances[k]))
		{
			return false;
		}
	}
	return true;
}

typedef struct
{
	const objects_t *objects;
	float x;
	float z;
	float footprint;
} spot_query_t;

static bool spot_visitor(void *user_data, const object_instance_t *instance)
{
	const spot_query_t *query = user_data;
	float other = query->objects->types.types[instance->type].footprint_meters * instance->scale;
	float limit = query->footprint + other;
	float offset_x = instance->x - query->x;
	float offset_z = instance->z - query->z;

	// Two footprintless things (grass) never block each other
	if (limit <= 0.0f)
	{
		return true;
	}
	return offset_x * offset_x + offset_z * offset_z >= limit * limit;
}

// True if nothing solid is in the way of a new object with this footprint
static bool brush_spot_free(const objects_t *objects, float x, float z, float footprint)
{
	spot_query_t query = { objects, x, z, footprint };

	return visit_neighborhood(objects, x, z, footprint + MAXIMUM_FOOTPRINT_METERS, spot_visitor, &query);
}

typedef struct
{
	const objects_t *objects;
	int category;
	float x;
	float z;
	float radius;
	int count;
} count_query_t;

static bool count_visitor(void *user_data, const object_instance_t *instance)
{
	count_query_t *query = user_data;
	float offset_x = instance->x - query->x;
	float offset_z = instance->z - query->z;

	if (query->objects->types.types[instance->type].category == query->category
		&& offset_x * offset_x + offset_z * offset_z <= query->radius * query->radius)
	{
		query->count++;
	}
	return true;
}

// Meters to the nearest water deeper than a hand, searched up to limit
// meters away (limit + 1 if none is)
static float brush_water_distance(const water_t *water, float x, float z, float limit)
{
	float best = limit + 1.0f;
	int32_t reach;
	int32_t center_x;
	int32_t center_y;
	int32_t deep = (int32_t)(NEAR_WATER_DEPTH_METERS * (float)TERRAIN_HEIGHT_UNITS_PER_METER);

	if (water == NULL || water->depth == NULL)
	{
		return best;
	}
	reach = (int32_t)SDL_ceilf(limit / water->cell_size_meters);
	center_x = (int32_t)(x / water->cell_size_meters);
	center_y = (int32_t)(z / water->cell_size_meters);
	for (int32_t y = SDL_max(center_y - reach, 0); y <= SDL_min(center_y + reach, water->size - 1); y++)
	{
		for (int32_t cell_x = SDL_max(center_x - reach, 0); cell_x <= SDL_min(center_x + reach, water->size - 1); cell_x++)
		{
			if (water->depth[y * water->size + cell_x] > deep)
			{
				float offset_x = ((float)cell_x + 0.5f) * water->cell_size_meters - x;
				float offset_z = ((float)y + 0.5f) * water->cell_size_meters - z;

				best = SDL_min(best, SDL_sqrtf(offset_x * offset_x + offset_z * offset_z));
			}
		}
	}
	return best;
}

int populate_brush(objects_t *objects, const terrain_t *terrain, const water_t *water, const objects_settings_t *settings,
	int category, float center_x, float center_z, float radius, float seconds, random_t *random)
{
	float weights[OBJECT_TYPES_MAX];
	int type_indices[OBJECT_TYPES_MAX];
	int type_count = 0;
	float density = 0.0f;
	float target;
	int present = 0;
	int wanted;
	int planted = 0;

	for (int t = 0; t < objects->types.type_count; t++)
	{
		const object_type_t *type = &objects->types.types[t];

		if (type->category == category && type->brush_per_hectare > 0.0f && type->model >= 0)
		{
			type_indices[type_count] = t;
			weights[type_count] = type->brush_per_hectare;
			density += type->brush_per_hectare;
			type_count++;
		}
	}
	if (type_count == 0 || radius <= 0.0f)
	{
		return 0;
	}
	objects_sort(objects);
	target = density * 1e-4f * SDL_max(settings->density_scale, 0.0f) * MATH3D_PI * radius * radius;

	// How many of the category the circle already holds
	{
		count_query_t query = { objects, category, center_x, center_z, radius, 0 };

		visit_neighborhood(objects, center_x, center_z, radius, count_visitor, &query);
		present = query.count;
	}
	if ((float)present >= target)
	{
		return 0;
	}
	// The circle fills up in brush_fill_seconds
	wanted = (int)SDL_ceilf(SDL_min(target - (float)present, target * seconds / SDL_max(settings->brush_fill_seconds, 0.05f)));
	wanted = SDL_clamp(wanted, 1, 500);

	for (int attempt = 0; attempt < wanted * 4 && planted < wanted; attempt++)
	{
		float angle = random_float(random) * 2.0f * MATH3D_PI;
		float distance = radius * SDL_sqrtf(random_float(random));
		float x = center_x + SDL_cosf(angle) * distance;
		float z = center_z + SDL_sinf(angle) * distance;
		float pick = random_float(random) * density;
		int chosen = type_indices[type_count - 1];
		const object_type_t *type;
		float yaw;
		float scale;

		for (int k = 0; k < type_count; k++)
		{
			if (pick < weights[k])
			{
				chosen = type_indices[k];
				break;
			}
			pick -= weights[k];
		}
		type = &objects->types.types[chosen];
		if (!ground_allows(type, terrain, water, x, z, true))
		{
			continue;
		}
		// The water nearby and the neighbors (as likely as when populating)
		if (type->spawn_water_near_meters > 0.0f || type->spawn_water_far_meters > 0.0f)
		{
			float distance = brush_water_distance(water, x, z, SDL_max(type->spawn_water_near_meters, type->spawn_water_far_meters));

			if (random_float(random) * 2.0f >= water_near_factor(type, distance) * (type->spawn_water_near_meters > 0.0f ? 1.0f : 2.0f))
			{
				continue;
			}
		}
		if (type->near_category >= 0)
		{
			count_query_t query = { objects, type->near_category, x, z, type->spawn_near_meters, 0 };

			visit_neighborhood(objects, x, z, type->spawn_near_meters, count_visitor, &query);
			if (random_float(random) >= neighbors_factor(type, (float)query.count))
			{
				continue;
			}
		}
		objects_random_placement(objects, chosen, random_float(random), random_float(random), &yaw, &scale);
		if (!brush_spot_free(objects, x, z, type->footprint_meters * scale))
		{
			continue;
		}
		if (objects_add(objects, terrain, chosen, x, z, yaw, scale, 0))
		{
			planted++;
		}
	}
	return planted;
}
