// The water simulation on the GPU. See water_gpu.h, and water_sim.glsl for
// the layout of the buffers.

#include "water_gpu.h"
#include "objects.h"
#include "profile.h"
#include "water.h"

#include "math3d.h"
#include <SDL3/SDL.h>
#include <string.h>

#define TILE_CELLS (WATER_TILE_SIZE * WATER_TILE_SIZE)

// The control buffer (data/shaders/water_sim.glsl)
// The indirect dispatch arguments, in a buffer of their own (see
// water_sim.glsl)
#define ARGUMENT_ACTIVE_TILES 0
#define ARGUMENT_ACTIVE_THREADS 3
#define ARGUMENT_UPDATE_TILES 6
#define ARGUMENT_UPDATE_THREADS 9
#define ARGUMENT_LIST_TILES 12
#define ARGUMENT_LIST_THREADS 15
#define ARGUMENT_WORDS 18
#define HEADER_LAST_FASTEST_DEEP_SPEED 23
#define HEADER_COUNTERS 24
#define HEADER_MOST_UPDATE_TILES 62
#define HEADER_WORDS 64

#define COUNTER_ADDED 0
#define COUNTER_REMOVED 1
#define COUNTER_DRAINED 2
#define COUNTER_EVAPORATED 3
#define COUNTER_DISCARDED 4
#define COUNTER_CORRECTIONS 5
#define COUNTER_SEDIMENT_DRAINED 6
#define COUNTER_SEDIMENT_CORRECTIONS 7
#define COUNTER_KEPT_AWAKE_BY_FLOW 8
#define COUNTER_KEPT_AWAKE_BY_LEVEL 9
#define COUNTER_WAKE_FIRST 10
#define COUNTER_ERODED 15
#define COUNTER_DEPOSITED 16
#define COUNTER_COLLAPSED 17
#define COUNTER_COUNT 18

#define TILE_STATE 0
#define TILE_IDLE_STEPS 1
#define TILE_TOTAL_DEPTH 4
#define TILE_MAX_DEPTH 6
#define TILE_MAX_SPEED 7
#define TILE_MAX_DEEP_SPEED 8
#define TILE_MAX_CHANGE 9
#define TILE_SEDIMENT 10
#define TILE_FLAGS 12
#define TILE_WAKE_REASON 13
#define TILE_WAKE_STEP 14
#define TILE_STEEP_CELLS 15
#define TILE_WORDS 24
#define TILE_FLAG_CPU_DIRTY 1u
#define TILE_FLAG_TERRAIN_DIRTY 4u

// What the kernels do (their mode)
#define CONTROL_RESET_TILE_LISTS 1
#define CONTROL_RESET_LIST 2
#define CONTROL_ARGUMENTS 4
#define CONTROL_RESET_UPDATE_AFTER 32
#define CONTROL_RESET_ACTIVE_AFTER 64
#define CONTROL_STEP_BEGIN 8
#define CONTROL_RESET_COUNTERS 16
#define TILES_ACTIVE 0
#define TILES_WET 1
#define TILES_RENDER 2
#define TILES_CLEAR_CPU_FLAGS 4
#define TILES_EROSION 5
#define TILES_EROSION_COLOR 6
#define TILES_GROUND 7
// (water_sim.glsl: a tile only to be drawn again)
#define WAKE_GROUND_ONLY 255
// The silt and the bare rock shown take this long to follow the ground
// all the way (water_ground.comp)
static const float GROUND_FADE_SECONDS = 1.5f;
#define SOURCE_BRUSH 0
#define SOURCE_SPRINGS 1
#define SOURCE_WAKE 2
#define CELLS_RAIN 0
#define CELLS_EVAPORATE 1
#define CELLS_CLEAR 2

// The buffers the kernels read and write, in their binding order
enum
{
	BUFFER_DEPTH = 0,
	BUFFER_SEDIMENT,
	BUFFER_FLUX,
	BUFFER_AUX,
	BUFFER_MOISTURE,
	BUFFER_HEIGHT,
	BUFFER_LAYER,
	BUFFER_CONTROL,
	BUFFER_COUNT
};

// Copies back in flight, and how many tiles each may bring (about 10 MB a
// frame at most); the tiles around the camera's target come first
#define DOWNLOAD_SLOTS 3
// How often the statistics are copied back while nothing changes in them
#define STATISTICS_QUIET_FRAMES 4
#define DOWNLOAD_BUDGET_TILES 128
#define FOCUS_RADIUS_TILES 3
// A tile's water in a copy: depth, sediment, flux; its ground: height,
// bedrock and silt. Ground tiles are marked in the list by this bit.
#define DOWNLOAD_TILE_BYTES (TILE_CELLS * (4 + 4 + 8))
#define DOWNLOAD_TERRAIN_BYTES (TILE_CELLS * (4 + 8))
#define TERRAIN_ENTRY 0x40000000
// Ground tiles each copy may bring (erosion changes the ground slowly)
#define DOWNLOAD_TERRAIN_TILES 64
// A tile's copy back no more often than this (milliseconds): what reads
// it (the physics, the actors, the paths, the ground's moisture) does not
// need it every frame, and each copy is 64 KB a tile. Near the focus more
// often (the logs floating by the camera)
static const uint64_t DOWNLOAD_INTERVAL_MILLISECONDS = 100;
static const uint64_t DOWNLOAD_FOCUS_INTERVAL_MILLISECONDS = 50;
static const uint64_t DOWNLOAD_TERRAIN_INTERVAL_MILLISECONDS = 250;
// The full copy (water_gpu_sync) goes this many tiles at a time
#define SYNC_BATCH_TILES 1024

// The parameter block (water_block in water_sim.glsl, std140)
typedef struct
{
	int32_t map_size;
	int32_t tiles_per_side;
	int32_t tile_count;
	int32_t mode;

	float step_seconds;
	float gravity_times_cell;
	float pipe_depth_limit;
	float superfluid_depth;

	float shore_depth;
	float shore_damping_times_step;
	float asleep_wake_units;
	int32_t asleep_level_wake_units;

	float friction_times_step;
	float manning_times_step;
	float cell_size;
	float linear_damping_factor;

	float cell_area;
	int32_t film_units;
	int32_t meniscus_units;
	int32_t thin_units;

	int32_t lip_units;
	float border_wake_units;
	float units_per_flux;
	int32_t still_units;

	int32_t depth_scaled_pipes;
	int32_t drain_edges;
	int32_t silt_units;
	int32_t sleep_change_units;

	float friction_factors[4];
	float more_friction_factors[4];

	float speed_depth_meters;
	int32_t deep_flow_units;
	float sleep_speed;
	int32_t sleep_after_substeps;

	float brush[4];
	int32_t brush_rectangle[4];

	int32_t brush_adding;
	int32_t command_count;
	int32_t rain_units;
	int32_t step_number;

	int32_t evaporation_units;
	int32_t thin_evaporation_units;
	int32_t wake_reason;
	int32_t command_first;

	float transport_capacity;
	float deposition_rate;
	float still_speed;
	float still_deposition_rate;

	int32_t erosion_start_units;
	int32_t erosion_full_units;
	int32_t minimum_soil_units;
	int32_t submerged_units;

	float erosion_smoothing;
	float wet_critical_speed_factor;
	int32_t collapse_color;
	int32_t render_stepped;

	float erodibilities[4];
	float critical_speeds[4];
	float talus_slopes[4];
	float silt_soil[4];

	float ground_shown[4];
	int32_t ground_more[4];

	int32_t ice[4];                // 1: the ice (water_ice.h) holds the water under it still (water_fluxes.comp); -
} water_gpu_parameters_t;

SDL_COMPILE_TIME_ASSERT(water_gpu_parameters_size, sizeof(water_gpu_parameters_t) == 384);

typedef struct
{
	SDL_GPUTransferBuffer *transfer;
	uint32_t capacity;
	SDL_GPUFence *fence;
	uint64_t sequence;             // order of asking
	uint32_t serial;               // the CPU's edit serial when asked
	uint32_t generation;           // clears since the start, when asked
	bool has_statistics;
	int32_t tile_count;
	int32_t *tiles;
	bool busy;
} download_t;

typedef struct water_gpu_s
{
	int32_t tile_count;
	uint32_t cell_count;

	// What the CPU changed, waiting to go up
	uint8_t *tile_edited;
	bool any_edited;
	int32_t *wake_commands;        // tile, reason
	int32_t wake_count;
	int32_t wake_capacity;
	bool clear_pending;
	bool restore_pending;
	bool upload_all;               // the GPU's buffers are new: everything goes up
	bool baked_clearable;          // baked water: its textures can be cleared on the GPU

	// The CPU's copy: tiles it is behind on, and its newest change of each
	// (a copy asked for before that change must not overwrite it)
	uint8_t *tile_needs_download;
	// Each tile's words of the last statistics taken in (TILE_WORDS a
	// tile): one whose words come the same again has nothing new, its
	// dozen numbers not copied out again (most tiles of a map with little
	// water, every time). NULL: none kept, every tile taken in
	uint32_t *tile_words_last;
	uint64_t *tile_downloaded_ticks;   // when each was last asked for (SDL_GetTicks)
	uint64_t *tile_terrain_downloaded_ticks;
	uint32_t *tile_edit_serial;
	uint32_t edit_serial;
	uint32_t generation;
	int32_t download_cursor;
	uint64_t download_sequence;
	download_t downloads[DOWNLOAD_SLOTS];
	int32_t *chosen_tiles;
	// The ground: tiles erosion changed that the CPU's copy is behind on,
	// and the newest upload of each (a copy asked for before must not
	// overwrite it)
	uint8_t *tile_terrain_needs_download;
	uint32_t *tile_terrain_serial;
	int32_t terrain_cursor;
	terrain_t *terrain;            // the CPU's ground, which the copies update
	int64_t shadow_threshold_units;
	// Steps recorded since the last submit, and the fence of the last
	// submit made for that (runs without frames: the tests, the warmup)
	int32_t steps_since_submit;
	SDL_GPUFence *step_fence;
	bool stepped_since_render;         // the depth texture's previous step moves on
	// The last statistics copied back showed no tile changed nor asking for
	// anything (a map with no water moving): they are then copied only
	// every STATISTICS_QUIET_FRAMES frames, every tile's words being read
	// and compared each time. What the GPU counts adds up meanwhile
	bool statistics_quiet;
	int32_t statistics_wait_frames;
	uint32_t statistics_edit_serial;   // the CPU's edits as they stood at the last copy asked for (an edit: asked again at once)

	// The GPU's side
	bool ready;
	bool failed;
	SDL_GPUBuffer *buffers[BUFFER_COUNT];
	SDL_GPUBuffer *obstacle_buffer;
	SDL_GPUBuffer *soil_buffer;
	SDL_GPUBuffer *command_buffer;
	SDL_GPUBuffer *argument_buffer;    // indirect dispatch arguments
	SDL_GPUTexture *depth_texture;
	SDL_GPUTexture *flow_texture;
	// The ice that holds the water still (water_gpu_set_ice; NULL: none),
	// and what stands in for it without (all clear)
	SDL_GPUTexture *ice_texture;
	SDL_GPUTexture *ice_stand_in;
	SDL_GPUComputePipeline *control_pipeline;
	SDL_GPUComputePipeline *tiles_pipeline;
	SDL_GPUComputePipeline *fluxes_pipeline;
	SDL_GPUComputePipeline *update_list_pipeline;
	SDL_GPUComputePipeline *depths_pipeline;
	SDL_GPUComputePipeline *finish_pipeline;
	SDL_GPUComputePipeline *sources_pipeline;
	SDL_GPUComputePipeline *cells_pipeline;
	SDL_GPUComputePipeline *render_pipeline;
	SDL_GPUComputePipeline *ground_pipeline;   // the terrain's textures (water_ground.comp)
	// Where the terrain's textures go, and the tiles asked to be drawn
	// again (one request each)
	water_ground_targets_t ground;
	bool ground_set;
	bool ground_all_pending;               // every tile, at once (new textures)
	bool ground_live;                      // they are drawn from here now
	uint8_t *ground_requested;
	uint64_t ground_ticks;                 // the last pass (its fade)
	SDL_GPUComputePipeline *erode_pipeline;
	SDL_GPUComputePipeline *collapse_pipeline;
	water_gpu_parameters_t parameters;
	uint8_t *scratch;              // one tile packed for the GPU
} water_gpu_t;

// ---------------------------------------------------------------------------
// Tiles in the GPU's order (contiguous) from and to the CPU's rows

static void pack_tile(const water_t *water, const void *source, size_t element_size, int32_t tile, void *destination)
{
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

	for (int32_t y = 0; y < WATER_TILE_SIZE; y++)
	{
		SDL_memcpy((uint8_t *)destination + (size_t)y * WATER_TILE_SIZE * element_size,
			(const uint8_t *)source + ((size_t)(start_y + y) * (size_t)water->size + (size_t)start_x) * element_size,
			WATER_TILE_SIZE * element_size);
	}
}

static void unpack_tile(const water_t *water, const void *source, size_t element_size, int32_t tile, void *destination)
{
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

	for (int32_t y = 0; y < WATER_TILE_SIZE; y++)
	{
		SDL_memcpy((uint8_t *)destination + ((size_t)(start_y + y) * (size_t)water->size + (size_t)start_x) * element_size,
			(const uint8_t *)source + (size_t)y * WATER_TILE_SIZE * element_size, WATER_TILE_SIZE * element_size);
	}
}

static uint32_t tile_offset(int32_t tile, size_t element_size)
{
	return (uint32_t)((size_t)tile * TILE_CELLS * element_size);
}

#if WATER_FLUX_BITS != 16
// Half float bits to a float (the CPU's copy keeps 32 bit fluxes here)
static float half_to_float(uint16_t half)
{
	uint32_t sign = (uint32_t)(half & 0x8000u) << 16;
	uint32_t exponent = (half >> 10) & 0x1Fu;
	uint32_t mantissa = half & 0x3FFu;
	uint32_t bits;
	float value;

	if (exponent == 0)
	{
		value = (float)mantissa * (1.0f / 16777216.0f);
		return sign != 0 ? -value : value;
	}
	if (exponent == 31)
	{
		bits = sign | 0x7F800000u | (mantissa << 13);
	}
	else
	{
		bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
	}
	SDL_memcpy(&value, &bits, sizeof(value));
	return value;
}
#endif

// ---------------------------------------------------------------------------
// The CPU side

bool water_gpu_init(water_t *water)
{
	water_gpu_t *state = SDL_calloc(1, sizeof(water_gpu_t));
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;

	if (state == NULL)
	{
		return false;
	}
	water->gpu = state;
	state->tile_count = tile_count;
	state->cell_count = (uint32_t)water->size * (uint32_t)water->size;
	state->tile_edited = SDL_calloc((size_t)tile_count, 1);
	state->tile_needs_download = SDL_calloc((size_t)tile_count, 1);
	// (all ones: no tile's first words are the same as these)
	state->tile_words_last = SDL_malloc(sizeof(uint32_t) * TILE_WORDS * (size_t)tile_count);
	if (state->tile_words_last != NULL)
	{
		SDL_memset(state->tile_words_last, 0xFF, sizeof(uint32_t) * TILE_WORDS * (size_t)tile_count);
	}
	state->tile_downloaded_ticks = SDL_calloc((size_t)tile_count, sizeof(uint64_t));
	state->ground_requested = SDL_calloc((size_t)tile_count, 1);
	state->tile_terrain_downloaded_ticks = SDL_calloc((size_t)tile_count, sizeof(uint64_t));
	state->tile_edit_serial = SDL_calloc((size_t)tile_count, sizeof(uint32_t));
	state->chosen_tiles = SDL_calloc((size_t)tile_count, sizeof(int32_t));
	state->tile_terrain_needs_download = SDL_calloc((size_t)tile_count, 1);
	state->tile_terrain_serial = SDL_calloc((size_t)tile_count, sizeof(uint32_t));
	state->scratch = SDL_malloc(TILE_CELLS * 8);
	state->upload_all = true;
	return state->tile_edited != NULL && state->tile_needs_download != NULL && state->tile_edit_serial != NULL && state->chosen_tiles != NULL
		&& state->tile_downloaded_ticks != NULL && state->tile_terrain_downloaded_ticks != NULL && state->ground_requested != NULL
		&& state->tile_terrain_needs_download != NULL && state->tile_terrain_serial != NULL && state->scratch != NULL;
}

void water_gpu_release(water_t *water)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL)
	{
		return;
	}
	if (state->ready && gpu.device != NULL)
	{
		SDL_GPUComputePipeline *pipelines[] = { state->control_pipeline, state->tiles_pipeline, state->fluxes_pipeline,
			state->update_list_pipeline, state->depths_pipeline, state->finish_pipeline, state->sources_pipeline, state->cells_pipeline,
			state->render_pipeline, state->erode_pipeline, state->collapse_pipeline, state->ground_pipeline };

		gpu_submit_work(NULL);
		SDL_WaitForGPUIdle(gpu.device);
		if (state->step_fence != NULL)
		{
			SDL_ReleaseGPUFence(gpu.device, state->step_fence);
		}
		for (int i = 0; i < DOWNLOAD_SLOTS; i++)
		{
			if (state->downloads[i].fence != NULL)
			{
				SDL_ReleaseGPUFence(gpu.device, state->downloads[i].fence);
			}
			if (state->downloads[i].transfer != NULL)
			{
				SDL_ReleaseGPUTransferBuffer(gpu.device, state->downloads[i].transfer);
			}
		}
		for (size_t i = 0; i < SDL_arraysize(pipelines); i++)
		{
			if (pipelines[i] != NULL)
			{
				SDL_ReleaseGPUComputePipeline(gpu.device, pipelines[i]);
			}
		}
	}
	if (gpu.device != NULL)
	{
		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			if (state->buffers[i] != NULL)
			{
				SDL_ReleaseGPUBuffer(gpu.device, state->buffers[i]);
			}
		}
		if (state->obstacle_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, state->obstacle_buffer);
		}
		if (state->soil_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, state->soil_buffer);
		}
		if (state->command_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, state->command_buffer);
		}
		if (state->argument_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, state->argument_buffer);
		}
		if (state->depth_texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, state->depth_texture);
		}
		if (state->ice_stand_in != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, state->ice_stand_in);
		}
		if (state->flow_texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, state->flow_texture);
		}
	}
	for (int i = 0; i < DOWNLOAD_SLOTS; i++)
	{
		SDL_free(state->downloads[i].tiles);
	}
	SDL_free(state->tile_edited);
	SDL_free(state->tile_needs_download);
	SDL_free(state->tile_words_last);
	SDL_free(state->tile_downloaded_ticks);
	SDL_free(state->ground_requested);
	SDL_free(state->tile_terrain_downloaded_ticks);
	SDL_free(state->tile_edit_serial);
	SDL_free(state->chosen_tiles);
	SDL_free(state->tile_terrain_needs_download);
	SDL_free(state->tile_terrain_serial);
	SDL_free(state->wake_commands);
	SDL_free(state->scratch);
	SDL_free(state);
	water->gpu = NULL;
}

void water_gpu_note_edit(water_t *water, int32_t tile)
{
	water_gpu_t *state = water->gpu;

	if (state != NULL && tile >= 0 && tile < state->tile_count)
	{
		state->tile_edited[tile] = 1;
		state->any_edited = true;
		state->tile_edit_serial[tile] = ++state->edit_serial;
	}
}

void water_gpu_note_wake(water_t *water, int32_t tile, int reason)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL || tile < 0 || tile >= state->tile_count)
	{
		return;
	}
	if (state->wake_count == state->wake_capacity)
	{
		int32_t capacity = SDL_max(64, state->wake_capacity * 2);
		int32_t *grown = SDL_realloc(state->wake_commands, sizeof(int32_t) * 2 * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		state->wake_commands = grown;
		state->wake_capacity = capacity;
	}
	state->wake_commands[2 * state->wake_count] = tile;
	state->wake_commands[2 * state->wake_count + 1] = reason;
	state->wake_count++;
}

void water_gpu_note_ground(water_t *water, int32_t tile)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL || tile < 0 || tile >= state->tile_count || state->ground_requested[tile])
	{
		return;
	}
	state->ground_requested[tile] = 1;
	water_gpu_note_wake(water, tile, WAKE_GROUND_ONLY);
}

void water_gpu_set_ground_targets(water_t *water, const water_ground_targets_t *targets)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL)
	{
		return;
	}
	if (targets == NULL || targets->height_texture == NULL || targets->normal_texture == NULL || targets->surface_texture == NULL)
	{
		state->ground_set = false;
		state->ground_live = false;
		return;
	}
	// New textures: every tile into them, at once
	if (!state->ground_set || targets->height_texture != state->ground.height_texture || targets->normal_texture != state->ground.normal_texture
		|| targets->surface_texture != state->ground.surface_texture || targets->normal_reach != state->ground.normal_reach)
	{
		state->ground_all_pending = true;
		state->ground_live = false;
	}
	state->ground = *targets;
	state->ground_set = true;
}

bool water_gpu_ground_live(const water_t *water)
{
	const water_gpu_t *state = water->gpu;

	return state != NULL && state->ready && !state->failed && state->ground_pipeline != NULL && state->ground_set && state->ground_live;
}

void water_gpu_ground_wetness(water_t *water, int32_t tile, const uint8_t *wetness)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL || !state->ready || state->failed || state->ground_pipeline == NULL || tile < 0 || tile >= state->tile_count)
	{
		return;
	}
	gpu_upload_buffer(state->buffers[BUFFER_MOISTURE], tile_offset(tile, 1), wetness, TILE_CELLS);
	water_gpu_note_ground(water, tile);
}

void water_gpu_note_clear(water_t *water)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL)
	{
		return;
	}
	state->clear_pending = true;
	state->restore_pending = false;
	state->any_edited = false;
	state->wake_count = 0;
	SDL_memset(state->tile_edited, 0, (size_t)state->tile_count);
	SDL_memset(state->tile_needs_download, 0, (size_t)state->tile_count);
	// Copies in flight show the water before the clear
	state->generation++;
	state->edit_serial++;
}

void water_gpu_note_restore(water_t *water)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL)
	{
		return;
	}
	water_gpu_note_clear(water);
	state->restore_pending = true;
}

// ---------------------------------------------------------------------------
// The GPU's resources

static bool create_resources(water_t *water)
{
	water_gpu_t *state = water->gpu;
	SDL_GPUBufferUsageFlags read_write = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
	uint32_t cells = state->cell_count;
	uint32_t element_sizes[BUFFER_COUNT] = { 4, 4, 8, 8, 4, 4, 8, 0 };
	uint32_t control_words = HEADER_WORDS + (uint32_t)state->tile_count * (TILE_WORDS + 3);
	gpu_compute_layout_t layout = { 0, 3, BUFFER_COUNT, 1, 64, 0, 0 };
	bool ok = true;

	for (int i = 0; i < BUFFER_COUNT; i++)
	{
		// (the moisture: a byte a cell, the CPU's wetness for the terrain's
		// textures)
		uint32_t size = i == BUFFER_CONTROL ? control_words * 4 : i == BUFFER_MOISTURE ? (cells + 3u) & ~3u : cells * element_sizes[i];

		state->buffers[i] = gpu_create_buffer(read_write, size);
		ok = ok && state->buffers[i] != NULL;
	}
	state->argument_buffer = gpu_create_buffer(read_write | SDL_GPU_BUFFERUSAGE_INDIRECT, ARGUMENT_WORDS * 4);
	ok = ok && state->argument_buffer != NULL;
	state->obstacle_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, cells * 2);
	state->soil_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, cells);
	// Springs first, then tiles to wake
	state->command_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
		(uint32_t)(sizeof(int32_t) * 4 * (WATER_MAX_SPRINGS + (size_t)state->tile_count)));
	// The depth after the last step and after the one before (the renderer
	// blends between them)
	state->depth_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, (uint32_t)water->size,
		(uint32_t)water->size, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
	state->flow_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)water->size,
		(uint32_t)water->size, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
	ok = ok && state->obstacle_buffer != NULL && state->soil_buffer != NULL && state->command_buffer != NULL && state->depth_texture != NULL
		&& state->flow_texture != NULL;
	// (no ice: a texel of none)
	state->ice_stand_in = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 1, 1, 1, 1,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (state->ice_stand_in != NULL)
	{
		uint16_t none[4] = { 0, 0, 0, 0 };

		gpu_upload_texture(state->ice_stand_in, 0, 0, 0, 0, 1, 1, none, sizeof(none));
	}
	ok = ok && state->ice_stand_in != NULL;

	{
		// Only the control words and the arguments
		gpu_compute_layout_t control_layout = { 0, 0, 2, 1, 1, 0, 0 };

		state->control_pipeline = gpu_load_compute("water_control.comp", &control_layout);
	}
	layout.threads_x = 64;
	state->tiles_pipeline = gpu_load_compute("water_tiles.comp", &layout);
	state->update_list_pipeline = gpu_load_compute("water_update_list.comp", &layout);
	state->finish_pipeline = gpu_load_compute("water_finish.comp", &layout);
	state->sources_pipeline = gpu_load_compute("water_sources.comp", &layout);
	layout.threads_x = 256;
	state->collapse_pipeline = gpu_load_compute("water_collapse.comp", &layout);
	state->erode_pipeline = gpu_load_compute("water_erode.comp", &layout);
	// (the ice, a sampler before the read only buffers)
	layout.samplers = 1;
	state->fluxes_pipeline = gpu_load_compute("water_fluxes.comp", &layout);
	layout.samplers = 0;
	state->depths_pipeline = gpu_load_compute("water_depths.comp", &layout);
	state->cells_pipeline = gpu_load_compute("water_cells.comp", &layout);
	layout.readwrite_storage_textures = 2;
	state->render_pipeline = gpu_load_compute("water_render.comp", &layout);
	layout.readwrite_storage_textures = 3;
	state->ground_pipeline = gpu_load_compute("water_ground.comp", &layout);
	if (state->ground_pipeline == NULL)
	{
		// (the terrain's textures then come from the CPU, as before)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Water: no water_ground.comp; the terrain is drawn from the CPU's copy");
	}
	ok = ok && state->control_pipeline != NULL && state->tiles_pipeline != NULL && state->update_list_pipeline != NULL
		&& state->finish_pipeline != NULL && state->sources_pipeline != NULL && state->fluxes_pipeline != NULL && state->depths_pipeline != NULL
		&& state->cells_pipeline != NULL && state->render_pipeline != NULL
		&& state->erode_pipeline != NULL && state->collapse_pipeline != NULL;

	for (int i = 0; i < DOWNLOAD_SLOTS && ok; i++)
	{
		SDL_GPUTransferBufferCreateInfo info;

		SDL_zero(info);
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = control_words * 4 + DOWNLOAD_BUDGET_TILES * DOWNLOAD_TILE_BYTES + DOWNLOAD_TERRAIN_TILES * DOWNLOAD_TERRAIN_BYTES;
		state->downloads[i].transfer = SDL_CreateGPUTransferBuffer(gpu.device, &info);
		state->downloads[i].capacity = info.size;
		state->downloads[i].tiles = SDL_calloc(DOWNLOAD_BUDGET_TILES + DOWNLOAD_TERRAIN_TILES, sizeof(int32_t));
		ok = state->downloads[i].transfer != NULL && state->downloads[i].tiles != NULL;
	}
	if (ok)
	{
		// The control words start at zero (every tile dry); the cells are
		// cleared by the first sync
		uint32_t *zeros = SDL_calloc(control_words, sizeof(uint32_t));

		if (zeros == NULL)
		{
			return false;
		}
		gpu_upload_buffer(state->buffers[BUFFER_CONTROL], 0, zeros, (size_t)control_words * 4);
		SDL_free(zeros);
		SDL_Log("Water on the GPU: %.0f MB for %dx%d cells", (double)((size_t)cells * (4 + 4 + 8 + 8 + 4 + 8 + 2 + 1)) / (1024.0 * 1024.0),
			water->size, water->size);
	}
	return ok;
}

// A float as a half (for the flow texture), rounded to the nearest
static uint16_t float_to_half(float value)
{
	union
	{
		float f;
		uint32_t u;
	} bits = { value };
	uint32_t sign = (bits.u >> 16) & 0x8000u;
	int32_t exponent = (int32_t)((bits.u >> 23) & 0xffu) - 127 + 15;
	uint32_t mantissa = bits.u & 0x7fffffu;

	if (exponent <= 0)
	{
		// (too small: zero; the flow has no use for subnormals)
		return (uint16_t)sign;
	}
	if (exponent >= 31)
	{
		return (uint16_t)(sign | 0x7bffu);
	}
	mantissa += 0x1000u;
	if (mantissa & 0x800000u)
	{
		mantissa = 0;
		exponent++;
		if (exponent >= 31)
		{
			return (uint16_t)(sign | 0x7bffu);
		}
	}
	return (uint16_t)(sign | ((uint32_t)exponent << 10) | (mantissa >> 13));
}

// A baked map's water (water_t.baked): no simulation, only the textures
// the renderer draws from, filled from the CPU's copy once (and again
// where it changes)
static bool create_baked_resources(water_t *water)
{
	water_gpu_t *state = water->gpu;

	// (cleared as color targets where the GPU can: then only the tiles
	// with water go up)
	SDL_GPUTextureUsageFlags clearable = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

	state->baked_clearable = SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, SDL_GPU_TEXTURETYPE_2D, clearable)
		&& SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTURETYPE_2D, clearable);
	state->depth_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, (uint32_t)water->size,
		(uint32_t)water->size, 1, 1, state->baked_clearable ? clearable : SDL_GPU_TEXTUREUSAGE_SAMPLER);
	state->flow_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)water->size,
		(uint32_t)water->size, 1, 1, state->baked_clearable ? clearable : SDL_GPU_TEXTUREUSAGE_SAMPLER);
	state->ice_stand_in = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 1, 1, 1, 1,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (state->depth_texture == NULL || state->flow_texture == NULL || state->ice_stand_in == NULL)
	{
		return false;
	}
	{
		uint16_t none[4] = { 0, 0, 0, 0 };

		gpu_upload_texture(state->ice_stand_in, 0, 0, 0, 0, 1, 1, none, sizeof(none));
	}
	SDL_Log("Water on the GPU: baked, %.0f MB of textures for %dx%d cells", (double)((size_t)state->cell_count * 16) / (1024.0 * 1024.0),
		water->size, water->size);
	return true;
}

// One row of tiles of the baked water's textures, from the CPU's copy
static void upload_baked_strip(water_t *water, int32_t tile_y, float *depths, uint16_t *flows)
{
	water_gpu_t *state = water->gpu;
	int32_t size = water->size;

	for (int32_t y = 0; y < WATER_TILE_SIZE; y++)
	{
		int32_t cell_y = tile_y * WATER_TILE_SIZE + y;

		for (int32_t x = 0; x < size; x++)
		{
			size_t index = (size_t)cell_y * (size_t)size + (size_t)x;
			size_t texel = (size_t)y * (size_t)size + (size_t)x;
			float depth = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
			const uint8_t *flow = water->baked_flow[water_tile_index_of_cell(water, x, cell_y)];

			depths[texel * 2] = depth;
			depths[texel * 2 + 1] = depth;
			if (flow != NULL && depth > 0.0f)
			{
				flow += (y * WATER_TILE_SIZE + x % WATER_TILE_SIZE) * WATER_BAKED_FLOW_BYTES;
				flows[texel * 4] = float_to_half(water_baked_decode_velocity(flow[0]));
				flows[texel * 4 + 1] = float_to_half(water_baked_decode_velocity(flow[1]));
				flows[texel * 4 + 2] = float_to_half(water_baked_decode_amount(flow[2], WATER_BAKED_MUDDIEST));
				flows[texel * 4 + 3] = float_to_half(water_baked_decode_amount(flow[3], WATER_BAKED_FASTEST));
			}
			else
			{
				SDL_memset(&flows[texel * 4], 0, 4 * sizeof(uint16_t));
			}
		}
	}
	gpu_upload_texture(state->depth_texture, 0, 0, 0, (uint32_t)(tile_y * WATER_TILE_SIZE), (uint32_t)size, WATER_TILE_SIZE, depths,
		(size_t)size * WATER_TILE_SIZE * 2 * sizeof(float));
	gpu_upload_texture(state->flow_texture, 0, 0, 0, (uint32_t)(tile_y * WATER_TILE_SIZE), (uint32_t)size, WATER_TILE_SIZE, flows,
		(size_t)size * WATER_TILE_SIZE * 4 * sizeof(uint16_t));
}

// One tile of the baked water's textures, from the CPU's copy
static void upload_baked_tile(water_t *water, int32_t tile, float *depths, uint16_t *flows)
{
	water_gpu_t *state = water->gpu;
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;
	const uint8_t *flow = water->baked_flow[tile];

	for (int32_t cell = 0; cell < TILE_CELLS; cell++)
	{
		size_t index = (size_t)(start_y + cell / WATER_TILE_SIZE) * (size_t)water->size + (size_t)(start_x + cell % WATER_TILE_SIZE);
		float depth = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;

		depths[cell * 2] = depth;
		depths[cell * 2 + 1] = depth;
		if (flow != NULL && depth > 0.0f)
		{
			const uint8_t *bytes = flow + cell * WATER_BAKED_FLOW_BYTES;

			flows[cell * 4] = float_to_half(water_baked_decode_velocity(bytes[0]));
			flows[cell * 4 + 1] = float_to_half(water_baked_decode_velocity(bytes[1]));
			flows[cell * 4 + 2] = float_to_half(water_baked_decode_amount(bytes[2], WATER_BAKED_MUDDIEST));
			flows[cell * 4 + 3] = float_to_half(water_baked_decode_amount(bytes[3], WATER_BAKED_FASTEST));
		}
		else
		{
			SDL_memset(&flows[cell * 4], 0, 4 * sizeof(uint16_t));
		}
	}
	gpu_upload_texture(state->depth_texture, 0, 0, (uint32_t)start_x, (uint32_t)start_y, WATER_TILE_SIZE, WATER_TILE_SIZE, depths,
		TILE_CELLS * 2 * sizeof(float));
	gpu_upload_texture(state->flow_texture, 0, 0, (uint32_t)start_x, (uint32_t)start_y, WATER_TILE_SIZE, WATER_TILE_SIZE, flows,
		TILE_CELLS * 4 * sizeof(uint16_t));
}

// Both baked water textures to nothing, on the GPU (a render pass that only
// clears them)
static bool clear_baked_textures(water_t *water)
{
	water_gpu_t *state = water->gpu;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUColorTargetInfo targets[2];
	SDL_GPURenderPass *pass;

	if (!state->baked_clearable || (command_buffer = gpu_commands()) == NULL)
	{
		return false;
	}
	SDL_zero(targets);
	targets[0].texture = state->depth_texture;
	targets[1].texture = state->flow_texture;
	for (int i = 0; i < 2; i++)
	{
		targets[i].load_op = SDL_GPU_LOADOP_CLEAR;
		targets[i].store_op = SDL_GPU_STOREOP_STORE;
	}
	pass = SDL_BeginGPURenderPass(command_buffer, targets, 2, NULL);
	if (pass == NULL)
	{
		return false;
	}
	SDL_EndGPURenderPass(pass);
	return true;
}

static void baked_end_frame(water_t *water)
{
	water_gpu_t *state = water->gpu;
	bool all;
	float *depths;
	uint16_t *flows;
	uint64_t start = SDL_GetPerformanceCounter();
	int32_t strips = 0;

	if (state == NULL || state->failed || gpu.device == NULL)
	{
		return;
	}
	if (!state->ready)
	{
		state->ready = true;
		if (!create_baked_resources(water))
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Water: the GPU refused the baked water's textures; no water");
			state->failed = true;
			return;
		}
		state->upload_all = true;
	}
	all = state->upload_all || state->restore_pending || state->clear_pending;
	if (!all && !state->any_edited)
	{
		return;
	}
	depths = SDL_malloc((size_t)water->size * WATER_TILE_SIZE * 2 * sizeof(float));
	flows = SDL_malloc((size_t)water->size * WATER_TILE_SIZE * 4 * sizeof(uint16_t));
	if (depths != NULL && flows != NULL && (!all || clear_baked_textures(water)))
	{
		// All of it cleared, then the tiles with water up alone; or only
		// the tiles changed
		gpu_flush_uploads();
		for (int32_t tile = 0; tile < state->tile_count; tile++)
		{
			if (all ? water->baked_flow[tile] != NULL : state->tile_edited[tile] != 0)
			{
				upload_baked_tile(water, tile, depths, flows);
				strips++;
			}
		}
		state->upload_all = false;
		state->restore_pending = false;
		state->clear_pending = false;
		state->any_edited = false;
		SDL_memset(state->tile_edited, 0, (size_t)state->tile_count);
	}
	else if (depths != NULL && flows != NULL)
	{
		for (int32_t tile_y = 0; tile_y < water->tiles_per_side; tile_y++)
		{
			bool wanted = all;

			for (int32_t tile_x = 0; tile_x < water->tiles_per_side && !wanted; tile_x++)
			{
				wanted = state->tile_edited[tile_y * water->tiles_per_side + tile_x] != 0;
			}
			if (wanted)
			{
				upload_baked_strip(water, tile_y, depths, flows);
				strips++;
			}
		}
		state->upload_all = false;
		state->restore_pending = false;
		state->clear_pending = false;
		state->any_edited = false;
		SDL_memset(state->tile_edited, 0, (size_t)state->tile_count);
	}
	SDL_free(depths);
	SDL_free(flows);
	if (all)
	{
		SDL_Log("Water: the baked water's textures up, %d %s, %.0f ms", strips, state->baked_clearable ? "tiles with water (the rest cleared)" : "rows of tiles",
			1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency());
	}
}

static bool ensure_resources(water_t *water)
{
	water_gpu_t *state = water->gpu;

	// (baked water is not simulated: baked_end_frame)
	if (state == NULL || state->failed || gpu.device == NULL || water->baked)
	{
		return false;
	}
	if (!state->ready)
	{
		state->ready = true;
		if (!create_resources(water))
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Water: the GPU refused the simulation's resources; no water");
			state->failed = true;
			return false;
		}
		state->upload_all = true;
	}
	return true;
}

// One kernel, in a compute pass of its own (passes are what orders their
// reads and writes). groups 0 with indirect_word: the group count is in
// the control buffer at that word.
static void dispatch(water_t *water, SDL_GPUComputePipeline *pipeline, int32_t mode, uint32_t groups, int32_t indirect_word)
{
	water_gpu_t *state = water->gpu;
	SDL_GPUStorageBufferReadWriteBinding buffers[BUFFER_COUNT];
	SDL_GPUStorageTextureReadWriteBinding textures[3];
	SDL_GPUBuffer *read_only[3] = { state->obstacle_buffer, state->soil_buffer, state->command_buffer };
	bool render = pipeline == state->render_pipeline;
	bool ground = pipeline != NULL && pipeline == state->ground_pipeline;
	uint32_t texture_count = render ? 2 : (ground ? 3 : 0);
	bool control = pipeline == state->control_pipeline;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUComputePass *pass;

	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}
	SDL_zeroa(buffers);
	for (int i = 0; i < BUFFER_COUNT; i++)
	{
		buffers[i].buffer = state->buffers[i];
	}
	if (control)
	{
		buffers[0].buffer = state->buffers[BUFFER_CONTROL];
		buffers[1].buffer = state->argument_buffer;
	}
	SDL_zeroa(textures);
	textures[0].texture = state->depth_texture;
	textures[1].texture = state->flow_texture;
	if (ground)
	{
		textures[0].texture = state->ground.height_texture;
		textures[1].texture = state->ground.normal_texture;
		textures[2].texture = state->ground.surface_texture;
	}
	pass = SDL_BeginGPUComputePass(command_buffer, texture_count > 0 ? textures : NULL, texture_count, buffers, control ? 2 : BUFFER_COUNT);
	if (pass == NULL)
	{
		return;
	}
	SDL_BindGPUComputePipeline(pass, pipeline);
	if (!control)
	{
		SDL_BindGPUComputeStorageBuffers(pass, 0, read_only, 3);
	}
	if (pipeline == state->fluxes_pipeline)
	{
		SDL_GPUTextureSamplerBinding ice = { state->ice_texture != NULL ? state->ice_texture : state->ice_stand_in, gpu.nearest_clamp };

		SDL_BindGPUComputeSamplers(pass, 0, &ice, 1);
		state->parameters.ice[0] = state->ice_texture != NULL ? 1 : 0;
	}
	state->parameters.mode = mode;
	SDL_PushGPUComputeUniformData(command_buffer, 0, &state->parameters, sizeof(state->parameters));
	if (indirect_word >= 0)
	{
		SDL_DispatchGPUComputeIndirect(pass, state->argument_buffer, (uint32_t)indirect_word * 4);
	}
	else if (groups > 0)
	{
		SDL_DispatchGPUCompute(pass, groups, 1, 1);
	}
	SDL_EndGPUComputePass(pass);
}

static uint32_t tile_thread_groups(const water_gpu_t *state)
{
	return (uint32_t)(state->tile_count + 63) / 64;
}

static void fill_grid_parameters(water_t *water)
{
	water_gpu_parameters_t *parameters = &water->gpu->parameters;

	parameters->map_size = water->size;
	parameters->tiles_per_side = water->tiles_per_side;
	parameters->tile_count = water->gpu->tile_count;
	parameters->cell_size = water->cell_size_meters;
	parameters->step_number = (int32_t)water->step_count;
	parameters->speed_depth_meters = water->speed_depth_meters;
}

// ---------------------------------------------------------------------------
// The CPU's changes going up

static void upload_tile_array(water_t *water, SDL_GPUBuffer *buffer, const void *source, size_t element_size, int32_t tile)
{
	pack_tile(water, source, element_size, tile, water->gpu->scratch);
	gpu_upload_buffer(buffer, tile_offset(tile, element_size), water->gpu->scratch, TILE_CELLS * element_size);
}

static void upload_water_tile(water_t *water, int32_t tile)
{
	water_gpu_t *state = water->gpu;

	upload_tile_array(water, state->buffers[BUFFER_DEPTH], water->depth, sizeof(int32_t), tile);
	upload_tile_array(water, state->buffers[BUFFER_SEDIMENT], water->sediment, sizeof(int32_t), tile);
}

static void upload_terrain_tile(water_t *water, const terrain_t *terrain, int32_t tile)
{
	water_gpu_t *state = water->gpu;
	int32_t *layers = (int32_t *)(void *)state->scratch;
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

	upload_tile_array(water, state->buffers[BUFFER_HEIGHT], terrain->heights, sizeof(int32_t), tile);
	upload_tile_array(water, state->soil_buffer, terrain->soil_types, sizeof(uint8_t), tile);
	for (int32_t y = 0; y < WATER_TILE_SIZE; y++)
	{
		for (int32_t x = 0; x < WATER_TILE_SIZE; x++)
		{
			size_t index = (size_t)(start_y + y) * (size_t)water->size + (size_t)(start_x + x);

			layers[2 * (y * WATER_TILE_SIZE + x)] = terrain->bedrock_heights[index];
			layers[2 * (y * WATER_TILE_SIZE + x) + 1] = terrain->silt_thickness[index];
		}
	}
	gpu_upload_buffer(state->buffers[BUFFER_LAYER], tile_offset(tile, 8), layers, TILE_CELLS * 8);
	// A copy of the ground asked for before this is older than the CPU's
	state->tile_terrain_serial[tile] = ++state->edit_serial;
	state->tile_terrain_needs_download[tile] = 0;
	// Its textures again, and its neighbors' (their normals reach into it)
	for (int32_t dy = -1; dy <= 1; dy++)
	{
		for (int32_t dx = -1; dx <= 1; dx++)
		{
			int32_t neighbor_x = tile % water->tiles_per_side + dx;
			int32_t neighbor_y = tile / water->tiles_per_side + dy;

			if (neighbor_x >= 0 && neighbor_y >= 0 && neighbor_x < water->tiles_per_side && neighbor_y < water->tiles_per_side)
			{
				water_gpu_note_ground(water, neighbor_y * water->tiles_per_side + neighbor_x);
			}
		}
	}
}

static bool tile_has_water(const water_t *water, int32_t tile)
{
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

	for (int32_t y = start_y; y < start_y + WATER_TILE_SIZE; y++)
	{
		const size_t row = (size_t)y * (size_t)water->size;

		for (int32_t x = start_x; x < start_x + WATER_TILE_SIZE; x++)
		{
			if (water->depth[row + (size_t)x] != 0 || water->sediment[row + (size_t)x] != 0)
			{
				return true;
			}
		}
	}
	return false;
}

// Everything the CPU changed since the last time goes up, in order: a
// clear, then the tiles, then the wake ups
static void send_changes(water_t *water, const terrain_t *terrain)
{
	water_gpu_t *state = water->gpu;
	bool restore = state->restore_pending;

	fill_grid_parameters(water);
	if (terrain != NULL && terrain->size == water->size)
	{
		// The copies of the ground land in it (water_gpu_poll)
		state->terrain = (terrain_t *)terrain;
	}
	if (state->upload_all || state->clear_pending)
	{
		dispatch(water, state->cells_pipeline, CELLS_CLEAR, (uint32_t)state->tile_count, -1);
		dispatch(water, state->control_pipeline, CONTROL_RESET_COUNTERS | CONTROL_RESET_TILE_LISTS | CONTROL_RESET_LIST, 1, -1);
		state->clear_pending = false;
	}
	if (state->upload_all || restore)
	{
		// The CPU's water as it is: every tile that holds some
		for (int32_t tile = 0; tile < state->tile_count; tile++)
		{
			if (tile_has_water(water, tile))
			{
				state->tile_edited[tile] = 1;
				state->any_edited = true;
			}
		}
		state->restore_pending = false;
	}
	if (state->upload_all)
	{
		for (int32_t tile = 0; tile < state->tile_count; tile++)
		{
			water->tile_obstacles_dirty[tile] = true;
		}
		water->any_obstacles_dirty = true;
	}
	if (state->any_edited)
	{
		for (int32_t tile = 0; tile < state->tile_count; tile++)
		{
			if (state->tile_edited[tile])
			{
				upload_water_tile(water, tile);
				state->tile_edited[tile] = 0;
				water_gpu_note_wake(water, tile, WATER_WAKE_EDIT);
			}
		}
		state->any_edited = false;
	}
	if (water->any_obstacles_dirty)
	{
		for (int32_t tile = 0; tile < state->tile_count; tile++)
		{
			if (water->tile_obstacles_dirty[tile])
			{
				upload_tile_array(water, state->obstacle_buffer, water->obstacle_centimeters, sizeof(uint16_t), tile);
				water->tile_obstacles_dirty[tile] = false;
			}
		}
		water->any_obstacles_dirty = false;
	}
	if (terrain != NULL && terrain->size == water->size && (state->upload_all || terrain->any_chunk_sim_dirty))
	{
		terrain_t *changed = (terrain_t *)terrain;

		for (int32_t tile = 0; tile < state->tile_count; tile++)
		{
			if (state->upload_all || changed->chunks[tile].sim_dirty)
			{
				upload_terrain_tile(water, terrain, tile);
				changed->chunks[tile].sim_dirty = false;
			}
		}
		changed->any_chunk_sim_dirty = false;
		state->upload_all = false;
	}
	if (state->wake_count > 0)
	{
		int32_t (*commands)[4] = SDL_calloc((size_t)state->wake_count, sizeof(int32_t) * 4);

		if (commands != NULL)
		{
			for (int32_t i = 0; i < state->wake_count; i++)
			{
				commands[i][0] = state->wake_commands[2 * i];
				commands[i][1] = state->wake_commands[2 * i + 1];
			}
			for (int32_t first = 0; first < state->wake_count; first += state->tile_count)
			{
				int32_t count = SDL_min(state->wake_count - first, state->tile_count);

				gpu_upload_buffer(state->command_buffer, (uint32_t)(sizeof(int32_t) * 4 * WATER_MAX_SPRINGS), commands[first],
					sizeof(int32_t) * 4 * (size_t)count);
				state->parameters.command_first = WATER_MAX_SPRINGS;
				state->parameters.command_count = count;
				dispatch(water, state->sources_pipeline, SOURCE_WAKE, (uint32_t)(count + 63) / 64, -1);
			}
			SDL_free(commands);
		}
		state->wake_count = 0;
		SDL_memset(state->ground_requested, 0, (size_t)state->tile_count);
	}
}

// ---------------------------------------------------------------------------
// Steps

void water_gpu_brush(water_t *water, float center_x, float center_z, float radius_meters, float meters_per_second, float step_seconds)
{
	water_gpu_t *state = water->gpu;
	float radius_cells = radius_meters / water->cell_size_meters;
	float center_cell_x = center_x / water->cell_size_meters;
	float center_cell_y = center_z / water->cell_size_meters;
	int32_t minimum_x = clamp_int(floor_to_int(center_cell_x - radius_cells), 0, water->size - 1);
	int32_t minimum_y = clamp_int(floor_to_int(center_cell_y - radius_cells), 0, water->size - 1);
	int32_t maximum_x = clamp_int((int32_t)SDL_ceilf(center_cell_x + radius_cells), 0, water->size - 1);
	int32_t maximum_y = clamp_int((int32_t)SDL_ceilf(center_cell_y + radius_cells), 0, water->size - 1);
	uint32_t cell_count = (uint32_t)(maximum_x - minimum_x + 1) * (uint32_t)(maximum_y - minimum_y + 1);

	if (!ensure_resources(water))
	{
		return;
	}
	fill_grid_parameters(water);
	state->parameters.brush[0] = center_cell_x;
	state->parameters.brush[1] = center_cell_y;
	state->parameters.brush[2] = radius_cells;
	state->parameters.brush[3] = SDL_fabsf(meters_per_second) * step_seconds * (float)TERRAIN_HEIGHT_UNITS_PER_METER;
	state->parameters.brush_rectangle[0] = minimum_x;
	state->parameters.brush_rectangle[1] = minimum_y;
	state->parameters.brush_rectangle[2] = maximum_x - minimum_x + 1;
	state->parameters.brush_rectangle[3] = maximum_y - minimum_y + 1;
	state->parameters.brush_adding = meters_per_second > 0.0f ? 1 : 0;
	dispatch(water, state->sources_pipeline, SOURCE_BRUSH, (cell_count + 63) / 64, -1);
}

static void fill_flux_parameters(water_t *water, const water_settings_t *settings, const soil_settings_t *soil_settings, float substep_seconds)
{
	water_gpu_parameters_t *parameters = &water->gpu->parameters;
	water_flux_parameters_t flux;

	water_make_flux_parameters(water, settings, soil_settings, substep_seconds, &flux);
	parameters->step_seconds = flux.step_seconds;
	parameters->gravity_times_cell = flux.gravity_times_cell;
	parameters->pipe_depth_limit = flux.pipe_depth_limit;
	parameters->superfluid_depth = flux.superfluid_depth;
	parameters->shore_depth = flux.shore_depth;
	parameters->shore_damping_times_step = flux.shore_damping_times_step;
	parameters->asleep_wake_units = flux.asleep_wake_units;
	parameters->asleep_level_wake_units = flux.asleep_level_wake_units;
	parameters->friction_times_step = flux.friction_times_step;
	parameters->manning_times_step = flux.manning_times_step;
	parameters->cell_size = flux.cell_size;
	parameters->linear_damping_factor = flux.linear_damping_factor;
	parameters->cell_area = flux.cell_area;
	parameters->film_units = flux.film_units;
	parameters->meniscus_units = flux.meniscus_units;
	parameters->thin_units = flux.thin_units;
	parameters->lip_units = flux.lip_units;
	parameters->border_wake_units = flux.border_wake_units;
	parameters->units_per_flux = flux.units_per_flux;
	parameters->still_units = flux.still_units;
	parameters->depth_scaled_pipes = flux.depth_scaled_pipes ? 1 : 0;
	parameters->drain_edges = flux.drain_edges ? 1 : 0;
	parameters->silt_units = flux.silt_units;
	for (int i = 0; i < 4; i++)
	{
		parameters->friction_factors[i] = flux.friction_factor[i];
	}
	parameters->more_friction_factors[0] = flux.friction_factor[SOIL_SILT];
	parameters->sleep_change_units = water_sleep_change_units(settings, substep_seconds);
	parameters->deep_flow_units = (int32_t)(water->cell_size_meters * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->sleep_speed = settings->sleep_speed_meters_per_second;
	parameters->sleep_after_substeps = settings->sleep_after_substeps;
}

void water_gpu_step(water_t *water, const terrain_t *terrain, const water_settings_t *settings, const soil_settings_t *soil_settings,
	float step_seconds, int32_t substeps, int32_t spring_units[], int32_t rain_units, bool evaporate, float evaporation_seconds)
{
	water_gpu_t *state = water->gpu;
	int32_t spring_count = 0;

	if (!ensure_resources(water))
	{
		return;
	}
	send_changes(water, terrain);
	state->stepped_since_render = true;

	// Sources
	{
		int32_t commands[WATER_MAX_SPRINGS][4];

		for (int i = 0; i < water->spring_count; i++)
		{
			if (spring_units[i] > 0)
			{
				commands[spring_count][0] = water->springs[i].cell_x;
				commands[spring_count][1] = water->springs[i].cell_y;
				commands[spring_count][2] = spring_units[i];
				commands[spring_count][3] = 0;
				spring_count++;
			}
		}
		if (spring_count > 0)
		{
			gpu_upload_buffer(state->command_buffer, 0, commands, sizeof(int32_t) * 4 * (size_t)spring_count);
			state->parameters.command_first = 0;
			state->parameters.command_count = spring_count;
			dispatch(water, state->sources_pipeline, SOURCE_SPRINGS, (uint32_t)(spring_count * 9 + 63) / 64, -1);
		}
	}
	if (rain_units > 0)
	{
		state->parameters.rain_units = rain_units;
		dispatch(water, state->cells_pipeline, CELLS_RAIN, (uint32_t)state->tile_count, -1);
	}

	// (the tile lists' counts empty for the first substep; later ones have
	// them emptied by the arguments passes: one dispatch a substep saved)
	dispatch(water, state->control_pipeline, CONTROL_STEP_BEGIN | CONTROL_RESET_TILE_LISTS, 1, -1);
	fill_flux_parameters(water, settings, soil_settings, step_seconds / (float)substeps);
	for (int32_t substep = 0; substep < substeps; substep++)
	{
		// The active tiles; pass 1 on them (the update list emptied once
		// its count is read)
		dispatch(water, state->tiles_pipeline, TILES_ACTIVE, tile_thread_groups(state), -1);
		dispatch(water, state->control_pipeline, CONTROL_ARGUMENTS | CONTROL_RESET_UPDATE_AFTER, 1, -1);
		dispatch(water, state->fluxes_pipeline, 0, 0, ARGUMENT_ACTIVE_TILES);
		// The tiles taking part in pass 2: the active ones and the
		// neighbors they woke (the active list emptied for the next
		// substep: nothing after this reads it)
		dispatch(water, state->update_list_pipeline, 0, 0, ARGUMENT_ACTIVE_THREADS);
		dispatch(water, state->control_pipeline, CONTROL_ARGUMENTS | CONTROL_RESET_ACTIVE_AFTER, 1, -1);
		dispatch(water, state->depths_pipeline, 0, 0, ARGUMENT_UPDATE_TILES);
		// Their sleep
		dispatch(water, state->finish_pipeline, 0, 0, ARGUMENT_UPDATE_THREADS);
	}

	if (evaporate)
	{
		int32_t film_units = (int32_t)(settings->film_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
		int32_t thin_units = (int32_t)(settings->thin_water_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);

		state->parameters.film_units = film_units;
		state->parameters.thin_units = thin_units;
		state->parameters.evaporation_units = (int32_t)(settings->film_evaporation_millimeters_per_second * 0.001f * evaporation_seconds
			* (float)TERRAIN_HEIGHT_UNITS_PER_METER);
		state->parameters.thin_evaporation_units = thin_units <= film_units ? 0
			: (int32_t)(settings->thin_water_evaporation_millimeters_per_second * 0.001f * evaporation_seconds * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
		if (film_units <= 0)
		{
			state->parameters.evaporation_units = 0;
		}
		if (state->parameters.evaporation_units > 0 || state->parameters.thin_evaporation_units > 0)
		{
			dispatch(water, state->control_pipeline, CONTROL_RESET_LIST, 1, -1);
			dispatch(water, state->tiles_pipeline, TILES_WET, tile_thread_groups(state), -1);
			dispatch(water, state->control_pipeline, CONTROL_ARGUMENTS, 1, -1);
			dispatch(water, state->cells_pipeline, CELLS_EVAPORATE, 0, ARGUMENT_LIST_TILES);
			frame_profile_count(FRAME_EVAPORATIONS, 1);
		}
	}

	// Many steps without a frame (tests, the warmup): the work goes out
	// every few steps, and the CPU waits for the batch before last, so it
	// never runs far ahead of the GPU
	state->steps_since_submit++;
	if (state->steps_since_submit >= 8 && gpu.command_buffer == NULL)
	{
		SDL_GPUFence *fence = NULL;

		gpu_submit_work(&fence);
		if (state->step_fence != NULL)
		{
			SDL_WaitForGPUFences(gpu.device, true, &state->step_fence, 1);
			SDL_ReleaseGPUFence(gpu.device, state->step_fence);
		}
		state->step_fence = fence;
		state->steps_since_submit = 0;
	}
}

void water_gpu_erosion_step(water_t *water, const soil_settings_t *soil_settings, const water_settings_t *water_settings, float step_seconds,
	bool collapse, int64_t shadow_threshold_units)
{
	water_gpu_t *state = water->gpu;
	water_gpu_parameters_t *parameters;
	float units_per_millimeter = (float)TERRAIN_HEIGHT_UNITS_PER_METER * 0.001f;

	if (!ensure_resources(water) || !state->ready)
	{
		return;
	}
	parameters = &state->parameters;
	state->shadow_threshold_units = shadow_threshold_units;
	fill_grid_parameters(water);
	parameters->step_seconds = step_seconds;
	parameters->film_units = (int32_t)(water_settings->film_millimeters * units_per_millimeter);
	parameters->silt_units = SDL_max(1, (int32_t)(soil_settings->silt_visible_millimeters * units_per_millimeter));
	parameters->transport_capacity = soil_settings->transport_capacity;
	parameters->deposition_rate = soil_settings->deposition_rate;
	parameters->still_speed = water_settings->still_water_speed_meters_per_second;
	parameters->still_deposition_rate = SDL_max(0.0f, water_settings->still_water_deposition_per_second);
	parameters->erosion_full_units = (int32_t)(SDL_max(0.0f, water_settings->erosion_minimum_depth_millimeters) * units_per_millimeter);
	parameters->erosion_start_units = parameters->erosion_full_units / 2;
	parameters->minimum_soil_units = (int32_t)(soil_settings->minimum_soil_millimeters * units_per_millimeter);
	// Banks are wet with water this deep at their foot or on them
	parameters->submerged_units = (int32_t)(0.05f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
	parameters->erosion_smoothing = soil_settings->erosion_smoothing;
	parameters->wet_critical_speed_factor = soil_settings->wet_critical_speed_factor;
	for (int i = 0; i < 4; i++)
	{
		parameters->erodibilities[i] = soil_settings->soils[i].erodibility;
		parameters->critical_speeds[i] = soil_settings->soils[i].critical_speed_meters_per_second;
		parameters->talus_slopes[i] = soil_settings->soils[i].wet_talus_slope;
	}
	parameters->silt_soil[0] = soil_settings->soils[SOIL_SILT].erodibility;
	parameters->silt_soil[1] = soil_settings->soils[SOIL_SILT].critical_speed_meters_per_second;
	parameters->silt_soil[2] = soil_settings->soils[SOIL_SILT].wet_talus_slope;
	parameters->silt_soil[3] = 0.0f;

	// Erosion and deposition on the tiles with flowing water, sediment, or
	// a recent change
	dispatch(water, state->control_pipeline, CONTROL_RESET_LIST, 1, -1);
	dispatch(water, state->tiles_pipeline, TILES_EROSION, tile_thread_groups(state), -1);
	dispatch(water, state->control_pipeline, CONTROL_ARGUMENTS, 1, -1);
	dispatch(water, state->erode_pipeline, 0, 0, ARGUMENT_LIST_TILES);

	// Bank collapse and smoothing, by checkerboard color: tiles of one
	// color are two apart, and a tile's checks reach one cell into its
	// neighbors, so no two threads touch the same cell
	if (collapse)
	{
		for (int32_t color = 0; color < 4; color++)
		{
			parameters->collapse_color = color;
			dispatch(water, state->control_pipeline, CONTROL_RESET_LIST, 1, -1);
			dispatch(water, state->tiles_pipeline, TILES_EROSION_COLOR, tile_thread_groups(state), -1);
			dispatch(water, state->control_pipeline, CONTROL_ARGUMENTS, 1, -1);
			dispatch(water, state->collapse_pipeline, 0, 0, ARGUMENT_LIST_TILES);
		}
	}
}

// ---------------------------------------------------------------------------
// The copies back

static uint32_t statistics_bytes(const water_gpu_t *state)
{
	return (uint32_t)(HEADER_WORDS + state->tile_count * TILE_WORDS) * 4;
}

static uint64_t wide(const uint32_t *words)
{
	return (uint64_t)words[0] | ((uint64_t)words[1] << 32);
}

static float float_of_bits(uint32_t bits)
{
	float value;

	SDL_memcpy(&value, &bits, sizeof(value));
	return value;
}

static void apply_statistics(water_t *water, const uint32_t *words)
{
	water_gpu_t *state = water->gpu;
	const uint32_t *counters = &words[HEADER_COUNTERS];
	int64_t added = (int64_t)wide(&counters[2 * COUNTER_ADDED]);
	int64_t removed = (int64_t)wide(&counters[2 * COUNTER_REMOVED]);
	int64_t drained = (int64_t)wide(&counters[2 * COUNTER_DRAINED]);
	int64_t evaporated = (int64_t)wide(&counters[2 * COUNTER_EVAPORATED]);
	int64_t discarded = (int64_t)wide(&counters[2 * COUNTER_DISCARDED]);
	int64_t corrections = (int64_t)wide(&counters[2 * COUNTER_CORRECTIONS]);
	bool quiet;

	// The balance: total = added - removed - drained - evaporated (the
	// trickles thrown away count as evaporated; a cell clamped at zero
	// created water, which counts as added)
	water->added_units += added + corrections;
	water->removed_units += removed;
	water->drained_units += drained;
	water->evaporated_units += evaporated + discarded;
	water->discarded_units += discarded;
	water->negative_depth_corrections += corrections;
	water->total_units += added + corrections - removed - drained - evaporated - discarded;
	water->sediment_drained_units += (int64_t)wide(&counters[2 * COUNTER_SEDIMENT_DRAINED]);
	water->sediment_corrections += (int64_t)wide(&counters[2 * COUNTER_SEDIMENT_CORRECTIONS]);
	water->eroded_units += (int64_t)wide(&counters[2 * COUNTER_ERODED]);
	water->deposited_units += (int64_t)wide(&counters[2 * COUNTER_DEPOSITED]);
	water->collapsed_units += (int64_t)wide(&counters[2 * COUNTER_COLLAPSED]);
	water->kept_awake_by_flow += (int64_t)wide(&counters[2 * COUNTER_KEPT_AWAKE_BY_FLOW]);
	water->kept_awake_by_level_change += (int64_t)wide(&counters[2 * COUNTER_KEPT_AWAKE_BY_LEVEL]);
	for (int i = 0; i < WATER_WAKE_REASON_COUNT; i++)
	{
		water->wake_counts[i] += (int64_t)wide(&counters[2 * (COUNTER_WAKE_FIRST + i)]);
	}
	if (corrections > 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Water: %lld units of negative depth corrected (bug in flux scaling?)", (long long)corrections);
	}
	water->fastest_flow_last_step = float_of_bits(words[HEADER_LAST_FASTEST_DEEP_SPEED]);
	water->simulated_tiles_last_step = (int32_t)words[HEADER_MOST_UPDATE_TILES];

	// (all the tiles' words as they were, and none flagged in what was kept
	// of them: nothing to copy out, told in one comparison)
	if (state->tile_words_last != NULL && state->statistics_quiet
		&& SDL_memcmp(&words[HEADER_WORDS], state->tile_words_last, sizeof(uint32_t) * TILE_WORDS * (size_t)state->tile_count) == 0)
	{
		return;
	}
	quiet = state->tile_words_last != NULL;
	for (int32_t tile = 0; tile < state->tile_count; tile++)
	{
		const uint32_t *tile_words = &words[HEADER_WORDS + tile * TILE_WORDS];

		// (the same words as last time, and no flag in them asking for
		// anything: what was copied out of them then still stands)
		if (state->tile_words_last != NULL)
		{
			uint32_t *last = &state->tile_words_last[(size_t)tile * TILE_WORDS];

			if (tile_words[TILE_FLAGS] == 0 && SDL_memcmp(tile_words, last, sizeof(uint32_t) * TILE_WORDS) == 0)
			{
				continue;
			}
			SDL_memcpy(last, tile_words, sizeof(uint32_t) * TILE_WORDS);
		}
		quiet = false;
		water->tile_state[tile] = (uint8_t)tile_words[TILE_STATE];
		water->tile_idle_steps[tile] = (uint8_t)SDL_min(tile_words[TILE_IDLE_STEPS], 255u);
		water->tile_total_depth[tile] = (int64_t)wide(&tile_words[TILE_TOTAL_DEPTH]);
		water->tile_max_depth[tile] = (int32_t)tile_words[TILE_MAX_DEPTH];
		water->tile_steep_cells[tile] = (int32_t)tile_words[TILE_STEEP_CELLS];
		water->steep_cells_known = true;
		water->tile_max_speed[tile] = float_of_bits(tile_words[TILE_MAX_SPEED]);
		water->tile_max_deep_speed[tile] = float_of_bits(tile_words[TILE_MAX_DEEP_SPEED]);
		water->tile_max_change[tile] = (int32_t)tile_words[TILE_MAX_CHANGE];
		water->tile_sediment[tile] = (int64_t)wide(&tile_words[TILE_SEDIMENT]);
		water->tile_wake_reason[tile] = (uint8_t)tile_words[TILE_WAKE_REASON];
		water->tile_wake_step[tile] = (int64_t)tile_words[TILE_WAKE_STEP];
		if ((tile_words[TILE_FLAGS] & TILE_FLAG_CPU_DIRTY) != 0)
		{
			state->tile_needs_download[tile] = 1;
		}
		if ((tile_words[TILE_FLAGS] & TILE_FLAG_TERRAIN_DIRTY) != 0)
		{
			state->tile_terrain_needs_download[tile] = 1;
		}
	}
	state->statistics_quiet = quiet;
}

static void apply_tile(water_t *water, const uint8_t *data, int32_t tile)
{
	const int32_t *depths = (const int32_t *)(const void *)data;
	const int32_t *sediments = depths + TILE_CELLS;
	const uint16_t *halves = (const uint16_t *)(const void *)(sediments + TILE_CELLS);
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

	unpack_tile(water, depths, sizeof(int32_t), tile, water->depth);
	unpack_tile(water, sediments, sizeof(int32_t), tile, water->sediment);
	for (int32_t y = 0; y < WATER_TILE_SIZE; y++)
	{
		size_t row = (size_t)(start_y + y) * (size_t)water->size + (size_t)start_x;

#if WATER_FLUX_BITS == 16
		// A direction's row at a time: one stream read and one written,
		// which the compiler can do several cells at once (cell by cell,
		// four arrays were written in turn)
		for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
		{
			const uint16_t *source = &halves[4 * (y * WATER_TILE_SIZE) + k];
			water_flux_t *row_flux = &water->flux[k][row];

			for (int32_t x = 0; x < WATER_TILE_SIZE; x++)
			{
				// (the half's bits as they are)
				SDL_memcpy(&row_flux[x], &source[4 * x], sizeof(uint16_t));
			}
		}
#else
		for (int32_t x = 0; x < WATER_TILE_SIZE; x++)
		{
			const uint16_t *cell = &halves[4 * (y * WATER_TILE_SIZE + x)];

			for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
			{
				water->flux[k][row + (size_t)x] = half_to_float(cell[k]);
			}
		}
#endif
	}
}

// The ground erosion changed: heights, bedrock and silt into the CPU's
// terrain, whose chunks then follow (mesh, physics). Returns the largest
// height change.
static int32_t apply_terrain_tile(water_t *water, const uint8_t *data, int32_t tile)
{
	terrain_t *terrain = water->gpu->terrain;
	const int32_t *heights = (const int32_t *)(const void *)data;
	const int32_t *layers = heights + TILE_CELLS;
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t max_change = 0;
	int32_t max_lowering = 0;

	for (int32_t y = 0; y < WATER_TILE_SIZE; y++)
	{
		size_t row = (size_t)(start_y + y) * (size_t)water->size + (size_t)start_x;

		for (int32_t x = 0; x < WATER_TILE_SIZE; x++)
		{
			int32_t local = y * WATER_TILE_SIZE + x;
			int32_t change = heights[local] - terrain->heights[row + (size_t)x];

			max_change = SDL_max(max_change, SDL_abs(change));
			max_lowering = SDL_max(max_lowering, -change);
			terrain->heights[row + (size_t)x] = heights[local];
			terrain->bedrock_heights[row + (size_t)x] = layers[2 * local];
			terrain->silt_thickness[row + (size_t)x] = layers[2 * local + 1];
		}
	}
	if (max_change > 0)
	{
		terrain_chunk_t *chunk = &terrain->chunks[tile];

		terrain_mark_cells_eroded(terrain, start_x, start_y, start_x + WATER_TILE_SIZE - 1, start_y + WATER_TILE_SIZE - 1);
		frame_profile_count(FRAME_GROUND_TILES, 1);
		// The objects on it sink into the ground a little: they are put
		// back on it only once erosion could have uncovered their feet
		// (only by what it wore away: silt laid down buries them where
		// they stand, none to look at; objects_follow_terrain)
		chunk->objects_erosion_units += max_lowering;
		if (chunk->objects_erosion_units >= OBJECTS_SINK_UNITS)
		{
			chunk->objects_erosion_units = 0;
			chunk->objects_dirty = true;
			terrain->any_chunk_objects_dirty = true;
		}
		// Where the shadows may have to follow
		if (!terrain->eroded_rectangle_valid)
		{
			terrain->eroded_rectangle[0] = start_x;
			terrain->eroded_rectangle[1] = start_y;
			terrain->eroded_rectangle[2] = start_x + WATER_TILE_SIZE - 1;
			terrain->eroded_rectangle[3] = start_y + WATER_TILE_SIZE - 1;
			terrain->eroded_rectangle_valid = true;
		}
		else
		{
			terrain->eroded_rectangle[0] = SDL_min(terrain->eroded_rectangle[0], start_x);
			terrain->eroded_rectangle[1] = SDL_min(terrain->eroded_rectangle[1], start_y);
			terrain->eroded_rectangle[2] = SDL_max(terrain->eroded_rectangle[2], start_x + WATER_TILE_SIZE - 1);
			terrain->eroded_rectangle[3] = SDL_max(terrain->eroded_rectangle[3], start_y + WATER_TILE_SIZE - 1);
		}
	}
	return max_change;
}

static void apply_download(water_t *water, download_t *download)
{
	water_gpu_t *state = water->gpu;
	const uint8_t *mapped = SDL_MapGPUTransferBuffer(gpu.device, download->transfer, false);
	bool current = download->generation == state->generation;
	size_t offset = 0;
	int32_t ground_change = 0;

	if (mapped == NULL)
	{
		return;
	}
	if (download->has_statistics)
	{
		// A clear since this was asked for: it shows water that is gone
		if (current)
		{
			apply_statistics(water, (const uint32_t *)(const void *)mapped);
		}
		offset = statistics_bytes(state);
	}
	for (int32_t i = 0; i < download->tile_count; i++)
	{
		int32_t entry = download->tiles[i];
		int32_t tile = entry & ~TERRAIN_ENTRY;

		if ((entry & TERRAIN_ENTRY) != 0)
		{
			// The ground does not care about water clears; it must not
			// overwrite the CPU's edits waiting to go up, or newer ones
			if (state->terrain != NULL && !state->terrain->chunks[tile].sim_dirty && state->tile_terrain_serial[tile] <= download->serial)
			{
				ground_change = SDL_max(ground_change, apply_terrain_tile(water, mapped + offset, tile));
			}
			offset += DOWNLOAD_TERRAIN_BYTES;
		}
		else
		{
			// Not over a change of the CPU's made after this was asked for
			if (current && state->tile_edit_serial[tile] <= download->serial)
			{
				apply_tile(water, mapped + offset, tile);
				frame_profile_count(FRAME_WATER_TILES, 1);
			}
			offset += DOWNLOAD_TILE_BYTES;
		}
	}
	SDL_UnmapGPUTransferBuffer(gpu.device, download->transfer);
	// Erosion is slow and gradual: the near shadows follow only once the
	// ground has changed noticeably (the far map never: it does not show
	// centimeters). What counts is the largest change of each copy (about
	// a frame of erosion), not the sum over its tiles, which would pass
	// the threshold all the time.
	if (ground_change > 0 && state->terrain != NULL)
	{
		state->terrain->shadow_change_units += ground_change;
		if (state->terrain->shadow_change_units >= state->shadow_threshold_units)
		{
			state->terrain->eroded_shadow_pending = true;
			state->terrain->shadow_change_units = 0;
		}
	}
}

void water_gpu_poll(water_t *water)
{
	water_gpu_t *state = water->gpu;

	if (state == NULL || !state->ready || state->failed)
	{
		return;
	}
	// In the order they were asked for (the GPU finishes them in that order)
	for (;;)
	{
		download_t *oldest = NULL;

		for (int i = 0; i < DOWNLOAD_SLOTS; i++)
		{
			if (state->downloads[i].busy && (oldest == NULL || state->downloads[i].sequence < oldest->sequence))
			{
				oldest = &state->downloads[i];
			}
		}
		if (oldest == NULL || !SDL_QueryGPUFence(gpu.device, oldest->fence))
		{
			return;
		}
		// Done already, so this does not wait: it lets SDL clean up the
		// finished command buffers, which it otherwise does only when a
		// frame is submitted (runs without frames would pile them up)
		SDL_WaitForGPUFences(gpu.device, true, &oldest->fence, 1);
		SDL_ReleaseGPUFence(gpu.device, oldest->fence);
		oldest->fence = NULL;
		oldest->busy = false;
		apply_download(water, oldest);
	}
}

// Records the copies of the statistics and of count tiles into a transfer
// buffer, and clears what the GPU counted since the last copy
static void record_download(water_t *water, SDL_GPUTransferBuffer *transfer, bool statistics, const int32_t *tiles, int32_t count)
{
	water_gpu_t *state = water->gpu;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUCopyPass *copy_pass;
	uint32_t offset = 0;

	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}
	copy_pass = SDL_BeginGPUCopyPass(command_buffer);
#define COPY(buffer_, source_offset, size_) \
	do \
	{ \
		SDL_GPUBufferRegion source = { (buffer_), (source_offset), (size_) }; \
		SDL_GPUTransferBufferLocation destination = { transfer, offset }; \
		SDL_DownloadFromGPUBuffer(copy_pass, &source, &destination); \
		offset += (size_); \
	} \
	while (0)
	if (statistics)
	{
		COPY(state->buffers[BUFFER_CONTROL], 0, statistics_bytes(state));
	}
	for (int32_t i = 0; i < count; i++)
	{
		int32_t tile = tiles[i] & ~TERRAIN_ENTRY;

		if ((tiles[i] & TERRAIN_ENTRY) != 0)
		{
			COPY(state->buffers[BUFFER_HEIGHT], tile_offset(tile, 4), TILE_CELLS * 4);
			COPY(state->buffers[BUFFER_LAYER], tile_offset(tile, 8), TILE_CELLS * 8);
		}
		else
		{
			COPY(state->buffers[BUFFER_DEPTH], tile_offset(tile, 4), TILE_CELLS * 4);
			COPY(state->buffers[BUFFER_SEDIMENT], tile_offset(tile, 4), TILE_CELLS * 4);
			COPY(state->buffers[BUFFER_FLUX], tile_offset(tile, 8), TILE_CELLS * 8);
		}
	}
#undef COPY
	SDL_EndGPUCopyPass(copy_pass);
	if (statistics)
	{
		fill_grid_parameters(water);
		dispatch(water, state->tiles_pipeline, TILES_CLEAR_CPU_FLAGS, tile_thread_groups(state), -1);
		dispatch(water, state->control_pipeline, CONTROL_RESET_COUNTERS, 1, -1);
	}
}

// The tiles waiting (needs: a byte each, 1) and not asked for within the
// interval, in turn from the cursor once round the map, into chosen (each
// with entry_flag) up to limit; returns the new count. From one waiting
// tile to the next with memchr: every tile was looked at one by one, each
// frame any of them waited
static int32_t choose_waiting_tiles(uint8_t *needs, uint64_t *asked_ticks, int32_t tile_count, int32_t *cursor, uint64_t now,
	uint64_t interval_milliseconds, int32_t *chosen, int32_t count, int32_t limit, int32_t entry_flag)
{
	int32_t start = tile_count > 0 ? SDL_clamp(*cursor, 0, tile_count - 1) : 0;

	// (from the cursor to the end, then from the start up to the cursor)
	for (int part = 0; part < 2 && count < limit; part++)
	{
		int32_t tile = part == 0 ? start : 0;
		int32_t end = part == 0 ? tile_count : start;

		while (tile < end && count < limit)
		{
			const uint8_t *found = memchr(&needs[tile], 1, (size_t)(end - tile));

			if (found == NULL)
			{
				break;
			}
			tile = (int32_t)(found - needs);
			if (now - asked_ticks[tile] >= interval_milliseconds)
			{
				needs[tile] = 0;
				asked_ticks[tile] = now;
				chosen[count++] = tile | entry_flag;
				*cursor = (tile + 1) % tile_count;
			}
			tile++;
		}
	}
	return count;
}

// The tiles to copy back this frame: those the CPU is behind on, around
// the focus first, then in turn from where the last frame stopped
static int32_t choose_tiles(water_t *water, float focus_x, float focus_z, int32_t budget)
{
	water_gpu_t *state = water->gpu;
	int32_t focus_tile_x = clamp_int((int32_t)(focus_x / water->cell_size_meters) / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t focus_tile_y = clamp_int((int32_t)(focus_z / water->cell_size_meters) / WATER_TILE_SIZE, 0, water->tiles_per_side - 1);
	int32_t count = 0;
	uint64_t now = SDL_GetTicks();

	for (int32_t tile_y = SDL_max(focus_tile_y - FOCUS_RADIUS_TILES, 0);
		tile_y <= SDL_min(focus_tile_y + FOCUS_RADIUS_TILES, water->tiles_per_side - 1) && count < budget; tile_y++)
	{
		for (int32_t tile_x = SDL_max(focus_tile_x - FOCUS_RADIUS_TILES, 0);
			tile_x <= SDL_min(focus_tile_x + FOCUS_RADIUS_TILES, water->tiles_per_side - 1) && count < budget; tile_x++)
		{
			int32_t tile = tile_y * water->tiles_per_side + tile_x;

			if (state->tile_needs_download[tile] && now - state->tile_downloaded_ticks[tile] >= DOWNLOAD_FOCUS_INTERVAL_MILLISECONDS)
			{
				state->tile_needs_download[tile] = 0;
				state->tile_downloaded_ticks[tile] = now;
				state->chosen_tiles[count++] = tile;
			}
		}
	}
	return choose_waiting_tiles(state->tile_needs_download, state->tile_downloaded_ticks, state->tile_count, &state->download_cursor, now,
		DOWNLOAD_INTERVAL_MILLISECONDS, state->chosen_tiles, count, budget, 0);
}

void water_gpu_end_frame(water_t *water, const terrain_t *terrain, float focus_x, float focus_z)
{
	water_gpu_t *state = water->gpu;
	download_t *slot = NULL;
	SDL_GPUFence *fence = NULL;

	if (water->baked)
	{
		baked_end_frame(water);
		return;
	}

	if (!ensure_resources(water))
	{
		return;
	}
	send_changes(water, terrain);

	// The water textures follow the tiles that changed
	fill_grid_parameters(water);
	state->parameters.render_stepped = state->stepped_since_render ? 1 : 0;
	state->stepped_since_render = false;
	dispatch(water, state->control_pipeline, CONTROL_RESET_LIST, 1, -1);
	dispatch(water, state->tiles_pipeline, TILES_RENDER, tile_thread_groups(state), -1);
	dispatch(water, state->control_pipeline, CONTROL_ARGUMENTS, 1, -1);
	dispatch(water, state->render_pipeline, 0, 0, ARGUMENT_LIST_TILES);

	// The terrain's textures follow the tiles whose ground or water changed
	// (and those still fading), or every tile into new textures
	if (state->ground_set && state->ground_pipeline != NULL)
	{
		uint64_t now = SDL_GetTicks();
		float frame_seconds = state->ground_ticks != 0 ? (float)(now - state->ground_ticks) * 0.001f : 0.0f;
		bool all = state->ground_all_pending;

		state->ground_ticks = now;
		state->parameters.ground_shown[0] = state->ground.bare_units;
		state->parameters.ground_shown[1] = state->ground.silt_visible_units;
		state->parameters.ground_shown[2] = state->ground.silt_full_units;
		state->parameters.ground_shown[3] = SDL_clamp(frame_seconds / GROUND_FADE_SECONDS, 0.002f, 1.0f);
		state->parameters.ground_more[0] = state->ground.normal_reach;
		state->parameters.ground_more[1] = all ? 1 : 0;
		state->parameters.ground_more[2] = all ? 1 : 0;
		state->parameters.ground_more[3] = 0;
		dispatch(water, state->control_pipeline, CONTROL_RESET_LIST, 1, -1);
		dispatch(water, state->tiles_pipeline, TILES_GROUND, tile_thread_groups(state), -1);
		dispatch(water, state->control_pipeline, CONTROL_ARGUMENTS, 1, -1);
		dispatch(water, state->ground_pipeline, 0, 0, ARGUMENT_LIST_TILES);
		state->parameters.ground_more[1] = 0;
		state->parameters.ground_more[2] = 0;
		if (all)
		{
			state->ground_all_pending = false;
			state->ground_live = true;
			SDL_Log("Terrain: its heights, normals and surface are made on the GPU from the simulation's ground");
		}
	}

	// A copy back. With every slot still on its way, the GPU is that many
	// frames behind: wait for the oldest, rather than pile up more work
	// than it can do (the headless runs have no window to hold them back)
	for (int attempt = 0; attempt < 2 && slot == NULL; attempt++)
	{
		for (int i = 0; i < DOWNLOAD_SLOTS; i++)
		{
			if (!state->downloads[i].busy)
			{
				slot = &state->downloads[i];
				break;
			}
		}
		if (slot == NULL)
		{
			download_t *oldest = NULL;
			uint64_t wait_start = SDL_GetPerformanceCounter();

			for (int i = 0; i < DOWNLOAD_SLOTS; i++)
			{
				if (oldest == NULL || state->downloads[i].sequence < oldest->sequence)
				{
					oldest = &state->downloads[i];
				}
			}
			SDL_WaitForGPUFences(gpu.device, true, &oldest->fence, 1);
			frame_profile_end(FRAME_COPY_WAIT, wait_start);
			water_gpu_poll(water);
		}
	}
	if (slot != NULL)
	{
		slot->tile_count = choose_tiles(water, focus_x, focus_z, DOWNLOAD_BUDGET_TILES);
		SDL_memcpy(slot->tiles, state->chosen_tiles, sizeof(int32_t) * (size_t)slot->tile_count);
		// The ground erosion changed, in turn (it changes slowly)
		uint64_t now = SDL_GetTicks();

		slot->tile_count = choose_waiting_tiles(state->tile_terrain_needs_download, state->tile_terrain_downloaded_ticks, state->tile_count,
			&state->terrain_cursor, now, DOWNLOAD_TERRAIN_INTERVAL_MILLISECONDS, slot->tiles, slot->tile_count,
			DOWNLOAD_BUDGET_TILES + DOWNLOAD_TERRAIN_TILES, TERRAIN_ENTRY);
		// The statistics with them: every frame while tiles change or the
		// CPU edits, else now and then (statistics_quiet)
		bool statistics = !state->statistics_quiet || state->statistics_edit_serial != state->edit_serial
			|| ++state->statistics_wait_frames >= STATISTICS_QUIET_FRAMES;

		if (statistics)
		{
			state->statistics_wait_frames = 0;
			state->statistics_edit_serial = state->edit_serial;
		}
		if (!statistics && slot->tile_count == 0)
		{
			// (nothing to copy this frame: the slot stays free)
			slot = NULL;
		}
		else
		{
			slot->serial = state->edit_serial;
			slot->generation = state->generation;
			slot->has_statistics = statistics;
			slot->sequence = ++state->download_sequence;
			record_download(water, slot->transfer, statistics, slot->tiles, slot->tile_count);
		}
	}
	gpu_submit_work(slot != NULL ? &fence : NULL);
	state->steps_since_submit = 0;
	if (slot != NULL && fence != NULL)
	{
		slot->fence = fence;
		slot->busy = true;
	}
}

void water_gpu_sync(water_t *water, const terrain_t *terrain)
{
	water_gpu_t *state = water->gpu;
	SDL_GPUTransferBuffer *transfer;
	SDL_GPUTransferBufferCreateInfo info;
	int32_t *tiles;

	if (!ensure_resources(water))
	{
		return;
	}
	send_changes(water, terrain);
	// Whatever is in flight lands first
	{
		SDL_GPUFence *fence = NULL;

		gpu_submit_work(&fence);
		if (fence != NULL)
		{
			SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
			SDL_ReleaseGPUFence(gpu.device, fence);
		}
		// With nothing new to submit there is no fence above: wait for
		// the copies themselves, or their counters would be lost
		for (int i = 0; i < DOWNLOAD_SLOTS; i++)
		{
			if (state->downloads[i].busy)
			{
				SDL_WaitForGPUFences(gpu.device, true, &state->downloads[i].fence, 1);
			}
		}
		water_gpu_poll(water);
	}

	SDL_zero(info);
	info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	info.size = statistics_bytes(state) + SYNC_BATCH_TILES * DOWNLOAD_TILE_BYTES;
	transfer = SDL_CreateGPUTransferBuffer(gpu.device, &info);
	tiles = SDL_malloc(sizeof(int32_t) * SYNC_BATCH_TILES);
	if (transfer == NULL || tiles == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Water: cannot copy the water back: %s", SDL_GetError());
		if (transfer != NULL)
		{
			SDL_ReleaseGPUTransferBuffer(gpu.device, transfer);
		}
		SDL_free(tiles);
		return;
	}
	// The water of every tile, then its ground
	for (int32_t first = 0; first < 2 * state->tile_count; first += SYNC_BATCH_TILES)
	{
		download_t download;
		SDL_GPUFence *fence = NULL;

		SDL_zero(download);
		download.transfer = transfer;
		download.tiles = tiles;
		download.tile_count = SDL_min(SYNC_BATCH_TILES, 2 * state->tile_count - first);
		download.serial = state->edit_serial;
		download.generation = state->generation;
		download.has_statistics = first == 0;
		for (int32_t i = 0; i < download.tile_count; i++)
		{
			int32_t entry = first + i;

			if (entry < state->tile_count)
			{
				tiles[i] = entry;
				state->tile_needs_download[entry] = 0;
			}
			else
			{
				tiles[i] = (entry - state->tile_count) | TERRAIN_ENTRY;
				state->tile_terrain_needs_download[entry - state->tile_count] = 0;
			}
		}
		record_download(water, transfer, download.has_statistics, tiles, download.tile_count);
		gpu_submit_work(&fence);
		if (fence != NULL)
		{
			SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
			SDL_ReleaseGPUFence(gpu.device, fence);
		}
		apply_download(water, &download);
	}
	SDL_ReleaseGPUTransferBuffer(gpu.device, transfer);
	SDL_free(tiles);
}

void water_gpu_set_ice(water_t *water, SDL_GPUTexture *ice_texture)
{
	if (water->gpu != NULL)
	{
		water->gpu->ice_texture = ice_texture;
	}
}

SDL_GPUTexture *water_gpu_depth_texture(const water_t *water)
{
	return water->gpu != NULL ? water->gpu->depth_texture : NULL;
}

SDL_GPUTexture *water_gpu_flow_texture(const water_t *water)
{
	return water->gpu != NULL ? water->gpu->flow_texture : NULL;
}
