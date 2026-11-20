// Procedural terrain generation. See terrain_gen.h.

#include "terrain_gen.h"
#include "noise.h"
#include "math3d.h"

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


// --- Coarse grids ------------------------------------------------------------------

// The heights averaged down to a grid of side small
static void downsample(const float *heights, int32_t size, float *grid, int32_t small)
{
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < small; y++)
	{
		int32_t first_y = (int32_t)((int64_t)y * size / small);
		int32_t end_y = SDL_max((int32_t)((int64_t)(y + 1) * size / small), first_y + 1);

		for (int32_t x = 0; x < small; x++)
		{
			int32_t first_x = (int32_t)((int64_t)x * size / small);
			int32_t end_x = SDL_max((int32_t)((int64_t)(x + 1) * size / small), first_x + 1);
			double sum = 0.0;

			for (int32_t j = first_y; j < end_y; j++)
			{
				for (int32_t i = first_x; i < end_x; i++)
				{
					sum += heights[(size_t)j * (size_t)size + (size_t)i];
				}
			}
			grid[(size_t)y * (size_t)small + (size_t)x] = (float)(sum / (double)((end_x - first_x) * (end_y - first_y)));
		}
	}
}

// A coarse grid at (u, v) (0..1 over the map), between its texels
static float grid_sample(const float *grid, int32_t side, float u, float v)
{
	float x = clamp_float(u * (float)side - 0.5f, 0.0f, (float)(side - 1));
	float y = clamp_float(v * (float)side - 0.5f, 0.0f, (float)(side - 1));
	int32_t x0 = (int32_t)x;
	int32_t y0 = (int32_t)y;
	int32_t x1 = SDL_min(x0 + 1, side - 1);
	int32_t y1 = SDL_min(y0 + 1, side - 1);
	float fx = x - (float)x0;
	float fy = y - (float)y0;
	float top = lerp_float(grid[(size_t)y0 * (size_t)side + (size_t)x0], grid[(size_t)y0 * (size_t)side + (size_t)x1], fx);
	float bottom = lerp_float(grid[(size_t)y1 * (size_t)side + (size_t)x0], grid[(size_t)y1 * (size_t)side + (size_t)x1], fx);

	return lerp_float(top, bottom, fy);
}

// --- The sketch over the seed's land ------------------------------------------------

#define SKETCH_GRID 512

// A running box blur (sum over 2 reach + 1, the edges held), a row at a time
static void running_blur_rows(const float *source, float *target, int32_t side, int32_t reach)
{
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < side; y++)
	{
		const float *row = &source[(size_t)y * (size_t)side];
		float *out = &target[(size_t)y * (size_t)side];
		float sum = 0.0f;
		float scale = 1.0f / (float)(2 * reach + 1);

		for (int32_t k = -reach; k <= reach; k++)
		{
			sum += row[clamp_int(k, 0, side - 1)];
		}
		for (int32_t x = 0; x < side; x++)
		{
			out[x] = sum * scale;
			sum += row[clamp_int(x + reach + 1, 0, side - 1)] - row[clamp_int(x - reach, 0, side - 1)];
		}
	}
}

// Rows and columns swapped (so the rows' blur does the columns)
static void transpose(const float *source, float *target, int32_t side)
{
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < side; y++)
	{
		for (int32_t x = 0; x < side; x++)
		{
			target[(size_t)x * (size_t)side + (size_t)y] = source[(size_t)y * (size_t)side + (size_t)x];
		}
	}
}

// A sketch painted: the land the regions make (each its height, rolling
// land more or less over it, crests out of the mountains), blended across
// their borders; the seed giving only the shapes within (its noises)
static void apply_sketch(float *heights, int32_t size, const terrain_settings_t *settings, const generation_noise_t *noise, int octave_count,
	terrain_sketch_t *sketch)
{
	size_t grid_texels = (size_t)SKETCH_GRID * SKETCH_GRID;
	float *scratch = SDL_malloc(grid_texels * sizeof(float));
	// (what the regions make, blended: the height, the detail, the crests)
	float *blended[3];
	float *other = SDL_malloc(grid_texels * sizeof(float));
	// (how much of each texel's own counts: none of a lake's, which takes
	// the land round it, its lake sunk into that after: sink_lakes)
	float *weights = SDL_malloc(grid_texels * sizeof(float));
	const terrain_sketch_land_t *land = &sketch->land;
	int32_t blend_reach = SDL_max(1, (int32_t)SDL_lroundf((float)SKETCH_GRID * clamp_float(land->blend_share, 0.0f, 0.2f)));
	float most = settings->max_height_meters;
	const uint8_t *regions = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	const uint8_t *levels = terrain_sketch_plane(sketch, TERRAIN_SKETCH_LEVEL);
	bool enough = scratch != NULL && other != NULL && weights != NULL;

	for (int k = 0; k < 3; k++)
	{
		blended[k] = SDL_malloc(grid_texels * sizeof(float));
		enough = enough && blended[k] != NULL;
	}
	if (!enough)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory: the sketch is left out");
		goto done;
	}
	// The regions' land on a grid, blurred three times (near enough a bell)
	for (int32_t y = 0; y < SKETCH_GRID; y++)
	{
		for (int32_t x = 0; x < SKETCH_GRID; x++)
		{
			int32_t texel_x = x * TERRAIN_SKETCH_SIZE / SKETCH_GRID;
			int32_t texel_y = y * TERRAIN_SKETCH_SIZE / SKETCH_GRID;
			size_t texel = (size_t)texel_y * TERRAIN_SKETCH_SIZE + (size_t)texel_x;
			uint8_t region = regions[texel];
			int kind = region < TERRAIN_SKETCH_REGION_COUNT ? region : 0;
			size_t index = (size_t)y * SKETCH_GRID + (size_t)x;
			float weight = kind == TERRAIN_SKETCH_LAKE ? 0.0f : 1.0f;
			// (a low or high one: as much more or less of its kind, its
			// height over the unpainted land's, a plain's its own)
			float scale = kind == TERRAIN_SKETCH_NONE ? 1.0f : SDL_max(land->level_scale[SDL_min(levels[texel], TERRAIN_SKETCH_HIGH)], 0.0f);
			float base = kind == TERRAIN_SKETCH_PLAIN ? 0.0f : land->height[TERRAIN_SKETCH_NONE];

			weights[index] = weight;
			blended[0][index] = SDL_max(base + (land->height[kind] - base) * scale, 0.0f) * weight;
			blended[1][index] = land->roll[kind] * scale * weight;
			blended[2][index] = land->crests[kind] * scale * weight;
		}
	}
	for (int k = 0; k < 4; k++)
	{
		float *grid = k < 3 ? blended[k] : weights;

		for (int pass = 0; pass < 3; pass++)
		{
			running_blur_rows(grid, other, SKETCH_GRID, blend_reach);
			transpose(other, scratch, SKETCH_GRID);
			running_blur_rows(scratch, other, SKETCH_GRID, blend_reach);
			transpose(other, grid, SKETCH_GRID);
		}
	}
	// (each the weighted mean of what is round it: a lake's own land none,
	// the land round it carried over it, as high as its shores; far into
	// one, the unpainted land's)
	for (size_t i = 0; i < grid_texels; i++)
	{
		bool known = weights[i] > 1e-3f;

		blended[0][i] = known ? blended[0][i] / weights[i] : land->height[TERRAIN_SKETCH_NONE];
		blended[1][i] = known ? blended[1][i] / weights[i] : land->roll[TERRAIN_SKETCH_NONE];
		blended[2][i] = known ? blended[2][i] / weights[i] : land->crests[TERRAIN_SKETCH_NONE];
	}
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			size_t index = (size_t)y * (size_t)size + (size_t)x;
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			float height = grid_sample(blended[0], SKETCH_GRID, u, v);
			float rolling = grid_sample(blended[1], SKETCH_GRID, u, v);
			float crests = grid_sample(blended[2], SKETCH_GRID, u, v);

			float detail = rolling > 0.001f ? noise_fbm(&noise->hills, u * settings->feature_count * 3.0f + 7.0f, v * settings->feature_count * 3.0f - 3.0f,
				octave_count, settings->persistence) : 0.0f;

			heights[index] = (height + detail * rolling * 0.08f) * most;
			if (crests > 0.001f)
			{
				// (as the seed's mountains: their ridged noise over bent
				// coordinates, meandering crests; a little sharpened)
				float x_bent = u * settings->feature_count * clamp_float(land->crest_scale, 0.1f, 20.0f);
				float y_bent = v * settings->feature_count * clamp_float(land->crest_scale, 0.1f, 20.0f);
				float bend_x = noise_fbm(&noise->warp, x_bent + 5.2f, y_bent + 1.3f, 4, 0.5f);
				float bend_y = noise_fbm(&noise->warp, x_bent - 3.1f, y_bent + 7.7f, 4, 0.5f);
				float ridge = noise_ridged(&noise->ridges, x_bent + settings->warp_amount * bend_x, y_bent + settings->warp_amount * bend_y,
					octave_count, settings->persistence);

				heights[index] += crests * SDL_powf(clamp_float(ridge, 0.0f, 1.0f), 1.5f) * most;
			}
			heights[index] = SDL_max(heights[index], 0.0f);
		}
	}
done:
	SDL_free(scratch);
	SDL_free(other);
	SDL_free(weights);
	for (int k = 0; k < 3; k++)
	{
		SDL_free(blended[k]);
	}
}

// The sketch's lakes. What is painted is a hint, not their shape: the
// painted water blurred (its corners rounded, LAKE_HINT_SHARE of the map's
// side) after its edge is bent by the warp noise; each lake's surface the level
// of the land round it (LAKE_SURFACE_SHARE of its shore lower), the land
// under it sunk into a bowl from there, its low shores raised a little
// over the surface so it holds its water. The world fills each with water
// to its surface, as far as the land under it is lower (world.c): the
// shore where the land meets the water, wherever that is
#define LAKE_HINT_SHARE 0.006f          // the painted water blurred over this share of the map's side
#define LAKE_WIDE_REACH 3               // the bowl's depth blurred this many times wider
#define LAKE_WARP_SHARE 0.02f           // the shore bent by up to this share of the map's side (the broad bends; the small ones a quarter)
#define LAKE_SURFACE_SHARE 0.25f        // the share of a lake's shore lower than its surface (raised over it)
#define LAKE_RIM_METERS 0.25f           // a low shore raised this much over the surface
#define LAKE_EDGE_METERS 0.2f           // the water this deep at the shore, deepening from there

static int compare_floats(const void *first, const void *second)
{
	float a = *(const float *)first;
	float b = *(const float *)second;

	return a < b ? -1 : (a > b ? 1 : 0);
}

// The sketch's grid blurred three times (near enough a bell)
static void blur_sketch_grid(float *grid, float *scratch, float *other, int32_t reach)
{
	for (int pass = 0; pass < 3; pass++)
	{
		running_blur_rows(grid, other, TERRAIN_SKETCH_SIZE, reach);
		transpose(other, scratch, TERRAIN_SKETCH_SIZE);
		running_blur_rows(scratch, other, TERRAIN_SKETCH_SIZE, reach);
		transpose(other, grid, TERRAIN_SKETCH_SIZE);
	}
}

// The land's height at a texel of the sketch (its cell)
static float height_at_texel(const float *heights, int32_t size, int32_t x, int32_t y)
{
	int32_t cell_x = clamp_int((int)(((float)x + 0.5f) / (float)TERRAIN_SKETCH_SIZE * (float)size), 0, size - 1);
	int32_t cell_y = clamp_int((int)(((float)y + 0.5f) / (float)TERRAIN_SKETCH_SIZE * (float)size), 0, size - 1);

	return heights[(size_t)cell_y * (size_t)size + (size_t)cell_x];
}

// The lakes found in the hint (over a half, side by side), each texel's
// lake spread out over the hint's fringe (over LAKE_FRINGE: the nearest
// lake's), each lake's surface from its shore's land
#define LAKE_FRINGE 0.05f

static void find_lakes(const float *heights, int32_t size, terrain_sketch_t *sketch)
{
	size_t texels = (size_t)TERRAIN_SKETCH_SIZE * TERRAIN_SKETCH_SIZE;
	const float *hint = sketch->lake_hint;
	const uint8_t *regions = terrain_sketch_plane(sketch, TERRAIN_SKETCH_REGION);
	const uint8_t *levels = terrain_sketch_plane(sketch, TERRAIN_SKETCH_LEVEL);
	uint32_t *queue = SDL_malloc(texels * sizeof(uint32_t));
	float *shore = SDL_malloc(texels * sizeof(float));
	static const int32_t steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
	size_t head;
	size_t tail = 0;

	SDL_memset(sketch->lake_labels, 0, texels * sizeof(uint16_t));
	sketch->lake_count = 0;
	if (queue == NULL || shore == NULL)
	{
		SDL_free(queue);
		SDL_free(shore);
		return;
	}
	// Each lake: its texels joined, the land just past its edge its shore
	for (size_t start = 0; start < texels && sketch->lake_count < TERRAIN_SKETCH_LAKES_MAX; start++)
	{
		uint16_t label;
		size_t first = tail;
		size_t shore_count = 0;
		double inside_sum = 0.0;
		size_t inside_count = 0;
		float surface;
		int deepest_level = -1;

		if (hint[start] < 0.5f || sketch->lake_labels[start] != 0)
		{
			continue;
		}
		label = (uint16_t)(sketch->lake_count + 1);
		sketch->lake_labels[start] = label;
		queue[tail++] = (uint32_t)start;
		for (head = first; head < tail; head++)
		{
			int32_t x = (int32_t)(queue[head] % TERRAIN_SKETCH_SIZE);
			int32_t y = (int32_t)(queue[head] / TERRAIN_SKETCH_SIZE);

			inside_sum += height_at_texel(heights, size, x, y);
			inside_count++;
			if (regions[queue[head]] == TERRAIN_SKETCH_LAKE)
			{
				deepest_level = SDL_max(deepest_level, (int)levels[queue[head]]);
			}
			for (int k = 0; k < 4; k++)
			{
				int32_t nx = x + steps[k][0];
				int32_t ny = y + steps[k][1];
				size_t neighbor;

				if (nx < 0 || ny < 0 || nx >= TERRAIN_SKETCH_SIZE || ny >= TERRAIN_SKETCH_SIZE)
				{
					continue;
				}
				neighbor = (size_t)ny * TERRAIN_SKETCH_SIZE + (size_t)nx;
				if (hint[neighbor] >= 0.5f)
				{
					if (sketch->lake_labels[neighbor] == 0)
					{
						sketch->lake_labels[neighbor] = label;
						queue[tail++] = (uint32_t)neighbor;
					}
				}
				else
				{
					shore[shore_count++] = height_at_texel(heights, size, nx, ny);
				}
			}
		}
		// (its surface: as high as most of its shore, a quarter of it
		// lower and raised; none, out to the map's edge everywhere: its
		// own land's level)
		if (shore_count > 0)
		{
			SDL_qsort(shore, shore_count, sizeof(float), compare_floats);
			surface = shore[(size_t)((float)(shore_count - 1) * LAKE_SURFACE_SHARE)];
		}
		else
		{
			surface = (float)(inside_sum / (double)SDL_max(inside_count, (size_t)1));
		}
		sketch->lake_surfaces[sketch->lake_count] = SDL_max(surface, 0.5f);
		// (as deep as the deepest painted in it says)
		sketch->lake_depth_scales[sketch->lake_count] =
			SDL_max(sketch->land.level_scale[deepest_level < 0 ? TERRAIN_SKETCH_MEDIUM : SDL_min(deepest_level, TERRAIN_SKETCH_HIGH)], 0.05f);
		sketch->lake_count++;
	}
	// Spread over the fringe, the nearest lake's (breadth first from all of
	// them at once)
	for (head = 0; head < tail; head++)
	{
		int32_t x = (int32_t)(queue[head] % TERRAIN_SKETCH_SIZE);
		int32_t y = (int32_t)(queue[head] / TERRAIN_SKETCH_SIZE);
		uint16_t label = sketch->lake_labels[queue[head]];

		for (int k = 0; k < 4; k++)
		{
			int32_t nx = x + steps[k][0];
			int32_t ny = y + steps[k][1];
			size_t neighbor;

			if (nx < 0 || ny < 0 || nx >= TERRAIN_SKETCH_SIZE || ny >= TERRAIN_SKETCH_SIZE)
			{
				continue;
			}
			neighbor = (size_t)ny * TERRAIN_SKETCH_SIZE + (size_t)nx;
			if (sketch->lake_labels[neighbor] == 0 && hint[neighbor] >= LAKE_FRINGE)
			{
				sketch->lake_labels[neighbor] = label;
				queue[tail++] = (uint32_t)neighbor;
			}
		}
	}
	SDL_free(queue);
	SDL_free(shore);
}

static float sketch_grid_sample(const float *grid, float u, float v)
{
	return grid_sample(grid, TERRAIN_SKETCH_SIZE, u, v);
}

// The lakes made: the hint worked out, the lakes found, the land shaped
// round and under them (after the erosion, which would fill them)
static void sink_lakes(float *heights, int32_t size, const terrain_settings_t *settings, const generation_noise_t *noise,
	terrain_sketch_t *sketch)
{
	size_t texels = (size_t)TERRAIN_SKETCH_SIZE * TERRAIN_SKETCH_SIZE;
	const uint8_t *water = terrain_sketch_plane(sketch, TERRAIN_SKETCH_WATER);
	float *wide = SDL_malloc(texels * sizeof(float));
	float *scratch = SDL_malloc(texels * sizeof(float));
	float *other = SDL_malloc(texels * sizeof(float));
	int32_t reach = SDL_max(2, (int32_t)SDL_lroundf((float)TERRAIN_SKETCH_SIZE * LAKE_HINT_SHARE));
	float depth = SDL_max(sketch->land.lake_depth_meters, 0.5f);
	float warp_frequency = SDL_max(settings->feature_count, 1.0f) * 4.0f;

	sketch->lake_count = 0;
	if (wide == NULL || scratch == NULL || other == NULL || sketch->lake_hint == NULL)
	{
		goto done;
	}
	// What is painted, its edge bent (broad bays and points, smaller
	// ones on them): no lake as round as the brush
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < TERRAIN_SKETCH_SIZE; y++)
	{
		for (int32_t x = 0; x < TERRAIN_SKETCH_SIZE; x++)
		{
			float u = ((float)x + 0.5f) / (float)TERRAIN_SKETCH_SIZE;
			float v = ((float)y + 0.5f) / (float)TERRAIN_SKETCH_SIZE;
			float bent_u = u + LAKE_WARP_SHARE * (noise_fbm(&noise->warp, u * warp_frequency + 11.3f, v * warp_frequency - 5.7f, 2, 0.5f)
				+ 0.25f * noise_fbm(&noise->warp, u * warp_frequency * 4.0f + 3.1f, v * warp_frequency * 4.0f + 9.4f, 2, 0.5f));
			float bent_v = v + LAKE_WARP_SHARE * (noise_fbm(&noise->warp, u * warp_frequency - 2.9f, v * warp_frequency + 8.1f, 2, 0.5f)
				+ 0.25f * noise_fbm(&noise->warp, u * warp_frequency * 4.0f - 7.6f, v * warp_frequency * 4.0f - 1.8f, 2, 0.5f));
			int32_t texel_x = clamp_int((int)(bent_u * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);
			int32_t texel_y = clamp_int((int)(bent_v * (float)TERRAIN_SKETCH_SIZE), 0, TERRAIN_SKETCH_SIZE - 1);

			sketch->lake_hint[(size_t)y * TERRAIN_SKETCH_SIZE + (size_t)x] =
				water[(size_t)texel_y * TERRAIN_SKETCH_SIZE + (size_t)texel_x] >= TERRAIN_SKETCH_LAKE_THRESHOLD ? 1.0f : 0.0f;
		}
	}
	blur_sketch_grid(sketch->lake_hint, scratch, other, reach);
	SDL_memcpy(wide, sketch->lake_hint, texels * sizeof(float));
	blur_sketch_grid(wide, scratch, other, reach * LAKE_WIDE_REACH);
	find_lakes(heights, size, sketch);
	if (sketch->lake_count == 0)
	{
		goto done;
	}
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			size_t index = (size_t)y * (size_t)size + (size_t)x;
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int lake = terrain_sketch_lake_at(sketch, u, v);
			float edge;
			float middle;
			float surface;
			float bed;
			float height = heights[index];

			if (lake < 0)
			{
				continue;
			}
			edge = sketch_grid_sample(sketch->lake_hint, u, v);
			middle = sketch_grid_sample(wide, u, v);
			surface = sketch->lake_surfaces[lake];
			// Its low shores raised over the surface (none lower than it
			// round the water), then the land under it sunk: just under
			// the surface at the shore, deepest in the middle
			height = lerp_float(height, SDL_max(height, surface + LAKE_RIM_METERS), smoothstep_float(0.15f, 0.42f, edge));
			bed = surface - LAKE_EDGE_METERS - SDL_max(depth * sketch->lake_depth_scales[lake] - LAKE_EDGE_METERS, 0.0f) * smoothstep_float(0.5f, 0.95f, middle);
			height = lerp_float(height, SDL_min(height, bed), smoothstep_float(0.42f, 0.6f, edge));
			heights[index] = height;
		}
	}
done:
	SDL_free(wide);
	SDL_free(scratch);
	SDL_free(other);
}

// --- Erosion by the rain's droplets ---------------------------------------------------

#define DROPLET_GRID_MAXIMUM 1024       // the erosion's grid at most (finer maps: worked out on it, the change laid over theirs)
#define DROPLET_TILE 256                // droplets started tile by tile, a tile's neighbors not at the same time
#define DROPLET_STEPS 80
#define DROPLET_RADIUS 2
#define DROPLET_INERTIA 0.05f
#define DROPLET_CAPACITY 3.0f
#define DROPLET_MINIMUM_SLOPE 0.01f
#define DROPLET_ERODE 0.15f
#define DROPLET_DEPOSIT 0.1f
#define DROPLET_EVAPORATE 0.02f
#define DROPLET_GRAVITY 4.0f

typedef struct
{
	int32_t offsets[(2 * DROPLET_RADIUS + 1) * (2 * DROPLET_RADIUS + 1)][2];
	float weights[(2 * DROPLET_RADIUS + 1) * (2 * DROPLET_RADIUS + 1)];
	int count;
} droplet_brush_t;

static uint32_t droplet_random(uint32_t *state)
{
	// (xorshift: each tile its own, so the result does not depend on the threads)
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static float droplet_unit(uint32_t *state)
{
	return (float)(droplet_random(state) >> 8) * (1.0f / 16777216.0f);
}

// The height and its slope at a point between the texels
static float droplet_height(const float *grid, int32_t side, float x, float y, float *slope_x, float *slope_y)
{
	int32_t cell_x = (int32_t)x;
	int32_t cell_y = (int32_t)y;
	float fx = x - (float)cell_x;
	float fy = y - (float)cell_y;
	size_t index = (size_t)cell_y * (size_t)side + (size_t)cell_x;
	float a = grid[index];
	float b = grid[index + 1];
	float c = grid[index + (size_t)side];
	float d = grid[index + (size_t)side + 1];

	*slope_x = (b - a) * (1.0f - fy) + (d - c) * fy;
	*slope_y = (c - a) * (1.0f - fx) + (d - b) * fx;
	return a * (1.0f - fx) * (1.0f - fy) + b * fx * (1.0f - fy) + c * (1.0f - fx) * fy + d * fx * fy;
}

static void run_droplet(float *grid, int32_t side, const droplet_brush_t *brush, float x, float y)
{
	float direction_x = 0.0f;
	float direction_y = 0.0f;
	float speed = 1.0f;
	float water = 1.0f;
	float sediment = 0.0f;

	// (the heights round it are read a texel on: not on the last ones)
	if (x < 1.0f || y < 1.0f || x >= (float)(side - 2) || y >= (float)(side - 2))
	{
		return;
	}
	for (int step = 0; step < DROPLET_STEPS; step++)
	{
		int32_t cell_x = (int32_t)x;
		int32_t cell_y = (int32_t)y;
		float fx = x - (float)cell_x;
		float fy = y - (float)cell_y;
		size_t index = (size_t)cell_y * (size_t)side + (size_t)cell_x;
		float slope_x;
		float slope_y;
		float height = droplet_height(grid, side, x, y, &slope_x, &slope_y);
		float length;
		float new_height;
		float drop;
		float capacity;
		float unused_x;
		float unused_y;

		direction_x = direction_x * DROPLET_INERTIA - slope_x * (1.0f - DROPLET_INERTIA);
		direction_y = direction_y * DROPLET_INERTIA - slope_y * (1.0f - DROPLET_INERTIA);
		length = SDL_sqrtf(direction_x * direction_x + direction_y * direction_y);
		if (!(length > 1e-9f))
		{
			break;
		}
		direction_x /= length;
		direction_y /= length;
		x += direction_x;
		y += direction_y;
		if (x < 1.0f || y < 1.0f || x >= (float)(side - 2) || y >= (float)(side - 2))
		{
			break;
		}
		new_height = droplet_height(grid, side, x, y, &unused_x, &unused_y);
		drop = new_height - height;
		capacity = SDL_max(-drop, DROPLET_MINIMUM_SLOPE) * speed * water * DROPLET_CAPACITY;
		if (sediment > capacity || drop > 0.0f)
		{
			// Drops what it cannot carry (uphill: fills the pit behind it)
			float deposit = drop > 0.0f ? SDL_min(drop, sediment) : (sediment - capacity) * DROPLET_DEPOSIT;

			sediment -= deposit;
			grid[index] += deposit * (1.0f - fx) * (1.0f - fy);
			grid[index + 1] += deposit * fx * (1.0f - fy);
			grid[index + (size_t)side] += deposit * (1.0f - fx) * fy;
			grid[index + (size_t)side + 1] += deposit * fx * fy;
		}
		else
		{
			// Picks up, from round where it was (no deeper than the drop)
			float erode = SDL_min((capacity - sediment) * DROPLET_ERODE, -drop);

			for (int k = 0; k < brush->count; k++)
			{
				int32_t bx = cell_x + brush->offsets[k][0];
				int32_t by = cell_y + brush->offsets[k][1];
				float *target;
				float taken;

				if (bx < 0 || by < 0 || bx >= side || by >= side)
				{
					continue;
				}
				target = &grid[(size_t)by * (size_t)side + (size_t)bx];
				taken = SDL_min(*target, erode * brush->weights[k]);
				*target -= taken;
				sediment += taken;
			}
		}
		speed = SDL_sqrtf(SDL_max(speed * speed - drop * DROPLET_GRAVITY, 0.0f));
		water *= 1.0f - DROPLET_EVAPORATE;
	}
}

// Rain over the land: droplets run downhill, picking up where they speed
// up and dropping it where they slow, which cuts gullies and valleys and
// fills the hollows. Worked out on a grid of at most DROPLET_GRID_MAXIMUM
// a side (the same for a preview a quarter as big), its change laid over
// the map's heights; droplets_per_texel over it
static void apply_droplet_erosion(float *heights, int32_t size, const terrain_settings_t *settings, uint32_t seed)
{
	int32_t side = SDL_min(size, DROPLET_GRID_MAXIMUM);
	size_t texels = (size_t)side * (size_t)side;
	float *grid = SDL_malloc(texels * sizeof(float));
	float *original = SDL_malloc(texels * sizeof(float));
	float most = SDL_max(settings->max_height_meters, 1.0f);
	int32_t tiles = (side + DROPLET_TILE - 1) / DROPLET_TILE;
	droplet_brush_t brush;
	float weight_sum = 0.0f;
	Uint64 start_ticks = SDL_GetTicks();

	if (grid == NULL || original == NULL)
	{
		SDL_free(grid);
		SDL_free(original);
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory: no erosion by the rain");
		return;
	}
	brush.count = 0;
	for (int32_t dy = -DROPLET_RADIUS; dy <= DROPLET_RADIUS; dy++)
	{
		for (int32_t dx = -DROPLET_RADIUS; dx <= DROPLET_RADIUS; dx++)
		{
			float distance = SDL_sqrtf((float)(dx * dx + dy * dy));

			if (distance <= (float)DROPLET_RADIUS)
			{
				brush.offsets[brush.count][0] = dx;
				brush.offsets[brush.count][1] = dy;
				brush.weights[brush.count] = 1.0f - distance / (float)DROPLET_RADIUS;
				weight_sum += brush.weights[brush.count];
				brush.count++;
			}
		}
	}
	for (int k = 0; k < brush.count; k++)
	{
		brush.weights[k] /= weight_sum;
	}
	// (in shares of the highest ground: the droplets' numbers are made for that)
	downsample(heights, size, grid, side);
	for (size_t i = 0; i < texels; i++)
	{
		grid[i] /= most;
		original[i] = grid[i];
	}
	// Tile by tile, a quarter of them at a time: those of one color are two
	// tiles apart, farther than a droplet runs, so they never meet
	for (int color = 0; color < 4; color++)
	{
#pragma omp parallel for schedule(dynamic, 1)
		for (int32_t tile = 0; tile < tiles * tiles; tile++)
		{
			int32_t tile_x = tile % tiles;
			int32_t tile_y = tile / tiles;
			int32_t first_x = tile_x * DROPLET_TILE;
			int32_t first_y = tile_y * DROPLET_TILE;
			int32_t width = SDL_min(DROPLET_TILE, side - first_x);
			int32_t depth = SDL_min(DROPLET_TILE, side - first_y);
			uint32_t state = (seed * 2654435761u) ^ ((uint32_t)tile * 40503u + 0x9E3779B9u);
			int64_t count = (int64_t)((float)(width * depth) * settings->erosion_droplets);

			if ((tile_x & 1) + 2 * (tile_y & 1) != color)
			{
				continue;
			}
			state = state != 0 ? state : 1u;
			for (int64_t d = 0; d < count; d++)
			{
				float x = (float)first_x + droplet_unit(&state) * (float)width;
				float y = (float)first_y + droplet_unit(&state) * (float)depth;

				run_droplet(grid, side, &brush, x, y);
			}
		}
	}
	// The change, over the map's own heights
	for (size_t i = 0; i < texels; i++)
	{
		grid[i] = (grid[i] - original[i]) * most;
	}
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			size_t index = (size_t)y * (size_t)size + (size_t)x;

			heights[index] = SDL_max(heights[index] + grid_sample(grid, side, ((float)x + 0.5f) / (float)size, ((float)y + 0.5f) / (float)size),
				0.0f);
		}
	}
	SDL_free(grid);
	SDL_free(original);
	SDL_Log("Erosion by the rain: %.2f droplets a texel on %d x %d, %llu ms", (double)settings->erosion_droplets, side, side,
		(unsigned long long)(SDL_GetTicks() - start_ticks));
}

// --- The heights ------------------------------------------------------------------------

static int octaves_for(int32_t size, float cell_size_meters, const terrain_settings_t *settings)
{
	float base_wavelength_meters = (float)size * cell_size_meters / SDL_max(settings->feature_count, 0.1f);

	// Each octave halves the wavelength; stop at the smallest wanted feature
	return clamp_int((int)SDL_floorf(SDL_logf(base_wavelength_meters / SDL_max(settings->minimum_feature_meters, 1.0f)) / SDL_logf(2.0f)) + 1, 1,
		12);
}

bool terrain_generate_heights(float *heights, int32_t size, float cell_size_meters, const terrain_settings_t *settings, uint32_t seed,
	terrain_sketch_t *sketch, unsigned int steps)
{
	size_t cell_count = (size_t)size * (size_t)size;
	generation_noise_t noise;
	float minimum = 1e30f;
	float maximum = -1e30f;
	float inverse_range;
	int octave_count = octaves_for(size, cell_size_meters, settings);

	initialize_generation_noise(&noise, seed);
	// (painted: the land is the sketch's alone, the seed's not needed)
	if ((steps & TERRAIN_HEIGHTS_LAND) != 0 && sketch != NULL && terrain_sketch_painted(sketch))
	{
		SDL_memset(heights, 0, cell_count * sizeof(float));
	}
	else if ((steps & TERRAIN_HEIGHTS_LAND) != 0)
	{
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
	}

	// 3. The sketch where it is painted
	if (sketch != NULL && terrain_sketch_painted(sketch))
	{
		apply_sketch(heights, size, settings, &noise, octave_count, sketch);
	}
	else if (sketch != NULL)
	{
		sketch->lake_count = 0;
	}

	// 4. The rain's erosion: on a new map made from the sketch only, as
	// much as asked (a seed's map comes out as it always has)
	if ((steps & TERRAIN_HEIGHTS_EROSION) != 0 && sketch != NULL && settings->erosion_droplets > 0.0f)
	{
		apply_droplet_erosion(heights, size, settings, seed);
	}

	// 5. Thermal erosion; then the steep faces' gullies and buttresses
	if ((steps & TERRAIN_HEIGHTS_FINISH) != 0)
	{
		apply_thermal_erosion(heights, size, cell_size_meters, settings);
		carve_steep_faces(heights, size, cell_size_meters, settings, &noise);
	}

	// 6. The sketch's lakes, last: their surfaces under their shores as the
	// land ends up (the thermal erosion after them wore the shores down
	// below the water, which ran out down the slopes)
	if (sketch != NULL && terrain_sketch_painted(sketch))
	{
		sink_lakes(heights, size, settings, &noise, sketch);
	}
	return true;
}

void terrain_generate(terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings, uint32_t seed,
	terrain_sketch_t *sketch)
{
	int32_t size = terrain->size;
	size_t cell_count = (size_t)size * (size_t)size;
	float *heights = SDL_malloc(cell_count * sizeof(float));
	float *bedrock;
	float *scratch;
	// Soil depth and bedrock are smoothed over about two meters
	int32_t bedrock_reach = SDL_max(1, (int32_t)SDL_lroundf(2.0f / terrain->cell_size_meters));
	generation_noise_t noise;
	int64_t pocket_count = 0;
	int64_t rock_count = 0;
	Uint64 start_ticks = SDL_GetTicks();
	int octave_count = octaves_for(size, terrain->cell_size_meters, settings);

	if (heights == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory generating terrain");
		return;
	}
	initialize_generation_noise(&noise, seed);
	terrain->seed = seed;
	terrain->max_height_meters = settings->max_height_meters;
	terrain_generate_heights(heights, size, terrain->cell_size_meters, settings, seed, sketch, TERRAIN_HEIGHTS_LAND | TERRAIN_HEIGHTS_EROSION | TERRAIN_HEIGHTS_FINISH);

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
