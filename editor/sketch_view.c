// The sketch window's picture. See sketch_view.h.

#include "sketch_view.h"

#include "gpu.h"
#include "math3d.h"
#include "screenshot.h"
#include "terrain_gen.h"

#define VIEW_TEXELS ((size_t)SKETCH_VIEW_SIZE * SKETCH_VIEW_SIZE)

const sketch_preset_t sketch_presets[SKETCH_PRESET_COUNT] =
{
	{ "Mountains", "High and rough: crests and crags", { 0.85f, 0.92f, -1.0f, false } },
	{ "Hills", "Middling and rolling", { 0.42f, 0.55f, -1.0f, false } },
	{ "Plain", "Low and smooth", { 0.10f, 0.12f, -1.0f, false } },
	{ "Valley", "Low, a little rough: the rain cuts a river bed down it", { 0.03f, 0.35f, -1.0f, false } },
	{ "Lake", "Water: the land under it sunk, filled to a little below its lowest shore", { -1.0f, -1.0f, 1.0f, false } },
};

int sketch_preset_find(const char *name)
{
	for (int i = 0; i < SKETCH_PRESET_COUNT; i++)
	{
		if (SDL_strcasecmp(name, sketch_presets[i].name) == 0)
		{
			return i;
		}
	}
	return SDL_strcasecmp(name, "erase") == 0 ? SKETCH_PRESET_COUNT : -1;
}

bool sketch_view_create(sketch_view_t *view)
{
	SDL_zerop(view);
	view->land = SDL_malloc(VIEW_TEXELS * sizeof(float));
	view->heights = SDL_malloc(VIEW_TEXELS * sizeof(float));
	view->pixels = SDL_calloc(VIEW_TEXELS, 4);
	view->tint_painted = true;
	if (view->land == NULL || view->heights == NULL || view->pixels == NULL)
	{
		sketch_view_destroy(view);
		return false;
	}
	return true;
}

void sketch_view_destroy(sketch_view_t *view)
{
	if (view->texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, view->texture);
	}
	SDL_free(view->land);
	SDL_free(view->heights);
	SDL_free(view->pixels);
	SDL_zerop(view);
}

// The land's color by its share of the highest ground
static void height_color(float share, float color_out[3])
{
	static const float stops[5][4] =
	{
		{ 0.00f, 86.0f, 125.0f, 70.0f },
		{ 0.25f, 120.0f, 142.0f, 82.0f },
		{ 0.50f, 152.0f, 136.0f, 96.0f },
		{ 0.78f, 128.0f, 120.0f, 112.0f },
		{ 0.95f, 230.0f, 230.0f, 232.0f },
	};

	share = clamp_float(share, 0.0f, 1.0f);
	for (int k = 0; k < 4; k++)
	{
		if (share <= stops[k + 1][0] || k == 3)
		{
			float along = clamp_float((share - stops[k][0]) / (stops[k + 1][0] - stops[k][0]), 0.0f, 1.0f);

			for (int c = 0; c < 3; c++)
			{
				color_out[c] = lerp_float(stops[k][c + 1], stops[k + 1][c + 1], along);
			}
			return;
		}
	}
}

static void compose_picture(sketch_view_t *view, const terrain_sketch_t *sketch, float most, float texel_meters)
{
	const uint8_t *height_weight = terrain_sketch_plane(sketch, TERRAIN_SKETCH_HEIGHT_WEIGHT);
	const uint8_t *relief_weight = terrain_sketch_plane(sketch, TERRAIN_SKETCH_RELIEF_WEIGHT);
	const uint8_t *water = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);
	// (the light from the north west, up high; the relief a little taller
	// than it is, to read from above)
	float light[3] = { -0.55f, 0.65f, -0.52f };
	int32_t step = TERRAIN_SKETCH_SIZE / SKETCH_VIEW_SIZE;

#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < SKETCH_VIEW_SIZE; y++)
	{
		for (int32_t x = 0; x < SKETCH_VIEW_SIZE; x++)
		{
			size_t index = (size_t)y * SKETCH_VIEW_SIZE + (size_t)x;
			float height = view->heights[index];
			float left = view->heights[(size_t)y * SKETCH_VIEW_SIZE + (size_t)SDL_max(x - 1, 0)];
			float right = view->heights[(size_t)y * SKETCH_VIEW_SIZE + (size_t)SDL_min(x + 1, SKETCH_VIEW_SIZE - 1)];
			float back = view->heights[(size_t)SDL_max(y - 1, 0) * SKETCH_VIEW_SIZE + (size_t)x];
			float front = view->heights[(size_t)SDL_min(y + 1, SKETCH_VIEW_SIZE - 1) * SKETCH_VIEW_SIZE + (size_t)x];
			float normal[3] = { -(right - left) * 1.5f, 2.0f * texel_meters, -(front - back) * 1.5f };
			float length = SDL_sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
			float shade = clamp_float((normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2]) / SDL_max(length, 1e-6f), 0.0f, 1.0f);
			float color[3];
			float u = ((float)x + 0.5f) / (float)SKETCH_VIEW_SIZE;
			float v = ((float)y + 0.5f) / (float)SKETCH_VIEW_SIZE;
			int lake = terrain_sketch_lake_at(sketch, u, v);
			size_t texel = (size_t)(y * step) * TERRAIN_SKETCH_SIZE + (size_t)(x * step);
			uint8_t *pixel = &view->pixels[index * 4];

			height_color(height / SDL_max(most, 1.0f), color);
			for (int c = 0; c < 3; c++)
			{
				color[c] *= 0.3f + 0.85f * shade;
			}
			if (lake >= 0 && height < sketch->lake_surfaces[lake])
			{
				float deep = clamp_float((sketch->lake_surfaces[lake] - height) / 6.0f, 0.0f, 1.0f);

				color[0] = lerp_float(70.0f, 30.0f, deep);
				color[1] = lerp_float(120.0f, 70.0f, deep);
				color[2] = lerp_float(170.0f, 140.0f, deep);
			}
			if (view->tint_painted)
			{
				static const float tints[3][3] = { { 235.0f, 140.0f, 40.0f }, { 160.0f, 80.0f, 210.0f }, { 40.0f, 110.0f, 235.0f } };
				float amounts[3] = { 0.30f * (float)height_weight[texel] / 255.0f, 0.25f * (float)relief_weight[texel] / 255.0f,
					0.35f * (float)water[texel] / 255.0f };

				for (int t = 0; t < 3; t++)
				{
					for (int c = 0; c < 3; c++)
					{
						color[c] = lerp_float(color[c], tints[t][c], amounts[t]);
					}
				}
			}
			pixel[0] = (uint8_t)clamp_float(color[0], 0.0f, 255.0f);
			pixel[1] = (uint8_t)clamp_float(color[1], 0.0f, 255.0f);
			pixel[2] = (uint8_t)clamp_float(color[2], 0.0f, 255.0f);
			pixel[3] = 255;
		}
	}
}

void sketch_view_update(sketch_view_t *view, terrain_sketch_t *sketch, const terrain_settings_t *settings, uint32_t seed, int32_t map_size,
	float cell_size_meters, uint64_t now_ticks)
{
	float map_meters = (float)map_size * cell_size_meters;
	float texel_meters = map_meters / (float)SKETCH_VIEW_SIZE;
	bool land_changed;
	double quiet;

	if (view->land == NULL || sketch == NULL || sketch->planes == NULL)
	{
		return;
	}
	land_changed = !view->land_valid || view->land_seed != seed || view->land_map_meters != map_meters
		|| SDL_memcmp(&view->land_settings, settings, sizeof(*settings)) != 0;
	if (land_changed)
	{
		terrain_generate_heights(view->land, SKETCH_VIEW_SIZE, texel_meters, settings, seed, NULL, TERRAIN_HEIGHTS_LAND);
		view->land_seed = seed;
		view->land_settings = *settings;
		view->land_map_meters = map_meters;
		view->land_valid = true;
		view->heights_valid = false;
	}
	if (sketch->revision != view->seen_revision)
	{
		view->seen_revision = sketch->revision;
		view->changed_ticks = now_ticks;
	}
	quiet = (double)(now_ticks - view->changed_ticks) * 0.001;
	// The sketch over it: soon after it changes; the erosion once it is left alone
	if ((!view->heights_valid || view->heights_revision != sketch->revision) && (quiet >= SKETCH_VIEW_QUIET_SECONDS || !view->heights_valid))
	{
		SDL_memcpy(view->heights, view->land, VIEW_TEXELS * sizeof(float));
		terrain_generate_heights(view->heights, SKETCH_VIEW_SIZE, texel_meters, settings, seed, sketch, 0);
		view->heights_revision = sketch->revision;
		view->heights_eroded = false;
		view->heights_valid = true;
		view->picture_dirty = true;
	}
	else if (view->heights_valid && !view->heights_eroded && quiet >= SKETCH_VIEW_EROSION_SECONDS && settings->erosion_droplets > 0.0f)
	{
		SDL_memcpy(view->heights, view->land, VIEW_TEXELS * sizeof(float));
		terrain_generate_heights(view->heights, SKETCH_VIEW_SIZE, texel_meters, settings, seed, sketch, TERRAIN_HEIGHTS_EROSION);
		view->heights_eroded = true;
		view->picture_dirty = true;
	}
	if (!view->picture_dirty)
	{
		return;
	}
	compose_picture(view, sketch, settings->max_height_meters, texel_meters);
	if (view->texture == NULL && gpu.device != NULL)
	{
		view->texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SKETCH_VIEW_SIZE, SKETCH_VIEW_SIZE, 1, 1,
			SDL_GPU_TEXTUREUSAGE_SAMPLER);
	}
	if (view->texture != NULL)
	{
		gpu_upload_texture(view->texture, 0, 0, 0, 0, SKETCH_VIEW_SIZE, SKETCH_VIEW_SIZE, view->pixels, VIEW_TEXELS * 4);
	}
	view->picture_dirty = false;
}

bool sketch_view_save_picture(const sketch_view_t *view, const char *path)
{
	return view->pixels != NULL && screenshot_save(path, view->pixels, SKETCH_VIEW_SIZE, SKETCH_VIEW_SIZE);
}
