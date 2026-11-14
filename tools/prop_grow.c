// Making props: see prop_grow.h.
//
// Rocks and walls are carved: each stone is a shape told by its distance
// (a signed distance: below 0 inside it), a lump between an ellipsoid and a
// rounded block, made lumpy with noise, cut flat where it split (facets) and
// at its foot, grooved between its layers, holed through. A wall is many
// such stones laid in courses, a heap of pebbles a few. The distances on a
// grid of cells, the surface is found where they change sign (surface
// nets: a vertex in each cell the surface passes through, a quad across
// each edge it crosses), and what the moss, the lichen and the snow need is
// asked of the same distances: how hidden each vertex is (its crevices),
// how sharp the surface is there (its edges) and how much sky it sees.
//
// A fence is built of pieces of wood, each a prism (a board, a post, a
// picket with its pointed top), every face its own, with texture
// coordinates along its grain from a random place of the wood's texture.

// (mkdir, strtok_r: POSIX)
#define _DEFAULT_SOURCE
#include "prop_grow.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "stb_image_write.h"

#define PROP_PI 3.14159265358979f
#define MAX_FACETS 16
#define MAX_HOLES 4
#define MAX_STONES 1024
#define SKY_DIRECTIONS 24
#define TEXTURE_SIZE 1024
// The wood's texture: the side grain above this (of its height), the ends below
#define WOOD_SIDE_PART 0.75f

static char last_error[1024];

static void prop_error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(last_error, sizeof(last_error), format, arguments);
	va_end(arguments);
}

const char *prop_last_error(void)
{
	return last_error;
}

// ---------------------------------------------------------------------------
// Small helpers

typedef struct
{
	float x;
	float y;
	float z;
} vec_t;

static vec_t vec(float x, float y, float z)
{
	vec_t v = { x, y, z };

	return v;
}

static vec_t add(vec_t a, vec_t b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec_t sub(vec_t a, vec_t b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec_t scale(vec_t a, float s) { return vec(a.x * s, a.y * s, a.z * s); }
static float dot(vec_t a, vec_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length_of(vec_t a) { return sqrtf(dot(a, a)); }

static vec_t cross(vec_t a, vec_t b)
{
	return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static vec_t normalized(vec_t a)
{
	float length = length_of(a);

	return length > 1e-12f ? scale(a, 1.0f / length) : vec(0.0f, 1.0f, 0.0f);
}

static float clamp_float(float value, float low, float high)
{
	return value < low ? low : (value > high ? high : value);
}

static float smooth_step(float low, float high, float value)
{
	float t = clamp_float((value - low) / (high - low), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

static float minimum_of(float a, float b) { return a < b ? a : b; }
static float maximum_of(float a, float b) { return a > b ? a : b; }

// A random number generator of its own (a seed gives the same prop on any
// machine)
typedef struct
{
	uint64_t state;
} random_t;

static uint32_t random_next(random_t *random)
{
	uint64_t old = random->state;
	uint32_t shifted;
	uint32_t rotation;

	random->state = old * 6364136223846793005ull + 1442695040888963407ull;
	shifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
	rotation = (uint32_t)(old >> 59u);
	return (shifted >> rotation) | (shifted << ((-rotation) & 31u));
}

static void random_seed(random_t *random, uint32_t seed)
{
	random->state = 0x853c49e6748fea9bull ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull);
	random_next(random);
	random_next(random);
}

// 0..1
static float random_unit(random_t *random)
{
	return (float)(random_next(random) >> 8) / 16777216.0f;
}

static float random_between(random_t *random, float low, float high)
{
	return low + (high - low) * random_unit(random);
}

// ---------------------------------------------------------------------------
// Noise

static uint32_t hash_cell(int x, int y, int z, uint32_t seed)
{
	uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)x * 0x85EBCA77u ^ (uint32_t)y * 0xC2B2AE3Du ^ (uint32_t)z * 0x27D4EB2Fu;

	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return h;
}

static float gradient_dot(uint32_t hash, float x, float y, float z)
{
	switch (hash % 12u)
	{
		case 0: return x + y;
		case 1: return -x + y;
		case 2: return x - y;
		case 3: return -x - y;
		case 4: return x + z;
		case 5: return -x + z;
		case 6: return x - z;
		case 7: return -x - z;
		case 8: return y + z;
		case 9: return -y + z;
		case 10: return y - z;
		default: return -y - z;
	}
}

static float fade_curve(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

// Gradient noise in 3D, about -1..1
static float noise3(vec_t p, uint32_t seed)
{
	float fx = floorf(p.x);
	float fy = floorf(p.y);
	float fz = floorf(p.z);
	int x = (int)fx;
	int y = (int)fy;
	int z = (int)fz;
	float dx = p.x - fx;
	float dy = p.y - fy;
	float dz = p.z - fz;
	float u = fade_curve(dx);
	float v = fade_curve(dy);
	float w = fade_curve(dz);
	float corners[8];

	for (int c = 0; c < 8; c++)
	{
		int ox = c & 1;
		int oy = (c >> 1) & 1;
		int oz = (c >> 2) & 1;

		corners[c] = gradient_dot(hash_cell(x + ox, y + oy, z + oz, seed), dx - (float)ox, dy - (float)oy, dz - (float)oz);
	}
	return lerp(lerp(lerp(corners[0], corners[1], u), lerp(corners[2], corners[3], u), v),
		lerp(lerp(corners[4], corners[5], u), lerp(corners[6], corners[7], u), v), w);
}

static float fractal3(vec_t p, int octaves, uint32_t seed)
{
	float sum = 0.0f;
	float amplitude = 0.5f;

	for (int o = 0; o < octaves; o++)
	{
		sum += amplitude * noise3(p, seed + (uint32_t)o * 101u);
		p = scale(p, 2.03f);
		amplitude *= 0.5f;
	}
	return sum * 1.6f;
}

// ---------------------------------------------------------------------------
// Stones

typedef struct
{
	vec_t center;
	vec_t axes[3];                       // its own x, y, z in the model's
	vec_t half;                          // its half sizes
	float boxiness;
	float lumps;                         // meters
	float lump_size;
	float grain;                         // meters
	float bottom;                        // its foot cut flat here (its own y)
	int facet_count;
	vec_t facet_normals[MAX_FACETS];
	float facet_offsets[MAX_FACETS];
	float layers;                        // meters deep, the grooves
	float layer_spacing;
	vec_t layer_direction;
	int hole_count;
	vec_t hole_starts[MAX_HOLES];
	vec_t hole_ends[MAX_HOLES];
	float hole_radii[MAX_HOLES];
	uint32_t seed;
	float bound;                         // all of it within this of its center
} stone_t;

typedef struct
{
	stone_t *stones;
	int count;
} scene_t;

static float segment_distance(vec_t p, vec_t a, vec_t b)
{
	vec_t along = sub(b, a);
	float t = clamp_float(dot(sub(p, a), along) / maximum_of(dot(along, along), 1e-9f), 0.0f, 1.0f);

	return length_of(sub(p, add(a, scale(along, t))));
}

static float stone_distance(const stone_t *stone, vec_t p)
{
	vec_t from = sub(p, stone->center);
	vec_t q = vec(dot(from, stone->axes[0]), dot(from, stone->axes[1]), dot(from, stone->axes[2]));
	float smallest = minimum_of(stone->half.x, minimum_of(stone->half.y, stone->half.z));
	float distance;

	// Between an ellipsoid and a rounded block
	{
		vec_t k = vec(q.x / stone->half.x, q.y / stone->half.y, q.z / stone->half.z);
		float ellipsoid = (length_of(k) - 1.0f) * smallest;
		float rounding = smallest * 0.3f;
		vec_t outside = vec(fabsf(q.x) - (stone->half.x - rounding), fabsf(q.y) - (stone->half.y - rounding),
			fabsf(q.z) - (stone->half.z - rounding));
		vec_t positive = vec(maximum_of(outside.x, 0.0f), maximum_of(outside.y, 0.0f), maximum_of(outside.z, 0.0f));
		float block = length_of(positive) + minimum_of(maximum_of(outside.x, maximum_of(outside.y, outside.z)), 0.0f) - rounding;

		distance = lerp(ellipsoid, block, stone->boxiness);
	}
	if (stone->lumps > 0.0f)
	{
		distance += stone->lumps * fractal3(scale(q, 1.0f / stone->lump_size), 4, stone->seed);
	}
	for (int f = 0; f < stone->facet_count; f++)
	{
		distance = maximum_of(distance, dot(q, stone->facet_normals[f]) - stone->facet_offsets[f]);
	}
	distance = maximum_of(distance, stone->bottom - q.y);
	if (stone->layers > 0.0f)
	{
		float s = dot(q, stone->layer_direction) / stone->layer_spacing + 0.25f * noise3(scale(q, 1.5f / stone->layer_spacing), stone->seed + 7u);
		float across = fabsf(s - floorf(s) - 0.5f) * 2.0f;
		float groove = across * across;

		groove *= groove;
		distance += stone->layers * groove * groove;
	}
	for (int h = 0; h < stone->hole_count; h++)
	{
		float radius = stone->hole_radii[h] * (1.0f + 0.25f * noise3(scale(q, 3.0f / stone->lump_size), stone->seed + 13u));

		distance = maximum_of(distance, radius - segment_distance(q, stone->hole_starts[h], stone->hole_ends[h]));
	}
	if (stone->grain > 0.0f)
	{
		distance += stone->grain * fractal3(scale(q, 1.0f / maximum_of(stone->lump_size * 0.12f, 0.01f)), 3, stone->seed + 29u);
	}
	return distance;
}

// The nearest stone's distance; beyond give_up only roughly (the stones'
// bounds), which is enough there
static float scene_distance(const scene_t *scene, vec_t p, float give_up)
{
	float best = 1e9f;

	for (int s = 0; s < scene->count; s++)
	{
		const stone_t *stone = &scene->stones[s];
		float bound = length_of(sub(p, stone->center)) - stone->bound;

		if (bound >= best)
		{
			continue;
		}
		if (bound > give_up)
		{
			best = bound;
			continue;
		}
		best = minimum_of(best, stone_distance(stone, p));
	}
	return best;
}

// How far it reaches along a direction (its own axes), roughly
static float stone_support(const stone_t *stone, vec_t direction)
{
	float round = sqrtf(powf(stone->half.x * direction.x, 2.0f) + powf(stone->half.y * direction.y, 2.0f)
		+ powf(stone->half.z * direction.z, 2.0f));
	float block = fabsf(stone->half.x * direction.x) + fabsf(stone->half.y * direction.y) + fabsf(stone->half.z * direction.z);

	return lerp(round, block, stone->boxiness * 0.6f);
}

static vec_t direction_of(float yaw, float elevation)
{
	return vec(cosf(elevation) * cosf(yaw), sinf(elevation), cosf(elevation) * sinf(yaw));
}

// Its own axes: turned round the vertical, leaning a little
static void stone_turn(stone_t *stone, float yaw, float tilt_x, float tilt_z)
{
	vec_t x = vec(cosf(yaw), 0.0f, -sinf(yaw));
	vec_t z = vec(sinf(yaw), 0.0f, cosf(yaw));
	vec_t y = vec(0.0f, 1.0f, 0.0f);

	// (small leans: turned about its own x, then its z)
	y = normalized(add(y, add(scale(z, tilt_x), scale(x, -tilt_z))));
	z = normalized(cross(x, y));
	x = normalized(cross(y, z));
	stone->axes[0] = x;
	stone->axes[1] = y;
	stone->axes[2] = z;
}

// A stone of the recipe's shape, its half sizes given
static void stone_make(stone_t *stone, const prop_recipe_t *recipe, vec_t half, random_t *random, float detail)
{
	float smallest = minimum_of(half.x, minimum_of(half.y, half.z));
	float largest = maximum_of(half.x, maximum_of(half.y, half.z));

	memset(stone, 0, sizeof(*stone));
	stone->half = half;
	stone->axes[0] = vec(1.0f, 0.0f, 0.0f);
	stone->axes[1] = vec(0.0f, 1.0f, 0.0f);
	stone->axes[2] = vec(0.0f, 0.0f, 1.0f);
	stone->boxiness = clamp_float(recipe->boxiness, 0.0f, 1.0f);
	stone->lump_size = maximum_of(minimum_of(recipe->lump_size, largest * 2.0f), 0.02f);
	stone->lumps = clamp_float(recipe->lumps, 0.0f, 1.0f) * smallest;
	stone->grain = clamp_float(recipe->grain, 0.0f, 1.0f) * minimum_of(smallest * 0.05f, 0.03f) * detail;
	stone->seed = random_next(random);
	stone->bottom = recipe->flat_bottom > 0.0f ? -half.y * (1.0f - clamp_float(recipe->flat_bottom, 0.0f, 1.0f)) : -1e9f;
	stone->facet_count = recipe->facets < 0 ? 0 : (recipe->facets > MAX_FACETS ? MAX_FACETS : recipe->facets);
	for (int f = 0; f < stone->facet_count; f++)
	{
		vec_t normal = direction_of(random_between(random, 0.0f, 2.0f * PROP_PI), random_between(random, -0.35f, 1.2f));

		stone->facet_normals[f] = normal;
		stone->facet_offsets[f] = stone_support(stone, normal) * (1.0f - clamp_float(recipe->facet_depth, 0.0f, 0.9f)
			* random_between(random, 0.3f, 1.0f));
	}
	if (recipe->layers > 0.0f && recipe->layer_spacing > 0.005f)
	{
		float tilt = recipe->layer_tilt * PROP_PI / 180.0f;
		float toward = random_between(random, 0.0f, 2.0f * PROP_PI);

		stone->layer_spacing = recipe->layer_spacing;
		stone->layers = clamp_float(recipe->layers, 0.0f, 1.0f) * recipe->layer_spacing * 0.45f;
		stone->layer_direction = normalized(vec(sinf(tilt) * cosf(toward), cosf(tilt), sinf(tilt) * sinf(toward)));
	}
	stone->hole_count = recipe->holes < 0 ? 0 : (recipe->holes > MAX_HOLES ? MAX_HOLES : recipe->holes);
	for (int h = 0; h < stone->hole_count; h++)
	{
		vec_t direction = direction_of(random_between(random, 0.0f, 2.0f * PROP_PI), random_between(random, -0.4f, 0.4f));
		vec_t middle = vec(random_between(random, -0.3f, 0.3f) * half.x, random_between(random, -0.15f, 0.35f) * half.y,
			random_between(random, -0.3f, 0.3f) * half.z);
		// One in three a pit, not through
		bool through = random_unit(random) > 0.33f;

		stone->hole_starts[h] = through ? sub(middle, scale(direction, largest * 2.5f)) : middle;
		stone->hole_ends[h] = add(middle, scale(direction, largest * 2.5f));
		stone->hole_radii[h] = clamp_float(recipe->hole_size, 0.05f, 0.8f) * smallest * random_between(random, 0.7f, 1.15f);
	}
	stone->bound = length_of(half) + stone->lumps * 1.2f + stone->grain * 2.0f + smallest * 0.05f;
}

// ---------------------------------------------------------------------------
// The mesh

static bool mesh_vertex(prop_mesh_t *mesh, vec_t position, vec_t normal, float u, float v, const float tangent[4])
{
	object_file_vertex_t *vertex;

	if (mesh->vertex_count == mesh->vertex_capacity)
	{
		size_t capacity = mesh->vertex_capacity > 0 ? mesh->vertex_capacity * 2 : 4096;
		object_file_vertex_t *grown = realloc(mesh->vertices, sizeof(object_file_vertex_t) * capacity);

		if (grown == NULL)
		{
			return false;
		}
		mesh->vertices = grown;
		mesh->vertex_capacity = capacity;
	}
	vertex = &mesh->vertices[mesh->vertex_count++];
	vertex->position[0] = position.x;
	vertex->position[1] = position.y;
	vertex->position[2] = position.z;
	vertex->normal[0] = normal.x;
	vertex->normal[1] = normal.y;
	vertex->normal[2] = normal.z;
	vertex->uv[0] = u;
	vertex->uv[1] = v;
	memcpy(vertex->tangent, tangent, sizeof(float) * 4);
	return true;
}

static bool mesh_triangle(prop_mesh_t *mesh, uint32_t a, uint32_t b, uint32_t c)
{
	if (mesh->index_count + 3 > mesh->index_capacity)
	{
		size_t capacity = mesh->index_capacity > 0 ? mesh->index_capacity * 2 : 8192;
		uint32_t *grown = realloc(mesh->indices, sizeof(uint32_t) * capacity);

		if (grown == NULL)
		{
			return false;
		}
		mesh->indices = grown;
		mesh->index_capacity = capacity;
	}
	mesh->indices[mesh->index_count++] = a;
	mesh->indices[mesh->index_count++] = b;
	mesh->indices[mesh->index_count++] = c;
	return true;
}

static vec_t vertex_position(const prop_mesh_t *mesh, uint32_t index)
{
	const float *p = mesh->vertices[index].position;

	return vec(p[0], p[1], p[2]);
}

static void mesh_bounds(prop_mesh_t *mesh)
{
	for (int k = 0; k < 3; k++)
	{
		mesh->minimum[k] = mesh->vertex_count > 0 ? 1e9f : 0.0f;
		mesh->maximum[k] = mesh->vertex_count > 0 ? -1e9f : 0.0f;
	}
	for (size_t i = 0; i < mesh->vertex_count; i++)
	{
		for (int k = 0; k < 3; k++)
		{
			mesh->minimum[k] = minimum_of(mesh->minimum[k], mesh->vertices[i].position[k]);
			mesh->maximum[k] = maximum_of(mesh->maximum[k], mesh->vertices[i].position[k]);
		}
	}
}

// The next material: its indices start here
static prop_material_t *material_begin(prop_mesh_t *mesh, const char *name)
{
	prop_material_t *material = &mesh->materials[mesh->material_count < PROP_MAX_MATERIALS ? mesh->material_count++ : PROP_MAX_MATERIALS - 1];

	memset(material, 0, sizeof(*material));
	snprintf(material->name, sizeof(material->name), "%s", name);
	material->first_index = mesh->index_count;
	material->texture_meters = 1.0f;
	for (int k = 0; k < 3; k++)
	{
		material->base_color[k] = 1.0f;
	}
	material->metallic = 1.0f;
	material->roughness = 1.0f;
	return material;
}

// What the material's indices so far are: its own (level 0), or a level's
// (from the end of the level before)
static void material_level_done(const prop_mesh_t *mesh, prop_material_t *material, int level)
{
	size_t done = material->first_index + material->own_index_count;

	if (level == 0)
	{
		material->own_index_count = mesh->index_count - material->first_index;
		return;
	}
	for (int k = 0; k < level - 1; k++)
	{
		done += material->lod_index_count[k];
	}
	material->lod_index_count[level - 1] = mesh->index_count - done;
}

size_t prop_mesh_triangles(const prop_mesh_t *mesh, int level)
{
	size_t total = 0;

	for (int m = 0; m < mesh->material_count; m++)
	{
		const prop_material_t *material = &mesh->materials[m];

		total += (level > 0 && level <= 3 && material->lod_index_count[0] > 0 ? material->lod_index_count[level - 1] : material->own_index_count) / 3;
	}
	return total;
}

bool prop_mesh_has_levels(const prop_mesh_t *mesh)
{
	for (int m = 0; m < mesh->material_count; m++)
	{
		if (mesh->materials[m].lod_index_count[0] > 0)
		{
			return true;
		}
	}
	return false;
}

void prop_mesh_free(prop_mesh_t *mesh)
{
	free(mesh->vertices);
	free(mesh->indices);
	memset(mesh, 0, sizeof(*mesh));
}

// ---------------------------------------------------------------------------
// The distances on a grid, and the surface through them

typedef struct
{
	int counts[3];                       // nodes along x, y, z
	vec_t origin;                        // the first node
	float cell;                          // meters between nodes
	float *values;
} grid_t;

static float grid_at(const grid_t *grid, int x, int y, int z)
{
	return grid->values[((size_t)z * (size_t)grid->counts[1] + (size_t)y) * (size_t)grid->counts[0] + (size_t)x];
}

static vec_t grid_point(const grid_t *grid, int x, int y, int z)
{
	return add(grid->origin, vec((float)x * grid->cell, (float)y * grid->cell, (float)z * grid->cell));
}

// The distance anywhere, from the grid's (outside it: far, open)
static float grid_sample(const grid_t *grid, vec_t p)
{
	float coordinates[3] =
	{
		(p.x - grid->origin.x) / grid->cell, (p.y - grid->origin.y) / grid->cell, (p.z - grid->origin.z) / grid->cell,
	};
	int base[3];
	float within[3];

	for (int k = 0; k < 3; k++)
	{
		float limit = (float)(grid->counts[k] - 1);

		if (coordinates[k] < 0.0f || coordinates[k] >= limit)
		{
			float beyond = coordinates[k] < 0.0f ? -coordinates[k] : coordinates[k] - limit;

			return (beyond + 1.0f) * grid->cell;
		}
		base[k] = (int)coordinates[k];
		within[k] = coordinates[k] - (float)base[k];
	}
	{
		float c[8];

		for (int i = 0; i < 8; i++)
		{
			c[i] = grid_at(grid, base[0] + (i & 1), base[1] + ((i >> 1) & 1), base[2] + ((i >> 2) & 1));
		}
		return lerp(lerp(lerp(c[0], c[1], within[0]), lerp(c[2], c[3], within[0]), within[1]),
			lerp(lerp(c[4], c[5], within[0]), lerp(c[6], c[7], within[0]), within[1]), within[2]);
	}
}

// resolution: cells along its longest side; or, a wall's, along a stone
// (stone_size: its size)
static bool grid_fill(grid_t *grid, const scene_t *scene, int resolution, float stone_size)
{
	vec_t low = vec(1e9f, 1e9f, 1e9f);
	vec_t high = vec(-1e9f, -1e9f, -1e9f);
	float longest;

	for (int s = 0; s < scene->count; s++)
	{
		const stone_t *stone = &scene->stones[s];

		low = vec(minimum_of(low.x, stone->center.x - stone->bound), minimum_of(low.y, stone->center.y - stone->bound),
			minimum_of(low.z, stone->center.z - stone->bound));
		high = vec(maximum_of(high.x, stone->center.x + stone->bound), maximum_of(high.y, stone->center.y + stone->bound),
			maximum_of(high.z, stone->center.z + stone->bound));
	}
	longest = maximum_of(high.x - low.x, maximum_of(high.y - low.y, high.z - low.z));
	if (stone_size > 0.0f)
	{
		grid->cell = stone_size / (float)(resolution < 3 ? 3 : (resolution > 40 ? 40 : resolution));
		grid->cell = maximum_of(grid->cell, longest / 600.0f);
	}
	else
	{
		grid->cell = longest / (float)(resolution < 8 ? 8 : (resolution > 400 ? 400 : resolution));
	}
	grid->origin = sub(low, vec(grid->cell * 2.0f, grid->cell * 2.0f, grid->cell * 2.0f));
	grid->counts[0] = (int)ceilf((high.x - low.x) / grid->cell) + 5;
	grid->counts[1] = (int)ceilf((high.y - low.y) / grid->cell) + 5;
	grid->counts[2] = (int)ceilf((high.z - low.z) / grid->cell) + 5;
	grid->values = malloc(sizeof(float) * (size_t)grid->counts[0] * (size_t)grid->counts[1] * (size_t)grid->counts[2]);
	if (grid->values == NULL)
	{
		prop_error("No memory for the grid (%d x %d x %d)", grid->counts[0], grid->counts[1], grid->counts[2]);
		return false;
	}
	for (int z = 0; z < grid->counts[2]; z++)
	{
		for (int y = 0; y < grid->counts[1]; y++)
		{
			for (int x = 0; x < grid->counts[0]; x++)
			{
				grid->values[((size_t)z * (size_t)grid->counts[1] + (size_t)y) * (size_t)grid->counts[0] + (size_t)x] =
					scene_distance(scene, grid_point(grid, x, y, z), grid->cell * 3.0f);
			}
		}
	}
	return true;
}

static const float no_tangent[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

// The surface (surface nets): a vertex in each cell it passes through, two
// triangles across each edge it crosses, facing out
static bool surface_nets(const grid_t *grid, prop_mesh_t *mesh)
{
	int cells[3] = { grid->counts[0] - 1, grid->counts[1] - 1, grid->counts[2] - 1 };
	size_t cell_total = (size_t)cells[0] * (size_t)cells[1] * (size_t)cells[2];
	int32_t *cell_vertex = malloc(sizeof(int32_t) * cell_total);

	if (cell_vertex == NULL)
	{
		prop_error("No memory for the surface");
		return false;
	}
	for (int z = 0; z < cells[2]; z++)
	{
		for (int y = 0; y < cells[1]; y++)
		{
			for (int x = 0; x < cells[0]; x++)
			{
				float corner[8];
				int inside = 0;
				vec_t sum = vec(0.0f, 0.0f, 0.0f);
				int crossings = 0;
				size_t index = ((size_t)z * (size_t)cells[1] + (size_t)y) * (size_t)cells[0] + (size_t)x;

				cell_vertex[index] = -1;
				for (int c = 0; c < 8; c++)
				{
					corner[c] = grid_at(grid, x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1));
					inside += corner[c] < 0.0f ? 1 : 0;
				}
				if (inside == 0 || inside == 8)
				{
					continue;
				}
				for (int c = 0; c < 8; c++)
				{
					for (int bit = 1; bit <= 4; bit <<= 1)
					{
						int other = c | bit;

						if ((c & bit) != 0 || (corner[c] < 0.0f) == (corner[other] < 0.0f))
						{
							continue;
						}
						{
							float t = corner[c] / (corner[c] - corner[other]);
							vec_t a = vec((float)(c & 1), (float)((c >> 1) & 1), (float)((c >> 2) & 1));
							vec_t b = vec((float)(other & 1), (float)((other >> 1) & 1), (float)((other >> 2) & 1));

							sum = add(sum, add(a, scale(sub(b, a), t)));
							crossings++;
						}
					}
				}
				sum = scale(sum, 1.0f / (float)crossings);
				if (!mesh_vertex(mesh, add(grid_point(grid, x, y, z), scale(sum, grid->cell)), vec(0.0f, 1.0f, 0.0f), 0.0f, 0.0f, no_tangent))
				{
					free(cell_vertex);
					prop_error("No memory for the surface");
					return false;
				}
				cell_vertex[index] = (int32_t)(mesh->vertex_count - 1);
			}
		}
	}
	for (int z = 0; z < grid->counts[2]; z++)
	{
		for (int y = 0; y < grid->counts[1]; y++)
		{
			for (int x = 0; x < grid->counts[0]; x++)
			{
				int node[3] = { x, y, z };
				float here = grid_at(grid, x, y, z);

				for (int axis = 0; axis < 3; axis++)
				{
					int b = (axis + 1) % 3;
					int c = (axis + 2) % 3;
					int next[3] = { x, y, z };
					int32_t quad[4];
					bool ok = true;
					vec_t outward = vec(0.0f, 0.0f, 0.0f);

					next[axis]++;
					if (next[axis] >= grid->counts[axis] || node[b] < 1 || node[c] < 1 || node[b] > cells[b] - 1 || node[c] > cells[c] - 1)
					{
						continue;
					}
					if ((here < 0.0f) == (grid_at(grid, next[0], next[1], next[2]) < 0.0f))
					{
						continue;
					}
					for (int q = 0; q < 4; q++)
					{
						int cell[3] = { x, y, z };
						size_t index;

						cell[b] -= (q == 1 || q == 2) ? 1 : 0;
						cell[c] -= (q == 2 || q == 3) ? 1 : 0;
						index = ((size_t)cell[2] * (size_t)cells[1] + (size_t)cell[1]) * (size_t)cells[0] + (size_t)cell[0];
						quad[q] = cell_vertex[index];
						ok = ok && quad[q] >= 0;
					}
					if (!ok)
					{
						continue;
					}
					// Out is from inside to outside along the edge
					if (axis == 0)
					{
						outward.x = here < 0.0f ? 1.0f : -1.0f;
					}
					else if (axis == 1)
					{
						outward.y = here < 0.0f ? 1.0f : -1.0f;
					}
					else
					{
						outward.z = here < 0.0f ? 1.0f : -1.0f;
					}
					{
						// Split along the shorter diagonal
						vec_t p[4];
						int order[2][3];

						for (int q = 0; q < 4; q++)
						{
							p[q] = vertex_position(mesh, (uint32_t)quad[q]);
						}
						if (length_of(sub(p[0], p[2])) <= length_of(sub(p[1], p[3])))
						{
							int split[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };

							memcpy(order, split, sizeof(order));
						}
						else
						{
							int split[2][3] = { { 0, 1, 3 }, { 1, 2, 3 } };

							memcpy(order, split, sizeof(order));
						}
						for (int t = 0; t < 2; t++)
						{
							int i0 = order[t][0];
							int i1 = order[t][1];
							int i2 = order[t][2];
							vec_t normal = cross(sub(p[i1], p[i0]), sub(p[i2], p[i0]));

							if (dot(normal, normal) < 1e-14f)
							{
								continue;
							}
							if (dot(normal, outward) < 0.0f)
							{
								int swap = i1;

								i1 = i2;
								i2 = swap;
							}
							if (!mesh_triangle(mesh, (uint32_t)quad[i0], (uint32_t)quad[i1], (uint32_t)quad[i2]))
							{
								free(cell_vertex);
								prop_error("No memory for the surface");
								return false;
							}
						}
					}
				}
			}
		}
	}
	free(cell_vertex);
	return true;
}

static uint32_t find_root(uint32_t *parents, uint32_t i)
{
	while (parents[i] != i)
	{
		parents[i] = parents[parents[i]];
		i = parents[i];
	}
	return i;
}

// Crumbs (bits of noise floating free) go: pieces of fewer triangles than
// a small share of the whole; then the vertices no triangle uses
static void drop_crumbs(prop_mesh_t *mesh)
{
	uint32_t *parents = malloc(sizeof(uint32_t) * (mesh->vertex_count + 1));
	uint32_t *counts = calloc(mesh->vertex_count + 1, sizeof(uint32_t));
	int32_t *renumber = malloc(sizeof(int32_t) * (mesh->vertex_count + 1));
	size_t triangle_count = mesh->index_count / 3;
	size_t kept = 0;
	size_t vertices_kept = 0;
	uint32_t smallest_kept = (uint32_t)maximum_of(24.0f, (float)triangle_count * 0.004f);

	if (parents == NULL || counts == NULL || renumber == NULL)
	{
		free(parents);
		free(counts);
		free(renumber);
		return;
	}
	for (uint32_t i = 0; i < (uint32_t)mesh->vertex_count; i++)
	{
		parents[i] = i;
		renumber[i] = -1;
	}
	for (size_t t = 0; t < triangle_count; t++)
	{
		uint32_t a = find_root(parents, mesh->indices[t * 3]);

		for (int k = 1; k < 3; k++)
		{
			uint32_t b = find_root(parents, mesh->indices[t * 3 + (size_t)k]);

			if (a != b)
			{
				parents[b] = a;
			}
		}
	}
	for (size_t t = 0; t < triangle_count; t++)
	{
		counts[find_root(parents, mesh->indices[t * 3])]++;
	}
	for (size_t t = 0; t < triangle_count; t++)
	{
		if (counts[find_root(parents, mesh->indices[t * 3])] < smallest_kept)
		{
			continue;
		}
		memmove(&mesh->indices[kept * 3], &mesh->indices[t * 3], sizeof(uint32_t) * 3);
		kept++;
	}
	mesh->index_count = kept * 3;
	// (in order: a vertex moves only down, onto one already moved)
	for (size_t i = 0; i < mesh->index_count; i++)
	{
		renumber[mesh->indices[i]] = 0;
	}
	for (size_t v = 0; v < mesh->vertex_count; v++)
	{
		if (renumber[v] >= 0)
		{
			renumber[v] = (int32_t)vertices_kept;
			mesh->vertices[vertices_kept++] = mesh->vertices[v];
		}
	}
	for (size_t i = 0; i < mesh->index_count; i++)
	{
		mesh->indices[i] = (uint32_t)renumber[mesh->indices[i]];
	}
	mesh->vertex_count = vertices_kept;
	free(parents);
	free(counts);
	free(renumber);
}

// A little smoother (the cells' steps): each vertex a part of the way to
// its neighbours' middle, then a part back (Taubin: it does not shrink)
static void smooth_surface(prop_mesh_t *mesh, int rounds)
{
	vec_t *sums = malloc(sizeof(vec_t) * (mesh->vertex_count + 1));
	float *counts = malloc(sizeof(float) * (mesh->vertex_count + 1));

	if (sums == NULL || counts == NULL)
	{
		free(sums);
		free(counts);
		return;
	}
	for (int r = 0; r < rounds * 2; r++)
	{
		float factor = (r & 1) == 0 ? 0.5f : -0.53f;

		memset(sums, 0, sizeof(vec_t) * mesh->vertex_count);
		memset(counts, 0, sizeof(float) * mesh->vertex_count);
		for (size_t t = 0; t < mesh->index_count; t += 3)
		{
			for (int k = 0; k < 3; k++)
			{
				uint32_t a = mesh->indices[t + (size_t)k];
				uint32_t b = mesh->indices[t + (size_t)((k + 1) % 3)];

				sums[a] = add(sums[a], vertex_position(mesh, b));
				counts[a] += 1.0f;
				sums[b] = add(sums[b], vertex_position(mesh, a));
				counts[b] += 1.0f;
			}
		}
		for (size_t i = 0; i < mesh->vertex_count; i++)
		{
			if (counts[i] > 0.0f)
			{
				vec_t p = vertex_position(mesh, (uint32_t)i);
				vec_t moved = add(p, scale(sub(scale(sums[i], 1.0f / counts[i]), p), factor));

				mesh->vertices[i].position[0] = moved.x;
				mesh->vertices[i].position[1] = moved.y;
				mesh->vertices[i].position[2] = moved.z;
			}
		}
	}
	free(sums);
	free(counts);
}

// The vertices' normals: their triangles', by their areas
static void surface_normals(prop_mesh_t *mesh)
{
	vec_t *sums = calloc(mesh->vertex_count + 1, sizeof(vec_t));

	if (sums == NULL)
	{
		return;
	}
	for (size_t t = 0; t < mesh->index_count; t += 3)
	{
		uint32_t a = mesh->indices[t];
		uint32_t b = mesh->indices[t + 1];
		uint32_t c = mesh->indices[t + 2];
		vec_t pa = vertex_position(mesh, a);
		vec_t normal = cross(sub(vertex_position(mesh, b), pa), sub(vertex_position(mesh, c), pa));

		sums[a] = add(sums[a], normal);
		sums[b] = add(sums[b], normal);
		sums[c] = add(sums[c], normal);
	}
	for (size_t i = 0; i < mesh->vertex_count; i++)
	{
		vec_t normal = normalized(sums[i]);

		mesh->vertices[i].normal[0] = normal.x;
		mesh->vertices[i].normal[1] = normal.y;
		mesh->vertices[i].normal[2] = normal.z;
	}
	free(sums);
}

// The distance with the ground (y = ground) as a solid too
static float with_ground(const grid_t *grid, vec_t p, float ground)
{
	return minimum_of(grid_sample(grid, p), p.y - ground);
}

// What the weather finds at each vertex, into its tangent (object_format.h):
// x how hidden (in a crevice, or close by the ground), y how sharp an edge
// it is on, z how much of the sky it sees (1: all of it, as a flat top)
static void weathering(const grid_t *grid, prop_mesh_t *mesh, float ground)
{
	vec_t directions[SKY_DIRECTIONS];
	float step = maximum_of(grid->cell * 1.5f, 0.012f);
	float spacing = grid->cell * 1.5f;

	// Over the sky: spread evenly (a spiral), more of them up high
	for (int d = 0; d < SKY_DIRECTIONS; d++)
	{
		float height = 1.0f - ((float)d + 0.5f) / (float)SKY_DIRECTIONS;
		float round = sqrtf(maximum_of(1.0f - height * height, 0.0f));
		float turn = (float)d * 2.39996323f;

		directions[d] = vec(round * cosf(turn), height, round * sinf(turn));
	}
	for (size_t i = 0; i < mesh->vertex_count; i++)
	{
		object_file_vertex_t *vertex = &mesh->vertices[i];
		vec_t p = vec(vertex->position[0], vertex->position[1], vertex->position[2]);
		vec_t n = vec(vertex->normal[0], vertex->normal[1], vertex->normal[2]);
		float hidden = 0.0f;
		float weights = 0.0f;
		float weight = 1.0f;
		float curvature;
		float open = 0.0f;

		// Hidden: how much nearer than it should be the solid is, out along
		// the normal
		for (int k = 1; k <= 5; k++)
		{
			float h = step * (float)k;
			float found = with_ground(grid, add(p, scale(n, h)), ground);

			hidden += weight * clamp_float((h - found) / h, 0.0f, 1.0f);
			weights += weight;
			weight *= 0.8f;
		}
		hidden /= weights;
		// Sharp: the distances' curvature (a sphere of radius r: 2 / r)
		{
			float centre = grid_sample(grid, p);
			float sum = grid_sample(grid, add(p, vec(spacing, 0.0f, 0.0f))) + grid_sample(grid, sub(p, vec(spacing, 0.0f, 0.0f)))
				+ grid_sample(grid, add(p, vec(0.0f, spacing, 0.0f))) + grid_sample(grid, sub(p, vec(0.0f, spacing, 0.0f)))
				+ grid_sample(grid, add(p, vec(0.0f, 0.0f, spacing))) + grid_sample(grid, sub(p, vec(0.0f, 0.0f, spacing)));

			curvature = (sum - 6.0f * centre) / spacing;
		}
		// The sky: which of the directions over it are open
		for (int d = 0; d < SKY_DIRECTIONS; d++)
		{
			vec_t direction = directions[d];
			vec_t from = add(p, scale(n, grid->cell * 1.2f));
			float travelled = 0.0f;
			bool blocked = dot(direction, n) <= 0.02f;

			for (int s = 0; s < 64 && !blocked && travelled < 6.0f; s++)
			{
				float found = with_ground(grid, add(from, scale(direction, travelled)), ground);

				if (found < grid->cell * 0.25f)
				{
					blocked = true;
				}
				travelled += maximum_of(found * 0.9f, grid->cell * 0.5f);
			}
			open += blocked ? 0.0f : 1.0f;
		}
		vertex->tangent[0] = clamp_float(maximum_of(smooth_step(0.18f, 0.6f, hidden), smooth_step(0.15f, 0.8f, -curvature)), 0.0f, 1.0f);
		vertex->tangent[1] = smooth_step(0.2f, 0.9f, curvature);
		vertex->tangent[2] = open / (float)SKY_DIRECTIONS * 1.0f;
		vertex->tangent[3] = 1.0f;
	}
}

// ---------------------------------------------------------------------------
// Fewer triangles: edges collapsed where that changes the shape least
// (quadric error metrics, Garland and Heckbert; the passes with a rising
// threshold as in Sven Forstmann's Fast Quadric Mesh Simplification)

typedef struct
{
	double m[10];
} quadric_t;

typedef struct
{
	uint32_t v[3];
	double error[4];
	bool deleted;
	bool dirty;
	vec_t normal;
} collapse_triangle_t;

typedef struct
{
	vec_t p;
	quadric_t q;
	uint32_t first;                      // its references
	uint32_t count;
	bool border;
} collapse_vertex_t;

typedef struct
{
	uint32_t triangle;
	uint32_t corner;
} collapse_reference_t;

typedef struct
{
	collapse_triangle_t *triangles;
	size_t triangle_count;
	collapse_vertex_t *vertices;
	size_t vertex_count;
	collapse_reference_t *references;
	size_t reference_count;
	size_t reference_capacity;
} collapse_t;

static void quadric_add(quadric_t *to, const quadric_t *from)
{
	for (int k = 0; k < 10; k++)
	{
		to->m[k] += from->m[k];
	}
}

static double quadric_determinant(const quadric_t *q, int a11, int a12, int a13, int a21, int a22, int a23, int a31, int a32, int a33)
{
	const double *m = q->m;

	return m[a11] * m[a22] * m[a33] + m[a13] * m[a21] * m[a32] + m[a12] * m[a23] * m[a31] - m[a13] * m[a22] * m[a31]
		- m[a11] * m[a23] * m[a32] - m[a12] * m[a21] * m[a33];
}

static double quadric_error(const quadric_t *q, double x, double y, double z)
{
	const double *m = q->m;

	return m[0] * x * x + 2.0 * m[1] * x * y + 2.0 * m[2] * x * z + 2.0 * m[3] * x + m[4] * y * y + 2.0 * m[5] * y * z + 2.0 * m[6] * y
		+ m[7] * z * z + 2.0 * m[8] * z + m[9];
}

// The error of collapsing the edge, and where the vertex would go
static double collapse_error(const collapse_t *collapse, uint32_t a, uint32_t b, vec_t *place_out)
{
	quadric_t q = collapse->vertices[a].q;
	bool border = collapse->vertices[a].border && collapse->vertices[b].border;
	double determinant;

	quadric_add(&q, &collapse->vertices[b].q);
	determinant = quadric_determinant(&q, 0, 1, 2, 1, 4, 5, 2, 5, 7);
	if (fabs(determinant) > 1e-15 && !border)
	{
		double x = -1.0 / determinant * quadric_determinant(&q, 1, 2, 3, 4, 5, 6, 5, 7, 8);
		double y = 1.0 / determinant * quadric_determinant(&q, 0, 2, 3, 1, 5, 6, 2, 7, 8);
		double z = -1.0 / determinant * quadric_determinant(&q, 0, 1, 3, 1, 4, 6, 2, 5, 8);
		vec_t pa = collapse->vertices[a].p;
		vec_t pb = collapse->vertices[b].p;
		vec_t place = vec((float)x, (float)y, (float)z);

		// (not far off the edge: a nearly flat place has its best point
		// anywhere)
		if (length_of(sub(place, scale(add(pa, pb), 0.5f))) < length_of(sub(pa, pb)) * 2.0f)
		{
			*place_out = place;
			return quadric_error(&q, x, y, z);
		}
	}
	{
		vec_t pa = collapse->vertices[a].p;
		vec_t pb = collapse->vertices[b].p;
		vec_t middle = scale(add(pa, pb), 0.5f);
		double error_a = quadric_error(&q, pa.x, pa.y, pa.z);
		double error_b = quadric_error(&q, pb.x, pb.y, pb.z);
		double error_middle = quadric_error(&q, middle.x, middle.y, middle.z);
		double best = error_a < error_b ? (error_a < error_middle ? error_a : error_middle) : (error_b < error_middle ? error_b : error_middle);

		*place_out = best == error_a ? pa : (best == error_b ? pb : middle);
		return best;
	}
}

static void triangle_errors(collapse_t *collapse, collapse_triangle_t *triangle)
{
	vec_t unused;

	for (int j = 0; j < 3; j++)
	{
		triangle->error[j] = collapse_error(collapse, triangle->v[j], triangle->v[(j + 1) % 3], &unused);
	}
	triangle->error[3] = triangle->error[0] < triangle->error[1] ? triangle->error[0] : triangle->error[1];
	triangle->error[3] = triangle->error[3] < triangle->error[2] ? triangle->error[3] : triangle->error[2];
}

// Whether moving vertex a (its edge to b collapsing) to place folds one of
// its other triangles over (or makes it a sliver); which go away with the
// edge into gone
static bool collapse_flips(const collapse_t *collapse, vec_t place, uint32_t a, uint32_t b, bool *gone)
{
	const collapse_vertex_t *vertex = &collapse->vertices[a];

	for (uint32_t k = 0; k < vertex->count; k++)
	{
		const collapse_reference_t *reference = &collapse->references[vertex->first + k];
		const collapse_triangle_t *triangle = &collapse->triangles[reference->triangle];
		uint32_t id1;
		uint32_t id2;
		vec_t d1;
		vec_t d2;

		if (triangle->deleted)
		{
			continue;
		}
		id1 = triangle->v[(reference->corner + 1) % 3];
		id2 = triangle->v[(reference->corner + 2) % 3];
		if (id1 == b || id2 == b)
		{
			gone[k] = true;
			continue;
		}
		gone[k] = false;
		d1 = normalized(sub(collapse->vertices[id1].p, place));
		d2 = normalized(sub(collapse->vertices[id2].p, place));
		if (fabsf(dot(d1, d2)) > 0.999f)
		{
			return true;
		}
		if (dot(normalized(cross(d1, d2)), triangle->normal) < 0.2f)
		{
			return true;
		}
	}
	return false;
}

static bool reference_add(collapse_t *collapse, collapse_reference_t reference)
{
	if (collapse->reference_count == collapse->reference_capacity)
	{
		size_t capacity = collapse->reference_capacity * 2 + 64;
		collapse_reference_t *grown = realloc(collapse->references, sizeof(collapse_reference_t) * capacity);

		if (grown == NULL)
		{
			return false;
		}
		collapse->references = grown;
		collapse->reference_capacity = capacity;
	}
	collapse->references[collapse->reference_count++] = reference;
	return true;
}

// The vertex's triangles after the collapse: to the kept one, or gone
static bool collapse_update(collapse_t *collapse, uint32_t kept, const collapse_vertex_t *vertex, const bool *gone, size_t *deleted_count)
{
	for (uint32_t k = 0; k < vertex->count; k++)
	{
		collapse_reference_t reference = collapse->references[vertex->first + k];
		collapse_triangle_t *triangle = &collapse->triangles[reference.triangle];

		if (triangle->deleted)
		{
			continue;
		}
		if (gone[k])
		{
			triangle->deleted = true;
			(*deleted_count)++;
			continue;
		}
		triangle->v[reference.corner] = kept;
		triangle->dirty = true;
		triangle_errors(collapse, triangle);
		if (!reference_add(collapse, reference))
		{
			return false;
		}
	}
	return true;
}

// The references again (each vertex's triangles), the deleted triangles
// dropped; the first time the quadrics, the errors and the borders too
static bool collapse_refresh(collapse_t *collapse, int iteration)
{
	if (iteration > 0)
	{
		size_t kept = 0;

		for (size_t t = 0; t < collapse->triangle_count; t++)
		{
			if (!collapse->triangles[t].deleted)
			{
				collapse->triangles[kept++] = collapse->triangles[t];
			}
		}
		collapse->triangle_count = kept;
	}
	for (size_t v = 0; v < collapse->vertex_count; v++)
	{
		collapse->vertices[v].first = 0;
		collapse->vertices[v].count = 0;
	}
	for (size_t t = 0; t < collapse->triangle_count; t++)
	{
		for (int j = 0; j < 3; j++)
		{
			collapse->vertices[collapse->triangles[t].v[j]].count++;
		}
	}
	{
		uint32_t first = 0;

		for (size_t v = 0; v < collapse->vertex_count; v++)
		{
			collapse->vertices[v].first = first;
			first += collapse->vertices[v].count;
			collapse->vertices[v].count = 0;
		}
	}
	if (collapse->reference_capacity < collapse->triangle_count * 3)
	{
		collapse_reference_t *grown = realloc(collapse->references, sizeof(collapse_reference_t) * (collapse->triangle_count * 3 + 64));

		if (grown == NULL)
		{
			return false;
		}
		collapse->references = grown;
		collapse->reference_capacity = collapse->triangle_count * 3 + 64;
	}
	collapse->reference_count = collapse->triangle_count * 3;
	for (size_t t = 0; t < collapse->triangle_count; t++)
	{
		for (int j = 0; j < 3; j++)
		{
			collapse_vertex_t *vertex = &collapse->vertices[collapse->triangles[t].v[j]];
			collapse_reference_t *reference = &collapse->references[vertex->first + vertex->count++];

			reference->triangle = (uint32_t)t;
			reference->corner = (uint32_t)j;
		}
	}
	if (iteration == 0)
	{
		// The borders: edges only one triangle has (the surface is closed,
		// mostly; where it is not, its edge stays)
		for (size_t v = 0; v < collapse->vertex_count; v++)
		{
			const collapse_vertex_t *vertex = &collapse->vertices[v];
			uint32_t neighbours[64];
			uint32_t counts[64];
			int neighbour_count = 0;

			for (uint32_t k = 0; k < vertex->count; k++)
			{
				const collapse_triangle_t *triangle = &collapse->triangles[collapse->references[vertex->first + k].triangle];

				for (int j = 0; j < 3; j++)
				{
					uint32_t id = triangle->v[j];
					int found = -1;

					if (id == v)
					{
						continue;
					}
					for (int n = 0; n < neighbour_count; n++)
					{
						if (neighbours[n] == id)
						{
							found = n;
							break;
						}
					}
					if (found >= 0)
					{
						counts[found]++;
					}
					else if (neighbour_count < 64)
					{
						neighbours[neighbour_count] = id;
						counts[neighbour_count++] = 1;
					}
				}
			}
			for (int n = 0; n < neighbour_count; n++)
			{
				if (counts[n] == 1)
				{
					collapse->vertices[v].border = true;
					collapse->vertices[neighbours[n]].border = true;
				}
			}
		}
		for (size_t v = 0; v < collapse->vertex_count; v++)
		{
			memset(&collapse->vertices[v].q, 0, sizeof(quadric_t));
		}
		for (size_t t = 0; t < collapse->triangle_count; t++)
		{
			collapse_triangle_t *triangle = &collapse->triangles[t];
			vec_t p0 = collapse->vertices[triangle->v[0]].p;
			vec_t normal = normalized(cross(sub(collapse->vertices[triangle->v[1]].p, p0), sub(collapse->vertices[triangle->v[2]].p, p0)));
			double d = -(double)dot(normal, p0);
			quadric_t plane =
			{
				{
					(double)normal.x * normal.x, (double)normal.x * normal.y, (double)normal.x * normal.z, (double)normal.x * d,
					(double)normal.y * normal.y, (double)normal.y * normal.z, (double)normal.y * d,
					(double)normal.z * normal.z, (double)normal.z * d, d * d,
				}
			};

			triangle->normal = normal;
			for (int j = 0; j < 3; j++)
			{
				quadric_add(&collapse->vertices[triangle->v[j]].q, &plane);
			}
		}
		for (size_t t = 0; t < collapse->triangle_count; t++)
		{
			triangle_errors(collapse, &collapse->triangles[t]);
		}
	}
	return true;
}

// Down to about target triangles (or as near as it goes without spoiling
// the shape)
static void simplify(prop_mesh_t *mesh, size_t target)
{
	collapse_t collapse;
	size_t deleted_count = 0;
	bool *gone_a = NULL;
	bool *gone_b = NULL;
	size_t gone_capacity = 0;
	bool ok = true;

	if (mesh->index_count / 3 <= target)
	{
		return;
	}
	memset(&collapse, 0, sizeof(collapse));
	collapse.triangle_count = mesh->index_count / 3;
	collapse.vertex_count = mesh->vertex_count;
	collapse.triangles = calloc(collapse.triangle_count, sizeof(collapse_triangle_t));
	collapse.vertices = calloc(collapse.vertex_count, sizeof(collapse_vertex_t));
	if (collapse.triangles == NULL || collapse.vertices == NULL)
	{
		free(collapse.triangles);
		free(collapse.vertices);
		return;
	}
	for (size_t v = 0; v < collapse.vertex_count; v++)
	{
		collapse.vertices[v].p = vertex_position(mesh, (uint32_t)v);
	}
	for (size_t t = 0; t < collapse.triangle_count; t++)
	{
		memcpy(collapse.triangles[t].v, &mesh->indices[t * 3], sizeof(uint32_t) * 3);
	}
	for (int iteration = 0; iteration < 100 && ok; iteration++)
	{
		double threshold;
		size_t triangles_now;

		if (collapse.triangle_count - deleted_count <= target)
		{
			break;
		}
		if (iteration % 5 == 0)
		{
			ok = collapse_refresh(&collapse, iteration);
			deleted_count = 0;
			if (!ok)
			{
				break;
			}
		}
		for (size_t t = 0; t < collapse.triangle_count; t++)
		{
			collapse.triangles[t].dirty = false;
		}
		threshold = 1e-9 * pow((double)iteration + 3.0, 7.0);
		triangles_now = collapse.triangle_count;
		for (size_t t = 0; t < triangles_now && ok; t++)
		{
			collapse_triangle_t *triangle = &collapse.triangles[t];

			if (triangle->error[3] > threshold || triangle->deleted || triangle->dirty)
			{
				continue;
			}
			for (int j = 0; j < 3; j++)
			{
				uint32_t a = triangle->v[j];
				uint32_t b = triangle->v[(j + 1) % 3];
				collapse_vertex_t *va = &collapse.vertices[a];
				collapse_vertex_t *vb = &collapse.vertices[b];
				vec_t place;
				size_t first;
				size_t count;
				size_t needed;

				if (triangle->error[j] >= threshold || va->border != vb->border)
				{
					continue;
				}
				collapse_error(&collapse, a, b, &place);
				needed = va->count > vb->count ? va->count : vb->count;
				if (needed > gone_capacity)
				{
					bool *grown_a = realloc(gone_a, needed * 2);
					bool *grown_b = grown_a != NULL ? realloc(gone_b, needed * 2) : NULL;

					if (grown_a == NULL || grown_b == NULL)
					{
						gone_a = grown_a != NULL ? grown_a : gone_a;
						ok = false;
						break;
					}
					gone_a = grown_a;
					gone_b = grown_b;
					gone_capacity = needed * 2;
				}
				if (collapse_flips(&collapse, place, a, b, gone_a) || collapse_flips(&collapse, place, b, a, gone_b))
				{
					continue;
				}
				va->p = place;
				quadric_add(&va->q, &vb->q);
				first = collapse.reference_count;
				{
					// (vb's own copy: the references may move as they grow)
					collapse_vertex_t old_a = *va;
					collapse_vertex_t old_b = *vb;

					ok = collapse_update(&collapse, a, &old_a, gone_a, &deleted_count)
						&& collapse_update(&collapse, a, &old_b, gone_b, &deleted_count);
				}
				va = &collapse.vertices[a];
				count = collapse.reference_count - first;
				if (count <= va->count)
				{
					// Fits where its old ones were
					if (count > 0)
					{
						memmove(&collapse.references[va->first], &collapse.references[first], sizeof(collapse_reference_t) * count);
					}
					collapse.reference_count = first;
				}
				else
				{
					va->first = (uint32_t)first;
				}
				va->count = (uint32_t)count;
				break;
			}
			if (collapse.triangle_count - deleted_count <= target)
			{
				break;
			}
		}
	}
	// Back into the mesh: the triangles left, the vertices they use
	{
		int32_t *renumber = malloc(sizeof(int32_t) * collapse.vertex_count);
		size_t vertex_count = 0;
		size_t index_count = 0;

		if (renumber != NULL)
		{
			for (size_t v = 0; v < collapse.vertex_count; v++)
			{
				renumber[v] = -1;
			}
			for (size_t t = 0; t < collapse.triangle_count; t++)
			{
				const collapse_triangle_t *triangle = &collapse.triangles[t];

				if (triangle->deleted)
				{
					continue;
				}
				for (int j = 0; j < 3; j++)
				{
					uint32_t v = triangle->v[j];

					if (renumber[v] < 0)
					{
						object_file_vertex_t *vertex = &mesh->vertices[vertex_count];

						renumber[v] = (int32_t)vertex_count++;
						vertex->position[0] = collapse.vertices[v].p.x;
						vertex->position[1] = collapse.vertices[v].p.y;
						vertex->position[2] = collapse.vertices[v].p.z;
					}
					mesh->indices[index_count++] = (uint32_t)renumber[v];
				}
			}
			mesh->vertex_count = vertex_count;
			mesh->index_count = index_count;
			free(renumber);
		}
	}
	free(gone_a);
	free(gone_b);
	free(collapse.triangles);
	free(collapse.vertices);
	free(collapse.references);
}

// A level of detail's weathering from its own surface's (the full one's,
// level): each full vertex's marks to the level's vertex nearest it,
// averaged (the area it stands for), so the moss stays where it was as the
// levels change; a vertex none came to takes its nearest full one's
static void weathering_from(prop_mesh_t *made, const prop_mesh_t *full)
{
	float *sums = calloc(made->vertex_count * 4 + 4, sizeof(float));
	// (the crevices' most too: an average of a crevice and the stone round
	// it is no crevice, and the moss would go)
	float *most = calloc(made->vertex_count + 1, sizeof(float));

	if (sums == NULL || most == NULL)
	{
		free(sums);
		free(most);
		return;
	}
	for (size_t f = 0; f < full->vertex_count; f++)
	{
		vec_t p = vertex_position(full, (uint32_t)f);
		vec_t n = vec(full->vertices[f].normal[0], full->vertices[f].normal[1], full->vertices[f].normal[2]);
		size_t nearest = 0;
		float best = 1e30f;

		for (size_t m = 0; m < made->vertex_count; m++)
		{
			vec_t to = sub(vertex_position(made, (uint32_t)m), p);
			float distance = dot(to, to);
			// (not across a thin wall: the other side's vertex faces away)
			vec_t other = vec(made->vertices[m].normal[0], made->vertices[m].normal[1], made->vertices[m].normal[2]);

			distance *= dot(other, n) > 0.0f ? 1.0f : 16.0f;
			if (distance < best)
			{
				best = distance;
				nearest = m;
			}
		}
		for (int k = 0; k < 3; k++)
		{
			sums[nearest * 4 + (size_t)k] += full->vertices[f].tangent[k];
		}
		sums[nearest * 4 + 3] += 1.0f;
		most[nearest] = maximum_of(most[nearest], full->vertices[f].tangent[0]);
	}
	for (size_t m = 0; m < made->vertex_count; m++)
	{
		if (sums[m * 4 + 3] > 0.0f)
		{
			for (int k = 0; k < 3; k++)
			{
				made->vertices[m].tangent[k] = sums[m * 4 + (size_t)k] / sums[m * 4 + 3];
			}
			made->vertices[m].tangent[0] = lerp(made->vertices[m].tangent[0], most[m], 0.6f);
			made->vertices[m].tangent[3] = 1.0f;
			continue;
		}
		{
			vec_t p = vertex_position(made, (uint32_t)m);
			size_t nearest = 0;
			float best = 1e30f;

			for (size_t f = 0; f < full->vertex_count; f++)
			{
				vec_t to = sub(vertex_position(full, (uint32_t)f), p);

				if (dot(to, to) < best)
				{
					best = dot(to, to);
					nearest = f;
				}
			}
			memcpy(made->vertices[m].tangent, full->vertices[nearest].tangent, sizeof(float) * 4);
		}
	}
	free(sums);
	free(most);
}

static int compare_floats(const void *a, const void *b)
{
	float x = *(const float *)a;
	float y = *(const float *)b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

// How far off a mesh is from the surface (meters): at its corners, edges'
// middles and faces' middles, how far the real surface is, and how far its
// normals lean from the real ones (that much over its triangle: the light
// on it is off by as much as a bump that big). Not the worst one (a crumb
// would decide it), one in a hundred
static float surface_error(const grid_t *grid, const prop_mesh_t *mesh)
{
	size_t triangle_count = mesh->index_count / 3;
	float *errors = malloc(sizeof(float) * (triangle_count + 1));
	float error;

	if (errors == NULL || triangle_count == 0)
	{
		free(errors);
		return 0.0f;
	}
	for (size_t t = 0; t < triangle_count; t++)
	{
		vec_t p[3];
		vec_t n[3];
		float worst = 0.0f;
		float longest = 0.0f;

		for (int k = 0; k < 3; k++)
		{
			const object_file_vertex_t *vertex = &mesh->vertices[mesh->indices[t * 3 + (size_t)k]];

			p[k] = vec(vertex->position[0], vertex->position[1], vertex->position[2]);
			n[k] = vec(vertex->normal[0], vertex->normal[1], vertex->normal[2]);
		}
		for (int k = 0; k < 3; k++)
		{
			vec_t middle = scale(add(p[k], p[(k + 1) % 3]), 0.5f);

			worst = maximum_of(worst, fabsf(grid_sample(grid, p[k])));
			worst = maximum_of(worst, fabsf(grid_sample(grid, middle)));
			longest = maximum_of(longest, length_of(sub(p[k], p[(k + 1) % 3])));
		}
		{
			vec_t centre = scale(add(p[0], add(p[1], p[2])), 1.0f / 3.0f);
			vec_t normal = normalized(add(n[0], add(n[1], n[2])));
			float step = grid->cell;
			vec_t real = normalized(vec(grid_sample(grid, add(centre, vec(step, 0.0f, 0.0f))) - grid_sample(grid, sub(centre, vec(step, 0.0f, 0.0f))),
				grid_sample(grid, add(centre, vec(0.0f, step, 0.0f))) - grid_sample(grid, sub(centre, vec(0.0f, step, 0.0f))),
				grid_sample(grid, add(centre, vec(0.0f, 0.0f, step))) - grid_sample(grid, sub(centre, vec(0.0f, 0.0f, step)))));

			worst = maximum_of(worst, fabsf(grid_sample(grid, centre)));
			worst = maximum_of(worst, length_of(sub(normal, real)) * longest * 0.25f);
		}
		errors[t] = worst;
	}
	qsort(errors, triangle_count, sizeof(float), compare_floats);
	error = errors[(triangle_count - 1) * 99 / 100];
	free(errors);
	return error;
}

// A copy of a mesh's vertices and indices (the rest left out)
static bool mesh_copy(const prop_mesh_t *from, prop_mesh_t *to)
{
	memset(to, 0, sizeof(*to));
	to->vertices = malloc(sizeof(object_file_vertex_t) * (from->vertex_count + 1));
	to->indices = malloc(sizeof(uint32_t) * (from->index_count + 1));
	if (to->vertices == NULL || to->indices == NULL)
	{
		prop_mesh_free(to);
		return false;
	}
	memcpy(to->vertices, from->vertices, sizeof(object_file_vertex_t) * from->vertex_count);
	memcpy(to->indices, from->indices, sizeof(uint32_t) * from->index_count);
	to->vertex_count = to->vertex_capacity = from->vertex_count;
	to->index_count = to->index_capacity = from->index_count;
	return true;
}

// A level's mesh after the others: its vertices numbered after theirs
static bool mesh_append(prop_mesh_t *mesh, const prop_mesh_t *level)
{
	uint32_t base = (uint32_t)mesh->vertex_count;

	for (size_t v = 0; v < level->vertex_count; v++)
	{
		const object_file_vertex_t *vertex = &level->vertices[v];

		if (!mesh_vertex(mesh, vec(vertex->position[0], vertex->position[1], vertex->position[2]),
			vec(vertex->normal[0], vertex->normal[1], vertex->normal[2]), vertex->uv[0], vertex->uv[1], vertex->tangent))
		{
			return false;
		}
	}
	for (size_t i = 0; i + 2 < level->index_count; i += 3)
	{
		if (!mesh_triangle(mesh, base + level->indices[i], base + level->indices[i + 1], base + level->indices[i + 2]))
		{
			return false;
		}
	}
	return true;
}

// The stones' surface into the mesh: their foot sink deep in the ground
// (y 0), the weather's marks on it; and its levels of detail, each
// simplified from the same surface, with marks of its own
static bool carve(const scene_t *scene, const prop_recipe_t *recipe, float stone_size, prop_mesh_t *mesh)
{
	grid_t grid;
	prop_mesh_t surface;
	prop_mesh_t own;                     // its own level, done (the others' weathering)
	float shift = 0.0f;
	size_t targets[4];
	size_t previous_count = 0;
	float previous_error = 0.0f;
	prop_material_t *material = material_begin(mesh, "stone");
	bool ok = true;

	memset(&grid, 0, sizeof(grid));
	memset(&surface, 0, sizeof(surface));
	memset(&own, 0, sizeof(own));
	if (!grid_fill(&grid, scene, recipe->resolution, stone_size) || !surface_nets(&grid, &surface))
	{
		free(grid.values);
		prop_mesh_free(&surface);
		return false;
	}
	drop_crumbs(&surface);
	smooth_surface(&surface, 2);
	if (surface.index_count == 0)
	{
		free(grid.values);
		prop_mesh_free(&surface);
		prop_error("Nothing is left of it (too small for its grid?)");
		return false;
	}
	// Its own as asked; the levels a quarter, a twelfth, a fortieth of it
	targets[0] = recipe->triangles > 0 ? (size_t)recipe->triangles : surface.index_count / 3;
	targets[1] = targets[0] / 4 > 48 ? targets[0] / 4 : 48;
	targets[2] = targets[0] / 12 > 32 ? targets[0] / 12 : 32;
	targets[3] = targets[0] / 40 > 24 ? targets[0] / 40 : 24;
	for (int level = 0; level < 4 && ok; level++)
	{
		prop_mesh_t made;

		// No fewer than the level before (a small one): that one again
		if (level > 0 && targets[level] * 10 >= previous_count * 9)
		{
			size_t start = mesh->index_count - previous_count * 3;

			for (size_t i = 0; i < previous_count * 3 && ok; i += 3)
			{
				ok = mesh_triangle(mesh, mesh->indices[start + i], mesh->indices[start + i + 1], mesh->indices[start + i + 2]);
			}
			material_level_done(mesh, material, level);
			mesh->lod_errors[level - 1] = previous_error;
			continue;
		}
		if (!mesh_copy(&surface, &made))
		{
			ok = false;
			break;
		}
		simplify(&made, targets[level]);
		surface_normals(&made);
		// (each level at least as far off as the one before)
		previous_error = maximum_of(surface_error(&grid, &made), previous_error);
		if (level > 0)
		{
			mesh->lod_errors[level - 1] = previous_error;
		}
		if (level == 0)
		{
			float lowest = 1e9f;

			for (size_t i = 0; i < made.vertex_count; i++)
			{
				lowest = minimum_of(lowest, made.vertices[i].position[1]);
			}
			// (every level as deep in the ground as its own)
			shift = -maximum_of(recipe->sink, 0.0f) - lowest;
		}
		// The weather's marks, the ground where it will be; the levels' from
		// their own surface's
		if (level == 0)
		{
			weathering(&grid, &made, -shift);
		}
		for (size_t i = 0; i < made.vertex_count; i++)
		{
			made.vertices[i].position[1] += shift;
		}
		if (level == 0)
		{
			ok = mesh_copy(&made, &own) && mesh_append(mesh, &made);
		}
		else
		{
			weathering_from(&made, &own);
			ok = mesh_append(mesh, &made);
		}
		previous_count = made.index_count / 3;
		material_level_done(mesh, material, level);
		prop_mesh_free(&made);
	}
	free(grid.values);
	prop_mesh_free(&surface);
	prop_mesh_free(&own);
	if (!ok)
	{
		prop_error("No memory for its levels of detail");
	}
	return ok;
}

// ---------------------------------------------------------------------------
// Rocks and walls

static bool grow_rock(const prop_recipe_t *recipe, prop_mesh_t *mesh)
{
	random_t random;
	scene_t scene;
	int count = recipe->stones < 1 ? 1 : (recipe->stones > 64 ? 64 : recipe->stones);
	vec_t half = vec(maximum_of(recipe->length, 0.02f) * 0.5f, maximum_of(recipe->height, 0.02f) * 0.5f, maximum_of(recipe->width, 0.02f) * 0.5f);
	bool ok;

	random_seed(&random, recipe->seed);
	scene.count = count;
	scene.stones = calloc((size_t)count, sizeof(stone_t));
	if (scene.stones == NULL)
	{
		prop_error("No memory");
		return false;
	}
	if (count == 1)
	{
		stone_make(&scene.stones[0], recipe, half, &random, 1.0f);
	}
	else
	{
		// A heap: the stones round the middle, the bigger ones first, nearer it
		for (int s = 0; s < count; s++)
		{
			float size = s == 0 ? 1.0f : random_between(&random, 0.4f, 0.95f);
			vec_t own = vec(half.x * size * random_between(&random, 0.8f, 1.2f), half.y * size * random_between(&random, 0.75f, 1.2f),
				half.z * size * random_between(&random, 0.8f, 1.2f));
			stone_t *stone = &scene.stones[s];
			vec_t place = vec(0.0f, 0.0f, 0.0f);

			stone_make(stone, recipe, own, &random, 1.0f);
			// (a few tries for a place not too much in the others)
			for (int attempt = 0; attempt < 12 && s > 0; attempt++)
			{
				float angle = random_between(&random, 0.0f, 2.0f * PROP_PI);
				float reach = maximum_of(recipe->spread, 0.01f) * sqrtf(random_unit(&random));
				bool clear = true;

				place = vec(cosf(angle) * reach, 0.0f, sinf(angle) * reach);
				for (int o = 0; o < s && clear; o++)
				{
					float apart = length_of(sub(vec(place.x, 0.0f, place.z), vec(scene.stones[o].center.x, 0.0f, scene.stones[o].center.z)));

					clear = apart > (own.x + own.z) * 0.35f + (scene.stones[o].half.x + scene.stones[o].half.z) * 0.35f;
				}
				if (clear)
				{
					break;
				}
			}
			stone->center = vec(place.x, own.y * 0.6f, place.z);
			stone_turn(stone, random_between(&random, 0.0f, 2.0f * PROP_PI), random_between(&random, -0.25f, 0.25f),
				random_between(&random, -0.25f, 0.25f));
		}
	}
	ok = carve(&scene, recipe, 0.0f, mesh);
	free(scene.stones);
	return ok;
}

// Where along the wall's line, and which way it runs there (a ring: round
// its middle)
static void wall_line(const prop_recipe_t *recipe, float along, vec_t *point_out, float *yaw_out)
{
	if (recipe->ring_radius > 0.05f)
	{
		float angle = along / recipe->ring_radius;

		*point_out = vec(cosf(angle) * recipe->ring_radius, 0.0f, sinf(angle) * recipe->ring_radius);
		// (its own x along the ring: the tangent)
		*yaw_out = -angle - PROP_PI * 0.5f;
	}
	else
	{
		*point_out = vec(along - recipe->wall_length * 0.5f, 0.0f, 0.0f);
		*yaw_out = 0.0f;
	}
}

static bool grow_wall(const prop_recipe_t *recipe, prop_mesh_t *mesh)
{
	random_t random;
	scene_t scene;
	bool ring = recipe->ring_radius > 0.05f;
	float total = ring ? 2.0f * PROP_PI * recipe->ring_radius : maximum_of(recipe->wall_length, 0.2f);
	float height = maximum_of(recipe->wall_height, 0.1f);
	float stone_size = clamp_float(recipe->stone_size, 0.05f, 2.0f);
	float y = 0.0f;
	int course = 0;
	prop_recipe_t stone_recipe = *recipe;
	bool ok;

	random_seed(&random, recipe->seed);
	scene.count = 0;
	scene.stones = calloc(MAX_STONES, sizeof(stone_t));
	if (scene.stones == NULL)
	{
		prop_error("No memory");
		return false;
	}
	// The wall's stones: rough blocks, a few split faces
	stone_recipe.facets = recipe->facets < 3 ? recipe->facets : 3;
	stone_recipe.flat_bottom = 0.0f;
	stone_recipe.holes = 0;
	stone_recipe.layers = 0.0f;
	stone_recipe.lump_size = stone_size * 0.8f;
	while (y < height - stone_size * 0.12f && scene.count < MAX_STONES)
	{
		float course_height = minimum_of(stone_size * random_between(&random, 0.42f, 0.62f), height - y + stone_size * 0.05f);
		float up = y / height;
		float thickness = maximum_of(recipe->wall_thickness, 0.1f) * (1.0f - clamp_float(recipe->batter, 0.0f, 0.6f) * up);
		// Thick walls: two stones across, their joints apart
		int across = thickness > stone_size * 1.6f ? 2 : 1;
		float along = (course & 1) != 0 ? stone_size * 0.5f : random_between(&random, 0.0f, stone_size * 0.3f);
		float start = along;

		for (int side = 0; side < across; side++)
		{
			along = start + (float)side * stone_size * 0.37f;
			while (along < total - (ring ? 0.0f : stone_size * 0.2f) && scene.count < MAX_STONES)
			{
				float stone_length = stone_size * random_between(&random, 0.65f, 1.35f);
				float gone = clamp_float(recipe->ruin, 0.0f, 1.0f) * smooth_step(0.25f, 1.0f, up) * 1.3f;
				vec_t point;
				float yaw;
				stone_t *stone;
				float depth = thickness / (float)across;
				float out = across == 2 ? (side == 0 ? -0.25f : 0.25f) * thickness : 0.0f;

				if (ring)
				{
					stone_length = minimum_of(stone_length, total - along + stone_size * 0.05f);
				}
				else
				{
					stone_length = minimum_of(stone_length, total - along);
				}
				if (stone_length < stone_size * 0.25f)
				{
					break;
				}
				if (random_unit(&random) < gone)
				{
					along += stone_length;
					continue;
				}
				wall_line(recipe, along + stone_length * 0.5f, &point, &yaw);
				stone = &scene.stones[scene.count++];
				// (a little bigger than its place: the stones grown into one
				// another, their rounded edges the joints)
				stone_make(stone, &stone_recipe, vec(stone_length * 0.55f, course_height * 0.56f, depth * random_between(&random, 0.47f, 0.53f)),
					&random, 1.0f);
				stone_turn(stone, yaw + random_between(&random, -0.07f, 0.07f), random_between(&random, -0.05f, 0.05f),
					random_between(&random, -0.06f, 0.06f));
				// Out from the line (a ring: out from its middle)
				stone->center = add(point, scale(stone->axes[2], out + random_between(&random, -0.04f, 0.04f) * depth));
				stone->center.y = y + course_height * 0.5f + random_between(&random, -0.02f, 0.02f) * course_height;
				along += stone_length + stone_size * random_between(&random, -0.02f, 0.03f);
			}
		}
		y += course_height;
		course++;
	}
	if (scene.count == 0)
	{
		free(scene.stones);
		prop_error("No stones: the wall is too small for them");
		return false;
	}
	ok = carve(&scene, recipe, stone_size, mesh);
	free(scene.stones);
	return ok;
}

// ---------------------------------------------------------------------------
// Fences: pieces of wood

typedef struct
{
	vec_t origin;                        // the middle of its foot
	vec_t axis;                          // along it (the grain)
	vec_t side_a;                        // its cross section's two axes
	vec_t side_b;
	float length;
	float half_a;                        // its cross section's half sizes
	float half_b;
	float point;                         // its far end pointed this much (meters, along side_a)
} piece_t;

// A face's triangle, its normal and tangent its own (flat)
static bool wood_triangle(prop_mesh_t *mesh, const vec_t corners[3], const float corner_uv[3][2], vec_t outward)
{
	vec_t p[3] = { corners[0], corners[1], corners[2] };
	float uv[3][2] = { { corner_uv[0][0], corner_uv[0][1] }, { corner_uv[1][0], corner_uv[1][1] }, { corner_uv[2][0], corner_uv[2][1] } };

	// Facing out (counter clockwise from outside)
	if (dot(cross(sub(p[1], p[0]), sub(p[2], p[0])), outward) < 0.0f)
	{
		p[1] = corners[2];
		p[2] = corners[1];
		uv[1][0] = corner_uv[2][0];
		uv[1][1] = corner_uv[2][1];
		uv[2][0] = corner_uv[1][0];
		uv[2][1] = corner_uv[1][1];
	}
	vec_t e1 = sub(p[1], p[0]);
	vec_t e2 = sub(p[2], p[0]);
	vec_t normal = cross(e1, e2);
	float du1 = uv[1][0] - uv[0][0];
	float dv1 = uv[1][1] - uv[0][1];
	float du2 = uv[2][0] - uv[0][0];
	float dv2 = uv[2][1] - uv[0][1];
	float determinant = du1 * dv2 - du2 * dv1;
	vec_t tangent;
	vec_t bitangent;
	float frame[4];
	uint32_t first = (uint32_t)mesh->vertex_count;

	if (dot(normal, normal) < 1e-16f)
	{
		return true;
	}
	normal = normalized(normal);
	if (fabsf(determinant) < 1e-12f)
	{
		tangent = normalized(cross(vec(0.0f, 1.0f, 0.0f), normal));
		bitangent = cross(normal, tangent);
	}
	else
	{
		tangent = scale(sub(scale(e1, dv2), scale(e2, dv1)), 1.0f / determinant);
		bitangent = scale(sub(scale(e2, du1), scale(e1, du2)), 1.0f / determinant);
	}
	tangent = normalized(sub(tangent, scale(normal, dot(normal, tangent))));
	frame[0] = tangent.x;
	frame[1] = tangent.y;
	frame[2] = tangent.z;
	// The bitangent up the picture (against V: the normal maps' green up,
	// OpenGL's way)
	frame[3] = dot(cross(normal, tangent), bitangent) < 0.0f ? 1.0f : -1.0f;
	for (int k = 0; k < 3; k++)
	{
		if (!mesh_vertex(mesh, p[k], normal, uv[k][0], uv[k][1], frame))
		{
			return false;
		}
	}
	return mesh_triangle(mesh, first, first + 1, first + 2);
}

// The cross section's corners, round it (counter clockwise seen from the
// far end): a, b; with a pointed end, the middle of the two wide sides too
static int piece_outline(const piece_t *piece, float outline[6][2])
{
	if (piece->point > 0.0f)
	{
		float corners[6][2] =
		{
			{ -piece->half_a, -piece->half_b }, { 0.0f, -piece->half_b }, { piece->half_a, -piece->half_b },
			{ piece->half_a, piece->half_b }, { 0.0f, piece->half_b }, { -piece->half_a, piece->half_b },
		};

		memcpy(outline, corners, sizeof(corners));
		return 6;
	}
	{
		float corners[4][2] =
		{
			{ -piece->half_a, -piece->half_b }, { piece->half_a, -piece->half_b }, { piece->half_a, piece->half_b }, { -piece->half_a, piece->half_b },
		};

		memcpy(outline, corners, sizeof(corners));
		return 4;
	}
}

// How far along a corner of the outline reaches (shorter towards the
// pointed end's sides)
static float piece_reach(const piece_t *piece, const float corner[2])
{
	return piece->length - (piece->point > 0.0f ? piece->point * fabsf(corner[0]) / maximum_of(piece->half_a, 1e-6f) : 0.0f);
}

static vec_t piece_point(const piece_t *piece, const float corner[2], float along)
{
	return add(piece->origin, add(scale(piece->axis, along), add(scale(piece->side_a, corner[0]), scale(piece->side_b, corner[1]))));
}

// A piece of wood: its sides along the grain from a random place of the
// texture's side part, its ends from one of its four end pictures
static bool wood_piece(prop_mesh_t *mesh, const piece_t *piece, float meters, random_t *random)
{
	float outline[6][2];
	int count = piece_outline(piece, outline);
	float around = 0.0f;
	float perimeter = 0.0f;
	float u_start = random_unit(random);
	float v_scale;
	float v_start;

	for (int k = 0; k < count; k++)
	{
		const float *a = outline[k];
		const float *b = outline[(k + 1) % count];

		perimeter += hypotf(b[0] - a[0], b[1] - a[1]);
	}
	// Round it within the side part (shrunk to fit if it is too thick)
	v_scale = minimum_of(1.0f / meters, (WOOD_SIDE_PART - 0.02f) / perimeter);
	v_start = random_between(random, 0.01f, maximum_of(WOOD_SIDE_PART - 0.01f - perimeter * v_scale, 0.011f));
	for (int k = 0; k < count; k++)
	{
		const float *a = outline[k];
		const float *b = outline[(k + 1) % count];
		float side = hypotf(b[0] - a[0], b[1] - a[1]);
		float reach_a = piece_reach(piece, a);
		float reach_b = piece_reach(piece, b);
		vec_t p0 = piece_point(piece, a, 0.0f);
		vec_t p1 = piece_point(piece, b, 0.0f);
		vec_t p2 = piece_point(piece, b, reach_b);
		vec_t p3 = piece_point(piece, a, reach_a);
		float v0 = v_start + around * v_scale;
		float v1 = v_start + (around + side) * v_scale;
		// (the texture's V runs down its rows: the side part from the top)
		vec_t first[3] = { p0, p1, p2 };
		vec_t second[3] = { p0, p2, p3 };
		float first_uv[3][2] = { { u_start, v0 }, { u_start, v1 }, { u_start + reach_b / meters, v1 } };
		float second_uv[3][2] = { { u_start, v0 }, { u_start + reach_b / meters, v1 }, { u_start + reach_a / meters, v0 } };

		vec_t outward = add(scale(piece->side_a, (a[0] + b[0]) * 0.5f), scale(piece->side_b, (a[1] + b[1]) * 0.5f));

		if (!wood_triangle(mesh, first, first_uv, outward) || !wood_triangle(mesh, second, second_uv, outward))
		{
			return false;
		}
		around += side;
	}
	// The two ends: the foot, the top (a pointed top: fanned from the first
	// middle corner, which keeps its two slopes flat)
	{
		float largest = maximum_of(piece->half_a, piece->half_b);

		for (int end = 0; end < 2; end++)
		{
			int picture = (int)(random_unit(random) * 3.999f);
			float centre_u = ((float)picture + 0.5f) * 0.25f;
			float centre_v = WOOD_SIDE_PART + (1.0f - WOOD_SIDE_PART) * 0.5f;
			float spread = 0.25f * 0.44f / maximum_of(largest, 1e-6f);
			int fan = piece->point > 0.0f && end == 1 ? 1 : 0;

			for (int k = 1; k < count - 1; k++)
			{
				int corners[3] = { fan, (fan + k) % count, (fan + k + 1) % count };
				vec_t p[3];
				float uv[3][2];

				for (int c = 0; c < 3; c++)
				{
					const float *corner = outline[corners[c]];

					p[c] = piece_point(piece, corner, end == 0 ? 0.0f : piece_reach(piece, corner));
					uv[c][0] = centre_u + corner[0] * spread;
					uv[c][1] = centre_v + corner[1] * spread;
				}
				// (the foot faces back along it)
				if (!wood_triangle(mesh, p, uv, end == 0 ? scale(piece->axis, -1.0f) : piece->axis))
				{
					return false;
				}
			}
		}
	}
	return true;
}

// A piece along a direction, its cross section's a axis as near level (or,
// standing, along x) as it can be
static piece_t piece_along(vec_t origin, vec_t axis, float length, float half_a, float half_b, float point)
{
	piece_t piece;
	vec_t reference = fabsf(axis.y) > 0.9f ? vec(1.0f, 0.0f, 0.0f) : vec(0.0f, 1.0f, 0.0f);

	piece.origin = origin;
	piece.axis = normalized(axis);
	piece.side_b = normalized(cross(piece.axis, reference));
	// (standing: a along x, b across; lying: a up, b across)
	piece.side_a = normalized(cross(piece.side_b, piece.axis));
	piece.length = length;
	piece.half_a = half_a;
	piece.half_b = half_b;
	piece.point = point;
	return piece;
}

static vec_t leaning(vec_t axis, float amount, random_t *random)
{
	float toward = random_between(random, 0.0f, 2.0f * PROP_PI);

	return normalized(add(axis, vec(cosf(toward) * amount, 0.0f, sinf(toward) * amount)));
}

static bool grow_fence(const prop_recipe_t *recipe, prop_mesh_t *mesh)
{
	random_t random;
	float length = maximum_of(recipe->fence_length, 0.3f);
	float height = maximum_of(recipe->fence_height, 0.2f);
	float wear = clamp_float(recipe->wear, 0.0f, 1.0f);
	int posts = (int)roundf(length / maximum_of(recipe->post_spacing, 0.2f)) + 1;
	float post_half = maximum_of(recipe->post_thickness, 0.02f) * 0.5f;
	float rail_half_height = maximum_of(recipe->rail_height, 0.01f) * 0.5f;
	float rail_half_depth = maximum_of(recipe->rail_depth, 0.01f) * 0.5f;
	float meters = maximum_of(recipe->texture_meters, 0.1f);
	float sink = maximum_of(recipe->sink, 0.0f);
	int rails = recipe->rails < 0 ? 0 : (recipe->rails > 6 ? 6 : recipe->rails);
	float post_x[256];
	float post_top[256];

	random_seed(&random, recipe->seed);
	posts = posts < 2 ? 2 : (posts > 256 ? 256 : posts);
	for (int p = 0; p < posts; p++)
	{
		float top = height * (1.0f + random_between(&random, -0.04f, 0.04f) * (0.3f + wear));
		vec_t axis = leaning(vec(0.0f, 1.0f, 0.0f), random_between(&random, 0.0f, 0.1f) * wear, &random);
		piece_t post;

		post_x[p] = -length * 0.5f + length * (float)p / (float)(posts - 1);
		post_top[p] = top;
		post = piece_along(vec(post_x[p], -sink, 0.0f), axis, top + sink + (recipe->picket_spacing > 0.0f ? 0.0f : 0.04f), post_half,
			post_half, recipe->picket_spacing > 0.0f ? 0.0f : post_half * 0.6f);
		if (!wood_piece(mesh, &post, meters, &random))
		{
			prop_error("No memory");
			return false;
		}
	}
	// The rails, from post to post, on their front
	for (int p = 0; p + 1 < posts; p++)
	{
		for (int r = 0; r < rails; r++)
		{
			float at = rails == 1 ? 0.62f : 0.22f + 0.6f * (float)r / (float)(rails - 1);
			float y = minimum_of(post_top[p], post_top[p + 1]) * at + random_between(&random, -0.03f, 0.03f) * wear;
			float droop = random_between(&random, -0.06f, 0.06f) * wear;
			vec_t start = vec(post_x[p] - post_half * 0.6f, y - droop, post_half + rail_half_depth);
			vec_t end = vec(post_x[p + 1] + post_half * 0.6f, y + droop, post_half + rail_half_depth);
			piece_t rail;

			// Worn: now and then one fallen off
			if (rails > 1 && random_unit(&random) < wear * 0.12f)
			{
				continue;
			}
			rail = piece_along(start, sub(end, start), length_of(sub(end, start)), rail_half_height, rail_half_depth, 0.0f);
			if (!wood_piece(mesh, &rail, meters, &random))
			{
				prop_error("No memory");
				return false;
			}
		}
	}
	// The pickets, on the rails' front
	if (recipe->picket_spacing > 0.01f && rails > 0)
	{
		float spacing = recipe->picket_spacing;
		float half_width = clamp_float(recipe->picket_width, 0.01f, spacing) * 0.5f;
		float half_thickness = 0.011f;
		float z = post_half + rail_half_depth * 2.0f + half_thickness;

		for (float x = -length * 0.5f + spacing * 0.5f; x < length * 0.5f; x += spacing)
		{
			float top = height * random_between(&random, 0.97f, 1.02f) + recipe->picket_point * 0.5f;
			vec_t axis = leaning(vec(0.0f, 1.0f, 0.0f), random_between(&random, 0.0f, 0.06f) * wear, &random);
			piece_t picket;

			if (random_unit(&random) < wear * 0.08f)
			{
				continue;
			}
			picket = piece_along(vec(x, 0.04f, z), axis, top - 0.04f, half_width, half_thickness, maximum_of(recipe->picket_point, 0.0f));
			if (!wood_piece(mesh, &picket, meters, &random))
			{
				prop_error("No memory");
				return false;
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Barrels, tubs, buckets: staves of wood round a bulging profile, iron
// hoops round them, a lid of boards. Every level of detail made again with
// fewer rows and segments (the same staves, the same hoops: each its own
// random numbers, the same at every level)

typedef struct
{
	int rows;                            // along a stave
	int columns;                         // across it
	bool sides;                          // the staves' sides (their joints, seen through the gaps)
	int hoop_segments;                   // round a hoop
	int handle_segments;                 // along a bucket's handle
} barrel_detail_t;

static const barrel_detail_t barrel_details[4] =
{
	{ 10, 2, true, 36, 16 },
	{ 5, 1, true, 24, 8 },
	{ 3, 1, false, 16, 5 },
	{ 2, 1, false, 12, 3 },
};

typedef struct
{
	float height;
	float belly;                         // radius at its middle
	float end;                           // radius at its ends
	float thickness;                     // a stave's
	float meters;                        // the wood's texture: a repeat along
	float wear;
	bool open;                           // no lid: its inside shows
	float bottom_top;                    // the bottom's top (its boards'), and the lid's
	float lid_top;
} barrel_shape_t;

// Its radius at a height, and how fast that changes with the height
static float barrel_radius(const barrel_shape_t *barrel, float y, float *slope_out)
{
	float s = 2.0f * y / barrel->height - 1.0f;

	if (slope_out != NULL)
	{
		*slope_out = (barrel->belly - barrel->end) * -2.0f * s * 2.0f / barrel->height;
	}
	return barrel->end + (barrel->belly - barrel->end) * (1.0f - s * s);
}

// A grid of vertices (rows x columns, row major) into the mesh, its
// triangles facing as its normals say; its tangents along U (from the grid
// itself), the bitangent up the picture as wood_triangle's
static bool wood_sheet(prop_mesh_t *mesh, int rows, int columns, const vec_t *points, const vec_t *normals, const float (*uvs)[2])
{
	uint32_t base = (uint32_t)mesh->vertex_count;

	for (int r = 0; r < rows; r++)
	{
		for (int c = 0; c < columns; c++)
		{
			int here = r * columns + c;
			int r0 = r > 0 ? r - 1 : r;
			int r1 = r < rows - 1 ? r + 1 : r;
			int c0 = c > 0 ? c - 1 : c;
			int c1 = c < columns - 1 ? c + 1 : c;
			vec_t normal = normals[here];
			// Which way U and V go across the grid here
			float du_rows = uvs[r1 * columns + c][0] - uvs[r0 * columns + c][0];
			float du_columns = uvs[r * columns + c1][0] - uvs[r * columns + c0][0];
			vec_t along_rows = sub(points[r1 * columns + c], points[r0 * columns + c]);
			vec_t along_columns = sub(points[r * columns + c1], points[r * columns + c0]);
			vec_t tangent = fabsf(du_rows) >= fabsf(du_columns) ? scale(along_rows, du_rows >= 0.0f ? 1.0f : -1.0f)
				: scale(along_columns, du_columns >= 0.0f ? 1.0f : -1.0f);
			vec_t across = fabsf(du_rows) >= fabsf(du_columns) ? along_columns : along_rows;
			float dv = fabsf(du_rows) >= fabsf(du_columns) ? uvs[r * columns + c1][1] - uvs[r * columns + c0][1]
				: uvs[r1 * columns + c][1] - uvs[r0 * columns + c][1];
			float frame[4];

			tangent = normalized(sub(tangent, scale(normal, dot(normal, tangent))));
			across = scale(across, dv >= 0.0f ? 1.0f : -1.0f);
			frame[0] = tangent.x;
			frame[1] = tangent.y;
			frame[2] = tangent.z;
			frame[3] = dot(cross(normal, tangent), across) < 0.0f ? 1.0f : -1.0f;
			if (!mesh_vertex(mesh, points[here], normal, uvs[here][0], uvs[here][1], frame))
			{
				return false;
			}
		}
	}
	for (int r = 0; r + 1 < rows; r++)
	{
		for (int c = 0; c + 1 < columns; c++)
		{
			uint32_t a = base + (uint32_t)(r * columns + c);
			uint32_t b = base + (uint32_t)((r + 1) * columns + c);
			uint32_t d = base + (uint32_t)(r * columns + c + 1);
			uint32_t e = base + (uint32_t)((r + 1) * columns + c + 1);
			vec_t face = cross(sub(points[(r + 1) * columns + c], points[r * columns + c]), sub(points[r * columns + c + 1], points[r * columns + c]));
			bool flip = dot(face, normals[r * columns + c]) < 0.0f;

			// (a, b, d's turn is the face's: as its normal, unless flipped)
			if (!(flip ? mesh_triangle(mesh, a, e, b) && mesh_triangle(mesh, a, d, e)
				: mesh_triangle(mesh, a, b, e) && mesh_triangle(mesh, a, e, d)))
			{
				return false;
			}
		}
	}
	return true;
}

// One stave, between two angles, from foot to top (its own push out from
// the profile, a worn one's)
static bool barrel_stave(prop_mesh_t *mesh, const barrel_shape_t *barrel, const barrel_detail_t *detail, float angle0, float angle1,
	float foot, float top, float push, random_t *random)
{
	int rows = detail->rows + 1;
	int columns = detail->columns + 1;
	vec_t points[11 * 3];
	vec_t normals[11 * 3];
	float uvs[11 * 3][2];
	float u_start = random_unit(random);
	float width = (angle1 - angle0) * barrel->belly;
	float v_start = random_between(random, 0.01f, maximum_of(WOOD_SIDE_PART - 0.02f - (width + barrel->thickness * 2.0f) / barrel->meters, 0.011f));
	int pictures[2] = { (int)(random_unit(random) * 3.999f), (int)(random_unit(random) * 3.999f) };

	// Outside, then inside: all of it in an open one; else what shows over
	// the lid and under the bottom (looking in past the lid's edge)
	for (int part = 0; part < (barrel->open ? 2 : 3); part++)
	{
		int side = part == 0 ? 0 : 1;
		float from = foot;
		float to = top;
		int part_rows = rows;

		if (side == 1 && !barrel->open)
		{
			from = part == 1 ? maximum_of(foot, barrel->lid_top - 0.03f) : foot;
			to = part == 1 ? top : minimum_of(top, barrel->bottom_top + 0.005f);
			part_rows = 2;
			if (to <= from)
			{
				continue;
			}
		}
		for (int r = 0; r < part_rows; r++)
		{
			float y = from + (to - from) * (float)r / (float)(part_rows - 1);
			float slope;
			float radius = barrel_radius(barrel, y, &slope) + push - (side == 1 ? barrel->thickness : 0.0f);

			for (int c = 0; c < columns; c++)
			{
				float angle = angle0 + (angle1 - angle0) * (float)c / (float)(columns - 1);
				vec_t out = normalized(vec(cosf(angle), -slope, sinf(angle)));

				points[r * columns + c] = vec(cosf(angle) * radius, y, sinf(angle) * radius);
				normals[r * columns + c] = side == 0 ? out : scale(out, -1.0f);
				uvs[r * columns + c][0] = u_start + y / barrel->meters;
				uvs[r * columns + c][1] = v_start + (float)c / (float)(columns - 1) * width / barrel->meters;
			}
		}
		if (!wood_sheet(mesh, part_rows, columns, points, normals, (const float (*)[2])uvs))
		{
			return false;
		}
	}
	// Its two sides, across its thickness (the joints)
	for (int side = 0; detail->sides && side < 2; side++)
	{
		float angle = side == 0 ? angle0 : angle1;
		vec_t facing = vec(-sinf(angle) * (side == 0 ? -1.0f : 1.0f), 0.0f, cosf(angle) * (side == 0 ? -1.0f : 1.0f));

		for (int r = 0; r < rows; r++)
		{
			float y = foot + (top - foot) * (float)r / (float)(rows - 1);
			float radius = barrel_radius(barrel, y, NULL) + push;

			for (int c = 0; c < 2; c++)
			{
				float at = radius - (c == 0 ? barrel->thickness : 0.0f);

				points[r * 2 + c] = vec(cosf(angle) * at, y, sinf(angle) * at);
				normals[r * 2 + c] = facing;
				uvs[r * 2 + c][0] = u_start + y / barrel->meters;
				uvs[r * 2 + c][1] = v_start + (width + (float)c * barrel->thickness) / barrel->meters;
			}
		}
		if (!wood_sheet(mesh, rows, 2, points, normals, (const float (*)[2])uvs))
		{
			return false;
		}
	}
	// Its ends: the end grain, from one of the end pictures
	for (int end = 0; end < 2; end++)
	{
		float y = end == 0 ? foot : top;
		float radius = barrel_radius(barrel, y, NULL) + push;
		float centre_u = ((float)pictures[end] + 0.5f) * 0.25f;
		float centre_v = WOOD_SIDE_PART + (1.0f - WOOD_SIDE_PART) * 0.5f;
		float spread = 0.25f * 0.44f / maximum_of(width * 0.5f, barrel->thickness);

		for (int c = 0; c + 1 < columns; c++)
		{
			float a0 = angle0 + (angle1 - angle0) * (float)c / (float)(columns - 1);
			float a1 = angle0 + (angle1 - angle0) * (float)(c + 1) / (float)(columns - 1);
			float middle = (angle0 + angle1) * 0.5f;
			vec_t corners[4] =
			{
				vec(cosf(a0) * radius, y, sinf(a0) * radius), vec(cosf(a1) * radius, y, sinf(a1) * radius),
				vec(cosf(a1) * (radius - barrel->thickness), y, sinf(a1) * (radius - barrel->thickness)),
				vec(cosf(a0) * (radius - barrel->thickness), y, sinf(a0) * (radius - barrel->thickness)),
			};
			float corner_uv[4][2];

			for (int k = 0; k < 4; k++)
			{
				float across = (k == 0 || k == 3 ? a0 - middle : a1 - middle) * radius;
				float depth = (k < 2 ? 0.5f : -0.5f) * barrel->thickness;

				corner_uv[k][0] = centre_u + across * spread;
				corner_uv[k][1] = centre_v + depth * spread;
			}
			{
				vec_t first[3] = { corners[0], corners[1], corners[2] };
				vec_t second[3] = { corners[0], corners[2], corners[3] };
				float first_uv[3][2] = { { corner_uv[0][0], corner_uv[0][1] }, { corner_uv[1][0], corner_uv[1][1] }, { corner_uv[2][0], corner_uv[2][1] } };
				float second_uv[3][2] = { { corner_uv[0][0], corner_uv[0][1] }, { corner_uv[2][0], corner_uv[2][1] }, { corner_uv[3][0], corner_uv[3][1] } };
				vec_t facing = vec(0.0f, end == 0 ? -1.0f : 1.0f, 0.0f);

				if (!wood_triangle(mesh, first, first_uv, facing) || !wood_triangle(mesh, second, second_uv, facing))
				{
					return false;
				}
			}
		}
	}
	return true;
}

// A flat board of a lid (the grain along z), its outline in x, z (convex,
// counter clockwise from above), between two heights
static bool wood_slab(prop_mesh_t *mesh, const float (*outline)[2], int count, float bottom, float top, float meters, random_t *random)
{
	float u_start = random_unit(random);
	float v_start = random_between(random, 0.05f, WOOD_SIDE_PART - 0.3f);

	for (int face = 0; face < 2; face++)
	{
		float y = face == 0 ? top : bottom;
		vec_t facing = vec(0.0f, face == 0 ? 1.0f : -1.0f, 0.0f);

		for (int k = 1; k + 1 < count; k++)
		{
			int corner_indices[3] = { 0, k, k + 1 };
			vec_t corners[3];
			float uv[3][2];

			for (int c = 0; c < 3; c++)
			{
				const float *corner = outline[corner_indices[c]];

				corners[c] = vec(corner[0], y, corner[1]);
				uv[c][0] = u_start + corner[1] / meters;
				uv[c][1] = v_start + corner[0] / meters * 0.5f + 0.12f;
			}
			if (!wood_triangle(mesh, corners, uv, facing))
			{
				return false;
			}
		}
	}
	for (int k = 0; k < count; k++)
	{
		const float *a = outline[k];
		const float *b = outline[(k + 1) % count];
		vec_t facing = normalized(vec(b[1] - a[1], 0.0f, -(b[0] - a[0])));
		vec_t first[3] = { vec(a[0], bottom, a[1]), vec(b[0], bottom, b[1]), vec(b[0], top, b[1]) };
		vec_t second[3] = { vec(a[0], bottom, a[1]), vec(b[0], top, b[1]), vec(a[0], top, a[1]) };
		float run = hypotf(b[0] - a[0], b[1] - a[1]);
		float first_uv[3][2] = { { u_start, v_start }, { u_start + run / meters, v_start }, { u_start + run / meters, v_start + (top - bottom) / meters } };
		float second_uv[3][2] = { { u_start, v_start }, { u_start + run / meters, v_start + (top - bottom) / meters }, { u_start, v_start + (top - bottom) / meters } };

		// (outward: the outline goes counter clockwise seen from above)
		if (dot(facing, vec((a[0] + b[0]) * 0.5f, 0.0f, (a[1] + b[1]) * 0.5f)) < 0.0f)
		{
			facing = scale(facing, -1.0f);
		}
		if (!wood_triangle(mesh, first, first_uv, facing) || !wood_triangle(mesh, second, second_uv, facing))
		{
			return false;
		}
	}
	return true;
}

// A lid (or a bottom) of boards inside the staves, at a height
static bool barrel_lid(prop_mesh_t *mesh, const barrel_shape_t *barrel, float y, float board_thickness, random_t *random)
{
	float inner = barrel_radius(barrel, y, NULL) - barrel->thickness + 0.002f;
	int boards = (int)roundf(2.0f * inner / 0.13f);
	float outline[6][2];

	boards = boards < 2 ? 2 : (boards > 8 ? 8 : boards);
	for (int b = 0; b < boards; b++)
	{
		float x0 = -inner + 2.0f * inner * (float)b / (float)boards + 0.002f;
		float x1 = -inner + 2.0f * inner * (float)(b + 1) / (float)boards - 0.002f;
		float xs[3] = { x0, (x0 + x1) * 0.5f, x1 };
		float lift = random_between(random, -0.004f, 0.004f) * (0.3f + barrel->wear);

		// The circle's segment between x0 and x1: three points along each side
		for (int k = 0; k < 3; k++)
		{
			float half = sqrtf(maximum_of(inner * inner - xs[k] * xs[k], 0.0f)) * 0.995f;

			outline[k][0] = xs[k];
			outline[k][1] = -half;
			outline[5 - k][0] = xs[k];
			outline[5 - k][1] = half;
		}
		if (!wood_slab(mesh, (const float (*)[2])outline, 6, y - board_thickness + lift, y + lift, barrel->meters, random))
		{
			return false;
		}
	}
	return true;
}

// An iron hoop round it at a height (its middle), this wide and thick
static bool barrel_hoop(prop_mesh_t *mesh, const barrel_shape_t *barrel, float y, float width, float thickness, int segments)
{
	float meters = 0.5f;
	int columns = segments + 1;
	vec_t points[2 * 64];
	vec_t normals[2 * 64];
	float uvs[2 * 64][2];

	columns = columns > 64 ? 64 : columns;
	// Its outside, then its two edges
	for (int r = 0; r < 2; r++)
	{
		float at = y + (r == 0 ? -0.5f : 0.5f) * width;
		float slope;
		// (out round its corners: a few segments' flat sides would sink
		// into the staves)
		float radius = (barrel_radius(barrel, at, &slope) + thickness) / cosf(PROP_PI / (float)segments);

		for (int c = 0; c < columns; c++)
		{
			float angle = 2.0f * PROP_PI * (float)c / (float)(columns - 1);

			points[r * columns + c] = vec(cosf(angle) * radius, at, sinf(angle) * radius);
			normals[r * columns + c] = normalized(vec(cosf(angle), -slope, sinf(angle)));
			uvs[r * columns + c][0] = angle * radius / meters;
			uvs[r * columns + c][1] = (float)r * width / meters;
		}
	}
	if (!wood_sheet(mesh, 2, columns, points, normals, (const float (*)[2])uvs))
	{
		return false;
	}
	for (int edge = 0; edge < 2; edge++)
	{
		float at = y + (edge == 0 ? -0.5f : 0.5f) * width;
		float radius = barrel_radius(barrel, at, NULL);

		for (int r = 0; r < 2; r++)
		{
			for (int c = 0; c < columns; c++)
			{
				float angle = 2.0f * PROP_PI * (float)c / (float)(columns - 1);
				float here = (radius + (r == 0 ? -0.002f : thickness)) / (r == 0 ? 1.0f : cosf(PROP_PI / (float)segments));

				points[r * columns + c] = vec(cosf(angle) * here, at, sinf(angle) * here);
				normals[r * columns + c] = vec(0.0f, edge == 0 ? -1.0f : 1.0f, 0.0f);
				uvs[r * columns + c][0] = angle * radius / meters;
				uvs[r * columns + c][1] = 0.3f + (float)r * thickness / meters;
			}
		}
		if (!wood_sheet(mesh, 2, columns, points, normals, (const float (*)[2])uvs))
		{
			return false;
		}
	}
	return true;
}

// A bucket's handle: an iron bar bent over its top, from side to side
static bool barrel_handle(prop_mesh_t *mesh, const barrel_shape_t *barrel, int segments)
{
	float radius = barrel_radius(barrel, barrel->height, NULL) + 0.012f;
	float bar = 0.006f;
	int rows = segments + 1;
	vec_t points[17 * 7];
	vec_t normals[17 * 7];
	float uvs[17 * 7][2];

	rows = rows > 17 ? 17 : rows;
	for (int r = 0; r < rows; r++)
	{
		// From one side (its ear, a little below the top) over to the other
		float turn = PROP_PI * (float)r / (float)(rows - 1);
		vec_t centre = vec(cosf(turn) * radius, barrel->height - 0.03f + sinf(turn) * radius * 0.9f, 0.0f);
		vec_t along = normalized(vec(-sinf(turn) * radius, cosf(turn) * radius * 0.9f, 0.0f));
		vec_t side = vec(0.0f, 0.0f, 1.0f);
		vec_t up = cross(side, along);

		for (int c = 0; c < 7; c++)
		{
			float around = 2.0f * PROP_PI * (float)c / 6.0f;
			vec_t out = add(scale(up, cosf(around)), scale(side, sinf(around)));

			points[r * 7 + c] = add(centre, scale(out, bar));
			normals[r * 7 + c] = out;
			uvs[r * 7 + c][0] = (float)r * 0.1f;
			uvs[r * 7 + c][1] = (float)c / 6.0f * 0.1f;
		}
	}
	return wood_sheet(mesh, rows, 7, points, normals, (const float (*)[2])uvs);
}

// Where the hoops go (of its height, from the foot), as many as asked
static int barrel_hoop_places(int count, float places_out[8])
{
	static const float from_ends[3] = { 0.07f, 0.27f, 0.15f };
	int placed = 0;

	count = count < 0 ? 0 : (count > 8 ? 8 : count);
	for (int k = 0; k < count / 2 && k < 3; k++)
	{
		places_out[placed++] = from_ends[k];
		places_out[placed++] = 1.0f - from_ends[k];
	}
	if ((count & 1) != 0 || placed < count)
	{
		places_out[placed++] = 0.5f;
	}
	return placed;
}

// How far off a level is: the profile's chords, the round's facets
static float barrel_error(const barrel_shape_t *barrel, const barrel_detail_t *detail, int staves)
{
	float profile = (barrel->belly - barrel->end) / (float)(detail->rows * detail->rows);
	float round = barrel->belly * (1.0f - cosf(PROP_PI / (float)(staves * detail->columns)));
	float hoops = barrel->belly * (1.0f - cosf(PROP_PI / (float)detail->hoop_segments));
	float joints = detail->sides ? 0.0f : barrel->thickness * 0.3f;

	return maximum_of(maximum_of(profile, round), maximum_of(hoops, joints));
}

// Lying on its side (its axis along x, its belly on the ground)
static void barrel_lie_down(prop_mesh_t *mesh, size_t first_vertex, const barrel_shape_t *barrel)
{
	for (size_t v = first_vertex; v < mesh->vertex_count; v++)
	{
		object_file_vertex_t *vertex = &mesh->vertices[v];
		float x = vertex->position[0];
		float y = vertex->position[1];
		float nx = vertex->normal[0];
		float ny = vertex->normal[1];
		float tx = vertex->tangent[0];
		float ty = vertex->tangent[1];

		// (turned about z: up becomes along x)
		vertex->position[0] = y - barrel->height * 0.5f;
		vertex->position[1] = -x + barrel->belly;
		vertex->normal[0] = ny;
		vertex->normal[1] = -nx;
		vertex->tangent[0] = ty;
		vertex->tangent[1] = -tx;
	}
}

static bool grow_barrel(const prop_recipe_t *recipe, prop_mesh_t *mesh)
{
	barrel_shape_t barrel;
	int staves = recipe->staves < 6 ? 6 : (recipe->staves > 48 ? 48 : recipe->staves);
	float hoop_places[8];
	int hoop_count = barrel_hoop_places(recipe->hoops, hoop_places);
	float previous_error = 0.0f;
	prop_material_t *wood;
	prop_material_t *metal;

	barrel.height = maximum_of(recipe->barrel_height, 0.05f);
	barrel.belly = maximum_of(recipe->barrel_radius, 0.03f);
	barrel.end = clamp_float(recipe->barrel_end_radius, 0.02f, barrel.belly * 1.5f);
	barrel.thickness = clamp_float(recipe->stave_thickness, 0.004f, barrel.end * 0.5f);
	barrel.meters = maximum_of(recipe->texture_meters, 0.1f);
	barrel.wear = clamp_float(recipe->wear, 0.0f, 1.0f);
	barrel.open = recipe->lid == 0 || barrel.wear > 0.75f;
	barrel.bottom_top = barrel.height * 0.06f + 0.02f;
	barrel.lid_top = barrel.height - 0.025f;
	// The wood: every level, the same staves
	wood = material_begin(mesh, "wood");
	for (int level = 0; level < 4; level++)
	{
		const barrel_detail_t *detail = &barrel_details[level];
		size_t first_vertex = mesh->vertex_count;
		random_t random;

		for (int s = 0; s < staves; s++)
		{
			float span = 2.0f * PROP_PI / (float)staves;
			float gap = span * (0.025f + 0.04f * barrel.wear);
			float start = span * (float)s;

			random_seed(&random, recipe->seed * 7919u + (uint32_t)s * 104729u + 17u);
			{
				float foot = random_between(&random, 0.0f, 0.006f) * (0.3f + barrel.wear);
				float top = barrel.height - random_between(&random, 0.0f, 0.012f) * (0.3f + barrel.wear);
				float push = random_between(&random, -0.002f, 0.004f) * (0.3f + barrel.wear);
				// A broken one: staves gone, a few short
				bool gone = barrel.wear > 0.6f && random_unit(&random) < (barrel.wear - 0.6f) * 0.6f;
				bool short_one = barrel.wear > 0.6f && random_unit(&random) < (barrel.wear - 0.6f);

				if (gone)
				{
					continue;
				}
				if (short_one)
				{
					top = barrel.height * random_between(&random, 0.45f, 0.85f);
				}
				if (!barrel_stave(mesh, &barrel, detail, start + gap * 0.5f, start + span - gap * 0.5f, foot, top, push, &random))
				{
					return false;
				}
			}
		}
		random_seed(&random, recipe->seed * 31u + 5u);
		if (!barrel_lid(mesh, &barrel, barrel.bottom_top, 0.022f, &random)
			|| (!barrel.open && !barrel_lid(mesh, &barrel, barrel.lid_top, 0.022f, &random)))
		{
			return false;
		}
		if (recipe->lying)
		{
			barrel_lie_down(mesh, first_vertex, &barrel);
		}
		previous_error = maximum_of(previous_error, level > 0 ? barrel_error(&barrel, detail, staves) : 0.0f);
		if (level > 0)
		{
			mesh->lod_errors[level - 1] = previous_error;
		}
		material_level_done(mesh, wood, level);
	}
	// The iron: the hoops (and a bucket's handle)
	if (hoop_count == 0 && !recipe->handle)
	{
		return true;
	}
	metal = material_begin(mesh, "metal");
	metal->metallic = 1.0f;
	metal->roughness = 1.0f;
	for (int level = 0; level < 4; level++)
	{
		const barrel_detail_t *detail = &barrel_details[level];
		size_t first_vertex = mesh->vertex_count;

		for (int h = 0; h < hoop_count; h++)
		{
			random_t random;
			float y;

			random_seed(&random, recipe->seed * 6007u + (uint32_t)h * 7u + 3u);
			// Worn: slipped down a little
			y = hoop_places[h] * barrel.height - random_between(&random, 0.0f, 0.04f) * barrel.wear * barrel.height;
			if (!barrel_hoop(mesh, &barrel, y, maximum_of(recipe->hoop_width, 0.005f), maximum_of(recipe->hoop_thickness, 0.001f),
				detail->hoop_segments))
			{
				return false;
			}
		}
		if (recipe->handle && !barrel_handle(mesh, &barrel, detail->handle_segments))
		{
			return false;
		}
		if (recipe->lying)
		{
			barrel_lie_down(mesh, first_vertex, &barrel);
		}
		material_level_done(mesh, metal, level);
	}
	return true;
}

// ---------------------------------------------------------------------------
// The stand-in textures

typedef struct
{
	const char *name;
	float base[3];                       // sRGB 0..1
	float second[3];
	float dark[3];
	float speckles;                      // 0..1
	float speckle_color[3];
	float layering;                      // bands
	float cracks;
	float bumps;                         // the normal map's strength
	float roughness;
	bool wood;
} style_t;

static const style_t styles[] =
{
	{ "granite", { 0.56f, 0.55f, 0.53f }, { 0.48f, 0.46f, 0.45f }, { 0.20f, 0.20f, 0.21f }, 0.8f, { 0.78f, 0.70f, 0.66f }, 0.0f, 0.25f, 1.0f, 0.75f, false },
	{ "sandstone", { 0.58f, 0.54f, 0.48f }, { 0.52f, 0.47f, 0.40f }, { 0.32f, 0.29f, 0.25f }, 0.15f, { 0.72f, 0.66f, 0.56f }, 0.8f, 0.2f, 0.8f, 0.85f, false },
	{ "slate", { 0.34f, 0.34f, 0.35f }, { 0.28f, 0.28f, 0.29f }, { 0.15f, 0.15f, 0.16f }, 0.0f, { 0.45f, 0.45f, 0.45f }, 0.6f, 0.45f, 0.9f, 0.6f, false },
	{ "limestone", { 0.74f, 0.72f, 0.66f }, { 0.64f, 0.62f, 0.56f }, { 0.38f, 0.37f, 0.33f }, 0.25f, { 0.55f, 0.52f, 0.47f }, 0.2f, 0.35f, 1.1f, 0.8f, false },
	{ "wood", { 0.47f, 0.39f, 0.30f }, { 0.38f, 0.30f, 0.22f }, { 0.20f, 0.15f, 0.11f }, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.3f, 1.0f, 0.8f, true },
	{ "rusty_iron", { 0.30f, 0.19f, 0.12f }, { 0.42f, 0.24f, 0.11f }, { 0.11f, 0.09f, 0.08f }, 0.3f, { 0.50f, 0.30f, 0.14f }, 0.0f, 0.0f, 0.8f, 0.8f, false },
	{ "iron", { 0.22f, 0.22f, 0.23f }, { 0.17f, 0.17f, 0.18f }, { 0.08f, 0.08f, 0.09f }, 0.1f, { 0.36f, 0.26f, 0.18f }, 0.0f, 0.0f, 0.5f, 0.55f, false },
	{ "old_wood", { 0.55f, 0.53f, 0.49f }, { 0.43f, 0.41f, 0.38f }, { 0.19f, 0.18f, 0.17f }, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.6f, 1.3f, 0.9f, true },
};

static const style_t *style_find(const char *name)
{
	for (size_t s = 0; s < sizeof(styles) / sizeof(styles[0]); s++)
	{
		if (strcmp(styles[s].name, name) == 0)
		{
			return &styles[s];
		}
	}
	return NULL;
}

// Value noise in 2D that repeats every period cells (period: the cells
// across the texture)
static float tiling_noise(float x, float y, int period, uint32_t seed)
{
	float fx = floorf(x);
	float fy = floorf(y);
	int ix = (int)fx;
	int iy = (int)fy;
	float u = fade_curve(x - fx);
	float v = fade_curve(y - fy);
	float c[4];

	for (int k = 0; k < 4; k++)
	{
		int cx = ((ix + (k & 1)) % period + period) % period;
		int cy = ((iy + (k >> 1)) % period + period) % period;

		c[k] = (float)(hash_cell(cx, cy, 0, seed) & 0xFFFFu) / 65535.0f;
	}
	return lerp(lerp(c[0], c[1], u), lerp(c[2], c[3], u), v);
}

// 0..1, x and y 0..1 across the texture (it tiles)
static float tiling_fractal(float x, float y, int cells, int octaves, uint32_t seed)
{
	float sum = 0.0f;
	float amplitude = 0.5f;
	float total = 0.0f;

	for (int o = 0; o < octaves; o++)
	{
		sum += amplitude * tiling_noise(x * (float)cells, y * (float)cells, cells, seed + (uint32_t)o * 31u);
		total += amplitude;
		cells *= 2;
		amplitude *= 0.5f;
	}
	return sum / total;
}

// Cracks: the edges between cells (that tile), 0 on one .. 1 away
static float tiling_cracks(float x, float y, int cells, uint32_t seed)
{
	float px = x * (float)cells;
	float py = y * (float)cells;
	int ix = (int)floorf(px);
	int iy = (int)floorf(py);
	float nearest = 9.0f;
	float second = 9.0f;

	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			int cx = ix + dx;
			int cy = iy + dy;
			uint32_t h = hash_cell(((cx % cells) + cells) % cells, ((cy % cells) + cells) % cells, 5, seed);
			float sx = (float)cx + (float)(h & 0xFFu) / 255.0f;
			float sy = (float)cy + (float)((h >> 8) & 0xFFu) / 255.0f;
			float distance = hypotf(px - sx, py - sy);

			if (distance < nearest)
			{
				second = nearest;
				nearest = distance;
			}
			else if (distance < second)
			{
				second = distance;
			}
		}
	}
	return clamp_float((second - nearest) * 6.0f, 0.0f, 1.0f);
}

static void mix_color(float out[3], const float a[3], const float b[3], float t)
{
	for (int k = 0; k < 3; k++)
	{
		out[k] = lerp(a[k], b[k], t);
	}
}

// One texel's look: its color, how high it stands (0..1)
static void stone_texel(const style_t *style, float x, float y, float color[3], float *height_out)
{
	float broad = tiling_fractal(x, y, 4, 5, 11u);
	float fine = tiling_fractal(x, y, 32, 3, 23u);
	float cracks = tiling_cracks(x + 0.05f * tiling_fractal(x, y, 8, 2, 3u), y, 5, 17u);
	float height = broad * 0.6f + fine * 0.4f;

	mix_color(color, style->base, style->second, smooth_step(0.35f, 0.7f, broad));
	if (style->layering > 0.0f)
	{
		float band = 0.5f + 0.5f * sinf((y * 9.0f + broad * 1.6f) * 2.0f * PROP_PI);

		mix_color(color, color, style->second, band * style->layering * 0.6f);
		height -= band * style->layering * 0.08f;
	}
	if (style->speckles > 0.0f)
	{
		float speck = tiling_fractal(x, y, 128, 2, 41u);
		float dark_speck = tiling_fractal(x, y, 96, 2, 43u);

		if (speck > 0.68f - style->speckles * 0.08f)
		{
			mix_color(color, color, style->speckle_color, style->speckles * smooth_step(0.6f, 0.75f, speck));
		}
		if (dark_speck > 0.7f - style->speckles * 0.08f)
		{
			mix_color(color, color, style->dark, style->speckles * 0.8f * smooth_step(0.66f, 0.78f, dark_speck));
		}
	}
	// Grime in the low places, the cracks dark and sunk
	mix_color(color, color, style->dark, (1.0f - smooth_step(0.25f, 0.55f, height)) * 0.35f);
	if (style->cracks > 0.0f)
	{
		float crack = (1.0f - cracks) * style->cracks;

		mix_color(color, color, style->dark, crack * 0.8f);
		height -= crack * 0.25f;
	}
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.92f + 0.16f * fine;
	}
	*height_out = height;
}

// The wood's: the side grain above WOOD_SIDE_PART (along x, tiling along
// it), four ends' rings below it
static void wood_texel(const style_t *style, float x, float y, float color[3], float *height_out)
{
	if (y < WOOD_SIDE_PART)
	{
		float warp = tiling_fractal(x, y, 3, 3, 7u) * 2.0f;
		float grain = 0.5f + 0.5f * sinf((y * 46.0f + warp * 3.0f + 0.6f * sinf(x * 2.0f * PROP_PI * 2.0f)) * 2.0f * PROP_PI);
		float fibre = tiling_fractal(x * 0.25f, y * 4.0f, 64, 2, 19u);
		float crack_line = fabsf(sinf((y * 13.0f + warp) * PROP_PI));
		float crack = (1.0f - smooth_step(0.0f, 0.04f, crack_line)) * smooth_step(0.55f, 0.75f, tiling_fractal(x, y, 4, 2, 29u)) * style->cracks;
		float height = 0.55f + 0.25f * grain + 0.2f * fibre - crack * 0.5f;

		mix_color(color, style->base, style->second, grain * 0.8f);
		mix_color(color, color, style->dark, crack * 0.9f + (1.0f - fibre) * 0.15f);
		for (int k = 0; k < 3; k++)
		{
			color[k] *= 0.9f + 0.2f * tiling_fractal(x, y, 8, 3, 37u);
		}
		*height_out = height;
		return;
	}
	{
		// The end in its square: rings round a middle a little off it
		int picture = (int)(x * 4.0f);
		float local_x = x * 4.0f - (float)picture - 0.5f + 0.06f * (float)((picture * 7) % 3 - 1);
		float local_y = (y - WOOD_SIDE_PART) / (1.0f - WOOD_SIDE_PART) - 0.5f + 0.05f * (float)((picture * 5) % 3 - 1);
		float radius = hypotf(local_x, local_y);
		float angle = atan2f(local_y, local_x);
		float wobble = 0.02f * sinf(angle * 3.0f + (float)picture) + 0.015f * tiling_fractal(x, y, 16, 2, 53u);
		float ring = 0.5f + 0.5f * sinf((radius + wobble) * 2.0f * PROP_PI * 14.0f);
		float split = (1.0f - smooth_step(0.0f, 0.05f, fabsf(sinf(angle * 1.5f + (float)picture * 2.1f)))) * smooth_step(0.08f, 0.2f, radius)
			* style->cracks;
		float lighter[3] = { style->base[0] * 1.15f, style->base[1] * 1.12f, style->base[2] * 1.1f };

		mix_color(color, lighter, style->second, ring * 0.7f);
		mix_color(color, color, style->dark, split * 0.9f + smooth_step(0.42f, 0.5f, radius) * 0.6f);
		*height_out = 0.6f + 0.15f * ring - split * 0.5f;
	}
}

static float to_byte(float value)
{
	return clamp_float(value, 0.0f, 1.0f) * 255.0f + 0.5f;
}

static void write_to_file(void *context, void *data, int size)
{
	fwrite(data, 1, (size_t)size, (FILE *)context);
}

static bool write_png(const char *path, const uint8_t *pixels, int width, int height)
{
	FILE *file = fopen(path, "wb");
	bool ok;

	if (file == NULL)
	{
		prop_error("Cannot write '%s'", path);
		return false;
	}
	ok = stbi_write_png_to_func(write_to_file, file, width, height, 4, pixels, width * 4) != 0;
	fclose(file);
	if (!ok)
	{
		prop_error("Cannot write '%s'", path);
	}
	return ok;
}

static bool file_exists(const char *path)
{
	FILE *file = fopen(path, "rb");

	if (file != NULL)
	{
		fclose(file);
		return true;
	}
	return false;
}

// The folders of a path, made (as mkdir -p)
static void make_folders(const char *path)
{
	for (size_t i = 1; path[i] != '\0'; i++)
	{
		if (path[i] == '/')
		{
			char folder[PROP_PATH_LENGTH * 2];

			snprintf(folder, sizeof(folder), "%.*s", (int)i, path);
			// (there already, mostly)
			(void)mkdir(folder, 0755);
		}
	}
}

bool prop_style_textures(const char *style_name, const char *data_directory, bool remake, char names_out[3][PROP_PATH_LENGTH])
{
	static const char *const suffixes[3] = { "color", "normal", "arm" };
	const style_t *style = style_find(style_name);
	char paths[3][PROP_PATH_LENGTH * 2];
	bool missing = remake;
	float *heights;
	uint8_t *maps[3];
	int size = TEXTURE_SIZE;

	if (style == NULL)
	{
		prop_error("No style '%s' (granite, sandstone, slate, limestone, wood, old_wood, rusty_iron, iron)", style_name);
		return false;
	}
	for (int m = 0; m < 3; m++)
	{
		snprintf(names_out[m], PROP_PATH_LENGTH, "props/textures/%s_%s.png", style->name, suffixes[m]);
		snprintf(paths[m], sizeof(paths[m]), "%s/%s", data_directory, names_out[m]);
		missing = missing || !file_exists(paths[m]);
	}
	if (!missing)
	{
		return true;
	}
	make_folders(paths[0]);
	heights = malloc(sizeof(float) * (size_t)size * (size_t)size);
	for (int m = 0; m < 3; m++)
	{
		maps[m] = malloc((size_t)size * (size_t)size * 4);
	}
	if (heights == NULL || maps[0] == NULL || maps[1] == NULL || maps[2] == NULL)
	{
		free(heights);
		for (int m = 0; m < 3; m++)
		{
			free(maps[m]);
		}
		prop_error("No memory for the textures");
		return false;
	}
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float color[3];
			float height;
			uint8_t *texel = &maps[0][((size_t)y * (size_t)size + (size_t)x) * 4];

			if (style->wood)
			{
				wood_texel(style, ((float)x + 0.5f) / (float)size, ((float)y + 0.5f) / (float)size, color, &height);
			}
			else
			{
				stone_texel(style, ((float)x + 0.5f) / (float)size, ((float)y + 0.5f) / (float)size, color, &height);
			}
			heights[(size_t)y * (size_t)size + (size_t)x] = height;
			for (int k = 0; k < 3; k++)
			{
				texel[k] = (uint8_t)to_byte(color[k]);
			}
			texel[3] = 255;
		}
	}
	// The normals and the occlusion from the heights (wrapping round: it
	// tiles; the wood's ends' squares only within the ends' part, roughly)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			size_t here = (size_t)y * (size_t)size + (size_t)x;
			float left = heights[(size_t)y * (size_t)size + (size_t)((x + size - 1) % size)];
			float right = heights[(size_t)y * (size_t)size + (size_t)((x + 1) % size)];
			float up = heights[(size_t)((y + size - 1) % size) * (size_t)size + (size_t)x];
			float down = heights[(size_t)((y + 1) % size) * (size_t)size + (size_t)x];
			float strength = style->bumps * 6.0f;
			// (the rows run down: the texture's V; OpenGL's normal maps: green up)
			vec_t normal = normalized(vec((left - right) * strength, (down - up) * strength, 1.0f));
			float around = (left + right + up + down) * 0.25f;
			float occlusion = clamp_float(1.0f - (around - heights[here]) * 4.0f - (1.0f - heights[here]) * 0.25f, 0.4f, 1.0f);
			uint8_t *n = &maps[1][here * 4];
			uint8_t *a = &maps[2][here * 4];

			n[0] = (uint8_t)to_byte(normal.x * 0.5f + 0.5f);
			n[1] = (uint8_t)to_byte(normal.y * 0.5f + 0.5f);
			n[2] = (uint8_t)to_byte(normal.z * 0.5f + 0.5f);
			n[3] = 255;
			a[0] = (uint8_t)to_byte(occlusion);
			a[1] = (uint8_t)to_byte(style->roughness + (0.5f - heights[here]) * 0.2f);
			// (iron is a metal; rust, where it is high, is not)
			a[2] = (uint8_t)to_byte(strcmp(style->name, "iron") == 0 ? 0.9f
				: (strcmp(style->name, "rusty_iron") == 0 ? 0.6f * (1.0f - smooth_step(0.4f, 0.7f, heights[here])) : 0.0f));
			a[3] = 255;
		}
	}
	{
		bool ok = write_png(paths[0], maps[0], size, size) && write_png(paths[1], maps[1], size, size) && write_png(paths[2], maps[2], size, size);

		free(heights);
		for (int m = 0; m < 3; m++)
		{
			free(maps[m]);
		}
		return ok;
	}
}

bool prop_wood_template(const char *path)
{
	int size = TEXTURE_SIZE;
	uint8_t *pixels = malloc((size_t)size * (size_t)size * 4);
	bool ok;

	if (pixels == NULL)
	{
		prop_error("No memory");
		return false;
	}
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			uint8_t *texel = &pixels[((size_t)y * (size_t)size + (size_t)x) * 4];
			float shade;

			if (v < WOOD_SIDE_PART)
			{
				// The side grain: lines along U (the grain's way), a mark every
				// tenth of the width (the texture_meters' tenths)
				bool line = (y % 32) == 0;
				bool tick = (x % (size / 10)) == 0 && (y % 32) < 10;

				shade = line ? 0.55f : 0.85f;
				texel[0] = (uint8_t)(shade * 230.0f);
				texel[1] = (uint8_t)(shade * 190.0f);
				texel[2] = (uint8_t)(shade * 140.0f);
				if (tick)
				{
					texel[0] = 120;
					texel[1] = 60;
					texel[2] = 40;
				}
			}
			else
			{
				// The four ends: rings, the part a piece's end uses within the
				// square's dashed edge (0.88 of it), the square's own edge black
				int picture = (int)(u * 4.0f);
				float local_x = u * 4.0f - (float)picture - 0.5f;
				float local_y = (v - WOOD_SIDE_PART) / (1.0f - WOOD_SIDE_PART) - 0.5f;
				float radius = hypotf(local_x, local_y);
				bool edge = fabsf(local_x) > 0.49f || fabsf(local_y) > 0.49f;
				bool used = (fabsf(fabsf(local_x) - 0.44f) < 0.004f && fabsf(local_y) < 0.44f && ((y / 8) & 1) == 0)
					|| (fabsf(fabsf(local_y) - 0.44f) < 0.004f && fabsf(local_x) < 0.44f && ((x / 8) & 1) == 0);
				float ring = 0.5f + 0.5f * cosf(radius * 2.0f * PROP_PI * 10.0f);

				shade = 0.65f + 0.3f * ring;
				texel[0] = (uint8_t)(shade * 225.0f);
				texel[1] = (uint8_t)(shade * 200.0f);
				texel[2] = (uint8_t)(shade * 160.0f);
				if (used)
				{
					texel[0] = 200;
					texel[1] = 30;
					texel[2] = 30;
				}
				if (edge)
				{
					texel[0] = texel[1] = texel[2] = 0;
				}
			}
			texel[3] = 255;
		}
	}
	// The line between the two parts
	for (int x = 0; x < size; x++)
	{
		uint8_t *texel = &pixels[((size_t)(int)(WOOD_SIDE_PART * (float)size) * (size_t)size + (size_t)x) * 4];

		texel[0] = texel[1] = texel[2] = 0;
	}
	ok = write_png(path, pixels, size, size);
	free(pixels);
	return ok;
}

// ---------------------------------------------------------------------------
// Recipes

static const char *const kind_names[PROP_KIND_COUNT] = { "rock", "wall", "fence", "barrel" };

const char *prop_kind_name(prop_kind_t kind)
{
	return kind >= 0 && kind < PROP_KIND_COUNT ? kind_names[kind] : "rock";
}

int prop_kind_find(const char *name)
{
	for (int k = 0; k < PROP_KIND_COUNT; k++)
	{
		if (strcmp(kind_names[k], name) == 0)
		{
			return k;
		}
	}
	return -1;
}

typedef enum
{
	FIELD_FLOAT,
	FIELD_INT,
	FIELD_TEXT,
	FIELD_NAME,
	FIELD_SEED,
	FIELD_KIND
} field_type_t;

typedef struct
{
	const char *key;
	field_type_t type;
	size_t offset;
} field_t;

static const field_t fields[] =
{
	{ "kind", FIELD_KIND, offsetof(prop_recipe_t, kind) },
	{ "preset", FIELD_NAME, offsetof(prop_recipe_t, preset) },
	{ "seed", FIELD_SEED, offsetof(prop_recipe_t, seed) },
	{ "output", FIELD_TEXT, offsetof(prop_recipe_t, output) },
	{ "color_texture", FIELD_TEXT, offsetof(prop_recipe_t, textures[0]) },
	{ "normal_texture", FIELD_TEXT, offsetof(prop_recipe_t, textures[1]) },
	{ "arm_texture", FIELD_TEXT, offsetof(prop_recipe_t, textures[2]) },
	{ "style", FIELD_NAME, offsetof(prop_recipe_t, style) },
	{ "metal_color_texture", FIELD_TEXT, offsetof(prop_recipe_t, metal_textures[0]) },
	{ "metal_normal_texture", FIELD_TEXT, offsetof(prop_recipe_t, metal_textures[1]) },
	{ "metal_arm_texture", FIELD_TEXT, offsetof(prop_recipe_t, metal_textures[2]) },
	{ "metal_style", FIELD_NAME, offsetof(prop_recipe_t, metal_style) },
	{ "texture_meters", FIELD_FLOAT, offsetof(prop_recipe_t, texture_meters) },
	{ "moss", FIELD_FLOAT, offsetof(prop_recipe_t, moss) },
	{ "lichen", FIELD_FLOAT, offsetof(prop_recipe_t, lichen) },
	{ "resolution", FIELD_INT, offsetof(prop_recipe_t, resolution) },
	{ "sink", FIELD_FLOAT, offsetof(prop_recipe_t, sink) },
	{ "triangles", FIELD_INT, offsetof(prop_recipe_t, triangles) },
	{ "length", FIELD_FLOAT, offsetof(prop_recipe_t, length) },
	{ "height", FIELD_FLOAT, offsetof(prop_recipe_t, height) },
	{ "width", FIELD_FLOAT, offsetof(prop_recipe_t, width) },
	{ "boxiness", FIELD_FLOAT, offsetof(prop_recipe_t, boxiness) },
	{ "lumps", FIELD_FLOAT, offsetof(prop_recipe_t, lumps) },
	{ "lump_size", FIELD_FLOAT, offsetof(prop_recipe_t, lump_size) },
	{ "grain", FIELD_FLOAT, offsetof(prop_recipe_t, grain) },
	{ "facets", FIELD_INT, offsetof(prop_recipe_t, facets) },
	{ "facet_depth", FIELD_FLOAT, offsetof(prop_recipe_t, facet_depth) },
	{ "flat_bottom", FIELD_FLOAT, offsetof(prop_recipe_t, flat_bottom) },
	{ "layers", FIELD_FLOAT, offsetof(prop_recipe_t, layers) },
	{ "layer_spacing", FIELD_FLOAT, offsetof(prop_recipe_t, layer_spacing) },
	{ "layer_tilt", FIELD_FLOAT, offsetof(prop_recipe_t, layer_tilt) },
	{ "holes", FIELD_INT, offsetof(prop_recipe_t, holes) },
	{ "hole_size", FIELD_FLOAT, offsetof(prop_recipe_t, hole_size) },
	{ "stones", FIELD_INT, offsetof(prop_recipe_t, stones) },
	{ "spread", FIELD_FLOAT, offsetof(prop_recipe_t, spread) },
	{ "wall_length", FIELD_FLOAT, offsetof(prop_recipe_t, wall_length) },
	{ "wall_height", FIELD_FLOAT, offsetof(prop_recipe_t, wall_height) },
	{ "wall_thickness", FIELD_FLOAT, offsetof(prop_recipe_t, wall_thickness) },
	{ "stone_size", FIELD_FLOAT, offsetof(prop_recipe_t, stone_size) },
	{ "ring_radius", FIELD_FLOAT, offsetof(prop_recipe_t, ring_radius) },
	{ "ruin", FIELD_FLOAT, offsetof(prop_recipe_t, ruin) },
	{ "batter", FIELD_FLOAT, offsetof(prop_recipe_t, batter) },
	{ "fence_length", FIELD_FLOAT, offsetof(prop_recipe_t, fence_length) },
	{ "fence_height", FIELD_FLOAT, offsetof(prop_recipe_t, fence_height) },
	{ "post_spacing", FIELD_FLOAT, offsetof(prop_recipe_t, post_spacing) },
	{ "post_thickness", FIELD_FLOAT, offsetof(prop_recipe_t, post_thickness) },
	{ "rails", FIELD_INT, offsetof(prop_recipe_t, rails) },
	{ "rail_height", FIELD_FLOAT, offsetof(prop_recipe_t, rail_height) },
	{ "rail_depth", FIELD_FLOAT, offsetof(prop_recipe_t, rail_depth) },
	{ "picket_spacing", FIELD_FLOAT, offsetof(prop_recipe_t, picket_spacing) },
	{ "picket_width", FIELD_FLOAT, offsetof(prop_recipe_t, picket_width) },
	{ "picket_point", FIELD_FLOAT, offsetof(prop_recipe_t, picket_point) },
	{ "wear", FIELD_FLOAT, offsetof(prop_recipe_t, wear) },
	{ "barrel_height", FIELD_FLOAT, offsetof(prop_recipe_t, barrel_height) },
	{ "barrel_radius", FIELD_FLOAT, offsetof(prop_recipe_t, barrel_radius) },
	{ "barrel_end_radius", FIELD_FLOAT, offsetof(prop_recipe_t, barrel_end_radius) },
	{ "staves", FIELD_INT, offsetof(prop_recipe_t, staves) },
	{ "stave_thickness", FIELD_FLOAT, offsetof(prop_recipe_t, stave_thickness) },
	{ "hoops", FIELD_INT, offsetof(prop_recipe_t, hoops) },
	{ "hoop_width", FIELD_FLOAT, offsetof(prop_recipe_t, hoop_width) },
	{ "hoop_thickness", FIELD_FLOAT, offsetof(prop_recipe_t, hoop_thickness) },
	{ "lid", FIELD_INT, offsetof(prop_recipe_t, lid) },
	{ "lying", FIELD_INT, offsetof(prop_recipe_t, lying) },
	{ "handle", FIELD_INT, offsetof(prop_recipe_t, handle) },
	{ "variants", FIELD_INT, offsetof(prop_recipe_t, variants) },
};

// The presets: settings over the defaults
typedef struct
{
	const char *name;
	const char *settings;
} preset_t;

static const preset_t presets[] =
{
	{ "boulder", "triangles=2500 kind=rock style=granite length=1.6 height=1.1 width=1.3 boxiness=0.25 lumps=0.22 lump_size=0.8 grain=0.5 facets=3 "
		"facet_depth=0.2 flat_bottom=0.3 sink=0.12 resolution=44 moss=0.6 lichen=0.5" },
	{ "river_stone", "triangles=600 kind=rock style=granite length=0.45 height=0.22 width=0.34 boxiness=0 lumps=0.05 lump_size=0.4 grain=0.1 facets=0 "
		"flat_bottom=0 sink=0.02 resolution=30 moss=0.2 lichen=0.1 texture_meters=0.6" },
	{ "slab", "triangles=2000 kind=rock style=slate length=2.4 height=0.45 width=1.6 boxiness=0.8 lumps=0.12 lump_size=0.9 grain=0.4 facets=4 "
		"facet_depth=0.15 flat_bottom=0.4 layers=0.5 layer_spacing=0.11 layer_tilt=4 sink=0.08 resolution=56 moss=0.5 lichen=0.6" },
	{ "crag", "triangles=6000 kind=rock style=sandstone length=3.2 height=3.6 width=2.6 boxiness=0.6 lumps=0.28 lump_size=1.3 grain=0.6 facets=7 "
		"facet_depth=0.3 flat_bottom=0.15 layers=0.6 layer_spacing=0.34 layer_tilt=12 sink=0.35 resolution=64 moss=0.7 lichen=0.6 "
		"texture_meters=2" },
	{ "holed_rock", "triangles=4000 kind=rock style=limestone length=2.2 height=1.6 width=1.5 boxiness=0.3 lumps=0.25 lump_size=0.9 grain=0.5 facets=2 "
		"facet_depth=0.2 flat_bottom=0.25 holes=2 hole_size=0.32 sink=0.15 resolution=60 moss=0.5 lichen=0.7" },
	{ "split_rock", "triangles=2500 kind=rock style=granite length=1.8 height=1.3 width=1.4 boxiness=0.45 lumps=0.12 lump_size=1.0 grain=0.5 facets=6 "
		"facet_depth=0.45 flat_bottom=0.35 sink=0.1 resolution=52 moss=0.5 lichen=0.6" },
	{ "pebbles", "triangles=2500 kind=rock style=granite length=0.28 height=0.15 width=0.22 boxiness=0.1 lumps=0.08 lump_size=0.3 grain=0.15 facets=1 "
		"facet_depth=0.2 flat_bottom=0.1 stones=7 spread=0.45 sink=0.02 resolution=70 moss=0.25 lichen=0.2 texture_meters=0.6" },
	{ "dry_wall", "triangles=6000 kind=wall style=granite wall_length=4 wall_height=1.1 wall_thickness=0.6 stone_size=0.4 batter=0.25 ruin=0.15 "
		"boxiness=0.8 lumps=0.12 grain=0.5 facets=2 facet_depth=0.15 sink=0.05 resolution=9 moss=0.7 lichen=0.6" },
	{ "ruined_wall", "triangles=7000 kind=wall style=limestone wall_length=4 wall_height=1.6 wall_thickness=0.7 stone_size=0.42 batter=0.15 ruin=0.65 "
		"boxiness=0.75 lumps=0.15 grain=0.6 facets=2 facet_depth=0.2 sink=0.08 resolution=9 moss=0.8 lichen=0.7" },
	{ "well", "triangles=6000 kind=wall style=granite ring_radius=0.75 wall_height=0.8 wall_thickness=0.38 stone_size=0.32 batter=0 ruin=0.05 "
		"boxiness=0.85 lumps=0.1 grain=0.5 facets=1 facet_depth=0.1 sink=0.05 resolution=9 moss=0.6 lichen=0.5" },
	{ "rail_fence", "kind=fence style=old_wood fence_length=4 fence_height=1.1 post_spacing=2 post_thickness=0.13 rails=2 rail_height=0.11 "
		"rail_depth=0.05 picket_spacing=0 wear=0.35 sink=0.3 moss=0.35 lichen=0.5 texture_meters=1" },
	{ "wine_barrel", "kind=barrel style=wood barrel_height=0.95 barrel_radius=0.31 barrel_end_radius=0.25 staves=22 stave_thickness=0.022 "
		"hoops=6 hoop_width=0.035 hoop_thickness=0.004 lid=1 wear=0.15 moss=0.15 lichen=0.15 texture_meters=1" },
	{ "water_barrel", "kind=barrel style=old_wood barrel_height=0.85 barrel_radius=0.3 barrel_end_radius=0.27 staves=20 stave_thickness=0.025 "
		"hoops=4 hoop_width=0.04 hoop_thickness=0.004 lid=0 wear=0.35 moss=0.35 lichen=0.15 texture_meters=1" },
	{ "lying_barrel", "kind=barrel style=wood barrel_height=0.95 barrel_radius=0.31 barrel_end_radius=0.25 staves=22 stave_thickness=0.022 "
		"hoops=4 hoop_width=0.035 hoop_thickness=0.004 lid=1 lying=1 wear=0.3 moss=0.3 lichen=0.15 texture_meters=1" },
	{ "broken_barrel", "kind=barrel style=old_wood barrel_height=0.9 barrel_radius=0.3 barrel_end_radius=0.25 staves=20 stave_thickness=0.022 "
		"hoops=3 hoop_width=0.035 hoop_thickness=0.004 lid=0 wear=0.95 moss=0.6 lichen=0.25 texture_meters=1" },
	{ "tub", "kind=barrel style=old_wood barrel_height=0.42 barrel_radius=0.5 barrel_end_radius=0.44 staves=28 stave_thickness=0.025 "
		"hoops=2 hoop_width=0.04 hoop_thickness=0.004 lid=0 wear=0.3 moss=0.3 lichen=0.15 texture_meters=1" },
	{ "bucket", "kind=barrel style=wood barrel_height=0.3 barrel_radius=0.15 barrel_end_radius=0.135 staves=14 stave_thickness=0.014 "
		"hoops=2 hoop_width=0.02 hoop_thickness=0.003 lid=0 handle=1 wear=0.2 moss=0.1 lichen=0.1 texture_meters=1" },
	{ "picket_fence", "kind=fence style=wood fence_length=4 fence_height=1.0 post_spacing=2 post_thickness=0.1 rails=2 rail_height=0.08 "
		"rail_depth=0.03 picket_spacing=0.13 picket_width=0.08 picket_point=0.06 wear=0.15 sink=0.3 moss=0.2 lichen=0.3 texture_meters=1" },
};

// The numbers before any preset (what a preset leaves out stays so)
static void base_defaults(prop_recipe_t *recipe)
{
	memset(recipe, 0, sizeof(*recipe));
	recipe->kind = PROP_KIND_ROCK;
	recipe->seed = 1;
	snprintf(recipe->style, sizeof(recipe->style), "granite");
	recipe->texture_meters = 1.5f;
	recipe->resolution = 44;
	recipe->triangles = 3000;
	recipe->stones = 1;
	recipe->length = 1.0f;
	recipe->height = 1.0f;
	recipe->width = 1.0f;
	recipe->lump_size = 0.8f;
	recipe->facet_depth = 0.2f;
	recipe->layer_spacing = 0.3f;
	recipe->hole_size = 0.3f;
	recipe->spread = 0.5f;
	recipe->wall_length = 4.0f;
	recipe->wall_height = 1.1f;
	recipe->wall_thickness = 0.6f;
	recipe->stone_size = 0.4f;
	recipe->fence_length = 4.0f;
	recipe->fence_height = 1.1f;
	recipe->post_spacing = 2.0f;
	recipe->post_thickness = 0.12f;
	recipe->rails = 2;
	recipe->rail_height = 0.1f;
	recipe->rail_depth = 0.05f;
	recipe->picket_width = 0.08f;
	recipe->picket_point = 0.05f;
	recipe->barrel_height = 0.95f;
	recipe->barrel_radius = 0.31f;
	recipe->barrel_end_radius = 0.25f;
	recipe->staves = 22;
	recipe->stave_thickness = 0.022f;
	recipe->hoops = 4;
	recipe->hoop_width = 0.035f;
	recipe->hoop_thickness = 0.004f;
	recipe->lid = 1;
	snprintf(recipe->metal_style, sizeof(recipe->metal_style), "rusty_iron");
}

void prop_recipe_defaults(prop_recipe_t *recipe)
{
	base_defaults(recipe);
	prop_preset(recipe, "boulder");
}

int prop_preset_count(void)
{
	return (int)(sizeof(presets) / sizeof(presets[0]));
}

const char *prop_preset_name(int index)
{
	return index >= 0 && index < prop_preset_count() ? presets[index].name : "";
}

bool prop_preset(prop_recipe_t *recipe, const char *name)
{
	for (int p = 0; p < prop_preset_count(); p++)
	{
		if (strcmp(presets[p].name, name) == 0)
		{
			prop_recipe_t fresh;
			char settings[1024];
			char *token;
			char *rest = NULL;

			// The shape's numbers from the start, the rest kept
			base_defaults(&fresh);
			fresh.seed = recipe->seed;
			memcpy(fresh.output, recipe->output, sizeof(fresh.output));
			memcpy(fresh.textures, recipe->textures, sizeof(fresh.textures));
			memcpy(fresh.metal_textures, recipe->metal_textures, sizeof(fresh.metal_textures));
			fresh.variants = recipe->variants;
			snprintf(settings, sizeof(settings), "%s", presets[p].settings);
			for (token = strtok_r(settings, " ", &rest); token != NULL; token = strtok_r(NULL, " ", &rest))
			{
				char *equals = strchr(token, '=');

				if (equals != NULL)
				{
					*equals = '\0';
					prop_recipe_set(&fresh, token, equals + 1);
				}
			}
			snprintf(fresh.preset, sizeof(fresh.preset), "%s", presets[p].name);
			*recipe = fresh;
			return true;
		}
	}
	prop_error("No preset '%s'", name);
	return false;
}

bool prop_recipe_set(prop_recipe_t *recipe, const char *key, const char *value)
{
	for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++)
	{
		const field_t *field = &fields[f];
		char *target = (char *)recipe + field->offset;

		if (strcmp(field->key, key) != 0)
		{
			continue;
		}
		switch (field->type)
		{
			case FIELD_FLOAT:
				*(float *)(void *)target = strtof(value, NULL);
				return true;
			case FIELD_INT:
				*(int *)(void *)target = (int)strtol(value, NULL, 10);
				return true;
			case FIELD_SEED:
				*(uint32_t *)(void *)target = (uint32_t)strtoul(value, NULL, 10);
				return true;
			case FIELD_TEXT:
				snprintf(target, PROP_PATH_LENGTH, "%s", value);
				return true;
			case FIELD_NAME:
				snprintf(target, PROP_NAME_LENGTH, "%s", value);
				return true;
			case FIELD_KIND:
			{
				int kind = prop_kind_find(value);

				if (kind < 0)
				{
					prop_error("No kind '%s' (rock, wall, fence, barrel)", value);
					return false;
				}
				recipe->kind = (prop_kind_t)kind;
				return true;
			}
		}
	}
	prop_error("No setting '%s'", key);
	return false;
}

static char *trimmed(char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t')
	{
		text++;
	}
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
	{
		*--end = '\0';
	}
	return text;
}

bool prop_recipe_load(prop_recipe_t *recipe, const char *path)
{
	FILE *file = fopen(path, "r");
	char line[PROP_PATH_LENGTH + 64];

	if (file == NULL)
	{
		prop_error("Cannot read '%s'", path);
		return false;
	}
	prop_recipe_defaults(recipe);
	while (fgets(line, sizeof(line), file) != NULL)
	{
		char *text = trimmed(line);
		char *equals;

		if (text[0] == '\0' || text[0] == '#' || text[0] == ';')
		{
			continue;
		}
		equals = strchr(text, '=');
		if (equals == NULL)
		{
			continue;
		}
		*equals = '\0';
		// (an unknown key: skipped, said once)
		if (!prop_recipe_set(recipe, trimmed(text), trimmed(equals + 1)))
		{
			fprintf(stderr, "%s: %s\n", path, prop_last_error());
		}
	}
	fclose(file);
	return true;
}

bool prop_recipe_save(const prop_recipe_t *recipe, const char *path)
{
	FILE *file;

	make_folders(path);
	file = fopen(path, "w");
	if (file == NULL)
	{
		prop_error("Cannot write '%s'", path);
		return false;
	}
	fprintf(file, "# A prop's recipe (tools/potop_prop, tools/prop_editor; see tools/prop_grow.h)\n");
	for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++)
	{
		const field_t *field = &fields[f];
		const char *source = (const char *)recipe + field->offset;

		switch (field->type)
		{
			case FIELD_FLOAT:
				fprintf(file, "%s = %g\n", field->key, (double)*(const float *)(const void *)source);
				break;
			case FIELD_INT:
				fprintf(file, "%s = %d\n", field->key, *(const int *)(const void *)source);
				break;
			case FIELD_SEED:
				fprintf(file, "%s = %u\n", field->key, *(const uint32_t *)(const void *)source);
				break;
			case FIELD_TEXT:
			case FIELD_NAME:
				fprintf(file, "%s = %s\n", field->key, source);
				break;
			case FIELD_KIND:
				fprintf(file, "%s = %s\n", field->key, prop_kind_name(recipe->kind));
				break;
		}
	}
	fclose(file);
	return true;
}

// ---------------------------------------------------------------------------
// Growing, writing

bool prop_grow(const prop_recipe_t *recipe, const char *data_directory, prop_mesh_t *mesh_out)
{
	bool ok;

	memset(mesh_out, 0, sizeof(*mesh_out));
	switch (recipe->kind)
	{
		case PROP_KIND_WALL:
			ok = grow_wall(recipe, mesh_out);
			break;
		case PROP_KIND_FENCE:
			material_begin(mesh_out, "wood");
			ok = grow_fence(recipe, mesh_out);
			material_level_done(mesh_out, &mesh_out->materials[0], 0);
			break;
		case PROP_KIND_BARREL:
			ok = grow_barrel(recipe, mesh_out);
			break;
		case PROP_KIND_ROCK:
		default:
			ok = grow_rock(recipe, mesh_out);
			break;
	}
	if (!ok)
	{
		prop_mesh_free(mesh_out);
		return false;
	}
	mesh_bounds(mesh_out);
	mesh_out->moss = clamp_float(recipe->moss, 0.0f, 1.0f);
	mesh_out->lichen = clamp_float(recipe->lichen, 0.0f, 1.0f);
	// The first material (the stone, the wood): the recipe's textures or its
	// style's; the metal's (a barrel's hoops) the same way
	for (int m = 0; m < mesh_out->material_count; m++)
	{
		prop_material_t *material = &mesh_out->materials[m];
		bool metal = strcmp(material->name, "metal") == 0;
		const char (*given)[PROP_PATH_LENGTH] = metal ? recipe->metal_textures : recipe->textures;
		const char *style = metal ? (recipe->metal_style[0] != '\0' ? recipe->metal_style : "rusty_iron")
			: (recipe->style[0] != '\0' ? recipe->style : (recipe->kind == PROP_KIND_ROCK || recipe->kind == PROP_KIND_WALL ? "granite" : "wood"));

		material->triplanar = recipe->kind == PROP_KIND_ROCK || recipe->kind == PROP_KIND_WALL;
		material->weathered = !metal;
		material->texture_meters = maximum_of(recipe->texture_meters, 0.05f);
		if (given[0][0] != '\0')
		{
			memcpy(material->textures, given, sizeof(material->textures));
			// No arm map: its factors alone say how rough and how metal (as
			// they are, 1 and 1, they multiply a map's: without one the wood
			// would be all metal)
			if (material->textures[2][0] == '\0')
			{
				material->roughness = metal ? 0.6f : 0.85f;
				material->metallic = metal ? 0.7f : 0.0f;
			}
		}
		else if (!prop_style_textures(style, data_directory, false, material->textures))
		{
			prop_mesh_free(mesh_out);
			return false;
		}
	}
	return true;
}

bool prop_write(const char *path, const prop_mesh_t *mesh)
{
	object_file_header_t header;
	FILE *file;
	int texture_count = 0;
	int32_t texture_indices[PROP_MAX_MATERIALS][3];

	make_folders(path);
	file = fopen(path, "wb");
	if (file == NULL)
	{
		prop_error("Cannot write '%s'", path);
		return false;
	}
	// Every material's textures, one after the other (a file named by two
	// is named twice: the game loads it once)
	for (int m = 0; m < mesh->material_count; m++)
	{
		for (int t = 0; t < 3; t++)
		{
			texture_indices[m][t] = mesh->materials[m].textures[t][0] != '\0' ? texture_count++ : -1;
		}
	}
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, OBJECT_FILE_MAGIC, 4);
	header.version = OBJECT_FILE_VERSION;
	header.vertex_count = (uint32_t)mesh->vertex_count;
	header.index_count = (uint32_t)mesh->index_count;
	header.material_count = (uint32_t)mesh->material_count;
	header.texture_count = (uint32_t)texture_count;
	header.flags = OBJECT_FILE_STILL;
	memcpy(header.bounds_minimum, mesh->minimum, sizeof(float) * 3);
	memcpy(header.bounds_maximum, mesh->maximum, sizeof(float) * 3);
	header.moss = mesh->moss;
	header.lichen = mesh->lichen;
	for (int k = 0; k < 3; k++)
	{
		header.lod_errors[k] = prop_mesh_has_levels(mesh) ? mesh->lod_errors[k] : 0.0f;
	}
	fwrite(&header, sizeof(header), 1, file);
	fwrite(mesh->vertices, sizeof(object_file_vertex_t), mesh->vertex_count, file);
	fwrite(mesh->indices, sizeof(uint32_t), mesh->index_count, file);
	for (int m = 0; m < mesh->material_count; m++)
	{
		const prop_material_t *source = &mesh->materials[m];
		object_file_material_t material;

		memset(&material, 0, sizeof(material));
		snprintf(material.name, sizeof(material.name), "%s", source->name);
		material.flags = (source->weathered ? OBJECT_MATERIAL_WEATHERED : 0u) | (source->triplanar ? OBJECT_MATERIAL_TRIPLANAR : 0u);
		material.index_offset = (uint32_t)source->first_index;
		material.index_count = (uint32_t)source->own_index_count;
		// Its levels of detail: meshes of their own, after its own indices
		if (source->lod_index_count[0] > 0)
		{
			material.flags |= OBJECT_MATERIAL_LODS | OBJECT_MATERIAL_LOD_MESHES;
			for (int k = 0; k < 3; k++)
			{
				material.lod_index_count[k] = (uint32_t)source->lod_index_count[k];
			}
		}
		memcpy(material.base_color, source->base_color, sizeof(float) * 3);
		material.base_color[3] = 1.0f;
		material.metallic = source->metallic;
		material.roughness = source->roughness;
		material.normal_scale = 1.0f;
		material.occlusion_strength = 1.0f;
		material.alpha_cutoff = 0.5f;
		material.base_color_texture = texture_indices[m][0];
		material.normal_texture = texture_indices[m][1];
		material.orm_texture = texture_indices[m][2];
		material.emissive_texture = -1;
		material.triplanar_meters = source->texture_meters;
		fwrite(&material, sizeof(material), 1, file);
	}
	for (int m = 0; m < mesh->material_count; m++)
	{
		for (int t = 0; t < 3; t++)
		{
			const char *named = mesh->materials[m].textures[t];
			object_file_texture_t texture;
			size_t length;
			size_t padded;
			char name[PROP_PATH_LENGTH + 4];

			if (named[0] == '\0')
			{
				continue;
			}
			length = strlen(named) + 1;
			padded = (length + 3) & ~(size_t)3;
			memset(&texture, 0, sizeof(texture));
			texture.kind = OBJECT_TEXTURE_REFERENCE | (t == 0 ? OBJECT_TEXTURE_COLOR : OBJECT_TEXTURE_DATA);
			texture.compressed_size = (uint32_t)padded;
			memset(name, 0, sizeof(name));
			memcpy(name, named, length);
			fwrite(&texture, sizeof(texture), 1, file);
			fwrite(name, 1, padded, file);
		}
	}
	fclose(file);
	return true;
}
