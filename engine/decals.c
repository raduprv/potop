// Decals (see decals.h): the kinds, the atlas, the ring of decals laid.

#include <SDL3/SDL.h>

#include "decals.h"
#include "file_io.h"
#include "ini_parser.h"
#include "stb_image.h"

#define CELL_TEXELS (DECALS_ATLAS_SIZE / DECALS_ATLAS_CELLS)

// ---------------------------------------------------------------------------
// The kinds (data/decals.ini)

static decal_kind_t *kind_named(decals_t *decals, const char *name)
{
	for (int i = 0; i < decals->kind_count; i++)
	{
		if (SDL_strcmp(decals->kinds[i].name, name) == 0)
		{
			return &decals->kinds[i];
		}
	}
	if (decals->kind_count >= DECALS_KINDS_MAX)
	{
		return NULL;
	}
	{
		decal_kind_t *kind = &decals->kinds[decals->kind_count++];

		SDL_zerop(kind);
		SDL_strlcpy(kind->name, name, sizeof(kind->name));
		kind->cell_count = 1;
		kind->size_meters = 0.1f;
		kind->depth_meters = 0.05f;
		kind->opacity = 1.0f;
		kind->relief = 1.0f;
		kind->roughness_weight = 0.5f;
		kind->fade_seconds = 2.0f;
		return kind;
	}
}

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	decals_t *decals = user_data;
	decal_kind_t *kind;

	if (section == NULL || section[0] == '\0' || (kind = kind_named(decals, section)) == NULL)
	{
		return false;
	}
	if (SDL_strcmp(key, "cell") == 0)
	{
		return ini_value_to_int(value, &kind->first_cell);
	}
	if (SDL_strcmp(key, "cells") == 0)
	{
		return ini_value_to_int(value, &kind->cell_count);
	}
	if (SDL_strcmp(key, "size_meters") == 0)
	{
		return ini_value_to_float(value, &kind->size_meters);
	}
	if (SDL_strcmp(key, "depth_meters") == 0)
	{
		return ini_value_to_float(value, &kind->depth_meters);
	}
	if (SDL_strcmp(key, "opacity") == 0)
	{
		return ini_value_to_float(value, &kind->opacity);
	}
	if (SDL_strcmp(key, "relief") == 0)
	{
		return ini_value_to_float(value, &kind->relief);
	}
	if (SDL_strcmp(key, "roughness_weight") == 0)
	{
		return ini_value_to_float(value, &kind->roughness_weight);
	}
	if (SDL_strcmp(key, "life_seconds") == 0)
	{
		return ini_value_to_float(value, &kind->life_seconds);
	}
	if (SDL_strcmp(key, "fade_seconds") == 0)
	{
		return ini_value_to_float(value, &kind->fade_seconds);
	}
	return false;
}

// ---------------------------------------------------------------------------
// The stand-in atlas: each cell a height and a color drawn from noise, the
// relief made from the height

static uint32_t hash_u32(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

static float hash_unit(uint32_t value)
{
	return (float)(hash_u32(value) & 0xFFFFFFu) / 16777215.0f;
}

// Smooth value noise on a grid of period cells (wraps), 0..1
static float value_noise(float x, float y, uint32_t seed)
{
	int ix = (int)SDL_floorf(x);
	int iy = (int)SDL_floorf(y);
	float fx = x - (float)ix;
	float fy = y - (float)iy;
	float sx = fx * fx * (3.0f - 2.0f * fx);
	float sy = fy * fy * (3.0f - 2.0f * fy);
	float a = hash_unit((uint32_t)(ix * 73856093) ^ (uint32_t)(iy * 19349663) ^ seed);
	float b = hash_unit((uint32_t)((ix + 1) * 73856093) ^ (uint32_t)(iy * 19349663) ^ seed);
	float c = hash_unit((uint32_t)(ix * 73856093) ^ (uint32_t)((iy + 1) * 19349663) ^ seed);
	float d = hash_unit((uint32_t)((ix + 1) * 73856093) ^ (uint32_t)((iy + 1) * 19349663) ^ seed);

	return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

static float fractal_noise(float x, float y, uint32_t seed)
{
	float total = 0.0f;
	float amplitude = 0.5f;

	for (int octave = 0; octave < 4; octave++)
	{
		total += value_noise(x, y, seed + (uint32_t)octave * 1013u) * amplitude;
		x *= 2.03f;
		y *= 2.03f;
		amplitude *= 0.5f;
	}
	return total;
}

static float smooth_step(float edge_0, float edge_1, float value)
{
	float t = SDL_clamp((value - edge_0) / (edge_1 - edge_0), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

typedef enum
{
	STAND_IN_HOLE_WOOD = 0,
	STAND_IN_HOLE_STONE,
	STAND_IN_HOLE_STONE_CHIPPED,
	STAND_IN_HOLE_DIRT,
	STAND_IN_SCORCH,
	STAND_IN_MUD,
	STAND_IN_DAMP,
	STAND_IN_SOOT,
	STAND_IN_COUNT
} stand_in_t;

// One texel of a stand-in cell at (u, v) in -1..1: its color and coverage,
// its height (0..1) and roughness
static void stand_in_texel(stand_in_t kind, float u, float v, uint32_t seed, float color[4], float *height, float *roughness)
{
	float radius = SDL_sqrtf(u * u + v * v);
	float angle = SDL_atan2f(v, u);
	float ragged = fractal_noise(SDL_cosf(angle) * 2.0f + 5.0f, SDL_sinf(angle) * 2.0f + 5.0f, seed);
	float grain = fractal_noise(u * 6.0f + 11.0f, v * 6.0f + 3.0f, seed ^ 0x5bd1e995u);

	color[0] = color[1] = color[2] = 0.0f;
	color[3] = 0.0f;
	*height = 0.5f;
	*roughness = 0.8f;
	switch (kind)
	{
	case STAND_IN_HOLE_WOOD:
	case STAND_IN_HOLE_STONE:
	case STAND_IN_HOLE_STONE_CHIPPED:
	case STAND_IN_HOLE_DIRT:
	{
		// A dark hole in the middle, a crater round it, then the surface
		// torn up (splinters out along the grain in wood, chips in stone)
		bool wood = kind == STAND_IN_HOLE_WOOD;
		bool dirt = kind == STAND_IN_HOLE_DIRT;
		float hole = kind == STAND_IN_HOLE_STONE_CHIPPED ? 0.14f : 0.11f;
		float torn_radius = wood ? 0.45f + 0.35f * fractal_noise(u * 1.5f + 2.0f, v * 9.0f, seed) : 0.5f + 0.3f * ragged;
		float crater = 1.0f - smooth_step(hole, torn_radius, radius);
		float core = 1.0f - smooth_step(hole * 0.7f, hole * 1.15f, radius);
		float chips = dirt ? 0.0f : smooth_step(0.6f, 0.75f, grain) * (1.0f - smooth_step(torn_radius, torn_radius + 0.25f, radius));

		if (dirt)
		{
			torn_radius = 0.6f + 0.3f * ragged;
			crater = 1.0f - smooth_step(0.1f, torn_radius, radius);
		}
		*height = 0.5f - 0.45f * core - 0.25f * crater * crater + 0.1f * chips;
		color[3] = SDL_clamp(SDL_max(crater * 1.4f, chips), 0.0f, 1.0f);
		if (wood)
		{
			// (bare wood, lighter, round a charred hole)
			float bare = crater * (1.0f - core);

			color[0] = 0.55f * bare + 0.03f * core;
			color[1] = 0.40f * bare + 0.025f * core;
			color[2] = 0.24f * bare + 0.02f * core;
		}
		else if (dirt)
		{
			color[0] = 0.16f + 0.08f * grain;
			color[1] = 0.12f + 0.06f * grain;
			color[2] = 0.09f + 0.04f * grain;
		}
		else
		{
			// (stone: the fresh broken face paler than the weathered one)
			float fresh = SDL_max(crater * (1.0f - core), chips);

			color[0] = 0.62f * fresh + 0.04f * core;
			color[1] = 0.60f * fresh + 0.04f * core;
			color[2] = 0.56f * fresh + 0.04f * core;
		}
		*roughness = 0.9f;
		break;
	}
	case STAND_IN_SCORCH:
	{
		float reach = 0.55f + 0.35f * ragged;
		float burnt = 1.0f - smooth_step(reach * 0.3f, reach, radius + (grain - 0.5f) * 0.3f);

		color[0] = 0.03f + 0.05f * (1.0f - burnt);
		color[1] = 0.025f + 0.03f * (1.0f - burnt);
		color[2] = 0.02f + 0.02f * (1.0f - burnt);
		color[3] = SDL_clamp(burnt * (0.75f + 0.4f * grain), 0.0f, 1.0f);
		*height = 0.5f - 0.05f * burnt * grain;
		*roughness = 0.95f;
		break;
	}
	case STAND_IN_MUD:
	{
		// A blob, and drops flung round it
		float reach = 0.4f + 0.25f * ragged;
		float blob = 1.0f - smooth_step(reach - 0.08f, reach, radius);
		float drops = 0.0f;

		for (int d = 0; d < 9; d++)
		{
			float drop_angle = hash_unit(seed + (uint32_t)d * 7u) * 6.2831853f;
			float drop_distance = 0.5f + 0.4f * hash_unit(seed + (uint32_t)d * 13u);
			float drop_size = 0.03f + 0.05f * hash_unit(seed + (uint32_t)d * 29u);
			float du = u - SDL_cosf(drop_angle) * drop_distance;
			float dv = v - SDL_sinf(drop_angle) * drop_distance;

			drops = SDL_max(drops, 1.0f - smooth_step(drop_size * 0.7f, drop_size, SDL_sqrtf(du * du + dv * dv)));
		}
		color[3] = SDL_max(blob, drops);
		color[0] = 0.20f + 0.05f * grain;
		color[1] = 0.15f + 0.04f * grain;
		color[2] = 0.10f + 0.03f * grain;
		*height = 0.5f + 0.15f * color[3] * (0.6f + 0.4f * grain);
		*roughness = 0.45f;
		break;
	}
	case STAND_IN_DAMP:
	{
		float reach = 0.6f + 0.3f * ragged;
		float wet = 1.0f - smooth_step(reach * 0.5f, reach, radius);

		color[0] = 0.12f;
		color[1] = 0.11f;
		color[2] = 0.10f;
		color[3] = wet * 0.55f;
		*roughness = 0.25f;
		break;
	}
	case STAND_IN_SOOT:
	{
		// Up from the bottom, thinning and spreading as it goes
		float up = v * 0.5f + 0.5f;
		float width = 0.25f + 0.55f * up;
		float across = SDL_fabsf(u + (fractal_noise(v * 2.0f, 1.0f, seed) - 0.5f) * 0.4f) / width;
		float soot = (1.0f - smooth_step(0.4f, 1.0f, across)) * (1.0f - smooth_step(0.6f, 1.0f, up)) * smooth_step(-1.0f, -0.8f, v);

		color[0] = 0.025f;
		color[1] = 0.022f;
		color[2] = 0.02f;
		color[3] = SDL_clamp(soot * (0.6f + 0.6f * grain), 0.0f, 1.0f);
		*roughness = 0.95f;
		break;
	}
	default:
		break;
	}
}

static uint32_t pack_rgba(float r, float g, float b, float a)
{
	uint32_t x = (uint32_t)(SDL_clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
	uint32_t y = (uint32_t)(SDL_clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
	uint32_t z = (uint32_t)(SDL_clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
	uint32_t w = (uint32_t)(SDL_clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);

	return x | (y << 8) | (z << 16) | (w << 24);
}

static void draw_stand_ins(decals_t *decals)
{
	static float heights[CELL_TEXELS * CELL_TEXELS];
	static float roughnesses[CELL_TEXELS * CELL_TEXELS];
	uint32_t *color_texels = decals->atlas;
	uint32_t *normal_texels = decals->atlas + DECALS_ATLAS_SIZE * DECALS_ATLAS_SIZE;

	for (int cell = 0; cell < DECALS_ATLAS_CELLS * DECALS_ATLAS_CELLS; cell++)
	{
		int cell_x = (cell % DECALS_ATLAS_CELLS) * CELL_TEXELS;
		int cell_y = (cell / DECALS_ATLAS_CELLS) * CELL_TEXELS;
		stand_in_t kind = cell < STAND_IN_COUNT ? (stand_in_t)cell : STAND_IN_COUNT;

		for (int y = 0; y < CELL_TEXELS; y++)
		{
			for (int x = 0; x < CELL_TEXELS; x++)
			{
				float u = ((float)x + 0.5f) / (float)CELL_TEXELS * 2.0f - 1.0f;
				float v = ((float)y + 0.5f) / (float)CELL_TEXELS * 2.0f - 1.0f;
				float color[4];
				int at = (cell_y + y) * DECALS_ATLAS_SIZE + cell_x + x;

				stand_in_texel(kind, u, v, 0x9e3779b9u * (uint32_t)(cell + 1), color, &heights[y * CELL_TEXELS + x],
					&roughnesses[y * CELL_TEXELS + x]);
				// (nothing at the cell's very edge: filtering there reads the next cell)
				if (x < 2 || y < 2 || x >= CELL_TEXELS - 2 || y >= CELL_TEXELS - 2)
				{
					color[3] = 0.0f;
				}
				color_texels[at] = pack_rgba(color[0], color[1], color[2], color[3]);
			}
		}
		for (int y = 0; y < CELL_TEXELS; y++)
		{
			for (int x = 0; x < CELL_TEXELS; x++)
			{
				int left = y * CELL_TEXELS + SDL_max(x - 1, 0);
				int right = y * CELL_TEXELS + SDL_min(x + 1, CELL_TEXELS - 1);
				int down = SDL_max(y - 1, 0) * CELL_TEXELS + x;
				int up = SDL_min(y + 1, CELL_TEXELS - 1) * CELL_TEXELS + x;
				// (a texel is about a millimeter: steep enough to read)
				float slope_x = (heights[right] - heights[left]) * 6.0f;
				float slope_y = (heights[up] - heights[down]) * 6.0f;
				float length = SDL_sqrtf(slope_x * slope_x + slope_y * slope_y + 1.0f);
				int at = (cell_y + y) * DECALS_ATLAS_SIZE + cell_x + x;

				normal_texels[at] = pack_rgba(-slope_x / length * 0.5f + 0.5f, -slope_y / length * 0.5f + 0.5f, roughnesses[y * CELL_TEXELS + x], 1.0f);
			}
		}
	}
}

// One of the atlas' pictures from a file, if it is there and the right size
static bool load_atlas_picture(const char *path, uint32_t *texels)
{
	size_t file_size = 0;
	void *file_data;
	uint8_t *pixels;
	int width;
	int height;
	int channels;

	if (!file_io_data_file_exists(path))
	{
		return false;
	}
	file_data = file_io_load_data_file(path, &file_size);
	if (file_data == NULL)
	{
		return false;
	}
	pixels = stbi_load_from_memory(file_data, (int)file_size, &width, &height, &channels, 4);
	SDL_free(file_data);
	if (pixels == NULL)
	{
		return false;
	}
	if (width != DECALS_ATLAS_SIZE || height != DECALS_ATLAS_SIZE)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Decals: %s is %dx%d, not %dx%d (left out)", path, width, height, DECALS_ATLAS_SIZE, DECALS_ATLAS_SIZE);
		stbi_image_free(pixels);
		return false;
	}
	SDL_memcpy(texels, pixels, sizeof(uint32_t) * DECALS_ATLAS_SIZE * DECALS_ATLAS_SIZE);
	stbi_image_free(pixels);
	return true;
}

// ---------------------------------------------------------------------------

bool decals_create(decals_t *decals)
{
	bool color_read;
	bool normal_read;

	SDL_zerop(decals);
	decals->seed = 0x2545f491u;
	decals->atlas = SDL_calloc((size_t)DECALS_ATLAS_SIZE * DECALS_ATLAS_SIZE * 2, sizeof(uint32_t));
	if (decals->atlas == NULL)
	{
		return false;
	}
	draw_stand_ins(decals);
	color_read = load_atlas_picture("props/textures/decals_color.png", decals->atlas);
	normal_read = load_atlas_picture("props/textures/decals_normal.png", decals->atlas + DECALS_ATLAS_SIZE * DECALS_ATLAS_SIZE);
	if (color_read != normal_read)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Decals: only one of decals_color.png and decals_normal.png read: the other is the stand-in's");
	}
	if (!ini_parse_data_file("decals.ini", load_key, decals))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Decals: no data/decals.ini (no kinds of decals)");
	}
	for (int i = 0; i < decals->kind_count; i++)
	{
		decal_kind_t *kind = &decals->kinds[i];
		int cells = DECALS_ATLAS_CELLS * DECALS_ATLAS_CELLS;

		kind->first_cell = SDL_clamp(kind->first_cell, 0, cells - 1);
		kind->cell_count = SDL_clamp(kind->cell_count, 1, cells - kind->first_cell);
	}
	SDL_Log("Decals: %d kinds, the atlas %s", decals->kind_count, color_read ? "from decals_color.png" : "drawn (stand-ins)");
	return true;
}

void decals_destroy(decals_t *decals)
{
	SDL_free(decals->atlas);
	SDL_zerop(decals);
}

int decals_find_kind(const decals_t *decals, const char *name)
{
	for (int i = 0; name != NULL && i < decals->kind_count; i++)
	{
		if (SDL_strcmp(decals->kinds[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

bool decals_add(decals_t *decals, int kind_index, const float point[3], const float normal[3], float size_meters, double now)
{
	const decal_kind_t *kind;
	decal_t *decal;
	float helper[3];
	float along[3];
	float across[3];
	float length;
	float turn;
	float size;

	if (kind_index < 0 || kind_index >= decals->kind_count)
	{
		return false;
	}
	kind = &decals->kinds[kind_index];
	decal = &decals->list[decals->next];
	decals->next = (decals->next + 1) % DECALS_MAX;
	if (!decal->used)
	{
		decals->count++;
	}
	decals->seed = decals->seed * 1664525u + 1013904223u;
	// A frame on the surface: any direction square to the normal, turned
	// at random about it
	if (SDL_fabsf(normal[1]) < 0.9f)
	{
		helper[0] = 0.0f;
		helper[1] = 1.0f;
		helper[2] = 0.0f;
	}
	else
	{
		helper[0] = 1.0f;
		helper[1] = 0.0f;
		helper[2] = 0.0f;
	}
	along[0] = helper[1] * normal[2] - helper[2] * normal[1];
	along[1] = helper[2] * normal[0] - helper[0] * normal[2];
	along[2] = helper[0] * normal[1] - helper[1] * normal[0];
	length = SDL_sqrtf(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
	for (int k = 0; k < 3; k++)
	{
		along[k] /= SDL_max(length, 1e-6f);
	}
	across[0] = normal[1] * along[2] - normal[2] * along[1];
	across[1] = normal[2] * along[0] - normal[0] * along[2];
	across[2] = normal[0] * along[1] - normal[1] * along[0];
	turn = hash_unit(decals->seed) * 6.2831853f;
	size = size_meters > 0.0f ? size_meters : kind->size_meters;
	// (a little bigger or smaller each time)
	size *= 0.85f + 0.3f * hash_unit(decals->seed ^ 0x68e31da4u);
	SDL_zerop(decal);
	decal->used = true;
	decal->kind = kind_index;
	decal->cell = kind->first_cell + (int)(hash_u32(decals->seed ^ 0xb5297a4du) % (uint32_t)kind->cell_count);
	for (int k = 0; k < 3; k++)
	{
		decal->center[k] = point[k];
		decal->axes[0][k] = along[k] * SDL_cosf(turn) + across[k] * SDL_sinf(turn);
		decal->axes[1][k] = across[k] * SDL_cosf(turn) - along[k] * SDL_sinf(turn);
		decal->axes[2][k] = normal[k];
	}
	decal->half_sizes[0] = size * 0.5f;
	decal->half_sizes[1] = size * 0.5f;
	decal->half_sizes[2] = SDL_max(kind->depth_meters, 0.01f);
	decal->born_seconds = now;
	return true;
}

void decals_clear(decals_t *decals)
{
	for (int i = 0; i < DECALS_MAX; i++)
	{
		decals->list[i].used = false;
	}
	decals->count = 0;
	decals->next = 0;
}

float decals_age_fade(const decals_t *decals, const decal_t *decal, double now)
{
	const decal_kind_t *kind = &decals->kinds[decal->kind];
	float age;

	if (kind->life_seconds <= 0.0f)
	{
		return 1.0f;
	}
	age = (float)(now - decal->born_seconds);
	return SDL_clamp((kind->life_seconds - age) / SDL_max(kind->fade_seconds, 0.01f), 0.0f, 1.0f);
}

void decals_update(decals_t *decals, double now)
{
	for (int i = 0; i < DECALS_MAX; i++)
	{
		decal_t *decal = &decals->list[i];

		if (decal->used && decals_age_fade(decals, decal, now) <= 0.0f)
		{
			decal->used = false;
			decals->count--;
		}
	}
}

static uint32_t float_bits(float value)
{
	uint32_t bits;

	SDL_memcpy(&bits, &value, sizeof(bits));
	return bits;
}

void decals_pack(const decals_t *decals, const decal_t *decal, float fade, uint32_t record[DECALS_RECORD_WORDS])
{
	const decal_kind_t *kind = &decals->kinds[decal->kind];
	float cell_size = 1.0f / (float)DECALS_ATLAS_CELLS;

	// Three planes: the point's place in the box, -1..1 along each axis
	for (int a = 0; a < 3; a++)
	{
		float inverse = 1.0f / decal->half_sizes[a];
		float offset = -(decal->axes[a][0] * decal->center[0] + decal->axes[a][1] * decal->center[1] + decal->axes[a][2] * decal->center[2]);

		record[a * 4 + 0] = float_bits(decal->axes[a][0] * inverse);
		record[a * 4 + 1] = float_bits(decal->axes[a][1] * inverse);
		record[a * 4 + 2] = float_bits(decal->axes[a][2] * inverse);
		record[a * 4 + 3] = float_bits(offset * inverse);
	}
	// Its cell of the atlas
	record[12] = float_bits((float)(decal->cell % DECALS_ATLAS_CELLS) * cell_size);
	record[13] = float_bits((float)(decal->cell / DECALS_ATLAS_CELLS) * cell_size);
	record[14] = float_bits(cell_size);
	record[15] = float_bits(cell_size);
	// How much of it shows, its relief, its roughness' weight
	record[16] = float_bits(kind->opacity * SDL_clamp(fade, 0.0f, 1.0f));
	record[17] = float_bits(kind->relief);
	record[18] = float_bits(kind->roughness_weight);
	record[19] = 0;
}
