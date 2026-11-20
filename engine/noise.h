// Procedural noise: 2D simplex noise for terrain generation and periodic
// (tileable) gradient noise for the fallback textures, plus the usual
// fractal sums on top of them. Everything is deterministic for a given seed.

#ifndef NOISE_H
#define NOISE_H

#include <stdint.h>

typedef struct
{
	// Doubled permutation table, so lookups never need wrapping
	uint8_t permutation[512];
} noise_context_t;

// Small fast PRNG (splitmix64). Good enough for generation, not for crypto.
typedef struct
{
	uint64_t state;
} random_t;

void random_seed(random_t *random, uint64_t seed);
uint64_t random_next(random_t *random);
// Uniform in [0, 1)
float random_float(random_t *random);
// Uniform in [minimum, maximum)
float random_range(random_t *random, float minimum, float maximum);

void noise_initialize(noise_context_t *context, uint64_t seed);

// Simplex noise, roughly in -1..1, one feature per unit.
float noise_simplex(const noise_context_t *context, float x, float y);

// Fractal sum of simplex octaves, normalized to roughly -1..1. Each octave
// doubles the frequency and multiplies the amplitude by persistence.
float noise_fbm(const noise_context_t *context, float x, float y, int octave_count, float persistence);

// Ridged multifractal: sharp crests where the noise crosses zero, the
// classic look for mountain ridges. Output roughly 0..1.
float noise_ridged(const noise_context_t *context, float x, float y, int octave_count, float persistence);

// The same with its crests rounded: the noise's distance from zero eased
// over softness (in the noise's own units, of its -1..1; 0: as sharp as
// noise_ridged), so neither its crests nor where they meet are a crease
// sharper than the grid it is laid on (its peaks no single vertex)
float noise_ridged_soft(const noise_context_t *context, float x, float y, int octave_count, float persistence, float softness);

// Gradient noise that repeats every `period` units in x and y (period must be
// 1..256). Roughly -1..1.
float noise_periodic(const noise_context_t *context, float x, float y, int period);

// Fractal sum of periodic noise; repeats every base_period units, octave i
// uses period base_period << i so the sum stays seamless.
float noise_periodic_fbm(const noise_context_t *context, float x, float y, int base_period, int octave_count, float persistence);

#endif
