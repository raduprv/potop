// The world map's picture. See world_map.h.

#include "world_map.h"

#include "file_io.h"
#include "gpu.h"
#include "material_map.h"
#include "materials.h"
#include "object_model.h"
#include "stb_image.h"

// The made picture's side at most (pixels)
static const int MADE_SIZE_MOST = 1024;

// The ground's materials, muted (sRGB 0..1), the order of materials.h
static const float material_colors[MATERIAL_COUNT][3] = {
	{ 0.50f, 0.58f, 0.36f },               // grass
	{ 0.58f, 0.50f, 0.38f },               // dirt
	{ 0.56f, 0.55f, 0.53f },               // rock
	{ 0.80f, 0.74f, 0.56f },               // sand
	{ 0.62f, 0.48f, 0.38f },               // clay
	{ 0.94f, 0.94f, 0.95f },               // snow
	{ 0.52f, 0.48f, 0.40f },               // silt
	{ 0.48f, 0.44f, 0.34f },               // needles
	{ 0.56f, 0.48f, 0.32f },               // leaves
	{ 0.62f, 0.56f, 0.44f },               // trampled
	{ 0.62f, 0.56f, 0.52f },               // petals
	{ 0.50f, 0.42f, 0.32f },               // trampled dirt
};
static const float paper[3] = { 0.93f, 0.88f, 0.76f };
static const float shallow_water[3] = { 0.52f, 0.68f, 0.78f };
static const float deep_water[3] = { 0.22f, 0.38f, 0.58f };
static const float wood_color[3] = { 0.22f, 0.34f, 0.20f };

void world_map_destroy(world_map_t *world_map)
{
	if (world_map->texture != NULL && gpu.device != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, world_map->texture);
	}
	SDL_zerop(world_map);
}

static bool load_artist_picture(world_map_t *world_map, const char *map_name)
{
	char path[160];
	const char *candidates[2];
	char named[160];

	SDL_snprintf(named, sizeof(named), "maps/%s.png", map_name != NULL ? map_name : "");
	candidates[0] = map_name != NULL && map_name[0] != '\0' ? named : NULL;
	candidates[1] = "maps/world.png";
	for (int i = 0; i < 2; i++)
	{
		size_t size = 0;
		void *data;
		uint8_t *pixels;
		int width;
		int height;
		int channels;

		if (candidates[i] == NULL || !file_io_data_file_exists(candidates[i]))
		{
			continue;
		}
		SDL_strlcpy(path, candidates[i], sizeof(path));
		data = file_io_load_data_file(path, &size);
		if (data == NULL)
		{
			continue;
		}
		pixels = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
		SDL_free(data);
		if (pixels == NULL)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "World map: cannot decode %s", path);
			continue;
		}
		world_map->texture = object_texture_upload(pixels, width, height);
		stbi_image_free(pixels);
		if (world_map->texture != NULL)
		{
			world_map->width = width;
			world_map->height = height;
			world_map->from_artist = true;
			SDL_Log("World map (the map window, the minimap): the artist's, %s (%dx%d)", path, width, height);
			return true;
		}
	}
	return false;
}

static void mix3(float out[3], const float a[3], const float b[3], float t)
{
	for (int k = 0; k < 3; k++)
	{
		out[k] = a[k] + (b[k] - a[k]) * t;
	}
}

static bool make_picture(world_map_t *world_map, const terrain_t *terrain, const water_t *water, const objects_t *objects)
{
	int size = SDL_min(terrain->size, MADE_SIZE_MOST);
	float extent = (float)terrain->size * terrain->cell_size_meters;
	float pixel_meters = extent / (float)size;
	uint8_t *pixels = SDL_malloc((size_t)size * (size_t)size * 4);
	uint16_t *woods = SDL_calloc((size_t)size * (size_t)size, sizeof(uint16_t));
	int tree_category = objects != NULL ? object_types_find_category(&objects->types, "trees") : -1;
	uint64_t start = SDL_GetTicks();

	if (pixels == NULL || woods == NULL)
	{
		SDL_free(pixels);
		SDL_free(woods);
		return false;
	}
	// The trees, counted per pixel (a wood: many)
	for (int i = 0; objects != NULL && i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		int type = instance->type;
		int x;
		int y;

		if (type < 0 || type >= objects->types.type_count || objects->types.types[type].category != tree_category)
		{
			continue;
		}
		x = SDL_clamp((int)(instance->x / pixel_meters), 0, size - 1);
		y = SDL_clamp((int)(instance->z / pixel_meters), 0, size - 1);
		woods[(size_t)y * (size_t)size + (size_t)x]++;
	}
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float world_x = ((float)x + 0.5f) * pixel_meters;
			float world_z = ((float)y + 0.5f) * pixel_meters;
			// The relief, lit from the north-west (up and left on the picture)
			float east = terrain_sample_height_meters(terrain, world_x + pixel_meters, world_z) - terrain_sample_height_meters(terrain,
				world_x - pixel_meters, world_z);
			float south = terrain_sample_height_meters(terrain, world_x, world_z + pixel_meters) - terrain_sample_height_meters(terrain,
				world_x, world_z - pixel_meters);
			float slope_light = SDL_clamp(0.5f - (east + south) / (2.0f * pixel_meters) * 0.9f, 0.0f, 1.0f);
			int cell_x = SDL_clamp((int)(world_x / terrain->cell_size_meters), 0, terrain->size - 1);
			int cell_z = SDL_clamp((int)(world_z / terrain->cell_size_meters), 0, terrain->size - 1);
			size_t cell = (size_t)cell_z * (size_t)terrain->size + (size_t)cell_x;
			int material = terrain->materials != NULL ? terrain->materials[cell * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_BASE] : MATERIAL_GRASS;
			float depth = 0.0f;
			float flow_x;
			float flow_z;
			float color[3];
			int wood = 0;

			if (material < 0 || material >= MATERIAL_COUNT)
			{
				material = MATERIAL_GRASS;
			}
			SDL_memcpy(color, material_colors[material], sizeof(color));
			// (the trees round: a few pixels' worth)
			for (int dy = -1; dy <= 1; dy++)
			{
				for (int dx = -1; dx <= 1; dx++)
				{
					int nx = SDL_clamp(x + dx, 0, size - 1);
					int ny = SDL_clamp(y + dy, 0, size - 1);

					wood += woods[(size_t)ny * (size_t)size + (size_t)nx];
				}
			}
			mix3(color, color, wood_color, SDL_min((float)wood / (9.0f * SDL_max(pixel_meters * pixel_meters / 40.0f, 0.15f)), 1.0f) * 0.75f);
			if (water != NULL && water_sample(water, world_x, world_z, &depth, &flow_x, &flow_z) && depth > 0.05f)
			{
				float water_color[3];

				mix3(water_color, shallow_water, deep_water, SDL_clamp(depth / 4.0f, 0.0f, 1.0f));
				mix3(color, color, water_color, SDL_clamp(depth / 0.3f, 0.0f, 1.0f));
				slope_light = 0.5f + (slope_light - 0.5f) * 0.25f;
			}
			// Shaded, then on paper
			for (int k = 0; k < 3; k++)
			{
				color[k] *= 0.65f + 0.7f * slope_light;
			}
			mix3(color, color, paper, 0.18f);
			for (int k = 0; k < 3; k++)
			{
				pixels[((size_t)y * (size_t)size + (size_t)x) * 4 + (size_t)k] = (uint8_t)SDL_clamp((int)(color[k] * 255.0f + 0.5f), 0, 255);
			}
			pixels[((size_t)y * (size_t)size + (size_t)x) * 4 + 3] = 255;
		}
	}
	world_map->texture = object_texture_upload(pixels, size, size);
	SDL_free(pixels);
	SDL_free(woods);
	if (world_map->texture == NULL)
	{
		return false;
	}
	world_map->width = size;
	world_map->height = size;
	world_map->from_artist = false;
	SDL_Log("World map (the map window, the minimap): made from the land, %dx%d, %llu ms", size, size, (unsigned long long)(SDL_GetTicks() - start));
	return true;
}

bool world_map_make(world_map_t *world_map, const terrain_t *terrain, const water_t *water, const objects_t *objects, const char *map_name)
{
	world_map_destroy(world_map);
	world_map->extent_meters = (float)terrain->size * terrain->cell_size_meters;
	if (!load_artist_picture(world_map, map_name) && !make_picture(world_map, terrain, water, objects))
	{
		return false;
	}
	world_map->made_ticks = SDL_GetTicks();
	SDL_strlcpy(world_map->made_for, map_name != NULL ? map_name : "", sizeof(world_map->made_for));
	return true;
}
