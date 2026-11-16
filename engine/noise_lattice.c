// The value noise's lattice. See noise_lattice.h.

#include "noise_lattice.h"

#include <SDL3/SDL.h>

static uint8_t *lattice;

// A well mixed hash (PCG's output step)
static uint32_t pcg_hash(uint32_t value)
{
	uint32_t state = value * 747796405u + 2891336453u;
	uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

	return (word >> 22u) ^ word;
}

const uint8_t *noise_lattice(void)
{
	if (lattice == NULL)
	{
		lattice = SDL_malloc((size_t)NOISE_LATTICE_SIZE * NOISE_LATTICE_SIZE);
		if (lattice != NULL)
		{
			for (uint32_t i = 0; i < (uint32_t)NOISE_LATTICE_SIZE * NOISE_LATTICE_SIZE; i++)
			{
				lattice[i] = (uint8_t)(pcg_hash(i + 0x5bd1e995u) >> 24);
			}
		}
	}
	return lattice;
}

static float lattice_at(const uint8_t *points, int32_t x, int32_t y)
{
	x &= NOISE_LATTICE_SIZE - 1;
	y &= NOISE_LATTICE_SIZE - 1;
	return (float)points[y * NOISE_LATTICE_SIZE + x] / 255.0f;
}

float noise_lattice_value(float x, float y)
{
	const uint8_t *points = noise_lattice();
	float cell_x = SDL_floorf(x);
	float cell_y = SDL_floorf(y);
	float fraction_x = x - cell_x;
	float fraction_y = y - cell_y;
	float blend_x = fraction_x * fraction_x * (3.0f - 2.0f * fraction_x);
	float blend_y = fraction_y * fraction_y * (3.0f - 2.0f * fraction_y);
	int32_t corner_x;
	int32_t corner_y;
	float low;
	float high;

	if (points == NULL)
	{
		return 0.5f;
	}
	corner_x = (int32_t)cell_x;
	corner_y = (int32_t)cell_y;
	low = lattice_at(points, corner_x, corner_y) + (lattice_at(points, corner_x + 1, corner_y) - lattice_at(points, corner_x, corner_y)) * blend_x;
	high = lattice_at(points, corner_x, corner_y + 1)
		+ (lattice_at(points, corner_x + 1, corner_y + 1) - lattice_at(points, corner_x, corner_y + 1)) * blend_x;
	return low + (high - low) * blend_y;
}
