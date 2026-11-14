// The clouds' shapes and where they are. See cloud_weather.h.

#include "cloud_weather.h"
#include "file_io.h"
#include "math3d.h"

#include <SDL3/SDL.h>

#include "miniz.h"

#define SHAPES_PER_FAMILY CLOUD_SHAPES_PER_TYPE
#define ATLAS_WIDTH (CLOUD_SHAPE_TEXELS * CLOUD_ATLAS_COLUMNS)
#define ATLAS_HEIGHT CLOUD_SHAPE_TEXELS_UP
#define ATLAS_DEPTH (CLOUD_SHAPE_TEXELS * CLOUD_ATLAS_ROWS)
// Remaking the stamps takes some milliseconds: not more often than this
// while a setting is being dragged
#define REBUILD_INTERVAL_MILLISECONDS 200
// Cumulus have flat bottoms, this high in the shape (of its width)
#define FLOOR_HEIGHT 0.01f

typedef enum
{
	FAMILY_HUMILIS = 0,    // fair weather puffs: low, flat topped
	FAMILY_MEDIOCRIS,      // heaps, a second tier of billows
	FAMILY_CONGESTUS,      // a tower rising from the heap
	FAMILY_CUMULONIMBUS,   // a fat tower spreading into an anvil downwind
	FAMILY_STRATUS,        // flat sheets
	FAMILY_COUNT
} family_t;

// An ellipsoid of a shape (units: the shape's width; x and z from -0.5 to
// 0.5, y up from 0)
typedef struct
{
	float x;
	float y;
	float z;
	float radius;             // across
	float radius_up;
	float stretch;            // along x
} blob_t;

// A stamp as the shader reads it: three vec4
typedef struct
{
	float x;                  // center (meters, 0 .. CLOUD_WEATHER_METERS)
	float z;
	float width;              // meters
	float base;               // meters over the layer's bottom
	float cosine;             // turned by
	float sine;
	float shape;
	float kind;               // 0: a shape of the blob atlas; else a density shape, this dense, mirrored if negative
	float life_phase;         // where in its life it is at time 0 (0..1)
	float life_seconds;       // how long a life (0: it stays)
	float spare[2];
} stamp_t;

// ---------------------------------------------------------------------------
// Random numbers

static uint32_t next_random(uint32_t *state)
{
	uint32_t value = *state = *state * 747796405u + 2891336453u;

	value = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
	return (value >> 22u) ^ value;
}

static float random_unit(uint32_t *state)
{
	return (float)(next_random(state) >> 8) * (1.0f / 16777216.0f);
}

static float random_range(uint32_t *state, float low, float high)
{
	return low + (high - low) * random_unit(state);
}

// ---------------------------------------------------------------------------
// The atlas

typedef struct
{
	blob_t *blobs;
	int32_t count;
	int32_t capacity;
} blob_list_t;

static void add_blob(blob_list_t *list, float x, float y, float z, float radius, float radius_up, float stretch)
{
	if (list->count < list->capacity)
	{
		blob_t *blob = &list->blobs[list->count++];

		// Inside the shape's square, whatever the randomness asked for
		blob->radius = radius;
		blob->radius_up = radius_up;
		blob->stretch = stretch;
		blob->x = clamp_float(x, -0.46f + radius * stretch, 0.46f - radius * stretch);
		blob->z = clamp_float(z, -0.46f + radius, 0.46f - radius);
		blob->y = clamp_float(y, 0.0f, CLOUD_SHAPE_REACH_UP - 0.02f - radius_up);
	}
}

// A ring of lumps around (x, z) at height y: a cloud's tier
static void add_tier(blob_list_t *list, uint32_t *random, int32_t lumps, float x, float z, float reach, float y, float low_radius,
	float high_radius)
{
	for (int32_t i = 0; i < lumps; i++)
	{
		float angle = random_range(random, 0.0f, 6.2831853f);
		float distance = reach * SDL_sqrtf(random_unit(random));
		float radius = random_range(random, low_radius, high_radius);

		add_blob(list, x + SDL_cosf(angle) * distance, y + radius * random_range(random, 0.4f, 0.7f), z + SDL_sinf(angle) * distance * 0.8f,
			radius, radius * random_range(random, 0.75f, 0.95f), 1.0f);
	}
}

// A column of lumps rising from y to top, as wide as a good part of the
// cloud (a heap growing up, not a pole): at each level a middle lump and a
// ring of side lumps, narrowing slowly upwards
static void add_tower(blob_list_t *list, uint32_t *random, float x, float z, float y, float top, float radius, int32_t sides)
{
	while (y < top)
	{
		float turn = random_range(random, 0.0f, 6.2831853f);

		x += random_range(random, -0.015f, 0.015f);
		z += random_range(random, -0.015f, 0.015f);
		add_blob(list, x, y, z, radius, radius * 0.85f, 1.0f);
		for (int32_t i = 0; i < sides; i++)
		{
			float angle = turn + 6.2831853f * (float)i / (float)sides + random_range(random, -0.4f, 0.4f);
			float side = radius * random_range(random, 0.65f, 0.85f);

			add_blob(list, x + SDL_cosf(angle) * radius * 0.75f, y + random_range(random, -0.3f, 0.3f) * radius,
				z + SDL_sinf(angle) * radius * 0.75f, side, side * 0.85f, 1.0f);
		}
		y += radius * 0.7f;
		radius *= 0.95f;
	}
}

// Cumulus grow in heaps of heaps: on the surface of each lump smaller ones
// bulge out (mostly upwards and sideways, never under the flat bottom),
// and on those smaller ones still. Flat blobs (anvils, sheets) get none.
// upward: how much the new lumps lean up rather than out
static void add_children(blob_list_t *list, uint32_t *random, int32_t first, int32_t last, int32_t per_blob, float scale, float upward)
{
	for (int32_t b = first; b < last; b++)
	{
		blob_t parent = list->blobs[b];

		if (parent.radius_up < parent.radius * 0.5f || parent.stretch > 1.01f)
		{
			continue;
		}
		for (int32_t i = 0; i < per_blob; i++)
		{
			float dx = random_range(random, -1.0f, 1.0f);
			float dy = random_range(random, -0.3f, 1.0f) + upward;
			float dz = random_range(random, -1.0f, 1.0f);
			float length = SDL_sqrtf(dx * dx + dy * dy + dz * dz);
			float radius = parent.radius * scale * random_range(random, 0.8f, 1.2f);

			if (length < 1e-3f)
			{
				continue;
			}
			dx /= length;
			dy /= length;
			dz /= length;
			add_blob(list, parent.x + dx * parent.radius * 0.8f, parent.y + dy * parent.radius_up * 0.8f, parent.z + dz * parent.radius,
				radius, radius * 0.85f, 1.0f);
		}
	}
}

static int32_t make_blobs(family_t family, uint32_t *random, blob_t *blobs, int32_t capacity)
{
	blob_list_t list = { blobs, 0, capacity };
	float upward = 0.6f;
	int32_t generations = 2;

	switch (family)
	{
	case FAMILY_HUMILIS:
		// Wide and low: a few lumps side by side, the bulges leaning out
		add_tier(&list, random, 3 + (int32_t)(random_unit(random) * 3.0f), 0.0f, 0.0f, 0.2f, 0.0f, 0.09f, 0.13f);
		upward = 0.1f;
		break;
	case FAMILY_MEDIOCRIS:
		add_tier(&list, random, 5 + (int32_t)(random_unit(random) * 3.0f), 0.0f, 0.0f, 0.22f, 0.0f, 0.1f, 0.13f);
		add_tier(&list, random, 2 + (int32_t)(random_unit(random) * 2.0f), random_range(random, -0.05f, 0.05f),
			random_range(random, -0.05f, 0.05f), 0.08f, 0.1f, 0.09f, 0.12f);
		break;
	case FAMILY_CONGESTUS:
	{
		float x = random_range(random, -0.05f, 0.05f);
		float z = random_range(random, -0.05f, 0.05f);

		add_tier(&list, random, 5 + (int32_t)(random_unit(random) * 3.0f), 0.0f, 0.0f, 0.22f, 0.0f, 0.1f, 0.13f);
		add_tower(&list, random, x, z, 0.12f, random_range(random, 0.42f, 0.52f), 0.12f, 3);
		upward = 0.8f;
		break;
	}
	case FAMILY_CUMULONIMBUS:
	{
		float x = random_range(random, -0.1f, -0.06f);
		float z = random_range(random, -0.03f, 0.03f);

		// A massive base, a fat tower, an anvil spreading downwind (+x)
		add_tier(&list, random, 7 + (int32_t)(random_unit(random) * 3.0f), x, 0.0f, 0.22f, 0.0f, 0.11f, 0.14f);
		add_tower(&list, random, x, z, 0.12f, 0.5f, 0.14f, 4);
		add_blob(&list, x + 0.17f, 0.62f, z, 0.2f, 0.045f, 1.35f);
		add_blob(&list, x + 0.08f, 0.6f, z + random_range(random, -0.05f, 0.05f), 0.16f, 0.05f, 1.2f);
		add_blob(&list, x + 0.02f, 0.57f, z, 0.13f, 0.07f, 1.0f);
		upward = 0.8f;
		break;
	}
	case FAMILY_STRATUS:
	default:
	{
		int32_t slabs = 4 + (int32_t)(random_unit(random) * 3.0f);

		for (int32_t i = 0; i < slabs; i++)
		{
			float radius = random_range(random, 0.18f, 0.25f);
			float radius_up = random_range(random, 0.035f, 0.055f);

			add_blob(&list, random_range(random, -0.1f, 0.1f), 0.02f + radius_up, random_range(random, -0.1f, 0.1f), radius, radius_up,
				random_range(random, 1.0f, 1.3f));
		}
		generations = 0;
		break;
	}
	}
	{
		int32_t first = 0;

		for (int32_t generation = 0; generation < generations; generation++)
		{
			int32_t last = list.count;

			add_children(&list, random, first, last, generation == 0 ? 4 : 2, generation == 0 ? 0.55f : 0.5f, upward);
			first = last;
		}
	}
	return list.count;
}

// A shape's distance field into its cell of the atlas: the signed distance
// to the union of its blobs (approximate for ellipsoids), stored as 0.5 at
// the surface, more inside, and saturating CLOUD_SHAPE_DISTANCE_RANGE away
// either way. Each blob only touches the texels within that range of it;
// returns the shape's highest point
static float make_shape(uint8_t *atlas, int32_t shape, family_t family)
{
	blob_t blobs[640];
	uint32_t random = (uint32_t)shape * 2654435761u + 12345u;
	int32_t count = make_blobs(family, &random, blobs, (int32_t)SDL_arraysize(blobs));
	int32_t cell_x = (shape % CLOUD_ATLAS_COLUMNS) * CLOUD_SHAPE_TEXELS;
	int32_t cell_z = (shape / CLOUD_ATLAS_COLUMNS) * CLOUD_SHAPE_TEXELS;
	size_t texel_count = (size_t)CLOUD_SHAPE_TEXELS * CLOUD_SHAPE_TEXELS_UP * CLOUD_SHAPE_TEXELS;
	float *field = SDL_malloc(sizeof(float) * texel_count);
	float texel_across = 1.0f / (float)CLOUD_SHAPE_TEXELS;
	float texel_up = CLOUD_SHAPE_REACH_UP / (float)CLOUD_SHAPE_TEXELS_UP;
	float top = 0.0f;

	if (field == NULL)
	{
		return CLOUD_SHAPE_REACH_UP;
	}
	for (size_t t = 0; t < texel_count; t++)
	{
		field[t] = CLOUD_SHAPE_DISTANCE_RANGE;
	}
	for (int32_t b = 0; b < count; b++)
	{
		const blob_t *blob = &blobs[b];
		float across = blob->radius * blob->stretch;
		float smallest = SDL_min(SDL_min(across, blob->radius_up), blob->radius);
		int32_t low_i = SDL_max((int32_t)SDL_floorf((blob->x - across - CLOUD_SHAPE_DISTANCE_RANGE + 0.5f) / texel_across), 0);
		int32_t high_i = SDL_min((int32_t)SDL_ceilf((blob->x + across + CLOUD_SHAPE_DISTANCE_RANGE + 0.5f) / texel_across), CLOUD_SHAPE_TEXELS - 1);
		int32_t low_j = SDL_max((int32_t)SDL_floorf((blob->y - blob->radius_up - CLOUD_SHAPE_DISTANCE_RANGE) / texel_up), 0);
		int32_t high_j = SDL_min((int32_t)SDL_ceilf((blob->y + blob->radius_up + CLOUD_SHAPE_DISTANCE_RANGE) / texel_up),
			CLOUD_SHAPE_TEXELS_UP - 1);
		int32_t low_k = SDL_max((int32_t)SDL_floorf((blob->z - blob->radius - CLOUD_SHAPE_DISTANCE_RANGE + 0.5f) / texel_across), 0);
		int32_t high_k = SDL_min((int32_t)SDL_ceilf((blob->z + blob->radius + CLOUD_SHAPE_DISTANCE_RANGE + 0.5f) / texel_across),
			CLOUD_SHAPE_TEXELS - 1);

		top = SDL_max(top, blob->y + blob->radius_up);
		for (int32_t k = low_k; k <= high_k; k++)
		{
			for (int32_t j = low_j; j <= high_j; j++)
			{
				for (int32_t i = low_i; i <= high_i; i++)
				{
					float qx = (((float)i + 0.5f) * texel_across - 0.5f - blob->x) / across;
					float qy = (((float)j + 0.5f) * texel_up - blob->y) / blob->radius_up;
					float qz = (((float)k + 0.5f) * texel_across - 0.5f - blob->z) / blob->radius;
					float distance = (SDL_sqrtf(qx * qx + qy * qy + qz * qz) - 1.0f) * smallest;
					float *value = &field[((size_t)k * CLOUD_SHAPE_TEXELS_UP + (size_t)j) * CLOUD_SHAPE_TEXELS + (size_t)i];

					*value = SDL_min(*value, distance);
				}
			}
		}
	}
	for (int32_t k = 0; k < CLOUD_SHAPE_TEXELS; k++)
	{
		for (int32_t j = 0; j < CLOUD_SHAPE_TEXELS_UP; j++)
		{
			float y = ((float)j + 0.5f) * texel_up;

			for (int32_t i = 0; i < CLOUD_SHAPE_TEXELS; i++)
			{
				float distance = field[((size_t)k * CLOUD_SHAPE_TEXELS_UP + (size_t)j) * CLOUD_SHAPE_TEXELS + (size_t)i];
				size_t index = ((size_t)(cell_z + k) * ATLAS_HEIGHT + (size_t)j) * ATLAS_WIDTH + (size_t)(cell_x + i);

				if (family != FAMILY_STRATUS)
				{
					// Cumulus have flat bottoms
					distance = SDL_max(distance, FLOOR_HEIGHT - y);
				}
				atlas[index] = (uint8_t)SDL_clamp((int32_t)((0.5f - distance / CLOUD_SHAPE_DISTANCE_RANGE * 0.5f) * 255.0f + 0.5f), 0, 255);
			}
		}
	}
	SDL_free(field);
	return SDL_min(top, CLOUD_SHAPE_REACH_UP);
}

// Shapes grown by tools/cloud_bake (data/clouds/<family>_<n>.cloud, n from
// 1) take the place of the shapes made of blobs: a family's cells of the
// atlas take its simulated clouds in turn. A family without any keeps its
// blobs
static bool load_simulated_shape(const char *path, uint8_t *texels_out, float *top_out)
{
	size_t size = 0;
	uint8_t *file = file_io_load_data_file(path, &size);
	size_t texels = (size_t)CLOUD_SHAPE_TEXELS * CLOUD_SHAPE_TEXELS_UP * CLOUD_SHAPE_TEXELS;
	uint32_t header[4];
	bool ok;

	if (file == NULL)
	{
		return false;
	}
	ok = size == sizeof(header) + sizeof(float) + texels;
	if (ok)
	{
		SDL_memcpy(header, file, sizeof(header));
		SDL_memcpy(top_out, file + sizeof(header), sizeof(float));
		ok = header[0] == 0x444c4350u && header[1] == CLOUD_SHAPE_TEXELS && header[2] == CLOUD_SHAPE_TEXELS_UP
			&& header[3] == CLOUD_SHAPE_TEXELS;
	}
	if (ok)
	{
		SDL_memcpy(texels_out, file + sizeof(header) + sizeof(float), texels);
	}
	SDL_free(file);
	return ok;
}

static void load_simulated(cloud_weather_t *weather, uint8_t *atlas)
{
	static const char *const family_names[FAMILY_COUNT] = { "humilis", "mediocris", "congestus", "storm", "stratus" };
	size_t texels = (size_t)CLOUD_SHAPE_TEXELS * CLOUD_SHAPE_TEXELS_UP * CLOUD_SHAPE_TEXELS;
	uint8_t *shapes = SDL_malloc(texels * SHAPES_PER_FAMILY);
	float tops[SHAPES_PER_FAMILY];

	if (shapes == NULL)
	{
		return;
	}
	for (int32_t family = 0; family < FAMILY_COUNT; family++)
	{
		int32_t found = 0;

		for (int32_t n = 1; n <= SHAPES_PER_FAMILY; n++)
		{
			char path[64];

			SDL_snprintf(path, sizeof(path), "clouds/%s_%d.cloud", family_names[family], (int)n);
			if (load_simulated_shape(path, shapes + texels * (size_t)found, &tops[found]))
			{
				found++;
			}
		}
		for (int32_t slot = 0; slot < SHAPES_PER_FAMILY && found > 0; slot++)
		{
			int32_t shape = family * SHAPES_PER_FAMILY + slot;
			int32_t cell_x = (shape % CLOUD_ATLAS_COLUMNS) * CLOUD_SHAPE_TEXELS;
			int32_t cell_z = (shape / CLOUD_ATLAS_COLUMNS) * CLOUD_SHAPE_TEXELS;
			const uint8_t *source = shapes + texels * (size_t)(slot % found);

			for (int32_t k = 0; k < CLOUD_SHAPE_TEXELS; k++)
			{
				for (int32_t j = 0; j < CLOUD_SHAPE_TEXELS_UP; j++)
				{
					SDL_memcpy(&atlas[((size_t)(cell_z + k) * ATLAS_HEIGHT + (size_t)j) * ATLAS_WIDTH + (size_t)cell_x],
						&source[((size_t)k * CLOUD_SHAPE_TEXELS_UP + (size_t)j) * CLOUD_SHAPE_TEXELS], CLOUD_SHAPE_TEXELS);
				}
			}
			weather->shape_tops[shape] = tops[slot % found];
		}
		if (found > 0)
		{
			SDL_Log("Clouds: %d simulated %s shape(s)", (int)found, family_names[family]);
		}
	}
	SDL_free(shapes);
}

// ---------------------------------------------------------------------------
// Clouds made elsewhere: their density

// tools/cloud_bake's density cloud: "PCL2", texels across, up, deep, a
// texel's meters, the zlib packed size, then the density (x fastest)
#define DENSITY_CLOUD_MAGIC 0x324c4350u
// The density atlas's sides at most (SDL's limit for 3D textures)
#define DENSITY_ATLAS_MAXIMUM 2048

typedef struct
{
	uint8_t *texels;
	int32_t size[3];
	float meters;
} density_cloud_t;

static bool load_density_cloud(const char *path, density_cloud_t *cloud_out)
{
	size_t size = 0;
	uint8_t *file = file_io_load_data_file(path, &size);
	uint32_t header[6];
	mz_ulong unpacked;
	size_t count;
	bool ok;

	SDL_zerop(cloud_out);
	if (file == NULL)
	{
		return false;
	}
	ok = size >= sizeof(header);
	if (ok)
	{
		SDL_memcpy(header, file, sizeof(header));
		ok = header[0] == DENSITY_CLOUD_MAGIC && header[1] >= 4 && header[2] >= 4 && header[3] >= 1 && header[1] % 4 == 0 && header[2] % 4 == 0
			&& header[1] <= 512 && header[2] <= 512 && header[3] <= 512 && size >= sizeof(header) + header[5];
	}
	if (ok)
	{
		count = (size_t)header[1] * header[2] * header[3];
		cloud_out->texels = SDL_malloc(count);
		unpacked = (mz_ulong)count;
		ok = cloud_out->texels != NULL
			&& mz_uncompress(cloud_out->texels, &unpacked, file + sizeof(header), (mz_ulong)header[5]) == MZ_OK && unpacked == count;
	}
	if (ok)
	{
		for (int32_t a = 0; a < 3; a++)
		{
			cloud_out->size[a] = (int32_t)header[1 + a];
		}
		SDL_memcpy(&cloud_out->meters, &header[4], sizeof(float));
	}
	else
	{
		SDL_free(cloud_out->texels);
		cloud_out->texels = NULL;
		SDL_Log("Clouds: %s is not a density cloud", path);
	}
	SDL_free(file);
	return ok;
}

// A BC4 block: 16 values (x fastest) as their two ends and 3 bits each for
// one of eight steps between them
static void encode_bc4_block(const uint8_t values[16], uint8_t block_out[8])
{
	uint8_t high = 0;
	uint8_t low = 255;
	uint64_t bits = 0;

	for (int32_t v = 0; v < 16; v++)
	{
		high = SDL_max(high, values[v]);
		low = SDL_min(low, values[v]);
	}
	if (high == low)
	{
		// Code 0 everywhere: the first end
		SDL_memset(block_out, 0, 8);
		block_out[0] = high;
		block_out[1] = low;
		return;
	}
	for (int32_t v = 0; v < 16; v++)
	{
		// Codes: 0 the high end, 1 the low end, 2..7 the steps from high to low
		int32_t step = (int32_t)SDL_lroundf((float)(high - values[v]) * 7.0f / (float)(high - low));
		uint64_t code = step == 0 ? 0u : step == 7 ? 1u : (uint64_t)(step + 1);

		bits |= code << (3 * v);
	}
	block_out[0] = high;
	block_out[1] = low;
	for (int32_t b = 0; b < 6; b++)
	{
		block_out[2 + b] = (uint8_t)(bits >> (8 * b));
	}
}

// The atlas (width x height x depth, x fastest) as BC4 blocks: 4 x 4 texels
// of each slice in 8 bytes, slice after slice
static uint8_t *encode_bc4(const uint8_t *texels, int32_t width, int32_t height, int32_t depth, size_t *size_out)
{
	int32_t blocks_x = width / 4;
	int32_t blocks_y = height / 4;
	size_t size = (size_t)blocks_x * blocks_y * depth * 8;
	uint8_t *blocks = SDL_malloc(size);

	*size_out = size;
	if (blocks == NULL)
	{
		return NULL;
	}
#pragma omp parallel for schedule(static)
	for (int32_t z = 0; z < depth; z++)
	{
		for (int32_t by = 0; by < blocks_y; by++)
		{
			for (int32_t bx = 0; bx < blocks_x; bx++)
			{
				uint8_t values[16];

				for (int32_t y = 0; y < 4; y++)
				{
					SDL_memcpy(&values[y * 4], &texels[((size_t)z * height + (size_t)(by * 4 + y)) * width + (size_t)(bx * 4)], 4);
				}
				encode_bc4_block(values, &blocks[(((size_t)z * blocks_y + (size_t)by) * blocks_x + (size_t)bx) * 8]);
			}
		}
	}
	return blocks;
}

// data/clouds/cumulus_<n>.cloud into one atlas: the clouds side by side in
// rows (across x, the rows along z), all from the bottom; BC4 if the GPU
// samples it in 3D, else a byte a texel. Without any, a one texel atlas
static bool make_density_atlas(cloud_weather_t *weather)
{
	density_cloud_t clouds[CLOUD_DENSITY_SHAPES_MAX];
	int32_t count = 0;
	int32_t width = 4;
	int32_t height = 4;
	int32_t depth = 1;
	int32_t row_x = 0;
	int32_t row_z = 0;
	int32_t row_depth = 0;
	uint8_t *atlas;
	size_t texel_count;

	for (int32_t n = 1; n <= CLOUD_DENSITY_SHAPES_MAX && count < CLOUD_DENSITY_SHAPES_MAX; n++)
	{
		char path[64];

		SDL_snprintf(path, sizeof(path), "clouds/cumulus_%d.cloud", (int)n);
		if (load_density_cloud(path, &clouds[count]))
		{
			count++;
		}
	}
	// Where each goes
	for (int32_t c = 0; c < count; c++)
	{
		cloud_density_shape_t *shape = &weather->density_shapes[c];

		if (row_x + clouds[c].size[0] > DENSITY_ATLAS_MAXIMUM)
		{
			row_x = 0;
			row_z += row_depth;
			row_depth = 0;
		}
		if (row_z + clouds[c].size[2] > DENSITY_ATLAS_MAXIMUM)
		{
			SDL_Log("Clouds: the density atlas is full, %d cloud(s) left out", (int)(count - c));
			for (int32_t left = c; left < count; left++)
			{
				SDL_free(clouds[left].texels);
			}
			count = c;
			break;
		}
		shape->origin[0] = row_x;
		shape->origin[1] = 0;
		shape->origin[2] = row_z;
		SDL_memcpy(shape->size, clouds[c].size, sizeof(shape->size));
		shape->meters = clouds[c].meters;
		row_x += clouds[c].size[0];
		row_depth = SDL_max(row_depth, clouds[c].size[2]);
		width = SDL_max(width, row_x);
		height = SDL_max(height, clouds[c].size[1]);
		depth = SDL_max(depth, row_z + row_depth);
	}
	weather->density_shape_count = count;
	texel_count = (size_t)width * height * depth;
	atlas = SDL_calloc(texel_count, 1);
	if (atlas == NULL)
	{
		for (int32_t c = 0; c < count; c++)
		{
			SDL_free(clouds[c].texels);
		}
		weather->density_shape_count = 0;
		return false;
	}
	for (int32_t c = 0; c < count; c++)
	{
		const cloud_density_shape_t *shape = &weather->density_shapes[c];

		for (int32_t k = 0; k < shape->size[2]; k++)
		{
			for (int32_t j = 0; j < shape->size[1]; j++)
			{
				SDL_memcpy(&atlas[((size_t)(shape->origin[2] + k) * height + (size_t)j) * width + (size_t)shape->origin[0]],
					&clouds[c].texels[((size_t)k * shape->size[1] + (size_t)j) * shape->size[0]], (size_t)shape->size[0]);
			}
		}
		SDL_free(clouds[c].texels);
	}
	{
		bool compressed = count > 0
			&& SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM, SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		size_t size = texel_count;
		uint8_t *data = compressed ? encode_bc4(atlas, width, height, depth, &size) : atlas;

		if (data == NULL)
		{
			compressed = false;
			data = atlas;
			size = texel_count;
		}
		weather->density_atlas_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_3D,
			compressed ? SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM : SDL_GPU_TEXTUREFORMAT_R8_UNORM, (uint32_t)width, (uint32_t)height, (uint32_t)depth, 1,
			SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (weather->density_atlas_texture != NULL)
		{
			gpu_upload_volume(weather->density_atlas_texture, (uint32_t)width, (uint32_t)height, (uint32_t)depth, data, size);
		}
		if (count > 0)
		{
			SDL_Log("Clouds: %d cloud(s) of real density, an atlas of %d x %d x %d (%s, %.0f MB)", (int)count, (int)width, (int)height,
				(int)depth, compressed ? "BC4" : "R8", (double)size / 1048576.0);
		}
		if (data != atlas)
		{
			SDL_free(data);
		}
	}
	SDL_free(atlas);
	return weather->density_atlas_texture != NULL;
}

static bool make_atlas(cloud_weather_t *weather)
{
	size_t size = (size_t)ATLAS_WIDTH * ATLAS_HEIGHT * ATLAS_DEPTH;
	uint8_t *atlas = SDL_calloc(size, 1);
	int32_t shape_count = FAMILY_COUNT * SHAPES_PER_FAMILY;

	if (atlas == NULL)
	{
		return false;
	}
#pragma omp parallel for schedule(dynamic, 1)
	for (int32_t s = 0; s < shape_count; s++)
	{
		weather->shape_tops[s] = make_shape(atlas, s, (family_t)(s / SHAPES_PER_FAMILY));
	}
	load_simulated(weather, atlas);
	weather->atlas_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R8_UNORM, ATLAS_WIDTH, ATLAS_HEIGHT,
		ATLAS_DEPTH, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (weather->atlas_texture != NULL)
	{
		gpu_upload_volume(weather->atlas_texture, ATLAS_WIDTH, ATLAS_HEIGHT, ATLAS_DEPTH, atlas, size);
	}
	SDL_free(atlas);
	weather->atlas_made = weather->atlas_texture != NULL && make_density_atlas(weather);
	return weather->atlas_made;
}

// ---------------------------------------------------------------------------
// The stamps

// A smooth random field over the map, 0..1, repeating with it (groups of
// clouds with clear sky between them)
static float cluster_field(float x, float z, int32_t period, uint32_t seed)
{
	float u = x / CLOUD_WEATHER_METERS * (float)period;
	float v = z / CLOUD_WEATHER_METERS * (float)period;
	int32_t cell_u = (int32_t)SDL_floorf(u);
	int32_t cell_v = (int32_t)SDL_floorf(v);
	float fu = u - (float)cell_u;
	float fv = v - (float)cell_v;
	float corners[4];

	fu = fu * fu * (3.0f - 2.0f * fu);
	fv = fv * fv * (3.0f - 2.0f * fv);
	for (int32_t c = 0; c < 4; c++)
	{
		uint32_t wrapped_u = (uint32_t)(((cell_u + (c & 1)) % period + period) % period);
		uint32_t wrapped_v = (uint32_t)(((cell_v + (c >> 1)) % period + period) % period);
		uint32_t state = wrapped_u * 73856093u ^ wrapped_v * 19349663u ^ seed;

		corners[c] = random_unit(&state);
	}
	return (corners[0] + (corners[1] - corners[0]) * fu) * (1.0f - fv) + (corners[2] + (corners[3] - corners[2]) * fu) * fv;
}

static family_t pick_family(float towers, uint32_t *random)
{
	// Shares of each cumulus family with no towers, half and full, blended
	static const float shares[3][4] =
	{
		{ 0.6f, 0.4f, 0.0f, 0.0f },
		{ 0.1f, 0.6f, 0.3f, 0.0f },
		{ 0.0f, 0.3f, 0.4f, 0.3f },
	};
	float position = clamp_float(towers, 0.0f, 1.0f) * 2.0f;
	int32_t low = SDL_min((int32_t)position, 1);
	float blend = position - (float)low;
	float pick = random_unit(random);

	for (int32_t f = 0; f < 4; f++)
	{
		float share = shares[low][f] + (shares[low + 1][f] - shares[low][f]) * blend;

		if (pick < share)
		{
			return (family_t)f;
		}
		pick -= share;
	}
	return FAMILY_MEDIOCRIS;
}

// A density shape's top (of its width: its longest side across)
static float density_shape_top(const cloud_weather_t *weather, int32_t shape)
{
	const cloud_density_shape_t *density = &weather->density_shapes[shape];

	return (float)density->size[1] / (float)SDL_max(SDL_max(density->size[0], density->size[2]), 1);
}

// The painted sky at a point (neutral without one)
static float painted(const cloud_paint_t *paint, cloud_paint_channel_t channel, float x, float z)
{
	return paint != NULL ? cloud_paint_sample(paint, channel, x, z) : 0.5f;
}

static int32_t place_stamps(const cloud_weather_t *weather, const render_settings_t *settings, const cloud_paint_t *paint,
	stamp_t **stamps_out)
{
	float spacing = SDL_max(settings->cloud_tiling_meters, 300.0f);
	int32_t cells = SDL_max((int32_t)SDL_lroundf(CLOUD_WEATHER_METERS / spacing), 2);
	float cell_meters = CLOUD_WEATHER_METERS / (float)cells;
	float coverage = clamp_float(settings->cloud_coverage, 0.0f, 1.0f);
	bool overcast = coverage >= 0.9f;
	float size = SDL_sqrtf(clamp_float(settings->cloud_size, 0.25f, 8.0f));
	float thickness = SDL_max(settings->cloud_thickness_meters, 10.0f);
	float wind = degrees_to_radians(settings->wind_direction_degrees);
	int32_t per_cell = overcast ? 3 : 1;
	stamp_t *stamps = SDL_malloc(sizeof(stamp_t) * (size_t)(cells * cells * per_cell));
	int32_t count = 0;

	if (stamps == NULL)
	{
		*stamps_out = NULL;
		return 0;
	}
	for (int32_t cell_z = 0; cell_z < cells; cell_z++)
	{
		for (int32_t cell_x = 0; cell_x < cells; cell_x++)
		{
			uint32_t random = (uint32_t)(cell_z * cells + cell_x) * 2654435761u + 977u;

			for (int32_t k = 0; k < per_cell; k++)
			{
				float x = ((float)cell_x + random_unit(&random)) * cell_meters;
				float z = ((float)cell_z + random_unit(&random)) * cell_meters;
				float cluster = cluster_field(x, z, 6, 4242u) * 0.7f + cluster_field(x, z, 17, 777u) * 0.3f;
				// The painted sky: twice as many clouds .. none (see cloud_paint.h)
				float more = painted(paint, CLOUD_PAINT_COVERAGE, x, z) * 2.0f;
				float chance = clamp_float(SDL_min(coverage * more, 1.0f) * 1.5f + (cluster - 0.5f) * 1.2f * SDL_min(more, 1.0f), 0.0f, 1.0f)
					* clamp_float(more * 4.0f, 0.0f, 1.0f);
				float towers = clamp_float(settings->cloud_towers + (painted(paint, CLOUD_PAINT_KIND, x, z) - 0.5f) * 2.0f, 0.0f, 1.0f);
				float heavier = SDL_powf(4.0f, (painted(paint, CLOUD_PAINT_DENSITY, x, z) - 0.5f) * 2.0f);
				family_t family;
				float angle = random_range(&random, 0.0f, 6.2831853f);
				float width;
				int32_t shape;
				stamp_t *stamp;

				if (!overcast && random_unit(&random) >= chance)
				{
					continue;
				}
				family = overcast ? FAMILY_STRATUS : pick_family(towers, &random);
				shape = (int32_t)family * SHAPES_PER_FAMILY + (int32_t)(random_unit(&random) * SHAPES_PER_FAMILY) % SHAPES_PER_FAMILY;
				width = cell_meters * random_range(&random, 0.6f, 1.0f) * size;
				if (family != FAMILY_STRATUS && weather->density_shape_count > 0)
				{
					// A cloud of real density: the flatter of two picks for fair
					// weather puffs, the taller for towers; smaller or bigger
					// with its family
					static const float family_sizes[FAMILY_COUNT] = { 0.5f, 0.8f, 1.0f, 1.3f, 1.0f };
					int32_t first = (int32_t)(random_unit(&random) * (float)weather->density_shape_count) % weather->density_shape_count;
					int32_t second = (int32_t)(random_unit(&random) * (float)weather->density_shape_count) % weather->density_shape_count;
					float first_tall = density_shape_top(weather, first);
					float second_tall = density_shape_top(weather, second);
					int32_t picked = first;
					float top;

					if (family == FAMILY_HUMILIS)
					{
						picked = second_tall < first_tall ? second : first;
					}
					else if (family == FAMILY_CONGESTUS || family == FAMILY_CUMULONIMBUS)
					{
						picked = second_tall > first_tall ? second : first;
					}
					top = density_shape_top(weather, picked);
					width *= family_sizes[family];
					width = SDL_min(width, thickness / SDL_max(top, 0.05f));
					stamp = &stamps[count++];
					stamp->x = x;
					stamp->z = z;
					stamp->width = width;
					stamp->base = random_range(&random, 0.0f, 0.04f) * thickness;
					stamp->cosine = SDL_cosf(angle);
					stamp->sine = SDL_sinf(angle);
					stamp->shape = (float)picked;
					// How dense this one is (white and puffy .. heavy and dark:
					// up to four times either way at full variety; fair weather
					// puffs lighter), mirrored or not (the sign)
					stamp->kind = SDL_powf(4.0f, clamp_float(settings->cloud_density_variation, 0.0f, 1.0f) * random_range(&random, -1.0f, 1.0f))
						* (family == FAMILY_HUMILIS ? 0.6f : 1.0f) * heavier;
					if (random_unit(&random) < 0.5f)
					{
						stamp->kind = -stamp->kind;
					}
					// Its life: somewhere in it now, as long as the setting
					// says give or take a third (so they do not all go together)
					stamp->life_phase = random_unit(&random);
					stamp->life_seconds = SDL_max(settings->cloud_lifetime_minutes, 0.0f) * 60.0f * random_range(&random, 0.67f, 1.33f);
					stamp->spare[0] = 0.0f;
					stamp->spare[1] = 0.0f;
					continue;
				}
				if (family == FAMILY_CUMULONIMBUS)
				{
					// Big, the anvil downwind
					width *= 1.3f;
					angle = wind + random_range(&random, -0.3f, 0.3f);
				}
				else if (family == FAMILY_STRATUS)
				{
					width = cell_meters * random_range(&random, 1.6f, 2.4f) * size;
				}
				// No taller than the layer
				width = SDL_min(width, thickness / SDL_max(weather->shape_tops[shape], 0.05f));
				stamp = &stamps[count++];
				stamp->x = x;
				stamp->z = z;
				stamp->width = width;
				stamp->base = family == FAMILY_STRATUS
					? random_range(&random, 0.0f, SDL_max(thickness - weather->shape_tops[shape] * width, 0.0f))
					: random_range(&random, 0.0f, 0.04f) * thickness;
				stamp->cosine = SDL_cosf(angle);
				stamp->sine = SDL_sinf(angle);
				stamp->shape = (float)shape;
				stamp->kind = 0.0f;
				stamp->life_phase = 0.0f;
				stamp->life_seconds = 0.0f;
				stamp->spare[0] = 0.0f;
				stamp->spare[1] = 0.0f;
			}
		}
	}
	*stamps_out = stamps;
	return count;
}

// For each patch of the map, the stamps over it (index + 1), up to four:
// the widest ones when more overlap
static void make_map(uint16_t *map, const stamp_t *stamps, int32_t stamp_count)
{
	float patch_meters = CLOUD_WEATHER_METERS / (float)CLOUD_WEATHER_MAP_SIZE;
	// A patch is covered when any of it is: its center within the stamp's
	// reach plus half its diagonal
	float patch_reach = patch_meters * 0.71f;

#pragma omp parallel for schedule(dynamic, 8)
	for (int32_t row = 0; row < CLOUD_WEATHER_MAP_SIZE; row++)
	{
		uint16_t *line = &map[(size_t)row * CLOUD_WEATHER_MAP_SIZE * 4];
		float z = ((float)row + 0.5f) * patch_meters;

		SDL_memset(line, 0, sizeof(uint16_t) * CLOUD_WEATHER_MAP_SIZE * 4);
		for (int32_t s = 0; s < stamp_count; s++)
		{
			const stamp_t *stamp = &stamps[s];
			float reach = stamp->width * 0.71f + patch_reach;
			float dz = z - stamp->z;
			int32_t first;
			int32_t last;

			dz -= SDL_roundf(dz / CLOUD_WEATHER_METERS) * CLOUD_WEATHER_METERS;
			if (SDL_fabsf(dz) > reach)
			{
				continue;
			}
			first = (int32_t)SDL_floorf((stamp->x - reach) / patch_meters);
			last = (int32_t)SDL_ceilf((stamp->x + reach) / patch_meters);
			for (int32_t c = first; c <= last; c++)
			{
				int32_t column = ((c % CLOUD_WEATHER_MAP_SIZE) + CLOUD_WEATHER_MAP_SIZE) % CLOUD_WEATHER_MAP_SIZE;
				float dx = ((float)c + 0.5f) * patch_meters - stamp->x;
				uint16_t *patch = &line[column * 4];
				int32_t slot = -1;

				if (dx * dx + dz * dz > reach * reach)
				{
					continue;
				}
				for (int32_t k = 0; k < 4 && slot < 0; k++)
				{
					if (patch[k] == 0)
					{
						slot = k;
					}
				}
				if (slot < 0)
				{
					// Full: instead of the narrowest, if this one is wider
					float narrowest = stamp->width;

					for (int32_t k = 0; k < 4; k++)
					{
						if (stamps[patch[k] - 1].width < narrowest)
						{
							narrowest = stamps[patch[k] - 1].width;
							slot = k;
						}
					}
				}
				if (slot >= 0)
				{
					patch[slot] = (uint16_t)(s + 1);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------

bool cloud_weather_update(cloud_weather_t *weather, const render_settings_t *settings, const cloud_paint_t *paint, bool *rebuilt_out)
{
	float wanted[8] = { settings->cloud_coverage, settings->cloud_tiling_meters, settings->cloud_size, settings->cloud_towers,
		settings->wind_direction_degrees, settings->cloud_thickness_meters, settings->cloud_density_variation,
		settings->cloud_lifetime_minutes };
	uint64_t now = SDL_GetTicks();
	uint64_t start;
	stamp_t *stamps = NULL;
	uint16_t *map;
	bool ok;

	*rebuilt_out = false;
	uint32_t revision = paint != NULL ? paint->stamp_revision : 0u;

	if (weather->made && SDL_memcmp(wanted, weather->made_for, sizeof(wanted)) == 0 && weather->made_revision == revision)
	{
		return true;
	}
	if (weather->made && now - weather->made_ticks < REBUILD_INTERVAL_MILLISECONDS)
	{
		return true;
	}
	if (!weather->atlas_made && !make_atlas(weather))
	{
		return false;
	}
	start = SDL_GetPerformanceCounter();
	if (weather->map_texture == NULL)
	{
		weather->map_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM, CLOUD_WEATHER_MAP_SIZE,
			CLOUD_WEATHER_MAP_SIZE, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (weather->map_texture == NULL)
		{
			return false;
		}
	}
	weather->stamp_count = place_stamps(weather, settings, paint, &stamps);
	map = SDL_malloc(sizeof(uint16_t) * 4 * CLOUD_WEATHER_MAP_SIZE * CLOUD_WEATHER_MAP_SIZE);
	ok = stamps != NULL && map != NULL;
	if (ok && (weather->stamp_buffer == NULL || weather->stamp_capacity < (uint32_t)SDL_max(weather->stamp_count, 1)))
	{
		if (weather->stamp_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, weather->stamp_buffer);
		}
		weather->stamp_capacity = (uint32_t)SDL_max(weather->stamp_count, 64) * 2u;
		weather->stamp_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
			(uint32_t)(CLOUD_DENSITY_TABLE_VEC4S * 4 * sizeof(float)) + weather->stamp_capacity * (uint32_t)sizeof(stamp_t));
		ok = weather->stamp_buffer != NULL;
	}
	if (ok)
	{
		// The density shapes' table first, then the stamps
		float table[CLOUD_DENSITY_TABLE_VEC4S * 4];

		SDL_zeroa(table);
		for (int32_t d = 0; d < weather->density_shape_count; d++)
		{
			const cloud_density_shape_t *density = &weather->density_shapes[d];
			float *entry = &table[d * 8];

			entry[0] = (float)density->origin[0];
			entry[1] = (float)density->origin[2];
			entry[2] = (float)density->size[0];
			entry[3] = (float)density->size[2];
			entry[4] = (float)density->size[1];
		}
		gpu_upload_buffer(weather->stamp_buffer, 0, table, sizeof(table));
		make_map(map, stamps, weather->stamp_count);
		if (weather->stamp_count > 0)
		{
			gpu_upload_buffer(weather->stamp_buffer, (uint32_t)sizeof(table), stamps, sizeof(stamp_t) * (size_t)weather->stamp_count);
		}
		gpu_upload_texture(weather->map_texture, 0, 0, 0, 0, CLOUD_WEATHER_MAP_SIZE, CLOUD_WEATHER_MAP_SIZE, map,
			sizeof(uint16_t) * 4 * CLOUD_WEATHER_MAP_SIZE * CLOUD_WEATHER_MAP_SIZE);
		SDL_memcpy(weather->made_for, wanted, sizeof(wanted));
		weather->made_revision = revision;
		weather->made = true;
		weather->made_ticks = now;
		*rebuilt_out = true;
		SDL_Log("Clouds: %d stamped in %.0f ms", weather->stamp_count,
			(double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency());
	}
	SDL_free(stamps);
	SDL_free(map);
	return weather->made;
}

void cloud_weather_destroy(cloud_weather_t *weather)
{
	if (gpu.device != NULL)
	{
		if (weather->atlas_texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, weather->atlas_texture);
		}
		if (weather->map_texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, weather->map_texture);
		}
		if (weather->density_atlas_texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, weather->density_atlas_texture);
		}
		if (weather->stamp_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, weather->stamp_buffer);
		}
	}
	SDL_zerop(weather);
}
