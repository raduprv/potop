// The sketch window's picture. See sketch_view.h.

#include "sketch_view.h"

#include "gpu.h"
#include "math3d.h"
#include "screenshot.h"
#include "terrain_gen.h"

#define VIEW_TEXELS ((size_t)SKETCH_VIEW_SIZE * SKETCH_VIEW_SIZE)

const sketch_preset_t sketch_presets[SKETCH_PRESET_COUNT] =
{
	{ "Plain", "Low, flat land", TERRAIN_SKETCH_PLAIN, { 176, 204, 122 } },
	{ "Hills", "Rolling hills", TERRAIN_SKETCH_HILLS, { 118, 158, 78 } },
	{ "Mountains", "Mountains: crests and peaks", TERRAIN_SKETCH_MOUNTAINS, { 146, 112, 88 } },
	{ "Valley", "A low valley: the rain cuts a river bed down it", TERRAIN_SKETCH_VALLEY, { 86, 156, 146 } },
	{ "Lake", "A lake about here (a hint: its shore where the land meets the water), as high as the land round it", TERRAIN_SKETCH_LAKE, { 62, 112, 196 } },
};

// The blank sheet's color
static const uint8_t paper_color[3] = { 228, 222, 204 };

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

const uint8_t *sketch_region_color(terrain_sketch_region_t region)
{
	for (int i = 0; i < SKETCH_PRESET_COUNT; i++)
	{
		if (sketch_presets[i].region == region)
		{
			return sketch_presets[i].color;
		}
	}
	return paper_color;
}

bool sketch_view_create(sketch_view_t *view)
{
	SDL_zerop(view);
	view->land = SDL_malloc(VIEW_TEXELS * sizeof(float));
	view->heights = SDL_malloc(VIEW_TEXELS * sizeof(float));
	view->pixels = SDL_calloc(VIEW_TEXELS, 4);
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

// The picture of the land as it will come out
static void compose_picture(sketch_view_t *view, const terrain_sketch_t *sketch, const float *heights, float most, float texel_meters)
{
	// (the light from the north west, up high; the relief a little taller
	// than it is, to read from above)
	float light[3] = { -0.55f, 0.65f, -0.52f };

#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < SKETCH_VIEW_SIZE; y++)
	{
		for (int32_t x = 0; x < SKETCH_VIEW_SIZE; x++)
		{
			size_t index = (size_t)y * SKETCH_VIEW_SIZE + (size_t)x;
			float height = heights[index];
			float left = heights[(size_t)y * SKETCH_VIEW_SIZE + (size_t)SDL_max(x - 1, 0)];
			float right = heights[(size_t)y * SKETCH_VIEW_SIZE + (size_t)SDL_min(x + 1, SKETCH_VIEW_SIZE - 1)];
			float back = heights[(size_t)SDL_max(y - 1, 0) * SKETCH_VIEW_SIZE + (size_t)x];
			float front = heights[(size_t)SDL_min(y + 1, SKETCH_VIEW_SIZE - 1) * SKETCH_VIEW_SIZE + (size_t)x];
			float normal[3] = { -(right - left) * 1.5f, 2.0f * texel_meters, -(front - back) * 1.5f };
			float length = SDL_sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
			float shade = clamp_float((normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2]) / SDL_max(length, 1e-6f), 0.0f, 1.0f);
			float color[3];
			float u = ((float)x + 0.5f) / (float)SKETCH_VIEW_SIZE;
			float v = ((float)y + 0.5f) / (float)SKETCH_VIEW_SIZE;
			int lake = terrain_sketch_lake_at(sketch, u, v);
			uint8_t *pixel = &view->pixels[index * 4];

			height_color(height / SDL_max(most, 1.0f), color);
			for (int c = 0; c < 3; c++)
			{
				color[c] *= 0.3f + 0.85f * shade;
			}
			if (lake >= 0 && height < sketch->lake_surfaces[lake] && terrain_sketch_lake_hint(sketch, u, v) >= 0.3f)
			{
				float deep = clamp_float((sketch->lake_surfaces[lake] - height) / 6.0f, 0.0f, 1.0f);

				color[0] = lerp_float(70.0f, 30.0f, deep);
				color[1] = lerp_float(120.0f, 70.0f, deep);
				color[2] = lerp_float(170.0f, 140.0f, deep);
			}
			pixel[0] = (uint8_t)clamp_float(color[0], 0.0f, 255.0f);
			pixel[1] = (uint8_t)clamp_float(color[1], 0.0f, 255.0f);
			pixel[2] = (uint8_t)clamp_float(color[2], 0.0f, 255.0f);
			pixel[3] = 255;
		}
	}
}

// The sheet as painted: each region its flat color, a darker line where
// two meet
static void compose_paper(sketch_view_t *view, const terrain_sketch_t *sketch)
{
	const uint8_t *regions = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	int32_t step = TERRAIN_SKETCH_SIZE / SKETCH_VIEW_SIZE;

#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < SKETCH_VIEW_SIZE; y++)
	{
		for (int32_t x = 0; x < SKETCH_VIEW_SIZE; x++)
		{
			int32_t texel_x = x * step;
			int32_t texel_y = y * step;
			uint8_t region = regions[(size_t)texel_y * TERRAIN_SKETCH_SIZE + (size_t)texel_x];
			const uint8_t *color = sketch_region_color((terrain_sketch_region_t)region);
			uint8_t *pixel = &view->pixels[((size_t)y * SKETCH_VIEW_SIZE + (size_t)x) * 4];
			bool border = false;

			for (int k = 0; k < 4 && !border; k++)
			{
				int32_t nx = clamp_int(texel_x + (k == 0 ? step : k == 1 ? -step : 0), 0, TERRAIN_SKETCH_SIZE - 1);
				int32_t ny = clamp_int(texel_y + (k == 2 ? step : k == 3 ? -step : 0), 0, TERRAIN_SKETCH_SIZE - 1);

				border = regions[(size_t)ny * TERRAIN_SKETCH_SIZE + (size_t)nx] != region;
			}
			for (int c = 0; c < 3; c++)
			{
				pixel[c] = border ? (uint8_t)(color[c] * 0.6f) : color[c];
			}
			pixel[3] = 255;
		}
	}
}

static void upload_picture(sketch_view_t *view)
{
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
	if (sketch->revision != view->painted_revision)
	{
		view->painted = terrain_sketch_painted(sketch);
		view->painted_revision = sketch->revision;
	}
	if (view->show_result != view->shown_result)
	{
		view->shown_result = view->show_result;
		view->picture_dirty = true;
	}
	// The sheet as painted
	if (!view->show_result)
	{
		if (sketch->revision == view->paper_revision && !view->picture_dirty && view->paper_valid)
		{
			return;
		}
		view->paper_revision = sketch->revision;
		view->paper_valid = true;
		compose_paper(view, sketch);
		upload_picture(view);
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
	// The sketch over it: as it is painted, every so often; the erosion once
	// it is left alone
	if ((!view->heights_valid || view->heights_revision != sketch->revision)
		&& ((double)(now_ticks - view->computed_ticks) * 0.001 >= SKETCH_VIEW_QUIET_SECONDS || !view->heights_valid))
	{
		view->computed_ticks = now_ticks;
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
	compose_picture(view, sketch, view->heights, settings->max_height_meters, texel_meters);
	upload_picture(view);
}

bool sketch_view_save_picture(const sketch_view_t *view, const char *path)
{
	return view->pixels != NULL && screenshot_save(path, view->pixels, SKETCH_VIEW_SIZE, SKETCH_VIEW_SIZE);
}
