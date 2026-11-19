// The grass round the things that stand. See grass_mask.h.

#include "grass_mask.h"

#include <SDL3/SDL.h>

#include "object_format.h"

// The objects looked for round a strip: its half diagonal plus this (the
// biggest building's reach from its middle)
#define GRASS_MASK_OBJECT_REACH_METERS 40.0f

typedef struct
{
	grass_mask_t *mask;
	int32_t rectangle[4];          // texels made: first x, first z, last x + 1, last z + 1 (texel coordinates)
} rebuild_t;

// Wrapped into the window's storage (a negative coordinate too: two's complement)
static size_t texel_index(int32_t x, int32_t z)
{
	return (size_t)(z & (GRASS_MASK_SIDE - 1)) * GRASS_MASK_SIDE + (size_t)(x & (GRASS_MASK_SIDE - 1));
}

bool grass_mask_create(grass_mask_t *mask)
{
	SDL_zerop(mask);
	mask->texels = SDL_calloc(1, (size_t)GRASS_MASK_SIDE * GRASS_MASK_SIDE);
	mask->buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, (uint32_t)GRASS_MASK_SIDE * GRASS_MASK_SIDE);
	mask->failed = mask->texels == NULL || mask->buffer == NULL;
	if (mask->failed)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Grass mask: no memory for it; the grass grows round the things that stand as the material map says");
	}
	return !mask->failed;
}

void grass_mask_destroy(grass_mask_t *mask)
{
	if (mask->buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, mask->buffer);
	}
	SDL_free(mask->texels);
	SDL_free(mask->hulls);
	SDL_zerop(mask);
}

// --- Where a model meets the ground -------------------------------------------

static int compare_points(const void *a, const void *b)
{
	const float *first = a;
	const float *second = b;

	if (first[0] != second[0])
	{
		return first[0] < second[0] ? -1 : 1;
	}
	if (first[1] != second[1])
	{
		return first[1] < second[1] ? -1 : 1;
	}
	return 0;
}

static float cross_of(const float *origin, const float *a, const float *b)
{
	return (a[0] - origin[0]) * (b[1] - origin[1]) - (a[1] - origin[1]) * (b[0] - origin[0]);
}

// The convex hull of the points (x, z pairs; sorted in place), counter
// clockwise into hull_out (room for count + 1); how many
static int convex_hull(float (*points)[2], int count, float (*hull_out)[2])
{
	int size = 0;

	if (count < 3)
	{
		return 0;
	}
	SDL_qsort(points, (size_t)count, sizeof(points[0]), compare_points);
	// (Andrew's monotone chain: the lower half, then the upper)
	for (int i = 0; i < count; i++)
	{
		while (size >= 2 && cross_of(hull_out[size - 2], hull_out[size - 1], points[i]) <= 0.0f)
		{
			size--;
		}
		hull_out[size][0] = points[i][0];
		hull_out[size][1] = points[i][1];
		size++;
	}
	for (int i = count - 2, lower = size + 1; i >= 0; i--)
	{
		while (size >= lower && cross_of(hull_out[size - 2], hull_out[size - 1], points[i]) <= 0.0f)
		{
			size--;
		}
		hull_out[size][0] = points[i][0];
		hull_out[size][1] = points[i][1];
		size++;
	}
	return size > 1 ? size - 1 : 0;
}

// A point kept (the list grown as needed); false: no memory
static bool keep_point(float (**points)[2], int *count, int *capacity, float x, float z)
{
	if (*count >= *capacity)
	{
		int grown_capacity = *capacity > 0 ? *capacity * 2 : 1024;
		float (*grown)[2] = SDL_realloc(*points, sizeof(float) * 2 * (size_t)grown_capacity);

		if (grown == NULL)
		{
			return false;
		}
		*points = grown;
		*capacity = grown_capacity;
	}
	(*points)[*count][0] = x;
	(*points)[*count][1] = z;
	(*count)++;
	return true;
}

// The model's triangles cut by the slab round its origin: the corners
// inside it and where the edges cross its two planes, their hull
static void work_out_hull(const object_model_t *model, grass_mask_hull_t *hull)
{
	float (*points)[2] = NULL;
	int count = 0;
	int capacity = 0;
	float planes[2] = { -GRASS_MASK_SLICE_BELOW, GRASS_MASK_SLICE_ABOVE };
	int index_count = SDL_max(model->pick_index_count, model->collision_index_count);
	bool enough_memory = true;

	SDL_zerop(hull);
	hull->known = true;
	hull->source = model->collision_positions;
	if (model->collision_positions == NULL || model->collision_indices == NULL)
	{
		return;
	}
	for (int t = 0; t + 2 < index_count && enough_memory; t += 3)
	{
		const float *corners[3];

		for (int k = 0; k < 3; k++)
		{
			uint32_t vertex = model->collision_indices[t + k];

			if ((int)vertex >= model->collision_vertex_count)
			{
				corners[0] = NULL;
				break;
			}
			corners[k] = &model->collision_positions[(size_t)vertex * 3];
		}
		if (corners[0] == NULL)
		{
			continue;
		}
		for (int k = 0; k < 3 && enough_memory; k++)
		{
			const float *a = corners[k];
			const float *b = corners[(k + 1) % 3];

			if (a[1] >= planes[0] && a[1] <= planes[1])
			{
				enough_memory = keep_point(&points, &count, &capacity, a[0], a[2]);
			}
			for (int p = 0; p < 2 && enough_memory; p++)
			{
				if ((a[1] < planes[p]) != (b[1] < planes[p]))
				{
					float along = (planes[p] - a[1]) / (b[1] - a[1]);

					enough_memory = keep_point(&points, &count, &capacity, a[0] + (b[0] - a[0]) * along, a[2] + (b[2] - a[2]) * along);
				}
			}
		}
	}
	if (count >= 3)
	{
		float (*outline)[2] = SDL_malloc(sizeof(float) * 2 * (size_t)(count + 1));

		if (outline != NULL)
		{
			int size = convex_hull(points, count, outline);

			// (too many points: every so many kept, a little inside the
			// hull, which hardly shows a tenth of a meter at a time)
			for (int k = 0; k < GRASS_MASK_HULL_POINTS && k < size; k++)
			{
				int from = size <= GRASS_MASK_HULL_POINTS ? k : (int)((int64_t)k * size / GRASS_MASK_HULL_POINTS);

				hull->points[k][0] = outline[from][0];
				hull->points[k][1] = outline[from][1];
			}
			hull->point_count = SDL_min(size, GRASS_MASK_HULL_POINTS);
			SDL_free(outline);
		}
	}
	SDL_free(points);
}

static const grass_mask_hull_t *model_hull(grass_mask_t *mask, const objects_t *objects, int model_index)
{
	const object_model_t *model = &objects->models[model_index];

	if (model_index >= mask->hull_count)
	{
		int count = objects->model_count;
		grass_mask_hull_t *grown = SDL_realloc(mask->hulls, sizeof(grass_mask_hull_t) * (size_t)count);

		if (grown == NULL)
		{
			return NULL;
		}
		SDL_memset(grown + mask->hull_count, 0, sizeof(grass_mask_hull_t) * (size_t)(count - mask->hull_count));
		mask->hulls = grown;
		mask->hull_count = count;
	}
	if (!mask->hulls[model_index].known || mask->hulls[model_index].source != model->collision_positions)
	{
		work_out_hull(model, &mask->hulls[model_index]);
	}
	return &mask->hulls[model_index];
}

// --- Drawing into the mask ------------------------------------------------------

static bool inside_outline(const float (*points)[2], int count, float x, float z)
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

static float squared_distance_to_outline(const float (*points)[2], int count, float x, float z)
{
	float nearest = 1e30f;

	for (int i = 0, j = count - 1; i < count; j = i++)
	{
		float edge_x = points[i][0] - points[j][0];
		float edge_z = points[i][1] - points[j][1];
		float length_squared = edge_x * edge_x + edge_z * edge_z;
		float along = length_squared > 1e-12f ? ((x - points[j][0]) * edge_x + (z - points[j][1]) * edge_z) / length_squared : 0.0f;
		float dx;
		float dz;

		along = SDL_clamp(along, 0.0f, 1.0f);
		dx = x - (points[j][0] + edge_x * along);
		dz = z - (points[j][1] + edge_z * along);
		nearest = SDL_min(nearest, dx * dx + dz * dz);
	}
	return nearest;
}

// An outline (world x, z) into the texels of the rectangle: inside 255,
// thinning to 0 over margin meters outside; each texel keeps the most
static void draw_outline(grass_mask_t *mask, const int32_t rectangle[4], const float (*points)[2], int count, float margin)
{
	float minimum[2] = { 1e30f, 1e30f };
	float maximum[2] = { -1e30f, -1e30f };
	float texel = 1.0f / (float)GRASS_MASK_TEXELS_PER_METER;
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;

	for (int k = 0; k < count; k++)
	{
		minimum[0] = SDL_min(minimum[0], points[k][0]);
		minimum[1] = SDL_min(minimum[1], points[k][1]);
		maximum[0] = SDL_max(maximum[0], points[k][0]);
		maximum[1] = SDL_max(maximum[1], points[k][1]);
	}
	first_x = SDL_max((int32_t)SDL_floorf((minimum[0] - margin) * (float)GRASS_MASK_TEXELS_PER_METER), rectangle[0]);
	first_z = SDL_max((int32_t)SDL_floorf((minimum[1] - margin) * (float)GRASS_MASK_TEXELS_PER_METER), rectangle[1]);
	last_x = SDL_min((int32_t)SDL_ceilf((maximum[0] + margin) * (float)GRASS_MASK_TEXELS_PER_METER), rectangle[2]);
	last_z = SDL_min((int32_t)SDL_ceilf((maximum[1] + margin) * (float)GRASS_MASK_TEXELS_PER_METER), rectangle[3]);
	for (int32_t z = first_z; z < last_z; z++)
	{
		float world_z = ((float)z + 0.5f) * texel;

		for (int32_t x = first_x; x < last_x; x++)
		{
			float world_x = ((float)x + 0.5f) * texel;
			uint8_t *stored = &mask->texels[texel_index(x, z)];
			uint8_t value;

			if (inside_outline(points, count, world_x, world_z))
			{
				value = 255;
			}
			else
			{
				float distance_squared;

				if (margin <= 0.0f || *stored == 255)
				{
					continue;
				}
				distance_squared = squared_distance_to_outline(points, count, world_x, world_z);
				if (distance_squared >= margin * margin)
				{
					continue;
				}
				value = (uint8_t)(255.0f * (1.0f - SDL_sqrtf(distance_squared) / margin) + 0.5f);
			}
			if (value > *stored)
			{
				*stored = value;
			}
		}
	}
}

// How far round it the grass thins away (meters); below 0: it does not
// clear the grass
static float clear_meters_of(const object_type_t *type)
{
	if (type->grass_clear_meters == OBJECT_GRASS_CLEAR_AUTO)
	{
		return type->physics == OBJECT_PHYSICS_STATIC || type->physics == OBJECT_PHYSICS_BODY ? GRASS_MASK_AUTO_METERS : -1.0f;
	}
	return type->grass_clear_meters;
}

// Model x, z to the world: turned by its yaw (from its rotation, as the
// footprints do: a footprint lies flat), sized, moved where it stands
static void place_points(const object_instance_t *instance, const float (*model_points)[2], int count, float (*world_out)[2])
{
	float yaw_x = 1.0f - 2.0f * (instance->rotation[1] * instance->rotation[1] + instance->rotation[2] * instance->rotation[2]);
	float yaw_z = 2.0f * (instance->rotation[0] * instance->rotation[2] - instance->rotation[3] * instance->rotation[1]);
	float length = SDL_sqrtf(yaw_x * yaw_x + yaw_z * yaw_z);

	if (length < 1e-6f)
	{
		yaw_x = 1.0f;
		yaw_z = 0.0f;
	}
	else
	{
		yaw_x /= length;
		yaw_z /= length;
	}
	for (int k = 0; k < count; k++)
	{
		float model_x = model_points[k][0] * instance->scale;
		float model_z = model_points[k][1] * instance->scale;

		// (the model's X axis turned is (yaw_x, yaw_z); its Z axis (-yaw_z, yaw_x))
		world_out[k][0] = instance->x + model_x * yaw_x - model_z * yaw_z;
		world_out[k][1] = instance->z + model_x * yaw_z + model_z * yaw_x;
	}
}

static bool draw_object(const objects_t *objects, int index, void *user)
{
	rebuild_t *rebuild = user;
	const object_instance_t *instance = &objects->instances[index];
	const object_type_t *type = &objects->types.types[instance->type];
	float margin = clear_meters_of(type);
	float placed[SDL_max(GRASS_MASK_HULL_POINTS, OBJECT_FILE_MAX_FOOTPRINT_POINTS)][2];
	const grass_mask_hull_t *hull;
	const object_model_t *model;
	int model_index;

	if (margin < 0.0f || type->floats
		|| (instance->state & (OBJECT_STATE_FREE | OBJECT_STATE_MOVING | OBJECT_STATE_HIDDEN | OBJECT_STATE_DEAD)) != 0)
	{
		return true;
	}
	model_index = objects_instance_model(objects, instance);
	if (model_index < 0 || model_index >= objects->model_count)
	{
		return true;
	}
	model = &objects->models[model_index];
	hull = model_hull(rebuild->mask, objects, model_index);
	if (hull != NULL && hull->point_count >= 3)
	{
		place_points(instance, (const float(*)[2])hull->points, hull->point_count, placed);
		draw_outline(rebuild->mask, rebuild->rectangle, (const float(*)[2])placed, hull->point_count, margin);
	}
	for (int f = 0; f < model->footprint_count; f++)
	{
		const object_file_footprint_t *footprint = &model->footprints[f];
		int count = (int)SDL_min(footprint->point_count, (uint32_t)OBJECT_FILE_MAX_FOOTPRINT_POINTS);

		if ((footprint->flags & OBJECT_FOOTPRINT_NO_GRASS) == 0 || count < 3)
		{
			continue;
		}
		place_points(instance, (const float(*)[2])footprint->points, count, placed);
		draw_outline(rebuild->mask, rebuild->rectangle, (const float(*)[2])placed, count, margin);
	}
	return true;
}

// The texels of the rectangle (texel coordinates, inside the window)
// cleared and drawn again from the objects round it
static void rebuild_rectangle(grass_mask_t *mask, const objects_t *objects, int32_t first_x, int32_t first_z, int32_t end_x, int32_t end_z)
{
	rebuild_t rebuild;
	float texel = 1.0f / (float)GRASS_MASK_TEXELS_PER_METER;
	float half_width;
	float half_depth;

	if (end_x <= first_x || end_z <= first_z)
	{
		return;
	}
	for (int32_t z = first_z; z < end_z; z++)
	{
		for (int32_t x = first_x; x < end_x; x++)
		{
			mask->texels[texel_index(x, z)] = 0;
		}
	}
	rebuild.mask = mask;
	rebuild.rectangle[0] = first_x;
	rebuild.rectangle[1] = first_z;
	rebuild.rectangle[2] = end_x;
	rebuild.rectangle[3] = end_z;
	half_width = 0.5f * (float)(end_x - first_x) * texel;
	half_depth = 0.5f * (float)(end_z - first_z) * texel;
	objects_visit_circle(objects, (float)(first_x + end_x) * 0.5f * texel, (float)(first_z + end_z) * 0.5f * texel,
		SDL_sqrtf(half_width * half_width + half_depth * half_depth) + GRASS_MASK_OBJECT_REACH_METERS, draw_object, &rebuild);
}

// --- Following the eye ------------------------------------------------------------

void grass_mask_update(grass_mask_t *mask, const objects_t *objects, float eye_x, float eye_z)
{
	int32_t wanted_x = (int32_t)SDL_floorf(eye_x * (float)GRASS_MASK_TEXELS_PER_METER) - GRASS_MASK_SIDE / 2;
	int32_t wanted_z = (int32_t)SDL_floorf(eye_z * (float)GRASS_MASK_TEXELS_PER_METER) - GRASS_MASK_SIDE / 2;
	uint32_t changes = objects->change_base + (uint32_t)objects->change_count;
	bool changed = false;
	uint64_t now = SDL_GetTicksNS();

	if (mask->failed)
	{
		return;
	}
	// (what stands changed: added, gone, sorted, come loose, moved by hand)
	if (objects->generation != mask->objects_generation || changes != mask->objects_changes || objects->moved_box_count > 0)
	{
		mask->objects_generation = objects->generation;
		mask->objects_changes = changes;
		mask->rebuild_pending = true;
	}
	if (!mask->valid || (mask->rebuild_pending && (double)(now - mask->rebuilt_nanoseconds) >= GRASS_MASK_LOOK_SECONDS * 1e9)
		|| SDL_abs(wanted_x - mask->origin_x) >= GRASS_MASK_SIDE || SDL_abs(wanted_z - mask->origin_z) >= GRASS_MASK_SIDE)
	{
		mask->origin_x = wanted_x;
		mask->origin_z = wanted_z;
		rebuild_rectangle(mask, objects, wanted_x, wanted_z, wanted_x + GRASS_MASK_SIDE, wanted_z + GRASS_MASK_SIDE);
		mask->valid = true;
		mask->rebuild_pending = false;
		mask->rebuilt_nanoseconds = now;
		changed = true;
	}
	else if (SDL_abs(wanted_x - mask->origin_x) >= GRASS_MASK_RECENTER_TEXELS || SDL_abs(wanted_z - mask->origin_z) >= GRASS_MASK_RECENTER_TEXELS)
	{
		int32_t old_x = mask->origin_x;
		int32_t old_z = mask->origin_z;

		mask->origin_x = wanted_x;
		mask->origin_z = wanted_z;
		// The strips come into the window (where the old one did not reach),
		// the corner between them in the first
		if (wanted_x > old_x)
		{
			rebuild_rectangle(mask, objects, old_x + GRASS_MASK_SIDE, wanted_z, wanted_x + GRASS_MASK_SIDE, wanted_z + GRASS_MASK_SIDE);
		}
		else if (wanted_x < old_x)
		{
			rebuild_rectangle(mask, objects, wanted_x, wanted_z, old_x, wanted_z + GRASS_MASK_SIDE);
		}
		{
			int32_t first_x = SDL_max(wanted_x, old_x);
			int32_t end_x = SDL_min(wanted_x, old_x) + GRASS_MASK_SIDE;

			if (wanted_z > old_z)
			{
				rebuild_rectangle(mask, objects, first_x, old_z + GRASS_MASK_SIDE, end_x, wanted_z + GRASS_MASK_SIDE);
			}
			else if (wanted_z < old_z)
			{
				rebuild_rectangle(mask, objects, first_x, wanted_z, end_x, old_z);
			}
		}
		changed = true;
	}
	if (changed)
	{
		gpu_upload_buffer(mask->buffer, 0, mask->texels, (size_t)GRASS_MASK_SIDE * GRASS_MASK_SIDE);
		gpu_flush_uploads();
	}
}

void grass_mask_window(const grass_mask_t *mask, float window_out[4])
{
	bool usable = !mask->failed && mask->valid;

	window_out[0] = (float)mask->origin_x;
	window_out[1] = (float)mask->origin_z;
	window_out[2] = (float)GRASS_MASK_TEXELS_PER_METER;
	window_out[3] = usable ? (float)GRASS_MASK_SIDE : 0.0f;
}
