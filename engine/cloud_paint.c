// The painted sky. See cloud_paint.h.

#include "cloud_paint.h"
#include "cloud_weather.h"
#include "gpu.h"

void cloud_paint_reset(cloud_paint_t *paint)
{
	for (size_t t = 0; t < (size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE; t++)
	{
		paint->values[t * 4 + 0] = CLOUD_PAINT_NEUTRAL;
		paint->values[t * 4 + 1] = CLOUD_PAINT_NEUTRAL;
		paint->values[t * 4 + 2] = CLOUD_PAINT_NEUTRAL;
		paint->values[t * 4 + 3] = 0;
	}
	paint->revision++;
	paint->stamp_revision++;
	paint->changed_ticks = SDL_GetTicks();
}

void cloud_paint_brush(cloud_paint_t *paint, cloud_paint_channel_t channel, float u, float v, float radius, float amount, bool neutral)
{
	float center_x = u * (float)CLOUD_PAINT_SIZE;
	float center_y = v * (float)CLOUD_PAINT_SIZE;
	float reach = SDL_max(radius * (float)CLOUD_PAINT_SIZE, 0.5f);
	int32_t first_x = (int32_t)SDL_floorf(center_x - reach);
	int32_t last_x = (int32_t)SDL_ceilf(center_x + reach);
	int32_t first_y = (int32_t)SDL_floorf(center_y - reach);
	int32_t last_y = (int32_t)SDL_ceilf(center_y + reach);
	bool changed = false;
	float neutral_value = channel == CLOUD_PAINT_FLOCKS ? 0.0f : (float)CLOUD_PAINT_NEUTRAL;

	for (int32_t y = first_y; y <= last_y; y++)
	{
		for (int32_t x = first_x; x <= last_x; x++)
		{
			float dx = ((float)x + 0.5f - center_x) / reach;
			float dy = ((float)y + 0.5f - center_y) / reach;
			float distance = SDL_sqrtf(dx * dx + dy * dy);
			// Smooth to nothing at the edge
			float weight = distance >= 1.0f ? 0.0f : (1.0f - distance * distance) * (1.0f - distance * distance);
			int32_t wrapped_x = ((x % CLOUD_PAINT_SIZE) + CLOUD_PAINT_SIZE) % CLOUD_PAINT_SIZE;
			int32_t wrapped_y = ((y % CLOUD_PAINT_SIZE) + CLOUD_PAINT_SIZE) % CLOUD_PAINT_SIZE;
			uint8_t *value = &paint->values[((size_t)wrapped_y * CLOUD_PAINT_SIZE + (size_t)wrapped_x) * 4 + (size_t)channel];
			float current = (float)*value;
			float wanted;

			if (weight <= 0.0f)
			{
				continue;
			}
			if (neutral)
			{
				wanted = current + (neutral_value - current) * SDL_min(SDL_fabsf(amount) * weight * 4.0f, 1.0f);
			}
			else
			{
				wanted = current + amount * weight * 255.0f;
			}
			wanted = SDL_clamp(wanted, 0.0f, 255.0f);
			if ((uint8_t)SDL_lroundf(wanted) != *value)
			{
				*value = (uint8_t)SDL_lroundf(wanted);
				changed = true;
			}
		}
	}
	if (changed)
	{
		paint->revision++;
		if (channel != CLOUD_PAINT_FLOCKS)
		{
			paint->stamp_revision++;
		}
		paint->changed_ticks = SDL_GetTicks();
	}
}

float cloud_paint_sample(const cloud_paint_t *paint, cloud_paint_channel_t channel, float x_meters, float z_meters)
{
	float u = x_meters / CLOUD_WEATHER_METERS * (float)CLOUD_PAINT_SIZE - 0.5f;
	float v = z_meters / CLOUD_WEATHER_METERS * (float)CLOUD_PAINT_SIZE - 0.5f;
	int32_t x0 = (int32_t)SDL_floorf(u);
	int32_t y0 = (int32_t)SDL_floorf(v);
	float fx = u - (float)x0;
	float fy = v - (float)y0;
	float corners[4];

	for (int32_t c = 0; c < 4; c++)
	{
		int32_t x = (((x0 + (c & 1)) % CLOUD_PAINT_SIZE) + CLOUD_PAINT_SIZE) % CLOUD_PAINT_SIZE;
		int32_t y = (((y0 + (c >> 1)) % CLOUD_PAINT_SIZE) + CLOUD_PAINT_SIZE) % CLOUD_PAINT_SIZE;

		corners[c] = (float)paint->values[((size_t)y * CLOUD_PAINT_SIZE + (size_t)x) * 4 + (size_t)channel] * (1.0f / 255.0f);
	}
	return (corners[0] + (corners[1] - corners[0]) * fx) * (1.0f - fy) + (corners[2] + (corners[3] - corners[2]) * fx) * fy;
}

// A value's color in the picture: coverage from clear blue to white, density
// from white to dark gray, kind from green (puffs) through yellow to red
// (towers), flocks from clear blue through light violet to white
static void value_color(cloud_paint_channel_t channel, float value, uint8_t color_out[4])
{
	static const float ends[CLOUD_PAINT_CHANNELS][3][3] =
	{
		{ { 55.0f, 105.0f, 185.0f }, { 150.0f, 180.0f, 220.0f }, { 250.0f, 250.0f, 250.0f } },
		{ { 250.0f, 250.0f, 250.0f }, { 165.0f, 170.0f, 180.0f }, { 55.0f, 58.0f, 70.0f } },
		{ { 100.0f, 200.0f, 120.0f }, { 230.0f, 210.0f, 90.0f }, { 215.0f, 75.0f, 55.0f } },
		{ { 55.0f, 105.0f, 185.0f }, { 170.0f, 150.0f, 220.0f }, { 250.0f, 250.0f, 250.0f } },
	};
	int32_t low = value < 0.5f ? 0 : 1;
	float t = value < 0.5f ? value * 2.0f : (value - 0.5f) * 2.0f;

	for (int32_t c = 0; c < 3; c++)
	{
		float mixed = ends[channel][low][c] + (ends[channel][low + 1][c] - ends[channel][low][c]) * t;

		color_out[c] = (uint8_t)SDL_clamp((int32_t)(mixed + 0.5f), 0, 255);
	}
	color_out[3] = 255;
}

SDL_GPUTexture *cloud_paint_picture(cloud_paint_t *paint, cloud_paint_channel_t channel, float center_u, float center_v, float corner_out[2])
{
	uint8_t *pixels;
	int32_t offset_x = (int32_t)SDL_floorf(center_u * (float)CLOUD_PAINT_SIZE) - CLOUD_PAINT_SIZE / 2;
	int32_t offset_y = (int32_t)SDL_floorf(center_v * (float)CLOUD_PAINT_SIZE) - CLOUD_PAINT_SIZE / 2;

	corner_out[0] = (float)offset_x / (float)CLOUD_PAINT_SIZE;
	corner_out[1] = (float)offset_y / (float)CLOUD_PAINT_SIZE;

	if (paint->texture == NULL)
	{
		paint->texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, CLOUD_PAINT_SIZE, CLOUD_PAINT_SIZE, 1,
			1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		paint->shown_channel = -1;
		if (paint->texture == NULL)
		{
			return NULL;
		}
	}
	if (paint->shown_channel == (int32_t)channel && paint->shown_revision == paint->revision && paint->shown_offset[0] == offset_x
		&& paint->shown_offset[1] == offset_y)
	{
		return paint->texture;
	}
	pixels = SDL_malloc((size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE * 4);
	if (pixels == NULL)
	{
		return paint->texture;
	}
	for (int32_t y = 0; y < CLOUD_PAINT_SIZE; y++)
	{
		int32_t source_y = (((y + offset_y) % CLOUD_PAINT_SIZE) + CLOUD_PAINT_SIZE) % CLOUD_PAINT_SIZE;

		for (int32_t x = 0; x < CLOUD_PAINT_SIZE; x++)
		{
			int32_t source_x = (((x + offset_x) % CLOUD_PAINT_SIZE) + CLOUD_PAINT_SIZE) % CLOUD_PAINT_SIZE;
			size_t source = (size_t)source_y * CLOUD_PAINT_SIZE + (size_t)source_x;

			value_color(channel, (float)paint->values[source * 4 + (size_t)channel] * (1.0f / 255.0f),
				&pixels[((size_t)y * CLOUD_PAINT_SIZE + (size_t)x) * 4]);
		}
	}
	gpu_upload_texture(paint->texture, 0, 0, 0, 0, CLOUD_PAINT_SIZE, CLOUD_PAINT_SIZE, pixels, (size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE * 4);
	SDL_free(pixels);
	paint->shown_channel = (int32_t)channel;
	paint->shown_revision = paint->revision;
	paint->shown_offset[0] = offset_x;
	paint->shown_offset[1] = offset_y;
	return paint->texture;
}

void cloud_paint_destroy(cloud_paint_t *paint)
{
	if (paint->texture != NULL && gpu.device != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, paint->texture);
	}
	paint->texture = NULL;
	paint->shown_channel = -1;
}
