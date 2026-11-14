// Terrain tools. See tools.h.

#include "tools.h"

#include <SDL3/SDL.h>

void tools_initialize(tool_state_t *tools, const tool_settings_t *settings)
{
	SDL_zerop(tools);
	tools->current_tool = TOOL_NONE;
	tools->current_soil = SOIL_EARTH;
	tools->brush_radius_meters = settings->brush_radius_meters;
	tools->current_stamp = STAMP_DAM;
	tools->stamp_height_meters = settings->stamp_height_meters;
	tools->light_kind = TOOL_LIGHT_LAMP;
	tools->light_intensity = 1.5f;
	tools->light_radius_meters = 8.0f;
	tools->light_height_meters = 1.8f;
}

void tools_shutdown(tool_state_t *tools)
{
	SDL_free(tools->scratch_heights);
	tools->scratch_heights = NULL;
	tools->scratch_capacity = 0;
}

const char *tools_tool_name(tool_type_t tool)
{
	static const char *names[TOOL_COUNT] = { "raise", "dig", "paint soil", "smooth", "stamp", "plant", "clear", "layout", "add water",
		"sponge", "spring", "remove layout", "object", "inspect", "effect", "light", "no tool" };

	return (tool >= 0 && tool < TOOL_COUNT) ? names[tool] : "?";
}

const char *tools_soil_name(soil_type_t soil)
{
	static const char *names[SOIL_TYPE_COUNT] = { "sand", "earth", "clay", "rock" };

	return (soil >= 0 && soil < SOIL_TYPE_COUNT) ? names[soil] : "?";
}

// Cheap integer hash, used to dither the soft edge of the soil brush
static uint32_t hash_cell(int32_t x, int32_t y)
{
	uint32_t value = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u;

	value ^= value >> 13;
	value *= 0x5BD1E995u;
	value ^= value >> 15;
	return value;
}

// Smooth bell from 1 at the center to 0 at the brush edge
static float brush_falloff(float distance, float radius)
{
	if (distance >= radius)
	{
		return 0.0f;
	}
	return 0.5f * (1.0f + SDL_cosf(MATH3D_PI * distance / radius));
}

void tools_apply(tool_state_t *tools, terrain_t *terrain, const tool_settings_t *settings, float step_seconds)
{
	float cell_size = terrain->cell_size_meters;
	float radius_cells = tools->brush_radius_meters / cell_size;
	float center_x = tools->hover_point.x / cell_size;
	float center_y = tools->hover_point.z / cell_size;
	int32_t minimum_x = clamp_int((int32_t)SDL_floorf(center_x - radius_cells), 0, terrain->size - 1);
	int32_t minimum_y = clamp_int((int32_t)SDL_floorf(center_y - radius_cells), 0, terrain->size - 1);
	int32_t maximum_x = clamp_int((int32_t)SDL_ceilf(center_x + radius_cells), 0, terrain->size - 1);
	int32_t maximum_y = clamp_int((int32_t)SDL_ceilf(center_y + radius_cells), 0, terrain->size - 1);
	int32_t region_width = maximum_x - minimum_x + 1;
	int32_t region_height = maximum_y - minimum_y + 1;
	float raise_units = settings->raise_meters_per_second * step_seconds * (float)TERRAIN_HEIGHT_UNITS_PER_METER;

	if (!tools->hover_valid || region_width <= 0 || region_height <= 0 || tools->current_tool >= TOOL_STAMP)
	{
		return;
	}

	if (tools->current_tool == TOOL_SMOOTH)
	{
		// Smoothing reads neighbors, so it works from a copy of the region
		// (plus a one cell border) to stay independent of visiting order
		int32_t needed = (region_width + 2) * (region_height + 2);

		if (needed > tools->scratch_capacity)
		{
			int32_t *grown = SDL_realloc(tools->scratch_heights, (size_t)needed * sizeof(int32_t));

			if (grown == NULL)
			{
				return;
			}
			tools->scratch_heights = grown;
			tools->scratch_capacity = needed;
		}
		for (int32_t y = 0; y < region_height + 2; y++)
		{
			for (int32_t x = 0; x < region_width + 2; x++)
			{
				tools->scratch_heights[y * (region_width + 2) + x] =
					terrain_height_units_clamped(terrain, minimum_x + x - 1, minimum_y + y - 1);
			}
		}
	}

	for (int32_t y = minimum_y; y <= maximum_y; y++)
	{
		for (int32_t x = minimum_x; x <= maximum_x; x++)
		{
			float distance = SDL_sqrtf(((float)x - center_x) * ((float)x - center_x) + ((float)y - center_y) * ((float)y - center_y));
			float falloff = brush_falloff(distance, radius_cells);
			int32_t index = terrain_index(terrain, x, y);

			if (falloff <= 0.0f)
			{
				continue;
			}

			switch (tools->current_tool)
			{
				case TOOL_RAISE:
					terrain->heights[index] += (int32_t)(raise_units * falloff);
					// New material on top is of the selected soil, and buries
					// the silt there
					if (falloff > 0.3f)
					{
						terrain->soil_types[index] = (uint8_t)tools->current_soil;
						terrain->silt_thickness[index] = 0;
					}
					break;
				case TOOL_DIG:
					terrain->heights[index] = SDL_max(0, terrain->heights[index] - (int32_t)(raise_units * falloff));
					break;
				case TOOL_PAINT_SOIL:
					// Dithered edge: cells near the rim are painted with a
					// probability that follows the falloff
					if (falloff * 1.6f > (float)(hash_cell(x, y) & 0xFFFF) / 65535.0f)
					{
						terrain->soil_types[index] = (uint8_t)tools->current_soil;
						terrain->silt_thickness[index] = 0;
					}
					break;
				case TOOL_SMOOTH:
				{
					int32_t stride = region_width + 2;
					int32_t local_x = x - minimum_x + 1;
					int32_t local_y = y - minimum_y + 1;
					const int32_t *scratch = tools->scratch_heights;
					int64_t neighbor_sum = (int64_t)scratch[local_y * stride + local_x - 1] + scratch[local_y * stride + local_x + 1]
						+ scratch[(local_y - 1) * stride + local_x] + scratch[(local_y + 1) * stride + local_x];
					float average = (float)neighbor_sum * 0.25f;
					float current = (float)scratch[local_y * stride + local_x];
					float amount = clamp_float(settings->smooth_strength_per_second * step_seconds * falloff, 0.0f, 1.0f);

					terrain->heights[index] = (int32_t)SDL_lroundf(current + (average - current) * amount);
					break;
				}
				default:
					break;
			}
		}
	}

	// Digging below the bedrock lowers it; silt never outgrows its column
	terrain_fix_layers(terrain, minimum_x, minimum_y, maximum_x, maximum_y);
	terrain_mark_cells_changed(terrain, minimum_x, minimum_y, maximum_x, maximum_y);
}
