// A new map's sketch. See terrain_sketch.h.

#include "terrain_sketch.h"

#include <SDL3/SDL.h>

#include "file_io.h"
#include "math3d.h"
#include "stb_image.h"
#include "stb_image_write.h"

#define PLANE_TEXELS ((size_t)TERRAIN_SKETCH_SIZE * TERRAIN_SKETCH_SIZE)

bool terrain_sketch_create(terrain_sketch_t *sketch)
{
	SDL_zerop(sketch);
	sketch->planes = SDL_calloc(TERRAIN_SKETCH_PLANES, PLANE_TEXELS);
	sketch->lake_labels = SDL_calloc(PLANE_TEXELS, sizeof(uint16_t));
	if (sketch->planes == NULL || sketch->lake_labels == NULL)
	{
		terrain_sketch_destroy(sketch);
		return false;
	}
	terrain_sketch_clear(sketch);
	return true;
}

void terrain_sketch_destroy(terrain_sketch_t *sketch)
{
	SDL_free(sketch->planes);
	SDL_free(sketch->lake_labels);
	SDL_zerop(sketch);
}

void terrain_sketch_clear(terrain_sketch_t *sketch)
{
	if (sketch->planes == NULL)
	{
		return;
	}
	SDL_memset(sketch->planes, 0, TERRAIN_SKETCH_PLANES * PLANE_TEXELS);
	sketch->lake_count = 0;
	sketch->revision++;
}

bool terrain_sketch_painted(const terrain_sketch_t *sketch)
{
	const uint8_t *region;

	if (sketch == NULL || sketch->planes == NULL)
	{
		return false;
	}
	region = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	for (size_t i = 0; i < PLANE_TEXELS; i++)
	{
		if (region[i] != TERRAIN_SKETCH_NONE)
		{
			return true;
		}
	}
	return false;
}

static void paint_disc(terrain_sketch_t *sketch, terrain_sketch_region_t region, float u, float v, float radius)
{
	float center_x = u * (float)TERRAIN_SKETCH_SIZE - 0.5f;
	float center_y = v * (float)TERRAIN_SKETCH_SIZE - 0.5f;
	float reach = SDL_max(radius * (float)TERRAIN_SKETCH_SIZE, 0.5f);
	int32_t first_x = SDL_max((int32_t)SDL_floorf(center_x - reach), 0);
	int32_t first_y = SDL_max((int32_t)SDL_floorf(center_y - reach), 0);
	int32_t last_x = SDL_min((int32_t)SDL_ceilf(center_x + reach), TERRAIN_SKETCH_SIZE - 1);
	int32_t last_y = SDL_min((int32_t)SDL_ceilf(center_y + reach), TERRAIN_SKETCH_SIZE - 1);
	uint8_t *regions = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	uint8_t *water = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);

	for (int32_t y = first_y; y <= last_y; y++)
	{
		for (int32_t x = first_x; x <= last_x; x++)
		{
			float dx = (float)x - center_x;
			float dy = (float)y - center_y;
			size_t index = (size_t)y * TERRAIN_SKETCH_SIZE + (size_t)x;

			if (dx * dx + dy * dy > reach * reach)
			{
				continue;
			}
			regions[index] = (uint8_t)region;
			water[index] = region == TERRAIN_SKETCH_LAKE ? 255 : 0;
		}
	}
}

void terrain_sketch_paint(terrain_sketch_t *sketch, terrain_sketch_region_t region, float u, float v, float radius)
{
	if (sketch->planes == NULL || region < 0 || region >= TERRAIN_SKETCH_REGION_COUNT)
	{
		return;
	}
	paint_disc(sketch, region, u, v, radius);
	sketch->revision++;
}

void terrain_sketch_paint_line(terrain_sketch_t *sketch, terrain_sketch_region_t region, float from_u, float from_v, float to_u, float to_v,
	float radius)
{
	float length = SDL_sqrtf((to_u - from_u) * (to_u - from_u) + (to_v - from_v) * (to_v - from_v));
	// (discs a third of the radius apart: an even stroke)
	int steps = (int)SDL_ceilf(length / SDL_max(radius * 0.33f, 0.5f / (float)TERRAIN_SKETCH_SIZE));

	if (sketch->planes == NULL || region < 0 || region >= TERRAIN_SKETCH_REGION_COUNT)
	{
		return;
	}
	steps = SDL_clamp(steps, 1, 4096);
	for (int k = 1; k <= steps; k++)
	{
		float along = (float)k / (float)steps;

		paint_disc(sketch, region, lerp_float(from_u, to_u, along), lerp_float(from_v, to_v, along), radius);
	}
	sketch->revision++;
}

terrain_sketch_region_t terrain_sketch_region_at(const terrain_sketch_t *sketch, float u, float v)
{
	int32_t x = clamp_int((int)(u * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);
	int32_t y = clamp_int((int)(v * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);
	uint8_t region = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION)[(size_t)y * TERRAIN_SKETCH_SIZE + (size_t)x];

	return region < TERRAIN_SKETCH_REGION_COUNT ? (terrain_sketch_region_t)region : TERRAIN_SKETCH_NONE;
}

float terrain_sketch_water(const terrain_sketch_t *sketch, float u, float v)
{
	const uint8_t *values = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);
	float x = clamp_float(u * (float)TERRAIN_SKETCH_SIZE - 0.5f, 0.0f, (float)(TERRAIN_SKETCH_SIZE - 1));
	float y = clamp_float(v * (float)TERRAIN_SKETCH_SIZE - 0.5f, 0.0f, (float)(TERRAIN_SKETCH_SIZE - 1));
	int32_t x0 = (int32_t)x;
	int32_t y0 = (int32_t)y;
	int32_t x1 = SDL_min(x0 + 1, TERRAIN_SKETCH_SIZE - 1);
	int32_t y1 = SDL_min(y0 + 1, TERRAIN_SKETCH_SIZE - 1);
	float fx = x - (float)x0;
	float fy = y - (float)y0;
	float top = lerp_float((float)values[y0 * TERRAIN_SKETCH_SIZE + x0], (float)values[y0 * TERRAIN_SKETCH_SIZE + x1], fx);
	float bottom = lerp_float((float)values[y1 * TERRAIN_SKETCH_SIZE + x0], (float)values[y1 * TERRAIN_SKETCH_SIZE + x1], fx);

	return lerp_float(top, bottom, fy) * (1.0f / 255.0f);
}

int terrain_sketch_lake_at(const terrain_sketch_t *sketch, float u, float v)
{
	int32_t x = clamp_int((int)(u * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);
	int32_t y = clamp_int((int)(v * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);
	uint16_t label;

	if (sketch == NULL || sketch->lake_labels == NULL)
	{
		return -1;
	}
	label = sketch->lake_labels[(size_t)y * TERRAIN_SKETCH_SIZE + (size_t)x];
	return label > 0 && (int)label <= sketch->lake_count ? (int)label - 1 : -1;
}

// --- Files ---------------------------------------------------------------------

typedef struct
{
	uint8_t *data;
	size_t size;
	size_t capacity;
	bool failed;
} memory_writer_t;

static void append_to_memory(void *context, void *data, int size)
{
	memory_writer_t *writer = context;

	if (writer->failed)
	{
		return;
	}
	if (writer->size + (size_t)size > writer->capacity)
	{
		size_t new_capacity = SDL_max(writer->capacity * 2, writer->size + (size_t)size);
		uint8_t *grown = SDL_realloc(writer->data, new_capacity);

		if (grown == NULL)
		{
			writer->failed = true;
			return;
		}
		writer->data = grown;
		writer->capacity = new_capacity;
	}
	SDL_memcpy(writer->data + writer->size, data, (size_t)size);
	writer->size += (size_t)size;
}

// One PNG of one plane, or of a plane and its weight (gray, alpha)
static bool save_png(const char *folder, const char *name, const uint8_t *gray, const uint8_t *alpha)
{
	char path[1024];
	int components = alpha != NULL ? 2 : 1;
	uint8_t *pixels = SDL_malloc(PLANE_TEXELS * (size_t)components);
	memory_writer_t writer = { 0 };
	bool saved = false;

	if (pixels == NULL)
	{
		return false;
	}
	for (size_t i = 0; i < PLANE_TEXELS; i++)
	{
		pixels[i * (size_t)components] = gray[i];
		if (alpha != NULL)
		{
			pixels[i * 2 + 1] = alpha[i];
		}
	}
	SDL_snprintf(path, sizeof(path), "%s%s", folder, name);
	if (stbi_write_png_to_func(append_to_memory, &writer, TERRAIN_SKETCH_SIZE, TERRAIN_SKETCH_SIZE, components, pixels,
			TERRAIN_SKETCH_SIZE * components) && !writer.failed)
	{
		saved = file_io_save_file(path, writer.data, writer.size);
	}
	SDL_free(writer.data);
	SDL_free(pixels);
	return saved;
}

static bool load_png(const char *folder, const char *name, uint8_t *gray, uint8_t *alpha)
{
	char path[1024];
	size_t size = 0;
	void *file;
	int width = 0;
	int height = 0;
	int components = 0;
	int wanted = alpha != NULL ? 2 : 1;
	uint8_t *pixels;

	SDL_snprintf(path, sizeof(path), "%s%s", folder, name);
	file = file_io_load_file(path, &size);
	if (file == NULL)
	{
		return false;
	}
	pixels = stbi_load_from_memory(file, (int)size, &width, &height, &components, wanted);
	SDL_free(file);
	if (pixels == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Sketch: %s is not a picture it can read", path);
		return false;
	}
	// (any size: taken to the sketch's, nearest)
	for (int32_t y = 0; y < TERRAIN_SKETCH_SIZE; y++)
	{
		int32_t source_y = y * height / TERRAIN_SKETCH_SIZE;

		for (int32_t x = 0; x < TERRAIN_SKETCH_SIZE; x++)
		{
			int32_t source_x = x * width / TERRAIN_SKETCH_SIZE;
			const uint8_t *source = &pixels[((size_t)source_y * (size_t)width + (size_t)source_x) * (size_t)wanted];
			size_t index = (size_t)y * TERRAIN_SKETCH_SIZE + (size_t)x;

			gray[index] = source[0];
			if (alpha != NULL)
			{
				alpha[index] = source[1];
			}
		}
	}
	stbi_image_free(pixels);
	return true;
}

bool terrain_sketch_save(const terrain_sketch_t *sketch, const char *folder)
{
	const uint8_t *regions = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	uint8_t *gray;
	bool saved;

	if (sketch->planes == NULL || !SDL_CreateDirectory(folder))
	{
		return false;
	}
	gray = SDL_malloc(PLANE_TEXELS);
	if (gray == NULL)
	{
		return false;
	}
	for (size_t i = 0; i < PLANE_TEXELS; i++)
	{
		gray[i] = (uint8_t)(regions[i] * TERRAIN_SKETCH_PNG_STEP);
	}
	saved = save_png(folder, "regions.png", gray, NULL);
	SDL_free(gray);
	return saved;
}

bool terrain_sketch_load(terrain_sketch_t *sketch, const char *folder)
{
	uint8_t *regions = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	uint8_t *water = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);
	bool loaded;

	terrain_sketch_clear(sketch);
	if (sketch->planes == NULL)
	{
		return false;
	}
	loaded = load_png(folder, "regions.png", regions, NULL);
	// (the gray back to the regions, the nearest; the water follows)
	for (size_t i = 0; i < PLANE_TEXELS; i++)
	{
		int region = (regions[i] + TERRAIN_SKETCH_PNG_STEP / 2) / TERRAIN_SKETCH_PNG_STEP;

		regions[i] = (uint8_t)(region < TERRAIN_SKETCH_REGION_COUNT ? region : TERRAIN_SKETCH_NONE);
		water[i] = regions[i] == TERRAIN_SKETCH_LAKE ? 255 : 0;
	}
	sketch->revision++;
	return loaded;
}
