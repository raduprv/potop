// Water on the ground: see shallow_water.h

#include "shallow_water.h"

#include <SDL3/SDL.h>

#define GRAVITY 9.81f
// The flow kept from one step to the next (the rest lost to friction)
#define FLOW_KEPT 0.995f
// Thinner than this a cell counts as dry
#define DRY_DEPTH 0.002f

bool shallow_water_create(shallow_water_t *water, const float *corner_heights, int cells, float cell_meters, float origin_x, float origin_z)
{
	size_t count = (size_t)cells * (size_t)cells;
	int corners = cells + 1;

	SDL_zerop(water);
	water->cells = cells;
	water->cell_meters = cell_meters;
	water->origin_x = origin_x;
	water->origin_z = origin_z;
	water->floor = SDL_calloc(count, sizeof(float));
	water->depth = SDL_calloc(count, sizeof(float));
	water->flow_left = SDL_calloc(count, sizeof(float));
	water->flow_right = SDL_calloc(count, sizeof(float));
	water->flow_up = SDL_calloc(count, sizeof(float));
	water->flow_down = SDL_calloc(count, sizeof(float));
	if (water->floor == NULL || water->depth == NULL || water->flow_left == NULL || water->flow_right == NULL || water->flow_up == NULL
		|| water->flow_down == NULL)
	{
		shallow_water_destroy(water);
		return false;
	}
	for (int row = 0; row < cells; row++)
	{
		for (int column = 0; column < cells; column++)
		{
			water->floor[row * cells + column] = 0.25f * (corner_heights[row * corners + column] + corner_heights[row * corners + column + 1]
				+ corner_heights[(row + 1) * corners + column] + corner_heights[(row + 1) * corners + column + 1]);
		}
	}
	return true;
}

void shallow_water_destroy(shallow_water_t *water)
{
	SDL_free(water->floor);
	SDL_free(water->depth);
	SDL_free(water->flow_left);
	SDL_free(water->flow_right);
	SDL_free(water->flow_up);
	SDL_free(water->flow_down);
	SDL_zerop(water);
}

void shallow_water_clear(shallow_water_t *water)
{
	size_t count = (size_t)water->cells * (size_t)water->cells;

	SDL_memset(water->depth, 0, sizeof(float) * count);
	SDL_memset(water->flow_left, 0, sizeof(float) * count);
	SDL_memset(water->flow_right, 0, sizeof(float) * count);
	SDL_memset(water->flow_up, 0, sizeof(float) * count);
	SDL_memset(water->flow_down, 0, sizeof(float) * count);
	water->total_volume = 0.0;
}

// The new flow through one pipe: what it had, kept, plus what the
// difference of the two surfaces pushes through it
static float pipe_flow(float flow, float surface_here, float surface_there, float seconds, float cell_meters)
{
	float pipe_area = cell_meters * cell_meters;
	float pushed = flow * FLOW_KEPT + seconds * pipe_area * GRAVITY * (surface_here - surface_there) / cell_meters;

	return pushed > 0.0f ? pushed : 0.0f;
}

// One step of the flows and depths
static void step_once(shallow_water_t *water, float seconds);

// Split in as many steps as fast waves need: a wave goes as fast as the
// square root of gravity times the depth, and must not cross more than
// half a cell a step
void shallow_water_step(shallow_water_t *water, float seconds)
{
	int count = water->cells * water->cells;
	float deepest = 0.0f;
	float wave_speed;
	int steps;

	for (int i = 0; i < count; i++)
	{
		deepest = SDL_max(deepest, water->depth[i]);
	}
	wave_speed = SDL_sqrtf(GRAVITY * deepest);
	steps = (int)SDL_ceilf(wave_speed * seconds / (0.5f * water->cell_meters));
	steps = SDL_clamp(steps, 1, 16);
	for (int s = 0; s < steps; s++)
	{
		step_once(water, seconds / (float)steps);
	}
}

static void step_once(shallow_water_t *water, float seconds)
{
	int cells = water->cells;
	float cell_area = water->cell_meters * water->cell_meters;
	double total = 0.0;

	// The flows, from the surfaces as they are
	for (int row = 0; row < cells; row++)
	{
		for (int column = 0; column < cells; column++)
		{
			int index = row * cells + column;
			float depth = water->depth[index];
			float surface = water->floor[index] + depth;
			float out_total;
			float scale;

			if (depth <= 0.0f)
			{
				water->flow_left[index] = 0.0f;
				water->flow_right[index] = 0.0f;
				water->flow_up[index] = 0.0f;
				water->flow_down[index] = 0.0f;
				continue;
			}
			water->flow_left[index] = column > 0
				? pipe_flow(water->flow_left[index], surface, water->floor[index - 1] + water->depth[index - 1], seconds, water->cell_meters)
				: 0.0f;
			water->flow_right[index] = column < cells - 1
				? pipe_flow(water->flow_right[index], surface, water->floor[index + 1] + water->depth[index + 1], seconds, water->cell_meters)
				: 0.0f;
			water->flow_up[index] = row > 0
				? pipe_flow(water->flow_up[index], surface, water->floor[index - cells] + water->depth[index - cells], seconds, water->cell_meters)
				: 0.0f;
			water->flow_down[index] = row < cells - 1
				? pipe_flow(water->flow_down[index], surface, water->floor[index + cells] + water->depth[index + cells], seconds, water->cell_meters)
				: 0.0f;
			// No more out than there is
			out_total = (water->flow_left[index] + water->flow_right[index] + water->flow_up[index] + water->flow_down[index]) * seconds;
			if (out_total > depth * cell_area)
			{
				scale = depth * cell_area / out_total;
				water->flow_left[index] *= scale;
				water->flow_right[index] *= scale;
				water->flow_up[index] *= scale;
				water->flow_down[index] *= scale;
			}
		}
	}
	// The depths, from what flowed in and out
	for (int row = 0; row < cells; row++)
	{
		for (int column = 0; column < cells; column++)
		{
			int index = row * cells + column;
			float flow_in = 0.0f;
			float flow_out = water->flow_left[index] + water->flow_right[index] + water->flow_up[index] + water->flow_down[index];
			float depth;

			flow_in += column > 0 ? water->flow_right[index - 1] : 0.0f;
			flow_in += column < cells - 1 ? water->flow_left[index + 1] : 0.0f;
			flow_in += row > 0 ? water->flow_down[index - cells] : 0.0f;
			flow_in += row < cells - 1 ? water->flow_up[index + cells] : 0.0f;
			depth = water->depth[index] + (flow_in - flow_out) * seconds / cell_area;
			depth = depth > 0.0f ? depth : 0.0f;
			water->depth[index] = depth;
			total += (double)depth * (double)cell_area;
		}
	}
	water->total_volume = total;
}

void shallow_water_pour(shallow_water_t *water, float x, float z, float radius, float cubic_meters)
{
	int cells = water->cells;
	int first_column = (int)SDL_floorf((x - radius - water->origin_x) / water->cell_meters);
	int last_column = (int)SDL_floorf((x + radius - water->origin_x) / water->cell_meters);
	int first_row = (int)SDL_floorf((z - radius - water->origin_z) / water->cell_meters);
	int last_row = (int)SDL_floorf((z + radius - water->origin_z) / water->cell_meters);
	int touched[1024];
	int touched_count = 0;
	float cell_area = water->cell_meters * water->cell_meters;

	for (int row = SDL_max(first_row, 0); row <= SDL_min(last_row, cells - 1); row++)
	{
		for (int column = SDL_max(first_column, 0); column <= SDL_min(last_column, cells - 1); column++)
		{
			float center_x = water->origin_x + ((float)column + 0.5f) * water->cell_meters - x;
			float center_z = water->origin_z + ((float)row + 0.5f) * water->cell_meters - z;

			if (center_x * center_x + center_z * center_z <= radius * radius && touched_count < 1024)
			{
				touched[touched_count++] = row * cells + column;
			}
		}
	}
	// (a disc smaller than a cell: the cell under its middle)
	if (touched_count == 0)
	{
		int column = (int)SDL_floorf((x - water->origin_x) / water->cell_meters);
		int row = (int)SDL_floorf((z - water->origin_z) / water->cell_meters);

		if (column < 0 || row < 0 || column >= cells || row >= cells)
		{
			return;
		}
		touched[touched_count++] = row * cells + column;
	}
	for (int i = 0; i < touched_count; i++)
	{
		water->depth[touched[i]] += cubic_meters / (float)touched_count / cell_area;
	}
}

void shallow_water_fill_to(shallow_water_t *water, float surface_height)
{
	int count = water->cells * water->cells;

	for (int i = 0; i < count; i++)
	{
		float wanted = surface_height - water->floor[i];

		if (wanted > water->depth[i])
		{
			water->depth[i] = wanted;
		}
	}
}

bool shallow_water_sample(const shallow_water_t *water, float x, float z, float *depth, float *surface_height, float velocity[2])
{
	int cells = water->cells;
	int column = (int)SDL_floorf((x - water->origin_x) / water->cell_meters);
	int row = (int)SDL_floorf((z - water->origin_z) / water->cell_meters);
	int index;
	float here;
	float through;

	if (column < 0 || row < 0 || column >= cells || row >= cells)
	{
		return false;
	}
	index = row * cells + column;
	here = water->depth[index];
	*depth = here;
	*surface_height = water->floor[index] + here;
	velocity[0] = 0.0f;
	velocity[1] = 0.0f;
	if (here < DRY_DEPTH)
	{
		return false;
	}
	// The mean flow through the cell, over the area it goes through
	through = here * water->cell_meters;
	velocity[0] = 0.5f * (water->flow_right[index] - water->flow_left[index] + (column > 0 ? water->flow_right[index - 1] : 0.0f)
		- (column < cells - 1 ? water->flow_left[index + 1] : 0.0f)) / through;
	velocity[1] = 0.5f * (water->flow_down[index] - water->flow_up[index] + (row > 0 ? water->flow_down[index - cells] : 0.0f)
		- (row < cells - 1 ? water->flow_up[index + cells] : 0.0f)) / through;
	return true;
}
