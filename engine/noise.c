// Procedural noise. See noise.h.
//
// The simplex implementation follows Stefan Gustavson's well known public
// domain description ("Simplex noise demystified", 2005). The 2D variant is
// not affected by the old (and long expired) simplex noise patent anyway.

#include "noise.h"

#include <math.h>

void random_seed(random_t *random, uint64_t seed)
{
	random->state = seed;
}

uint64_t random_next(random_t *random)
{
	uint64_t value;

	random->state += 0x9E3779B97F4A7C15ULL;
	value = random->state;
	value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
	value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
	return value ^ (value >> 31);
}

float random_float(random_t *random)
{
	// Top 24 bits give an exact float in [0, 1)
	return (float)(random_next(random) >> 40) * (1.0f / 16777216.0f);
}

float random_range(random_t *random, float minimum, float maximum)
{
	return minimum + (maximum - minimum) * random_float(random);
}

void noise_initialize(noise_context_t *context, uint64_t seed)
{
	random_t random;
	uint8_t table[256];

	random_seed(&random, seed);
	for (int i = 0; i < 256; i++)
	{
		table[i] = (uint8_t)i;
	}
	// Fisher-Yates shuffle
	for (int i = 255; i > 0; i--)
	{
		int j = (int)(random_next(&random) % (uint64_t)(i + 1));
		uint8_t swap = table[i];

		table[i] = table[j];
		table[j] = swap;
	}
	for (int i = 0; i < 512; i++)
	{
		context->permutation[i] = table[i & 255];
	}
}

// Twelve gradient directions (the 2D projection of Gustavson's grad3 table)
static const float simplex_gradients[12][2] =
{
	{ 1.0f, 1.0f }, { -1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f },
	{ 1.0f, 0.0f }, { -1.0f, 0.0f }, { 1.0f, 0.0f }, { -1.0f, 0.0f },
	{ 0.0f, 1.0f }, { 0.0f, -1.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f }
};

static float simplex_corner(int gradient_index, float x, float y)
{
	float falloff = 0.5f - x * x - y * y;

	if (falloff <= 0.0f)
	{
		return 0.0f;
	}
	falloff *= falloff;
	return falloff * falloff * (simplex_gradients[gradient_index][0] * x + simplex_gradients[gradient_index][1] * y);
}

float noise_simplex(const noise_context_t *context, float x, float y)
{
	const float skew_factor = 0.36602540378f;    // (sqrt(3) - 1) / 2
	const float unskew_factor = 0.21132486540f;  // (3 - sqrt(3)) / 6
	const uint8_t *permutation = context->permutation;
	float skew = (x + y) * skew_factor;
	int cell_i = (int)floorf(x + skew);
	int cell_j = (int)floorf(y + skew);
	float unskew = (float)(cell_i + cell_j) * unskew_factor;
	float x0 = x - ((float)cell_i - unskew);
	float y0 = y - ((float)cell_j - unskew);
	// Which of the two triangles of the skewed cell we are in
	int offset_i = x0 > y0 ? 1 : 0;
	int offset_j = x0 > y0 ? 0 : 1;
	float x1 = x0 - (float)offset_i + unskew_factor;
	float y1 = y0 - (float)offset_j + unskew_factor;
	float x2 = x0 - 1.0f + 2.0f * unskew_factor;
	float y2 = y0 - 1.0f + 2.0f * unskew_factor;
	int wrapped_i = cell_i & 255;
	int wrapped_j = cell_j & 255;
	int gradient_0 = permutation[wrapped_i + permutation[wrapped_j]] % 12;
	int gradient_1 = permutation[wrapped_i + offset_i + permutation[wrapped_j + offset_j]] % 12;
	int gradient_2 = permutation[wrapped_i + 1 + permutation[wrapped_j + 1]] % 12;

	// 70 scales the sum to roughly -1..1
	return 70.0f * (simplex_corner(gradient_0, x0, y0) + simplex_corner(gradient_1, x1, y1) + simplex_corner(gradient_2, x2, y2));
}

float noise_fbm(const noise_context_t *context, float x, float y, int octave_count, float persistence)
{
	float sum = 0.0f;
	float amplitude = 1.0f;
	float amplitude_total = 0.0f;
	float frequency = 1.0f;

	for (int octave = 0; octave < octave_count; octave++)
	{
		// The per-octave offset decorrelates octaves (otherwise they all
		// share a lattice point at the origin)
		float offset = (float)octave * 37.17f;

		sum += amplitude * noise_simplex(context, x * frequency + offset, y * frequency - offset);
		amplitude_total += amplitude;
		amplitude *= persistence;
		frequency *= 2.0f;
	}
	return sum / amplitude_total;
}

float noise_ridged(const noise_context_t *context, float x, float y, int octave_count, float persistence)
{
	float sum = 0.0f;
	float amplitude = 1.0f;
	float amplitude_total = 0.0f;
	float frequency = 1.0f;
	float previous = 1.0f;

	for (int octave = 0; octave < octave_count; octave++)
	{
		float offset = (float)octave * 53.91f;
		float ridge = 1.0f - fabsf(noise_simplex(context, x * frequency + offset, y * frequency + offset));

		ridge *= ridge;
		// Weighting by the previous octave keeps detail on the crests and
		// leaves the valleys smoother, as in Musgrave's ridged multifractal
		sum += ridge * amplitude * previous;
		amplitude_total += amplitude;
		previous = ridge;
		amplitude *= persistence;
		frequency *= 2.0f;
	}
	return sum / amplitude_total;
}

static float quintic_fade(float value)
{
	return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

static float periodic_gradient_dot(const noise_context_t *context, int lattice_x, int lattice_y, float offset_x, float offset_y)
{
	// Eight evenly spaced unit gradients
	static const float directions[8][2] =
	{
		{ 1.0f, 0.0f }, { 0.70710678f, 0.70710678f }, { 0.0f, 1.0f }, { -0.70710678f, 0.70710678f },
		{ -1.0f, 0.0f }, { -0.70710678f, -0.70710678f }, { 0.0f, -1.0f }, { 0.70710678f, -0.70710678f }
	};
	int hash = context->permutation[context->permutation[lattice_x & 255] + (lattice_y & 255)] & 7;

	return directions[hash][0] * offset_x + directions[hash][1] * offset_y;
}

float noise_periodic(const noise_context_t *context, float x, float y, int period)
{
	int floor_x = (int)floorf(x);
	int floor_y = (int)floorf(y);
	float fraction_x = x - (float)floor_x;
	float fraction_y = y - (float)floor_y;
	// Lattice coordinates wrap at the period, which is what makes it tile
	int lattice_x0 = ((floor_x % period) + period) % period;
	int lattice_y0 = ((floor_y % period) + period) % period;
	int lattice_x1 = (lattice_x0 + 1) % period;
	int lattice_y1 = (lattice_y0 + 1) % period;
	float dot_00 = periodic_gradient_dot(context, lattice_x0, lattice_y0, fraction_x, fraction_y);
	float dot_10 = periodic_gradient_dot(context, lattice_x1, lattice_y0, fraction_x - 1.0f, fraction_y);
	float dot_01 = periodic_gradient_dot(context, lattice_x0, lattice_y1, fraction_x, fraction_y - 1.0f);
	float dot_11 = periodic_gradient_dot(context, lattice_x1, lattice_y1, fraction_x - 1.0f, fraction_y - 1.0f);
	float fade_x = quintic_fade(fraction_x);
	float fade_y = quintic_fade(fraction_y);
	float top = dot_00 + (dot_10 - dot_00) * fade_x;
	float bottom = dot_01 + (dot_11 - dot_01) * fade_x;

	// Gradient noise peaks around +-0.7 in 2D; scale to roughly -1..1
	return 1.4f * (top + (bottom - top) * fade_y);
}

float noise_periodic_fbm(const noise_context_t *context, float x, float y, int base_period, int octave_count, float persistence)
{
	float sum = 0.0f;
	float amplitude = 1.0f;
	float amplitude_total = 0.0f;
	int period = base_period;
	float frequency = 1.0f;

	for (int octave = 0; octave < octave_count && period <= 256; octave++)
	{
		sum += amplitude * noise_periodic(context, x * frequency, y * frequency, period);
		amplitude_total += amplitude;
		amplitude *= persistence;
		frequency *= 2.0f;
		period *= 2;
	}
	return amplitude_total > 0.0f ? sum / amplitude_total : 0.0f;
}
