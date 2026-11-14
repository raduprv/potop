// Water simulation: the "virtual pipes" model on a height field.
//
// Every cell holds a column of water. Each cell is connected to its four
// neighbors by a pipe, and the flow (flux) through each pipe has inertia:
// the difference in water level accelerates it, friction slows it down.
// That inertia is what gives waves. Each step has two passes:
//
//   1. fluxes: every cell updates the outflow through its four pipes from
//      the level differences, then scales all four down if together they
//      would take more water than the cell holds;
//   2. depths: every cell adds what flows in from its neighbors and
//      subtracts what flows out.
//
// Water depth is an integer in the same 0.01 mm units as the terrain height.
// Each pipe's flux is converted to integer units once per step, and exactly
// that amount leaves one cell and enters the other, so water is never
// created or lost by rounding: the total only changes through sources,
// the sponge and drains at the map border.
//
// Fluxes are stored as 16 bit floats where the compiler and CPU support them
// natively (x86 with F16C, ARM64), which halves their memory traffic.
//
// The map is split into tiles (the same 64x64 tiles the renderer uses as
// chunks). A tile is:
//   dry      - no water at all: not simulated, not drawn;
//   sleeping - water that has come to rest: not simulated, still drawn;
//   active   - simulated every step.
// Tiles wake up when water flows into them, when a neighbor's level changes
// against them, or when a tool, spring or rain touches them.
//
// Water thinner than a film threshold (about a millimeter) clings to the
// ground: it does not flow and does not wake anything, and it evaporates
// slowly, so the map dries up after rain instead of trickling forever.
//
// The simulation runs on the GPU (water_gpu.h); this water_t is the CPU's
// copy, a frame or two behind, that the rest of the game reads, and where
// its changes go in.
//
// Water also carries sediment (see erosion.h): an integer amount of
// material per cell, in the same units, that travels through the pipes
// in proportion to the water, again exactly: what leaves one cell enters
// its neighbor. And the ground under the water soaks some of it up
// (moisture, also in water units), which erosion.c moves back and forth.

#ifndef WATER_H
#define WATER_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"
#include "soils.h"
#include "terrain.h"

#define WATER_TILE_SIZE TERRAIN_CHUNK_SIZE
#define WATER_MAX_SPRINGS 64
#define WATER_UNITS_PER_OBSTACLE_CENTIMETER (TERRAIN_HEIGHT_UNITS_PER_METER / 100)
// A baked map's flow per cell: velocity x, z (signed), mud, the busiest
// pipe's speed, a byte each, square root scaled (finer where it is slow)
#define WATER_BAKED_FLOW_BYTES 4
#define WATER_BAKED_TILE_CELLS (WATER_TILE_SIZE * WATER_TILE_SIZE)

// Build with -DWATER_FLUX_FLOAT32 to force 32 bit fluxes (for comparisons)
#if defined(__FLT16_MAX__) && (defined(__F16C__) || defined(__aarch64__)) && !defined(WATER_FLUX_FLOAT32)
typedef _Float16 water_flux_t;
#define WATER_FLUX_BITS 16
#else
typedef float water_flux_t;
#define WATER_FLUX_BITS 32
#endif

typedef enum
{
	WATER_TILE_DRY = 0,
	WATER_TILE_SLEEPING,
	WATER_TILE_ACTIVE
} water_tile_state_t;

// Pipe directions: grid x is world X, grid y is world Z
typedef enum
{
	WATER_DIRECTION_LEFT = 0,      // -x
	WATER_DIRECTION_RIGHT,         // +x
	WATER_DIRECTION_BACK,          // -y
	WATER_DIRECTION_FRONT,         // +y
	WATER_DIRECTION_COUNT
} water_direction_t;

// Why a tile woke up (went from dry or sleeping to active), for diagnostics
typedef enum
{
	WATER_WAKE_SOURCE = 0,         // brush, spring, rain
	WATER_WAKE_NEIGHBOR,           // water flowing in, or a higher level next door
	WATER_WAKE_EDIT,               // terrain edited under or next to it
	WATER_WAKE_SOAK,               // the ground drank enough to unlevel it
	WATER_WAKE_EROSION,            // erosion or deposition changed the bed
	WATER_WAKE_REASON_COUNT
} water_wake_reason_t;

typedef struct
{
	int32_t cell_x;
	int32_t cell_y;
	double remainder_units;        // fractional units not yet added
} water_spring_t;

// What the next substep does with one pipe of a cell (see the inspector)
typedef struct
{
	bool inside;                   // false: the pipe leaves the map
	bool neighbor_wet;             // the neighbor holds more than a film
	bool superfluid;               // thin water flowing on over wet ground
	bool meniscus_holds;           // surface tension keeps it shut
	bool lip_holds;                // the lip keeps deep water from spilling into thin
	bool still_holds;              // static friction keeps it shut
	float drop_millimeters;        // our level minus the neighbor's
	float push_factor;             // pipe size against a classic pipe (0..1)
	float next_flux;               // outflow after the next substep, m^3/s
} water_pipe_report_t;

typedef struct
{
	bool flows;                    // false: no water, or only a film
	water_pipe_report_t pipes[4];  // water_direction_t order
	float dry_friction_depth;      // meters the friction sees, into dry ground
	float wet_friction_depth;      // ...and over wet ground
	float shore_factor;            // shore damping (1: none)
	bool outflow_limited;          // the cell holds less than the pipes want
	float units_per_flux;          // whole units a flux of 1 m^3/s moves per substep
	int32_t film_units;
	int32_t thin_units;
	int32_t still_units;
	// A tile sleeps once every cell changes by at most this per substep
	// and flows slower than sleep_speed, for 30 substeps in a row
	int32_t sleep_change_units;
	float substep_seconds;
} water_cell_report_t;

typedef struct water_s
{
	int32_t size;                  // cells per side, same as the terrain
	float cell_size_meters;
	int32_t tiles_per_side;

	int32_t *depth;                // TERRAIN_HEIGHT_UNITS_PER_METER units
	water_flux_t *flux[WATER_DIRECTION_COUNT];   // outflow, m^3/s
	int32_t *sediment;             // suspended material, height units
	float *sediment_concentration; // pass 1 output: sediment units per water unit
	// Bit per pipe: the last water through it came in, from the neighbor
	// (written by pass 2, for the static friction; see compute_cell_fluxes)
	uint8_t *inflow_directions;
	uint16_t *moisture;            // water held by the ground, units
	// Walls standing on the ground (houses, stone walls), in centimeters:
	// the water flows around them, or over them if it rises that high.
	// Set by the physics from the objects (see physics.h), which flags the
	// tiles it changed in tile_obstacles_dirty.
	uint16_t *obstacle_centimeters;

	// Per tile
	uint8_t *tile_state;           // water_tile_state_t
	uint8_t *tile_wake_reason;     // why it last woke (water_wake_reason_t)
	int64_t *tile_wake_step;       // ...and when (water steps)
	uint8_t *tile_idle_steps;      // steps in a row with negligible flow
	uint8_t *tile_wake_requests;   // pass 1 output: bit per direction
	bool *tile_obstacles_dirty;    // walls changed: to go up to the GPU
	int64_t *tile_total_depth;
	int32_t *tile_max_depth;
	// Cells whose surface drops steeply to a wet neighbor (the GPU counts
	// them now and then): a waterfall's lip can only be in a tile with some
	// (waterfalls.c looks nowhere else). steep_cells_known: the GPU has
	// counted them (without it, every wet tile is looked at)
	int32_t *tile_steep_cells;
	bool steep_cells_known;
	float *tile_max_speed;         // fastest flow in the tile, m/s
	float *tile_max_deep_speed;    // the same, in water at least 5 cm deep
	int32_t *tile_max_change;      // largest depth change of a cell, units
	int64_t *tile_drained;
	int64_t *tile_corrections;
	int64_t *tile_discarded;       // trickles thrown away at sleeping neighbors
	int64_t *tile_sediment_discarded;
	int64_t *tile_evaporated;
	int64_t *tile_sediment;        // suspended sediment in the tile
	int64_t *tile_moisture;        // ground moisture in the tile
	int64_t *tile_sediment_drained;
	int64_t *tile_sediment_corrections;
	bool any_obstacles_dirty;

	// The GPU side (water_gpu.c)
	struct water_gpu_s *gpu;

	// A baked map's water (map_file.h): still, as the simulation left it.
	// Only depth and the tiles' bookkeeping then; flux, sediment,
	// sediment_concentration, inflow_directions, moisture and
	// obstacle_centimeters are NULL. Per tile, NULL where it is dry: its
	// cells' flow as it was, WATER_BAKED_FLOW_BYTES each (velocity along
	// x and z, mud, the busiest pipe's speed: water_baked_* below)
	bool baked;
	uint8_t **baked_flow;

	// Scratch lists of tile indices
	int32_t *active_tiles;
	int32_t active_tile_count;
	int32_t *update_tiles;
	int32_t update_tile_count;
	uint8_t *tile_in_update_list;

	water_spring_t springs[WATER_MAX_SPRINGS];
	int spring_count;
	bool raining;
	// The cold holds all of it still and the ice over it bears (the game's,
	// water_ice.h): the walker and the actors stand on its surface
	bool iced;
	bool drain_edges;
	// Pipes sized by the water depth (shallow water equations) instead of
	// the classic fixed cross section; see update_tile_fluxes
	bool depth_scaled_pipes;
	double rain_remainder_units;
	// Water speeds (for the physics, erosion, sleep) are flux over this
	// depth at least: thin superfluid water moves a lot of water for its
	// depth, and divided by its real depth it would look like rapids.
	// Set every step from the settings.
	float speed_depth_meters;

	// Bookkeeping, all in units;
	// total = added - removed - drained - evaporated
	int64_t total_units;
	int64_t added_units;
	int64_t removed_units;
	int64_t drained_units;
	int64_t evaporated_units;      // includes discarded_units
	// Trickles into sleeping tiles, thrown away (see CHECK_BORDER in water.c)
	int64_t discarded_units;
	float evaporation_timer_seconds;
	int64_t negative_depth_corrections;  // should stay 0; logged if not

	// Ground moisture, units: kept by erosion.c for the farms, the wet look
	// and the banks; not part of the water balance (wetting the ground does
	// not take water away)
	int64_t moisture_units;
	int64_t dried_units;
	// Erosion's totals, from the GPU: ever picked up, dropped, moved by
	// bank collapse
	int64_t eroded_units;
	int64_t deposited_units;
	int64_t collapsed_units;
	// Sediment bookkeeping: material that left the map with the water, and
	// sediment created by clamping a cell at zero (should stay 0)
	int64_t sediment_drained_units;
	int64_t sediment_corrections;
	int32_t substeps_last_step;
	int64_t step_count;            // water steps since the start
	// Fastest flow at least 5 cm deep in any active tile during the last
	// step (m/s); decides
	// the next step's substeps (see water_step)
	float fastest_flow_last_step;
	int32_t simulated_tiles_last_step;

	// Diagnostics, counted since the caller last cleared them: wake ups by
	// reason, and (per tile and substep) why active tiles did not count as
	// still: flow faster than the sleep speed, or a level changing too fast
	int64_t wake_counts[WATER_WAKE_REASON_COUNT];
	int64_t kept_awake_by_flow;
	int64_t kept_awake_by_level_change;
} water_t;

// Everything a substep's flux pass needs that is the same for every cell
// (the GPU gets it in its parameter block; the inspector uses it as it is)
typedef struct
{
	float step_seconds;
	float gravity_times_cell;      // g * A / l with A = cell^2, l = cell (fixed pipes)
	float pipe_depth_limit;        // pipes are sized by the water depth up to this (meters)
	// Thin water flows as if it were at least this deep (meters, 0: off):
	// friction and pipe size, see thin_water_superfluid in settings.h
	float superfluid_depth;
	// Shallow, slow water near shores is braked harder (meters, per substep)
	float shore_depth;
	float shore_damping_times_step;
	// Static friction: water that came in through a pipe in the last
	// substep does not go back out through it for a level difference
	// below this (units)
	int32_t still_units;
	// A sleeping tile wakes for at least this much water pouring into one
	// of its cells in a substep, or a level of its own this much above
	// ours (units); smaller inflow is thrown away
	float asleep_wake_units;
	int32_t asleep_level_wake_units;
	bool depth_scaled_pipes;
	float friction_times_step;
	// g * n^2 * substep for the Manning friction (0: none)
	float manning_times_step;
	float cell_size;
	float linear_damping_factor;
	float cell_area;
	int32_t film_units;            // water at or below this does not flow
	int32_t meniscus_units;        // fake surface tension, see water.c
	int32_t thin_units;            // thin water (thin_water_millimeters)
	int32_t lip_units;             // deep water spills into thin only this far above it
	float border_wake_units;       // smallest inflow per substep that wakes a sleeping tile
	float units_per_flux;          // whole units a flux moves per substep
	bool drain_edges;
	// Bed friction factor of each soil; silt counts once it is at least
	// silt_units thick
	float friction_factor[SOIL_PROPERTY_COUNT];
	int32_t silt_units;
} water_flux_parameters_t;

void water_make_flux_parameters(const water_t *water, const water_settings_t *settings, const soil_settings_t *soil_settings,
	float substep_seconds, water_flux_parameters_t *parameters);

// Tiles whose cells all change by less than this per substep (and whose
// water moves slower than the sleep speed) count as still
int32_t water_sleep_change_units(const water_settings_t *settings, float substep_seconds);

bool water_create(water_t *water, int32_t size, float cell_size_meters, bool drain_edges, bool depth_scaled_pipes);
// A baked map's (see baked above): no simulation, a fraction of the memory
bool water_create_baked(water_t *water, int32_t size, float cell_size_meters);
void water_destroy(water_t *water);

// Removes all water and springs.
void water_clear(water_t *water);

// The water at a world position (nearest cell): its depth and how fast it
// flows (meters per second, along world X and Z). False outside the map.
bool water_sample(const water_t *water, float world_x, float world_z, float *depth_meters_out, float *velocity_x_out,
	float *velocity_z_out);

// After depth, sediment, moisture and the springs were filled in from a
// saved map: still water (no flow), the tiles and the bookkeeping redone.
void water_restore(water_t *water);

// Advances the simulation by step_seconds (one fixed simulation step). The
// soil settings give the bed friction of each soil.
void water_step(water_t *water, const terrain_t *terrain, const water_settings_t *settings, const soil_settings_t *soil_settings,
	float step_seconds);

// Adds (positive rate) or removes (negative rate) water with a soft round
// brush. rate is in meters of depth per second at the brush center.
void water_apply_brush(water_t *water, float center_x, float center_z, float radius_meters,
	float meters_per_second, float step_seconds);

// Adds water to one cell (a source, counted in added_units) and wakes it.
void water_add_units(water_t *water, int32_t cell_x, int32_t cell_y, int32_t units);

// After changing depth, sediment or moisture in water_t directly: those
// tiles go up to the GPU (and wake) at the next step
void water_mark_cells_edited(water_t *water, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y);

// Places a spring at the cell under the given position, or removes the
// spring already there. Returns true if a spring was added.
bool water_toggle_spring(water_t *water, float world_x, float world_z);

// Wakes every tile touching an inclusive rectangle of cells (for example
// after the terrain under the water was edited).
void water_wake_cells(water_t *water, int32_t minimum_x, int32_t minimum_y, int32_t maximum_x, int32_t maximum_y);

// Recounts all water from scratch (slow; for tests and sanity checks).
int64_t water_count_total_units(const water_t *water);

// Same for sediment and ground moisture.
int64_t water_count_sediment_units(const water_t *water);
int64_t water_count_moisture_units(const water_t *water);

// Marks a tile as dry and stops its flow in the CPU's copy (the CPU
// erosion's; goes with it in step b2).
void water_set_tile_dry(water_t *water, int32_t tile);

// Wakes one tile.
void water_wake_tile(water_t *water, int32_t tile, water_wake_reason_t reason);

const char *water_wake_reason_name(water_wake_reason_t reason);

// What the next substep would do with one cell's pipes, with the same
// code the simulation runs (for the inspector). substep_seconds is the
// length of one substep.
void water_inspect_cell(const water_t *water, const terrain_t *terrain, const water_settings_t *settings,
	const soil_settings_t *soil_settings, float substep_seconds, int32_t cell_x, int32_t cell_y, water_cell_report_t *report);

// Number of tiles in each state, for statistics.
void water_count_tiles(const water_t *water, int32_t *active_out, int32_t *sleeping_out, int32_t *dry_out);

static inline int32_t water_tile_index_of_cell(const water_t *water, int32_t cell_x, int32_t cell_y)
{
	return (cell_y / WATER_TILE_SIZE) * water->tiles_per_side + cell_x / WATER_TILE_SIZE;
}

// The ice's surface (water_t.iced): at a cell, the water's level there
// smoothed with its wet neighbors' (the ripples it froze in are not felt
// underfoot); false where there is no water. At a point (meters), between
// the cells
bool water_ice_level_at_cell(const water_t *water, const terrain_t *terrain, int32_t x, int32_t y, float *level_out);
bool water_ice_surface(const water_t *water, const terrain_t *terrain, float world_x, float world_z, float *height_out);

// The baked flow's bytes (WATER_BAKED_FLOW_BYTES): speeds up to
// WATER_BAKED_FASTEST m/s, signed for the velocity; mud (sediment units
// per water unit) up to WATER_BAKED_MUDDIEST
static const float WATER_BAKED_FASTEST = 8.0f;
static const float WATER_BAKED_MUDDIEST = 0.25f;

static inline uint8_t water_baked_encode_velocity(float velocity)
{
	float amount = sqrtf(fminf(fabsf(velocity) / WATER_BAKED_FASTEST, 1.0f)) * 127.0f + 0.5f;
	int value = (int)amount;

	return (uint8_t)(int8_t)(velocity < 0.0f ? -value : value);
}

static inline float water_baked_decode_velocity(uint8_t byte)
{
	float amount = (float)(int8_t)byte / 127.0f;

	return (amount < 0.0f ? -amount * amount : amount * amount) * WATER_BAKED_FASTEST;
}

static inline uint8_t water_baked_encode_amount(float value, float most)
{
	return (uint8_t)(sqrtf(fminf(fmaxf(value / most, 0.0f), 1.0f)) * 255.0f + 0.5f);
}

static inline float water_baked_decode_amount(uint8_t byte, float most)
{
	float amount = (float)byte / 255.0f;

	return amount * amount * most;
}

// What its cells take (bytes), for the log
size_t water_memory_bytes(const water_t *water);

// The mud in the water at a cell (sediment units per water unit), baked
// or not
float water_concentration_at(const water_t *water, int32_t index);

#endif
