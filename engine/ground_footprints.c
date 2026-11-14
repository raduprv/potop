// The ground under the buildings (ground_footprints.h)

#include "ground_footprints.h"

#include <SDL3/SDL.h>

#include "material_map.h"
#include "materials.h"

// How often what stands is looked over (seconds)
#define GROUND_FOOTPRINTS_LOOK_SECONDS 0.5f

// The ground byte's bit for a cell marked before this pass (its marks
// cleared, to tell after which lost their building)
#define GROUND_WAS_MARKED 0x80u
// And for a cell in the list of those marked now (a cover alone marks no
// other bit)
#define GROUND_IN_LIST 0x40u

void ground_footprints_initialize(ground_footprints_t *footprints)
{
	SDL_zerop(footprints);
}

void ground_footprints_destroy(ground_footprints_t *footprints)
{
	SDL_free(footprints->cells);
	SDL_free(footprints->covers);
	SDL_free(footprints->standing);
	SDL_zerop(footprints);
}

static uint64_t mix_signature(uint64_t signature, uint64_t value)
{
	signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
	return signature;
}

static uint64_t float_bits(float value)
{
	uint32_t bits;

	SDL_memcpy(&bits, &value, sizeof(bits));
	return bits;
}

// The model with footprints an object stands as, NULL if none (moving
// objects leave none: the physics has them)
static const object_model_t *footprint_model(const objects_t *objects, const object_instance_t *instance)
{
	int model = objects_instance_model(objects, instance);

	if (model < 0 || objects->models[model].footprint_count == 0 || (instance->state & OBJECT_STATE_FREE) != 0)
	{
		return NULL;
	}
	return &objects->models[model];
}

// Whether an object might stand with a footprint: one of its type's models
// has one (as it is, damaged, destroyed)
static bool may_have_footprint(const objects_t *objects, const object_instance_t *instance)
{
	const object_type_t *type = &objects->types.types[instance->type];
	int models[3] = { type->model, type->damaged_model, type->destroyed_model };

	for (int k = 0; k < 3; k++)
	{
		if (models[k] >= 0 && models[k] < objects->model_count && objects->models[models[k]].footprint_count > 0)
		{
			return true;
		}
	}
	return false;
}

// The objects that may have a footprint, found again when the list changed
static void find_standing(ground_footprints_t *footprints, const objects_t *objects)
{
	if (footprints->standing_known && footprints->standing_generation == objects->generation)
	{
		return;
	}
	footprints->standing_count = 0;
	for (int i = 0; i < objects->instance_count; i++)
	{
		if (!may_have_footprint(objects, &objects->instances[i]))
		{
			continue;
		}
		if (footprints->standing_count >= footprints->standing_capacity)
		{
			int capacity = SDL_max(footprints->standing_capacity * 2, 256);
			int *grown = SDL_realloc(footprints->standing, sizeof(int) * (size_t)capacity);

			if (grown == NULL)
			{
				break;
			}
			footprints->standing = grown;
			footprints->standing_capacity = capacity;
		}
		footprints->standing[footprints->standing_count++] = i;
	}
	footprints->standing_generation = objects->generation;
	footprints->standing_known = true;
}

static uint64_t signature_of(const ground_footprints_t *footprints, const objects_t *objects)
{
	uint64_t signature = 1469598103934665603ull;

	for (int n = 0; n < footprints->standing_count; n++)
	{
		int i = footprints->standing[n];
		const object_instance_t *instance = &objects->instances[i];
		const object_model_t *model = i < objects->instance_count ? footprint_model(objects, instance) : NULL;

		if (model == NULL)
		{
			continue;
		}
		signature = mix_signature(signature, instance->id);
		signature = mix_signature(signature, float_bits(instance->x));
		signature = mix_signature(signature, float_bits(instance->z));
		signature = mix_signature(signature, float_bits(instance->yaw_radians));
		signature = mix_signature(signature, float_bits(instance->scale));
		signature = mix_signature(signature, (uint64_t)(uintptr_t)model);
		signature = mix_signature(signature, (uint64_t)model->footprint_count);
	}
	return signature;
}

static bool point_inside(const float (*points)[2], int count, float x, float z)
{
	bool inside = false;

	for (int i = 0, j = count - 1; i < count; j = i++)
	{
		if ((points[i][1] > z) != (points[j][1] > z)
			&& x < (points[j][0] - points[i][0]) * (z - points[i][1]) / (points[j][1] - points[i][1]) + points[i][0])
		{
			inside = !inside;
		}
	}
	return inside;
}

// A cell newly marked: its index kept, its cover none yet; its place in
// the list (-1: no room)
static int remember_cell(ground_footprints_t *footprints, uint32_t index)
{
	if (footprints->cell_count >= footprints->cell_capacity)
	{
		int capacity = footprints->cell_capacity > 0 ? footprints->cell_capacity * 2 : 4096;
		uint32_t *grown = SDL_realloc(footprints->cells, sizeof(uint32_t) * (size_t)capacity);
		uint8_t *grown_covers;

		if (grown == NULL)
		{
			return -1;
		}
		footprints->cells = grown;
		grown_covers = SDL_realloc(footprints->covers, (size_t)capacity);
		if (grown_covers == NULL)
		{
			return -1;
		}
		footprints->covers = grown_covers;
		footprints->cell_capacity = capacity;
	}
	footprints->cells[footprints->cell_count] = index;
	footprints->covers[footprints->cell_count] = MATERIAL_NONE;
	return footprints->cell_count++;
}

// Where a cell is in the list of those marked (the last ones first: a
// footprint's cells were just added), -1 if not there
static int find_cell(const ground_footprints_t *footprints, uint32_t index, int from)
{
	for (int i = footprints->cell_count - 1; i >= from; i--)
	{
		if (footprints->cells[i] == index)
		{
			return i;
		}
	}
	for (int i = from - 1; i >= 0; i--)
	{
		if (footprints->cells[i] == index)
		{
			return i;
		}
	}
	return -1;
}

static uint8_t material_by_name(const char *name)
{
	for (int m = 0; m < MATERIAL_COUNT && name[0] != '\0'; m++)
	{
		if (SDL_strcasecmp(name, materials_name((material_type_t)m)) == 0)
		{
			return (uint8_t)m;
		}
	}
	return MATERIAL_NONE;
}

// One outline of an object's, into the ground bytes: the cells it touches
// at all no grass, those it covers whole hidden
static void mark_outline(ground_footprints_t *footprints, terrain_t *terrain, const object_instance_t *instance,
	const object_file_footprint_t *footprint, int32_t rectangle[4])
{
	float points[OBJECT_FILE_MAX_FOOTPRINT_POINTS][2];
	int count = (int)footprint->point_count;
	float cell = terrain->cell_size_meters;
	float minimum[2] = { 1e30f, 1e30f };
	float maximum[2] = { -1e30f, -1e30f };
	// (turned by its rotation about Y: its yaw; a footprint lies flat)
	uint8_t cover = (footprint->flags & OBJECT_FOOTPRINT_COVER) != 0 ? material_by_name(footprint->cover) : MATERIAL_NONE;
	int first_new = footprints->cell_count;
	float yaw_x = 1.0f - 2.0f * (instance->rotation[1] * instance->rotation[1] + instance->rotation[2] * instance->rotation[2]);
	float yaw_z = 2.0f * (instance->rotation[0] * instance->rotation[2] - instance->rotation[3] * instance->rotation[1]);
	float length = SDL_sqrtf(yaw_x * yaw_x + yaw_z * yaw_z);

	if (count < 3 || length < 1e-6f)
	{
		return;
	}
	yaw_x /= length;
	yaw_z /= length;
	for (int k = 0; k < count; k++)
	{
		float model_x = footprint->points[k][0] * instance->scale;
		float model_z = footprint->points[k][1] * instance->scale;

		// (the model's X axis turned is (yaw_x, yaw_z); its Z axis (-yaw_z, yaw_x))
		points[k][0] = instance->x + model_x * yaw_x - model_z * yaw_z;
		points[k][1] = instance->z + model_x * yaw_z + model_z * yaw_x;
		minimum[0] = SDL_min(minimum[0], points[k][0]);
		minimum[1] = SDL_min(minimum[1], points[k][1]);
		maximum[0] = SDL_max(maximum[0], points[k][0]);
		maximum[1] = SDL_max(maximum[1], points[k][1]);
	}
	{
		int32_t x0 = SDL_max((int32_t)SDL_floorf(minimum[0] / cell - 0.5f), 0);
		int32_t y0 = SDL_max((int32_t)SDL_floorf(minimum[1] / cell - 0.5f), 0);
		int32_t x1 = SDL_min((int32_t)SDL_ceilf(maximum[0] / cell + 0.5f), terrain->size - 1);
		int32_t y1 = SDL_min((int32_t)SDL_ceilf(maximum[1] / cell + 0.5f), terrain->size - 1);

		for (int32_t y = y0; y <= y1; y++)
		{
			for (int32_t x = x0; x <= x1; x++)
			{
				// The cell's square round its middle (x, y) * cell
				float left = ((float)x - 0.5f) * cell;
				float right = ((float)x + 0.5f) * cell;
				float near = ((float)y - 0.5f) * cell;
				float far = ((float)y + 0.5f) * cell;
				int corners = (point_inside((const float(*)[2])points, count, left, near) ? 1 : 0) + (point_inside((const float(*)[2])points, count, right, near) ? 1 : 0)
					+ (point_inside((const float(*)[2])points, count, left, far) ? 1 : 0) + (point_inside((const float(*)[2])points, count, right, far) ? 1 : 0);
				bool middle = point_inside((const float(*)[2])points, count, (float)x * cell, (float)y * cell);
				uint8_t marks = 0;
				uint8_t *ground;

				bool touched = corners > 0 || middle;
				int slot;

				if ((footprint->flags & OBJECT_FOOTPRINT_NO_GRASS) != 0 && touched)
				{
					marks |= MATERIAL_GROUND_NO_GRASS;
				}
				if ((footprint->flags & OBJECT_FOOTPRINT_NO_TERRAIN) != 0 && corners == 4)
				{
					marks |= MATERIAL_GROUND_HIDDEN | MATERIAL_GROUND_NO_GRASS;
				}
				if (marks == 0 && !(cover != MATERIAL_NONE && touched))
				{
					continue;
				}
				ground = &terrain->materials[(size_t)terrain_index(terrain, x, y) * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_GROUND];
				// (a cell is in the list once: marked already, or found
				// there with a cover only)
				slot = (*ground & (MATERIAL_GROUND_NO_GRASS | MATERIAL_GROUND_HIDDEN | GROUND_IN_LIST)) == 0
					? remember_cell(footprints, (uint32_t)terrain_index(terrain, x, y))
					: find_cell(footprints, (uint32_t)terrain_index(terrain, x, y), first_new);
				if (slot < 0)
				{
					continue;
				}
				*ground |= marks | GROUND_IN_LIST;
				if (cover != MATERIAL_NONE && touched)
				{
					footprints->covers[slot] = cover;
				}
				rectangle[0] = SDL_min(rectangle[0], x);
				rectangle[1] = SDL_min(rectangle[1], y);
				rectangle[2] = SDL_max(rectangle[2], x);
				rectangle[3] = SDL_max(rectangle[3], y);
			}
		}
	}
}

// The covers the footprints lay, where something took them (a flood, a
// restamp): put back
static void lay_covers(ground_footprints_t *footprints, terrain_t *terrain)
{
	int32_t rectangle[4] = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };

	for (int i = 0; i < footprints->cell_count; i++)
	{
		uint8_t *cell = &terrain->materials[(size_t)footprints->cells[i] * TERRAIN_MATERIAL_BYTES];
		int32_t x = (int32_t)(footprints->cells[i] % (uint32_t)terrain->size);
		int32_t y = (int32_t)(footprints->cells[i] / (uint32_t)terrain->size);

		if (footprints->covers[i] == MATERIAL_NONE || cell[MATERIAL_MAP_COVER] == footprints->covers[i])
		{
			continue;
		}
		cell[MATERIAL_MAP_COVER] = footprints->covers[i];
		rectangle[0] = SDL_min(rectangle[0], x);
		rectangle[1] = SDL_min(rectangle[1], y);
		rectangle[2] = SDL_max(rectangle[2], x);
		rectangle[3] = SDL_max(rectangle[3], y);
	}
	if (rectangle[0] <= rectangle[2])
	{
		terrain_mark_materials_changed(terrain, rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
	}
}

void ground_footprints_update(ground_footprints_t *footprints, terrain_t *terrain, const objects_t *objects, float frame_seconds)
{
	uint32_t *old_cells;
	int old_count;
	int32_t rectangle[4] = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
	uint64_t signature;
	bool stale = terrain->ground_footprints_stale;

	if (terrain->materials == NULL || terrain->size <= 0)
	{
		return;
	}
	footprints->seconds_to_next_look -= frame_seconds;
	if (footprints->seconds_to_next_look > 0.0f && !stale)
	{
		return;
	}
	footprints->seconds_to_next_look = GROUND_FOOTPRINTS_LOOK_SECONDS;
	find_standing(footprints, objects);
	signature = signature_of(footprints, objects);
	if (signature == footprints->signature && !stale)
	{
		lay_covers(footprints, terrain);
		return;
	}
	footprints->signature = signature;
	terrain->ground_footprints_stale = false;
	// The cells marked before: their marks cleared, remembered (not after
	// the bytes were cleared: those marks are gone, and nothing lost its
	// building)
	old_cells = footprints->cells;
	old_count = footprints->cell_count;
	SDL_free(footprints->covers);
	footprints->covers = NULL;
	footprints->cells = NULL;
	footprints->cell_count = 0;
	footprints->cell_capacity = 0;
	for (int i = 0; i < old_count && !stale; i++)
	{
		uint8_t *ground = &terrain->materials[(size_t)old_cells[i] * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_GROUND];
		int32_t x = (int32_t)(old_cells[i] % (uint32_t)terrain->size);
		int32_t y = (int32_t)(old_cells[i] / (uint32_t)terrain->size);

		*ground = (uint8_t)((*ground & ~(MATERIAL_GROUND_NO_GRASS | MATERIAL_GROUND_HIDDEN | GROUND_IN_LIST)) | GROUND_WAS_MARKED);
		rectangle[0] = SDL_min(rectangle[0], x);
		rectangle[1] = SDL_min(rectangle[1], y);
		rectangle[2] = SDL_max(rectangle[2], x);
		rectangle[3] = SDL_max(rectangle[3], y);
	}
	// Every footprint of what stands now
	for (int n = 0; n < footprints->standing_count; n++)
	{
		int i = footprints->standing[n];
		const object_instance_t *instance = &objects->instances[i];
		const object_model_t *model = i < objects->instance_count ? footprint_model(objects, instance) : NULL;

		for (int f = 0; model != NULL && f < model->footprint_count; f++)
		{
			mark_outline(footprints, terrain, instance, &model->footprints[f], rectangle);
		}
	}
	// Where a building stood and stands no more: bare earth
	for (int i = 0; i < old_count && !stale; i++)
	{
		uint8_t *cell = &terrain->materials[(size_t)old_cells[i] * TERRAIN_MATERIAL_BYTES];

		if ((cell[MATERIAL_MAP_GROUND] & GROUND_IN_LIST) == 0)
		{
			cell[MATERIAL_MAP_COVER] = MATERIAL_DIRT;
		}
		cell[MATERIAL_MAP_GROUND] &= (uint8_t)~GROUND_WAS_MARKED;
	}
	SDL_free(old_cells);
	if (rectangle[0] <= rectangle[2])
	{
		terrain_mark_materials_changed(terrain, rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
	}
	lay_covers(footprints, terrain);
}
