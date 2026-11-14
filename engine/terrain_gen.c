// Procedural terrain generation. See terrain_gen.h.

#include "terrain_gen.h"
#include "noise.h"

#include <SDL3/SDL.h>

// Each noise layer gets its own permutation table so that hills, ridges,
// warp and masks are independent of each other
typedef struct
{
	noise_context_t warp;
	noise_context_t hills;
	noise_context_t ridges;
	noise_context_t mountain_mask;
	noise_context_t soil_variation;
	noise_context_t gullies;
} generation_noise_t;

static void initialize_generation_noise(generation_noise_t *noise, uint32_t seed)
{
	uint64_t base = (uint64_t)seed * 0x9E3779B97F4A7C15ULL;

	noise_initialize(&noise->warp, base + 1);
	noise_initialize(&noise->hills, base + 2);
	noise_initialize(&noise->ridges, base + 3);
	noise_initialize(&noise->mountain_mask, base + 4);
	noise_initialize(&noise->soil_variation, base + 5);
	noise_initialize(&noise->gullies, base + 6);
}

// Raw shape of the landscape at normalized coordinates (0..1 across the
// map). The result is unnormalized; the caller rescales the whole map.
static float sample_landscape(const generation_noise_t *noise, const terrain_settings_t *settings, int octave_count, float u, float v)
{
	float x = u * settings->feature_count;
	float y = v * settings->feature_count;
	float warp_x = noise_fbm(&noise->warp, x + 5.2f, y + 1.3f, 4, 0.5f);
	float warp_y = noise_fbm(&noise->warp, x - 3.1f, y + 7.7f, 4, 0.5f);
	float hills;
	float ridges;
	float mask;
	float mask_threshold;

	// Domain warp: bend the coordinates with another noise before sampling,
	// which turns round blobs into meandering, organic shapes
	x += settings->warp_amount * warp_x;
	y += settings->warp_amount * warp_y;

	hills = noise_fbm(&noise->hills, x, y, octave_count, settings->persistence) * 0.5f + 0.5f;
	ridges = noise_ridged(&noise->ridges, x * 1.3f, y * 1.3f, octave_count, settings->persistence);

	// Mountains only where the low frequency mask says so
	mask = noise_fbm(&noise->mountain_mask, x * 0.6f + 11.0f, y * 0.6f - 4.0f, 3, 0.5f) * 0.5f + 0.5f;
	mask_threshold = 1.0f - settings->mountain_coverage;
	mask = smoothstep_float(mask_threshold - 0.15f, mask_threshold + 0.15f, mask);

	return hills * 0.6f + mask * settings->mountain_amount * ridges;
}

// Thermal erosion, written as a "gather": every cell computes its own new
// height from what it gives to lower neighbors and receives from higher
// ones, reading only the previous iteration. Each cell writes only itself,
// so rows can be processed in parallel, and the result does not depend on
// the order cells are visited in.
static void apply_thermal_erosion(float *heights, int32_t size, float cell_size_meters, const terrain_settings_t *settings)
{
	float talus_drop = settings->talus_slope * cell_size_meters;
	float *next_heights = SDL_malloc((size_t)size * (size_t)size * sizeof(float));
	float *current = heights;
	float *next = next_heights;

	if (next_heights == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory in thermal erosion; skipping it");
		return;
	}

	for (int32_t iteration = 0; iteration < settings->thermal_iterations; iteration++)
	{
#pragma omp parallel for schedule(static)
		for (int32_t y = 0; y < size; y++)
		{
			for (int32_t x = 0; x < size; x++)
			{
				int32_t index = y * size + x;
				float height = current[index];
				float change = 0.0f;
				float neighbors[4];
				int neighbor_count = 0;

				if (x > 0)
				{
					neighbors[neighbor_count++] = current[index - 1];
				}
				if (x < size - 1)
				{
					neighbors[neighbor_count++] = current[index + 1];
				}
				if (y > 0)
				{
					neighbors[neighbor_count++] = current[index - size];
				}
				if (y < size - 1)
				{
					neighbors[neighbor_count++] = current[index + size];
				}

				for (int k = 0; k < neighbor_count; k++)
				{
					float drop = height - neighbors[k];

					// Moving 0.125 of the excess per neighbor keeps a cell
					// from ever overshooting below the neighbors it feeds
					if (drop > talus_drop)
					{
						change -= (drop - talus_drop) * 0.125f;
					}
					else if (-drop > talus_drop)
					{
						change += (-drop - talus_drop) * 0.125f;
					}
				}
				next[index] = height + change;
			}
		}

		// Swap buffers instead of copying
		{
			float *swap = current;

			current = next;
			next = swap;
		}
	}

	// After an odd number of iterations the result is in the scratch buffer
	if (current != heights)
	{
		SDL_memcpy(heights, current, (size_t)size * (size_t)size * sizeof(float));
	}
	SDL_free(next_heights);
}

// A steep face's relief, seen from one side: across it (meters along the
// face, horizontal) and up it (its height): gullies down it where the water
// runs, narrow and long, wandering a little; broad buttresses between them;
// ribs. Meters, about -1 .. 0.5 of depth
static float face_relief(const noise_context_t *noise, float across, float up, float depth)
{
	float wander = noise_fbm(noise, across / 40.0f, up / 40.0f, 2, 0.5f) * 5.0f;
	float gully = 1.0f - SDL_fabsf(noise_fbm(noise, (across + wander) / 16.0f + 31.0f, up / 110.0f, 3, 0.5f));
	float buttress = noise_fbm(noise, across / 30.0f + 57.0f, up / 90.0f - 13.0f, 2, 0.5f);
	float rib = noise_fbm(noise, across / 2.5f - 71.0f, up / 14.0f + 5.0f, 3, 0.5f);

	gully *= gully;
	gully *= gully;
	gully *= gully;
	return -gully * depth + buttress * depth * 0.45f + rib * depth * 0.08f;
}

// Gullies and buttresses carved into the steep slopes (a mountain's faces
// are not smooth cones): seen from the side it faces most (the relief across
// the face, along x or z, up its height), the two sides blended by how much
// the slope faces each; more the steeper
static void carve_steep_faces(float *heights, int32_t size, float cell_size_meters, const terrain_settings_t *settings,
	const generation_noise_t *noise)
{
	float *original;
	int32_t reach = SDL_max(1, (int32_t)SDL_lroundf(2.0f / cell_size_meters));

	if (settings->gully_depth_meters <= 0.0f)
	{
		return;
	}
	original = SDL_malloc((size_t)size * (size_t)size * sizeof(float));
	if (original == NULL)
	{
		return;
	}
	SDL_memcpy(original, heights, (size_t)size * (size_t)size * sizeof(float));
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			int32_t left = SDL_max(x - reach, 0);
			int32_t right = SDL_min(x + reach, size - 1);
			int32_t back = SDL_max(y - reach, 0);
			int32_t front = SDL_min(y + reach, size - 1);
			float slope_x = (original[y * size + right] - original[y * size + left]) / ((float)(right - left) * cell_size_meters);
			float slope_z = (original[front * size + x] - original[back * size + x]) / ((float)(front - back) * cell_size_meters);
			float slope = SDL_sqrtf(slope_x * slope_x + slope_z * slope_z);
			float steep = smoothstep_float(0.45f, 0.8f, slope);
			float facing_x;
			float facing_z;
			float up = original[y * size + x];
			float world_x = (float)x * cell_size_meters;
			float world_z = (float)y * cell_size_meters;

			if (steep <= 0.0f)
			{
				continue;
			}
			// (sharpened: where it faces both ways about alike, not a mush
			// of the two)
			facing_x = slope_x * slope_x;
			facing_x *= facing_x;
			facing_z = slope_z * slope_z;
			facing_z *= facing_z;
			heights[y * size + x] += steep * (facing_x * face_relief(&noise->gullies, world_z, up, settings->gully_depth_meters)
				+ facing_z * face_relief(&noise->gullies, world_x + 1000.0f, up, settings->gully_depth_meters))
				/ SDL_max(facing_x + facing_z, 1e-9f);
		}
	}
	SDL_free(original);
}

// Steepest height difference to a neighbor, in meters per meter
static float local_slope(const float *heights, int32_t size, int32_t x, int32_t y, float cell_size_meters)
{
	float center = heights[y * size + x];
	float steepest = 0.0f;

	if (x > 0)
	{
		steepest = SDL_max(steepest, SDL_fabsf(center - heights[y * size + x - 1]));
	}
	if (x < size - 1)
	{
		steepest = SDL_max(steepest, SDL_fabsf(center - heights[y * size + x + 1]));
	}
	if (y > 0)
	{
		steepest = SDL_max(steepest, SDL_fabsf(center - heights[(y - 1) * size + x]));
	}
	if (y < size - 1)
	{
		steepest = SDL_max(steepest, SDL_fabsf(center - heights[(y + 1) * size + x]));
	}
	return steepest / cell_size_meters;
}

// Steepest slope over reach cells in each direction, in meters per meter
static float wide_local_slope(const float *heights, int32_t size, int32_t x, int32_t y, float cell_size_meters, int32_t reach)
{
	int32_t left = SDL_max(x - reach, 0);
	int32_t right = SDL_min(x + reach, size - 1);
	int32_t back = SDL_max(y - reach, 0);
	int32_t front = SDL_min(y + reach, size - 1);
	float slope_x = right > left ? SDL_fabsf(heights[y * size + right] - heights[y * size + left]) / ((float)(right - left) * cell_size_meters) : 0.0f;
	float slope_y = front > back ? SDL_fabsf(heights[front * size + x] - heights[back * size + x]) / ((float)(front - back) * cell_size_meters) : 0.0f;

	return SDL_max(slope_x, slope_y);
}

// Box blur of radius reach along rows (source to destination), clamped at
// the map edges
static void box_blur_rows(const float *source, float *destination, int32_t size, int32_t reach)
{
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		const float *row = &source[(size_t)y * (size_t)size];
		float *out = &destination[(size_t)y * (size_t)size];

		for (int32_t x = 0; x < size; x++)
		{
			float sum = 0.0f;

			for (int32_t k = -reach; k <= reach; k++)
			{
				sum += row[clamp_int(x + k, 0, size - 1)];
			}
			out[x] = sum / (float)(2 * reach + 1);
		}
	}
}

// The same along columns
static void box_blur_columns(const float *source, float *destination, int32_t size, int32_t reach)
{
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			float sum = 0.0f;

			for (int32_t k = -reach; k <= reach; k++)
			{
				sum += source[(size_t)clamp_int(y + k, 0, size - 1) * (size_t)size + (size_t)x];
			}
			destination[(size_t)y * (size_t)size + (size_t)x] = sum / (float)(2 * reach + 1);
		}
	}
}

static soil_type_t classify_soil(const generation_noise_t *noise, const terrain_settings_t *settings,
	float height_fraction, float slope, float u, float v)
{
	// Noise shifts the thresholds a little so borders between soils are
	// ragged instead of following contour lines exactly
	float variation = noise_fbm(&noise->soil_variation, u * 40.0f, v * 40.0f, 3, 0.5f);

	if (slope > settings->rock_slope + variation * 0.15f || height_fraction > settings->rock_height_fraction + variation * 0.05f)
	{
		return SOIL_ROCK;
	}
	if (height_fraction < settings->sand_height_fraction + variation * 0.03f && slope < 0.35f)
	{
		return SOIL_SAND;
	}
	if (height_fraction < settings->clay_height_fraction + variation * 0.05f && slope < 0.25f)
	{
		return SOIL_CLAY;
	}
	return SOIL_EARTH;
}

void terrain_generate(terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings, uint32_t seed)
{
	int32_t size = terrain->size;
	size_t cell_count = (size_t)size * (size_t)size;
	float *heights = SDL_malloc(cell_count * sizeof(float));
	float *bedrock;
	float *scratch;
	// Soil depth and bedrock are smoothed over about two meters
	int32_t bedrock_reach = SDL_max(1, (int32_t)SDL_lroundf(2.0f / terrain->cell_size_meters));
	generation_noise_t noise;
	float minimum = 1e30f;
	float maximum = -1e30f;
	float inverse_range;
	int64_t pocket_count = 0;
	int64_t rock_count = 0;
	Uint64 start_ticks = SDL_GetTicks();
	float base_wavelength_meters = (float)size * terrain->cell_size_meters / SDL_max(settings->feature_count, 0.1f);
	// Each octave halves the wavelength; stop at the smallest wanted feature
	int octave_count = clamp_int((int)SDL_floorf(SDL_logf(base_wavelength_meters / SDL_max(settings->minimum_feature_meters, 1.0f)) / SDL_logf(2.0f)) + 1, 1, 12);

	if (heights == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory generating terrain");
		return;
	}
	initialize_generation_noise(&noise, seed);
	terrain->seed = seed;
	terrain->max_height_meters = settings->max_height_meters;

	// 1. Raw landscape
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			heights[y * size + x] = sample_landscape(&noise, settings, octave_count, (float)x / (float)size, (float)y / (float)size);
		}
	}

	// 2. Normalize to 0..1, flatten the lowlands, scale to meters
#pragma omp parallel for reduction(min:minimum) reduction(max:maximum) schedule(static)
	for (int64_t i = 0; i < (int64_t)cell_count; i++)
	{
		minimum = heights[i] < minimum ? heights[i] : minimum;
		maximum = heights[i] > maximum ? heights[i] : maximum;
	}
	inverse_range = maximum > minimum ? 1.0f / (maximum - minimum) : 0.0f;
#pragma omp parallel for schedule(static)
	for (int64_t i = 0; i < (int64_t)cell_count; i++)
	{
		float normalized = (heights[i] - minimum) * inverse_range;

		heights[i] = SDL_powf(normalized, 1.4f) * settings->max_height_meters;
	}

	// 3. Thermal erosion; then the steep faces' gullies and buttresses
	apply_thermal_erosion(heights, size, terrain->cell_size_meters, settings);
	carve_steep_faces(heights, size, terrain->cell_size_meters, settings, &noise);

	// 4. Store as integer units and classify soil; 5. bedrock
	bedrock = SDL_malloc(cell_count * sizeof(float));
	scratch = SDL_malloc(cell_count * sizeof(float));
	if (bedrock == NULL || scratch == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory generating the bedrock");
		SDL_free(bedrock);
		SDL_free(scratch);
		SDL_free(heights);
		return;
	}
#pragma omp parallel for schedule(static) reduction(+:pocket_count, rock_count)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			int32_t index = y * size + x;
			float height_meters = SDL_max(heights[index], 0.0f);
			float slope = local_slope(heights, size, x, y, terrain->cell_size_meters);
			float height_fraction = height_meters / settings->max_height_meters;
			float u = (float)x / (float)size;
			float v = (float)y / (float)size;
			soil_type_t soil = classify_soil(&noise, settings, height_fraction, slope, u, v);
			float soil_depth_meters = 0.0f;

			// Pockets of soil up the rock: where it is gentler over a meter or
			// two (a ledge, a buttress's top, a gully's floor), in patches,
			// fewer the higher up
			if (soil == SOIL_ROCK && settings->mountain_soil_pockets > 0.0f)
			{
				float gentle = 1.0f - smoothstep_float(settings->rock_slope * 0.7f, settings->rock_slope * 1.4f,
					wide_local_slope(heights, size, x, y, terrain->cell_size_meters, bedrock_reach));
				float patches = noise_fbm(&noise.soil_variation, u * 90.0f + 13.0f, v * 90.0f - 7.0f, 3, 0.5f) * 0.5f + 0.5f;
				float high = smoothstep_float(settings->rock_height_fraction, 1.0f, height_fraction);
				float pocket = gentle * (0.55f + 0.6f * patches) * settings->mountain_soil_pockets * 2.0f * (1.0f - 0.7f * high);

				if (pocket > 0.75f)
				{
					soil = SOIL_EARTH;
					pocket_count++;
				}
				rock_count++;
			}

			// Soil depth over the bedrock: deep where the ground is flat and
			// low (material collects there), thin on steep slopes and high
			// up, with noise so the bedrock does not mirror the surface. The
			// slope is measured over a few meters: from cell to cell it jumps
			// around on rough ground, and so would the depth, leaving a
			// sawtooth bedrock that erosion later lays bare. Rock cells are
			// bare bedrock.
			if (soil != SOIL_ROCK)
			{
				float wide_slope = wide_local_slope(heights, size, x, y, terrain->cell_size_meters, bedrock_reach);
				float flatness = 1.0f - smoothstep_float(0.0f, settings->rock_slope, wide_slope);
				float lowness = 1.0f - 0.6f * clamp_float(height_fraction / SDL_max(settings->rock_height_fraction, 0.01f), 0.0f, 1.0f);
				float variation = 1.0f + 0.35f * noise_fbm(&noise.soil_variation, u * 23.0f + 7.0f, v * 23.0f - 3.0f, 3, 0.5f);

				soil_depth_meters = lerp_float(soil_settings->soil_depth_minimum_meters, soil_settings->soil_depth_maximum_meters,
					flatness * lowness) * variation;
			}
			terrain->heights[index] = (int32_t)SDL_lroundf(height_meters * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
			terrain->soil_types[index] = (uint8_t)soil;
			terrain->silt_thickness[index] = 0;
			bedrock[index] = height_meters - soil_depth_meters;
		}
	}

	// The bedrock surface itself is smoothed too (two box blurs over a few
	// meters, which is close to a gaussian), so it has no steps where the
	// soil depth changes; it never comes out above the ground
	for (int pass = 0; pass < 2; pass++)
	{
		box_blur_rows(bedrock, scratch, size, bedrock_reach);
		box_blur_columns(scratch, bedrock, size, bedrock_reach);
	}
#pragma omp parallel for schedule(static)
	for (int64_t i = 0; i < (int64_t)cell_count; i++)
	{
		int32_t bedrock_units = (int32_t)SDL_lroundf(SDL_max(bedrock[i], 0.0f) * (float)TERRAIN_HEIGHT_UNITS_PER_METER);

		terrain->bedrock_heights[i] = terrain->soil_types[i] == SOIL_ROCK ? terrain->heights[i] : SDL_min(bedrock_units, terrain->heights[i]);
	}
	SDL_free(bedrock);
	SDL_free(scratch);

	SDL_free(heights);
	terrain_mark_all_changed(terrain);
	SDL_Log("Generated %dx%d terrain, seed %u, %d octaves, in %llu ms; %.0f%% of its rock left soil in pockets", size, size, seed, octave_count,
		(unsigned long long)(SDL_GetTicks() - start_ticks), rock_count > 0 ? 100.0 * (double)pocket_count / (double)rock_count : 0.0);
}
