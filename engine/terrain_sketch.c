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
	// (the relief's middle: as the seed makes it, should a weight come)
	SDL_memset(terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF), 128, PLANE_TEXELS);
	sketch->lake_count = 0;
	sketch->revision++;
}

bool terrain_sketch_painted(const terrain_sketch_t *sketch)
{
	const uint8_t *height_weight;
	const uint8_t *relief_weight;
	const uint8_t *water;

	if (sketch == NULL || sketch->planes == NULL)
	{
		return false;
	}
	height_weight = terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT_WEIGHT);
	relief_weight = terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF_WEIGHT);
	water = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);
	for (size_t i = 0; i < PLANE_TEXELS; i++)
	{
		if (height_weight[i] != 0 || relief_weight[i] != 0 || water[i] != 0)
		{
			return true;
		}
	}
	return false;
}

static uint8_t towards(uint8_t value, float target, float amount)
{
	float moved = (float)value + ((target * 255.0f) - (float)value) * amount;

	// (a little past, so that a slow brush gets all the way there)
	if (target * 255.0f > (float)value)
	{
		moved = SDL_ceilf(moved);
	}
	else
	{
		moved = SDL_floorf(moved);
	}
	return (uint8_t)clamp_float(moved, 0.0f, 255.0f);
}

void terrain_sketch_paint(terrain_sketch_t *sketch, const terrain_sketch_brush_t *brush, float u, float v, float radius, float amount)
{
	float center_x = u * (float)TERRAIN_SKETCH_SIZE - 0.5f;
	float center_y = v * (float)TERRAIN_SKETCH_SIZE - 0.5f;
	float reach = SDL_max(radius * (float)TERRAIN_SKETCH_SIZE, 0.5f);
	int32_t first_x = SDL_max((int32_t)SDL_floorf(center_x - reach), 0);
	int32_t first_y = SDL_max((int32_t)SDL_floorf(center_y - reach), 0);
	int32_t last_x = SDL_min((int32_t)SDL_ceilf(center_x + reach), TERRAIN_SKETCH_SIZE - 1);
	int32_t last_y = SDL_min((int32_t)SDL_ceilf(center_y + reach), TERRAIN_SKETCH_SIZE - 1);
	uint8_t *height = terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT);
	uint8_t *height_weight = terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT_WEIGHT);
	uint8_t *relief = terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF);
	uint8_t *relief_weight = terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF_WEIGHT);
	uint8_t *water = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);

	if (sketch->planes == NULL || amount <= 0.0f)
	{
		return;
	}
	for (int32_t y = first_y; y <= last_y; y++)
	{
		for (int32_t x = first_x; x <= last_x; x++)
		{
			float dx = ((float)x - center_x) / reach;
			float dy = ((float)y - center_y) / reach;
			float distance_squared = dx * dx + dy * dy;
			float falloff;
			float here;
			size_t index = (size_t)y * TERRAIN_SKETCH_SIZE + (size_t)x;

			if (distance_squared >= 1.0f)
			{
				continue;
			}
			// (full in the middle, softly to nothing at the edge)
			falloff = (1.0f - distance_squared) * (1.0f - distance_squared);
			here = clamp_float(amount * falloff, 0.0f, 1.0f);
			if (brush->erase)
			{
				height_weight[index] = towards(height_weight[index], 0.0f, here);
				relief_weight[index] = towards(relief_weight[index], 0.0f, here);
				water[index] = towards(water[index], 0.0f, here);
				continue;
			}
			if (brush->height >= 0.0f)
			{
				height[index] = towards(height[index], brush->height, here);
				height_weight[index] = towards(height_weight[index], 1.0f, here);
			}
			if (brush->relief >= 0.0f)
			{
				relief[index] = towards(relief[index], brush->relief, here);
				relief_weight[index] = towards(relief_weight[index], 1.0f, here);
			}
			if (brush->water >= 0.0f)
			{
				water[index] = towards(water[index], brush->water, here);
			}
		}
	}
	sketch->revision++;
}

float terrain_sketch_sample(const terrain_sketch_t *sketch, terrain_sketch_plane_t plane, float u, float v)
{
	const uint8_t *values = terrain_sketch_plane(sketch, plane);
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
	if (sketch->planes == NULL || !SDL_CreateDirectory(folder))
	{
		return false;
	}
	return save_png(folder, "height.png", terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT),
			terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT_WEIGHT))
		&& save_png(folder, "relief.png", terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF),
			terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF_WEIGHT))
		&& save_png(folder, "water.png", terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER), NULL);
}

bool terrain_sketch_load(terrain_sketch_t *sketch, const char *folder)
{
	bool any = false;

	terrain_sketch_clear(sketch);
	if (sketch->planes == NULL)
	{
		return false;
	}
	any |= load_png(folder, "height.png", terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT),
		terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT_WEIGHT));
	any |= load_png(folder, "relief.png", terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF),
		terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF_WEIGHT));
	any |= load_png(folder, "water.png", terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER), NULL);
	sketch->revision++;
	return any;
}
