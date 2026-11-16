// The water simulation on the GPU (see water.h for the model, water_gpu.c
// for the C side). Every water compute shader includes this: the same
// buffers, bound the same way, and the same parameter block.
//
// Cells are stored tile by tile (64 x 64 cells, 4096 in a row, the tiles
// in row order), so a tile is one contiguous block: a workgroup works on a
// tile, and the CPU copies tiles in and out with one copy each.
//
// Amounts of water and material are integers in height units (0.01 mm), as
// on the CPU: what leaves one cell is computed from the same stored values
// as what enters the other, so nothing is created or lost by rounding.

#define TILE_SIZE 64
#define TILE_CELLS 4096
#define UNITS_PER_OBSTACLE_CENTIMETER 1000
#define METERS_PER_UNIT 0.00001

// Pipe directions, as water_direction_t
#define DIRECTION_LEFT 0
#define DIRECTION_RIGHT 1
#define DIRECTION_BACK 2
#define DIRECTION_FRONT 3

#define TILE_DRY 0u
#define TILE_SLEEPING 1u
#define TILE_ACTIVE 2u

#define WAKE_SOURCE 0u
#define WAKE_NEIGHBOR 1u
#define WAKE_EDIT 2u
#define WAKE_SOAK 3u
#define WAKE_EROSION 4u

// Rounding noise, not flow (see water.c)
#define FLOW_DEAD_BAND_UNITS 2
#define MINIMUM_PIPE_DEPTH 0.001
#define MINIMUM_FRICTION_DEPTH 0.01
#define SHORE_DAMPING_SPEED 0.3

// The control buffer: counters, indirect arguments, then each tile's words,
// then the tile lists (must match water_gpu.c)
#define HEADER_ACTIVE_COUNT 0
#define HEADER_UPDATE_COUNT 1
#define HEADER_LIST_COUNT 2
// The indirect dispatch arguments live in a buffer of their own (see
// water_control.comp): a buffer read both by shaders and as dispatch
// arguments does not get its shader writes made visible to the next pass's
// shader reads on every GPU
#define ARGUMENT_ACTIVE_TILES 0
#define ARGUMENT_ACTIVE_THREADS 3
#define ARGUMENT_UPDATE_TILES 6
#define ARGUMENT_UPDATE_THREADS 9
#define ARGUMENT_LIST_TILES 12
#define ARGUMENT_LIST_THREADS 15
#define ARGUMENT_WORDS 18
#define HEADER_FASTEST_DEEP_SPEED 22
#define HEADER_LAST_FASTEST_DEEP_SPEED 23
#define HEADER_COUNTERS 24
#define HEADER_MOST_UPDATE_TILES 62
#define HEADER_WORDS 64

// 64 bit counters, two words each (low, high), from HEADER_COUNTERS
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

// A tile's words
#define TILE_STATE 0
#define TILE_IDLE_STEPS 1
#define TILE_WAKE_REQUESTS 2
#define TILE_IN_UPDATE 3
#define TILE_TOTAL_DEPTH 4          // two words
#define TILE_MAX_DEPTH 6
#define TILE_MAX_SPEED 7            // float bits
#define TILE_MAX_DEEP_SPEED 8       // float bits
#define TILE_MAX_CHANGE 9
#define TILE_SEDIMENT 10            // two words
#define TILE_FLAGS 12
#define TILE_WAKE_REASON 13
#define TILE_WAKE_STEP 14
#define TILE_STEEP_CELLS 15         // cells whose surface drops steeply to a wet neighbor (counted every STEEP_COUNT_STEPS steps: where waterfalls.c looks)
#define STEEP_COUNT_STEPS 8
#define STEEP_RISE_CELLS 1.5        // a drop of this many cells' widths is steep (waterfalls.c's STEEP_RISE)
#define STEEP_WET_UNITS 1000        // water this deep counts (waterfalls.c's WET_METERS, 1 cm)
#define TILE_SETTLE_STEPS 17        // erosion keeps checking the banks this many steps after a change
#define TILE_CARRIES_SEDIMENT 18    // 1 if a cell of it sends silt with its water (water_fluxes.comp, the last time it ran): its neighbors read the concentrations
#define TILE_WORDS 24

#define TILE_FLAG_CPU_DIRTY 1u      // the CPU's copy is out of date
#define TILE_FLAG_RENDER_DIRTY 2u   // the water textures are out of date
#define TILE_FLAG_RENDER_PREVIOUS 8u // the depth texture's previous step differs from its last
// A tile in the render list whose values are new (else: only its previous
// step catches up with its last)
#define RENDER_ENTRY_NEW 0x80000000u
#define TILE_FLAG_TERRAIN_DIRTY 4u  // erosion changed the ground: the CPU's copy is out of date
#define TILE_FLAG_GROUND_DIRTY 16u  // the terrain's textures (water_ground.comp) are out of date
#define TILE_FLAG_GROUND_SETTLING 32u // its silt and bare rock shown are still fading towards the ground's
#define TILE_FLAG_WATER_SHOWN_DIRTY 64u // only its water changed: the terrain's "water on it" (and wetness) out of date, not its ground
// A tile in the ground list that only needs the water on it written again
// (water_ground.comp: not its heights and normals)
#define GROUND_ENTRY_WATER_ONLY 0x80000000u
// A tile to wake that is only to be drawn again (the CPU changed its ground
// or its moisture): not woken
#define WAKE_GROUND_ONLY 255u

#define SOIL_SILT 4u

#ifndef WATER_RW_TEXTURES
#define WATER_RW_TEXTURES 0
#endif

#ifndef WATER_CONTROL_KERNEL
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 0) buffer depth_buffer { int depths[]; };
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 1) buffer sediment_buffer { int sediments[]; };
// Outflow (m^3/s) as half floats: x left and right, y back and front
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 2) buffer flux_buffer { uvec2 fluxes[]; };
// x: sediment concentration (float bits, pass 1 to pass 2), y: inflow
// direction bits (the static friction)
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 3) buffer aux_buffer { uvec2 auxes[]; };
// The ground's wetness as the CPU has it (its moisture lives there,
// erosion.c), a byte a cell (0..255), four to a word: for the terrain's
// textures (water_ground.comp)
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 4) buffer moisture_buffer { uint moistures[]; };
// A kernel whose threads read what others of the workgroup wrote (between
// barriers) defines this as coherent before the include
#ifndef GROUND_COHERENT
#define GROUND_COHERENT
#endif
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 5) GROUND_COHERENT buffer height_buffer { int heights[]; };
// x: bedrock height, y: silt thickness
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 6) GROUND_COHERENT buffer layer_buffer { ivec2 layers[]; };
layout(std430, set = READWRITE_SET, binding = WATER_RW_TEXTURES + 7) buffer control_buffer { uint control[]; };

// (a kernel with samplers of its own defines WATER_SAMPLERS: they come
// first, the read only buffers after them)
#ifndef WATER_SAMPLERS
#define WATER_SAMPLERS 0
#endif
// Walls standing on the ground, centimeters, two cells to a word
layout(std430, set = READONLY_SET, binding = WATER_SAMPLERS + 0) readonly buffer obstacle_buffer { uint obstacles[]; };
// Soil types, four cells to a word
layout(std430, set = READONLY_SET, binding = WATER_SAMPLERS + 1) readonly buffer soil_buffer { uint soils[]; };
// What the CPU asks for: springs (cell x, cell y, units, 0), tiles to wake
// (tile, reason, 0, 0)
layout(std430, set = READONLY_SET, binding = WATER_SAMPLERS + 2) readonly buffer command_buffer { ivec4 commands[]; };
#endif

// Must match water_gpu_parameters_t in water_gpu.c (std140)
layout(std140, set = UNIFORM_SET, binding = 0) uniform water_block
{
	int map_size;
	int tiles_per_side;
	int tile_count;
	int mode;

	float step_seconds;
	float gravity_times_cell;
	float pipe_depth_limit;
	float superfluid_depth;

	float shore_depth;
	float shore_damping_times_step;
	float asleep_wake_units;
	int asleep_level_wake_units;

	float friction_times_step;
	float manning_times_step;
	float cell_size;
	float linear_damping_factor;

	float cell_area;
	int film_units;
	int meniscus_units;
	int thin_units;

	int lip_units;
	float border_wake_units;
	float units_per_flux;
	int still_units;

	int depth_scaled_pipes;
	int drain_edges;
	int silt_units;
	int sleep_change_units;

	vec4 friction_factors;           // sand, earth, clay, rock
	vec4 more_friction_factors;      // silt

	float speed_depth_meters;
	int deep_flow_units;
	float sleep_speed;
	int sleep_after_substeps;

	vec4 brush;                      // center cell x and y, radius in cells, units at the center
	ivec4 brush_rectangle;           // first cell x and y, width, height

	int brush_adding;
	int command_count;
	int rain_units;
	int step_number;

	int evaporation_units;
	int thin_evaporation_units;
	int wake_reason;
	int command_first;               // where this dispatch's commands start

	// Erosion (see erosion.h)
	float transport_capacity;
	float deposition_rate;
	float still_speed;               // slower water drops its mud at still_deposition_rate
	float still_deposition_rate;

	int erosion_start_units;         // erosion grows from nothing at this depth...
	int erosion_full_units;          // ...to full at this one
	int minimum_soil_units;          // erosion stops this close to the bedrock
	int submerged_units;             // banks with water this deep at their foot are wet

	float erosion_smoothing;
	float wet_critical_speed_factor;
	int collapse_color;              // which tiles of the 2x2 checkerboard collapse now
	int render_stepped;            // water_render: the simulation stepped since the last time

	vec4 erodibilities;              // sand, earth, clay, rock
	vec4 critical_speeds;
	vec4 talus_slopes;
	vec4 silt_soil;                  // silt's erodibility, critical speed, talus slope

	// The terrain's textures (water_ground.comp)
	vec4 ground_shown;               // bare rock fades in over this soil (units), silt shows from, fully at (units), fade per frame (0..1)
	ivec4 ground_more;               // the material normal's reach (cells), every tile (1/0), at once: no fade (1/0), -

	ivec4 ice_more;                  // x 1: the ice holds the water under it still (water_fluxes.comp, water_ice.h); -
};

#ifndef WATER_CONTROL_KERNEL
// ---------------------------------------------------------------------------
// Cells and tiles

uint cell_of(int x, int y)
{
	return (uint((y >> 6) * tiles_per_side + (x >> 6)) << 12) | uint(((y & 63) << 6) | (x & 63));
}

uint tile_word(uint tile, uint word)
{
	return uint(HEADER_WORDS) + tile * uint(TILE_WORDS) + word;
}

uint active_list_word(uint i)
{
	return uint(HEADER_WORDS) + uint(tile_count) * uint(TILE_WORDS) + i;
}

uint update_list_word(uint i)
{
	return uint(HEADER_WORDS) + uint(tile_count) * uint(TILE_WORDS + 1) + i;
}

uint list_word(uint i)
{
	return uint(HEADER_WORDS) + uint(tile_count) * uint(TILE_WORDS + 2) + i;
}

uint tile_state(uint tile)
{
	return control[tile_word(tile, TILE_STATE)];
}

int obstacle_units(uint cell)
{
	return int((obstacles[cell >> 1] >> ((cell & 1u) * 16u)) & 0xFFFFu) * UNITS_PER_OBSTACLE_CENTIMETER;
}

uint soil_of(uint cell)
{
	return (soils[cell >> 2] >> ((cell & 3u) * 8u)) & 0xFFu;
}

// The bed the water lies on: the ground plus whatever wall stands there
int bed_of(uint cell)
{
	return heights[cell] + obstacle_units(cell);
}

// The soil's properties: silt (4) is kept apart
float erodibility_of(uint soil)
{
	return soil < 4u ? erodibilities[soil] : silt_soil.x;
}

float critical_speed_of(uint soil)
{
	return soil < 4u ? critical_speeds[soil] : silt_soil.y;
}

float talus_slope_of(uint soil)
{
	return soil < 4u ? talus_slopes[soil] : silt_soil.z;
}

// The layer that decides how the surface behaves: silt once it is thick
// enough to show, the cell's soil otherwise
uint surface_layer_of(uint cell)
{
	return layers[cell].y >= silt_units ? SOIL_SILT : soil_of(cell);
}

float friction_factor_of(uint cell)
{
	uint soil = layers[cell].y >= silt_units ? 4u : soil_of(cell);

	return soil < 4u ? friction_factors[soil] : more_friction_factors.x;
}

vec4 fluxes_of(uint cell)
{
	uvec2 packed = fluxes[cell];

	return vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
}

// What a pipe moves in one substep, in whole units: the same function on
// the same stored value on both sides of the pipe
int flux_units(float flux)
{
	return int(flux * units_per_flux);
}

int sediment_units(int water_units, float concentration)
{
	return int(float(water_units) * concentration);
}

// ---------------------------------------------------------------------------
// 64 bit counters

void add_counter(uint counter, uint value)
{
	if (value != 0u)
	{
		uint word = uint(HEADER_COUNTERS) + counter * 2u;
		uint old = atomicAdd(control[word], value);

		if (old + value < old)
		{
			atomicAdd(control[word + 1u], 1u);
		}
	}
}

// Wakes a tile (any thread may): counted once, by the thread that changed it
void wake_tile(uint tile, uint reason)
{
	uint old = atomicExchange(control[tile_word(tile, TILE_STATE)], TILE_ACTIVE);

	control[tile_word(tile, TILE_IDLE_STEPS)] = 0u;
	if (old != TILE_ACTIVE)
	{
		control[tile_word(tile, TILE_WAKE_REASON)] = reason;
		control[tile_word(tile, TILE_WAKE_STEP)] = uint(step_number);
		add_counter(uint(COUNTER_WAKE_FIRST) + reason, 1u);
	}
}

// Sets a 64 bit value from 64 bit sums kept as (low, high)
void store_wide(uint word, uvec2 value)
{
	control[word] = value.x;
	control[word + 1u] = value.y;
}

uvec2 add_wide(uvec2 sum, uint value)
{
	uint carry;
	uint low = uaddCarry(sum.x, value, carry);

	return uvec2(low, sum.y + carry);
}
#endif
