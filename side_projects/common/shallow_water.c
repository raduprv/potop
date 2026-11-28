// Water on the ground: see shallow_water.h

#include "shallow_water.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GRAVITY 9.81f
// The flow kept from one step to the next (the rest lost to friction)
#define FLOW_KEPT 0.995f
// Thinner than this a cell counts as dry
#define DRY_DEPTH 0.002f

bool shallow_water_create(shallow_water_t *water, const float *corner_heights, int cells, float cell_meters, float origin_x, float origin_z)
{
	size_t count = (size_t)cells * (size_t)cells;
	int corners = cells + 1;

	memset(water, 0, sizeof(*water));
	water->cells = cells;
	water->cell_meters = cell_meters;
	water->origin_x = origin_x;
	water->origin_z = origin_z;
	water->floor = calloc(count, sizeof(float));
	water->depth = calloc(count, sizeof(float));
	water->flow_left = calloc(count, sizeof(float));
	water->flow_right = calloc(count, sizeof(float));
	water->flow_up = calloc(count, sizeof(float));
	water->flow_down = calloc(count, sizeof(float));
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
	free(water->floor);
	free(water->depth);
	free(water->flow_left);
	free(water->flow_right);
	free(water->flow_up);
	free(water->flow_down);
	memset(water, 0, sizeof(*water));
}

void shallow_water_clear(shallow_water_t *water)
{
	size_t count = (size_t)water->cells * (size_t)water->cells;

	memset(water->depth, 0, sizeof(float) * count);
	memset(water->flow_left, 0, sizeof(float) * count);
	memset(water->flow_right, 0, sizeof(float) * count);
	memset(water->flow_up, 0, sizeof(float) * count);
	memset(water->flow_down, 0, sizeof(float) * count);
	water->total_volume = 0.0;
}

// The new flow through one pipe: what it had, kept, plus what the
// difference of the two surfaces pushes through it
// (Functions, not macros: an argument is worked out once)
static float larger_of(float first, float second)
{
	return first > second ? first : second;
}

static int larger_int(int first, int second)
{
	return first > second ? first : second;
}

static int smaller_int(int first, int second)
{
	return first < second ? first : second;
}

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
		deepest = larger_of(deepest, water->depth[i]);
	}
	wave_speed = sqrtf(GRAVITY * deepest);
	steps = (int)ceilf(wave_speed * seconds / (0.5f * water->cell_meters));
	steps = larger_int(1, smaller_int(steps, 16));
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
	int first_column = (int)floorf((x - radius - water->origin_x) / water->cell_meters);
	int last_column = (int)floorf((x + radius - water->origin_x) / water->cell_meters);
	int first_row = (int)floorf((z - radius - water->origin_z) / water->cell_meters);
	int last_row = (int)floorf((z + radius - water->origin_z) / water->cell_meters);
	int touched[1024];
	int touched_count = 0;
	float cell_area = water->cell_meters * water->cell_meters;

	for (int row = larger_int(first_row, 0); row <= smaller_int(last_row, cells - 1); row++)
	{
		for (int column = larger_int(first_column, 0); column <= smaller_int(last_column, cells - 1); column++)
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
		int column = (int)floorf((x - water->origin_x) / water->cell_meters);
		int row = (int)floorf((z - water->origin_z) / water->cell_meters);

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

void shallow_water_blast(shallow_water_t *water, float x, float y, float z, float kilograms)
{
	int cells = water->cells;
	float reach = 1.6f * cbrtf(kilograms > 0.0f ? kilograms : 0.0f);
	float ring_reach = reach * 1.6f;
	float cell_area = water->cell_meters * water->cell_meters;
	int center_column = (int)floorf((x - water->origin_x) / water->cell_meters);
	int center_row = (int)floorf((z - water->origin_z) / water->cell_meters);
	int span = (int)ceilf(ring_reach / water->cell_meters) + 1;
	float surface = y;
	float strength;
	double thrown = 0.0;
	int ring_cells = 0;

	if (reach <= 0.0f)
	{
		return;
	}
	// How high over the water it went off: the surface of the deepest
	// cell within its reach (it may go off over the shore)
	{
		float deepest = 0.0f;

		for (int row = larger_int(center_row - span, 0); row <= smaller_int(center_row + span, cells - 1); row++)
		{
			for (int column = larger_int(center_column - span, 0); column <= smaller_int(center_column + span, cells - 1); column++)
			{
				int index = row * cells + column;

				if (water->depth[index] > deepest)
				{
					deepest = water->depth[index];
					surface = water->floor[index] + water->depth[index];
				}
			}
		}
		if (deepest < DRY_DEPTH)
		{
			return;
		}
	}
	strength = 1.0f - (y - surface) / reach;
	strength = strength > 1.0f ? 1.0f : strength;
	if (strength <= 0.0f)
	{
		return;
	}
	// The crater: out of the cells within reach, most from the middle;
	// and every cell out to the ring set flowing outwards
	for (int row = larger_int(center_row - span, 0); row <= smaller_int(center_row + span, cells - 1); row++)
	{
		for (int column = larger_int(center_column - span, 0); column <= smaller_int(center_column + span, cells - 1); column++)
		{
			int index = row * cells + column;
			float offset_x = water->origin_x + ((float)column + 0.5f) * water->cell_meters - x;
			float offset_z = water->origin_z + ((float)row + 0.5f) * water->cell_meters - z;
			float distance = sqrtf(offset_x * offset_x + offset_z * offset_z);
			float outward_x = distance > 0.001f ? offset_x / distance : 0.0f;
			float outward_z = distance > 0.001f ? offset_z / distance : 0.0f;
			float depth = water->depth[index];
			float speed;
			float flow;

			if (distance > ring_reach)
			{
				continue;
			}
			if (distance < reach)
			{
				float fraction = 0.9f * strength * (1.0f - distance * distance / (reach * reach));
				float taken = depth * fraction;

				water->depth[index] -= taken;
				depth -= taken;
				thrown += (double)taken * (double)cell_area;
			}
			else
			{
				ring_cells++;
			}
			// (fastest at the crater's rim, none at the ring's edge)
			speed = 6.0f * strength * (1.0f - distance / ring_reach);
			flow = speed * depth * water->cell_meters;
			water->flow_right[index] = larger_of(outward_x, 0.0f) * flow;
			water->flow_left[index] = larger_of(-outward_x, 0.0f) * flow;
			water->flow_down[index] = larger_of(outward_z, 0.0f) * flow;
			water->flow_up[index] = larger_of(-outward_z, 0.0f) * flow;
		}
	}
	if (ring_cells == 0)
	{
		return;
	}
	// What was thrown out falls on the ring
	for (int row = larger_int(center_row - span, 0); row <= smaller_int(center_row + span, cells - 1); row++)
	{
		for (int column = larger_int(center_column - span, 0); column <= smaller_int(center_column + span, cells - 1); column++)
		{
			float offset_x = water->origin_x + ((float)column + 0.5f) * water->cell_meters - x;
			float offset_z = water->origin_z + ((float)row + 0.5f) * water->cell_meters - z;
			float distance = sqrtf(offset_x * offset_x + offset_z * offset_z);

			if (distance >= reach && distance <= ring_reach)
			{
				water->depth[row * cells + column] += (float)(thrown / (double)ring_cells / (double)cell_area);
			}
		}
	}
}

bool shallow_water_sample(const shallow_water_t *water, float x, float z, float *depth, float *surface_height, float velocity[2])
{
	int cells = water->cells;
	int column = (int)floorf((x - water->origin_x) / water->cell_meters);
	int row = (int)floorf((z - water->origin_z) / water->cell_meters);
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
