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
	sketch->lake_hint = SDL_calloc(PLANE_TEXELS, sizeof(float));
	if (sketch->planes == NULL || sketch->lake_labels == NULL || sketch->lake_hint == NULL)
	{
		terrain_sketch_destroy(sketch);
		return false;
	}
	terrain_sketch_land_defaults(&sketch->land);
	terrain_sketch_clear(sketch);
	return true;
}

void terrain_sketch_land_defaults(terrain_sketch_land_t *land)
{
	static const float heights[TERRAIN_SKETCH_REGION_COUNT] = { 0.14f, 0.08f, 0.26f, 0.22f, 0.0f, 0.08f };
	static const float rolls[TERRAIN_SKETCH_REGION_COUNT] = { 0.5f, 0.15f, 1.6f, 1.0f, 0.3f, 0.1f };
	static const float crests[TERRAIN_SKETCH_REGION_COUNT] = { 0.0f, 0.0f, 0.10f, 0.95f, 0.0f, 0.0f };

	for (int i = 0; i < TERRAIN_SKETCH_REGION_COUNT; i++)
	{
		land->height[i] = heights[i];
		land->roll[i] = rolls[i];
		land->crests[i] = crests[i];
	}
	land->blend_share = 0.015f;
	land->crest_scale = 2.0f;
	land->lake_depth_meters = 8.0f;
	land->level_scale[TERRAIN_SKETCH_LOW] = 0.5f;
	land->level_scale[TERRAIN_SKETCH_MEDIUM] = 1.0f;
	land->level_scale[TERRAIN_SKETCH_HIGH] = 1.7f;
	land->shape_scale[TERRAIN_SKETCH_BIG] = 0.45f;
	land->shape_scale[TERRAIN_SKETCH_NORMAL] = 1.0f;
	land->shape_scale[TERRAIN_SKETCH_SMALL] = 2.2f;
}

void terrain_sketch_destroy(terrain_sketch_t *sketch)
{
	SDL_free(sketch->planes);
	SDL_free(sketch->lake_labels);
	SDL_free(sketch->lake_hint);
	SDL_zerop(sketch);
}

void terrain_sketch_clear(terrain_sketch_t *sketch)
{
	if (sketch->planes == NULL)
	{
		return;
	}
	SDL_memset(sketch->planes, 0, TERRAIN_SKETCH_PLANES * PLANE_TEXELS);
	SDL_memset(terrain_sketch_plane(sketch, TERRAIN_SKETCH_LEVEL), TERRAIN_SKETCH_MEDIUM, PLANE_TEXELS);
	SDL_memset(terrain_sketch_plane(sketch, TERRAIN_SKETCH_SHAPE), TERRAIN_SKETCH_NORMAL, PLANE_TEXELS);
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

// The brush's values, each within its range (none: rubbed out, the rest
// as it starts)
static terrain_sketch_brush_t clean_brush(const terrain_sketch_brush_t *brush)
{
	terrain_sketch_brush_t clean = *brush;

	if (clean.level < TERRAIN_SKETCH_LOW || clean.level > TERRAIN_SKETCH_HIGH)
	{
		clean.level = TERRAIN_SKETCH_MEDIUM;
	}
	if (clean.shape < TERRAIN_SKETCH_BIG || clean.shape > TERRAIN_SKETCH_SMALL)
	{
		clean.shape = TERRAIN_SKETCH_NORMAL;
	}
	if (clean.region == TERRAIN_SKETCH_NONE)
	{
		clean.level = TERRAIN_SKETCH_MEDIUM;
		clean.shape = TERRAIN_SKETCH_NORMAL;
	}
	return clean;
}

static void paint_disc(terrain_sketch_t *sketch, const terrain_sketch_brush_t *brush, float u, float v, float radius)
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
	uint8_t *levels = terrain_sketch_plane(sketch, TERRAIN_SKETCH_LEVEL);
	uint8_t *shapes = terrain_sketch_plane(sketch, TERRAIN_SKETCH_SHAPE);

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
			regions[index] = (uint8_t)brush->region;
			water[index] = brush->region == TERRAIN_SKETCH_LAKE ? 255 : 0;
			levels[index] = (uint8_t)brush->level;
			shapes[index] = (uint8_t)brush->shape;
		}
	}
}

void terrain_sketch_paint(terrain_sketch_t *sketch, const terrain_sketch_brush_t *brush, float u, float v, float radius)
{
	terrain_sketch_brush_t clean;

	if (sketch->planes == NULL || brush->region < 0 || brush->region >= TERRAIN_SKETCH_REGION_COUNT)
	{
		return;
	}
	clean = clean_brush(brush);
	paint_disc(sketch, &clean, u, v, radius);
	sketch->revision++;
}

void terrain_sketch_paint_line(terrain_sketch_t *sketch, const terrain_sketch_brush_t *brush, float from_u, float from_v, float to_u, float to_v,
	float radius)
{
	float length = SDL_sqrtf((to_u - from_u) * (to_u - from_u) + (to_v - from_v) * (to_v - from_v));
	// (discs a third of the radius apart: an even stroke)
	int steps = (int)SDL_ceilf(length / SDL_max(radius * 0.33f, 0.5f / (float)TERRAIN_SKETCH_SIZE));
	terrain_sketch_brush_t clean;

	if (sketch->planes == NULL || brush->region < 0 || brush->region >= TERRAIN_SKETCH_REGION_COUNT)
	{
		return;
	}
	clean = clean_brush(brush);
	steps = SDL_clamp(steps, 1, 4096);
	for (int k = 1; k <= steps; k++)
	{
		float along = (float)k / (float)steps;

		paint_disc(sketch, &clean, lerp_float(from_u, to_u, along), lerp_float(from_v, to_v, along), radius);
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

float terrain_sketch_lake_hint(const terrain_sketch_t *sketch, float u, float v)
{
	int32_t x = clamp_int((int)(u * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);
	int32_t y = clamp_int((int)(v * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);

	if (sketch == NULL || sketch->lake_hint == NULL)
	{
		return 0.0f;
	}
	return sketch->lake_hint[(size_t)y * TERRAIN_SKETCH_SIZE + (size_t)x];
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
	const uint8_t *levels = terrain_sketch_plane(sketch, TERRAIN_SKETCH_LEVEL);
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
		int over = regions[i] == TERRAIN_SKETCH_NONE ? 0
			: (levels[i] == TERRAIN_SKETCH_LOW ? TERRAIN_SKETCH_PNG_LOW : (levels[i] == TERRAIN_SKETCH_HIGH ? TERRAIN_SKETCH_PNG_HIGH : 0));

		gray[i] = (uint8_t)(regions[i] * TERRAIN_SKETCH_PNG_STEP + over);
	}
	saved = save_png(folder, "regions.png", gray, NULL);
	// (the shapes: big black, normal gray, small white)
	if (saved)
	{
		const uint8_t *shapes = terrain_sketch_plane(sketch, TERRAIN_SKETCH_SHAPE);

		for (size_t i = 0; i < PLANE_TEXELS; i++)
		{
			gray[i] = (uint8_t)(shapes[i] == TERRAIN_SKETCH_BIG ? 0 : (shapes[i] == TERRAIN_SKETCH_SMALL ? 255 : 128));
		}
		saved = save_png(folder, "shapes.png", gray, NULL);
	}
	SDL_free(gray);
	if (saved)
	{
		static const char *const names[TERRAIN_SKETCH_REGION_COUNT] = { "unpainted", "plain", "hills", "mountains", "valley", "lake" };
		char text[2048];
		char path[1024];
		size_t length = 0;
		const terrain_sketch_land_t *land = &sketch->land;

		length += (size_t)SDL_snprintf(text + length, sizeof(text) - length,
			"; What the sketch's regions make (terrain_sketch.h)\nblend_share = %.4f\ncrest_scale = %.3f\nlake_depth_meters = %.2f\n"
			"low_scale = %.3f\nhigh_scale = %.3f\nbig_shapes = %.3f\nsmall_shapes = %.3f\n",
			(double)land->blend_share, (double)land->crest_scale, (double)land->lake_depth_meters,
			(double)land->level_scale[TERRAIN_SKETCH_LOW], (double)land->level_scale[TERRAIN_SKETCH_HIGH],
			(double)land->shape_scale[TERRAIN_SKETCH_BIG], (double)land->shape_scale[TERRAIN_SKETCH_SMALL]);
		for (int i = 0; i < TERRAIN_SKETCH_REGION_COUNT && length < sizeof(text); i++)
		{
			length += (size_t)SDL_snprintf(text + length, sizeof(text) - length, "%s_height = %.4f\n%s_roll = %.4f\n%s_crests = %.4f\n", names[i],
				(double)land->height[i], names[i], (double)land->roll[i], names[i], (double)land->crests[i]);
		}
		SDL_snprintf(path, sizeof(path), "%sland.ini", folder);
		saved = file_io_save_file(path, text, SDL_min(length, sizeof(text) - 1));
	}
	return saved;
}

// One line of land.ini: "name = value"
static void read_land_line(terrain_sketch_land_t *land, const char *line)
{
	static const char *const names[TERRAIN_SKETCH_REGION_COUNT] = { "unpainted", "plain", "hills", "mountains", "valley", "lake" };
	char key[64];
	float value;

	if (SDL_sscanf(line, " %63[a-z_] = %f", key, &value) != 2)
	{
		return;
	}
	if (SDL_strcmp(key, "blend_share") == 0)
	{
		land->blend_share = value;
	}
	else if (SDL_strcmp(key, "crest_scale") == 0)
	{
		land->crest_scale = value;
	}
	else if (SDL_strcmp(key, "lake_depth_meters") == 0)
	{
		land->lake_depth_meters = value;
	}
	else if (SDL_strcmp(key, "low_scale") == 0)
	{
		land->level_scale[TERRAIN_SKETCH_LOW] = value;
	}
	else if (SDL_strcmp(key, "high_scale") == 0)
	{
		land->level_scale[TERRAIN_SKETCH_HIGH] = value;
	}
	else if (SDL_strcmp(key, "big_shapes") == 0)
	{
		land->shape_scale[TERRAIN_SKETCH_BIG] = value;
	}
	else if (SDL_strcmp(key, "small_shapes") == 0)
	{
		land->shape_scale[TERRAIN_SKETCH_SMALL] = value;
	}
	for (int i = 0; i < TERRAIN_SKETCH_REGION_COUNT; i++)
	{
		size_t name_length = SDL_strlen(names[i]);

		if (SDL_strncmp(key, names[i], name_length) != 0 || key[name_length] != '_')
		{
			continue;
		}
		if (SDL_strcmp(key + name_length, "_height") == 0)
		{
			land->height[i] = value;
		}
		else if (SDL_strcmp(key + name_length, "_roll") == 0)
		{
			land->roll[i] = value;
		}
		else if (SDL_strcmp(key + name_length, "_crests") == 0)
		{
			land->crests[i] = value;
		}
	}
}

bool terrain_sketch_load(terrain_sketch_t *sketch, const char *folder)
{
	uint8_t *regions = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	uint8_t *water = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);
	uint8_t *levels = terrain_sketch_plane(sketch, TERRAIN_SKETCH_LEVEL);
	bool loaded;

	terrain_sketch_clear(sketch);
	if (sketch->planes == NULL)
	{
		return false;
	}
	loaded = load_png(folder, "regions.png", regions, NULL);
	// (the shapes, gray to the nearest; none: all normal, as cleared)
	{
		uint8_t *shapes = terrain_sketch_plane(sketch, TERRAIN_SKETCH_SHAPE);

		if (loaded && load_png(folder, "shapes.png", shapes, NULL))
		{
			for (size_t i = 0; i < PLANE_TEXELS; i++)
			{
				shapes[i] = (uint8_t)(shapes[i] < 64 ? TERRAIN_SKETCH_BIG : (shapes[i] > 192 ? TERRAIN_SKETCH_SMALL : TERRAIN_SKETCH_NORMAL));
			}
		}
		else
		{
			SDL_memset(shapes, TERRAIN_SKETCH_NORMAL, PLANE_TEXELS);
		}
	}
	terrain_sketch_land_defaults(&sketch->land);
	{
		char path[1024];
		size_t size = 0;
		char *text;

		SDL_snprintf(path, sizeof(path), "%sland.ini", folder);
		text = file_io_load_file(path, &size);
		if (text != NULL)
		{
			char *line = text;

			for (size_t i = 0; i <= size; i++)
			{
				if (i == size || text[i] == '\n')
				{
					if (i < size)
					{
						text[i] = '\0';
					}
					read_land_line(&sketch->land, line);
					line = &text[i + 1];
				}
			}
			SDL_free(text);
		}
	}
	// (the gray back to the regions and their levels: what is over a
	// region's step, nearer the low or the high one's, or none, medium; the
	// water follows)
	for (size_t i = 0; i < PLANE_TEXELS; i++)
	{
		int region = (regions[i] + 6) / TERRAIN_SKETCH_PNG_STEP;
		int over = regions[i] - region * TERRAIN_SKETCH_PNG_STEP;

		regions[i] = (uint8_t)(region < TERRAIN_SKETCH_REGION_COUNT ? region : TERRAIN_SKETCH_NONE);
		levels[i] = (uint8_t)(regions[i] == TERRAIN_SKETCH_NONE || over < TERRAIN_SKETCH_PNG_LOW / 2 ? TERRAIN_SKETCH_MEDIUM
			: (over < (TERRAIN_SKETCH_PNG_LOW + TERRAIN_SKETCH_PNG_HIGH) / 2 ? TERRAIN_SKETCH_LOW : TERRAIN_SKETCH_HIGH));
		water[i] = regions[i] == TERRAIN_SKETCH_LAKE ? 255 : 0;
	}
	sketch->revision++;
	return loaded;
}
