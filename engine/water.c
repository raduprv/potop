// Water simulation, the CPU's side: the copy of the water the game reads,
// the sources, the step's bookkeeping. The simulation itself runs on the
// GPU (water_gpu.c). See water.h for the model.

#include "water.h"
#include "profile.h"
#include "water_gpu.h"

#include <SDL3/SDL.h>
#include <math.h>

// Moving this many units per substep or fewer is rounding noise, not flow
#define WATER_FLOW_DEAD_BAND_UNITS 2
// Films evaporate in one sweep over all wet tiles every this many seconds
#define WATER_EVAPORATION_INTERVAL_SECONDS 1.0f
// Depth scaled pipes are never thinner than this (meters)
#define WATER_MINIMUM_PIPE_DEPTH 0.001f
// Friction never divides by less than this depth (meters)
#define WATER_MINIMUM_FRICTION_DEPTH 0.01f
// Shore damping fades out for water flowing faster than this (m/s): it is
// meant for waves on still water, not for floods
#define WATER_SHORE_DAMPING_SPEED 0.3f
// Courant number used to pick the number of substeps (waves must not cross
// more than this fraction of a cell per substep)
#define WATER_COURANT_LIMIT 0.4f
// The same for the water itself: a cell can pass on at most what it holds
// in one substep, so flow can never be faster than a cell per substep.
// Once the flow gets within this fraction of that limit, the next step is
// split into more substeps, which raises the limit.
#define WATER_FLOW_COURANT_LIMIT 0.5f
// Only water at least a cell deep counts for that: films of a few
// millimeters on steep slopes pour out everything they hold in one substep
// at any substep length, so their "speed" is always at the limit and would
// only push the substeps up to the maximum for nothing.

static const float meters_per_unit = TERRAIN_METERS_PER_HEIGHT_UNIT;

// Cube root of a positive number, in plain arithmetic: the same code in
// the scalar and the vector passes (a library cbrtf can be neither inlined
// nor vectorized), so both give the same bits. A guess from the float's
// bits (its exponent divided by three), then three Newton steps, which
// take it to float precision.
static inline float positive_cube_root(float value)
{
	int32_t bits;
	float root;

	__builtin_memcpy(&bits, &value, sizeof(bits));
	bits = (int32_t)((float)bits * (1.0f / 3.0f)) + 709921077;
	__builtin_memcpy(&root, &bits, sizeof(root));
	root = (2.0f * root + value / (root * root)) * (1.0f / 3.0f);
	root = (2.0f * root + value / (root * root)) * (1.0f / 3.0f);
	root = (2.0f * root + value / (root * root)) * (1.0f / 3.0f);
	return root;
}

// depth^(7/3), for Manning's friction (h * h^(4/3)): a cube root instead of
// powf, which costs several times as much and ran twice per cell and substep
static inline float depth_to_seven_thirds(float depth)
{
	return depth * depth * positive_cube_root(depth);
}

static bool create(water_t *water, int32_t size, float cell_size_meters, bool drain_edges, bool depth_scaled_pipes, bool baked)
{
	size_t cell_count = (size_t)size * (size_t)size;
	int32_t tile_count;
	bool cells_ok;

	SDL_zerop(water);
	water->size = size;
	water->cell_size_meters = cell_size_meters;
	water->tiles_per_side = size / WATER_TILE_SIZE;
	water->drain_edges = drain_edges;
	water->depth_scaled_pipes = depth_scaled_pipes;
	water->speed_depth_meters = WATER_MINIMUM_FRICTION_DEPTH;
	tile_count = water->tiles_per_side * water->tiles_per_side;

	water->baked = baked;
	water->depth = SDL_calloc(cell_count, sizeof(int32_t));
	if (baked)
	{
		// (the rest of the cells' arrays stay NULL)
		water->baked_flow = SDL_calloc((size_t)tile_count, sizeof(uint8_t *));
		cells_ok = water->baked_flow != NULL;
	}
	else
	{
		water->sediment = SDL_calloc(cell_count, sizeof(int32_t));
		water->sediment_concentration = SDL_calloc(cell_count, sizeof(float));
		water->inflow_directions = SDL_calloc(cell_count, sizeof(uint8_t));
		water->moisture = SDL_calloc(cell_count, sizeof(uint16_t));
		water->obstacle_centimeters = SDL_calloc(cell_count, sizeof(uint16_t));
		for (int i = 0; i < WATER_DIRECTION_COUNT; i++)
		{
			water->flux[i] = SDL_calloc(cell_count, sizeof(water_flux_t));
		}
		cells_ok = water->flux[0] != NULL && water->flux[1] != NULL && water->flux[2] != NULL && water->flux[3] != NULL
			&& water->sediment != NULL && water->sediment_concentration != NULL && water->inflow_directions != NULL && water->moisture != NULL
			&& water->obstacle_centimeters != NULL;
	}
	water->tile_state = SDL_calloc((size_t)tile_count, sizeof(uint8_t));
	water->tile_wake_reason = SDL_calloc((size_t)tile_count, sizeof(uint8_t));
	water->tile_wake_step = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_idle_steps = SDL_calloc((size_t)tile_count, sizeof(uint8_t));
	water->tile_wake_requests = SDL_calloc((size_t)tile_count, sizeof(uint8_t));
	water->tile_obstacles_dirty = SDL_calloc((size_t)tile_count, sizeof(bool));
	water->tile_total_depth = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_max_depth = SDL_calloc((size_t)tile_count, sizeof(int32_t));
	water->tile_steep_cells = SDL_calloc((size_t)tile_count, sizeof(int32_t));
	water->tile_max_speed = SDL_calloc((size_t)tile_count, sizeof(float));
	water->tile_max_deep_speed = SDL_calloc((size_t)tile_count, sizeof(float));
	water->tile_max_change = SDL_calloc((size_t)tile_count, sizeof(int32_t));
	water->tile_evaporated = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_drained = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_corrections = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_discarded = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_sediment_discarded = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_sediment = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_moisture = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_sediment_drained = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->tile_sediment_corrections = SDL_calloc((size_t)tile_count, sizeof(int64_t));
	water->active_tiles = SDL_calloc((size_t)tile_count, sizeof(int32_t));
	water->update_tiles = SDL_calloc((size_t)tile_count, sizeof(int32_t));
	water->tile_in_update_list = SDL_calloc((size_t)tile_count, sizeof(uint8_t));

	if (water->depth == NULL || !cells_ok
		|| water->tile_state == NULL || water->tile_wake_reason == NULL || water->tile_wake_step == NULL || water->tile_idle_steps == NULL || water->tile_wake_requests == NULL
		|| water->tile_obstacles_dirty == NULL || water->tile_total_depth == NULL || water->tile_max_depth == NULL || water->tile_steep_cells == NULL
		|| water->tile_max_speed == NULL || water->tile_max_deep_speed == NULL || water->tile_max_change == NULL || water->tile_evaporated == NULL || water->tile_drained == NULL || water->tile_corrections == NULL
		|| water->tile_discarded == NULL || water->tile_sediment_discarded == NULL || water->active_tiles == NULL
		|| water->update_tiles == NULL || water->tile_in_update_list == NULL
		|| water->tile_sediment == NULL || water->tile_moisture == NULL || water->tile_sediment_drained == NULL
		|| water->tile_sediment_corrections == NULL || !water_gpu_init(water))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory creating the water simulation (%dx%d)", size, size);
		water_destroy(water);
		return false;
	}
	water_clear(water);
	if (baked)
	{
		SDL_Log("Water: %dx%d cells, baked (still, no simulation)", size, size);
	}
	else
	{
		SDL_Log("Water simulation: %dx%d cells, %d-bit fluxes", size, size, WATER_FLUX_BITS);
	}
	return true;
}

bool water_create(water_t *water, int32_t size, float cell_size_meters, bool drain_edges, bool depth_scaled_pipes)
{
	return create(water, size, cell_size_meters, drain_edges, depth_scaled_pipes, false);
}

bool water_create_baked(water_t *water, int32_t size, float cell_size_meters)
{
	return create(water, size, cell_size_meters, false, false, true);
}

// The baked flow of the tiles all gone (dry)
static void free_baked_flow(water_t *water)
{
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;

	if (water->baked_flow == NULL)
	{
		return;
	}
	for (int32_t tile = 0; tile < tile_count; tile++)
	{
		SDL_free(water->baked_flow[tile]);
		water->baked_flow[tile] = NULL;
	}
}

float water_concentration_at(const water_t *water, int32_t index)
{
	if (water->baked)
	{
		int32_t x = index % water->size;
		int32_t y = index / water->size;
		const uint8_t *flow = water->baked_flow[water_tile_index_of_cell(water, x, y)];

		return flow != NULL ? water_baked_decode_amount(flow[((y % WATER_TILE_SIZE) * WATER_TILE_SIZE + x % WATER_TILE_SIZE) * WATER_BAKED_FLOW_BYTES + 2],
			WATER_BAKED_MUDDIEST) : 0.0f;
	}
	return water->depth[index] > 0 ? (float)water->sediment[index] / (float)water->depth[index] : 0.0f;
}

void water_destroy(water_t *water)
{
	water_gpu_release(water);
	free_baked_flow(water);
	SDL_free(water->baked_flow);
	SDL_free(water->depth);
	SDL_free(water->sediment);
	SDL_free(water->sediment_concentration);
	SDL_free(water->inflow_directions);
	SDL_free(water->moisture);
	SDL_free(water->obstacle_centimeters);
	SDL_free(water->tile_sediment);
	SDL_free(water->tile_moisture);
	SDL_free(water->tile_sediment_drained);
	SDL_free(water->tile_sediment_corrections);
	for (int i = 0; i < WATER_DIRECTION_COUNT; i++)
	{
		SDL_free(water->flux[i]);
	}
	SDL_free(water->tile_state);
	SDL_free(water->tile_wake_reason);
	SDL_free(water->tile_wake_step);
	SDL_free(water->tile_idle_steps);
	SDL_free(water->tile_wake_requests);
	SDL_free(water->tile_obstacles_dirty);
	SDL_free(water->tile_total_depth);
	SDL_free(water->tile_max_depth);
	SDL_free(water->tile_steep_cells);
	SDL_free(water->tile_max_speed);
	SDL_free(water->tile_max_deep_speed);
	SDL_free(water->tile_max_change);
	SDL_free(water->tile_evaporated);
	SDL_free(water->tile_drained);
	SDL_free(water->tile_corrections);
	SDL_free(water->tile_discarded);
	SDL_free(water->tile_sediment_discarded);
	SDL_free(water->active_tiles);
	SDL_free(water->update_tiles);
	SDL_free(water->tile_in_update_list);
	SDL_zerop(water);
}

void water_clear(water_t *water)
{
	size_t cell_count = (size_t)water->size * (size_t)water->size;
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;

	SDL_memset(water->depth, 0, cell_count * sizeof(int32_t));
	water->fastest_flow_last_step = 0.0f;
	if (water->baked)
	{
		free_baked_flow(water);
	}
	else
	{
		SDL_memset(water->sediment, 0, cell_count * sizeof(int32_t));
		SDL_memset(water->sediment_concentration, 0, cell_count * sizeof(float));
		SDL_memset(water->inflow_directions, 0, cell_count * sizeof(uint8_t));
		SDL_memset(water->moisture, 0, cell_count * sizeof(uint16_t));
		for (int i = 0; i < WATER_DIRECTION_COUNT; i++)
		{
			SDL_memset(water->flux[i], 0, cell_count * sizeof(water_flux_t));
		}
	}
	for (int32_t tile = 0; tile < tile_count; tile++)
	{
		water->tile_state[tile] = WATER_TILE_DRY;
		water->tile_wake_step[tile] = -1;
		water->tile_idle_steps[tile] = 0;
		water->tile_total_depth[tile] = 0;
		water->tile_max_depth[tile] = 0;
		water->tile_max_speed[tile] = 0.0f;
		water->tile_max_deep_speed[tile] = 0.0f;
		water->tile_max_change[tile] = 0;
		water->tile_sediment[tile] = 0;
		water->tile_moisture[tile] = 0;
	}
	water->spring_count = 0;
	water->raining = false;
	water->rain_remainder_units = 0.0;
	water->total_units = 0;
	water->added_units = 0;
	water->removed_units = 0;
	water->drained_units = 0;
	water->evaporated_units = 0;
	water->discarded_units = 0;
	water->evaporation_timer_seconds = 0.0f;
	water->moisture_units = 0;
	water->dried_units = 0;
	water->eroded_units = 0;
	water->deposited_units = 0;
	water->collapsed_units = 0;
	water->sediment_drained_units = 0;
	water->sediment_corrections = 0;
	water->negative_depth_corrections = 0;
	water_gpu_note_clear(water);
}

bool water_sample(const water_t *water, float world_x, float world_z, float *depth_meters_out, float *velocity_x_out,
	float *velocity_z_out)
{
	int32_t cell_x = (int32_t)SDL_floorf(world_x / water->cell_size_meters + 0.5f);
	int32_t cell_y = (int32_t)SDL_floorf(world_z / water->cell_size_meters + 0.5f);
	int32_t index;
	float depth;
	float flow_x;
	float flow_z;
	float section;

	*depth_meters_out = 0.0f;
	*velocity_x_out = 0.0f;
	*velocity_z_out = 0.0f;
	if (cell_x < 0 || cell_y < 0 || cell_x >= water->size || cell_y >= water->size)
	{
		return false;
	}
	index = cell_y * water->size + cell_x;
	depth = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
	*depth_meters_out = depth;
	if (depth <= 0.0f)
	{
		return true;
	}
	if (water->baked)
	{
		const uint8_t *flow = water->baked_flow[water_tile_index_of_cell(water, cell_x, cell_y)];

		if (flow != NULL)
		{
			flow += ((cell_y % WATER_TILE_SIZE) * WATER_TILE_SIZE + cell_x % WATER_TILE_SIZE) * WATER_BAKED_FLOW_BYTES;
			*velocity_x_out = water_baked_decode_velocity(flow[0]);
			*velocity_z_out = water_baked_decode_velocity(flow[1]);
		}
		return true;
	}
	// The flow through the cell: the average of what crosses its two faces
	// on each axis (outflows count one way, the neighbors' the other)
	flow_x = (float)water->flux[WATER_DIRECTION_RIGHT][index] - (float)water->flux[WATER_DIRECTION_LEFT][index];
	flow_z = (float)water->flux[WATER_DIRECTION_FRONT][index] - (float)water->flux[WATER_DIRECTION_BACK][index];
	if (cell_x > 0)
	{
		flow_x += (float)water->flux[WATER_DIRECTION_RIGHT][index - 1];
	}
	if (cell_x < water->size - 1)
	{
		flow_x -= (float)water->flux[WATER_DIRECTION_LEFT][index + 1];
	}
	if (cell_y > 0)
	{
		flow_z += (float)water->flux[WATER_DIRECTION_FRONT][index - water->size];
	}
	if (cell_y < water->size - 1)
	{
		flow_z -= (float)water->flux[WATER_DIRECTION_BACK][index + water->size];
	}
	// Thin water is superfluid and moves a lot for its depth; it counts as
	// speed_depth_meters deep, or a film would hit like rapids
	section = SDL_max(depth, water->speed_depth_meters) * water->cell_size_meters;
	*velocity_x_out = 0.5f * flow_x / section;
	*velocity_z_out = 0.5f * flow_z / section;
	return true;
}

void water_restore(water_t *water)
{
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;
	size_t cell_count = (size_t)water->size * (size_t)water->size;

	if (!water->baked)
	{
		for (int i = 0; i < WATER_DIRECTION_COUNT; i++)
		{
			SDL_memset(water->flux[i], 0, cell_count * sizeof(water_flux_t));
		}
		SDL_memset(water->sediment_concentration, 0, cell_count * sizeof(float));
		SDL_memset(water->inflow_directions, 0, cell_count * sizeof(uint8_t));
	}
#pragma omp parallel for schedule(dynamic, 4)
	for (int32_t tile = 0; tile < tile_count; tile++)
	{
		int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
		int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;
		int64_t total = 0;
		int64_t sediment = 0;
		int64_t moisture = 0;
		int32_t deepest = 0;

		for (int32_t y = start_y; y < start_y + WATER_TILE_SIZE; y++)
		{
			for (int32_t x = start_x; x < start_x + WATER_TILE_SIZE; x++)
			{
				size_t index = (size_t)y * (size_t)water->size + (size_t)x;

				total += water->depth[index];
				if (!water->baked)
				{
					sediment += water->sediment[index];
					moisture += water->moisture[index];
				}
				deepest = SDL_max(deepest, water->depth[index]);
			}
		}
		water->tile_total_depth[tile] = total;
		water->tile_sediment[tile] = sediment;
		water->tile_moisture[tile] = moisture;
		water->tile_max_depth[tile] = deepest;
		water->tile_max_speed[tile] = 0.0f;
		water->tile_max_deep_speed[tile] = 0.0f;
		water->tile_max_change[tile] = 0;
		water->tile_idle_steps[tile] = 0;
		// Awake for a start: it settles back to sleep within a second
		// if the water there was at rest
		// (baked: still for good)
		water->tile_state[tile] = total > 0 || sediment > 0 ? (water->baked ? WATER_TILE_SLEEPING : WATER_TILE_ACTIVE) : WATER_TILE_DRY;
	}
	water->total_units = water_count_total_units(water);
	water->moisture_units = water_count_moisture_units(water);
	// The balance starts here: everything there counts as added (the
	// ground's moisture is not water any more, see erosion.c)
	water->added_units = water->total_units;
	water->removed_units = 0;
	water->drained_units = 0;
	water->evaporated_units = 0;
	water->discarded_units = 0;
	water->dried_units = 0;
	water->sediment_drained_units = 0;
	water->sediment_corrections = 0;
	water->negative_depth_corrections = 0;
	water->rain_remainder_units = 0.0;
	water->fastest_flow_last_step = 0.0f;
	for (int i = 0; i < water->spring_count; i++)
	{
		water->springs[i].remainder_units = 0.0;
	}
	// Everything goes up to the GPU anew
	water_gpu_note_restore(water);
}

static void wake_tile(water_t *water, int32_t tile, water_wake_reason_t reason)
{
	// (baked water does not move)
	if (water->baked)
	{
		return;
	}
	if (water->tile_state[tile] != WATER_TILE_ACTIVE)
	{
		water->wake_counts[reason]++;
		water->tile_wake_reason[tile] = (uint8_t)reason;
		water->tile_wake_step[tile] = water->step_count;
	}
	water->tile_state[tile] = WATER_TILE_ACTIVE;
	water->tile_idle_steps[tile] = 0;
	water_gpu_note_wake(water, tile, (int)reason);
}

void water_wake_tile(water_t *water, int32_t tile, water_wake_reason_t reason)
{
	wake_tile(water, tile, reason);
}

const char *water_wake_reason_name(water_wake_reason_t reason)
{
	static const char *names[WATER_WAKE_REASON_COUNT] = { "source", "neighbor", "edit", "soak", "erosion" };

	return (reason >= 0 && reason < WATER_WAKE_REASON_COUNT) ? names[reason] : "?";
}

void water_wake_cells(water_t *water, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y)
{
	// One extra cell around the rectangle, so tiles whose neighbors changed
	// also notice
	int32_t first_tile_x = clamp_int((minimum_x - 1) / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t first_tile_y = clamp_int((minimum_y - 1) / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t last_tile_x = clamp_int((maximum_x + 1) / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t last_tile_y = clamp_int((maximum_y + 1) / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);

	for (int32_t tile_y = first_tile_y; tile_y <= last_tile_y; tile_y++)
	{
		for (int32_t tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++)
		{
			int32_t tile = tile_y * water->tiles_per_side + tile_x;

			// A dry tile next to an edit has nothing to simulate yet; it is
			// woken when water actually flows into it
			if (water->tile_state[tile] != WATER_TILE_DRY || water->tile_total_depth[tile] > 0)
			{
				wake_tile(water, tile, WATER_WAKE_EDIT);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Sources

static void add_units_to_cell(water_t *water, int32_t cell_x, int32_t cell_y, int32_t units)
{
	int32_t index = cell_y * water->size + cell_x;

	water->depth[index] += units;
	water->added_units += units;
	water->total_units += units;
	water_gpu_note_edit(water, water_tile_index_of_cell(water, cell_x, cell_y));
	wake_tile(water, water_tile_index_of_cell(water, cell_x, cell_y), WATER_WAKE_SOURCE);
}

void water_mark_cells_edited(water_t *water, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y)
{
	int32_t first_tile_x = clamp_int(minimum_x / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t first_tile_y = clamp_int(minimum_y / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t last_tile_x = clamp_int(maximum_x / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t last_tile_y = clamp_int(maximum_y / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);

	for (int32_t tile_y = first_tile_y; tile_y <= last_tile_y; tile_y++)
	{
		for (int32_t tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++)
		{
			int32_t tile = tile_y * water->tiles_per_side + tile_x;

			water_gpu_note_edit(water, tile);
			wake_tile(water, tile, WATER_WAKE_EDIT);
		}
	}
}

void water_apply_brush(water_t *water, float center_x, float center_z, float radius_meters,
	float meters_per_second, float step_seconds)
{
	// On the GPU, where the water is; the copy here follows
	water_gpu_brush(water, center_x, center_z, radius_meters, meters_per_second, step_seconds);
}

void water_add_units(water_t *water, int32_t cell_x, int32_t cell_y, int32_t units)
{
	if (!water->baked && units > 0 && cell_x >= 0 && cell_y >= 0 && cell_x < water->size && cell_y < water->size)
	{
		add_units_to_cell(water, cell_x, cell_y, units);
	}
}

bool water_toggle_spring(water_t *water, float world_x, float world_z)
{
	int32_t cell_x = (int32_t)SDL_floorf(world_x / water->cell_size_meters + 0.5f);
	int32_t cell_y = (int32_t)SDL_floorf(world_z / water->cell_size_meters + 0.5f);
	// Clicking within two meters (or three cells) of a spring removes it
	int32_t remove_radius_cells = SDL_max(3, (int32_t)SDL_ceilf(2.0f / water->cell_size_meters));

	if (water->baked || cell_x < 1 || cell_y < 1 || cell_x >= water->size - 1 || cell_y >= water->size - 1)
	{
		return false;
	}
	for (int i = 0; i < water->spring_count; i++)
	{
		int32_t delta_x = water->springs[i].cell_x - cell_x;
		int32_t delta_y = water->springs[i].cell_y - cell_y;

		if (delta_x * delta_x + delta_y * delta_y <= remove_radius_cells * remove_radius_cells)
		{
			water->springs[i] = water->springs[--water->spring_count];
			SDL_Log("Removed spring at cell %d,%d", cell_x, cell_y);
			return false;
		}
	}
	if (water->spring_count >= WATER_MAX_SPRINGS)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot add spring: the limit is %d", WATER_MAX_SPRINGS);
		return false;
	}
	water->springs[water->spring_count].cell_x = cell_x;
	water->springs[water->spring_count].cell_y = cell_y;
	water->springs[water->spring_count].remainder_units = 0.0;
	water->spring_count++;
	SDL_Log("Added spring at cell %d,%d", cell_x, cell_y);
	return true;
}

// ---------------------------------------------------------------------------
// The rules of one cell's pipes (the inspector; the GPU runs the same in
// data/shaders/water_fluxes.comp)

// The bed the water lies on: the ground plus whatever wall stands there
#define BED(cell_index) (ground[cell_index] + (int32_t)obstacle[cell_index] * WATER_UNITS_PER_OBSTACLE_CENTIMETER)

// One cell's new outflow through its four pipes (pass 1 of a substep),
// from the current depths and fluxes; false if the cell has no water that
// can flow. Shared by the simulation (report NULL) and the inspector
// (water_inspect_cell), so the inspector always shows the rules the
// simulation really applies.
static inline __attribute__((always_inline)) bool compute_cell_fluxes(const water_t *water, const terrain_t *terrain,
	const water_flux_parameters_t *parameters, int32_t x, int32_t y, float new_flux[WATER_DIRECTION_COUNT], water_cell_report_t *report)
{
	int32_t size = water->size;
	int32_t index = y * size + x;
	const int32_t *depth = water->depth;
	const int32_t *ground = terrain->heights;
	const uint16_t *obstacle = water->obstacle_centimeters;
	int32_t water_units = depth[index];
	int32_t level;
	float depth_meters;
	float friction_factor;
	float old_flux[WATER_DIRECTION_COUNT];
	bool inside[WATER_DIRECTION_COUNT];
	int32_t neighbor[WATER_DIRECTION_COUNT];
	float total_outflow;
	// The depth the friction sees: the real one for pipes into dry
	// ground, and for thin water flowing on over wet ground the
	// superfluid depth (see below)
	float dry_friction_depth;
	float wet_friction_depth;
	float dry_manning_per_flux = 0.0f;
	float wet_manning_per_flux = 0.0f;
	float shore_factor = 1.0f;
	float dry_damping;
	float wet_damping;

	// No water, or only a film clinging to the ground: nothing flows
	if (water_units <= parameters->film_units)
	{
		return false;
	}

	level = BED(index) + water_units;
	depth_meters = (float)water_units * meters_per_unit;
	friction_factor = parameters->friction_factor[terrain->silt_thickness[index] >= parameters->silt_units ? SOIL_SILT : terrain->soil_types[index]];
	for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
	{
		old_flux[k] = (float)water->flux[k][index];
	}
	inside[WATER_DIRECTION_LEFT] = x > 0;
	inside[WATER_DIRECTION_RIGHT] = x < size - 1;
	inside[WATER_DIRECTION_BACK] = y > 0;
	inside[WATER_DIRECTION_FRONT] = y < size - 1;
	neighbor[WATER_DIRECTION_LEFT] = index - 1;
	neighbor[WATER_DIRECTION_RIGHT] = index + 1;
	neighbor[WATER_DIRECTION_BACK] = index - size;
	neighbor[WATER_DIRECTION_FRONT] = index + size;

	// Thin water flowing on over wet ground is superfluid: it counts
	// as superfluid_depth deep, for friction and pipe size. Real
	// friction would make a trickle crawl for half an hour, and
	// nobody waits for that in a game. Into dry ground it stays
	// real, so a flood's front is the real, thick front and not a
	// film racing ahead of it.
	dry_friction_depth = SDL_max(depth_meters, WATER_MINIMUM_FRICTION_DEPTH);
	wet_friction_depth = SDL_max(dry_friction_depth, parameters->superfluid_depth);
	// Manning's turbulent bed friction, the one hydrologists use for
	// rivers: it grows with the square of the speed, g n^2 v |v| /
	// h^(4/3), so water rushing down a slope reaches a natural top
	// speed (about 10 m/s for half a meter of water on a 1:3 slope)
	// instead of accelerating for as long as the slope lasts. The
	// speed of each pipe comes from its current flux; the roughness
	// is scaled by the soil's friction factor.
	// k = g n^2 dt |v| / h^(4/3), with |v| = |flux| / (h * cell)
	if (parameters->manning_times_step > 0.0f)
	{
		dry_manning_per_flux = parameters->manning_times_step * friction_factor
			/ (depth_to_seven_thirds(dry_friction_depth) * parameters->cell_size);
		wet_manning_per_flux = wet_friction_depth == dry_friction_depth ? dry_manning_per_flux
			: parameters->manning_times_step * friction_factor
				/ (depth_to_seven_thirds(wet_friction_depth) * parameters->cell_size);
	}

	// Bed friction acts harder on shallow water; the implicit form
	// 1 / (1 + k) can never flip the sign of the flux. This linear
	// part matters for slow water (it lets lakes settle); fast
	// water is held back by the Manning part.
	dry_damping = 1.0f + parameters->friction_times_step * friction_factor / dry_friction_depth;
	wet_damping = wet_friction_depth == dry_friction_depth ? dry_damping
		: 1.0f + parameters->friction_times_step * friction_factor / wet_friction_depth;

	// Shore damping: shallow, slow water near the shore is braked
	// harder the shallower it is, the way a beach swallows waves
	// instead of throwing them back. Fast water (a flood) is not
	// touched.
	if (parameters->shore_damping_times_step > 0.0f && depth_meters < parameters->shore_depth)
	{
		// fabsf (one instruction) rather than SDL_fabsf, a call into the
		// SDL library that the compiler cannot inline
		float largest_flux = SDL_max(SDL_max(fabsf(old_flux[0]), fabsf(old_flux[1])), SDL_max(fabsf(old_flux[2]), fabsf(old_flux[3])));
		float speed = largest_flux / (wet_friction_depth * parameters->cell_size);
		float slowness = clamp_float(1.0f - speed / WATER_SHORE_DAMPING_SPEED, 0.0f, 1.0f);
		float shallowness = 1.0f - depth_meters / parameters->shore_depth;

		shore_factor = 1.0f / (1.0f + parameters->shore_damping_times_step * shallowness * slowness);
	}
	// Unrolled, so inside[] and neighbor[] become plain registers
#pragma GCC unroll 4
	for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
	{
		int32_t neighbor_index = neighbor[k];
		int32_t neighbor_level = 0;
		bool wet = inside[k] && depth[neighbor_index] > parameters->film_units;
		float factor;
		float drop;
		float push;

		factor = 1.0f / ((wet ? wet_damping : dry_damping) + (wet ? wet_manning_per_flux : dry_manning_per_flux) * fabsf(old_flux[k]));
		factor *= parameters->linear_damping_factor * shore_factor;

		// Level difference to the neighbor, in meters. Outside the
		// map a drain edge behaves like dry ground at our own
		// height, a wall like an infinitely high bank.
		if (inside[k])
		{
			neighbor_level = BED(neighbor_index) + depth[neighbor_index];
			drop = (float)(level - neighbor_level) * meters_per_unit;
		}
		else
		{
			drop = parameters->drain_edges ? depth_meters : -1.0f;
		}

		// How hard a level difference pushes. The classic virtual
		// pipes model gives every pipe the same cross section
		// (cell^2): then a film of a few millimeters on a slope races
		// at tens of meters per second, and waves travel at
		// sqrt(g * cell) in any depth. Sizing the pipe by the water
		// that can actually pass between the two cells (from the
		// higher water level down to the higher ground, times the
		// cell width) is the shallow water equations: thin water is
		// slow, as it should be. But only for really thin water (up
		// to pipe_depth_limit, a few centimeters): with depth sized
		// pipes, deeper water pushes harder, the crests of waves
		// catch up with the troughs ahead and every wave piles up
		// into a sawtooth front (the shallow water equations have
		// none of the dispersion that keeps real short waves round).
		// Above the limit the pipes are classic, and so are the
		// waves. Superfluid water gets its superfluid size.
		if (!parameters->depth_scaled_pipes)
		{
			push = parameters->gravity_times_cell;
		}
		else
		{
			float section = inside[k]
				? (float)(SDL_max(level, neighbor_level) - SDL_max(BED(index), BED(neighbor_index))) * meters_per_unit
				: depth_meters;
			float minimum = wet ? SDL_max(WATER_MINIMUM_PIPE_DEPTH, parameters->superfluid_depth) : WATER_MINIMUM_PIPE_DEPTH;

			push = parameters->gravity_times_cell
				* clamp_float(section / parameters->pipe_depth_limit, SDL_min(minimum / parameters->pipe_depth_limit, 1.0f), 1.0f);
		}
		new_flux[k] = old_flux[k] * factor + parameters->step_seconds * push * drop;
		if (report != NULL)
		{
			report->pipes[k].inside = inside[k];
			report->pipes[k].neighbor_wet = wet;
			report->pipes[k].superfluid = wet && parameters->superfluid_depth > depth_meters;
			report->pipes[k].drop_millimeters = drop * 1000.0f;
			report->pipes[k].push_factor = push / parameters->gravity_times_cell;
		}

		if (inside[k])
		{
			// Fake surface tension: water does not spill onto dry
			// ground (or ground with only a film on it) unless it
			// stands at least a meniscus height above it, like a
			// glass filled a little over the brim. Without this a
			// full pond drains over its lip forever, slower and
			// slower, the way the equations say and reality does
			// not. Moving fronts carry far more than a few
			// millimeters of head, so floods and waves are not
			// affected.
			if (!wet && level - neighbor_level < parameters->meniscus_units)
			{
				new_flux[k] = 0.0f;
				if (report != NULL)
				{
					report->pipes[k].meniscus_holds = true;
				}
			}
			// The lip: deep water does not spill into thin water (or onto
			// dry ground) unless it stands at least lip_units above it.
			// A pond a little over its lip otherwise drains for hours
			// through a trickle down the wet path it made, keeping every
			// tile along it awake; with this it stops a couple of
			// centimeters over the lip, which nobody sees. A real
			// overflow or a flood carries more head and passes.
			if (parameters->lip_units > 0 && water_units > parameters->thin_units && depth[neighbor_index] <= parameters->thin_units
				&& level - neighbor_level < parameters->lip_units)
			{
				new_flux[k] = 0.0f;
				if (report != NULL)
				{
					report->pipes[k].lip_holds = true;
				}
			}
			// Static friction, against turning back only: a closed pipe
			// whose water last came in (from the neighbor) does not open
			// the other way for a level difference below still_units. A
			// lake's rocking is water going back and forth; this stops it
			// once the waves are smaller than that, instead of trading a
			// millimeter back and forth for minutes. Flow that keeps its
			// direction is never held, however gentle the slope: a
			// holding rule for any slow flow froze tilted lake surfaces
			// (a few millimeters a cell, tens of centimeters across a
			// lake), and the tilt then slid on in little jerks forever.
			// What a pipe remembers is cleared when its tile goes to
			// sleep, so a lake woken later starts free.
			if (parameters->still_units > 0 && old_flux[k] <= 0.0f && (water->inflow_directions[index] & (1u << k)) != 0
				&& level - neighbor_level < parameters->still_units)
			{
				new_flux[k] = 0.0f;
				if (report != NULL)
				{
					report->pipes[k].still_holds = true;
				}
			}
		}
		else if (parameters->drain_edges && water_units < parameters->meniscus_units)
		{
			new_flux[k] = 0.0f;
		}
	}

	// A wall edge never lets anything through
	if (!parameters->drain_edges)
	{
		if (x == 0)
		{
			new_flux[WATER_DIRECTION_LEFT] = 0.0f;
		}
		if (x == size - 1)
		{
			new_flux[WATER_DIRECTION_RIGHT] = 0.0f;
		}
		if (y == 0)
		{
			new_flux[WATER_DIRECTION_BACK] = 0.0f;
		}
		if (y == size - 1)
		{
			new_flux[WATER_DIRECTION_FRONT] = 0.0f;
		}
	}

	total_outflow = 0.0f;
	for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
	{
		new_flux[k] = SDL_max(new_flux[k], 0.0f);
		total_outflow += new_flux[k];
	}

	// Never send more water than the cell holds. The 0.998 margin
	// covers rounding when the fluxes are stored as 16 bit floats.
	if (total_outflow > 0.0f)
	{
		float available = depth_meters * parameters->cell_area * 0.998f;
		float wanted = total_outflow * parameters->step_seconds;

		if (wanted > available)
		{
			float scale = available / wanted;

			if (report != NULL)
			{
				report->outflow_limited = true;
			}
			for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
			{
				new_flux[k] *= scale;
			}
		}
	}
	if (report != NULL)
	{
		report->dry_friction_depth = dry_friction_depth;
		report->wet_friction_depth = wet_friction_depth;
		report->shore_factor = shore_factor;
		for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
		{
			report->pipes[k].next_flux = new_flux[k];
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tiles

static void zero_tile_fluxes(water_t *water, int32_t tile)
{
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

	if (water->baked)
	{
		return;
	}
	for (int32_t y = start_y; y < start_y + WATER_TILE_SIZE; y++)
	{
		size_t offset = (size_t)y * (size_t)water->size + (size_t)start_x;

		for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
		{
			SDL_memset(&water->flux[k][offset], 0, WATER_TILE_SIZE * sizeof(water_flux_t));
		}
		SDL_memset(&water->inflow_directions[offset], 0, WATER_TILE_SIZE);
	}
}

void water_set_tile_dry(water_t *water, int32_t tile)
{
	water->tile_state[tile] = WATER_TILE_DRY;
	water->tile_idle_steps[tile] = 0;
	zero_tile_fluxes(water, tile);
}

// Tiles whose cells all change by less than this per substep (and whose
// water moves slower than the sleep speed) count as still. A cell can trade
// a couple of rounding units with each of its four neighbors at once, hence
// the margin; water still filling a sleeping tile wakes it anyway through
// the border check.
int32_t water_sleep_change_units(const water_settings_t *settings, float substep_seconds)
{
	return (int32_t)(settings->sleep_change_millimeters_per_second * 0.001f * substep_seconds * (float)TERRAIN_HEIGHT_UNITS_PER_METER)
		+ 4 * WATER_FLOW_DEAD_BAND_UNITS;
}

// Everything pass 1 needs that is the same for every cell of a substep
void water_make_flux_parameters(const water_t *water, const water_settings_t *settings, const soil_settings_t *soil_settings,
	float substep_seconds, water_flux_parameters_t *parameters)
{
	float cell_area = water->cell_size_meters * water->cell_size_meters;
	float units_per_flux = substep_seconds / cell_area * (float)TERRAIN_HEIGHT_UNITS_PER_METER;

	parameters->step_seconds = substep_seconds;
	parameters->gravity_times_cell = settings->gravity * water->cell_size_meters;
	// A limit of 0 (or less than the smallest pipe) means classic pipes
	parameters->depth_scaled_pipes = water->depth_scaled_pipes && settings->pipe_depth_limit_meters > WATER_MINIMUM_PIPE_DEPTH;
	parameters->pipe_depth_limit = settings->pipe_depth_limit_meters;
	parameters->superfluid_depth = settings->thin_water_superfluid ? SDL_max(0.0f, settings->thin_water_millimeters * 0.001f) : 0.0f;
	parameters->shore_depth = SDL_max(0.0f, settings->shore_damping_depth_meters);
	parameters->shore_damping_times_step = parameters->shore_depth > 0.0f ? SDL_max(0.0f, settings->shore_damping_per_second) * substep_seconds : 0.0f;
	parameters->still_units = (int32_t)(SDL_max(0.0f, settings->still_level_millimeters) * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->asleep_wake_units = SDL_max(1.0f, settings->asleep_wake_millimeters_per_second * 0.001f * substep_seconds
		* (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->asleep_level_wake_units = (int32_t)(SDL_max(0.0f, settings->asleep_level_wake_millimeters) * 0.001f
		* (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->friction_times_step = settings->friction * substep_seconds;
	parameters->manning_times_step = settings->gravity * settings->manning_roughness * settings->manning_roughness * substep_seconds;
	parameters->cell_size = water->cell_size_meters;
	parameters->linear_damping_factor = SDL_max(0.0f, 1.0f - settings->linear_damping * substep_seconds);
	parameters->cell_area = cell_area;
	parameters->film_units = (int32_t)(settings->film_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->units_per_flux = units_per_flux;
	parameters->meniscus_units = (int32_t)(settings->meniscus_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->thin_units = (int32_t)(SDL_max(0.0f, settings->thin_water_millimeters) * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->lip_units = (int32_t)(SDL_max(0.0f, settings->lip_millimeters) * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->drain_edges = water->drain_edges;
	parameters->border_wake_units = SDL_max(1.0f, settings->border_wake_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	for (int i = 0; i < SOIL_PROPERTY_COUNT; i++)
	{
		parameters->friction_factor[i] = soil_settings->soils[i].friction_factor;
	}
	parameters->silt_units = SDL_max(1, (int32_t)(soil_settings->silt_visible_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER));
}

void water_step(water_t *water, const terrain_t *terrain, const water_settings_t *settings, const soil_settings_t *soil_settings,
	float step_seconds)
{
	double cell_area = (double)water->cell_size_meters * (double)water->cell_size_meters;
	int32_t spring_units[WATER_MAX_SPRINGS];
	int32_t rain_units = 0;
	float wave_speed;
	int32_t substeps;
	bool evaporate = false;
	float evaporation_seconds = 0.0f;

	if (water->baked)
	{
		return;
	}
	water->step_count++;
	water->speed_depth_meters = WATER_MINIMUM_FRICTION_DEPTH;
	if (settings->thin_water_superfluid)
	{
		water->speed_depth_meters = SDL_max(water->speed_depth_meters, settings->thin_water_millimeters * 0.001f);
	}

	// Springs pour into a 3x3 block, so the flow starts a little spread
	// out; the fractions of a unit wait here for the next step
	for (int i = 0; i < water->spring_count; i++)
	{
		water_spring_t *spring = &water->springs[i];

		spring->remainder_units += settings->spring_cubic_meters_per_second * step_seconds / cell_area * TERRAIN_HEIGHT_UNITS_PER_METER / 9.0;
		spring_units[i] = (int32_t)spring->remainder_units;
		spring->remainder_units -= spring_units[i];
	}
	// Rain adds no water: a shower is a few millimeters nobody sees, and a
	// real flood comes from the springs and the brush. It only wets the
	// ground (erosion.c), and its drops are particles.

	// Waves in this model travel at about sqrt(g * cell size): every pipe
	// pushes at most as hard as a classic one, a cell squared in cross
	// section, whatever the depth, so deep lakes do not make waves faster.
	// Only if a step would let a wave cross too much of a cell is it split
	// into substeps.
	wave_speed = SDL_sqrtf(settings->gravity * water->cell_size_meters);
	substeps = (int32_t)SDL_ceilf(wave_speed * step_seconds / (WATER_COURANT_LIMIT * water->cell_size_meters));
	// The water itself cannot move faster than a cell per substep (a cell
	// cannot give away more than it has), so when the fastest flow came
	// close to that (the GPU's figure, a frame or two old) this step gets
	// more substeps; past substep_speed_meters_per_second it is not worth
	// more (the limit holds such water back, which looks the same)
	{
		float counted_speed = settings->substep_speed_meters_per_second > 0.0f
			? SDL_min(water->fastest_flow_last_step, settings->substep_speed_meters_per_second) : water->fastest_flow_last_step;
		int32_t flow_substeps = (int32_t)SDL_ceilf(counted_speed * step_seconds / (WATER_FLOW_COURANT_LIMIT * water->cell_size_meters));

		substeps = SDL_max(substeps, flow_substeps);
	}
	substeps = clamp_int(substeps, 1, SDL_max(1, settings->maximum_substeps));
	water->substeps_last_step = substeps;
	frame_profile_count(FRAME_STEPS, 1);
	frame_profile_count(FRAME_SUBSTEPS, substeps);
	// (the tiles the step before simulated, as the GPU said: the work it has)
	frame_profile_count(FRAME_GPU_WATER_TILES, water->simulated_tiles_last_step * substeps);

	// Films (and thin water left behind) evaporate in one sweep every second
	water->evaporation_timer_seconds += step_seconds;
	if (water->evaporation_timer_seconds >= WATER_EVAPORATION_INTERVAL_SECONDS)
	{
		evaporate = true;
		evaporation_seconds = water->evaporation_timer_seconds;
		water->evaporation_timer_seconds = 0.0f;
	}

	water_gpu_step(water, terrain, settings, soil_settings, step_seconds, substeps, spring_units, rain_units, evaporate, evaporation_seconds);
}

void water_inspect_cell(const water_t *water, const terrain_t *terrain, const water_settings_t *settings,
	const soil_settings_t *soil_settings, float substep_seconds, int32_t cell_x, int32_t cell_y, water_cell_report_t *report)
{
	water_flux_parameters_t parameters;
	float new_flux[WATER_DIRECTION_COUNT];

	SDL_zerop(report);
	if (water->baked || cell_x < 0 || cell_y < 0 || cell_x >= water->size || cell_y >= water->size)
	{
		return;
	}
	water_make_flux_parameters(water, settings, soil_settings, substep_seconds, &parameters);
	report->shore_factor = 1.0f;
	report->flows = compute_cell_fluxes(water, terrain, &parameters, cell_x, cell_y, new_flux, report);
	report->units_per_flux = parameters.units_per_flux;
	report->film_units = parameters.film_units;
	report->thin_units = (int32_t)(settings->thin_water_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	report->still_units = parameters.still_units;
	report->sleep_change_units = water_sleep_change_units(settings, substep_seconds);
	report->substep_seconds = substep_seconds;
}

int64_t water_count_total_units(const water_t *water)
{
	int64_t total = 0;
	int64_t cell_count = (int64_t)water->size * water->size;

	for (int64_t i = 0; i < cell_count; i++)
	{
		total += water->depth[i];
	}
	return total;
}

int64_t water_count_sediment_units(const water_t *water)
{
	int64_t total = 0;
	int64_t cell_count = (int64_t)water->size * water->size;

	if (water->sediment == NULL)
	{
		return 0;
	}

	for (int64_t i = 0; i < cell_count; i++)
	{
		total += water->sediment[i];
	}
	return total;
}

int64_t water_count_moisture_units(const water_t *water)
{
	int64_t total = 0;
	int64_t cell_count = (int64_t)water->size * water->size;

	if (water->moisture == NULL)
	{
		return 0;
	}

	for (int64_t i = 0; i < cell_count; i++)
	{
		total += water->moisture[i];
	}
	return total;
}

void water_count_tiles(const water_t *water, int32_t *active_out, int32_t *sleeping_out, int32_t *dry_out)
{
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;
	int32_t counts[3] = { 0, 0, 0 };

	for (int32_t tile = 0; tile < tile_count; tile++)
	{
		counts[water->tile_state[tile]]++;
	}
	*dry_out = counts[WATER_TILE_DRY];
	*sleeping_out = counts[WATER_TILE_SLEEPING];
	*active_out = counts[WATER_TILE_ACTIVE];
}

// Water shallower than this is no ice to stand on
static const float ICE_SURFACE_DEPTH_METERS = 0.003f;

bool water_ice_level_at_cell(const water_t *water, const terrain_t *terrain, int32_t x, int32_t y, float *level_out)
{
	float sum = 0.0f;
	int count = 0;

	if (x < 0 || y < 0 || x >= water->size || y >= water->size
		|| (float)water->depth[y * water->size + x] * TERRAIN_METERS_PER_HEIGHT_UNIT <= ICE_SURFACE_DEPTH_METERS)
	{
		return false;
	}
	for (int32_t dy = -1; dy <= 1; dy++)
	{
		for (int32_t dx = -1; dx <= 1; dx++)
		{
			int32_t nx = x + dx;
			int32_t ny = y + dy;
			float depth;

			if (nx < 0 || ny < 0 || nx >= water->size || ny >= water->size)
			{
				continue;
			}
			depth = (float)water->depth[ny * water->size + nx] * TERRAIN_METERS_PER_HEIGHT_UNIT;
			if (depth > ICE_SURFACE_DEPTH_METERS)
			{
				sum += terrain_height_meters_clamped(terrain, nx, ny) + depth;
				count++;
			}
		}
	}
	*level_out = sum / (float)count;
	return true;
}

bool water_ice_surface(const water_t *water, const terrain_t *terrain, float world_x, float world_z, float *height_out)
{
	float cell_x = world_x / water->cell_size_meters;
	float cell_y = world_z / water->cell_size_meters;
	int32_t x0 = (int32_t)SDL_floorf(cell_x);
	int32_t y0 = (int32_t)SDL_floorf(cell_y);
	float fraction_x = cell_x - (float)x0;
	float fraction_y = cell_y - (float)y0;
	float sum = 0.0f;
	float weight = 0.0f;

	for (int k = 0; k < 4; k++)
	{
		float level;
		float share = ((k & 1) ? fraction_x : 1.0f - fraction_x) * ((k >> 1) ? fraction_y : 1.0f - fraction_y);

		if (share > 0.0f && water_ice_level_at_cell(water, terrain, x0 + (k & 1), y0 + (k >> 1), &level))
		{
			sum += share * level;
			weight += share;
		}
	}
	if (weight < 1e-4f)
	{
		return false;
	}
	*height_out = sum / weight;
	return true;
}

size_t water_memory_bytes(const water_t *water)
{
	size_t cells = (size_t)water->size * (size_t)water->size;
	size_t bytes = cells * sizeof(int32_t);

	if (water->baked)
	{
		for (int32_t tile = 0; tile < water->tiles_per_side * water->tiles_per_side; tile++)
		{
			bytes += water->baked_flow[tile] != NULL ? WATER_BAKED_TILE_CELLS * WATER_BAKED_FLOW_BYTES : 0;
		}
		return bytes;
	}
	return bytes + cells * (sizeof(int32_t) + sizeof(float) + 1 + 2 * sizeof(uint16_t) + WATER_DIRECTION_COUNT * sizeof(water_flux_t));
}
