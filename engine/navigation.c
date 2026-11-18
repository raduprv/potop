// Navigation: the surfaces, the change stamps and A* (see navigation.h).

#include "navigation.h"
#include "profile.h"
#include "parallel.h"

#include <SDL3/SDL.h>
#include <math.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// A search with maximum_expansions surfaces gets about this many times its
// square root (in cells) away from where it starts (a ring of that size
// holds them all; along a corridor it gets farther)
#define SEARCH_REACH_FACTOR 1.5f
#define FLIGHT_PREPARE_METERS 1024.0f
// A walk's search gets this many surfaces, and this many more per meter to
// its goal (at most maximum_expansions): a way round a pond is found, the
// whole forest is not searched for a goal it cannot get to (a bear stuck
// on a bank, a goal walled in), it gets as near as it can and tries again
#define SEARCH_BASE_EXPANSIONS 4096
#define SEARCH_EXPANSIONS_PER_METER 256
#define TERRAIN_CHANGE_METERS 0.15f  // a cell's terrain moved more than this: its height follows (the objects are not built again)
#define OBJECT_HEIGHT_STEP_METERS 0.15f // objects following the ground count in steps of this (as TERRAIN_CHANGE_METERS)
#define SAMPLE_METERS 0.125f         // object triangles are sampled this finely (twice per quarter meter: no holes)
#define MERGE_METERS 0.1f            // an object's top this near the ground is the ground (what reaches higher blocks the ground: clearance_above)
#define MINIMUM_SURFACE_SUBCELLS 4   // of 16: an object's top covering less of a cell is nothing to stand on
#define WALKABLE_NORMAL_Y 0.64f      // triangles less steep than about 50 degrees are surfaces
#define LINE_STEP_METERS 0.5f
#define PULL_MAXIMUM_CELLS 64        // a straight stretch of the pulled path is at most this long
#define HEURISTIC_WEIGHT 1.3f       // weighted A*: paths up to 30% longer than the best, found much faster
// Flight: the search steps over blocks of cells (a bird crosses kilometers),
// the straight line is checked every meter
#define FLIGHT_BLOCK_CELLS 4
#define FLIGHT_LINE_STEP_METERS 1.0f
#define FLIGHT_CLIMB_COST 0.5f       // per meter climbed, on top of the distance: low ways are preferred

// Water classes of a surface, from easy to hard
#define CLASS_DRY 0
#define CLASS_WADE 1
#define CLASS_SWIM 2
// With a class, for the lines a path is pulled into: small objects may be
// walked through (the path went through some)
#define ALLOW_SMALL 8
#define CLASS_MASK 7
#define SMALL_UNIT_METERS 0.05f      // small objects' heights, per cell
// Cells per side of a region, known to the compiler (divisions by it are shifts)
#define REGION_CELLS ((int32_t)(NAVIGATION_REGION_METERS / NAVIGATION_CELL_METERS))

// The navigation mesh's (further down)
static void mesh_free(navigation_t *navigation);
static bool mesh_allocate(navigation_t *navigation);
static int mesh_find_path(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2],
	int *waypoint_count_out, bool *complete_out);
static int mesh_line_passable(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float *blocked_x, float *blocked_z, float *blocked_height);
static int32_t mesh_rect_of_node(const navigation_t *navigation, int c, int32_t node, int32_t cell);
static int mesh_class_of(const navigation_agent_t *agent);
static bool node_sub_height(const navigation_t *navigation, int32_t node, int32_t cell, int32_t sub_x, int32_t sub_z, uint8_t room_needed,
	float *height_out, bool *blocked_out);

static const int32_t direction_x[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int32_t direction_z[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
static const float direction_length[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f };

// ---------------------------------------------------------------------------
// Nodes: a cell's terrain, or an extra surface

static int32_t cell_count(const navigation_t *navigation)
{
	return navigation->size * navigation->size;
}

static int32_t region_of_cell(const navigation_t *navigation, int32_t cell)
{
	int32_t cell_x = cell % navigation->size;
	int32_t cell_z = cell / navigation->size;

	return (cell_z / navigation->region_size) * navigation->regions_per_side + cell_x / navigation->region_size;
}

static int32_t node_cell(const navigation_t *navigation, int32_t node)
{
	int32_t cells = cell_count(navigation);

	if (node < cells)
	{
		return node;
	}
	node -= cells;
	return navigation->regions[node / NAVIGATION_REGION_EXTRA_SLOTS].surfaces[node % NAVIGATION_REGION_EXTRA_SLOTS].cell;
}

// The nodes of a cell whose coordinates are known (its terrain first; no
// divisions: the search's inner loop); returns how many
static int cell_nodes_at(const navigation_t *navigation, int32_t cell, int32_t cell_x, int32_t cell_z,
	int32_t nodes_out[1 + NAVIGATION_MAX_EXTRA_SURFACES])
{
	int count = 1;

	nodes_out[0] = cell;
	if (navigation->extra_count[cell] > 0)
	{
		int32_t region = (cell_z / REGION_CELLS) * navigation->regions_per_side + cell_x / REGION_CELLS;
		int32_t base = navigation->size * navigation->size + region * NAVIGATION_REGION_EXTRA_SLOTS + navigation->extra_first[cell];

		for (int k = 0; k < navigation->extra_count[cell]; k++)
		{
			nodes_out[count++] = base + k;
		}
	}
	return count;
}

// The nodes of a cell (its terrain first); returns how many
static int cell_nodes(const navigation_t *navigation, int32_t cell, int32_t nodes_out[1 + NAVIGATION_MAX_EXTRA_SURFACES])
{
	int count = 1;

	nodes_out[0] = cell;
	if (navigation->extra_count[cell] > 0)
	{
		int32_t region = region_of_cell(navigation, cell);
		int32_t base = cell_count(navigation) + region * NAVIGATION_REGION_EXTRA_SLOTS + navigation->extra_first[cell];

		for (int k = 0; k < navigation->extra_count[cell]; k++)
		{
			nodes_out[count++] = base + k;
		}
	}
	return count;
}

typedef struct
{
	bool passable;
	int water_class;
	float cost_factor;                 // per meter, 1 on dry flat ground
	float height;                      // the surface, meters
	bool small;                        // a small object stands there: passable, but costs a detour
	bool surface;                      // an object's top (not the terrain): its slope is not the ground's
	float extra_cost;                  // meters, for stepping in (a small object)
} node_info_t;

// No water at all at a cell's middle (the water's cell water_sample would
// read there): most of the map, where reading the flow is not worth it.
// With a whole number of water cells to a cell (the usual) that cell is
// found without floats
static bool water_dry_at(const navigation_t *navigation, const water_t *water, int32_t cell_x, int32_t cell_z, float x, float z)
{
	int32_t water_x;
	int32_t water_z;

	if (navigation->water_cells_per_cell > 0 && navigation->water_cells_for_size == water->cell_size_meters)
	{
		// k water cells to a cell: (cell + 0.5) * k + 0.5 floored, in whole
		// numbers (no division, no floats)
		int32_t k = navigation->water_cells_per_cell;

		water_x = k * cell_x + (k + 1) / 2;
		water_z = k * cell_z + (k + 1) / 2;
	}
	else
	{
		// (never below zero: truncating is flooring)
		water_x = (int32_t)(x / water->cell_size_meters + 0.5f);
		water_z = (int32_t)(z / water->cell_size_meters + 0.5f);
	}
	if (water_x >= water->size || water_z >= water->size)
	{
		return true;
	}
	// A tile with no water in it at all (a byte of a small array) spares
	// reading the cell's depth (a cache miss in a big one)
	{
		int32_t tile = (water_z / WATER_TILE_SIZE) * water->tiles_per_side + water_x / WATER_TILE_SIZE;

		if (water->tile_state[tile] == WATER_TILE_DRY && water->tile_max_depth[tile] <= 0)
		{
			return true;
		}
	}
	return water->depth[water_z * water->size + water_x] <= 0;
}

// What a node is to the agent, its cell and the cell's coordinates known
// (the search's inner loop: no divisions)
static void node_info_at(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t node, int32_t cell, int32_t cell_x, int32_t cell_z, node_info_t *info)
{
	float x = ((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS;
	float z = ((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS;
	float terrain_height = navigation->ground_height[cell];
	uint8_t clearance;
	float depth = 0.0f;
	float velocity_x = 0.0f;
	float velocity_z = 0.0f;

	int32_t cells = navigation->size * navigation->size;

	if (node < cells)
	{
		info->height = terrain_height;
		clearance = navigation->terrain_clearance[cell];
	}
	else
	{
		int32_t index = node - cells;
		const navigation_surface_t *surface =
			&navigation->regions[index / NAVIGATION_REGION_EXTRA_SLOTS].surfaces[index % NAVIGATION_REGION_EXTRA_SLOTS];

		info->height = surface->height;
		clearance = surface->clearance;
	}
	info->passable = (clearance == NAVIGATION_CLEARANCE_OPEN
		|| (float)clearance * NAVIGATION_CLEARANCE_UNIT_METERS >= agent->height_meters)
		&& (navigation->blocked == NULL || (navigation->blocked[cell] & agent->groups) == 0);
	info->water_class = CLASS_DRY;
	// (the ground's slope for the terrain; an object's top is walked as it is)
	info->surface = node >= cells;
	info->cost_factor = node < cells ? navigation->walk_factors[navigation->ground_slope[cell]] : 1.0f;
	info->small = false;
	info->extra_cost = 0.0f;
	if (node < cells && navigation->small_height[cell] > 0)
	{
		// A stone on the ground: nothing to one that steps over it, a wall to
		// one it is taller than, else in the way but passable at a price
		// (it goes around when it can, through when it must)
		float small = (float)navigation->small_height[cell] * SMALL_UNIT_METERS;

		if (small >= agent->height_meters)
		{
			info->passable = false;
		}
		else if (small > agent->step_meters)
		{
			info->small = true;
			info->extra_cost = navigation->small_detour_meters;
		}
	}
	if (!info->passable || water == NULL || (!water->iced && water_dry_at(navigation, water, cell_x, cell_z, x, z)) || !water_sample(water, x, z, &depth, &velocity_x, &velocity_z))
	{
		return;
	}
	// The ice bears: its surface, dry
	if (water->iced)
	{
		float ice;

		if (water_ice_surface(water, terrain, x, z, &ice))
		{
			info->height = SDL_max(info->height, ice);
		}
		return;
	}
	// The water's surface over this one (a deck above the water is dry)
	depth = terrain_height + depth - info->height;
	if (depth > agent->swim_depth_meters)
	{
		float speed_squared = velocity_x * velocity_x + velocity_z * velocity_z;

		info->water_class = CLASS_SWIM;
		// Swimming is slow and tiring: taken when there is no better way
		info->cost_factor = 4.0f;
		if (!agent->can_swim || speed_squared > agent->swim_meters_per_second * agent->swim_meters_per_second)
		{
			info->passable = false;
		}
	}
	else if (depth > agent->wade_depth_meters)
	{
		info->water_class = CLASS_WADE;
		info->cost_factor *= 1.5f + 2.0f * depth;
	}
}

// node_info_at for a cell's terrain (not an extra surface), lean enough to
// go into the search's inner loop whole: the same answers, the water's
// part left to node_info_at where there is any
static void terrain_node_info(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t cell, int32_t cell_x, int32_t cell_z, node_info_t *info)
{
	uint8_t clearance = navigation->terrain_clearance[cell];
	uint8_t small_units = navigation->small_height[cell];

	info->height = navigation->ground_height[cell];
	info->passable = (clearance == NAVIGATION_CLEARANCE_OPEN || (float)clearance * NAVIGATION_CLEARANCE_UNIT_METERS >= agent->height_meters)
		&& (navigation->blocked == NULL || (navigation->blocked[cell] & agent->groups) == 0);
	info->water_class = CLASS_DRY;
	info->surface = false;
	info->cost_factor = navigation->walk_factors[navigation->ground_slope[cell]];
	info->small = false;
	info->extra_cost = 0.0f;
	if (small_units > 0)
	{
		float small = (float)small_units * SMALL_UNIT_METERS;

		if (small >= agent->height_meters)
		{
			info->passable = false;
		}
		else if (small > agent->step_meters)
		{
			info->small = true;
			info->extra_cost = navigation->small_detour_meters;
		}
	}
	if (!info->passable || water == NULL)
	{
		return;
	}
	if (water->iced || !water_dry_at(navigation, water, cell_x, cell_z, ((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS,
			((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS))
	{
		node_info_at(navigation, terrain, water, agent, cell, cell, cell_x, cell_z, info);
	}
}

static void node_info(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t node, node_info_t *info)
{
	int32_t cell = node_cell(navigation, node);

	node_info_at(navigation, terrain, water, agent, node, cell, cell % navigation->size, cell / navigation->size, info);
}

#define LINK_SHUT_METERS 3.0f         // a shut door costs this much more than its length (its opening)

// Can a step go from one surface to the next, distance apart? Within a step
// up or down, or the slope it climbs; anything goes swimming. Between the
// ground and an object's top only a step: the slope is the ground's (or a
// deck's, a ramp's, from its top to its next), not a way up the side of a
// thing (a fence's top a meter up was climbed onto diagonally, its 1.4 m
// at the steepest slope, walked along and left on the other side)
static bool step_allowed(const navigation_agent_t *agent, const node_info_t *from, const node_info_t *to, float distance)
{
	if (from->water_class == CLASS_SWIM && to->water_class == CLASS_SWIM)
	{
		return true;
	}
	if (from->surface != to->surface)
	{
		return fabsf(to->height - from->height) <= agent->step_meters;
	}
	return fabsf(to->height - from->height) <= SDL_max(agent->step_meters, agent->maximum_slope * distance);
}

// What a step costs: its length times what the ground it goes onto costs
// (its slope, baked with the region: walk_factors; wading, swimming), and a
// small object's detour. Onto or off an object's top (a rock, a deck) the
// climb between the two counts instead, against the steepest the agent goes
static float step_cost(const navigation_agent_t *agent, const node_info_t *from, const node_info_t *to, float distance)
{
	if (from->surface || to->surface)
	{
		float steepness = to->water_class == CLASS_SWIM ? 0.0f
			: SDL_min(fabsf(to->height - from->height) / (distance * SDL_max(agent->maximum_slope, 0.01f)), 1.0f);

		return distance * to->cost_factor * (1.0f + 1.5f * steepness) + to->extra_cost;
	}
	return distance * to->cost_factor + to->extra_cost;
}

// The cell's node whose surface is nearest the height
static int32_t nearest_node(const navigation_t *navigation, const terrain_t *terrain, int32_t cell, float height)
{
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count = cell_nodes(navigation, cell, nodes);
	int32_t best = nodes[0];
	float best_distance = SDL_fabsf(navigation->ground_height[cell] - height);

	for (int k = 1; k < count; k++)
	{
		int32_t index = nodes[k] - cell_count(navigation);
		float surface = navigation->regions[index / NAVIGATION_REGION_EXTRA_SLOTS].surfaces[index % NAVIGATION_REGION_EXTRA_SLOTS].height;

		if (SDL_fabsf(surface - height) < best_distance)
		{
			best_distance = SDL_fabsf(surface - height);
			best = nodes[k];
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// Building the surfaces of a region

// What one piece of an object takes in a quarter meter of a cell: the room
// from bottom to top, and the highest surface on it (or below bottom if
// none). An object gives a piece per run of its height there (see
// rasterize_object)
typedef struct
{
	int32_t cell;
	float bottom;
	float top;
	float walk_top;
	int16_t type;                      // the object's type (for telling what is in the way)
	uint8_t sub;                       // which quarter meter (row by row, x first)
} object_cell_t;

typedef struct
{
	object_cell_t *items;
	int32_t count;
	int32_t capacity;
} object_cells_t;

static bool append_object_cell(object_cells_t *list, const object_cell_t *item)
{
	if (list->count == list->capacity)
	{
		int32_t capacity = SDL_max(1024, list->capacity * 2);
		object_cell_t *grown = SDL_realloc(list->items, sizeof(object_cell_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return false;
		}
		list->items = grown;
		list->capacity = capacity;
	}
	list->items[list->count++] = *item;
	return true;
}

static int compare_object_cells(const void *left, const void *right)
{
	const object_cell_t *a = left;
	const object_cell_t *b = right;

	return a->cell < b->cell ? -1 : (a->cell > b->cell ? 1 : 0);
}

// The model an object takes room with, -1 if it takes none for the
// navigation (not solid, loose, too small)
static int navigation_model(const navigation_t *navigation, const objects_t *objects, const object_instance_t *instance, float *radius_out,
	bool *small_out)
{
	const object_type_t *type = &objects->types.types[instance->type];
	int model;

	if ((type->physics != OBJECT_PHYSICS_STATIC && type->physics != OBJECT_PHYSICS_BODY) || (instance->state & OBJECT_STATE_FREE) != 0)
	{
		return -1;
	}
	model = objects_instance_model(objects, instance);
	if (model < 0 || objects->models[model].collision_index_count == 0)
	{
		return -1;
	}
	*radius_out = objects->models[model].horizontal_radius * instance->scale;
	if (2.0f * *radius_out < navigation->object_minimum_meters)
	{
		return -1;
	}
	*small_out = 2.0f * *radius_out < navigation->small_object_meters;
	return model;
}

static void rotate_point(const float rotation[4], const float point[3], float out[3])
{
	float twice_cross[3] = {
		2.0f * (rotation[1] * point[2] - rotation[2] * point[1]),
		2.0f * (rotation[2] * point[0] - rotation[0] * point[2]),
		2.0f * (rotation[0] * point[1] - rotation[1] * point[0]) };

	out[0] = point[0] + rotation[3] * twice_cross[0] + (rotation[1] * twice_cross[2] - rotation[2] * twice_cross[1]);
	out[1] = point[1] + rotation[3] * twice_cross[1] + (rotation[2] * twice_cross[0] - rotation[0] * twice_cross[2]);
	out[2] = point[2] + rotation[3] * twice_cross[2] + (rotation[0] * twice_cross[1] - rotation[1] * twice_cross[0]);
}

// One object's room in the cells of [first_x, last_x] x [first_z, last_z]
// (a part of one region): its triangles sampled every eighth of a meter,
// each quarter meter's samples sorted into height bands of BAND_METERS.
// Bands with at least BAND_GAP empty ones between them are apart: each run
// of bands is a piece of its own (its room, its top to stand on), so a
// terrace under its porch's roof is a floor with room above it, not one
// block from the ground to the roof (a wall, a pillar, sampled all the
// way up, stays one). Per quarter meter: a stair's treads beside its
// railing are treads, the railing over its edge only its own quarters'
//
// A closed model is solid inside, not only where its faces are: within a
// quarter meter, from a face looking down (an underside) up to the next
// looking up (a top); and in a band, along a row of quarters, from a face
// looking back along the row to the next looking on (a wall's two sides,
// a terrace's), if they are at most SOLID_QUARTERS_MOST apart (a face
// without its other side, a fence's single plane, fills nothing)
#define BANDS 16                     // over NAVIGATION_OBJECT_HEIGHT_METERS
#define BAND_METERS (NAVIGATION_OBJECT_HEIGHT_METERS / (float)BANDS)
#define BAND_GAP 2
#define SOLID_QUARTERS_MOST 8        // two meters: the thickest wall
#define FACE_MINUS_X 1
#define FACE_PLUS_X 2
#define FACE_MINUS_Z 4
#define FACE_PLUS_Z 8
#define FACE_DOWN 16
#define FACE_UP 32

// A band of a quarter filled from y_low to y_high (no further than the
// band)
static void fill_band(float *bottom, float *top, size_t local, float y_low, float y_high)
{
	if (y_low > y_high)
	{
		return;
	}
	bottom[local] = SDL_min(bottom[local], y_low);
	top[local] = SDL_max(top[local], y_high);
}

// The solid inside a closed model, per band (see above): faces[] the
// sides each band's samples looked to, per quarter of a box quarters_x by
// quarters_z (cell by cell: index as rasterize_object's)
static void fill_solid(float *bottom, float *top, const uint8_t *faces, int32_t width, int32_t depth, float base_y)
{
	const int32_t subcells = NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS;
	int32_t quarters_x = width * NAVIGATION_SUBCELLS;
	int32_t quarters_z = depth * NAVIGATION_SUBCELLS;

	// Up the column: an underside to the top above it
	for (int32_t qz = 0; qz < quarters_z; qz++)
	{
		for (int32_t qx = 0; qx < quarters_x; qx++)
		{
			size_t first = ((((size_t)(qz / NAVIGATION_SUBCELLS) * (size_t)width + (size_t)(qx / NAVIGATION_SUBCELLS)) * (size_t)subcells
				+ (size_t)((qz % NAVIGATION_SUBCELLS) * NAVIGATION_SUBCELLS + qx % NAVIGATION_SUBCELLS)) * BANDS);
			int32_t under = -1;

			for (int32_t band = 0; band < BANDS; band++)
			{
				if ((faces[first + (size_t)band] & FACE_DOWN) != 0 && under < 0)
				{
					under = band;
				}
				if ((faces[first + (size_t)band] & FACE_UP) != 0 && under >= 0)
				{
					float y_low = bottom[first + (size_t)under];
					float y_high = top[first + (size_t)band];

					for (int32_t fill = under; fill <= band; fill++)
					{
						float band_low = base_y + (float)fill * BAND_METERS;

						fill_band(bottom, top, first + (size_t)fill, SDL_max(y_low, band_low), SDL_min(y_high, band_low + BAND_METERS));
					}
					under = -1;
				}
			}
		}
	}
	// Along the rows, x then z: a side looking back to the next looking on
	for (int axis = 0; axis < 2; axis++)
	{
		int32_t along_count = axis == 0 ? quarters_x : quarters_z;
		int32_t across_count = axis == 0 ? quarters_z : quarters_x;
		uint8_t back = axis == 0 ? FACE_MINUS_X : FACE_MINUS_Z;
		uint8_t on = axis == 0 ? FACE_PLUS_X : FACE_PLUS_Z;

		for (int32_t across = 0; across < across_count; across++)
		{
			for (int32_t band = 0; band < BANDS; band++)
			{
				int32_t start = -1;
				size_t start_local = 0;

				for (int32_t along = 0; along < along_count; along++)
				{
					int32_t qx = axis == 0 ? along : across;
					int32_t qz = axis == 0 ? across : along;
					size_t local = ((((size_t)(qz / NAVIGATION_SUBCELLS) * (size_t)width + (size_t)(qx / NAVIGATION_SUBCELLS)) * (size_t)subcells
						+ (size_t)((qz % NAVIGATION_SUBCELLS) * NAVIGATION_SUBCELLS + qx % NAVIGATION_SUBCELLS)) * BANDS) + (size_t)band;

					// (a side looking back: the solid starts here, whatever came
					// before; one looking on ends it, the same quarter's too: a
					// wall thinner than a quarter)
					if ((faces[local] & back) != 0)
					{
						start = along;
						start_local = local;
					}
					if ((faces[local] & on) != 0 && start >= 0)
					{
						if (along - start <= SOLID_QUARTERS_MOST)
						{
							float y_low = SDL_min(bottom[start_local], bottom[local]);
							float y_high = SDL_max(top[start_local], top[local]);

							for (int32_t fill = start; fill <= along; fill++)
							{
								int32_t fill_x = axis == 0 ? fill : across;
								int32_t fill_z = axis == 0 ? across : fill;
								size_t fill_local = ((((size_t)(fill_z / NAVIGATION_SUBCELLS) * (size_t)width + (size_t)(fill_x / NAVIGATION_SUBCELLS))
									* (size_t)subcells + (size_t)((fill_z % NAVIGATION_SUBCELLS) * NAVIGATION_SUBCELLS + fill_x % NAVIGATION_SUBCELLS))
									* BANDS) + (size_t)band;

								fill_band(bottom, top, fill_local, y_low, y_high);
							}
						}
						start = -1;
					}
				}
			}
		}
	}
}

static void rasterize_object(const navigation_t *navigation, const objects_t *objects, const object_instance_t *instance, int model_index,
	int32_t first_x, int32_t first_z, int32_t last_x, int32_t last_z, object_cells_t *out)
{
	const object_model_t *model = &objects->models[model_index];
	int32_t width = last_x - first_x + 1;
	int32_t depth = last_z - first_z + 1;
	float base_y = objects_instance_base_y(instance);
	float ceiling = base_y + NAVIGATION_OBJECT_HEIGHT_METERS;
	const int32_t subcells = NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS;
	// Per quarter meter and band: the lowest and highest sample, the highest
	// walkable one
	size_t band_count = (size_t)width * (size_t)depth * (size_t)subcells * BANDS;
	float *bottom = SDL_malloc(sizeof(float) * 3 * band_count);
	uint8_t *faces = SDL_calloc(band_count, 1);
	float *top;
	float *walk_top;

	if (bottom == NULL || faces == NULL)
	{
		SDL_free(bottom);
		SDL_free(faces);
		return;
	}
	top = bottom + band_count;
	walk_top = top + band_count;
	for (size_t i = 0; i < band_count; i++)
	{
		bottom[i] = 1e30f;
		top[i] = -1e30f;
		walk_top[i] = -1e30f;
	}
	// Only the solid triangles: leaves and reeds (cut out by their texture's
	// alpha) and passable branches are neither in the way nor something to
	// stand on, actors brush through them (object_model_t)
	for (int t = 0; t + 2 < model->collision_index_count; t += 3)
	{
		float corners[3][3];
		float edge_a[3];
		float edge_b[3];
		float normal[3];
		float normal_length;
		float longest;
		bool walkable;
		uint8_t facing = 0;
		int steps;

		for (int k = 0; k < 3; k++)
		{
			const float *position = &model->collision_positions[3 * model->collision_indices[t + k]];
			float scaled[3] = { position[0] * instance->scale, position[1] * instance->scale, position[2] * instance->scale };
			float turned[3];

			rotate_point(instance->rotation, scaled, turned);
			corners[k][0] = instance->x + turned[0];
			corners[k][1] = base_y + turned[1];
			corners[k][2] = instance->z + turned[2];
		}
		if (corners[0][1] > ceiling && corners[1][1] > ceiling && corners[2][1] > ceiling)
		{
			continue;                  // a crown, a roof: above anyone
		}
		for (int k = 0; k < 3; k++)
		{
			edge_a[k] = corners[1][k] - corners[0][k];
			edge_b[k] = corners[2][k] - corners[0][k];
		}
		normal[0] = edge_a[1] * edge_b[2] - edge_a[2] * edge_b[1];
		normal[1] = edge_a[2] * edge_b[0] - edge_a[0] * edge_b[2];
		normal[2] = edge_a[0] * edge_b[1] - edge_a[1] * edge_b[0];
		normal_length = SDL_sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		if (normal_length < 1e-9f)
		{
			continue;
		}
		walkable = normal[1] / normal_length >= WALKABLE_NORMAL_Y;
		// (where it looks: for the solid behind it, fill_solid)
		facing |= normal[0] < -0.5f * normal_length ? FACE_MINUS_X : 0;
		facing |= normal[0] > 0.5f * normal_length ? FACE_PLUS_X : 0;
		facing |= normal[2] < -0.5f * normal_length ? FACE_MINUS_Z : 0;
		facing |= normal[2] > 0.5f * normal_length ? FACE_PLUS_Z : 0;
		facing |= normal[1] < -0.5f * normal_length ? FACE_DOWN : 0;
		facing |= normal[1] > 0.5f * normal_length ? FACE_UP : 0;
		longest = SDL_max(SDL_sqrtf(edge_a[0] * edge_a[0] + edge_a[1] * edge_a[1] + edge_a[2] * edge_a[2]),
			SDL_sqrtf(edge_b[0] * edge_b[0] + edge_b[1] * edge_b[1] + edge_b[2] * edge_b[2]));
		steps = SDL_clamp((int)SDL_ceilf(longest / SAMPLE_METERS), 1, 256);
		// Points of the triangle on a grid of barycentric steps, edges included
		for (int i = 0; i <= steps; i++)
		{
			for (int j = 0; i + j <= steps; j++)
			{
				float a = (float)i / (float)steps;
				float b = (float)j / (float)steps;
				float x = corners[0][0] + edge_a[0] * a + edge_b[0] * b;
				float y = corners[0][1] + edge_a[1] * a + edge_b[1] * b;
				float z = corners[0][2] + edge_a[2] * a + edge_b[2] * b;
				int32_t cell_x = (int32_t)SDL_floorf(x / NAVIGATION_CELL_METERS) - first_x;
				int32_t cell_z = (int32_t)SDL_floorf(z / NAVIGATION_CELL_METERS) - first_z;
				float inside_x;
				float inside_z;
				int32_t sub_x;
				int32_t sub_z;
				int32_t band;
				size_t local;

				if (y > ceiling || cell_x < 0 || cell_z < 0 || cell_x >= width || cell_z >= depth)
				{
					continue;
				}
				inside_x = x / NAVIGATION_CELL_METERS - SDL_floorf(x / NAVIGATION_CELL_METERS);
				inside_z = z / NAVIGATION_CELL_METERS - SDL_floorf(z / NAVIGATION_CELL_METERS);
				sub_x = SDL_clamp((int32_t)(inside_x * NAVIGATION_SUBCELLS), 0, NAVIGATION_SUBCELLS - 1);
				sub_z = SDL_clamp((int32_t)(inside_z * NAVIGATION_SUBCELLS), 0, NAVIGATION_SUBCELLS - 1);
				band = SDL_clamp((int32_t)SDL_floorf((y - base_y) / BAND_METERS), 0, BANDS - 1);
				local = ((((size_t)cell_z * (size_t)width + (size_t)cell_x) * (size_t)subcells + (size_t)(sub_z * NAVIGATION_SUBCELLS + sub_x))
					* BANDS) + (size_t)band;
				bottom[local] = SDL_min(bottom[local], y);
				top[local] = SDL_max(top[local], y);
				if (walkable)
				{
					walk_top[local] = SDL_max(walk_top[local], y);
				}
				faces[local] |= facing;
			}
		}
	}
	fill_solid(bottom, top, faces, width, depth, base_y);
	SDL_free(faces);
	// Each quarter meter's runs of bands, a piece each
	for (int32_t cell_z = 0; cell_z < depth; cell_z++)
	{
		for (int32_t cell_x = 0; cell_x < width; cell_x++)
		{
			for (int32_t sub = 0; sub < subcells; sub++)
			{
				size_t first_band = (((size_t)cell_z * (size_t)width + (size_t)cell_x) * (size_t)subcells + (size_t)sub) * BANDS;
				object_cell_t item;
				bool open = false;
				int32_t empty = 0;

				for (int32_t band = 0; band <= BANDS; band++)
				{
					bool filled = band < BANDS && top[first_band + (size_t)band] >= bottom[first_band + (size_t)band];

					if (filled)
					{
						size_t local = first_band + (size_t)band;

						if (!open)
						{
							item.cell = (first_z + cell_z) * navigation->size + first_x + cell_x;
							item.bottom = 1e30f;
							item.top = -1e30f;
							item.walk_top = -1e30f;
							item.type = (int16_t)instance->type;
							item.sub = (uint8_t)sub;
							open = true;
						}
						item.bottom = SDL_min(item.bottom, bottom[local]);
						item.top = SDL_max(item.top, top[local]);
						item.walk_top = SDL_max(item.walk_top, walk_top[local]);
						empty = 0;
						continue;
					}
					empty++;
					// (a run ends at BAND_GAP empty bands, or at the top)
					if (open && (empty >= BAND_GAP || band == BANDS))
					{
						append_object_cell(out, &item);
						open = false;
					}
				}
			}
		}
	}
	SDL_free(bottom);
}

static uint8_t clearance_units(float meters)
{
	if (meters >= (float)(NAVIGATION_CLEARANCE_OPEN - 1) * NAVIGATION_CLEARANCE_UNIT_METERS)
	{
		return NAVIGATION_CLEARANCE_OPEN - 1;
	}
	return (uint8_t)SDL_max(0.0f, SDL_floorf(meters / NAVIGATION_CLEARANCE_UNIT_METERS));
}

// The free room above a surface at height: up to the lowest object that
// reaches above it (one that reaches through it leaves none)
static uint8_t clearance_above(const object_cell_t *items, int32_t count, float height, int16_t *type_out)
{
	float lowest = 1e30f;

	*type_out = -1;
	for (int32_t k = 0; k < count; k++)
	{
		if (items[k].top > height + MERGE_METERS && items[k].bottom < lowest)
		{
			lowest = items[k].bottom;
			*type_out = items[k].type;
		}
	}
	if (lowest >= 1e29f)
	{
		return NAVIGATION_CLEARANCE_OPEN;
	}
	return clearance_units(lowest - height);
}

static void region_bounds(const navigation_t *navigation, int32_t region_index, int32_t *first_x, int32_t *first_z, int32_t *last_x,
	int32_t *last_z)
{
	*first_x = (region_index % navigation->regions_per_side) * navigation->region_size;
	*first_z = (region_index / navigation->regions_per_side) * navigation->region_size;
	*last_x = SDL_min(*first_x + navigation->region_size, navigation->size) - 1;
	*last_z = SDL_min(*first_z + navigation->region_size, navigation->size) - 1;
}

// The cells of box (x, z first and last, in one region) about to be built:
// nothing in them yet (their surfaces' slots are left as they were: a
// whole region starts its surfaces again, a part packs the others' first)
static void clear_box(navigation_t *navigation, const terrain_t *terrain, const int32_t box[4])
{
	for (int32_t cell_z = box[1]; cell_z <= box[3]; cell_z++)
	{
		for (int32_t cell_x = box[0]; cell_x <= box[2]; cell_x++)
		{
			int32_t cell = cell_z * navigation->size + cell_x;

			navigation->ground_height[cell] = terrain_sample_height_meters(terrain, ((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS,
				((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS);
			navigation->terrain_clearance[cell] = NAVIGATION_CLEARANCE_OPEN;
			navigation->terrain_blocker[cell] = -1;
			navigation->object_top[cell] = 0;
			navigation->small_height[cell] = 0;
			navigation->extra_count[cell] = 0;
			navigation->extra_first[cell] = 0;
		}
	}
}

// A region's cells as a box
static void region_box(const navigation_t *navigation, int32_t region_index, int32_t box_out[4])
{
	region_bounds(navigation, region_index, &box_out[0], &box_out[1], &box_out[2], &box_out[3]);
}

// How steep the ground is in the cells of box (in one region whose
// heights are up to date): the fall across the cell's two neighbors each
// way (from their heights, or the terrain's for those of other regions)
static void work_out_slopes(navigation_t *navigation, const terrain_t *terrain, int32_t region_index, const int32_t box[4])
{
	int32_t region[4];

	region_box(navigation, region_index, region);
	for (int32_t cell_z = box[1]; cell_z <= box[3]; cell_z++)
	{
		for (int32_t cell_x = box[0]; cell_x <= box[2]; cell_x++)
		{
			float around[4];
			float fall_x;
			float fall_z;
			float degrees;

			for (int side = 0; side < 4; side++)
			{
				int32_t next_x = cell_x + (side == 0 ? 1 : side == 1 ? -1 : 0);
				int32_t next_z = cell_z + (side == 2 ? 1 : side == 3 ? -1 : 0);

				if (next_x >= region[0] && next_x <= region[2] && next_z >= region[1] && next_z <= region[3])
				{
					around[side] = navigation->ground_height[next_z * navigation->size + next_x];
				}
				else
				{
					around[side] = terrain_sample_height_meters(terrain, ((float)next_x + 0.5f) * NAVIGATION_CELL_METERS,
						((float)next_z + 0.5f) * NAVIGATION_CELL_METERS);
				}
			}
			fall_x = (around[0] - around[1]) / (2.0f * NAVIGATION_CELL_METERS);
			fall_z = (around[2] - around[3]) / (2.0f * NAVIGATION_CELL_METERS);
			degrees = SDL_atanf(SDL_sqrtf(fall_x * fall_x + fall_z * fall_z)) * (180.0f / 3.14159265f);
			navigation->ground_slope[cell_z * navigation->size + cell_x] = (uint8_t)SDL_clamp(degrees / NAVIGATION_SLOPE_UNIT_DEGREES + 0.5f, 0.0f, 255.0f);
		}
	}
}

void navigation_set_slope_cost(navigation_t *navigation, float full_degrees, float extra_cost)
{
	for (int value = 0; value < 256; value++)
	{
		float degrees = (float)value * NAVIGATION_SLOPE_UNIT_DEGREES;

		navigation->walk_factors[value] = 1.0f + SDL_max(extra_cost, 0.0f) * SDL_min(degrees / SDL_max(full_degrees, 1.0f), 1.0f);
	}
}

// The surfaces of the region's cells that keep theirs, one after another
// from the first slot (those of a part being built again were let go):
// the part's new ones go after them
static void pack_surfaces(navigation_t *navigation, int32_t region_index)
{
	navigation_region_t *region = &navigation->regions[region_index];
	navigation_surface_t *packed;
	int32_t box[4];
	int32_t count = 0;

	if (region->surfaces == NULL || region->surface_count == 0)
	{
		return;
	}
	packed = SDL_malloc(sizeof(navigation_surface_t) * (size_t)region->surface_count);
	if (packed == NULL)
	{
		return;
	}
	region_box(navigation, region_index, box);
	for (int32_t cell_z = box[1]; cell_z <= box[3]; cell_z++)
	{
		for (int32_t cell_x = box[0]; cell_x <= box[2]; cell_x++)
		{
			int32_t cell = cell_z * navigation->size + cell_x;
			int32_t first = navigation->extra_first[cell];

			if (navigation->extra_count[cell] == 0)
			{
				continue;
			}
			navigation->extra_first[cell] = (uint16_t)count;
			for (int k = 0; k < navigation->extra_count[cell]; k++)
			{
				packed[count++] = region->surfaces[first + k];
			}
		}
	}
	SDL_memcpy(region->surfaces, packed, sizeof(navigation_surface_t) * (size_t)count);
	region->surface_count = count;
	SDL_free(packed);
}

// The region's surfaces from what its objects take, cell by cell
// A cell's surfaces from its pieces: each quarter meter's tops (with the
// room over them there, up to the lowest piece over them in that quarter;
// one with no room worth having left out), gathered low to high into
// surfaces where they follow each other within SURFACE_CHAIN_METERS (a
// stair's treads one surface, its quarters at each tread's height; a
// porch's roof another, two meters up). A surface: its highest point, its
// quarters under it, its room the most four of its quarters have together
// (a railing over a stair's edge does not take the treads' room), and at
// least MINIMUM_SURFACE_SUBCELLS of the cell, else nothing to stand on
#define SURFACE_CHAIN_METERS 0.4f
#define SURFACE_LEAST_ROOM_METERS 0.3f

typedef struct
{
	float height;
	uint8_t clearance;
	uint8_t sub;
} surface_candidate_t;

static int compare_candidates(const void *left, const void *right)
{
	const surface_candidate_t *a = left;
	const surface_candidate_t *b = right;

	return a->height < b->height ? -1 : a->height > b->height ? 1 : 0;
}

static int compare_clearances(const void *left, const void *right)
{
	uint8_t a = *(const uint8_t *)left;
	uint8_t b = *(const uint8_t *)right;

	return (int)b - (int)a;
}

static void finish_region(navigation_t *navigation, const terrain_t *terrain, int32_t region_index, object_cells_t *list)
{
	navigation_region_t *region = &navigation->regions[region_index];
	static surface_candidate_t candidates[4096];

	if (list->count == 0)
	{
		return;
	}
	SDL_qsort(list->items, (size_t)list->count, sizeof(object_cell_t), compare_object_cells);
	for (int32_t start = 0; start < list->count; )
	{
		int32_t cell = list->items[start].cell;
		int32_t end = start;
		float terrain_height;
		int candidate_count = 0;

		while (end < list->count && list->items[end].cell == cell)
		{
			end++;
		}
		terrain_height = navigation->ground_height[cell];
		// (the terrain: anything over it anywhere in the cell takes it)
		navigation->terrain_clearance[cell] = clearance_above(&list->items[start], end - start, terrain_height,
			&navigation->terrain_blocker[cell]);
		// Each piece's top, with the room over it in its own quarter
		for (int32_t k = start; k < end && candidate_count < (int)SDL_arraysize(candidates); k++)
		{
			const object_cell_t *item = &list->items[k];
			float lowest = 1e30f;

			if (item->walk_top <= terrain_height + MERGE_METERS)
			{
				continue;
			}
			for (int32_t other = start; other < end; other++)
			{
				const object_cell_t *above = &list->items[other];

				if (above->sub == item->sub && above->top > item->walk_top + 0.1f)
				{
					lowest = SDL_min(lowest, above->bottom);
				}
			}
			if (lowest < item->walk_top + SURFACE_LEAST_ROOM_METERS)
			{
				continue;
			}
			candidates[candidate_count].height = item->walk_top;
			candidates[candidate_count].clearance = lowest >= 1e29f ? NAVIGATION_CLEARANCE_OPEN : clearance_units(lowest - item->walk_top);
			candidates[candidate_count].sub = item->sub;
			candidate_count++;
		}
		SDL_qsort(candidates, (size_t)candidate_count, sizeof(surface_candidate_t), compare_candidates);
		for (int first = 0; first < candidate_count; )
		{
			int last = first;
			float sub_height[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
			uint8_t sub_clearance[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
			uint8_t clearances[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
			float run_height[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
			uint8_t run_clearance[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
			int covered = 0;
			float height;
			navigation_surface_t *surface;

			while (last + 1 < candidate_count && candidates[last + 1].height - candidates[last].height <= SURFACE_CHAIN_METERS)
			{
				last++;
			}
			for (int sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
			{
				run_height[sub] = -1e30f;
				run_clearance[sub] = 0;
			}
			// (a quarter's highest of the run, and its room there)
			for (int k = first; k <= last; k++)
			{
				if (candidates[k].height > run_height[candidates[k].sub])
				{
					run_height[candidates[k].sub] = candidates[k].height;
					run_clearance[candidates[k].sub] = candidates[k].clearance;
				}
			}
			first = last + 1;
			// Its quarters that touch, apart: a wall through the cell (its
			// quarters no surface) leaves the floor on either side of it two
			// surfaces, not one way through the wall
			for (int seed = 0; seed < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; seed++)
			{
				int stack[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
				int stack_count = 0;

				if (run_height[seed] < -1e29f)
				{
					continue;
				}
				covered = 0;
				height = -1e30f;
				for (int sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
				{
					sub_height[sub] = -1e30f;
					sub_clearance[sub] = 0;
				}
				stack[stack_count++] = seed;
				sub_height[seed] = run_height[seed];
				sub_clearance[seed] = run_clearance[seed];
				run_height[seed] = -1e30f;
				while (stack_count > 0)
				{
					int sub = stack[--stack_count];
					int sub_x = sub % NAVIGATION_SUBCELLS;
					int sub_z = sub / NAVIGATION_SUBCELLS;

					height = SDL_max(height, sub_height[sub]);
					clearances[covered++] = sub_clearance[sub];
					for (int d = 0; d < 4; d++)
					{
						int next_x = sub_x + direction_x[d];
						int next_z = sub_z + direction_z[d];
						int next;

						if (next_x < 0 || next_z < 0 || next_x >= NAVIGATION_SUBCELLS || next_z >= NAVIGATION_SUBCELLS)
						{
							continue;
						}
						next = next_z * NAVIGATION_SUBCELLS + next_x;
						if (run_height[next] > -1e29f)
						{
							sub_height[next] = run_height[next];
							sub_clearance[next] = run_clearance[next];
							run_height[next] = -1e30f;
							stack[stack_count++] = next;
						}
					}
				}
				if (covered < MINIMUM_SURFACE_SUBCELLS || navigation->extra_count[cell] >= NAVIGATION_MAX_EXTRA_SURFACES)
				{
					continue;
				}
				if (region->surface_count == NAVIGATION_REGION_EXTRA_SLOTS)
				{
					SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Navigation: region %d has more object surfaces than %d; some are left out",
						region_index, NAVIGATION_REGION_EXTRA_SLOTS);
					break;
				}
				if (region->surfaces == NULL)
				{
					region->surfaces = SDL_malloc(sizeof(navigation_surface_t) * NAVIGATION_REGION_EXTRA_SLOTS);
					if (region->surfaces == NULL)
					{
						break;
					}
				}
				SDL_qsort(clearances, (size_t)covered, 1, compare_clearances);
				surface = &region->surfaces[region->surface_count];
				surface->height = height;
				surface->cell = cell;
				surface->clearance = clearances[MINIMUM_SURFACE_SUBCELLS - 1];
				for (int sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
				{
					surface->below[sub] = sub_height[sub] < -1e29f ? NAVIGATION_SUBCELL_NONE
						: (uint8_t)SDL_clamp(SDL_ceilf((height - sub_height[sub]) / NAVIGATION_SUBCELL_UNIT_METERS), 0.0f,
							(float)(NAVIGATION_SUBCELL_BLOCKED - 1));
					surface->room[sub] = sub_height[sub] < -1e29f ? 0 : sub_clearance[sub];
					// (not its, and something stands there at its height: a
					// wall, a post; the way to the edge stops there)
					if (surface->below[sub] == NAVIGATION_SUBCELL_NONE)
					{
						for (int32_t item = start; item < end; item++)
						{
							if (list->items[item].sub == sub && list->items[item].bottom < height + 0.2f && list->items[item].top > height + 0.3f)
							{
								surface->below[sub] = NAVIGATION_SUBCELL_BLOCKED;
								break;
							}
						}
					}
				}
				if (surface->clearance == 0)
				{
					continue;
				}
				if (navigation->extra_count[cell] == 0)
				{
					navigation->extra_first[cell] = (uint16_t)region->surface_count;
				}
				region->surface_count++;
				navigation->extra_count[cell]++;
			}
		}
		start = end;
	}
}

// What an object adds to the heights flyers go over, in the cells of box
// (x, z first and last, inside one region being built): its whole box,
// placed, turned and scaled (every standing object, small ones and leaves
// too; loose ones fly about and are left out)
static void raise_object_tops(navigation_t *navigation, const objects_t *objects, const object_instance_t *instance, const int32_t box[4])
{
	int model_index;
	const object_model_t *model;
	float base_y;
	float minimum_x = 1e30f;
	float minimum_z = 1e30f;
	float maximum_x = -1e30f;
	float maximum_z = -1e30f;
	float top = -1e30f;
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;

	if ((instance->state & OBJECT_STATE_FREE) != 0)
	{
		return;
	}
	model_index = objects_instance_model(objects, instance);
	if (model_index < 0)
	{
		return;
	}
	model = &objects->models[model_index];
	base_y = objects_instance_base_y(instance);
	for (int corner = 0; corner < 8; corner++)
	{
		float point[3] = {
			((corner & 1) ? model->bounds_maximum.x : model->bounds_minimum.x) * instance->scale,
			((corner & 2) ? model->bounds_maximum.y : model->bounds_minimum.y) * instance->scale,
			((corner & 4) ? model->bounds_maximum.z : model->bounds_minimum.z) * instance->scale };
		float turned[3];

		rotate_point(instance->rotation, point, turned);
		minimum_x = SDL_min(minimum_x, instance->x + turned[0]);
		maximum_x = SDL_max(maximum_x, instance->x + turned[0]);
		minimum_z = SDL_min(minimum_z, instance->z + turned[2]);
		maximum_z = SDL_max(maximum_z, instance->z + turned[2]);
		top = SDL_max(top, base_y + turned[1]);
	}
	// (into the box only: the other regions are built by other threads,
	// each gathering the same object)
	first_x = SDL_max((int32_t)SDL_floorf(minimum_x / NAVIGATION_CELL_METERS), box[0]);
	first_z = SDL_max((int32_t)SDL_floorf(minimum_z / NAVIGATION_CELL_METERS), box[1]);
	last_x = SDL_min((int32_t)SDL_floorf(maximum_x / NAVIGATION_CELL_METERS), box[2]);
	last_z = SDL_min((int32_t)SDL_floorf(maximum_z / NAVIGATION_CELL_METERS), box[3]);
	for (int32_t cell_z = first_z; cell_z <= last_z; cell_z++)
	{
		for (int32_t cell_x = first_x; cell_x <= last_x; cell_x++)
		{
			int32_t cell = cell_z * navigation->size + cell_x;
			float above = top - navigation->ground_height[cell];
			uint8_t units;

			if (above <= 0.0f)
			{
				continue;
			}
			units = (uint8_t)SDL_min(SDL_ceilf(above / NAVIGATION_OBJECT_TOP_UNIT_METERS), 255.0f);
			navigation->object_top[cell] = SDL_max(navigation->object_top[cell], units);
		}
	}
}

// Scratch room for object indices (one per thread building regions)
typedef struct
{
	int32_t *items;
	int32_t capacity;
} object_scratch_t;

static bool grow_object_scratch(object_scratch_t *scratch, int32_t needed)
{
	if (needed > scratch->capacity)
	{
		int32_t capacity = SDL_max(needed, scratch->capacity * 2);
		int32_t *grown = SDL_realloc(scratch->items, sizeof(int32_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return false;
		}
		scratch->items = grown;
		scratch->capacity = capacity;
	}
	return true;
}

// The objects that may reach into a region: those of the objects' grid cells
// over it and a margin around it (as big as the biggest object's reach), or
// all of them while the grid is being sorted again. Indices go to
// scratch; returns how many.
static int32_t gather_region_objects(const navigation_t *navigation, const objects_t *objects, int32_t region_index, object_scratch_t *scratch)
{
	int32_t count = 0;
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;
	bool use_grid = !objects->grid_dirty && objects->grid_first != NULL && objects->grid_size > 0;
	int32_t needed = use_grid ? 0 : objects->instance_count;

	region_bounds(navigation, region_index, &first_x, &first_z, &last_x, &last_z);
	if (use_grid)
	{
		float margin = navigation->object_margin_meters;
		int32_t grid_first_x = SDL_clamp((int32_t)SDL_floorf(((float)first_x * NAVIGATION_CELL_METERS - margin) / OBJECTS_GRID_CELL_METERS), 0,
			objects->grid_size - 1);
		int32_t grid_first_z = SDL_clamp((int32_t)SDL_floorf(((float)first_z * NAVIGATION_CELL_METERS - margin) / OBJECTS_GRID_CELL_METERS), 0,
			objects->grid_size - 1);
		int32_t grid_last_x = SDL_clamp((int32_t)SDL_floorf(((float)(last_x + 1) * NAVIGATION_CELL_METERS + margin) / OBJECTS_GRID_CELL_METERS), 0,
			objects->grid_size - 1);
		int32_t grid_last_z = SDL_clamp((int32_t)SDL_floorf(((float)(last_z + 1) * NAVIGATION_CELL_METERS + margin) / OBJECTS_GRID_CELL_METERS), 0,
			objects->grid_size - 1);

		for (int32_t grid_z = grid_first_z; grid_z <= grid_last_z; grid_z++)
		{
			int32_t row = grid_z * objects->grid_size;

			needed += objects->grid_first[row + grid_last_x + 1] - objects->grid_first[row + grid_first_x];
		}
		// (and those added since the sort, at the list's end: a few)
		needed += SDL_max(objects->instance_count - objects->sorted_count, 0);
		if (!grow_object_scratch(scratch, needed))
		{
			return 0;
		}
		for (int32_t grid_z = grid_first_z; grid_z <= grid_last_z; grid_z++)
		{
			int32_t row = grid_z * objects->grid_size;

			// The cells of a row are one run of instances
			for (int i = objects->grid_first[row + grid_first_x]; i < objects->grid_first[row + grid_last_x + 1]; i++)
			{
				scratch->items[count++] = i;
			}
		}
		{
			float minimum_x = (float)first_x * NAVIGATION_CELL_METERS - margin;
			float minimum_z = (float)first_z * NAVIGATION_CELL_METERS - margin;
			float maximum_x = (float)(last_x + 1) * NAVIGATION_CELL_METERS + margin;
			float maximum_z = (float)(last_z + 1) * NAVIGATION_CELL_METERS + margin;

			for (int i = objects->sorted_count; i < objects->instance_count; i++)
			{
				const object_instance_t *instance = &objects->instances[i];

				if (instance->x >= minimum_x && instance->x <= maximum_x && instance->z >= minimum_z && instance->z <= maximum_z)
				{
					scratch->items[count++] = i;
				}
			}
		}
		return count;
	}
	if (!grow_object_scratch(scratch, needed))
	{
		return 0;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		scratch->items[count++] = i;
	}
	return count;
}

// Whether an object reaches into a box of cells (x, z first and last), by
// its radius
static bool object_reaches_box(const object_instance_t *instance, float radius, const int32_t box[4])
{
	return instance->x + radius >= (float)box[0] * NAVIGATION_CELL_METERS && instance->x - radius < (float)(box[2] + 1) * NAVIGATION_CELL_METERS
		&& instance->z + radius >= (float)box[1] * NAVIGATION_CELL_METERS && instance->z - radius < (float)(box[3] + 1) * NAVIGATION_CELL_METERS;
}

// Whether an object reaches into a region (its box, by its radius)
static bool object_reaches_region(const navigation_t *navigation, const object_instance_t *instance, float radius, int32_t region_index)
{
	int32_t box[4];

	region_box(navigation, region_index, box);
	return object_reaches_box(instance, radius, box);
}

// Keeps an object as the signature sees it (grows the list)
static void keep_print(navigation_region_t *region, uint32_t id, int type, int model, const float fields[8], float reach_meters)
{
	if (region->print_count == region->print_capacity)
	{
		int32_t capacity = region->print_capacity > 0 ? region->print_capacity * 2 : 64;
		navigation_object_print_t *grown = SDL_realloc(region->prints, sizeof(navigation_object_print_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		region->prints = grown;
		region->print_capacity = capacity;
	}
	region->prints[region->print_count].id = id;
	region->prints[region->print_count].type = type;
	region->prints[region->print_count].model = model;
	SDL_memcpy(region->prints[region->print_count].fields, fields, sizeof(float) * 8);
	region->prints[region->print_count].reach_meters = reach_meters;
	region->print_count++;
}

static int compare_prints(const void *left, const void *right)
{
	uint32_t a = ((const navigation_object_print_t *)left)->id;
	uint32_t b = ((const navigation_object_print_t *)right)->id;

	return a < b ? -1 : (a > b ? 1 : 0);
}

// A number from the standing objects reaching into a region (every one
// counts, the heights flyers go over too; loose ones are left out, they
// move all the time): a region whose number changed is built again (the
// cells round what changed). The height of those the navigation stands on
// or goes round counts in steps (OBJECT_HEIGHT_STEP_METERS), so following
// the ground by a few centimeters of erosion changes nothing; that of the
// others (a bush, a parasol: only the flyers' heights) not at all.
// prints_out: each of them as seen, sorted by id (NULL: not wanted)
static uint64_t region_signature(const navigation_t *navigation, const objects_t *objects, int32_t region_index, object_scratch_t *scratch,
	navigation_region_t *prints_out)
{
	int32_t count = gather_region_objects(navigation, objects, region_index, scratch);
	uint64_t signature = 0;

	if (prints_out != NULL)
	{
		prints_out->print_count = 0;
	}
	for (int32_t k = 0; k < count; k++)
	{
		const object_instance_t *instance = &objects->instances[scratch->items[k]];
		int model = (instance->state & OBJECT_STATE_FREE) != 0 ? -1 : objects_instance_model(objects, instance);
		uint64_t hash = 1469598103934665603ull;
		float fields[8];
		float radius;
		float unused_radius;
		bool unused_small;
		bool stood_on;

		if (model < 0)
		{
			continue;
		}
		radius = objects->models[model].horizontal_radius * instance->scale;
		if (!object_reaches_region(navigation, instance, radius, region_index))
		{
			continue;
		}
		stood_on = navigation_model(navigation, objects, instance, &unused_radius, &unused_small) >= 0;
		fields[0] = instance->x;
		fields[1] = instance->z;
		fields[2] = stood_on ? SDL_floorf(objects_instance_base_y(instance) / OBJECT_HEIGHT_STEP_METERS) : 0.0f;
		fields[3] = instance->scale;
		SDL_memcpy(&fields[4], instance->rotation, sizeof(float) * 4);
		for (size_t k_byte = 0; k_byte < sizeof(fields); k_byte++)
		{
			hash = (hash ^ ((const uint8_t *)fields)[k_byte]) * 1099511628211ull;
		}
		hash = (hash ^ (uint64_t)model) * 1099511628211ull;
		// A sum: the objects' order does not matter
		signature += hash;
		if (prints_out != NULL)
		{
			keep_print(prints_out, instance->id, instance->type, model, fields, radius);
		}
	}
	if (prints_out != NULL && prints_out->print_count > 1)
	{
		SDL_qsort(prints_out->prints, (size_t)prints_out->print_count, sizeof(navigation_object_print_t), compare_prints);
	}
	return signature;
}

// The cells of a print's object (a cell more round it, and half as much
// again for its turned box's corners: its tops for the flyers) grown into
// box (x, z first and last)
static void grow_box_by_print(const navigation_t *navigation, const navigation_object_print_t *print, int32_t box[4])
{
	float reach = 1.5f * print->reach_meters + NAVIGATION_CELL_METERS;

	(void)navigation;
	box[0] = SDL_min(box[0], (int32_t)SDL_floorf((print->fields[0] - reach) / NAVIGATION_CELL_METERS));
	box[1] = SDL_min(box[1], (int32_t)SDL_floorf((print->fields[1] - reach) / NAVIGATION_CELL_METERS));
	box[2] = SDL_max(box[2], (int32_t)SDL_floorf((print->fields[0] + reach) / NAVIGATION_CELL_METERS));
	box[3] = SDL_max(box[3], (int32_t)SDL_floorf((print->fields[1] + reach) / NAVIGATION_CELL_METERS));
}

// What changed among a region's objects since it was last built: the cells
// round those that went, came or moved (where they were and where they
// are), within the region, to box_out. Returns how many changed
static int changed_box(const navigation_t *navigation, int32_t region_index, const navigation_region_t *now, int32_t box_out[4])
{
	const navigation_region_t *before = &navigation->regions[region_index];
	int32_t region[4];
	int changes = 0;
	int32_t a = 0;
	int32_t b = 0;

	box_out[0] = INT32_MAX;
	box_out[1] = INT32_MAX;
	box_out[2] = INT32_MIN;
	box_out[3] = INT32_MIN;
	while (a < before->print_count || b < now->print_count)
	{
		const navigation_object_print_t *old_print = a < before->print_count ? &before->prints[a] : NULL;
		const navigation_object_print_t *new_print = b < now->print_count ? &now->prints[b] : NULL;

		if (new_print == NULL || (old_print != NULL && old_print->id < new_print->id))
		{
			grow_box_by_print(navigation, old_print, box_out);
			changes++;
			a++;
			continue;
		}
		if (old_print == NULL || new_print->id < old_print->id)
		{
			grow_box_by_print(navigation, new_print, box_out);
			changes++;
			b++;
			continue;
		}
		if (SDL_memcmp(old_print->fields, new_print->fields, sizeof(old_print->fields)) != 0 || old_print->model != new_print->model)
		{
			grow_box_by_print(navigation, old_print, box_out);
			grow_box_by_print(navigation, new_print, box_out);
			changes++;
		}
		a++;
		b++;
	}
	region_box(navigation, region_index, region);
	box_out[0] = SDL_max(box_out[0], region[0]);
	box_out[1] = SDL_max(box_out[1], region[1]);
	box_out[2] = SDL_min(box_out[2], region[2]);
	box_out[3] = SDL_min(box_out[3], region[3]);
	return changes;
}

// One region's objects into the cells of box (in it, cleared): the room
// they take, their surfaces, the small ones' heights, the flyers' tops
static void build_box(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects, int32_t region_index, const int32_t box[4],
	object_scratch_t *scratch)
{
	object_cells_t list = { NULL, 0, 0 };
	object_cells_t small_cells = { NULL, 0, 0 };
	int32_t count = objects != NULL ? gather_region_objects(navigation, objects, region_index, scratch) : 0;

	for (int32_t object = 0; object < count; object++)
	{
		const object_instance_t *instance = &objects->instances[scratch->items[object]];
		float radius;
		int model;
		bool small;

		raise_object_tops(navigation, objects, instance, box);
		model = navigation_model(navigation, objects, instance, &radius, &small);
		if (model < 0 || !object_reaches_box(instance, radius, box))
		{
			continue;
		}
		rasterize_object(navigation, objects, instance, model,
			SDL_max(box[0], (int32_t)SDL_floorf((instance->x - radius) / NAVIGATION_CELL_METERS)),
			SDL_max(box[1], (int32_t)SDL_floorf((instance->z - radius) / NAVIGATION_CELL_METERS)),
			SDL_min(box[2], (int32_t)SDL_floorf((instance->x + radius) / NAVIGATION_CELL_METERS)),
			SDL_min(box[3], (int32_t)SDL_floorf((instance->z + radius) / NAVIGATION_CELL_METERS)), small ? &small_cells : &list);
		if (small)
		{
			// Small objects are no surface and take no room: each cell
			// keeps the height of the highest one standing in it
			for (int32_t item_index = 0; item_index < small_cells.count; item_index++)
			{
				const object_cell_t *item = &small_cells.items[item_index];
				float above = item->top - navigation->ground_height[item->cell];
				uint8_t units = (uint8_t)SDL_clamp(SDL_ceilf(above / SMALL_UNIT_METERS), 0.0f, 255.0f);

				navigation->small_height[item->cell] = SDL_max(navigation->small_height[item->cell], units);
			}
			small_cells.count = 0;
		}
	}
	finish_region(navigation, terrain, region_index, &list);
	SDL_free(list.items);
	SDL_free(small_cells.items);
}

// Builds the given regions again (marked dirty while it runs), from the
// objects near each, and stamps them with a new generation
static void build_regions(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects, const int32_t *region_list,
	int32_t list_count)
{
	uint64_t start = SDL_GetPerformanceCounter();
	int32_t partial_count = 0;
	int32_t partial_cells = 0;

	if (list_count == 0)
	{
		return;
	}
	for (int32_t k = 0; k < list_count; k++)
	{
		const navigation_region_t *region = &navigation->regions[region_list[k]];

		if (region->partial)
		{
			partial_count++;
			partial_cells += (region->partial_box[2] - region->partial_box[0] + 1) * (region->partial_box[3] - region->partial_box[1] + 1);
		}
	}
	navigation->generation++;
	// The regions apart from one another, a thread each (every cell
	// belongs to one region; each thread gathers its own objects)
#pragma omp parallel for schedule(dynamic, 1) num_threads(parallel_threads_for_items(list_count, 1))
	for (int32_t k = 0; k < list_count; k++)
	{
		int32_t r = region_list[k];
		navigation_region_t *region = &navigation->regions[r];
		object_scratch_t scratch = { NULL, 0 };
		int32_t box[4];

		region->dirty = true;
		if (region->partial)
		{
			// The cells round what changed: the others keep their surfaces
			SDL_memcpy(box, region->partial_box, sizeof(box));
			clear_box(navigation, terrain, box);
			pack_surfaces(navigation, r);
		}
		else
		{
			region_box(navigation, r, box);
			region->surface_count = 0;
			clear_box(navigation, terrain, box);
		}
		work_out_slopes(navigation, terrain, r, box);
		build_box(navigation, terrain, objects, r, box, &scratch);
		if (objects != NULL)
		{
			region->objects_signature = region_signature(navigation, objects, r, &scratch, region);
			region->objects_generation_checked = objects->generation;
		}
		region->stale = false;
		region->partial = false;
		region->terrain_touched = false;
		navigation->region_generation[r] = navigation->generation;
		SDL_free(scratch.items);
	}
	for (int32_t k = 0; k < list_count; k++)
	{
		navigation->regions[region_list[k]].dirty = false;
	}
	navigation->last_rebuilt_regions = list_count;
	frame_profile_count(FRAME_NAVIGATION_REGIONS, list_count);
	navigation->rebuilt_region_total += list_count;
	navigation->partial_region_total += partial_count;
	if (partial_count > 0)
	{
		SDL_Log("Navigation: %d region(s) of %.0f m built, %d of them in part (%d cells), in %.1f ms", list_count, (double)NAVIGATION_REGION_METERS,
			partial_count, partial_cells, (double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency());
		return;
	}
	SDL_Log("Navigation: %d region(s) of %.0f m built in %.1f ms", list_count, (double)NAVIGATION_REGION_METERS,
		(double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency());
}

// Whether the terrain under a region moved more than the navigation cares
// about since it was built (erosion shaving off millimeters does not count)
// The terrain moved under a region (water wearing it, a stamp): only the
// cells' own heights follow, those that moved more than
// TERRAIN_CHANGE_METERS; the objects over them stay as they were found (the
// room under them and their tops above the ground grow or shrink by as
// much). The objects themselves moving builds the region again (its
// signature). True if any cell changed: the region is stamped newer
static bool follow_terrain(navigation_t *navigation, const terrain_t *terrain, int32_t region_index)
{
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;
	bool changed = false;

	region_bounds(navigation, region_index, &first_x, &first_z, &last_x, &last_z);
	for (int32_t cell_z = first_z; cell_z <= last_z; cell_z++)
	{
		for (int32_t cell_x = first_x; cell_x <= last_x; cell_x++)
		{
			int32_t cell = cell_z * navigation->size + cell_x;
			float height = terrain_sample_height_meters(terrain, ((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS,
				((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS);
			float rise = height - navigation->ground_height[cell];

			if (SDL_fabsf(rise) <= TERRAIN_CHANGE_METERS)
			{
				continue;
			}
			navigation->ground_height[cell] = height;
			// What stands above keeps its height: the room under it less what
			// the ground rose (nothing above: still open)
			if (navigation->terrain_clearance[cell] != NAVIGATION_CLEARANCE_OPEN)
			{
				float room = (float)navigation->terrain_clearance[cell] * NAVIGATION_CLEARANCE_UNIT_METERS - rise;

				navigation->terrain_clearance[cell] = clearance_units(SDL_max(room, 0.0f));
			}
			if (navigation->object_top[cell] > 0)
			{
				float top = (float)navigation->object_top[cell] * NAVIGATION_OBJECT_TOP_UNIT_METERS - rise;

				navigation->object_top[cell] = (uint8_t)SDL_clamp((int)(top / NAVIGATION_OBJECT_TOP_UNIT_METERS + 0.5f), 0, 255);
			}
			changed = true;
		}
	}
	if (changed)
	{
		int32_t box[4] = { first_x, first_z, last_x, last_z };

		work_out_slopes(navigation, terrain, region_index, box);
	}
	return changed;
}

// Brings the regions of a rectangle (in regions) up to date, if anything
// in them changed enough: only what a search or a check is about to use is
// built, the rest of the map waits until somebody goes there
static void prepare_regions(navigation_t *navigation, int32_t first_x, int32_t first_z, int32_t last_x, int32_t last_z)
{
	const terrain_t *terrain = navigation->terrain;
	const objects_t *objects = navigation->objects;
	int32_t list_count = 0;

	if (terrain == NULL || navigation->regions == NULL)
	{
		return;
	}
	first_x = SDL_clamp(first_x, 0, navigation->regions_per_side - 1);
	first_z = SDL_clamp(first_z, 0, navigation->regions_per_side - 1);
	last_x = SDL_clamp(last_x, 0, navigation->regions_per_side - 1);
	last_z = SDL_clamp(last_z, 0, navigation->regions_per_side - 1);
	for (int32_t region_z = first_z; region_z <= last_z; region_z++)
	{
		for (int32_t region_x = first_x; region_x <= last_x; region_x++)
		{
			int32_t r = region_z * navigation->regions_per_side + region_x;
			navigation_region_t *region = &navigation->regions[r];

			if (!region->stale && region->terrain_touched)
			{
				// (the terrain alone: its cells follow, nothing is built)
				region->terrain_touched = false;
				if (follow_terrain(navigation, terrain, r))
				{
					navigation->generation++;
					navigation->region_generation[r] = navigation->generation;
				}
			}
			if (!region->stale && objects != NULL && (region->objects_generation_checked != objects->generation || region->objects_touched))
			{
				region->objects_touched = false;
				region->objects_generation_checked = objects->generation;
				object_scratch_t scratch = { navigation->object_scratch, navigation->object_scratch_capacity };

				{
					static navigation_region_t now;
					int32_t box[4];
					int32_t whole[4];

					region->stale = region_signature(navigation, objects, r, &scratch, &now) != region->objects_signature;
					// A few objects changed (a boulder washed away, a crate
					// moved): only the cells round them, if that is less than
					// half the region
					region_box(navigation, r, whole);
					if (region->stale && navigation->region_generation[r] != 0 && changed_box(navigation, r, &now, box) > 0 && box[0] <= box[2] && box[1] <= box[3]
						&& 2 * (box[2] - box[0] + 1) * (box[3] - box[1] + 1) < (whole[2] - whole[0] + 1) * (whole[3] - whole[1] + 1))
					{
						region->partial = true;
						SDL_memcpy(region->partial_box, box, sizeof(box));
					}
				}
				navigation->object_scratch = scratch.items;
				navigation->object_scratch_capacity = scratch.capacity;
			}
			if (region->stale)
			{
				navigation->build_list[list_count++] = r;
			}
		}
	}
	build_regions(navigation, terrain, objects, navigation->build_list, list_count);
}

// The cells a blocker covers (their middles in it), within its box; with
// the box, cells
static void blocker_box(const navigation_t *navigation, const navigation_blocker_t *blocker, int32_t box_out[4])
{
	float c = SDL_fabsf(SDL_cosf(blocker->yaw_radians));
	float s = SDL_fabsf(SDL_sinf(blocker->yaw_radians));
	float reach_x = blocker->half_width_meters * c + blocker->half_depth_meters * s;
	float reach_z = blocker->half_width_meters * s + blocker->half_depth_meters * c;

	box_out[0] = SDL_clamp((int32_t)SDL_floorf((blocker->x - reach_x) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
	box_out[1] = SDL_clamp((int32_t)SDL_floorf((blocker->z - reach_z) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
	box_out[2] = SDL_clamp((int32_t)SDL_floorf((blocker->x + reach_x) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
	box_out[3] = SDL_clamp((int32_t)SDL_floorf((blocker->z + reach_z) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
}

static void stamp_blocker(navigation_t *navigation, const navigation_blocker_t *blocker)
{
	int32_t box[4];
	float c = SDL_cosf(blocker->yaw_radians);
	float s = SDL_sinf(blocker->yaw_radians);

	blocker_box(navigation, blocker, box);
	for (int32_t z = box[1]; z <= box[3]; z++)
	{
		for (int32_t x = box[0]; x <= box[2]; x++)
		{
			float dx = ((float)x + 0.5f) * NAVIGATION_CELL_METERS - blocker->x;
			float dz = ((float)z + 0.5f) * NAVIGATION_CELL_METERS - blocker->z;
			// (as areas.c turns a point into its own frame; half a cell
			// more, so a thin wall still takes its cells)
			float along_width = dx * c - dz * s;
			float along_depth = dx * s + dz * c;

			if (SDL_fabsf(along_width) <= blocker->half_width_meters + 0.5f * NAVIGATION_CELL_METERS
				&& SDL_fabsf(along_depth) <= blocker->half_depth_meters + 0.5f * NAVIGATION_CELL_METERS)
			{
				navigation->blocked[z * navigation->size + x] |= blocker->groups;
			}
		}
	}
}

// The regions under a blocker's box: stamped newer (the paths through
// them are looked at again)
static void touch_blocker(navigation_t *navigation, const navigation_blocker_t *blocker)
{
	int32_t box[4];

	blocker_box(navigation, blocker, box);
	navigation->generation++;
	for (int32_t region_z = box[1] / navigation->region_size; region_z <= box[3] / navigation->region_size; region_z++)
	{
		for (int32_t region_x = box[0] / navigation->region_size; region_x <= box[2] / navigation->region_size; region_x++)
		{
			navigation->region_generation[region_z * navigation->regions_per_side + region_x] = navigation->generation;
		}
	}
}

static bool cell_of_point(const navigation_t *navigation, float x, float z, int32_t *cell_out);

// The doorways of the shut doors: the cells between each link's two ends
// (not their own) stamped with NAVIGATION_GROUP_SHUT_DOOR (set) or cleared,
// their regions stamped newer when anything changed
static void stamp_doors(navigation_t *navigation, bool set)
{
	for (int k = 0; k < navigation->link_count && navigation->blocked != NULL; k++)
	{
		const navigation_link_t *link = &navigation->links[k];
		float dx = link->b[0] - link->a[0];
		float dz = link->b[1] - link->a[1];
		float length = SDL_sqrtf(dx * dx + dz * dz);
		int steps = SDL_max(1, (int)SDL_ceilf(length / 0.25f));

		if (link->open && set)
		{
			continue;
		}
		for (int step = 1; step < steps; step++)
		{
			int32_t cell;
			float amount = (float)step / (float)steps;

			if (!cell_of_point(navigation, link->a[0] + dx * amount, link->a[1] + dz * amount, &cell) || cell == navigation->link_cells[k][0]
				|| cell == navigation->link_cells[k][1])
			{
				continue;
			}
			if (set && (navigation->blocked[cell] & NAVIGATION_GROUP_SHUT_DOOR) == 0)
			{
				navigation->blocked[cell] |= NAVIGATION_GROUP_SHUT_DOOR;
				navigation->generation++;
				navigation->region_generation[region_of_cell(navigation, cell)] = navigation->generation;
			}
			else if (!set && (navigation->blocked[cell] & NAVIGATION_GROUP_SHUT_DOOR) != 0)
			{
				navigation->blocked[cell] &= (uint8_t)~NAVIGATION_GROUP_SHUT_DOOR;
				navigation->generation++;
				navigation->region_generation[region_of_cell(navigation, cell)] = navigation->generation;
			}
		}
	}
}

void navigation_set_links(navigation_t *navigation, const navigation_link_t *links, int count)
{
	// (the doors as they were: their doorways cleared, the new ones stamped
	// after; nothing changed, nothing done)
	{
		bool same = count == navigation->links_given_count;

		count = SDL_clamp(count, 0, NAVIGATION_LINKS_MAX);
		for (int k = 0; k < count && same; k++)
		{
			same = SDL_memcmp(&links[k], &navigation->links_given[k], sizeof(navigation_link_t)) == 0;
		}
		if (same)
		{
			return;
		}
		if (count > 0)
		{
			SDL_memcpy(navigation->links_given, links, sizeof(navigation_link_t) * (size_t)count);
		}
		navigation->links_given_count = count;
	}
	stamp_doors(navigation, false);
	navigation->link_count = 0;
	for (int k = 0; k < count && navigation->link_count < NAVIGATION_LINKS_MAX; k++)
	{
		int32_t cell_a;
		int32_t cell_b;
		float dx = links[k].b[0] - links[k].a[0];
		float dz = links[k].b[1] - links[k].a[1];

		cell_of_point(navigation, links[k].a[0], links[k].a[1], &cell_a);
		cell_of_point(navigation, links[k].b[0], links[k].b[1], &cell_b);
		if (cell_a == cell_b)
		{
			continue;
		}
		navigation->links[navigation->link_count] = links[k];
		navigation->link_cells[navigation->link_count][0] = cell_a;
		navigation->link_cells[navigation->link_count][1] = cell_b;
		navigation->link_meters[navigation->link_count] = SDL_sqrtf(dx * dx + dz * dz);
		navigation->link_count++;
	}
	stamp_doors(navigation, true);
}

void navigation_set_blockers(navigation_t *navigation, const navigation_blocker_t *blockers, int count)
{
	count = SDL_clamp(count, 0, NAVIGATION_BLOCKERS_MAX);
	if (navigation->blocked == NULL || (count == navigation->blocker_count
		&& (count == 0 || SDL_memcmp(blockers, navigation->blockers, sizeof(navigation_blocker_t) * (size_t)count) == 0)))
	{
		return;
	}
	// Where the old ones were and the new ones are: told; then all
	// stamped again (there are few)
	for (int i = 0; i < navigation->blocker_count; i++)
	{
		int32_t box[4];

		blocker_box(navigation, &navigation->blockers[i], box);
		for (int32_t z = box[1]; z <= box[3]; z++)
		{
			SDL_memset(&navigation->blocked[z * navigation->size + box[0]], 0, (size_t)(box[2] - box[0] + 1));
		}
		touch_blocker(navigation, &navigation->blockers[i]);
	}
	if (count > 0)
	{
		SDL_memcpy(navigation->blockers, blockers, sizeof(navigation_blocker_t) * (size_t)count);
	}
	navigation->blocker_count = count;
	for (int i = 0; i < count; i++)
	{
		touch_blocker(navigation, &navigation->blockers[i]);
	}
	// (all of them: clearing an old one's box may have cleared a kept one's,
	// or a shut door's)
	for (int i = 0; i < count; i++)
	{
		stamp_blocker(navigation, &navigation->blockers[i]);
	}
	stamp_doors(navigation, true);
}

void navigation_prepare_around(navigation_t *navigation, float x, float z)
{
	float region_meters = (float)navigation->region_size * NAVIGATION_CELL_METERS;
	int32_t region_x = (int32_t)SDL_floorf(x / region_meters);
	int32_t region_z = (int32_t)SDL_floorf(z / region_meters);

	prepare_regions(navigation, region_x - 1, region_z - 1, region_x + 1, region_z + 1);
}

// The regions in meters
static void prepare_box(navigation_t *navigation, float minimum_x, float minimum_z, float maximum_x, float maximum_z)
{
	float region_meters = (float)navigation->region_size * NAVIGATION_CELL_METERS;

	prepare_regions(navigation, (int32_t)SDL_floorf(minimum_x / region_meters), (int32_t)SDL_floorf(minimum_z / region_meters),
		(int32_t)SDL_floorf(maximum_x / region_meters), (int32_t)SDL_floorf(maximum_z / region_meters));
}

// The regions a search from one point towards another may well go
// through: along the straight line, and one region on either side of it
// (a detour), as far as a search gets before it gives up. A detour wider
// than that reads the regions as they were last built.
static void prepare_way(navigation_t *navigation, float from_x, float from_z, float to_x, float to_z, float reach_meters)
{
	float region_meters = (float)navigation->region_size * NAVIGATION_CELL_METERS;
	float dx = to_x - from_x;
	float dz = to_z - from_z;
	float length = SDL_sqrtf(dx * dx + dz * dz);
	int steps;

	if (length > reach_meters)
	{
		dx *= reach_meters / length;
		dz *= reach_meters / length;
		length = reach_meters;
	}
	steps = SDL_max(1, (int)SDL_ceilf(length / (region_meters * 0.5f)));
	for (int step = 0; step <= steps; step++)
	{
		float x = from_x + dx * (float)step / (float)steps;
		float z = from_z + dz * (float)step / (float)steps;
		int32_t region_x = (int32_t)SDL_floorf(x / region_meters);
		int32_t region_z = (int32_t)SDL_floorf(z / region_meters);

		prepare_regions(navigation, region_x - 1, region_z - 1, region_x + 1, region_z + 1);
	}
}

// ---------------------------------------------------------------------------
// The map, and keeping up with it

static void free_search_memory(navigation_t *navigation)
{
	SDL_free(navigation->search);
	SDL_free(navigation->heap);
	SDL_free(navigation->bucket_heads);
	SDL_free(navigation->bucket_entry_nodes);
	SDL_free(navigation->bucket_entry_next);
	navigation->bucket_heads = NULL;
	navigation->bucket_entry_nodes = NULL;
	navigation->bucket_entry_next = NULL;
	navigation->bucket_entry_capacity = 0;
	SDL_free(navigation->node_path);
	navigation->search = NULL;
	navigation->heap = NULL;
	navigation->node_path = NULL;
	navigation->heap_capacity = 0;
	navigation->node_path_capacity = 0;
	navigation->search_number = 0;
}

static void free_map(navigation_t *navigation)
{
	mesh_free(navigation);
	for (int32_t r = 0; r < navigation->regions_per_side * navigation->regions_per_side && navigation->regions != NULL; r++)
	{
		SDL_free(navigation->regions[r].surfaces);
		SDL_free(navigation->regions[r].prints);
	}
	SDL_free(navigation->regions);
	SDL_free(navigation->ground_height);
	SDL_free(navigation->terrain_clearance);
	SDL_free(navigation->terrain_blocker);
	SDL_free(navigation->object_top);
	SDL_free(navigation->ground_slope);
	SDL_free(navigation->small_height);
	SDL_free(navigation->blocked);
	navigation->blocked = NULL;
	navigation->blocker_count = 0;
	SDL_free(navigation->extra_count);
	SDL_free(navigation->extra_first);
	SDL_free(navigation->region_generation);
	SDL_free(navigation->build_list);
	SDL_free(navigation->object_scratch);
	navigation->build_list = NULL;
	navigation->object_scratch = NULL;
	navigation->object_scratch_capacity = 0;
	navigation->regions = NULL;
	navigation->ground_height = NULL;
	navigation->terrain_clearance = NULL;
	navigation->terrain_blocker = NULL;
	navigation->object_top = NULL;
	navigation->ground_slope = NULL;
	navigation->small_height = NULL;
	navigation->extra_count = NULL;
	navigation->extra_first = NULL;
	navigation->region_generation = NULL;
}

static bool resize(navigation_t *navigation, const terrain_t *terrain)
{
	int32_t size = (int32_t)SDL_ceilf((float)terrain->size * terrain->cell_size_meters / NAVIGATION_CELL_METERS);
	size_t cells = (size_t)size * (size_t)size;
	int32_t region_count;

	free_search_memory(navigation);
	free_map(navigation);
	navigation->size = size;
	navigation->region_size = (int32_t)(NAVIGATION_REGION_METERS / NAVIGATION_CELL_METERS);
	navigation->regions_per_side = (size + navigation->region_size - 1) / navigation->region_size;
	region_count = navigation->regions_per_side * navigation->regions_per_side;
	navigation->regions = SDL_calloc((size_t)region_count, sizeof(navigation_region_t));
	navigation->ground_height = SDL_calloc(cells, sizeof(float));
	navigation->terrain_clearance = SDL_malloc(cells);
	navigation->terrain_blocker = SDL_malloc(cells * sizeof(int16_t));
	navigation->object_top = SDL_calloc(cells, 1);
	navigation->ground_slope = SDL_calloc(cells, 1);
	navigation->small_height = SDL_calloc(cells, 1);
	navigation->blocked = SDL_calloc(cells, 1);
	navigation->blocker_count = 0;
	navigation->extra_count = SDL_calloc(cells, 1);
	navigation->extra_first = SDL_calloc(cells, sizeof(uint16_t));
	navigation->region_generation = SDL_calloc((size_t)region_count, sizeof(uint32_t));
	navigation->build_list = SDL_calloc((size_t)region_count, sizeof(int32_t));
	if (navigation->build_list == NULL || navigation->regions == NULL || navigation->ground_height == NULL || navigation->terrain_clearance == NULL
		|| navigation->terrain_blocker == NULL || navigation->object_top == NULL || navigation->ground_slope == NULL || navigation->small_height == NULL || navigation->blocked == NULL
		|| navigation->extra_count == NULL
		|| navigation->extra_first == NULL || navigation->region_generation == NULL)
	{
		return false;
	}
	SDL_memset(navigation->terrain_clearance, NAVIGATION_CLEARANCE_OPEN, cells);
	SDL_memset(navigation->terrain_blocker, 0xFF, cells * sizeof(int16_t));
	if (!mesh_allocate(navigation))
	{
		return false;
	}
	// (the doors stamped again on the new cells at their next telling)
	navigation->links_given_count = -1;
	navigation->link_count = 0;
	// Everything is to be built
	navigation->build_everything = true;
	return true;
}

bool navigation_create(navigation_t *navigation, const terrain_t *terrain)
{
	SDL_zerop(navigation);
	navigation->maximum_expansions = 200000;
	navigation->object_minimum_meters = 0.3f;
	navigation->small_object_meters = 0.8f;
	navigation->small_detour_meters = 10.0f;
	navigation_set_slope_cost(navigation, 40.0f, 1.5f);
	return resize(navigation, terrain);
}

void navigation_destroy(navigation_t *navigation)
{
	free_search_memory(navigation);
	free_map(navigation);
	SDL_zerop(navigation);
}

// The farthest any object reaches from its center (the margin of objects'
// grid cells looked at around a region)
static float object_margin(const objects_t *objects)
{
	float largest_radius = 0.0f;
	float largest_scale = 1.0f;

	for (int m = 0; m < objects->model_count; m++)
	{
		largest_radius = SDL_max(largest_radius, objects->models[m].horizontal_radius);
	}
	for (int t = 0; t < objects->types.type_count; t++)
	{
		largest_scale = SDL_max(largest_scale, objects->types.types[t].scale_maximum);
	}
	return largest_radius * largest_scale + 1.0f;
}

void navigation_build_all(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects)
{
	int32_t region_count = navigation->regions_per_side * navigation->regions_per_side;

	navigation->terrain = terrain;
	navigation->objects = objects;
	if (objects != NULL)
	{
		navigation->object_margin_meters = object_margin(objects);
	}
	// (every region on its own thread: build_regions)
	for (int32_t r = 0; r < region_count; r++)
	{
		navigation->build_list[r] = r;
		navigation->regions[r].partial = false;
	}
	build_regions(navigation, terrain, objects, navigation->build_list, region_count);
	navigation->build_everything = false;
	// (the whole new terrain is in it already: its first update, which sees
	// every chunk changed, must not build it all again)
	navigation->whole_terrain_seen = true;
}

void navigation_forget(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects)
{
	int32_t region_count = navigation->regions_per_side * navigation->regions_per_side;

	navigation->terrain = terrain;
	navigation->objects = objects;
	if (objects != NULL)
	{
		navigation->object_margin_meters = object_margin(objects);
	}
	for (int32_t r = 0; r < region_count; r++)
	{
		navigation->regions[r].stale = true;
		navigation->regions[r].partial = false;
	}
	navigation->build_everything = false;
	// (the new terrain's first update sees every chunk changed: nothing to
	// build at once for it)
	navigation->whole_terrain_seen = true;
}

void navigation_update(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects, double now_seconds)
{
	int32_t expected_size = (int32_t)SDL_ceilf((float)terrain->size * terrain->cell_size_meters / NAVIGATION_CELL_METERS);

	(void)now_seconds;
	if (expected_size != navigation->size && !resize(navigation, terrain))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Navigation: out of memory");
		return;
	}
	navigation->terrain = terrain;
	navigation->objects = objects;
	if (objects != NULL)
	{
		navigation->object_margin_meters = object_margin(objects);
	}
	// The object list changed a little (no sort, no new generation): the
	// regions an object was added to, went from or came loose in compared
	// again (those within its reach of it)
	if (objects != NULL && navigation->regions != NULL)
	{
		int count = 0;
		uint32_t next;
		const object_change_t *changes = objects_changes_since(objects, navigation->object_change_serial, &count, &next);
		float region_meters = (float)navigation->region_size * NAVIGATION_CELL_METERS;
		float reach = navigation->object_margin_meters;

		for (int c = 0; c < count; c++)
		{
			int32_t first_x = SDL_clamp((int32_t)((changes[c].x - reach) / region_meters), 0, navigation->regions_per_side - 1);
			int32_t first_z = SDL_clamp((int32_t)((changes[c].z - reach) / region_meters), 0, navigation->regions_per_side - 1);
			int32_t last_x = SDL_clamp((int32_t)((changes[c].x + reach) / region_meters), 0, navigation->regions_per_side - 1);
			int32_t last_z = SDL_clamp((int32_t)((changes[c].z + reach) / region_meters), 0, navigation->regions_per_side - 1);

			for (int32_t region_z = first_z; region_z <= last_z; region_z++)
			{
				for (int32_t region_x = first_x; region_x <= last_x; region_x++)
				{
					navigation->regions[region_z * navigation->regions_per_side + region_x].objects_touched = true;
				}
			}
		}
		navigation->object_change_serial = next;
	}
	if (terrain->any_chunk_needs_upload)
	{
		float chunk_meters = (float)TERRAIN_CHUNK_SIZE * terrain->cell_size_meters;
		float region_meters = (float)navigation->region_size * NAVIGATION_CELL_METERS;
		int32_t chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;
		int32_t touched = 0;

		for (int32_t chunk = 0; chunk < chunk_count; chunk++)
		{
			if (terrain->chunks[chunk].needs_shape_upload)
			{
				float x = ((float)(chunk % terrain->chunks_per_side) + 0.5f) * chunk_meters;
				float z = ((float)(chunk / terrain->chunks_per_side) + 0.5f) * chunk_meters;
				int32_t region_x = SDL_clamp((int32_t)(x / region_meters), 0, navigation->regions_per_side - 1);
				int32_t region_z = SDL_clamp((int32_t)(z / region_meters), 0, navigation->regions_per_side - 1);

				navigation->regions[region_z * navigation->regions_per_side + region_x].terrain_touched = true;
				touched++;
			}
		}
		// The whole terrain at once is a new map: everything anew, now
		// (once: the flags stay until the terrain is uploaded)
		if (touched == chunk_count && !navigation->whole_terrain_seen)
		{
			navigation->build_everything = true;
		}
		navigation->whole_terrain_seen = touched == chunk_count;
	}
	else
	{
		navigation->whole_terrain_seen = false;
	}
	if (navigation->build_everything)
	{
		navigation_build_all(navigation, terrain, objects);
	}
}

bool navigation_changed_since(navigation_t *navigation, const water_t *water, float minimum_x, float minimum_z,
	float maximum_x, float maximum_z, uint32_t generation)
{
	float region_meters = (float)navigation->region_size * NAVIGATION_CELL_METERS;

	// What changed there is built now (and stamped newer than generation)
	prepare_box(navigation, minimum_x, minimum_z, maximum_x, maximum_z);
	int32_t first_x = SDL_clamp((int32_t)(minimum_x / region_meters), 0, navigation->regions_per_side - 1);
	int32_t first_z = SDL_clamp((int32_t)(minimum_z / region_meters), 0, navigation->regions_per_side - 1);
	int32_t last_x = SDL_clamp((int32_t)(maximum_x / region_meters), 0, navigation->regions_per_side - 1);
	int32_t last_z = SDL_clamp((int32_t)(maximum_z / region_meters), 0, navigation->regions_per_side - 1);

	for (int32_t region_z = first_z; region_z <= last_z; region_z++)
	{
		for (int32_t region_x = first_x; region_x <= last_x; region_x++)
		{
			if (navigation->region_generation[region_z * navigation->regions_per_side + region_x] > generation)
			{
				return true;
			}
		}
	}
	// Running water changes all the time; still and frozen water does not
	if (water != NULL)
	{
		float tile_meters = (float)WATER_TILE_SIZE * water->cell_size_meters;
		int32_t tile_first_x = SDL_clamp((int32_t)(minimum_x / tile_meters), 0, water->tiles_per_side - 1);
		int32_t tile_first_z = SDL_clamp((int32_t)(minimum_z / tile_meters), 0, water->tiles_per_side - 1);
		int32_t tile_last_x = SDL_clamp((int32_t)(maximum_x / tile_meters), 0, water->tiles_per_side - 1);
		int32_t tile_last_z = SDL_clamp((int32_t)(maximum_z / tile_meters), 0, water->tiles_per_side - 1);

		for (int32_t tile_z = tile_first_z; tile_z <= tile_last_z; tile_z++)
		{
			for (int32_t tile_x = tile_first_x; tile_x <= tile_last_x; tile_x++)
			{
				if (water->tile_state[tile_z * water->tiles_per_side + tile_x] == WATER_TILE_ACTIVE)
				{
					return true;
				}
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Queries

static bool cell_of_point(const navigation_t *navigation, float x, float z, int32_t *cell_out)
{
	int32_t cell_x = (int32_t)SDL_floorf(x / NAVIGATION_CELL_METERS);
	int32_t cell_z = (int32_t)SDL_floorf(z / NAVIGATION_CELL_METERS);
	bool inside = cell_x >= 0 && cell_z >= 0 && cell_x < navigation->size && cell_z < navigation->size;

	cell_x = SDL_clamp(cell_x, 0, navigation->size - 1);
	cell_z = SDL_clamp(cell_z, 0, navigation->size - 1);
	*cell_out = cell_z * navigation->size + cell_x;
	return inside;
}

// Is there room for the agent on a surface with this clearance?
static bool roomy(uint8_t clearance, const navigation_agent_t *agent)
{
	return clearance == NAVIGATION_CLEARANCE_OPEN || (float)clearance * NAVIGATION_CLEARANCE_UNIT_METERS >= agent->height_meters;
}

// An object surface's height at (x, z), the point taken into the
// surface's cell if it is outside; -1e30 if the surface does not reach
// that part of the cell
static float surface_height_at(const navigation_t *navigation, const navigation_surface_t *surface, float x, float z)
{
	int32_t cell_x = surface->cell % navigation->size;
	int32_t cell_z = surface->cell / navigation->size;
	float inside_x = SDL_clamp(x / NAVIGATION_CELL_METERS - (float)cell_x, 0.0f, 0.999f);
	float inside_z = SDL_clamp(z / NAVIGATION_CELL_METERS - (float)cell_z, 0.0f, 0.999f);
	int32_t sub = (int32_t)(inside_z * NAVIGATION_SUBCELLS) * NAVIGATION_SUBCELLS + (int32_t)(inside_x * NAVIGATION_SUBCELLS);
	uint8_t below = surface->below[sub];

	// (a quarter something stands in at its height, a wall's or a
	// balustrade's: the floor beside it, as one brushing the wall stands on
	// the floor, not on the ground under it)
	if (below == NAVIGATION_SUBCELL_BLOCKED)
	{
		int32_t sub_x = sub % NAVIGATION_SUBCELLS;
		int32_t sub_z = sub / NAVIGATION_SUBCELLS;
		float best = -1e30f;

		for (int d = 0; d < 4; d++)
		{
			int32_t next_x = sub_x + direction_x[d];
			int32_t next_z = sub_z + direction_z[d];
			uint8_t next_below;

			if (next_x < 0 || next_z < 0 || next_x >= NAVIGATION_SUBCELLS || next_z >= NAVIGATION_SUBCELLS)
			{
				continue;
			}
			next_below = surface->below[next_z * NAVIGATION_SUBCELLS + next_x];
			if (next_below < NAVIGATION_SUBCELL_BLOCKED)
			{
				best = SDL_max(best, surface->height - (float)next_below * NAVIGATION_SUBCELL_UNIT_METERS);
			}
		}
		return best;
	}
	if (below >= NAVIGATION_SUBCELL_BLOCKED)
	{
		return -1e30f;
	}
	return surface->height - (float)below * NAVIGATION_SUBCELL_UNIT_METERS;
}

static const navigation_surface_t *extra_surface(const navigation_t *navigation, int32_t node)
{
	int32_t index = node - cell_count(navigation);

	return &navigation->regions[index / NAVIGATION_REGION_EXTRA_SLOTS].surfaces[index % NAVIGATION_REGION_EXTRA_SLOTS];
}

float navigation_surface_height(const navigation_t *navigation, const terrain_t *terrain, const navigation_agent_t *agent, float x,
	float z, float near_height)
{
	return navigation_surface_height_on(navigation, terrain, agent, x, z, near_height, NULL);
}

float navigation_surface_height_on(const navigation_t *navigation, const terrain_t *terrain, const navigation_agent_t *agent, float x,
	float z, float near_height, bool *object_out)
{
	bool on_object = false;

	int32_t cell;
	float reach = agent != NULL ? SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS) : 0.0f;
	// The best so far: standable and within reach first, then the nearest
	float best = terrain_sample_height_meters(terrain, x, z);
	bool best_good;
	bool covered = false;
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count;

	cell_of_point(navigation, x, z, &cell);
	count = cell_nodes(navigation, cell, nodes);
	// The terrain is good where it has room, and also where no object's top
	// is at this very point (a cell a rock takes part of: beside the rock the
	// ground is there to stand on)
	for (int k = 1; k < count; k++)
	{
		covered = covered || surface_height_at(navigation, extra_surface(navigation, nodes[k]), x, z) > -1e29f;
	}
	best_good = agent == NULL
		|| ((roomy(navigation->terrain_clearance[cell], agent) || !covered) && SDL_fabsf(best - near_height) <= reach);
	// The cell's own surfaces first, at this point; its neighbors' object
	// tops (at their edge nearest the point) only when the cell has nothing
	// within reach: a deck stops at a cell's edge, and a foot just past its
	// last cell must not drop into the canyon under it. near_height is best
	// where that point was last (a foot's own last height): the body's
	// height would make the choice swing with it
	for (int pass = 0; pass < 2; pass++)
	{
		if (pass == 1 && (agent == NULL || best_good))
		{
			break;
		}
		for (int32_t neighbor = 0; neighbor < 9; neighbor++)
		{
			int32_t dx = neighbor % 3 - 1;
			int32_t dz = neighbor / 3 - 1;
			int32_t cell_x = cell % navigation->size + dx;
			int32_t cell_z = cell / navigation->size + dz;

			if ((pass == 0) != (dx == 0 && dz == 0))
			{
				continue;
			}
			if (cell_x < 0 || cell_z < 0 || cell_x >= navigation->size || cell_z >= navigation->size)
			{
				continue;
			}
			count = cell_nodes(navigation, cell_z * navigation->size + cell_x, nodes);
			for (int k = 1; k < count; k++)
			{
				const navigation_surface_t *surface = extra_surface(navigation, nodes[k]);
				float height = surface_height_at(navigation, surface, x, z);
				bool good;
				bool nearer;

				if (height < -1e29f)
				{
					continue;                  // not at this point
				}
				// (an object's top within a step: the slope's reach is the
				// terrain's, from cell to cell; with it a man by an anvil
				// stood on its horn, a hen in the water trough)
				good = agent == NULL || (roomy(surface->clearance, agent) && SDL_fabsf(height - near_height) <= agent->step_meters);
				nearer = SDL_fabsf(height - near_height) < SDL_fabsf(best - near_height);
				// A good one beats a bad one; among equals, the nearer
				if ((good && !best_good) || (good == best_good && nearer))
				{
					best = height;
					best_good = good;
					on_object = true;
				}
			}
		}
	}
	if (object_out != NULL)
	{
		*object_out = on_object;
	}
	return best;
}

bool navigation_standable(navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z)
{
	int32_t cell;
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count;

	prepare_box(navigation, x, z, x, z);
	if (!cell_of_point(navigation, x, z, &cell))
	{
		return false;
	}
	count = cell_nodes(navigation, cell, nodes);
	for (int k = 0; k < count; k++)
	{
		node_info_t info;

		node_info(navigation, terrain, water, agent, nodes[k], &info);
		if (info.passable && info.water_class != CLASS_SWIM)
		{
			return true;
		}
	}
	return false;
}

bool navigation_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z, float near_height)
{
	int32_t cell;
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count;
	float reach = SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);

	if (!cell_of_point(navigation, x, z, &cell))
	{
		return false;
	}
	// Any surface it can stand on, within a step of where it is: walking
	// onto a rock goes from the ground (blocked by the rock) to its top
	count = cell_nodes(navigation, cell, nodes);
	for (int k = 0; k < count; k++)
	{
		node_info_t info;

		node_info(navigation, terrain, water, agent, nodes[k], &info);
		if (info.passable && (info.water_class == CLASS_SWIM || SDL_fabsf(info.height - near_height) <= reach))
		{
			return true;
		}
	}
	return false;
}

// Whether an agent at near_height can stand at the quarter meter (sub_x,
// sub_z) of a cell, and at what height (height_out): see
// navigation_quarter_free
static bool quarter_stand(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t cell, int32_t sub_x, int32_t sub_z, float near_height, float *height_out)
{
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count;
	float reach = SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);
	uint8_t room_needed = (uint8_t)SDL_clamp(SDL_ceilf(agent->height_meters / NAVIGATION_CLEARANCE_UNIT_METERS), 0.0f,
		(float)(NAVIGATION_CLEARANCE_OPEN - 1));
	bool in_the_way = false;

	count = cell_nodes(navigation, cell, nodes);
	for (int k = 0; k < count; k++)
	{
		const navigation_surface_t *surface;
		uint8_t below;
		float height;
		bool blocked;

		if (nodes[k] < cell_count(navigation))
		{
			node_info_t info;

			node_info(navigation, terrain, water, agent, nodes[k], &info);
			if (info.passable && (info.water_class == CLASS_SWIM || SDL_fabsf(info.height - near_height) <= reach))
			{
				*height_out = info.height;
				return true;
			}
			continue;
		}
		if (node_sub_height(navigation, nodes[k], cell, sub_x, sub_z, room_needed, &height, &blocked))
		{
			// (an object's top: a step up onto it, no slope; higher than a
			// step over it: its side, in the way, a terrace's face; lower:
			// a drop the path never takes)
			if (SDL_fabsf(height - near_height) <= agent->step_meters + 0.05f)
			{
				*height_out = height;
				return true;
			}
			if (height > near_height && height - near_height < reach + NAVIGATION_OBJECT_HEIGHT_METERS)
			{
				in_the_way = true;
			}
			continue;
		}
		// (its, or something standing at its height there, within reach:
		// in the way)
		surface = extra_surface(navigation, nodes[k]);
		below = surface->below[sub_z * NAVIGATION_SUBCELLS + sub_x];
		if (blocked && SDL_fabsf(surface->height - (below < NAVIGATION_SUBCELL_BLOCKED ? (float)below * NAVIGATION_SUBCELL_UNIT_METERS : 0.0f)
			- near_height) <= reach)
		{
			in_the_way = true;
		}
	}
	// (no surface's there, nor the ground's room: the ground, if within a
	// step (the first tread of a stair taken for the ground); not a drop
	// off the side of a stair, a step's strip in front of a porch beside
	// it, where the path never went)
	*height_out = navigation->ground_height[cell];
	return !in_the_way && SDL_fabsf(navigation->ground_height[cell] - near_height) <= agent->step_meters + 0.05f;
}

// A point's cell and quarter meter in it
static bool quarter_of_point(const navigation_t *navigation, float x, float z, int32_t *cell_out, int32_t *sub_x_out, int32_t *sub_z_out)
{
	if (!cell_of_point(navigation, x, z, cell_out))
	{
		return false;
	}
	*sub_x_out = SDL_clamp((int32_t)((x / NAVIGATION_CELL_METERS - SDL_floorf(x / NAVIGATION_CELL_METERS)) * NAVIGATION_SUBCELLS), 0,
		NAVIGATION_SUBCELLS - 1);
	*sub_z_out = SDL_clamp((int32_t)((z / NAVIGATION_CELL_METERS - SDL_floorf(z / NAVIGATION_CELL_METERS)) * NAVIGATION_SUBCELLS), 0,
		NAVIGATION_SUBCELLS - 1);
	return true;
}

bool navigation_quarter_free(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z, float near_height)
{
	int32_t cell;
	int32_t sub_x;
	int32_t sub_z;
	float height;

	return quarter_of_point(navigation, x, z, &cell, &sub_x, &sub_z)
		&& quarter_stand(navigation, terrain, water, agent, cell, sub_x, sub_z, near_height, &height);
}

// Where an agent's body reaches round its center: eight points round, a
// little inside its radius (its sides may brush)
#define BODY_POINTS 8
#define BODY_REACH 0.8f

// navigation_quarter_tightness, and the height it stands at there
static int quarter_tightness(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float x, float z, float near_height, float *height_out)
{
	int32_t cell;
	int32_t sub_x;
	int32_t sub_z;
	float height;
	int tight = 0;

	if (!quarter_of_point(navigation, x, z, &cell, &sub_x, &sub_z)
		|| !quarter_stand(navigation, terrain, water, agent, cell, sub_x, sub_z, near_height, &height))
	{
		return -1;
	}
	for (int k = 0; k < BODY_POINTS; k++)
	{
		float angle = (float)k * (6.2831853f / (float)BODY_POINTS);
		float point_x = x + SDL_sinf(angle) * agent->radius_meters * BODY_REACH;
		float point_z = z + SDL_cosf(angle) * agent->radius_meters * BODY_REACH;
		float point_height;

		if (!quarter_of_point(navigation, point_x, point_z, &cell, &sub_x, &sub_z)
			|| !quarter_stand(navigation, terrain, water, agent, cell, sub_x, sub_z, height, &point_height)
			|| SDL_fabsf(point_height - height) > agent->step_meters + 0.05f)
		{
			tight++;
		}
	}
	*height_out = height;
	return tight;
}

int navigation_quarter_tightness(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float x, float z, float near_height)
{
	float height;

	return quarter_tightness(navigation, terrain, water, agent, x, z, near_height, &height);
}

// How far apart a body's line is looked at (navigation_body_line): half a
// quarter meter, none of them stepped over
#define BODY_LINE_STEP_METERS 0.125f

// Whether a cell and the eight round it are plain ground to an agent:
// nothing on them, nothing over them, nothing in its way. A body there
// has nothing to brush against: it fits where it stands (its radius is
// under half a cell)
static bool plain_round(const navigation_t *navigation, const navigation_agent_t *agent, int32_t cell)
{
	int32_t cell_x = cell % navigation->size;
	int32_t cell_z = cell / navigation->size;

	for (int32_t dz = -1; dz <= 1; dz++)
	{
		for (int32_t dx = -1; dx <= 1; dx++)
		{
			int32_t x = cell_x + dx;
			int32_t z = cell_z + dz;
			int32_t near;

			if (x < 0 || z < 0 || x >= navigation->size || z >= navigation->size)
			{
				return false;
			}
			near = z * navigation->size + x;
			if (navigation->extra_count[near] > 0 || navigation->terrain_clearance[near] != NAVIGATION_CLEARANCE_OPEN
				|| (navigation->blocked[near] & agent->groups) != 0 || navigation->small_height[near] > 0)
			{
				return false;
			}
		}
	}
	return true;
}

// navigation_body_line, and how far along the line its body gets (0..1:
// the last place of it that is its, all of it if true) and its height
// there
static bool body_line_reach(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, int most_brushes, float *reached_out, float *height_out)
{
	float dx = to_x - from_x;
	float dz = to_z - from_z;
	float length = SDL_sqrtf(dx * dx + dz * dz);
	int samples = (int)SDL_ceilf(length / BODY_LINE_STEP_METERS);
	float height = from_height;
	// (somewhere it cannot be, pushed there: out any way it stands)
	int allowed = navigation_quarter_tightness(navigation, terrain, water, agent, from_x, from_z, from_height);
	int32_t plain_cell = -1;
	bool plain = false;

	*reached_out = 1.0f;
	*height_out = from_height;
	if (navigation->blocked == NULL)
	{
		return true;
	}
	allowed = allowed < 0 ? BODY_POINTS : SDL_max(allowed, most_brushes);
	for (int k = 1; k <= samples; k++)
	{
		float t = (float)k / (float)samples;
		float x = from_x + dx * t;
		float z = from_z + dz * t;
		float height_there = height;
		int32_t cell;
		int32_t sub_x;
		int32_t sub_z;
		int tight;

		// (open ground all round, most of most lines: only whether it
		// stands there, not its body's eight points too)
		if (agent->radius_meters < 0.5f * NAVIGATION_CELL_METERS && quarter_of_point(navigation, x, z, &cell, &sub_x, &sub_z))
		{
			if (cell != plain_cell)
			{
				plain_cell = cell;
				plain = plain_round(navigation, agent, cell);
			}
			if (plain)
			{
				if (!quarter_stand(navigation, terrain, water, agent, cell, sub_x, sub_z, height, &height_there))
				{
					*reached_out = (float)(k - 1) / (float)samples;
					*height_out = height;
					return false;
				}
				height = height_there;
				allowed = most_brushes;
				continue;
			}
		}
		tight = quarter_tightness(navigation, terrain, water, agent, x, z, height, &height_there);
		if (tight < 0 || tight > allowed)
		{
			*reached_out = (float)(k - 1) / (float)samples;
			*height_out = height;
			return false;
		}
		height = height_there;
		// (out of a tight place: not back into one as tight further on)
		allowed = SDL_max(tight, most_brushes);
	}
	*height_out = height;
	return true;
}

bool navigation_body_line(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, int most_brushes)
{
	float reached;
	float height;

	return body_line_reach(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, most_brushes, &reached, &height);
}

// The fine way: a search over the quarter meters in a window round the
// two points (FINE_MARGIN_QUARTERS beyond them, at most FINE_SPAN_QUARTERS
// a side), each quarter one it stands on (quarter_stand) with room for
// its body round it (navigation_quarter_tightness 0; or, dearer, without:
// out from beside a post it brushed): round a post, through a gap a
// cell's corner makes. One layer: each quarter's height as first found
// (from the side it was come to from), enough in a few meters.
#define FINE_SPAN_QUARTERS 48
#define FINE_MARGIN_QUARTERS 6
#define FINE_UNKNOWN 0
#define FINE_CLEAR 1
#define FINE_TIGHT 2                 // stood on, but its body not clear (beside a post): dearer, to get out from there
#define FINE_NOT 3
#define FINE_TIGHT_COST 4.0f
#define FINE_ON_WAY 2                // a quarter of the way found (among the search's closed ones, for fine_visible)

typedef struct
{
	const navigation_t *navigation;
	const terrain_t *terrain;
	const water_t *water;
	const navigation_agent_t *agent;
	int32_t x0;                        // the window's first quarter, over the whole map
	int32_t z0;
	int32_t width;
	int32_t depth;
	uint8_t *state;
	float *height;
} fine_window_t;

// A quarter over the whole map (x, z in quarters): standable at near, and
// at what height
static bool fine_stand(const fine_window_t *window, int32_t qx, int32_t qz, float near_height, float *height_out)
{
	int32_t cell_x = qx / NAVIGATION_SUBCELLS;
	int32_t cell_z = qz / NAVIGATION_SUBCELLS;

	if (qx < 0 || qz < 0 || cell_x >= window->navigation->size || cell_z >= window->navigation->size)
	{
		return false;
	}
	return quarter_stand(window->navigation, window->terrain, window->water, window->agent, cell_z * window->navigation->size + cell_x,
		qx % NAVIGATION_SUBCELLS, qz % NAVIGATION_SUBCELLS, near_height, height_out);
}

// The window's quarter (local) for the body, found from near_height
// (cached): FINE_CLEAR, FINE_TIGHT or FINE_NOT (a step off near_height
// too)
static int fine_state(fine_window_t *window, int32_t local_x, int32_t local_z, float near_height)
{
	size_t at;
	float height = near_height;

	if (local_x < 0 || local_z < 0 || local_x >= window->width || local_z >= window->depth)
	{
		return FINE_NOT;
	}
	at = (size_t)local_z * (size_t)window->width + (size_t)local_x;
	if (window->state[at] == FINE_UNKNOWN)
	{
		const float quarters_per_meter = (float)NAVIGATION_SUBCELLS / NAVIGATION_CELL_METERS;
		bool stands = fine_stand(window, window->x0 + local_x, window->z0 + local_z, near_height, &height);
		bool clear = stands && navigation_quarter_tightness(window->navigation, window->terrain, window->water, window->agent,
			((float)(window->x0 + local_x) + 0.5f) / quarters_per_meter, ((float)(window->z0 + local_z) + 0.5f) / quarters_per_meter, height) == 0;

		window->state[at] = clear ? FINE_CLEAR : stands ? FINE_TIGHT : FINE_NOT;
		window->height[at] = height;
	}
	if (SDL_fabsf(window->height[at] - near_height) > window->agent->step_meters + 0.05f)
	{
		return FINE_NOT;
	}
	return window->state[at];
}

// Whether the straight line between two window quarters' centers keeps to
// clear quarters, a step at a time from from_height; and to tight ones
// only where the way found goes over them itself (on_way: out from beside
// a post), or where it is tight all the way between the two (all_tight: a
// porch as narrow as it is, no clear way along it to keep to). Not over
// any other: a line pulled straight past a clear way's corner went over
// the strip beside a stair, off its side
static bool fine_visible(fine_window_t *window, const uint8_t *on_way, bool all_tight, int32_t a_x, int32_t a_z, int32_t b_x, int32_t b_z,
	float from_height)
{
	float dx = (float)(b_x - a_x);
	float dz = (float)(b_z - a_z);
	int samples = (int)(SDL_sqrtf(dx * dx + dz * dz) * 3.0f) + 1;
	float height = from_height;

	for (int k = 1; k <= samples; k++)
	{
		float t = (float)k / (float)samples;
		int32_t x = (int32_t)SDL_floorf((float)a_x + 0.5f + dx * t);
		int32_t z = (int32_t)SDL_floorf((float)a_z + 0.5f + dz * t);
		int state = fine_state(window, x, z, height);
		size_t at = (size_t)z * (size_t)window->width + (size_t)x;

		if (state == FINE_NOT || (state == FINE_TIGHT && !all_tight && on_way[at] != FINE_ON_WAY))
		{
			return false;
		}
		height = window->height[at];
	}
	return true;
}

// The fine search's own (one search at a time: the walkers are moved on
// one thread)
#define FINE_QUARTERS_MOST (FINE_SPAN_QUARTERS * FINE_SPAN_QUARTERS)
#define FINE_HEAP_MOST (FINE_QUARTERS_MOST * 8)
// A tight quarter by the goal come to first: this many quarters more
// looked at for a clear one there
#define FINE_CLEAR_GOAL_MORE 48
// So many quarters looked at and not there: no way (a way round a post or
// two takes a few hundred; a goal behind a wall took every quarter of
// the window, a few milliseconds, each time it was asked)
#define FINE_QUARTERS_LOOKED_MOST 800

static uint8_t fine_states[FINE_QUARTERS_MOST];
static float fine_heights[FINE_QUARTERS_MOST];
static float fine_costs[FINE_QUARTERS_MOST];
static int32_t fine_parents[FINE_QUARTERS_MOST];
static uint8_t fine_closed[FINE_QUARTERS_MOST];
static int32_t fine_heap_nodes[FINE_HEAP_MOST];
static float fine_heap_scores[FINE_HEAP_MOST];
static int fine_heap_count;

static void fine_heap_push(int32_t node, float score)
{
	int at = fine_heap_count;

	if (fine_heap_count >= FINE_HEAP_MOST)
	{
		return;
	}
	fine_heap_count++;
	while (at > 0 && fine_heap_scores[(at - 1) / 2] > score)
	{
		fine_heap_nodes[at] = fine_heap_nodes[(at - 1) / 2];
		fine_heap_scores[at] = fine_heap_scores[(at - 1) / 2];
		at = (at - 1) / 2;
	}
	fine_heap_nodes[at] = node;
	fine_heap_scores[at] = score;
}

static int32_t fine_heap_pop(void)
{
	int32_t top = fine_heap_nodes[0];
	int32_t node;
	float score;
	int at = 0;

	fine_heap_count--;
	node = fine_heap_nodes[fine_heap_count];
	score = fine_heap_scores[fine_heap_count];
	for (;;)
	{
		int child = at * 2 + 1;

		if (child >= fine_heap_count)
		{
			break;
		}
		if (child + 1 < fine_heap_count && fine_heap_scores[child + 1] < fine_heap_scores[child])
		{
			child++;
		}
		if (fine_heap_scores[child] >= score)
		{
			break;
		}
		fine_heap_nodes[at] = fine_heap_nodes[child];
		fine_heap_scores[at] = fine_heap_scores[child];
		at = child;
	}
	fine_heap_nodes[at] = node;
	fine_heap_scores[at] = score;
	return top;
}

// A window quarter's distance to the goal's, in quarters (straight and
// diagonal steps)
static float fine_goal_distance(const fine_window_t *window, int32_t at, int32_t to_qx, int32_t to_qz)
{
	int32_t ddx = SDL_abs(at % window->width + window->x0 - to_qx);
	int32_t ddz = SDL_abs(at / window->width + window->z0 - to_qz);

	return (float)SDL_max(ddx, ddz) + 0.414f * (float)SDL_min(ddx, ddz);
}

// navigation_fine_path, and the height it stands at at the way's end
static int fine_path_search(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float (*points_out)[2], int most, float *end_height_out)
{
	static int32_t chain[FINE_QUARTERS_MOST];
	static int32_t tight_before[FINE_QUARTERS_MOST + 1];
	const float quarters_per_meter = (float)NAVIGATION_SUBCELLS / NAVIGATION_CELL_METERS;
	int32_t from_qx = (int32_t)SDL_floorf(from_x * quarters_per_meter);
	int32_t from_qz = (int32_t)SDL_floorf(from_z * quarters_per_meter);
	int32_t to_qx = (int32_t)SDL_floorf(to_x * quarters_per_meter);
	int32_t to_qz = (int32_t)SDL_floorf(to_z * quarters_per_meter);
	fine_window_t window;
	size_t count;
	int32_t start;
	int32_t reached = -1;
	int32_t tight_by_goal = -1;
	int32_t clear_by_goal = -1;
	int more = FINE_CLEAR_GOAL_MORE;
	int looked = 0;
	bool start_tight;
	int chain_count = 0;
	int written = 0;

	*end_height_out = from_height;
	window.navigation = navigation;
	window.terrain = terrain;
	window.water = water;
	window.agent = agent;
	window.x0 = SDL_min(from_qx, to_qx) - FINE_MARGIN_QUARTERS;
	window.z0 = SDL_min(from_qz, to_qz) - FINE_MARGIN_QUARTERS;
	window.width = SDL_max(from_qx, to_qx) + FINE_MARGIN_QUARTERS + 1 - window.x0;
	window.depth = SDL_max(from_qz, to_qz) + FINE_MARGIN_QUARTERS + 1 - window.z0;
	window.state = fine_states;
	window.height = fine_heights;
	if (window.width > FINE_SPAN_QUARTERS || window.depth > FINE_SPAN_QUARTERS || most < 1 || navigation->blocked == NULL)
	{
		return 0;
	}
	count = (size_t)window.width * (size_t)window.depth;
	SDL_memset(fine_states, FINE_UNKNOWN, count);
	SDL_memset(fine_closed, 0, count);
	for (size_t k = 0; k < count; k++)
	{
		fine_costs[k] = 1e30f;
		fine_parents[k] = -1;
	}
	// (where it stands: its own, clear or not, a brush against a post)
	start = (from_qz - window.z0) * window.width + (from_qx - window.x0);
	start_tight = navigation_quarter_tightness(navigation, terrain, water, agent, from_x, from_z, from_height) != 0;
	window.state[start] = FINE_CLEAR;
	window.height[start] = from_height;
	fine_costs[start] = 0.0f;
	fine_heap_count = 0;
	fine_heap_push(start, fine_goal_distance(&window, start, to_qx, to_qz));
	// A*
	while (fine_heap_count > 0)
	{
		int32_t best = fine_heap_pop();
		int32_t x = best % window.width;
		int32_t z = best / window.width;

		if (fine_closed[best] != 0)
		{
			continue;
		}
		if (++looked > FINE_QUARTERS_LOOKED_MOST)
		{
			break;
		}
		fine_closed[best] = 1;
		// (there: in the goal's quarter, clear; or, the goal on a cell's
		// corner beside a post, one beside it if the goal's own does not
		// turn up clear soon: a clear one rather than a tight one)
		if (SDL_abs(x + window.x0 - to_qx) <= 1 && SDL_abs(z + window.z0 - to_qz) <= 1)
		{
			bool clear = window.state[best] == FINE_CLEAR && !(best == start && start_tight);

			if (clear && x + window.x0 == to_qx && z + window.z0 == to_qz)
			{
				reached = best;
				break;
			}
			if (clear && clear_by_goal < 0)
			{
				clear_by_goal = best;
			}
			if (!clear && tight_by_goal < 0)
			{
				tight_by_goal = best;
			}
		}
		if ((tight_by_goal >= 0 || clear_by_goal >= 0) && --more <= 0)
		{
			break;
		}
		for (int32_t dz = -1; dz <= 1; dz++)
		{
			for (int32_t dx = -1; dx <= 1; dx++)
			{
				int32_t next_x = x + dx;
				int32_t next_z = z + dz;
				int32_t next;
				float step_cost;
				int state;

				if (dx == 0 && dz == 0)
				{
					continue;
				}
				state = fine_state(&window, next_x, next_z, window.height[best]);
				if (state == FINE_NOT)
				{
					continue;
				}
				// (corners not cut: a diagonal with both sides stood on)
				if (dx != 0 && dz != 0 && (fine_state(&window, x + dx, z, window.height[best]) == FINE_NOT
					|| fine_state(&window, x, z + dz, window.height[best]) == FINE_NOT))
				{
					continue;
				}
				next = next_z * window.width + next_x;
				step_cost = (dx != 0 && dz != 0 ? 1.414f : 1.0f) * (state == FINE_TIGHT ? FINE_TIGHT_COST : 1.0f);
				if (fine_closed[next] == 0 && fine_costs[best] + step_cost < fine_costs[next])
				{
					fine_costs[next] = fine_costs[best] + step_cost;
					fine_parents[next] = best;
					fine_heap_push(next, fine_costs[next] + fine_goal_distance(&window, next, to_qx, to_qz));
				}
			}
		}
	}
	if (reached < 0)
	{
		reached = clear_by_goal >= 0 ? clear_by_goal : tight_by_goal;
	}
	if (reached >= 0)
	{
		// The quarters back to the start, then pulled straight: from each,
		// the farthest it sees
		for (int32_t at = reached; at >= 0 && chain_count < (int)SDL_arraysize(chain); at = fine_parents[at])
		{
			chain[chain_count++] = at;
		}
		// (how many tight ones before each of the way's: all of them tight
		// between two, and the line between may go over tight ones)
		tight_before[0] = 0;
		for (int k = 0; k < chain_count; k++)
		{
			bool tight = chain[k] == start ? start_tight : window.state[chain[k]] == FINE_TIGHT;

			fine_closed[chain[k]] = FINE_ON_WAY;
			tight_before[k + 1] = tight_before[k] + (tight ? 1 : 0);
		}
		for (int i = chain_count - 1; i > 0 && written < most;)
		{
			int j = 0;

			while (j < i - 1 && !fine_visible(&window, fine_closed, tight_before[i] - tight_before[j + 1] == i - j - 1, chain[i] % window.width,
				chain[i] / window.width, chain[j] % window.width, chain[j] / window.width, window.height[chain[i]]))
			{
				j++;
			}
			points_out[written][0] = ((float)(chain[j] % window.width + window.x0) + 0.5f) / quarters_per_meter;
			points_out[written][1] = ((float)(chain[j] / window.width + window.z0) + 0.5f) / quarters_per_meter;
			*end_height_out = window.height[chain[j]];
			written++;
			i = j;
		}
	}
	return written;
}

int navigation_fine_path(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float (*points_out)[2], int most)
{
	float end_height;

	return fine_path_search(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, points_out, most, &end_height);
}

// The node of the cell an agent on from can step onto (passable, no worse
// water than allowed, the nearest in height); -1 if none
static int32_t reachable_node(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, const node_info_t *from, int32_t cell, float distance, int allowed_class, node_info_t *info_out)
{
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count = cell_nodes(navigation, cell, nodes);
	int32_t best = -1;
	float best_distance = 1e30f;

	for (int k = 0; k < count; k++)
	{
		node_info_t info;

		node_info(navigation, terrain, water, agent, nodes[k], &info);
		if (!info.passable || info.water_class > (allowed_class & CLASS_MASK) || (info.small && (allowed_class & ALLOW_SMALL) == 0)
			|| !step_allowed(agent, from, &info, distance))
		{
			continue;
		}
		if (SDL_fabsf(info.height - from->height) < best_distance)
		{
			best_distance = SDL_fabsf(info.height - from->height);
			best = nodes[k];
			*info_out = info;
		}
	}
	return best;
}

// The straight line, sampled every half meter, walked surface to surface
static bool line_clear(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, int allowed_class, float *blocked_x, float *blocked_z,
	float *blocked_height)
{
	float dx = to_x - from_x;
	float dz = to_z - from_z;
	float length = SDL_sqrtf(dx * dx + dz * dz);
	int steps = SDL_max((int)SDL_ceilf(length / LINE_STEP_METERS), 1);
	int32_t previous_cell;
	node_info_t current;

	cell_of_point(navigation, from_x, from_z, &previous_cell);
	node_info(navigation, terrain, water, agent, nearest_node(navigation, terrain, previous_cell, from_height), &current);
	for (int step = 1; step <= steps; step++)
	{
		float amount = (float)step / (float)steps;
		int32_t cell;
		int32_t step_x;
		int32_t step_z;
		node_info_t next;

		if (!cell_of_point(navigation, from_x + dx * amount, from_z + dz * amount, &cell))
		{
			return false;
		}
		if (cell != previous_cell)
		{
			step_x = cell % navigation->size - previous_cell % navigation->size;
			step_z = cell / navigation->size - previous_cell / navigation->size;
			if (reachable_node(navigation, terrain, water, agent, &current, cell,
					SDL_sqrtf((float)(step_x * step_x + step_z * step_z)) * NAVIGATION_CELL_METERS, allowed_class, &next) < 0)
			{
				if (blocked_x != NULL)
				{
					*blocked_x = ((float)(cell % navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
					*blocked_z = ((float)(cell / navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
					*blocked_height = current.height;
				}
				return false;
			}
			current = next;
			previous_cell = cell;
		}
		// Its width: the cells beside the line, as far as its radius, must
		// let it by too (a stone the line just misses would scrape its flank)
		if (agent->radius_meters > 0.05f && length > 1e-4f)
		{
			float side_x = -dz / length * agent->radius_meters;
			float side_z = dx / length * agent->radius_meters;

			for (int side = -1; side <= 1; side += 2)
			{
				int32_t side_cell;
				node_info_t beside;

				if (!cell_of_point(navigation, from_x + dx * amount + side_x * (float)side, from_z + dz * amount + side_z * (float)side,
						&side_cell) || side_cell == cell)
				{
					continue;
				}
				if (reachable_node(navigation, terrain, water, agent, &current, side_cell, agent->radius_meters + 0.5f * NAVIGATION_CELL_METERS,
						allowed_class, &beside) < 0)
				{
					if (blocked_x != NULL)
					{
						*blocked_x = ((float)(side_cell % navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
						*blocked_z = ((float)(side_cell / navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
						*blocked_height = current.height;
					}
					return false;
				}
			}
		}
	}
	return true;
}

bool navigation_line_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float from_x, float from_z, float from_height, float to_x, float to_z, float *blocked_x,
	float *blocked_z, float *blocked_height)
{
	// Over the rectangles: they are built as needed (the navigation is only
	// read, but its mesh kept up)
	if (navigation->use_navmesh)
	{
		int result = mesh_line_passable((navigation_t *)navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, blocked_x,
			blocked_z, blocked_height);

		if (result >= 0)
		{
			return result == 1;
		}
	}
	return line_clear(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, CLASS_SWIM | ALLOW_SMALL, blocked_x, blocked_z,
		blocked_height);
}

// ---------------------------------------------------------------------------
// A*

static int32_t node_count(const navigation_t *navigation)
{
	return cell_count(navigation) + navigation->regions_per_side * navigation->regions_per_side * NAVIGATION_REGION_EXTRA_SLOTS;
}

static bool ensure_search_memory(navigation_t *navigation)
{
	size_t nodes = (size_t)node_count(navigation);

	if (navigation->search != NULL)
	{
		return true;
	}
	navigation->search = SDL_calloc(nodes, sizeof(navigation_search_node_t));
	navigation->search_number = 0;
	return navigation->search != NULL;
}

// The open list is a 4-ary heap: half as deep as a binary one, and a
// node's four children side by side in one cache line
static bool heap_push(navigation_t *navigation, int32_t node, float priority)
{
	navigation_heap_entry_t *heap;
	int32_t position;

	if (navigation->heap_count == navigation->heap_capacity)
	{
		int32_t capacity = SDL_max(4096, navigation->heap_capacity * 2);
		navigation_heap_entry_t *grown = SDL_realloc(navigation->heap, sizeof(navigation_heap_entry_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return false;
		}
		navigation->heap = grown;
		navigation->heap_capacity = capacity;
	}
	heap = navigation->heap;
	position = navigation->heap_count++;
	while (position > 0)
	{
		int32_t parent = (position - 1) / 4;

		if (heap[parent].priority <= priority)
		{
			break;
		}
		heap[position] = heap[parent];
		position = parent;
	}
	heap[position].priority = priority;
	heap[position].node = node;
	return true;
}

static int32_t heap_pop(navigation_t *navigation)
{
	navigation_heap_entry_t *heap = navigation->heap;
	int32_t top = heap[0].node;
	navigation_heap_entry_t last;
	int32_t position = 0;
	int32_t count;

	count = --navigation->heap_count;
	if (count == 0)
	{
		return top;
	}
	last = heap[count];
	for (;;)
	{
		int32_t first_child = position * 4 + 1;
		int32_t last_child = SDL_min(first_child + 3, count - 1);
		int32_t best_child;

		if (first_child >= count)
		{
			break;
		}
		best_child = first_child;
		for (int32_t child = first_child + 1; child <= last_child; child++)
		{
			best_child = heap[child].priority < heap[best_child].priority ? child : best_child;
		}
		if (heap[best_child].priority >= last.priority)
		{
			break;
		}
		heap[position] = heap[best_child];
		position = best_child;
	}
	heap[position] = last;
	return top;
}

// The walks' open list: a bucket queue. A priority (meters) goes into its
// bucket of NAVIGATION_BUCKET_METERS, the cheapest bucket is taken first:
// a push and a pop cost the same however many are waiting (a heap's pop
// went down its levels, a cache miss each, a quarter of a search's time)
static void buckets_clear(navigation_t *navigation)
{
	navigation->bucket_entry_count = 0;
	navigation->bucket_open_count = 0;
	navigation->bucket_current = -1;
	if (navigation->bucket_heads != NULL)
	{
		SDL_memset(navigation->bucket_heads, 0xff, sizeof(int32_t) * NAVIGATION_BUCKETS);
	}
}

static bool buckets_push(navigation_t *navigation, int32_t node, float priority)
{
	int64_t bucket = (int64_t)(priority * (1.0f / NAVIGATION_BUCKET_METERS));
	int32_t slot;
	int32_t entry;

	if (navigation->bucket_heads == NULL)
	{
		navigation->bucket_heads = SDL_malloc(sizeof(int32_t) * NAVIGATION_BUCKETS);
		if (navigation->bucket_heads == NULL)
		{
			return false;
		}
		SDL_memset(navigation->bucket_heads, 0xff, sizeof(int32_t) * NAVIGATION_BUCKETS);
	}
	if (navigation->bucket_entry_count == navigation->bucket_entry_capacity)
	{
		int32_t capacity = SDL_max(4096, navigation->bucket_entry_capacity * 2);
		int32_t *nodes = SDL_realloc(navigation->bucket_entry_nodes, sizeof(int32_t) * (size_t)capacity);
		int32_t *next;

		if (nodes == NULL)
		{
			return false;
		}
		navigation->bucket_entry_nodes = nodes;
		next = SDL_realloc(navigation->bucket_entry_next, sizeof(int32_t) * (size_t)capacity);
		if (next == NULL)
		{
			return false;
		}
		navigation->bucket_entry_next = next;
		navigation->bucket_entry_capacity = capacity;
	}
	// (the first sets where the ring starts; cheaper than the cheapest
	// waiting, which the weighed estimate allows now and then: the cheapest;
	// beyond the ring: its last)
	if (navigation->bucket_current < 0)
	{
		navigation->bucket_current = bucket;
	}
	bucket = SDL_clamp(bucket, navigation->bucket_current, navigation->bucket_current + NAVIGATION_BUCKETS - 1);
	slot = (int32_t)(bucket % NAVIGATION_BUCKETS);
	entry = navigation->bucket_entry_count++;
	navigation->bucket_entry_nodes[entry] = node;
	navigation->bucket_entry_next[entry] = navigation->bucket_heads[slot];
	navigation->bucket_heads[slot] = entry;
	navigation->bucket_open_count++;
	return true;
}

// The cheapest waiting (there must be one: bucket_open_count > 0)
static int32_t buckets_pop(navigation_t *navigation)
{
	int32_t slot = (int32_t)(navigation->bucket_current % NAVIGATION_BUCKETS);
	int32_t entry;

	while (navigation->bucket_heads[slot] < 0)
	{
		navigation->bucket_current++;
		slot = slot + 1 == NAVIGATION_BUCKETS ? 0 : slot + 1;
	}
	entry = navigation->bucket_heads[slot];
	navigation->bucket_heads[slot] = navigation->bucket_entry_next[entry];
	navigation->bucket_open_count--;
	return navigation->bucket_entry_nodes[entry];
}

// Octile distance: what the trip costs at best (dry, flat)
static float estimate(int32_t from_x, int32_t from_z, int32_t to_x, int32_t to_z)
{
	int32_t dx = to_x > from_x ? to_x - from_x : from_x - to_x;
	int32_t dz = to_z > from_z ? to_z - from_z : from_z - to_z;

	return ((float)SDL_max(dx, dz) + 0.41421356f * (float)SDL_min(dx, dz)) * NAVIGATION_CELL_METERS;
}

// Is any surface of the cell roomy enough and within reach (for corners)?
// Only the room and the heights: cheaper than a full look, and a corner is
// only brushed past
static bool cell_open(const navigation_t *navigation, const navigation_agent_t *agent, const node_info_t *from, int32_t cell, int32_t cell_x,
	int32_t cell_z)
{
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count = cell_nodes_at(navigation, cell, cell_x, cell_z, nodes);
	float reach = SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);

	if (navigation->blocked != NULL && (navigation->blocked[cell] & agent->groups) != 0)
	{
		return false;
	}
	for (int k = 0; k < count; k++)
	{
		float height;
		uint8_t clearance;

		if (k == 0)
		{
			height = navigation->ground_height[cell];
			clearance = navigation->terrain_clearance[cell];
			if ((float)navigation->small_height[cell] * SMALL_UNIT_METERS >= agent->height_meters)
			{
				continue;                  // a stone taller than it
			}
		}
		else
		{
			int32_t index = nodes[k] - cell_count(navigation);
			const navigation_surface_t *surface =
				&navigation->regions[index / NAVIGATION_REGION_EXTRA_SLOTS].surfaces[index % NAVIGATION_REGION_EXTRA_SLOTS];

			height = surface->height;
			clearance = surface->clearance;
		}
		if ((clearance == NAVIGATION_CLEARANCE_OPEN || (float)clearance * NAVIGATION_CLEARANCE_UNIT_METERS >= agent->height_meters)
			&& (from->water_class == CLASS_SWIM || fabsf(height - from->height) <= reach))
		{
			return true;
		}
	}
	return false;
}

// The search's common step, done for all eight neighbors at once: from dry
// ground, among cells with no surfaces above their terrain, away from the
// map's edge, with no water anywhere round (its tiles dry). The same
// answers as the general way (node_info_at, cell_open, step_allowed,
// step_cost), worked out lane by lane in arrays of eight the compiler
// turns into vector instructions; only what can be stepped to comes out
// (valid_out), and at what cost from here. False: not such a place, the
// general way it is
static bool dry_block_steps(const navigation_t *navigation, const water_t *water, const navigation_agent_t *agent, const node_info_t *here,
	int32_t node, int32_t cell, int32_t cell_x, int32_t cell_z, bool valid_out[8], float costs_out[8])
{
	int32_t size = navigation->size;
	float height[8];
	float clearance[8];
	float small[8];
	float blocked[8];
	float distance[8];
	float walk[8];
	int32_t open[8];
	int32_t valid[8];
	float reach = SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);
	const uint8_t *above = &navigation->extra_count[cell - size - 1];
	const uint8_t *middle = &navigation->extra_count[cell - 1];
	const uint8_t *below = &navigation->extra_count[cell + size - 1];

	if (node != cell || here->water_class != CLASS_DRY || cell_x < 1 || cell_z < 1 || cell_x > size - 2 || cell_z > size - 2
		|| (above[0] | above[1] | above[2] | middle[0] | middle[2] | below[0] | below[1] | below[2]) != 0)
	{
		return false;
	}
	if (water != NULL)
	{
		// The water's tiles under the block's corners: all dry, none of its
		// cells has any (what water_dry_at would find for each)
		int32_t k = navigation->water_cells_per_cell;

		if (water->iced || k <= 0 || navigation->water_cells_for_size != water->cell_size_meters)
		{
			return false;
		}
		for (int corner = 0; corner < 4; corner++)
		{
			int32_t water_x = k * (cell_x + ((corner & 1) ? 1 : -1)) + (k + 1) / 2;
			int32_t water_z = k * (cell_z + ((corner & 2) ? 1 : -1)) + (k + 1) / 2;
			int32_t tile;

			if (water_x >= water->size || water_z >= water->size)
			{
				continue;
			}
			tile = (water_z / WATER_TILE_SIZE) * water->tiles_per_side + water_x / WATER_TILE_SIZE;
			if (water->tile_state[tile] != WATER_TILE_DRY || water->tile_max_depth[tile] > 0)
			{
				return false;
			}
		}
	}
	// The neighbors, in the directions' order
	for (int d = 0; d < 8; d++)
	{
		int32_t next = cell + direction_z[d] * size + direction_x[d];

		height[d] = navigation->ground_height[next];
		clearance[d] = (float)navigation->terrain_clearance[next];
		small[d] = (float)navigation->small_height[next] * SMALL_UNIT_METERS;
		blocked[d] = navigation->blocked != NULL && (navigation->blocked[next] & agent->groups) != 0 ? 1.0f : 0.0f;
		distance[d] = direction_length[d] * NAVIGATION_CELL_METERS;
		walk[d] = navigation->walk_factors[navigation->ground_slope[next]];
	}
#if defined(__SSE2__)
	// Four lanes at a time (SSE: every x86-64 has it), the same operations
	// in the same order as the general way, so the same answers to the bit
	{
		__m128 here_height = _mm_set1_ps(here->height);
		__m128 magnitude = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
		__m128 agent_height = _mm_set1_ps(agent->height_meters);
		__m128 step = _mm_set1_ps(agent->step_meters);

		for (int half = 0; half < 2; half++)
		{
			__m128 lane_height = _mm_loadu_ps(&height[4 * half]);
			__m128 lane_clearance = _mm_loadu_ps(&clearance[4 * half]);
			__m128 lane_small = _mm_loadu_ps(&small[4 * half]);
			__m128 lane_distance = _mm_loadu_ps(&distance[4 * half]);
			__m128 rise = _mm_and_ps(_mm_sub_ps(lane_height, here_height), magnitude);
			__m128 climb = _mm_max_ps(_mm_mul_ps(_mm_set1_ps(agent->maximum_slope), lane_distance), step);
			__m128 room = _mm_or_ps(_mm_cmpeq_ps(lane_clearance, _mm_set1_ps((float)NAVIGATION_CLEARANCE_OPEN)),
				_mm_cmpge_ps(_mm_mul_ps(lane_clearance, _mm_set1_ps(NAVIGATION_CLEARANCE_UNIT_METERS)), agent_height));
			__m128 free = _mm_and_ps(_mm_and_ps(room, _mm_cmplt_ps(lane_small, agent_height)),
				_mm_cmpeq_ps(_mm_loadu_ps(&blocked[4 * half]), _mm_setzero_ps()));
			__m128 cost = _mm_add_ps(_mm_mul_ps(lane_distance, _mm_loadu_ps(&walk[4 * half])),
				_mm_and_ps(_mm_cmpgt_ps(lane_small, step), _mm_set1_ps(navigation->small_detour_meters)));
			int open_bits = _mm_movemask_ps(_mm_and_ps(free, _mm_cmple_ps(rise, _mm_set1_ps(reach))));
			int valid_bits = _mm_movemask_ps(_mm_and_ps(free, _mm_cmple_ps(rise, climb)));

			_mm_storeu_ps(&costs_out[4 * half], cost);
			for (int lane = 0; lane < 4; lane++)
			{
				open[4 * half + lane] = (open_bits >> lane) & 1;
				valid[4 * half + lane] = (valid_bits >> lane) & 1;
			}
		}
	}
#else
	// Lane by lane, no branches: what each is, whether it can be gone to
	for (int d = 0; d < 8; d++)
	{
		float rise = fabsf(height[d] - here->height);
		float climb = agent->maximum_slope * distance[d];
		int32_t room = (clearance[d] == (float)NAVIGATION_CLEARANCE_OPEN) | (clearance[d] * NAVIGATION_CLEARANCE_UNIT_METERS >= agent->height_meters);
		int32_t free = room & (small[d] < agent->height_meters) & (blocked[d] == 0.0f);

		climb = climb > agent->step_meters ? climb : agent->step_meters;
		open[d] = free & (rise <= reach);
		valid[d] = free & (rise <= climb);
		costs_out[d] = distance[d] * walk[d] + (small[d] > agent->step_meters ? navigation->small_detour_meters : 0.0f);
	}
#endif
	// No cutting the corner of a cell that is not open (d 4..7: x then z)
	for (int d = 0; d < 8; d++)
	{
		valid_out[d] = d < 4 ? valid[d] != 0 : (valid[d] & open[direction_x[d] > 0 ? 0 : 1] & open[direction_z[d] > 0 ? 2 : 3]) != 0;
	}
	return true;
}

navigation_result_t navigation_find_path(navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float from_x, float from_z, float from_height, float to_x, float to_z,
	float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2], int *waypoint_count_out, bool *complete_out)
{
	int32_t size = navigation->size;
	int32_t start_cell;
	int32_t goal_cell;
	int32_t goal_x;
	int32_t goal_z;
	int32_t start;
	int32_t best;
	float best_distance;
	uint32_t reached_tag;
	int32_t path_length = 0;
	bool found = false;
	uint64_t search_start;
	float goal_distance = SDL_sqrtf((to_x - from_x) * (to_x - from_x) + (to_z - from_z) * (to_z - from_z));
	int32_t budget = (int32_t)SDL_min((float)navigation->maximum_expansions,
		(float)SEARCH_BASE_EXPANSIONS + (float)SEARCH_EXPANSIONS_PER_METER * goal_distance);

	*waypoint_count_out = 0;
	*complete_out = true;
	if (water != NULL && navigation->water_cells_for_size != water->cell_size_meters)
	{
		float ratio = NAVIGATION_CELL_METERS / water->cell_size_meters;
		int32_t whole = (int32_t)(ratio + 0.5f);

		navigation->water_cells_for_size = water->cell_size_meters;
		navigation->water_cells_per_cell = whole >= 1 && fabsf((float)whole * water->cell_size_meters - NAVIGATION_CELL_METERS) < 1e-6f ? whole : 0;
	}
	// The regions it will likely go through, as far as it searches
	prepare_way(navigation, from_x, from_z, to_x, to_z, SEARCH_REACH_FACTOR * SDL_sqrtf((float)budget) * NAVIGATION_CELL_METERS);
	// Over the rectangles (the cells' way where they have nothing at the start)
	if (navigation->use_navmesh)
	{
		int result = mesh_find_path(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, waypoints_out, waypoint_count_out,
			complete_out);

		if (result >= 0)
		{
			return (navigation_result_t)result;
		}
		*waypoint_count_out = 0;
		*complete_out = true;
	}
	cell_of_point(navigation, from_x, from_z, &start_cell);
	cell_of_point(navigation, to_x, to_z, &goal_cell);
	goal_x = goal_cell % size;
	goal_z = goal_cell / size;
	if (!ensure_search_memory(navigation))
	{
		return NAVIGATION_PATH_NONE;
	}
	navigation->search_number++;
	if (navigation->search_number >= 0x7FFFFFFFu)
	{
		// Wrapped: the old tags could look new
		SDL_memset(navigation->search, 0, sizeof(navigation_search_node_t) * (size_t)node_count(navigation));
		navigation->search_number = 1;
	}
	reached_tag = navigation->search_number * 2;
	buckets_clear(navigation);
	start = nearest_node(navigation, terrain, start_cell, from_height);
	navigation->search[start].tag = reached_tag;
	navigation->search[start].cost = 0.0f;
	navigation->search[start].came_from = -1;
	buckets_push(navigation, start, estimate(start_cell % size, start_cell / size, goal_x, goal_z));
	best = start;
	best_distance = estimate(start_cell % size, start_cell / size, goal_x, goal_z);
	navigation->last_expansions = 0;
	search_start = SDL_GetPerformanceCounter();
	navigation->last_pull_ticks = 0;

	while (navigation->bucket_open_count > 0)
	{
		int32_t node = buckets_pop(navigation);
		int32_t cell = node_cell(navigation, node);
		int32_t cell_x = cell % size;
		int32_t cell_z = cell / size;
		node_info_t here;
		bool side_open[4];
		bool block_valid[8];
		float block_costs[8];

		if (navigation->search[node].tag == reached_tag + 1)
		{
			continue;                  // a stale entry: already closed
		}
		navigation->search[node].tag = reached_tag + 1;
		if (cell == goal_cell)
		{
			found = true;
			best = node;
			break;
		}
		{
			float distance = estimate(cell_x, cell_z, goal_x, goal_z);

			if (distance < best_distance)
			{
				best_distance = distance;
				best = node;
			}
		}
		if (++navigation->last_expansions >= budget)
		{
			break;
		}
		// (its terrain the lean way: the same answers)
		if (node == cell)
		{
			terrain_node_info(navigation, terrain, water, agent, cell, cell_x, cell_z, &here);
		}
		else
		{
			node_info_at(navigation, terrain, water, agent, node, cell, cell_x, cell_z, &here);
		}
		// Through a door from here (its terrain: the doors' ends are on
		// the ground), to the other end's terrain, open or one it opens
		if (navigation->link_count > 0 && node == cell && here.passable)
		{
			for (int k = 0; k < navigation->link_count; k++)
			{
				int32_t next;
				int32_t next_x;
				int32_t next_z;
				node_info_t there;
				float cost;

				if (navigation->link_cells[k][0] == cell)
				{
					next = navigation->link_cells[k][1];
				}
				else if (navigation->link_cells[k][1] == cell)
				{
					next = navigation->link_cells[k][0];
				}
				else
				{
					continue;
				}
				if ((!navigation->links[k].open && !agent->opens_doors) || navigation->search[next].tag == reached_tag + 1)
				{
					continue;
				}
				next_x = next % size;
				next_z = next / size;
				terrain_node_info(navigation, terrain, water, agent, next, next_x, next_z, &there);
				if (!there.passable || SDL_fabsf(there.height - here.height) > SDL_max(agent->step_meters, 0.6f))
				{
					continue;
				}
				// (a shut one a little dearer: opening it takes a moment)
				cost = navigation->search[node].cost + navigation->link_meters[k] + (navigation->links[k].open ? 0.0f : LINK_SHUT_METERS);
				if (navigation->search[next].tag != reached_tag || cost < navigation->search[next].cost)
				{
					navigation->search[next].tag = reached_tag;
					navigation->search[next].cost = cost;
					navigation->search[next].came_from = node;
					if (!buckets_push(navigation, next, cost + HEURISTIC_WEIGHT * estimate(next_x, next_z, goal_x, goal_z)))
					{
						navigation->bucket_open_count = 0;
						break;
					}
				}
			}
		}
		if (dry_block_steps(navigation, water, agent, &here, node, cell, cell_x, cell_z, block_valid, block_costs))
		{
			for (int d = 0; d < 8; d++)
			{
				int32_t next = cell + direction_z[d] * size + direction_x[d];
				float cost;

				if (!block_valid[d] || navigation->search[next].tag == reached_tag + 1)
				{
					continue;
				}
				cost = navigation->search[node].cost + block_costs[d];
				if (navigation->search[next].tag != reached_tag || cost < navigation->search[next].cost)
				{
					navigation->search[next].tag = reached_tag;
					navigation->search[next].cost = cost;
					navigation->search[next].came_from = node;
					if (!buckets_push(navigation, next, cost + HEURISTIC_WEIGHT * estimate(cell_x + direction_x[d], cell_z + direction_z[d], goal_x,
						goal_z)))
					{
						navigation->bucket_open_count = 0;
						break;
					}
				}
			}
			continue;
		}
		// Whether the four cells beside it are open, for the diagonal steps
		// past their corners (each asked once, not by both diagonals)
		{
			bool inside = cell_x > 0 && cell_z > 0 && cell_x < size - 1 && cell_z < size - 1;

			for (int d = 0; d < 4; d++)
			{
				int32_t next_x = cell_x + direction_x[d];
				int32_t next_z = cell_z + direction_z[d];

				side_open[d] = (inside || (next_x >= 0 && next_z >= 0 && next_x < size && next_z < size))
					&& cell_open(navigation, agent, &here, next_z * size + next_x, next_x, next_z);
			}
		}
		for (int d = 0; d < 8; d++)
		{
			int32_t next_x = cell_x + direction_x[d];
			int32_t next_z = cell_z + direction_z[d];
			int32_t next_cell;
			int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
			int count;
			float distance = direction_length[d] * NAVIGATION_CELL_METERS;

			if (next_x < 0 || next_z < 0 || next_x >= size || next_z >= size)
			{
				continue;
			}
			next_cell = next_z * size + next_x;
			// No cutting the corner of a blocked cell (the one beside it
			// along x, and along z)
			if (d >= 4 && (!side_open[direction_x[d] > 0 ? 0 : 1] || !side_open[direction_z[d] > 0 ? 2 : 3]))
			{
				continue;
			}
			count = cell_nodes_at(navigation, next_cell, next_x, next_z, nodes);
			for (int k = 0; k < count; k++)
			{
				int32_t next = nodes[k];
				node_info_t there;
				float cost;

				if (navigation->search[next].tag == reached_tag + 1)
				{
					continue;
				}
				// Already open no dearer than this step could ever make it (a
				// step costs at least its length): nothing to look at
				if (navigation->search[next].tag == reached_tag
					&& navigation->search[next].cost <= navigation->search[node].cost + distance)
				{
					continue;
				}
				if (k == 0)
				{
					terrain_node_info(navigation, terrain, water, agent, next_cell, next_x, next_z, &there);
				}
				else
				{
					node_info_at(navigation, terrain, water, agent, next, next_cell, next_x, next_z, &there);
				}
				if (!there.passable || !step_allowed(agent, &here, &there, distance))
				{
					continue;
				}
				cost = navigation->search[node].cost + step_cost(agent, &here, &there, distance);
				if (navigation->search[next].tag != reached_tag || cost < navigation->search[next].cost)
				{
					navigation->search[next].tag = reached_tag;
					navigation->search[next].cost = cost;
					navigation->search[next].came_from = node;
					// The estimate weighed a little over the truth: the search heads
				// for the goal instead of flooding every cheaper corner around
				// a hill, for paths at most that much longer than the best
				if (!buckets_push(navigation, next, cost + HEURISTIC_WEIGHT * estimate(next_x, next_z, goal_x, goal_z)))
					{
						navigation->bucket_open_count = 0;
						break;
					}
				}
			}
		}
	}

	navigation->last_search_ticks = SDL_GetPerformanceCounter() - search_start;
	// The nodes from the end back to the start
	for (int32_t node = best; node != start; node = navigation->search[node].came_from)
	{
		if (path_length == navigation->node_path_capacity)
		{
			int32_t capacity = SDL_max(1024, navigation->node_path_capacity * 2);
			int32_t *grown = SDL_realloc(navigation->node_path, sizeof(int32_t) * (size_t)capacity);

			if (grown == NULL)
			{
				return NAVIGATION_PATH_NONE;
			}
			navigation->node_path = grown;
			navigation->node_path_capacity = capacity;
		}
		navigation->node_path[path_length++] = node;
	}
	if (path_length == 0)
	{
		if (found)
		{
			// Already in the goal's cell
			waypoints_out[0][0] = to_x;
			waypoints_out[0][1] = to_z;
			*waypoint_count_out = 1;
			return NAVIGATION_PATH_FOUND;
		}
		return NAVIGATION_PATH_NONE;
	}

	// Pull it tight: from each waypoint, straight on to the farthest path
	// node that can be reached along a line through ground no worse than
	// the path's own
	{
		float anchor_x = from_x;
		float anchor_z = from_z;
		float anchor_height = from_height;
		int32_t anchor = path_length;  // node_path runs end first: index path_length - 1 is the first step
		int count = 0;
		uint64_t pull_start = SDL_GetPerformanceCounter();

		while (anchor > 0)
		{
			int32_t visible = anchor - 1;
			int allowed_class = CLASS_DRY;
			int allow_small = 0;
			float visible_height = anchor_height;
			float point_x;
			float point_z;

			for (int32_t candidate = anchor - 1; candidate >= 0 && anchor - candidate <= PULL_MAXIMUM_CELLS; candidate--)
			{
				int32_t node = navigation->node_path[candidate];
				int32_t cell = node_cell(navigation, node);
				node_info_t info;
				float candidate_x = ((float)(cell % size) + 0.5f) * NAVIGATION_CELL_METERS;
				float candidate_z = ((float)(cell / size) + 0.5f) * NAVIGATION_CELL_METERS;

				node_info(navigation, terrain, water, agent, node, &info);
				allowed_class = SDL_max(allowed_class, info.water_class);
				allow_small |= info.small ? ALLOW_SMALL : 0;
				if (candidate == 0 && found)
				{
					candidate_x = to_x;
					candidate_z = to_z;
				}
				if (candidate == anchor - 1
					|| line_clear(navigation, terrain, water, agent, anchor_x, anchor_z, anchor_height, candidate_x, candidate_z,
						allowed_class | allow_small, NULL, NULL, NULL))
				{
					visible = candidate;
					visible_height = info.height;
				}
				else
				{
					break;
				}
			}
			{
				int32_t cell = node_cell(navigation, navigation->node_path[visible]);

				point_x = ((float)(cell % size) + 0.5f) * NAVIGATION_CELL_METERS;
				point_z = ((float)(cell / size) + 0.5f) * NAVIGATION_CELL_METERS;
			}
			if (visible == 0 && found)
			{
				point_x = to_x;
				point_z = to_z;
			}
			waypoints_out[count][0] = point_x;
			waypoints_out[count][1] = point_z;
			count++;
			anchor_x = point_x;
			anchor_z = point_z;
			anchor_height = visible_height;
			anchor = visible;
			if (count == NAVIGATION_MAX_WAYPOINTS && anchor > 0)
			{
				*complete_out = false;
				break;
			}
		}
		*waypoint_count_out = count;
		navigation->last_pull_ticks = SDL_GetPerformanceCounter() - pull_start;
	}
	// Through a door: its two ends where they are, not their cells' middles
	// (the way between them lined up with the gap, not across its posts)
	for (int w = 0; w + 1 < *waypoint_count_out && navigation->link_count > 0; w++)
	{
		int32_t here_cell;
		int32_t next_cell;

		cell_of_point(navigation, waypoints_out[w][0], waypoints_out[w][1], &here_cell);
		cell_of_point(navigation, waypoints_out[w + 1][0], waypoints_out[w + 1][1], &next_cell);
		for (int k = 0; k < navigation->link_count; k++)
		{
			int forward = navigation->link_cells[k][0] == here_cell && navigation->link_cells[k][1] == next_cell;
			int backward = navigation->link_cells[k][1] == here_cell && navigation->link_cells[k][0] == next_cell;

			if (!forward && !backward)
			{
				continue;
			}
			// (the goal itself kept where it is)
			if (!(w + 1 == *waypoint_count_out - 1 && found))
			{
				waypoints_out[w + 1][0] = forward ? navigation->links[k].b[0] : navigation->links[k].a[0];
				waypoints_out[w + 1][1] = forward ? navigation->links[k].b[1] : navigation->links[k].a[1];
			}
			waypoints_out[w][0] = forward ? navigation->links[k].a[0] : navigation->links[k].b[0];
			waypoints_out[w][1] = forward ? navigation->links[k].a[1] : navigation->links[k].b[1];
			break;
		}
	}
	return found ? NAVIGATION_PATH_FOUND : NAVIGATION_PATH_PARTIAL;
}

// ---------------------------------------------------------------------------
// The navigation mesh (see navigation.h)

#define MESH_PORTAL_RISE_MOST 2.0f     // a step higher than this between two rectangles: no portal
#define MESH_WATER_CHECK_MS 250u       // a tile's water's cells compared again at most this often
#define MESH_EXPANSIONS_DIVISOR 8      // a rectangle search's budget: the cells' over this
#define MESH_MINIMUM_EXPANSIONS 2048
#define MESH_RAY_MOST_STEPS 512
#define MESH_START_NEAR_CELLS 2        // a start on no rectangle: the nearest one this far round
#define MESH_TILE_CELLS_MOST 4096      // a region's cells at most (64 x 64)

// Thresholds a portal's steps are told apart by: a run of steps between the
// same two of them is one portal (whether an agent climbs them is the same)
static const float mesh_rise_levels[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.7f, 1.0f, 1.5f };

static int mesh_rise_level(float rise)
{
	int level = 0;

	while (level < (int)SDL_arraysize(mesh_rise_levels) && rise > mesh_rise_levels[level])
	{
		level++;
	}
	return level;
}

// An agent's width in quarter meters, the nearest (a hen 1, a man 2, a
// bear or a cow 3), at least 1, at most the widest class's
static int32_t agent_width_quarters(const navigation_agent_t *agent)
{
	int32_t quarters = (int32_t)SDL_floorf(2.0f * agent->radius_meters * (float)NAVIGATION_SUBCELLS / NAVIGATION_CELL_METERS + 0.5f);

	return SDL_clamp(quarters, 1, NAVIGATION_MESH_CLASSES);
}

// Its class: by its width in quarter meters
static int mesh_class_of(const navigation_agent_t *agent)
{
	return agent_width_quarters(agent) - 1;
}

static float agent_reach(const navigation_agent_t *agent)
{
	return SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);
}

// An agent seen: its class takes its reach, height and width in (the
// least reach and height, the widest); a change builds the class's tiles
// again (lazily)
static void mesh_note_agent(navigation_t *navigation, const navigation_agent_t *agent)
{
	navigation_mesh_class_t *class = &navigation->mesh.classes[mesh_class_of(agent)];
	float reach = agent_reach(agent);
	int32_t erosion = (int32_t)(agent->radius_meters / NAVIGATION_CELL_METERS + 0.25f);
	bool changed = false;

	if (reach < class->reach)
	{
		class->reach = reach;
		changed = true;
	}
	if (agent->height_meters < class->height)
	{
		class->height = agent->height_meters;
		changed = true;
	}
	if (agent->step_meters < class->step)
	{
		class->step = agent->step_meters;
		changed = true;
	}
	if (erosion > class->erosion_cells)
	{
		class->erosion_cells = erosion;
		changed = true;
	}
	if (changed)
	{
		class->epoch++;
	}
}

static void mesh_free_tile(navigation_mesh_tile_t *tile)
{
	SDL_free(tile->rects);
	SDL_free(tile->portals);
	for (int side = 0; side < 4; side++)
	{
		SDL_free(tile->border[side]);
	}
	SDL_free(tile->ground_rect);
	SDL_free(tile->extra_rect);
	SDL_zerop(tile);
}

static void mesh_free(navigation_t *navigation)
{
	int32_t region_count = navigation->regions_per_side * navigation->regions_per_side;

	for (int c = 0; c < NAVIGATION_MESH_CLASSES; c++)
	{
		navigation_mesh_class_t *class = &navigation->mesh.classes[c];

		for (int32_t r = 0; r < region_count && class->tiles != NULL; r++)
		{
			mesh_free_tile(&class->tiles[r]);
		}
		SDL_free(class->tiles);
		class->tiles = NULL;
	}
}

static bool mesh_allocate(navigation_t *navigation)
{
	int32_t region_count = navigation->regions_per_side * navigation->regions_per_side;

	// (no portal shut: they were other tiles')
	navigation->mesh.shut_count = 0;
	navigation->mesh.shut_next = 0;
	for (int c = 0; c < NAVIGATION_MESH_CLASSES; c++)
	{
		navigation_mesh_class_t *class = &navigation->mesh.classes[c];

		class->tiles = SDL_calloc((size_t)region_count, sizeof(navigation_mesh_tile_t));
		if (class->tiles == NULL)
		{
			return false;
		}
		// (no agent seen yet: the first one sets it)
		class->reach = 1e9f;
		class->height = 1e9f;
		class->step = 1e9f;
		class->erosion_cells = 0;
		class->width_quarters = c + 1;
		class->epoch++;
	}
	return true;
}

static bool mesh_grow(void **items, int32_t *capacity, int32_t needed, size_t item_size)
{
	if (needed <= *capacity)
	{
		return true;
	}
	{
		int32_t grown_capacity = SDL_max(needed, SDL_max(64, *capacity * 2));
		void *grown = SDL_realloc(*items, item_size * (size_t)grown_capacity);

		if (grown == NULL)
		{
			return false;
		}
		*items = grown;
		*capacity = grown_capacity;
	}
	return true;
}

static navigation_rect_t *mesh_rect(const navigation_t *navigation, int c, int32_t id)
{
	return &navigation->mesh.classes[c].tiles[id >> 16].rects[id & 0xFFFF];
}

// What a node is to a class at a build: whether it can be in a rectangle,
// and what rectangles it may share
typedef struct
{
	bool usable;
	bool wet;
	uint8_t kind;
	uint8_t clearance;
	uint8_t clearance_bucket;
	uint8_t blocked;
	uint8_t small;
	uint8_t slope_bucket;
	float height;
	float cost;
} mesh_node_t;

// The room a class needs over a quarter meter (its lowest agent's
// height, NAVIGATION_CLEARANCE_UNIT_METERS each)
static uint8_t mesh_room_needed(const navigation_mesh_class_t *class)
{
	return (uint8_t)SDL_clamp(SDL_ceilf(SDL_min(class->height, 50.0f) / NAVIGATION_CLEARANCE_UNIT_METERS), 0.0f,
		(float)(NAVIGATION_CLEARANCE_OPEN - 1));
}

// The most room over a square of quarter meters the class's width a side
// that takes in some of a surface's own (the least of the square's
// quarters'), over its cell and the eight round it: there a quarter is
// any node's within the class's reach of the surface's height (a stair's
// treads in the next cell, the ground beside it), its room as that node
// has it. 0 where no such square fits: a strip along an edge, the ledge
// outside a balustrade, the floor of a doorway too narrow
#define SQUARE_SPAN (3 * NAVIGATION_SUBCELLS)

static uint8_t surface_square_room(const navigation_t *navigation, const navigation_mesh_class_t *class, int32_t node, int32_t cell)
{
	const navigation_surface_t *surface = extra_surface(navigation, node);
	int32_t cells = cell_count(navigation);
	int32_t cell_x = cell % navigation->size;
	int32_t cell_z = cell / navigation->size;
	int32_t width = class->width_quarters;
	float reach = SDL_min(class->reach, 50.0f);
	uint8_t room[SQUARE_SPAN * SQUARE_SPAN];
	uint8_t best = 0;

	SDL_memset(room, 0, sizeof(room));
	for (int32_t dz = -1; dz <= 1; dz++)
	{
		for (int32_t dx = -1; dx <= 1; dx++)
		{
			int32_t near_x = cell_x + dx;
			int32_t near_z = cell_z + dz;
			int32_t near_cell;
			int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
			int count;

			if (near_x < 0 || near_z < 0 || near_x >= navigation->size || near_z >= navigation->size)
			{
				continue;
			}
			near_cell = near_z * navigation->size + near_x;
			// (its own cell: its quarters only)
			count = dx == 0 && dz == 0 ? 1 : cell_nodes(navigation, near_cell, nodes);
			if (dx == 0 && dz == 0)
			{
				nodes[0] = node;
			}
			for (int n = 0; n < count; n++)
			{
				for (int32_t sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
				{
					int32_t at = ((dz + 1) * NAVIGATION_SUBCELLS + sub / NAVIGATION_SUBCELLS) * SQUARE_SPAN + (dx + 1) * NAVIGATION_SUBCELLS
						+ sub % NAVIGATION_SUBCELLS;
					uint8_t quarter_room;
					float height;

					if (nodes[n] < cells)
					{
						height = navigation->ground_height[near_cell];
						quarter_room = navigation->terrain_clearance[near_cell];
					}
					else
					{
						const navigation_surface_t *other = extra_surface(navigation, nodes[n]);

						if (other->below[sub] >= NAVIGATION_SUBCELL_BLOCKED)
						{
							continue;
						}
						height = other->height - (float)other->below[sub] * NAVIGATION_SUBCELL_UNIT_METERS;
						quarter_room = other->room[sub];
					}
					if (SDL_fabsf(height - surface->height) <= reach + 0.01f || nodes[n] == node)
					{
						room[at] = SDL_max(room[at], quarter_room);
					}
				}
			}
		}
	}
	// (the squares that take in a quarter of its own cell)
	for (int32_t z0 = NAVIGATION_SUBCELLS - width + 1; z0 < 2 * NAVIGATION_SUBCELLS; z0++)
	{
		for (int32_t x0 = NAVIGATION_SUBCELLS - width + 1; x0 < 2 * NAVIGATION_SUBCELLS; x0++)
		{
			uint8_t least = NAVIGATION_CLEARANCE_OPEN;
			bool own = false;

			for (int32_t z = z0; z < z0 + width && least > best; z++)
			{
				for (int32_t x = x0; x < x0 + width && least > best; x++)
				{
					least = SDL_min(least, room[z * SQUARE_SPAN + x]);
					own = own || (x >= NAVIGATION_SUBCELLS && x < 2 * NAVIGATION_SUBCELLS && z >= NAVIGATION_SUBCELLS && z < 2 * NAVIGATION_SUBCELLS
						&& surface->below[(z - NAVIGATION_SUBCELLS) * NAVIGATION_SUBCELLS + x - NAVIGATION_SUBCELLS] < NAVIGATION_SUBCELL_BLOCKED);
				}
			}
			if (own)
			{
				best = SDL_max(best, least);
			}
		}
	}
	return best;
}

static void mesh_node_props(const navigation_t *navigation, const navigation_mesh_class_t *class, int32_t node, int32_t cell, bool wet,
	bool eroded, mesh_node_t *out)
{
	int32_t cells = cell_count(navigation);

	out->kind = node < cells ? 0 : 1;
	out->blocked = navigation->blocked != NULL ? navigation->blocked[cell] : 0;
	out->wet = wet;
	if (node < cells)
	{
		out->height = navigation->ground_height[cell];
		out->clearance = navigation->terrain_clearance[cell];
		out->small = navigation->small_height[cell];
		out->cost = navigation->walk_factors[navigation->ground_slope[cell]];
		out->slope_bucket = (uint8_t)SDL_clamp((int)((out->cost - 1.0f) * 4.0f), 0, 31);
	}
	else
	{
		const navigation_surface_t *surface = extra_surface(navigation, node);

		out->height = surface->height;
		// (the room over the best square of it the class's width: none on
		// a strip too narrow for it, the low under a rail left out)
		out->clearance = surface_square_room(navigation, class, node, cell);
		out->small = 0;
		out->cost = 1.0f;
		out->slope_bucket = 0;
		eroded = false;
	}
	out->clearance_bucket = out->clearance == NAVIGATION_CLEARANCE_OPEN ? 255 : (uint8_t)(out->clearance / 5);
	// (room for the class's lowest; the small object and the groups are each
	// agent's own, at the search)
	out->usable = !eroded && (out->clearance == NAVIGATION_CLEARANCE_OPEN
		|| (float)out->clearance * NAVIGATION_CLEARANCE_UNIT_METERS >= SDL_min(class->height, 50.0f));
}

static bool mesh_alike(const mesh_node_t *a, const mesh_node_t *b, float reach)
{
	return a->kind == b->kind && a->clearance_bucket == b->clearance_bucket && a->blocked == b->blocked && a->small == b->small
		&& a->slope_bucket == b->slope_bucket && !a->wet && !b->wet && SDL_fabsf(a->height - b->height) <= reach;
}

// The build's working state for one tile
typedef struct
{
	int32_t first_x;
	int32_t first_z;
	int32_t width;
	int32_t depth;
	int32_t region;
	bool wet[MESH_TILE_CELLS_MOST];
	bool eroded[MESH_TILE_CELLS_MOST];
} mesh_build_t;

// The rectangle a node is in, in the tile being built (-1: none yet)
static int32_t mesh_rect_of_node_local(const navigation_t *navigation, const navigation_mesh_tile_t *tile, const mesh_build_t *build,
	int32_t node, int32_t cell)
{
	int32_t cells = cell_count(navigation);

	if (node < cells)
	{
		return tile->ground_rect[(cell / navigation->size - build->first_z) * navigation->region_size + cell % navigation->size - build->first_x];
	}
	return tile->extra_rect[(node - cells) % NAVIGATION_REGION_EXTRA_SLOTS];
}

static void mesh_build_props(const navigation_t *navigation, const navigation_mesh_class_t *class, const mesh_build_t *build, int32_t node,
	int32_t cell, mesh_node_t *out)
{
	int32_t local = (cell / navigation->size - build->first_z) * build->width + cell % navigation->size - build->first_x;

	mesh_node_props(navigation, class, node, cell, build->wet[local], build->eroded[local], out);
}

// The node of to_cell (in the tile) that can join from_node's rectangle:
// usable, in none yet, alike it, the nearest in height; -1 if none
static float node_edge_rise(const navigation_t *navigation, int32_t from_node, int32_t from_cell, int32_t to_node, int32_t to_cell, int side,
	uint8_t room_needed);

static int32_t mesh_match(const navigation_t *navigation, const navigation_mesh_class_t *class, const navigation_mesh_tile_t *tile,
	const mesh_build_t *build, int32_t from_node, int32_t from_cell, int32_t to_cell, int side, float reach)
{
	mesh_node_t from;
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count = cell_nodes(navigation, to_cell, nodes);
	int32_t best = -1;
	float best_difference = 1e30f;

	mesh_build_props(navigation, class, build, from_node, from_cell, &from);
	for (int k = 0; k < count; k++)
	{
		mesh_node_t candidate;

		if (mesh_rect_of_node_local(navigation, tile, build, nodes[k], to_cell) >= 0)
		{
			continue;
		}
		mesh_build_props(navigation, class, build, nodes[k], to_cell, &candidate);
		if (candidate.usable && mesh_alike(&from, &candidate, reach) && SDL_fabsf(candidate.height - from.height) < best_difference
			&& node_edge_rise(navigation, from_node, from_cell, nodes[k], to_cell, side, mesh_room_needed(class)) <= reach)
		{
			best_difference = SDL_fabsf(candidate.height - from.height);
			best = nodes[k];
		}
	}
	return best;
}

// The rectangle (global id) a node is in, if its region's tile of the class
// is built; -1 if none
static int32_t mesh_rect_of_node(const navigation_t *navigation, int c, int32_t node, int32_t cell)
{
	int32_t cells = cell_count(navigation);
	int32_t region = region_of_cell(navigation, cell);
	const navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[region];
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;
	int16_t rect;

	if (!tile->built)
	{
		return -1;
	}
	if (node < cells)
	{
		region_bounds(navigation, region, &first_x, &first_z, &last_x, &last_z);
		rect = tile->ground_rect[(cell / navigation->size - first_z) * navigation->region_size + cell % navigation->size - first_x];
	}
	else
	{
		rect = tile->extra_rect[(node - cells) % NAVIGATION_REGION_EXTRA_SLOTS];
	}
	return rect < 0 ? -1 : (region << 16) | rect;
}

// The node of a rectangle in one of its cells
static int32_t mesh_node_in(const navigation_t *navigation, int c, int32_t id, int32_t cell)
{
	const navigation_rect_t *rect = mesh_rect(navigation, c, id);
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count;

	if (rect->kind == 0)
	{
		return cell;
	}
	count = cell_nodes(navigation, cell, nodes);
	for (int k = 1; k < count; k++)
	{
		if (mesh_rect_of_node(navigation, c, nodes[k], cell) == id)
		{
			return nodes[k];
		}
	}
	return -1;
}

static float node_height(const navigation_t *navigation, int32_t node, int32_t cell)
{
	return node < cell_count(navigation) ? navigation->ground_height[cell] : extra_surface(navigation, node)->height;
}

// A node's height at a quarter meter of its cell (sub_x, sub_z): the
// ground's (its middle's), an object's top where it reaches there; false
// where it does not (blocked_out: and something stands there at its height)
static bool node_sub_height(const navigation_t *navigation, int32_t node, int32_t cell, int32_t sub_x, int32_t sub_z, uint8_t room_needed,
	float *height_out, bool *blocked_out)
{
	const navigation_surface_t *surface;
	uint8_t below;

	*blocked_out = false;
	if (node < cell_count(navigation))
	{
		*height_out = navigation->ground_height[cell];
		return true;
	}
	surface = extra_surface(navigation, node);
	below = surface->below[sub_z * NAVIGATION_SUBCELLS + sub_x];
	if (below >= NAVIGATION_SUBCELL_BLOCKED)
	{
		*blocked_out = below == NAVIGATION_SUBCELL_BLOCKED;
		return false;
	}
	// (its, but too low for the class: under a balustrade's rail, as good
	// as a wall for it)
	if (surface->room[sub_z * NAVIGATION_SUBCELLS + sub_x] < room_needed)
	{
		*blocked_out = true;
		return false;
	}
	*height_out = surface->height - (float)below * NAVIGATION_SUBCELL_UNIT_METERS;
	return true;
}

// The step from one node into the next cell's over side (0: +x, 1: -x,
// 2: +z, 3: -z) in one line of quarter meters across the edge (k along
// it): each node's quarter nearest the edge in that line (a stair's first
// tread a quarter in from its cell's edge still faces the ground beyond),
// over bare ground in from the edge but not past what stands there (a
// floor beyond a wall in the same cell does not face the edge); 1e30
// where none face each other
static float node_edge_line_rise(const navigation_t *navigation, int32_t from_node, int32_t from_cell, int32_t to_node, int32_t to_cell, int side,
	int32_t k, uint8_t room_needed)
{
	int32_t last = NAVIGATION_SUBCELLS - 1;
	float from_height = 0.0f;
	float to_height = 0.0f;
	bool from_found = false;
	bool to_found = false;
	bool stopped = false;

	for (int32_t in = 0; in < NAVIGATION_SUBCELLS && !from_found && !stopped; in++)
	{
		int32_t from_x = side == 0 ? last - in : side == 1 ? in : k;
		int32_t from_z = side == 2 ? last - in : side == 3 ? in : k;

		from_found = node_sub_height(navigation, from_node, from_cell, from_x, from_z, room_needed, &from_height, &stopped);
	}
	stopped = false;
	for (int32_t in = 0; in < NAVIGATION_SUBCELLS && !to_found && !stopped; in++)
	{
		int32_t to_x = side == 0 ? in : side == 1 ? last - in : k;
		int32_t to_z = side == 2 ? in : side == 3 ? last - in : k;

		to_found = node_sub_height(navigation, to_node, to_cell, to_x, to_z, room_needed, &to_height, &stopped);
	}
	return from_found && to_found ? SDL_fabsf(to_height - from_height) : 1e30f;
}

// The least of those steps along the whole edge (a stair's first tread,
// not its cell's highest, a meter in); 1e30 where they do not touch
static float node_edge_rise(const navigation_t *navigation, int32_t from_node, int32_t from_cell, int32_t to_node, int32_t to_cell, int side,
	uint8_t room_needed)
{
	float least = 1e30f;

	for (int32_t k = 0; k < NAVIGATION_SUBCELLS; k++)
	{
		least = SDL_min(least, node_edge_line_rise(navigation, from_node, from_cell, to_node, to_cell, side, k, room_needed));
	}
	return least;
}

// Where along an edge cell the two nodes meet within a step (rise, at
// least): the first and last quarter lines that do, so a portal ends at
// a balustrade's opening, not at its cell's corner
static void node_edge_span(const navigation_t *navigation, int32_t from_node, int32_t from_cell, int32_t to_node, int32_t to_cell, int side,
	float rise, uint8_t room_needed, int32_t *first, int32_t *last)
{
	*first = NAVIGATION_SUBCELLS;
	*last = -1;
	for (int32_t k = 0; k < NAVIGATION_SUBCELLS; k++)
	{
		if (node_edge_line_rise(navigation, from_node, from_cell, to_node, to_cell, side, k, room_needed) <= rise + 0.01f)
		{
			*first = SDL_min(*first, k);
			*last = k;
		}
	}
	if (*last < 0)
	{
		*first = 0;
		*last = NAVIGATION_SUBCELLS - 1;
	}
}

// The portals out of the rectangle id over one of its sides (0: +x, 1: -x,
// 2: +z, 3: -z) into the cells beyond, appended to a list: along the side,
// a run of cells whose nearest surface beyond (within
// MESH_PORTAL_RISE_MOST) is in the same rectangle, with steps of the same
// level, is one portal. Only into tiles that are built
static bool mesh_side_portals(navigation_t *navigation, int c, int32_t id, int side, navigation_portal_t **list, int32_t *count,
	int32_t *capacity)
{
	const navigation_rect_t *rect = mesh_rect(navigation, c, id);
	int32_t along = side < 2 ? rect->depth : rect->width;
	int32_t run_to = -1;
	int run_level = 0;
	bool run_cross = false;
	float run_rise = 0.0f;
	int32_t run_start = 0;
	float reach = SDL_min(navigation->mesh.classes[c].reach, 50.0f);
	// (onto an object's top a step only: a stair's treads, not the
	// terrace's face beside them, a slope's reach up)
	float step = SDL_min(navigation->mesh.classes[c].step, 50.0f);
	uint8_t room_needed = mesh_room_needed(&navigation->mesh.classes[c]);
	int32_t width_quarters = navigation->mesh.classes[c].width_quarters;
	// (each cell's two nodes across the side, and their cells, for where
	// a portal's ends meet at quarter meters)
	int32_t inside_nodes[NAVIGATION_MESH_RECT_MOST + 1];
	int32_t beyond_nodes[NAVIGATION_MESH_RECT_MOST + 1];
	int32_t inside_cells[NAVIGATION_MESH_RECT_MOST + 1];
	int32_t beyond_cells[NAVIGATION_MESH_RECT_MOST + 1];

	for (int32_t k = 0; k <= along; k++)
	{
		int32_t to = -1;
		int level = 0;
		bool cross = false;
		float rise = 0.0f;

		if (k < along)
		{
			int32_t inside_x = side == 0 ? rect->x + rect->width - 1 : side == 1 ? rect->x : rect->x + k;
			int32_t inside_z = side == 2 ? rect->z + rect->depth - 1 : side == 3 ? rect->z : rect->z + k;
			int32_t beyond_x = inside_x + (side == 0 ? 1 : side == 1 ? -1 : 0);
			int32_t beyond_z = inside_z + (side == 2 ? 1 : side == 3 ? -1 : 0);

			if (beyond_x >= 0 && beyond_z >= 0 && beyond_x < navigation->size && beyond_z < navigation->size)
			{
				int32_t inside_cell = inside_z * navigation->size + inside_x;
				int32_t beyond_cell = beyond_z * navigation->size + beyond_x;
				int32_t inside_node = mesh_node_in(navigation, c, id, inside_cell);
				int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
				int node_count_beyond = cell_nodes(navigation, beyond_cell, nodes);
				float best = MESH_PORTAL_RISE_MOST;

				if (inside_node >= 0)
				{
					float height = node_height(navigation, inside_node, inside_cell);

					(void)height;
					for (int n = 0; n < node_count_beyond; n++)
					{
						int32_t other = mesh_rect_of_node(navigation, c, nodes[n], beyond_cell);
						// (where their edges meet: a stair's first tread)
						float difference = node_edge_rise(navigation, inside_node, inside_cell, nodes[n], beyond_cell, side, room_needed);

						if (other >= 0 && other != id && difference <= best)
						{
							best = difference;
							to = other;
							rise = difference;
							cross = (nodes[n] >= cell_count(navigation)) != (inside_node >= cell_count(navigation));
							inside_nodes[k] = inside_node;
							beyond_nodes[k] = nodes[n];
							inside_cells[k] = inside_cell;
							beyond_cells[k] = beyond_cell;
						}
					}
					level = mesh_rise_level(rise);
				}
			}
		}
		// The run so far ends here: its portal
		if (run_to >= 0 && (k == along || to != run_to || level != run_level || cross != run_cross))
		{
			navigation_portal_t *portal;
			float edge;
			float start = (float)((side < 2 ? rect->z : rect->x) + run_start) * NAVIGATION_CELL_METERS;
			float end = (float)((side < 2 ? rect->z : rect->x) + k) * NAVIGATION_CELL_METERS;
			int32_t first_line;
			int32_t last_line;

			// (its ends where the quarter meters meet, at the first and last
			// cells: a balustrade's opening narrower than its cells)
			node_edge_span(navigation, inside_nodes[run_start], inside_cells[run_start], beyond_nodes[run_start], beyond_cells[run_start], side,
				inside_nodes[run_start] >= cell_count(navigation) || beyond_nodes[run_start] >= cell_count(navigation) ? step : reach, room_needed, &first_line, &last_line);
			start += (float)first_line * (NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS);
			node_edge_span(navigation, inside_nodes[k - 1], inside_cells[k - 1], beyond_nodes[k - 1], beyond_cells[k - 1], side,
				inside_nodes[k - 1] >= cell_count(navigation) || beyond_nodes[k - 1] >= cell_count(navigation) ? step : reach, room_needed, &first_line, &last_line);
			end -= (float)(NAVIGATION_SUBCELLS - 1 - last_line) * (NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS);
			// (an opening narrower than the class: no way through)
			if (end - start >= (float)width_quarters * (NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS) - 0.01f)
			{
				if (!mesh_grow((void **)list, capacity, *count + 1, sizeof(navigation_portal_t)))
				{
					return false;
				}
				portal = &(*list)[(*count)++];
				portal->from = id & 0xFFFF;
				portal->to = run_to;
				portal->rise = run_rise;
				portal->cross_surface = run_cross;
				edge = side == 0 ? (float)(rect->x + rect->width) : side == 1 ? (float)rect->x : side == 2 ? (float)(rect->z + rect->depth)
					: (float)rect->z;
				edge *= NAVIGATION_CELL_METERS;
				if (side < 2)
				{
					portal->a[0] = edge;
					portal->a[1] = start;
					portal->b[0] = edge;
					portal->b[1] = end;
				}
				else
				{
					portal->a[0] = start;
					portal->a[1] = edge;
					portal->b[0] = end;
					portal->b[1] = edge;
				}
			}
			run_to = -1;
		}
		if (to >= 0 && run_to < 0)
		{
			run_to = to;
			run_level = level;
			run_cross = cross;
			run_rise = rise;
			run_start = k;
		}
		else if (to >= 0)
		{
			run_rise = SDL_max(run_rise, rise);
		}
	}
	return true;
}

static void region_neighbor(const navigation_t *navigation, int32_t region, int side, int32_t *neighbor_out)
{
	int32_t region_x = region % navigation->regions_per_side + (side == 0 ? 1 : side == 1 ? -1 : 0);
	int32_t region_z = region / navigation->regions_per_side + (side == 2 ? 1 : side == 3 ? -1 : 0);

	*neighbor_out = region_x >= 0 && region_z >= 0 && region_x < navigation->regions_per_side && region_z < navigation->regions_per_side
		? region_z * navigation->regions_per_side + region_x : -1;
}

// Does a rectangle touch its tile's side?
static bool rect_on_side(const navigation_t *navigation, const navigation_rect_t *rect, int32_t region, int side)
{
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;

	region_bounds(navigation, region, &first_x, &first_z, &last_x, &last_z);
	return side == 0 ? rect->x + rect->width - 1 == last_x : side == 1 ? rect->x == first_x : side == 2 ? rect->z + rect->depth - 1 == last_z
		: rect->z == first_z;
}

// The portals between a tile and the next one over a side, both ways, made
// again (either built anew: the other's portals into it are stale)
static void mesh_link_border(navigation_t *navigation, int c, int32_t region, int side)
{
	navigation_mesh_tile_t *tiles = navigation->mesh.classes[c].tiles;
	int32_t neighbor;
	int opposite = side ^ 1;

	region_neighbor(navigation, region, side, &neighbor);
	tiles[region].border_count[side] = 0;
	if (neighbor < 0)
	{
		return;
	}
	tiles[neighbor].border_count[opposite] = 0;
	if (!tiles[region].built || !tiles[neighbor].built)
	{
		return;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		int32_t from_region = pass == 0 ? region : neighbor;
		int from_side = pass == 0 ? side : opposite;
		navigation_mesh_tile_t *tile = &tiles[from_region];

		for (int32_t r = 0; r < tile->rect_count; r++)
		{
			if (rect_on_side(navigation, &tile->rects[r], from_region, from_side))
			{
				mesh_side_portals(navigation, c, (from_region << 16) | r, from_side, &tile->border[from_side], &tile->border_count[from_side],
					&tile->border_capacity[from_side]);
			}
		}
		// (only the portals into the other tile: the side's cells all lie
		// in it)
	}
}

// The water's cells of a region, in a number (0: none, or dry tiles all
// round): a change builds its tiles again
static uint64_t mesh_water_signature(const navigation_t *navigation, const water_t *water, int32_t region)
{
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;
	uint64_t hash = 1469598103934665603ull;
	bool any_tile = false;

	if (water == NULL || water->iced)
	{
		return 0;
	}
	region_bounds(navigation, region, &first_x, &first_z, &last_x, &last_z);
	{
		float tile_meters = (float)WATER_TILE_SIZE * water->cell_size_meters;
		int32_t tile_first_x = SDL_clamp((int32_t)((float)first_x * NAVIGATION_CELL_METERS / tile_meters), 0, water->tiles_per_side - 1);
		int32_t tile_first_z = SDL_clamp((int32_t)((float)first_z * NAVIGATION_CELL_METERS / tile_meters), 0, water->tiles_per_side - 1);
		int32_t tile_last_x = SDL_clamp((int32_t)((float)(last_x + 1) * NAVIGATION_CELL_METERS / tile_meters), 0, water->tiles_per_side - 1);
		int32_t tile_last_z = SDL_clamp((int32_t)((float)(last_z + 1) * NAVIGATION_CELL_METERS / tile_meters), 0, water->tiles_per_side - 1);

		for (int32_t tile_z = tile_first_z; tile_z <= tile_last_z && !any_tile; tile_z++)
		{
			for (int32_t tile_x = tile_first_x; tile_x <= tile_last_x; tile_x++)
			{
				int32_t tile = tile_z * water->tiles_per_side + tile_x;

				if (water->tile_state[tile] != WATER_TILE_DRY || water->tile_max_depth[tile] > 0)
				{
					any_tile = true;
					break;
				}
			}
		}
	}
	if (!any_tile)
	{
		return 0;
	}
	for (int32_t cell_z = first_z; cell_z <= last_z; cell_z++)
	{
		for (int32_t cell_x = first_x; cell_x <= last_x; cell_x++)
		{
			bool wet = !water_dry_at(navigation, water, cell_x, cell_z, ((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS,
				((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS);

			hash = (hash ^ (uint64_t)(wet ? 1 : 0)) * 1099511628211ull;
		}
	}
	return hash | 1u;
}

// Has a cell whose ground is taken an object's top with room for the
// class within its reach of a height (a terrace's step, a stair: not in
// the way, a way up; a wall under its roof is in the way)?
static bool mesh_cell_steppable(const navigation_t *navigation, const navigation_mesh_class_t *class, int32_t cell, float height)
{
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count = cell_nodes(navigation, cell, nodes);

	for (int k = 1; k < count; k++)
	{
		const navigation_surface_t *surface = extra_surface(navigation, nodes[k]);

		if ((surface->clearance == NAVIGATION_CLEARANCE_OPEN || (float)surface->clearance * NAVIGATION_CLEARANCE_UNIT_METERS >= SDL_min(class->height,
			50.0f)) && SDL_fabsf(surface->height - height) <= SDL_min(class->reach, 50.0f))
		{
			return true;
		}
	}
	return false;
}

// A tile of a class built anew from its region's cells (the region up to
// date): its rectangles, the portals among them, then the ones to the
// tiles round it
static bool mesh_build_tile(navigation_t *navigation, int c, int32_t region, const water_t *water)
{
	static mesh_build_t build;
	navigation_mesh_class_t *class = &navigation->mesh.classes[c];
	navigation_mesh_tile_t *tile = &class->tiles[region];
	int32_t last_x;
	int32_t last_z;
	int32_t cells = cell_count(navigation);
	uint64_t start_ticks = SDL_GetPerformanceCounter();
	float reach = SDL_min(class->reach, 50.0f);

	region_bounds(navigation, region, &build.first_x, &build.first_z, &last_x, &last_z);
	build.width = last_x - build.first_x + 1;
	build.depth = last_z - build.first_z + 1;
	build.region = region;
	tile->built = false;
	tile->rect_count = 0;
	tile->portal_count = 0;
	if (tile->ground_rect == NULL)
	{
		tile->ground_rect = SDL_malloc(sizeof(int16_t) * (size_t)(navigation->region_size * navigation->region_size));
		tile->extra_rect = SDL_malloc(sizeof(int16_t) * NAVIGATION_REGION_EXTRA_SLOTS);
		if (tile->ground_rect == NULL || tile->extra_rect == NULL)
		{
			return false;
		}
	}
	if (build.width * build.depth > MESH_TILE_CELLS_MOST)
	{
		return false;
	}
	SDL_memset(tile->ground_rect, 0xFF, sizeof(int16_t) * (size_t)(navigation->region_size * navigation->region_size));
	SDL_memset(tile->extra_rect, 0xFF, sizeof(int16_t) * NAVIGATION_REGION_EXTRA_SLOTS);
	// The water on the cells now, and where the class keeps off (round
	// what takes the room its lowest needs)
	for (int32_t local_z = 0; local_z < build.depth; local_z++)
	{
		for (int32_t local_x = 0; local_x < build.width; local_x++)
		{
			int32_t cell_x = build.first_x + local_x;
			int32_t cell_z = build.first_z + local_z;
			int32_t local = local_z * build.width + local_x;
			bool eroded = false;

			build.wet[local] = water != NULL && !water->iced && !water_dry_at(navigation, water, cell_x, cell_z,
				((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS, ((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS);
			for (int32_t dz = -class->erosion_cells; dz <= class->erosion_cells && !eroded; dz++)
			{
				for (int32_t dx = -class->erosion_cells; dx <= class->erosion_cells; dx++)
				{
					int32_t near_x = cell_x + dx;
					int32_t near_z = cell_z + dz;
					uint8_t clearance;

					if ((dx == 0 && dz == 0) || near_x < 0 || near_z < 0 || near_x >= navigation->size || near_z >= navigation->size)
					{
						continue;
					}
					clearance = navigation->terrain_clearance[near_z * navigation->size + near_x];
					if (clearance != NAVIGATION_CLEARANCE_OPEN && (float)clearance * NAVIGATION_CLEARANCE_UNIT_METERS < SDL_min(class->height, 50.0f)
						&& !mesh_cell_steppable(navigation, class, near_z * navigation->size + near_x, navigation->ground_height[cell_z * navigation->size
							+ cell_x]))
					{
						eroded = true;
						break;
					}
				}
			}
			build.eroded[local] = eroded;
		}
	}
	// The rectangles: from each usable node not yet in one, as wide as its
	// row goes on alike, then as deep as whole rows go on alike
	for (int32_t local_z = 0; local_z < build.depth; local_z++)
	{
		for (int32_t local_x = 0; local_x < build.width; local_x++)
		{
			int32_t cell = (build.first_z + local_z) * navigation->size + build.first_x + local_x;
			int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
			int count = cell_nodes(navigation, cell, nodes);

			for (int k = 0; k < count; k++)
			{
				static int32_t grid_nodes[NAVIGATION_MESH_RECT_MOST][NAVIGATION_MESH_RECT_MOST];
				mesh_node_t seed;
				int32_t width = 1;
				int32_t depth = 1;
				navigation_rect_t *rect;
				int32_t index;
				float cost_sum = 0.0f;

				if (mesh_rect_of_node_local(navigation, tile, &build, nodes[k], cell) >= 0)
				{
					continue;
				}
				mesh_node_props(navigation, class, nodes[k], cell, build.wet[local_z * build.width + local_x],
					build.eroded[local_z * build.width + local_x], &seed);
				if (!seed.usable || tile->rect_count >= 0x7FFF)
				{
					continue;
				}
				grid_nodes[0][0] = nodes[k];
				if (!seed.wet)
				{
					// Along x
					while (width < NAVIGATION_MESH_RECT_MOST && local_x + width < build.width)
					{
						int32_t next = mesh_match(navigation, class, tile, &build, grid_nodes[0][width - 1], cell + width - 1, cell + width, 0, reach);

						if (next < 0)
						{
							break;
						}
						grid_nodes[0][width++] = next;
					}
					// Down z, whole rows
					while (depth < NAVIGATION_MESH_RECT_MOST && local_z + depth < build.depth)
					{
						bool whole = true;

						for (int32_t i = 0; i < width && whole; i++)
						{
							int32_t above_cell = cell + (depth - 1) * navigation->size + i;
							int32_t next = mesh_match(navigation, class, tile, &build, grid_nodes[depth - 1][i], above_cell, above_cell + navigation->size,
								2, reach);

							// (and touching its neighbor along the row)
							if (next >= 0 && i > 0 && node_edge_rise(navigation, grid_nodes[depth][i - 1], above_cell + navigation->size - 1, next,
								above_cell + navigation->size, 0, mesh_room_needed(class)) > reach)
							{
								next = -1;
							}
							whole = next >= 0;
							grid_nodes[depth][i] = next;
						}
						if (!whole)
						{
							break;
						}
						depth++;
					}
				}
				if (!mesh_grow((void **)&tile->rects, &tile->rect_capacity, tile->rect_count + 1, sizeof(navigation_rect_t)))
				{
					return false;
				}
				index = tile->rect_count++;
				rect = &tile->rects[index];
				SDL_zerop(rect);
				rect->x = build.first_x + local_x;
				rect->z = build.first_z + local_z;
				rect->width = (uint8_t)width;
				rect->depth = (uint8_t)depth;
				rect->kind = seed.kind;
				rect->blocked = seed.blocked;
				rect->small = seed.small;
				rect->clearance = seed.clearance;
				rect->height_low = 1e30f;
				rect->height_high = -1e30f;
				rect->wet_node = seed.wet ? nodes[k] : -1;
				rect->came_from = -1;
				for (int32_t j = 0; j < depth; j++)
				{
					for (int32_t i = 0; i < width; i++)
					{
						int32_t node = grid_nodes[j][i];
						int32_t node_cell_index = cell + j * navigation->size + i;
						mesh_node_t props;

						mesh_node_props(navigation, class, node, node_cell_index, false, false, &props);
						cost_sum += props.cost;
						rect->clearance = SDL_min(props.clearance, rect->clearance);
						rect->height_low = SDL_min(rect->height_low, props.height);
						rect->height_high = SDL_max(rect->height_high, props.height);
						if (node < cells)
						{
							tile->ground_rect[(local_z + j) * navigation->region_size + local_x + i] = (int16_t)index;
						}
						else
						{
							tile->extra_rect[(node - cells) % NAVIGATION_REGION_EXTRA_SLOTS] = (int16_t)index;
						}
					}
				}
				rect->cost = cost_sum / (float)(width * depth);
			}
		}
	}
	tile->built = true;
	tile->generation = navigation->region_generation[region];
	tile->class_epoch = class->epoch;
	tile->water_signature = mesh_water_signature(navigation, water, region);
	tile->water_checked_ticks = SDL_GetTicks();
	// The portals within the tile (into rectangles of this tile only: the
	// sides' others are the border's), each rectangle's together
	for (int32_t r = 0; r < tile->rect_count; r++)
	{
		int32_t first = tile->portal_count;

		for (int side = 0; side < 4; side++)
		{
			int32_t before = tile->portal_count;

			if (!mesh_side_portals(navigation, c, (region << 16) | r, side, &tile->portals, &tile->portal_count, &tile->portal_capacity))
			{
				return false;
			}
			// (those into another tile are the border's)
			for (int32_t p = before; p < tile->portal_count;)
			{
				if ((tile->portals[p].to >> 16) != region)
				{
					tile->portals[p] = tile->portals[--tile->portal_count];
				}
				else
				{
					p++;
				}
			}
		}
		tile->rects[r].first_portal = first;
		tile->rects[r].portal_count = tile->portal_count - first;
	}
	for (int side = 0; side < 4; side++)
	{
		mesh_link_border(navigation, c, region, side);
	}
	navigation->mesh.tiles_built++;
	tile->build_serial = (uint32_t)navigation->mesh.tiles_built;
	navigation->mesh.last_build_milliseconds = (float)((double)(SDL_GetPerformanceCounter() - start_ticks) * 1000.0
		/ (double)SDL_GetPerformanceFrequency());
	return true;
}

// The tile of a class over a region, current (its region built first if
// stale; built again if the region, the class or its water changed): false
// if it could not be
static bool mesh_tile_ready(navigation_t *navigation, int c, int32_t region, const water_t *water)
{
	navigation_mesh_class_t *class = &navigation->mesh.classes[c];
	navigation_mesh_tile_t *tile;
	bool needed;

	if (class->tiles == NULL || region < 0)
	{
		return false;
	}
	tile = &class->tiles[region];
	if (tile->built && tile->checked_search == navigation->mesh.search_number)
	{
		return true;
	}
	prepare_regions(navigation, region % navigation->regions_per_side, region / navigation->regions_per_side,
		region % navigation->regions_per_side, region / navigation->regions_per_side);
	needed = !tile->built || tile->generation != navigation->region_generation[region] || tile->class_epoch != class->epoch;
	if (!needed && SDL_GetTicks() - tile->water_checked_ticks >= MESH_WATER_CHECK_MS)
	{
		tile->water_checked_ticks = SDL_GetTicks();
		needed = mesh_water_signature(navigation, water, region) != tile->water_signature;
	}
	if (needed && !mesh_build_tile(navigation, c, region, water))
	{
		tile->built = false;
		return false;
	}
	tile->checked_search = navigation->mesh.search_number;
	return true;
}

// What a rectangle is to an agent now: as node_info's (a wet one's its
// cell's, the water as it is)
static void mesh_rect_info(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	const navigation_rect_t *rect, node_info_t *info)
{
	if (rect->wet_node >= 0)
	{
		node_info(navigation, terrain, water, agent, rect->wet_node, info);
		return;
	}
	info->passable = roomy(rect->clearance, agent) && (rect->blocked & agent->groups) == 0
		&& (float)rect->small * SMALL_UNIT_METERS < agent->height_meters;
	info->water_class = CLASS_DRY;
	info->cost_factor = rect->cost;
	info->height = rect->height_low;
	info->small = (float)rect->small * SMALL_UNIT_METERS > agent->step_meters;
	info->surface = rect->kind == 1;
	info->extra_cost = info->small ? navigation->small_detour_meters : 0.0f;
}

// Can the agent go through a portal from one rectangle (as it is to it)
// to the next?
static bool mesh_portal_allowed(const navigation_agent_t *agent, const navigation_portal_t *portal, const node_info_t *from,
	const node_info_t *to)
{
	if (!to->passable)
	{
		return false;
	}
	if (from->water_class == CLASS_SWIM && to->water_class == CLASS_SWIM)
	{
		return true;
	}
	return portal->rise <= (portal->cross_surface ? agent->step_meters : agent_reach(agent));
}

// The rectangle of a class a point is in, on the surface nearest the
// height that it can be on (tiles readied); -1 if none. near: if none
// there, the nearest within MESH_START_NEAR_CELLS
static int32_t mesh_locate(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float x, float z, float height, bool near)
{
	int c = mesh_class_of(agent);
	int32_t cell;
	int32_t best = -1;
	float best_score = 1e30f;
	int32_t reach_cells = near ? MESH_START_NEAR_CELLS : 0;

	cell_of_point(navigation, x, z, &cell);
	for (int32_t dz = -reach_cells; dz <= reach_cells; dz++)
	{
		for (int32_t dx = -reach_cells; dx <= reach_cells; dx++)
		{
			int32_t cell_x = cell % navigation->size + dx;
			int32_t cell_z = cell / navigation->size + dz;
			int32_t here;
			int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
			int count;

			if (cell_x < 0 || cell_z < 0 || cell_x >= navigation->size || cell_z >= navigation->size)
			{
				continue;
			}
			here = cell_z * navigation->size + cell_x;
			if (!mesh_tile_ready(navigation, c, region_of_cell(navigation, here), water))
			{
				continue;
			}
			count = cell_nodes(navigation, here, nodes);
			for (int k = 0; k < count; k++)
			{
				int32_t id = mesh_rect_of_node(navigation, c, nodes[k], here);
				node_info_t info;
				float score;

				if (id < 0)
				{
					continue;
				}
				mesh_rect_info(navigation, terrain, water, agent, mesh_rect(navigation, c, id), &info);
				if (!info.passable)
				{
					continue;
				}
				// (out of its reach in height last: a deck a meter up in its own
				// cell is not where it stands; then away in cells, then in
				// height)
				{
					float difference = SDL_fabsf(node_height(navigation, nodes[k], here) - height);

					// (an object's top that does not reach the point's quarter:
					// the floor beyond the wall in the same cell, not here)
					float elsewhere = nodes[k] >= cell_count(navigation) && dx == 0 && dz == 0
						&& surface_height_at(navigation, extra_surface(navigation, nodes[k]), x, z) < -1e29f ? 50.0f : 0.0f;

					score = (difference > agent_reach(agent) ? 10000.0f : 0.0f) + (float)(dx * dx + dz * dz) * 100.0f + difference + elsewhere;
				}
				if (score < best_score)
				{
					best_score = score;
					best = id;
				}
			}
		}
	}
	return best;
}

static bool rect_has_cell(const navigation_rect_t *rect, int32_t cell_x, int32_t cell_z)
{
	return cell_x >= rect->x && cell_z >= rect->z && cell_x < rect->x + rect->width && cell_z < rect->z + rect->depth;
}

// The point of a rectangle nearest a point
static void rect_nearest(const navigation_rect_t *rect, float x, float z, float out[2])
{
	out[0] = SDL_clamp(x, (float)rect->x * NAVIGATION_CELL_METERS + 0.05f, (float)(rect->x + rect->width) * NAVIGATION_CELL_METERS - 0.05f);
	out[1] = SDL_clamp(z, (float)rect->z * NAVIGATION_CELL_METERS + 0.05f, (float)(rect->z + rect->depth) * NAVIGATION_CELL_METERS - 0.05f);
}

static float distance_2d(const float a[2], const float b[2])
{
	return SDL_sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]));
}

// The point of a segment nearest a point
static void segment_nearest(const float a[2], const float b[2], const float point[2], float out[2])
{
	float dx = b[0] - a[0];
	float dz = b[1] - a[1];
	float length_squared = dx * dx + dz * dz;
	float t = length_squared > 1e-8f ? ((point[0] - a[0]) * dx + (point[1] - a[1]) * dz) / length_squared : 0.0f;

	t = SDL_clamp(t, 0.0f, 1.0f);
	out[0] = a[0] + dx * t;
	out[1] = a[1] + dz * t;
}

// Twice the signed area of the triangle a, b, c (the funnel's sides test)
static float triangle_area_2(const float a[2], const float b[2], const float c[2])
{
	float ax = b[0] - a[0];
	float az = b[1] - a[1];
	float bx = c[0] - a[0];
	float bz = c[1] - a[1];

	return bx * az - ax * bz;
}

static bool same_point(const float a[2], const float b[2])
{
	return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) < 1e-6f;
}

// The corridor's portals pulled tight (the simple stupid funnel): each
// portal its left and right end, the first and last the start and the
// end; the corners out (the start left out), at most most of them. The
// count, or -1 if there were more
static int mesh_funnel(const float (*lefts)[2], const float (*rights)[2], int portal_count, float (*points)[2], int most)
{
	float apex[2] = { lefts[0][0], lefts[0][1] };
	float left[2] = { lefts[0][0], lefts[0][1] };
	float right[2] = { rights[0][0], rights[0][1] };
	int apex_index = 0;
	int left_index = 0;
	int right_index = 0;
	int count = 0;

	for (int i = 1; i < portal_count; i++)
	{
		const float *portal_left = lefts[i];
		const float *portal_right = rights[i];

		// The right side narrowed
		if (triangle_area_2(apex, right, portal_right) <= 0.0f)
		{
			if (same_point(apex, right) || triangle_area_2(apex, left, portal_right) > 0.0f)
			{
				right[0] = portal_right[0];
				right[1] = portal_right[1];
				right_index = i;
			}
			else
			{
				// Past the left: the left a corner, the funnel again from it
				if (count == most)
				{
					return -1;
				}
				points[count][0] = left[0];
				points[count][1] = left[1];
				count++;
				apex[0] = left[0];
				apex[1] = left[1];
				apex_index = left_index;
				right[0] = apex[0];
				right[1] = apex[1];
				right_index = apex_index;
				i = apex_index;
				continue;
			}
		}
		// The left side narrowed
		if (triangle_area_2(apex, left, portal_left) >= 0.0f)
		{
			if (same_point(apex, left) || triangle_area_2(apex, right, portal_left) < 0.0f)
			{
				left[0] = portal_left[0];
				left[1] = portal_left[1];
				left_index = i;
			}
			else
			{
				if (count == most)
				{
					return -1;
				}
				points[count][0] = right[0];
				points[count][1] = right[1];
				count++;
				apex[0] = right[0];
				apex[1] = right[1];
				apex_index = right_index;
				left[0] = apex[0];
				left[1] = apex[1];
				left_index = apex_index;
				i = apex_index;
				continue;
			}
		}
	}
	// The end
	if (count == 0 || !same_point(points[count - 1], lefts[portal_count - 1]))
	{
		if (count == most)
		{
			return -1;
		}
		points[count][0] = lefts[portal_count - 1][0];
		points[count][1] = lefts[portal_count - 1][1];
		count++;
	}
	return count;
}

// The search over the rectangles: as navigation_find_path; -1 (not a
// result) when the mesh has nothing at the start (the cells' way then)
// A way found holds over the quarter meters as the walker will take it
// (mesh_way_holds): its body's straight way this far at a time (the
// follower's BODY_WAY_METERS), a fine way this far where it has none
// (FINE_REACH_METERS; its end this near a waypoint: the waypoint's own),
// with this many brushes (BODY_BRUSHES)
#define MESH_VERIFY_AHEAD_METERS 5.0f
#define MESH_VERIFY_FINE_METERS 3.0f
#define MESH_VERIFY_BY_METERS 0.55f
#define MESH_VERIFY_BRUSHES 2
#define MESH_VERIFY_STEPS_MOST 96      // so many stretches of it looked at; a longer way: the rest as the walker finds it
#define MESH_VERIFY_OWN_METERS 0.5f    // not holding this near its start: where the walker stands is what is wrong, not the way
#define MESH_VERIFY_TRIES 4            // a way that does not hold: a portal shut and searched again, this often
#define MESH_VERIFY_END_METERS 1.0f    // not holding this near the goal: left as it is
#define MESH_SHUT_MILLISECONDS 30000u  // a portal shut this long

// The last search's rectangles, from the end back to the start
static int32_t mesh_chain[65536];
static int32_t mesh_chain_length;

// Whether a portal between two rectangles is shut to the agent
// (navigation_mesh_shut_t)
static bool mesh_is_shut(const navigation_t *navigation, int c, const navigation_agent_t *agent, int32_t from, int32_t to)
{
	uint64_t now = SDL_GetTicks();

	for (int32_t k = 0; k < navigation->mesh.shut_count; k++)
	{
		const navigation_mesh_shut_t *shut = &navigation->mesh.shut[k];

		if (shut->from == from && shut->to == to && shut->class == (uint8_t)c && now < shut->until_ticks
			&& shut->from_serial == navigation->mesh.classes[c].tiles[from >> 16].build_serial
			&& shut->to_serial == navigation->mesh.classes[c].tiles[to >> 16].build_serial
			&& SDL_fabsf(shut->step - agent->step_meters) < 1e-3f && SDL_fabsf(shut->height - agent->height_meters) < 1e-3f
			&& SDL_fabsf(shut->radius - agent->radius_meters) < 1e-3f)
		{
			return true;
		}
	}
	return false;
}

static void mesh_shut(navigation_t *navigation, int c, const navigation_agent_t *agent, int32_t from, int32_t to)
{
	navigation_mesh_shut_t *shut = &navigation->mesh.shut[navigation->mesh.shut_next];

	shut->from = from;
	shut->to = to;
	shut->from_serial = navigation->mesh.classes[c].tiles[from >> 16].build_serial;
	shut->to_serial = navigation->mesh.classes[c].tiles[to >> 16].build_serial;
	shut->class = (uint8_t)c;
	shut->step = agent->step_meters;
	shut->height = agent->height_meters;
	shut->radius = agent->radius_meters;
	shut->until_ticks = SDL_GetTicks() + MESH_SHUT_MILLISECONDS;
	navigation->mesh.shut_next = (navigation->mesh.shut_next + 1) % NAVIGATION_MESH_SHUT_MOST;
	navigation->mesh.shut_count = SDL_min(navigation->mesh.shut_count + 1, NAVIGATION_MESH_SHUT_MOST);
	navigation->mesh.ways_refused++;
}

// Whether a way the rectangles found holds over the quarter meters, gone
// as a walker goes it: straight to each waypoint where its body has the
// straight way (navigation_body_line), by a fine way where it has not
// (navigation_fine_path), on as far as the straight way goes where there
// is no fine way either; no further: it does not hold, and where
// (failed_out, at failed_height_out, on its way to the waypoint
// failed_leg_out). The rectangles take a cell whole
// that a square of the class fits in somewhere; whether each tread of the
// stair in it is within this agent's step, whether it gets under the
// beam across it, the quarters know
static bool mesh_way_holds(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float (*waypoints)[2], int count, float failed_out[2], float *failed_height_out,
	int *failed_leg_out)
{
	float x = from_x;
	float z = from_z;
	float height = from_height;
	int leg = 0;

	for (int step = 0; step < MESH_VERIFY_STEPS_MOST && leg < count; step++)
	{
		float dx = waypoints[leg][0] - x;
		float dz = waypoints[leg][1] - z;
		float distance = SDL_sqrtf(dx * dx + dz * dz);
		float share = distance > MESH_VERIFY_AHEAD_METERS ? MESH_VERIFY_AHEAD_METERS / distance : 1.0f;
		float fine_share = distance > MESH_VERIFY_FINE_METERS ? MESH_VERIFY_FINE_METERS / distance : 1.0f;
		float fine[12][2];
		int fine_count;
		float reached;
		float reached_height;
		float fine_height;

		if (distance < 1e-3f)
		{
			leg++;
			continue;
		}
		if (body_line_reach(navigation, terrain, water, agent, x, z, height, x + dx * share, z + dz * share, MESH_VERIFY_BRUSHES, &reached,
			&reached_height))
		{
			x += dx * share;
			z += dz * share;
			height = reached_height;
			leg += share >= 1.0f ? 1 : 0;
			continue;
		}
		fine_count = fine_path_search(navigation, terrain, water, agent, x, z, height, x + dx * fine_share, z + dz * fine_share, fine,
			(int)SDL_arraysize(fine), &fine_height);
		if (fine_count > 0)
		{
			float end_x = fine[fine_count - 1][0] - waypoints[leg][0];
			float end_z = fine[fine_count - 1][1] - waypoints[leg][1];

			x = fine[fine_count - 1][0];
			z = fine[fine_count - 1][1];
			height = fine_height;
			leg += end_x * end_x + end_z * end_z < MESH_VERIFY_BY_METERS * MESH_VERIFY_BY_METERS ? 1 : 0;
			continue;
		}
		// (no fine way from here: from as far as the straight way goes)
		if (reached * share * distance > 0.25f)
		{
			x += dx * share * reached;
			z += dz * share * reached;
			height = reached_height;
			continue;
		}
		failed_out[0] = x;
		failed_out[1] = z;
		*failed_height_out = height;
		*failed_leg_out = leg;
		return false;
	}
	return true;
}

static int mesh_search(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2],
	int *waypoint_count_out, bool *complete_out);

// The way over the rectangles, one that holds over the quarter meters
// too: found (mesh_search), then gone as the walker will go it
// (mesh_way_holds); where it does not hold, the portal out of the
// rectangle it got no further in is shut to agents like this one and the
// way searched again (another stair; or as near the goal as it gets, not
// up a stair it cannot climb to push at it). Nothing to shut (it does not
// hold in its last rectangle: the nearest the goal the rectangles have,
// its edge under a beam): the way as far as it holds
static int mesh_find_path(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2],
	int *waypoint_count_out, bool *complete_out)
{
	int c = mesh_class_of(agent);

	for (int attempt = 0;; attempt++)
	{
		int result = mesh_search(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, waypoints_out, waypoint_count_out,
			complete_out);
		float failed[2];
		float failed_height;
		int failed_leg;
		int32_t cell;
		int32_t at = -1;
		float at_off = 1e30f;

		if (result <= 0 || *waypoint_count_out <= 0 || mesh_way_holds(navigation, terrain, water, agent, from_x, from_z, from_height, waypoints_out,
			*waypoint_count_out, failed, &failed_height, &failed_leg))
		{
			return result;
		}
		if ((failed[0] - from_x) * (failed[0] - from_x) + (failed[1] - from_z) * (failed[1] - from_z) < MESH_VERIFY_OWN_METERS * MESH_VERIFY_OWN_METERS
			|| !cell_of_point(navigation, failed[0], failed[1], &cell))
		{
			return result;
		}
		// The way's rectangle it got no further in: the one over that
		// cell nearest its height there (a terrace over the ground: two of
		// them), the earlier of two as near
		for (int32_t k = 0; k < mesh_chain_length; k++)
		{
			const navigation_rect_t *rect = mesh_rect(navigation, c, mesh_chain[k]);
			float off;

			if (!rect_has_cell(rect, cell % navigation->size, cell / navigation->size))
			{
				continue;
			}
			off = SDL_fabsf(SDL_clamp(failed_height, rect->height_low, rect->height_high) - failed_height);
			if (off <= at_off)
			{
				at_off = off;
				at = k;
			}
		}
		if (at > 0 && attempt < MESH_VERIFY_TRIES)
		{
			mesh_shut(navigation, c, agent, mesh_chain[at], mesh_chain[at - 1]);
			continue;
		}
		// In the way's last rectangle (nothing to shut), or shut often
		// enough: the way as far as it holds, if that is not by the goal
		// itself (there the walker does what it can, as it did: a spot
		// beside a table)
		{
			const float *end = waypoints_out[*waypoint_count_out - 1];
			const float *before = failed_leg > 0 ? waypoints_out[failed_leg - 1] : NULL;

			if (result == NAVIGATION_PATH_FOUND
				&& (failed[0] - end[0]) * (failed[0] - end[0]) + (failed[1] - end[1]) * (failed[1] - end[1]) < MESH_VERIFY_END_METERS * MESH_VERIFY_END_METERS)
			{
				return result;
			}
			*waypoint_count_out = failed_leg;
			if (before == NULL || (failed[0] - before[0]) * (failed[0] - before[0]) + (failed[1] - before[1]) * (failed[1] - before[1]) > 0.3f * 0.3f)
			{
				waypoints_out[failed_leg][0] = failed[0];
				waypoints_out[failed_leg][1] = failed[1];
				*waypoint_count_out = failed_leg + 1;
			}
			*complete_out = true;
			navigation->mesh.ways_cut++;
			return NAVIGATION_PATH_PARTIAL;
		}
	}
}

static int mesh_search(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2],
	int *waypoint_count_out, bool *complete_out)
{
	int32_t *chain = mesh_chain;
	static float lefts[65536][2];
	static float rights[65536][2];
	static float corners[NAVIGATION_MAX_WAYPOINTS + 1][2];
	int c = mesh_class_of(agent);
	int32_t goal_cell;
	int32_t goal_x;
	int32_t goal_z;
	int32_t start;
	int32_t best;
	float best_distance;
	uint32_t reached_tag;
	bool found = false;
	int32_t chain_length = 0;
	float goal[2] = { to_x, to_z };

	mesh_chain_length = 0;
	float goal_distance = SDL_sqrtf((to_x - from_x) * (to_x - from_x) + (to_z - from_z) * (to_z - from_z));
	int32_t budget = SDL_max(MESH_MINIMUM_EXPANSIONS, (int32_t)SDL_min((float)navigation->maximum_expansions,
		(float)SEARCH_BASE_EXPANSIONS + (float)SEARCH_EXPANSIONS_PER_METER * goal_distance) / MESH_EXPANSIONS_DIVISOR);
	int32_t expansions = 0;
	uint64_t search_start = SDL_GetPerformanceCounter();

	mesh_note_agent(navigation, agent);
	navigation->mesh.search_number++;
	if (navigation->mesh.search_number >= 0x7FFFFFFFu)
	{
		navigation->mesh.search_number = 1;
	}
	reached_tag = navigation->mesh.search_number * 2;
	start = mesh_locate(navigation, terrain, water, agent, from_x, from_z, from_height, true);
	navigation->mesh.searches++;
	if (start < 0)
	{
		navigation->mesh.search_fallbacks++;
		return -1;
	}
	cell_of_point(navigation, to_x, to_z, &goal_cell);
	goal_x = goal_cell % navigation->size;
	goal_z = goal_cell / navigation->size;
	{
		navigation_rect_t *rect = mesh_rect(navigation, c, start);

		rect->tag = reached_tag;
		rect->search_cost = 0.0f;
		rect->came_from = -1;
		rect->entry[0] = from_x;
		rect->entry[1] = from_z;
		rect->came_by_door = false;
	}
	navigation->heap_count = 0;
	if (!heap_push(navigation, start, goal_distance))
	{
		return -1;
	}
	best = start;
	{
		float nearest[2];

		rect_nearest(mesh_rect(navigation, c, start), to_x, to_z, nearest);
		best_distance = distance_2d(nearest, goal);
	}
	while (navigation->heap_count > 0)
	{
		int32_t id = heap_pop(navigation);
		int32_t region = id >> 16;
		navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[region];
		navigation_rect_t *rect = &tile->rects[id & 0xFFFF];
		node_info_t here;

		if (rect->tag == reached_tag + 1)
		{
			continue;
		}
		rect->tag = reached_tag + 1;
		if (rect_has_cell(rect, goal_x, goal_z))
		{
			found = true;
			best = id;
			break;
		}
		{
			float nearest[2];
			float distance;

			rect_nearest(rect, to_x, to_z, nearest);
			distance = distance_2d(nearest, goal);
			if (distance < best_distance)
			{
				best_distance = distance;
				best = id;
			}
		}
		if (++expansions >= budget)
		{
			break;
		}
		mesh_rect_info(navigation, terrain, water, agent, rect, &here);
		// The tiles round it that it touches, current before their portals
		// are read (building one links its border both ways)
		for (int side = 0; side < 4; side++)
		{
			int32_t neighbor;

			if (rect_on_side(navigation, rect, region, side))
			{
				region_neighbor(navigation, region, side, &neighbor);
				if (neighbor >= 0)
				{
					mesh_tile_ready(navigation, c, neighbor, water);
					// (a build may have moved this tile's arrays: not, its
					// rectangles are its own; the border lists are read below)
				}
			}
		}
		for (int list = 0; list < 5; list++)
		{
			const navigation_portal_t *portals = list == 0 ? &tile->portals[rect->first_portal] : tile->border[list - 1];
			int32_t count = list == 0 ? rect->portal_count : tile->border_count[list - 1];

			if (list > 0 && !rect_on_side(navigation, rect, region, list - 1))
			{
				continue;
			}
			for (int32_t p = 0; p < count; p++)
			{
				const navigation_portal_t *portal = &portals[p];
				navigation_rect_t *next;
				node_info_t there;
				float crossing[2];
				float toward_entry[2];
				float toward_goal[2];
				float cost;

				if (list > 0 && portal->from != (id & 0xFFFF))
				{
					continue;
				}
				next = mesh_rect(navigation, c, portal->to);
				if (next->tag == reached_tag + 1)
				{
					continue;
				}
				mesh_rect_info(navigation, terrain, water, agent, next, &there);
				if (!mesh_portal_allowed(agent, portal, &here, &there)
					|| (navigation->mesh.shut_count > 0 && mesh_is_shut(navigation, c, agent, id, portal->to)))
				{
					continue;
				}
				// Where it crosses: between the portal's points nearest where
				// it came in and nearest the goal
				segment_nearest(portal->a, portal->b, rect->entry, toward_entry);
				segment_nearest(portal->a, portal->b, goal, toward_goal);
				crossing[0] = 0.5f * (toward_entry[0] + toward_goal[0]);
				crossing[1] = 0.5f * (toward_entry[1] + toward_goal[1]);
				cost = rect->search_cost + distance_2d(rect->entry, crossing) * here.cost_factor + there.extra_cost;
				if (next->tag != reached_tag || cost < next->search_cost)
				{
					next->tag = reached_tag;
					next->search_cost = cost;
					next->came_from = id;
					next->entry[0] = crossing[0];
					next->entry[1] = crossing[1];
					next->came_a[0] = portal->a[0];
					next->came_a[1] = portal->a[1];
					next->came_b[0] = portal->b[0];
					next->came_b[1] = portal->b[1];
					next->came_by_door = false;
					if (!heap_push(navigation, portal->to, cost + HEURISTIC_WEIGHT * distance_2d(crossing, goal)))
					{
						navigation->heap_count = 0;
						break;
					}
				}
			}
		}
		// Through a door from here (its ends on the ground), open or one it
		// opens, to the rectangle of its other end
		if (navigation->link_count > 0 && rect->kind == 0 && here.passable)
		{
			for (int k = 0; k < navigation->link_count; k++)
			{
				int end = -1;
				int32_t other_cell;
				int32_t other;
				navigation_rect_t *next;
				node_info_t there;
				float cost;
				float door_a[2];
				float door_b[2];

				for (int e = 0; e < 2; e++)
				{
					if (rect_has_cell(rect, navigation->link_cells[k][e] % navigation->size, navigation->link_cells[k][e] / navigation->size))
					{
						end = e;
					}
				}
				if (end < 0 || (!navigation->links[k].open && !agent->opens_doors))
				{
					continue;
				}
				other_cell = navigation->link_cells[k][1 - end];
				if (!mesh_tile_ready(navigation, c, region_of_cell(navigation, other_cell), water))
				{
					continue;
				}
				other = mesh_rect_of_node(navigation, c, other_cell, other_cell);
				if (other < 0 || other == id)
				{
					continue;
				}
				next = mesh_rect(navigation, c, other);
				if (next->tag == reached_tag + 1)
				{
					continue;
				}
				mesh_rect_info(navigation, terrain, water, agent, next, &there);
				if (!there.passable || SDL_fabsf(navigation->ground_height[other_cell] - navigation->ground_height[navigation->link_cells[k][end]])
					> SDL_max(agent->step_meters, 0.6f) || (navigation->mesh.shut_count > 0 && mesh_is_shut(navigation, c, agent, id, other)))
				{
					continue;
				}
				door_a[0] = end == 0 ? navigation->links[k].a[0] : navigation->links[k].b[0];
				door_a[1] = end == 0 ? navigation->links[k].a[1] : navigation->links[k].b[1];
				door_b[0] = end == 0 ? navigation->links[k].b[0] : navigation->links[k].a[0];
				door_b[1] = end == 0 ? navigation->links[k].b[1] : navigation->links[k].a[1];
				cost = rect->search_cost + distance_2d(rect->entry, door_a) * here.cost_factor + navigation->link_meters[k]
					+ (navigation->links[k].open ? 0.0f : LINK_SHUT_METERS);
				if (next->tag != reached_tag || cost < next->search_cost)
				{
					next->tag = reached_tag;
					next->search_cost = cost;
					next->came_from = id;
					next->entry[0] = door_b[0];
					next->entry[1] = door_b[1];
					next->came_a[0] = door_a[0];
					next->came_a[1] = door_a[1];
					next->came_b[0] = door_b[0];
					next->came_b[1] = door_b[1];
					next->came_by_door = true;
					if (!heap_push(navigation, other, cost + HEURISTIC_WEIGHT * distance_2d(door_b, goal)))
					{
						navigation->heap_count = 0;
						break;
					}
				}
			}
		}
	}
	navigation->mesh.last_search_ticks = SDL_GetPerformanceCounter() - search_start;
	navigation->last_search_ticks = navigation->mesh.last_search_ticks;
	navigation->last_expansions = expansions;
	// The rectangles from the end back to the start
	for (int32_t id = best; id >= 0 && chain_length < (int32_t)SDL_arraysize(mesh_chain); id = mesh_rect(navigation, c, id)->came_from)
	{
		chain[chain_length++] = id;
	}
	mesh_chain_length = chain_length;
	if (chain_length == 1 && !found)
	{
		// Nowhere nearer than its own rectangle: across it towards the goal,
		// if that is worth a step
		float end[2];
		float here[2] = { from_x, from_z };

		rect_nearest(mesh_rect(navigation, c, start), to_x, to_z, end);
		if (distance_2d(end, here) < 0.5f)
		{
			return NAVIGATION_PATH_NONE;
		}
		waypoints_out[0][0] = end[0];
		waypoints_out[0][1] = end[1];
		*waypoint_count_out = 1;
		return NAVIGATION_PATH_PARTIAL;
	}
	// The portals in order from the start, each its left end and right
	// (looking from the rectangle it leaves into the next), the agent's
	// radius off both ends; a door its two ends, one after the other
	{
		int portal_count = 0;
		float end[2];
		int corner_count;

		lefts[portal_count][0] = from_x;
		lefts[portal_count][1] = from_z;
		rights[portal_count][0] = from_x;
		rights[portal_count][1] = from_z;
		portal_count++;
		for (int32_t k = chain_length - 2; k >= 0 && portal_count < (int)SDL_arraysize(lefts) - 3; k--)
		{
			const navigation_rect_t *from_rect = mesh_rect(navigation, c, chain[k + 1]);
			const navigation_rect_t *to_rect = mesh_rect(navigation, c, chain[k]);

			if (to_rect->came_by_door)
			{
				for (int e = 0; e < 2; e++)
				{
					const float *point = e == 0 ? to_rect->came_a : to_rect->came_b;

					lefts[portal_count][0] = point[0];
					lefts[portal_count][1] = point[1];
					rights[portal_count][0] = point[0];
					rights[portal_count][1] = point[1];
					portal_count++;
				}
				continue;
			}
			{
				float from_middle[2] = { ((float)from_rect->x + 0.5f * (float)from_rect->width) * NAVIGATION_CELL_METERS,
					((float)from_rect->z + 0.5f * (float)from_rect->depth) * NAVIGATION_CELL_METERS };
				float portal_middle[2] = { 0.5f * (to_rect->came_a[0] + to_rect->came_b[0]), 0.5f * (to_rect->came_a[1] + to_rect->came_b[1]) };
				float direction[2] = { portal_middle[0] - from_middle[0], portal_middle[1] - from_middle[1] };
				float to_a[2] = { to_rect->came_a[0] - portal_middle[0], to_rect->came_a[1] - portal_middle[1] };
				// (a on the left: direction x (a - middle) above zero)
				bool a_left = direction[0] * to_a[1] - direction[1] * to_a[0] > 0.0f;
				float left[2];
				float right[2];
				float length = distance_2d(to_rect->came_a, to_rect->came_b);
				float inset = SDL_min(agent->radius_meters, 0.5f * length);

				left[0] = a_left ? to_rect->came_a[0] : to_rect->came_b[0];
				left[1] = a_left ? to_rect->came_a[1] : to_rect->came_b[1];
				right[0] = a_left ? to_rect->came_b[0] : to_rect->came_a[0];
				right[1] = a_left ? to_rect->came_b[1] : to_rect->came_a[1];
				if (length > 1e-4f)
				{
					float along[2] = { (right[0] - left[0]) / length, (right[1] - left[1]) / length };

					left[0] += along[0] * inset;
					left[1] += along[1] * inset;
					right[0] -= along[0] * inset;
					right[1] -= along[1] * inset;
				}
				lefts[portal_count][0] = left[0];
				lefts[portal_count][1] = left[1];
				rights[portal_count][0] = right[0];
				rights[portal_count][1] = right[1];
				portal_count++;
			}
		}
		if (found)
		{
			end[0] = to_x;
			end[1] = to_z;
		}
		else
		{
			rect_nearest(mesh_rect(navigation, c, best), to_x, to_z, end);
		}
		lefts[portal_count][0] = end[0];
		lefts[portal_count][1] = end[1];
		rights[portal_count][0] = end[0];
		rights[portal_count][1] = end[1];
		portal_count++;
		corner_count = mesh_funnel(lefts, rights, portal_count, corners, NAVIGATION_MAX_WAYPOINTS);
		if (corner_count < 0)
		{
			// More corners than a path holds: the first ones (written), searched
			// on from the last
			corner_count = NAVIGATION_MAX_WAYPOINTS;
			*complete_out = false;
		}
		// (corners a hair apart, where a corridor's portals meet at a
		// tile's edge: the later kept)
		{
			int kept = 0;

			for (int k = 0; k < corner_count; k++)
			{
				if (kept > 0 && distance_2d(waypoints_out[kept - 1], corners[k]) < 0.3f)
				{
					kept--;
				}
				waypoints_out[kept][0] = corners[k][0];
				waypoints_out[kept][1] = corners[k][1];
				kept++;
			}
			*waypoint_count_out = kept;
		}
	}
	return found ? NAVIGATION_PATH_FOUND : NAVIGATION_PATH_PARTIAL;
}

// The straight line over the rectangles: from one to the next where it
// leaves it, through the portal there (the agent's rules for the next one
// and the step); inside one nothing is looked at. With its width (a small
// agent's: the large class's rectangles keep away already) the two lines
// its radius to either side too. -1 (not an answer) when the mesh has
// nothing at the start
static int mesh_line(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent, float from_x,
	float from_z, float from_height, float to_x, float to_z, float *blocked_x, float *blocked_z, float *blocked_height)
{
	int c = mesh_class_of(agent);
	int32_t id;
	float direction[2] = { to_x - from_x, to_z - from_z };
	float travelled = 0.0f;

	mesh_note_agent(navigation, agent);
	navigation->mesh.search_number++;
	id = mesh_locate(navigation, terrain, water, agent, from_x, from_z, from_height, false);
	if (id < 0)
	{
		navigation->mesh.line_fallbacks++;
		return -1;
	}
	for (int step = 0; step < MESH_RAY_MOST_STEPS; step++)
	{
		const navigation_rect_t *rect = mesh_rect(navigation, c, id);
		float low_x = (float)rect->x * NAVIGATION_CELL_METERS;
		float low_z = (float)rect->z * NAVIGATION_CELL_METERS;
		float high_x = (float)(rect->x + rect->width) * NAVIGATION_CELL_METERS;
		float high_z = (float)(rect->z + rect->depth) * NAVIGATION_CELL_METERS;
		float leave = 1e30f;
		int side = -1;
		float exit[2];
		int32_t next = -1;
		node_info_t here;
		node_info_t there;
		int32_t region = id >> 16;
		const navigation_portal_t *through = NULL;

		if (to_x >= low_x && to_x <= high_x && to_z >= low_z && to_z <= high_z)
		{
			return 1;
		}
		// Where the line leaves it (from the part left of it)
		if (direction[0] > 1e-6f)
		{
			float t = (high_x - from_x) / direction[0];

			if (t < leave)
			{
				leave = t;
				side = 0;
			}
		}
		else if (direction[0] < -1e-6f)
		{
			float t = (low_x - from_x) / direction[0];

			if (t < leave)
			{
				leave = t;
				side = 1;
			}
		}
		if (direction[1] > 1e-6f)
		{
			float t = (high_z - from_z) / direction[1];

			if (t < leave)
			{
				leave = t;
				side = 2;
			}
		}
		else if (direction[1] < -1e-6f)
		{
			float t = (low_z - from_z) / direction[1];

			if (t < leave)
			{
				leave = t;
				side = 3;
			}
		}
		if (side < 0 || leave < travelled - 1e-4f)
		{
			return 1;
		}
		travelled = leave;
		exit[0] = from_x + direction[0] * leave;
		exit[1] = from_z + direction[1] * leave;
		// The portal over that side the point is in (the tile beyond made
		// current first)
		if (rect_on_side(navigation, rect, region, side))
		{
			int32_t neighbor;

			region_neighbor(navigation, region, side, &neighbor);
			if (neighbor >= 0)
			{
				mesh_tile_ready(navigation, c, neighbor, water);
			}
		}
		{
			const navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[region];

			for (int list = 0; list < 2 && through == NULL; list++)
			{
				const navigation_portal_t *portals = list == 0 ? &tile->portals[rect->first_portal] : tile->border[side];
				int32_t count = list == 0 ? rect->portal_count : (rect_on_side(navigation, rect, region, side) ? tile->border_count[side] : 0);

				for (int32_t p = 0; p < count; p++)
				{
					const navigation_portal_t *portal = &portals[p];
					bool along_x = side < 2;

					if (portal->from != (id & 0xFFFF))
					{
						continue;
					}
					// (on this side's line, the point within its run)
					if (along_x ? SDL_fabsf(portal->a[0] - exit[0]) > 1e-3f : SDL_fabsf(portal->a[1] - exit[1]) > 1e-3f)
					{
						continue;
					}
					if (along_x ? (exit[1] >= SDL_min(portal->a[1], portal->b[1]) - 1e-3f && exit[1] <= SDL_max(portal->a[1], portal->b[1]) + 1e-3f)
						: (exit[0] >= SDL_min(portal->a[0], portal->b[0]) - 1e-3f && exit[0] <= SDL_max(portal->a[0], portal->b[0]) + 1e-3f))
					{
						through = portal;
						break;
					}
				}
			}
		}
		mesh_rect_info(navigation, terrain, water, agent, rect, &here);
		if (through != NULL)
		{
			next = through->to;
			mesh_rect_info(navigation, terrain, water, agent, mesh_rect(navigation, c, next), &there);
		}
		if (through == NULL || !mesh_portal_allowed(agent, through, &here, &there))
		{
			if (blocked_x != NULL)
			{
				int32_t cell;

				cell_of_point(navigation, exit[0] + (side == 0 ? 0.5f : side == 1 ? -0.5f : 0.0f) * NAVIGATION_CELL_METERS,
					exit[1] + (side == 2 ? 0.5f : side == 3 ? -0.5f : 0.0f) * NAVIGATION_CELL_METERS, &cell);
				*blocked_x = ((float)(cell % navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
				*blocked_z = ((float)(cell / navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
				*blocked_height = here.height;
			}
			return 0;
		}
		id = next;
	}
	return 1;
}

static int mesh_line_passable(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float *blocked_x, float *blocked_z, float *blocked_height)
{
	int result = mesh_line(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, blocked_x, blocked_z, blocked_height);
	float dx = to_x - from_x;
	float dz = to_z - from_z;
	float length = SDL_sqrtf(dx * dx + dz * dz);

	// Its width: the lines a radius to either side (unless its class keeps
	// whole cells away from things in its rectangles already)
	navigation->mesh.lines++;
	if (result == 1 && !agent->line_center_only && navigation->mesh.classes[mesh_class_of(agent)].erosion_cells == 0 && agent->radius_meters > 0.05f
		&& length > 1e-4f)
	{
		float side_x = -dz / length * agent->radius_meters;
		float side_z = dx / length * agent->radius_meters;

		for (int side = -1; side <= 1 && result == 1; side += 2)
		{
			int beside = mesh_line(navigation, terrain, water, agent, from_x + side_x * (float)side, from_z + side_z * (float)side, from_height,
				to_x + side_x * (float)side, to_z + side_z * (float)side, blocked_x, blocked_z, blocked_height);

			// (a side starting off the mesh, up against something: that side
			// is not looked at, the cells' way would find it free there)
			if (beside == 0)
			{
				result = 0;
			}
		}
	}
	return result;
}

int navigation_mesh_rects(const navigation_t *navigation, const navigation_agent_t *agent, float minimum_x, float minimum_z, float maximum_x,
	float maximum_z, float (*rects_out)[6], int most)
{
	const navigation_mesh_class_t *class = &navigation->mesh.classes[mesh_class_of(agent)];
	int count = 0;

	for (int32_t r = 0; r < navigation->regions_per_side * navigation->regions_per_side && class->tiles != NULL; r++)
	{
		const navigation_mesh_tile_t *tile = &class->tiles[r];

		for (int32_t k = 0; tile->built && k < tile->rect_count && count < most; k++)
		{
			const navigation_rect_t *rect = &tile->rects[k];
			float x0 = (float)rect->x * NAVIGATION_CELL_METERS;
			float z0 = (float)rect->z * NAVIGATION_CELL_METERS;
			float x1 = (float)(rect->x + rect->width) * NAVIGATION_CELL_METERS;
			float z1 = (float)(rect->z + rect->depth) * NAVIGATION_CELL_METERS;

			if (x1 < minimum_x || z1 < minimum_z || x0 > maximum_x || z0 > maximum_z)
			{
				continue;
			}
			rects_out[count][0] = x0;
			rects_out[count][1] = z0;
			rects_out[count][2] = x1 - x0;
			rects_out[count][3] = z1 - z0;
			rects_out[count][4] = (float)rect->kind;
			rects_out[count][5] = rect->height_high;
			count++;
		}
	}
	return count;
}

void navigation_mesh_describe(const navigation_t *navigation, char *text_out, size_t text_size)
{
	int32_t region_count = navigation->regions_per_side * navigation->regions_per_side;
	int64_t tiles[NAVIGATION_MESH_CLASSES] = { 0 };
	int64_t rects[NAVIGATION_MESH_CLASSES] = { 0 };
	int64_t portals[NAVIGATION_MESH_CLASSES] = { 0 };
	size_t bytes = 0;

	for (int c = 0; c < NAVIGATION_MESH_CLASSES; c++)
	{
		const navigation_mesh_class_t *class = &navigation->mesh.classes[c];

		for (int32_t r = 0; r < region_count && class->tiles != NULL; r++)
		{
			const navigation_mesh_tile_t *tile = &class->tiles[r];

			if (!tile->built)
			{
				continue;
			}
			tiles[c]++;
			rects[c] += tile->rect_count;
			portals[c] += tile->portal_count;
			bytes += sizeof(navigation_rect_t) * (size_t)tile->rect_capacity + sizeof(navigation_portal_t) * (size_t)tile->portal_capacity;
			for (int side = 0; side < 4; side++)
			{
				portals[c] += tile->border_count[side];
				bytes += sizeof(navigation_portal_t) * (size_t)tile->border_capacity[side];
			}
			bytes += sizeof(int16_t) * (size_t)(navigation->region_size * navigation->region_size + NAVIGATION_REGION_EXTRA_SLOTS);
		}
	}
	{
		size_t used = (size_t)SDL_snprintf(text_out, text_size, "Navmesh %s:", navigation->use_navmesh ? "on" : "off");

		for (int c = 0; c < NAVIGATION_MESH_CLASSES && used < text_size; c++)
		{
			const navigation_mesh_class_t *class = &navigation->mesh.classes[c];

			used += (size_t)SDL_snprintf(text_out + used, text_size - used, " %d quarter%s wide: %lld tiles, %lld rectangles, %lld portals (reach %.2f m, "
				"height %.2f m, kept %d cells off);", c + 1, c == 0 ? "" : "s", (long long)tiles[c], (long long)rects[c], (long long)portals[c],
				(double)(class->reach < 1e8f ? class->reach : 0.0f), (double)(class->height < 1e8f ? class->height : 0.0f), class->erosion_cells);
		}
		if (used < text_size)
		{
			SDL_snprintf(text_out + used, text_size - used,
				" %.1f MB; %lld tiles built, the last in %.2f ms; the last search %.2f ms (either way %.2f ms, %d expanded); searches %lld (%lld by the cells, "
				"%lld ways that did not hold, %lld cut short), lines %lld (%lld by the cells)",
				(double)bytes / (1024.0 * 1024.0), (long long)navigation->mesh.tiles_built, (double)navigation->mesh.last_build_milliseconds,
				(double)navigation->mesh.last_search_ticks * 1000.0 / (double)SDL_GetPerformanceFrequency(),
				(double)navigation->last_search_ticks * 1000.0 / (double)SDL_GetPerformanceFrequency(), navigation->last_expansions,
				(long long)navigation->mesh.searches, (long long)navigation->mesh.search_fallbacks, (long long)navigation->mesh.ways_refused,
				(long long)navigation->mesh.ways_cut,
				(long long)navigation->mesh.lines,
				(long long)navigation->mesh.line_fallbacks);
		}
	}
}

// ---------------------------------------------------------------------------
// Flying

// The highest thing in a cell: its ground, or an object's top
static float cell_obstacle_height(const navigation_t *navigation, int32_t cell)
{
	return navigation->ground_height[cell] + (float)navigation->object_top[cell] * NAVIGATION_OBJECT_TOP_UNIT_METERS;
}

float navigation_obstacle_height(const navigation_t *navigation, const terrain_t *terrain, float x, float z)
{
	int32_t cell;
	float terrain_height = terrain_sample_height_meters(terrain, x, z);

	if (navigation->object_top == NULL)
	{
		return terrain_height;
	}
	cell_of_point(navigation, x, z, &cell);
	if (navigation->object_top[cell] == 0)
	{
		return terrain_height;
	}
	return SDL_max(terrain_height, cell_obstacle_height(navigation, cell));
}

bool navigation_flight_line_clear(const navigation_t *navigation, const terrain_t *terrain, float from_x, float from_z, float to_x,
	float to_z, float ceiling_height)
{
	float dx = to_x - from_x;
	float dz = to_z - from_z;
	int steps = SDL_max((int)SDL_ceilf(SDL_sqrtf(dx * dx + dz * dz) / FLIGHT_LINE_STEP_METERS), 1);

	(void)terrain;
	for (int step = 0; step <= steps; step++)
	{
		float amount = (float)step / (float)steps;
		int32_t cell;

		cell_of_point(navigation, from_x + dx * amount, from_z + dz * amount, &cell);
		if (cell_obstacle_height(navigation, cell) > ceiling_height)
		{
			return false;
		}
	}
	return true;
}

// The highest thing in a block of FLIGHT_BLOCK_CELLS x FLIGHT_BLOCK_CELLS
static float block_obstacle_height(const navigation_t *navigation, int32_t block_x, int32_t block_z)
{
	int32_t first_x = block_x * FLIGHT_BLOCK_CELLS;
	int32_t first_z = block_z * FLIGHT_BLOCK_CELLS;
	int32_t last_x = SDL_min(first_x + FLIGHT_BLOCK_CELLS, navigation->size) - 1;
	int32_t last_z = SDL_min(first_z + FLIGHT_BLOCK_CELLS, navigation->size) - 1;
	float highest = -1e30f;

	for (int32_t cell_z = first_z; cell_z <= last_z; cell_z++)
	{
		for (int32_t cell_x = first_x; cell_x <= last_x; cell_x++)
		{
			highest = SDL_max(highest, cell_obstacle_height(navigation, cell_z * navigation->size + cell_x));
		}
	}
	return highest;
}

navigation_result_t navigation_find_flight_path(navigation_t *navigation, const terrain_t *terrain, float from_x, float from_z,
	float to_x, float to_z, float ceiling_height, float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2], int *waypoint_count_out,
	bool *complete_out)
{
	int32_t blocks = (navigation->size + FLIGHT_BLOCK_CELLS - 1) / FLIGHT_BLOCK_CELLS;
	float block_meters = (float)FLIGHT_BLOCK_CELLS * NAVIGATION_CELL_METERS;
	int32_t start_cell;
	int32_t goal_cell;
	int32_t start;
	int32_t goal;
	int32_t goal_x;
	int32_t goal_z;
	int32_t best;
	float best_distance;
	uint32_t reached_tag;
	int32_t path_length = 0;
	bool found = false;

	// The heights flyers go over, along the way (farther than for walking:
	// flight searches go in blocks of 4 cells)
	prepare_way(navigation, from_x, from_z, to_x, to_z, FLIGHT_PREPARE_METERS);
	*waypoint_count_out = 0;
	*complete_out = true;
	navigation->last_expansions = 0;
	if (navigation_flight_line_clear(navigation, terrain, from_x, from_z, to_x, to_z, ceiling_height))
	{
		waypoints_out[0][0] = to_x;
		waypoints_out[0][1] = to_z;
		*waypoint_count_out = 1;
		return NAVIGATION_PATH_FOUND;
	}
	if (!ensure_search_memory(navigation))
	{
		return NAVIGATION_PATH_NONE;
	}
	cell_of_point(navigation, from_x, from_z, &start_cell);
	cell_of_point(navigation, to_x, to_z, &goal_cell);
	start = (start_cell / navigation->size / FLIGHT_BLOCK_CELLS) * blocks + start_cell % navigation->size / FLIGHT_BLOCK_CELLS;
	goal = (goal_cell / navigation->size / FLIGHT_BLOCK_CELLS) * blocks + goal_cell % navigation->size / FLIGHT_BLOCK_CELLS;
	goal_x = goal % blocks;
	goal_z = goal / blocks;
	navigation->search_number++;
	if (navigation->search_number >= 0x7FFFFFFFu)
	{
		SDL_memset(navigation->search, 0, sizeof(navigation_search_node_t) * (size_t)node_count(navigation));
		navigation->search_number = 1;
	}
	reached_tag = navigation->search_number * 2;
	navigation->heap_count = 0;
	navigation->search[start].tag = reached_tag;
	navigation->search[start].cost = 0.0f;
	navigation->search[start].came_from = -1;
	best = start;
	best_distance = estimate(start % blocks, start / blocks, goal_x, goal_z);
	heap_push(navigation, start, best_distance * block_meters);

	while (navigation->heap_count > 0)
	{
		int32_t node = heap_pop(navigation);
		int32_t block_x = node % blocks;
		int32_t block_z = node / blocks;
		float here_height;

		if (navigation->search[node].tag == reached_tag + 1)
		{
			continue;
		}
		navigation->search[node].tag = reached_tag + 1;
		if (node == goal)
		{
			found = true;
			best = node;
			break;
		}
		{
			float distance = estimate(block_x, block_z, goal_x, goal_z);

			if (distance < best_distance)
			{
				best_distance = distance;
				best = node;
			}
		}
		if (++navigation->last_expansions >= navigation->maximum_expansions)
		{
			break;
		}
		here_height = block_obstacle_height(navigation, block_x, block_z);
		for (int d = 0; d < 8; d++)
		{
			int32_t next_x = block_x + direction_x[d];
			int32_t next_z = block_z + direction_z[d];
			int32_t next;
			float next_height;
			float cost;

			if (next_x < 0 || next_z < 0 || next_x >= blocks || next_z >= blocks)
			{
				continue;
			}
			next = next_z * blocks + next_x;
			if (navigation->search[next].tag == reached_tag + 1)
			{
				continue;
			}
			next_height = block_obstacle_height(navigation, next_x, next_z);
			// The goal's own block is fine whatever else is in it (a cliff
			// next to the spot)
			if (next_height > ceiling_height && next != goal)
			{
				continue;
			}
			// No cutting the corner of a mountain
			if (d >= 4 && (block_obstacle_height(navigation, next_x, block_z) > ceiling_height
				|| block_obstacle_height(navigation, block_x, next_z) > ceiling_height))
			{
				continue;
			}
			cost = navigation->search[node].cost + direction_length[d] * block_meters
				+ FLIGHT_CLIMB_COST * SDL_max(next_height - here_height, 0.0f);
			if (navigation->search[next].tag != reached_tag || cost < navigation->search[next].cost)
			{
				navigation->search[next].tag = reached_tag;
				navigation->search[next].cost = cost;
				navigation->search[next].came_from = node;
				if (!heap_push(navigation, next, cost + HEURISTIC_WEIGHT * estimate(next_x, next_z, goal_x, goal_z) * block_meters))
				{
					navigation->heap_count = 0;
					break;
				}
			}
		}
	}

	for (int32_t node = best; node != start; node = navigation->search[node].came_from)
	{
		if (path_length == navigation->node_path_capacity)
		{
			int32_t capacity = SDL_max(1024, navigation->node_path_capacity * 2);
			int32_t *grown = SDL_realloc(navigation->node_path, sizeof(int32_t) * (size_t)capacity);

			if (grown == NULL)
			{
				return NAVIGATION_PATH_NONE;
			}
			navigation->node_path = grown;
			navigation->node_path_capacity = capacity;
		}
		navigation->node_path[path_length++] = node;
	}
	if (path_length == 0)
	{
		if (found)
		{
			waypoints_out[0][0] = to_x;
			waypoints_out[0][1] = to_z;
			*waypoint_count_out = 1;
			return NAVIGATION_PATH_FOUND;
		}
		return NAVIGATION_PATH_NONE;
	}

	// Pulled tight: from each waypoint straight on to the farthest block
	// center it sees under the ceiling
	{
		float anchor_x = from_x;
		float anchor_z = from_z;
		int32_t anchor = path_length;
		int count = 0;

		while (anchor > 0)
		{
			int32_t visible = anchor - 1;
			float point_x;
			float point_z;

			for (int32_t candidate = anchor - 1; candidate >= 0 && anchor - candidate <= PULL_MAXIMUM_CELLS; candidate--)
			{
				int32_t node = navigation->node_path[candidate];
				float candidate_x = ((float)(node % blocks) + 0.5f) * block_meters;
				float candidate_z = ((float)(node / blocks) + 0.5f) * block_meters;

				if (candidate == 0 && found)
				{
					candidate_x = to_x;
					candidate_z = to_z;
				}
				if (candidate == anchor - 1
					|| navigation_flight_line_clear(navigation, terrain, anchor_x, anchor_z, candidate_x, candidate_z, ceiling_height))
				{
					visible = candidate;
				}
				else
				{
					break;
				}
			}
			point_x = ((float)(navigation->node_path[visible] % blocks) + 0.5f) * block_meters;
			point_z = ((float)(navigation->node_path[visible] / blocks) + 0.5f) * block_meters;
			if (visible == 0 && found)
			{
				point_x = to_x;
				point_z = to_z;
			}
			waypoints_out[count][0] = point_x;
			waypoints_out[count][1] = point_z;
			count++;
			anchor_x = point_x;
			anchor_z = point_z;
			anchor = visible;
			if (count == NAVIGATION_MAX_WAYPOINTS && anchor > 0)
			{
				*complete_out = false;
				break;
			}
		}
		*waypoint_count_out = count;
	}
	return found ? NAVIGATION_PATH_FOUND : NAVIGATION_PATH_PARTIAL;
}

// ---------------------------------------------------------------------------
// Debugging

bool navigation_describe(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const objects_t *objects,
	const navigation_agent_t *agent, float x, float z, float near_height, char *text_out, size_t text_size)
{
	int32_t cell;
	int32_t node;
	node_info_t info;
	float room;
	uint8_t clearance;
	float worst_step = 0.0f;
	float reach = SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);
	const char *blocker = "";
	float depth = 0.0f;
	float velocity_x = 0.0f;
	float velocity_z = 0.0f;
	size_t used;

	if (!cell_of_point(navigation, x, z, &cell))
	{
		SDL_snprintf(text_out, text_size, "outside the map");
		return false;
	}
	prepare_box(navigation, x, z, x, z);
	node = nearest_node(navigation, terrain, cell, near_height);
	node_info(navigation, terrain, water, agent, node, &info);
	if (node < cell_count(navigation))
	{
		clearance = navigation->terrain_clearance[cell];
		if (navigation->terrain_blocker[cell] >= 0 && navigation->terrain_blocker[cell] < objects->types.type_count)
		{
			blocker = objects->types.types[navigation->terrain_blocker[cell]].name;
		}
	}
	else
	{
		int32_t index = node - cell_count(navigation);

		clearance = navigation->regions[index / NAVIGATION_REGION_EXTRA_SLOTS].surfaces[index % NAVIGATION_REGION_EXTRA_SLOTS].clearance;
	}
	room = clearance == NAVIGATION_CLEARANCE_OPEN ? -1.0f : (float)clearance * NAVIGATION_CLEARANCE_UNIT_METERS;
	for (int d = 0; d < 4; d++)
	{
		int32_t next_x = cell % navigation->size + direction_x[d];
		int32_t next_z = cell / navigation->size + direction_z[d];

		if (next_x >= 0 && next_z >= 0 && next_x < navigation->size && next_z < navigation->size)
		{
			worst_step = SDL_max(worst_step, SDL_fabsf(navigation->ground_height[next_z * navigation->size + next_x] - info.height));
		}
	}
	if (water != NULL)
	{
		water_sample(water, ((float)(cell % navigation->size) + 0.5f) * NAVIGATION_CELL_METERS,
			((float)(cell / navigation->size) + 0.5f) * NAVIGATION_CELL_METERS, &depth, &velocity_x, &velocity_z);
	}
	used = (size_t)SDL_snprintf(text_out, text_size, "cell (%d, %d) %s at %.2f m, room %s", cell % navigation->size,
		cell / navigation->size, node < cell_count(navigation) ? "terrain" : "object top", (double)info.height,
		room < 0.0f ? "open" : "");
	if (room >= 0.0f && used < text_size)
	{
		used += (size_t)SDL_snprintf(text_out + used, text_size - used, "%.2f m (needs %.2f)%s%s", (double)room,
			(double)agent->height_meters, blocker[0] != '\0' ? " under " : "", blocker);
	}
	if (used < text_size)
	{
		used += (size_t)SDL_snprintf(text_out + used, text_size - used, ", steepest step to a neighbor %.2f m (reach %.2f)",
			(double)worst_step, (double)reach);
	}
	if (depth > 0.01f && used < text_size)
	{
		SDL_snprintf(text_out + used, text_size - used, ", water %.2f m deep at %.2f m/s%s", (double)depth,
			(double)SDL_sqrtf(velocity_x * velocity_x + velocity_z * velocity_z), info.water_class == CLASS_SWIM ? " (swims)" : "");
	}
	return info.passable;
}

void navigation_fill_overlay(navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, uint8_t *overlay_out)
{
	float reach = SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);

	// A debugging view of the whole map: all of it up to date
	prepare_regions(navigation, 0, 0, navigation->regions_per_side - 1, navigation->regions_per_side - 1);

	for (int32_t cell = 0; cell < cell_count(navigation); cell++)
	{
		int32_t cell_x = cell % navigation->size;
		int32_t cell_z = cell / navigation->size;
		uint8_t clearance = navigation->terrain_clearance[cell];
		uint8_t value = NAVIGATION_OVERLAY_FREE;
		node_info_t info;

		float small = (float)navigation->small_height[cell] * SMALL_UNIT_METERS;

		if (navigation->extra_count[cell] > 0)
		{
			value = NAVIGATION_OVERLAY_SURFACE;
		}
		else if (small >= agent->height_meters)
		{
			value = NAVIGATION_OVERLAY_BLOCKED;
		}
		else if (small > agent->step_meters)
		{
			value = NAVIGATION_OVERLAY_SMALL;
		}
		else if (clearance == 0)
		{
			value = NAVIGATION_OVERLAY_BLOCKED;
		}
		else if (clearance != NAVIGATION_CLEARANCE_OPEN && (float)clearance * NAVIGATION_CLEARANCE_UNIT_METERS < agent->height_meters)
		{
			value = NAVIGATION_OVERLAY_LOW;
		}
		else
		{
			node_info(navigation, terrain, water, agent, cell, &info);
			if (!info.passable)
			{
				value = NAVIGATION_OVERLAY_WATER;
			}
			else if (info.water_class != CLASS_SWIM)
			{
				int out_of_reach = 0;
				int neighbors = 0;

				for (int d = 0; d < 4; d++)
				{
					int32_t next_x = cell_x + direction_x[d];
					int32_t next_z = cell_z + direction_z[d];

					if (next_x >= 0 && next_z >= 0 && next_x < navigation->size && next_z < navigation->size)
					{
						neighbors++;
						out_of_reach += SDL_fabsf(navigation->ground_height[next_z * navigation->size + next_x] - info.height) > reach;
					}
				}
				// Cut off on every side: a wall; on some: a slope it can only
				// go along
				if (out_of_reach > 0)
				{
					value = out_of_reach == neighbors ? NAVIGATION_OVERLAY_STEEP : NAVIGATION_OVERLAY_STEEP_SOME;
				}
			}
		}
		overlay_out[cell] = value;
	}
	// The mesh's rectangles of the agent's class: their edges' cells marked
	if (navigation->use_navmesh)
	{
		int c = mesh_class_of(agent);

		for (int32_t r = 0; r < navigation->regions_per_side * navigation->regions_per_side; r++)
		{
			const navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[r];

			for (int32_t k = 0; tile->built && k < tile->rect_count; k++)
			{
				const navigation_rect_t *rect = &tile->rects[k];

				if (rect->kind != 0)
				{
					continue;
				}
				for (int32_t z = rect->z; z < rect->z + rect->depth; z++)
				{
					for (int32_t x = rect->x; x < rect->x + rect->width; x++)
					{
						if (x == rect->x || z == rect->z || x == rect->x + rect->width - 1 || z == rect->z + rect->depth - 1)
						{
							overlay_out[z * navigation->size + x] |= NAVIGATION_OVERLAY_MESH_EDGE;
						}
					}
				}
			}
		}
	}
}
