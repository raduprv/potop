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
static int mesh_line(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent, float from_x,
	float from_z, float from_height, float to_x, float to_z, float *blocked_x, float *blocked_z, float *blocked_height);

#define MESH_FOLLOW_SLACK_METERS 0.13f  // a walker this far off a rectangle's edge is still on it (half a quarter meter)
#define MESH_FOLLOW_LEVEL_METERS 0.5f   // and on a level this far from its height, at least
#define MESH_FOLLOW_NEAR_QUARTERS 3     // what is underfoot off the rectangles: the nearest one's, this far
// The follower's questions answered from the rectangles (follow_mesh:
// see mesh_follow_level, further down)
static bool mesh_follow_level(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float x, float z, float near_height, float slack, float *height_out, bool *ground_out);
static bool mesh_follow_height(const navigation_t *navigation, const terrain_t *terrain, const navigation_agent_t *agent, float x, float z,
	float near_height, float *height_out, bool *object_out);
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
// The water over a cell as it is now, onto what a surface there is to an
// agent (info: its height, passable, its cost): ice bears, deep water is
// swum (if it swims, and the flow lets it), shallower waded
static void info_water(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t cell_x, int32_t cell_z, float terrain_height, node_info_t *info)
{
	float x = ((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS;
	float z = ((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS;
	float depth = 0.0f;
	float velocity_x = 0.0f;
	float velocity_z = 0.0f;

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

static void node_info_at(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t node, int32_t cell, int32_t cell_x, int32_t cell_z, node_info_t *info)
{
	float terrain_height = navigation->ground_height[cell];
	uint8_t clearance;
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
	info_water(navigation, terrain, water, agent, cell_x, cell_z, terrain_height, info);
}
static void node_info(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t node, node_info_t *info)
{
	int32_t cell = node_cell(navigation, node);

	node_info_at(navigation, terrain, water, agent, node, cell, cell % navigation->size, cell / navigation->size, info);
}

#define LINK_SHUT_METERS 3.0f         // a shut door costs this much more than its length (its opening)

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
static int navigation_model(const navigation_t *navigation, const objects_t *objects, const object_instance_t *instance, float *radius_out)
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

// ---------------------------------------------------------------------------
// Small things: what an object is to those that walk, and its capsules

#define MESH_WELD_SHARE 0.9f           // two small things with less than this much of a body's width between them: no way between
#define KIND_WALL 0                    // in the surfaces and in the rectangles
#define KIND_ROUND 1                   // a small thing gone round: in the surfaces, not in the rectangles
#define KIND_SOFT 2                    // a soft one: a height per cell, not in the rectangles
#define KIND_NONE 3                    // nothing to the walkers (a bush: only the flyers' heights)

#define SHAPE_SLICE_METERS 0.5f        // a model is measured in slices this high (a crooked trunk: a few capsules)
#define SHAPE_SLICES_MOST 16           // from its foot up (8 m at size 1: over any head)
#define SHAPE_FIT_SHARE 1.25f          // a capsule this much wider than its widest slice (and SHAPE_FIT_METERS) is cut in two
#define SHAPE_FIT_METERS 0.05f
#define KIND_REACH_HEIGHT_METERS 2.0f   // a thing's footprint is what of it stands this low (a man's height)
#define SHAPE_FULL_SHARE 0.6f          // of its slices' cylinders, a model with no closed shape to measure is taken to fill

// The part of a triangle between two heights: its corners (at most 5)
static int clip_triangle_to_heights(float corners[3][3], float low, float high, float out[8][3])
{
	float points[8][3];
	float clipped[8][3];
	int count = 3;

	SDL_memcpy(points, corners, sizeof(float) * 9);
	for (int side = 0; side < 2; side++)
	{
		float limit = side == 0 ? low : high;
		float sign = side == 0 ? 1.0f : -1.0f;
		int kept = 0;

		for (int k = 0; k < count; k++)
		{
			const float *from = points[k];
			const float *to = points[(k + 1) % count];
			float from_in = (from[1] - limit) * sign;
			float to_in = (to[1] - limit) * sign;

			if (from_in >= 0.0f)
			{
				SDL_memcpy(clipped[kept++], from, sizeof(float) * 3);
			}
			if ((from_in >= 0.0f) != (to_in >= 0.0f))
			{
				float t = from_in / (from_in - to_in);

				for (int axis = 0; axis < 3; axis++)
				{
					clipped[kept][axis] = from[axis] + (to[axis] - from[axis]) * t;
				}
				kept++;
			}
		}
		count = kept;
		SDL_memcpy(points, clipped, sizeof(float) * 3 * (size_t)count);
		if (count == 0)
		{
			return 0;
		}
	}
	SDL_memcpy(out, points, sizeof(float) * 3 * (size_t)count);
	return count;
}

// A model's capsules, from its collision triangles (the trunk alone of a
// tree: what the physics uses): a circle round what each slice of its
// height holds, one capsule from the lowest to the highest if every
// circle is about as wide as it (a straight trunk, a barrel), else cut in
// two where it fits worst (a crooked one), up to
// NAVIGATION_MODEL_CAPSULES_MOST of them
static void fit_model_shape(const object_model_t *model, navigation_model_shape_t *shape)
{
	float low = 1e30f;
	float high = -1e30f;
	float centers[SHAPE_SLICES_MOST][2];
	float radii[SHAPE_SLICES_MOST];
	float boxes[SHAPE_SLICES_MOST][4];
	bool filled[SHAPE_SLICES_MOST];
	int runs[NAVIGATION_MODEL_CAPSULES_MOST][2];
	int run_count = 0;
	int slice_count;
	int first = -1;
	int last = -1;
	float slices_volume = 0.0f;
	float closed_volume = 0.0f;

	SDL_zerop(shape);
	shape->fitted = true;
	for (int k = 0; k < model->collision_index_count; k++)
	{
		float y = model->collision_positions[3 * model->collision_indices[k] + 1];

		low = SDL_min(low, y);
		high = SDL_max(high, y);
	}
	if (model->collision_index_count < 3 || high - low < 1e-3f)
	{
		return;
	}
	slice_count = clamp_int((int)SDL_ceilf((high - low) / SHAPE_SLICE_METERS), 1, SHAPE_SLICES_MOST);
	for (int slice = 0; slice < slice_count; slice++)
	{
		boxes[slice][0] = boxes[slice][1] = 1e30f;
		boxes[slice][2] = boxes[slice][3] = -1e30f;
		radii[slice] = 0.0f;
		filled[slice] = false;
	}
	// The slices' boxes first (their middles), then how far round those
	// they reach
	for (int pass = 0; pass < 2; pass++)
	{
		for (int t = 0; t + 2 < model->collision_index_count; t += 3)
		{
			float corners[3][3];
			float triangle_low = 1e30f;
			float triangle_high = -1e30f;

			for (int k = 0; k < 3; k++)
			{
				SDL_memcpy(corners[k], &model->collision_positions[3 * model->collision_indices[t + k]], sizeof(float) * 3);
				triangle_low = SDL_min(triangle_low, corners[k][1]);
				triangle_high = SDL_max(triangle_high, corners[k][1]);
			}
			if (pass == 0)
			{
				// (six times the volume between it and the origin: a
				// closed shape's add up to its own)
				closed_volume += corners[0][0] * (corners[1][1] * corners[2][2] - corners[1][2] * corners[2][1])
					- corners[0][1] * (corners[1][0] * corners[2][2] - corners[1][2] * corners[2][0])
					+ corners[0][2] * (corners[1][0] * corners[2][1] - corners[1][1] * corners[2][0]);
			}
			for (int slice = SDL_max((int)SDL_floorf((triangle_low - low) / SHAPE_SLICE_METERS), 0);
				slice < slice_count && low + (float)slice * SHAPE_SLICE_METERS <= triangle_high; slice++)
			{
				float points[8][3];
				int count = clip_triangle_to_heights(corners, low + (float)slice * SHAPE_SLICE_METERS,
					low + (float)(slice + 1) * SHAPE_SLICE_METERS, points);

				for (int k = 0; k < count; k++)
				{
					if (pass == 0)
					{
						boxes[slice][0] = SDL_min(boxes[slice][0], points[k][0]);
						boxes[slice][1] = SDL_min(boxes[slice][1], points[k][2]);
						boxes[slice][2] = SDL_max(boxes[slice][2], points[k][0]);
						boxes[slice][3] = SDL_max(boxes[slice][3], points[k][2]);
						filled[slice] = true;
					}
					else
					{
						float dx = points[k][0] - centers[slice][0];
						float dz = points[k][2] - centers[slice][1];

						radii[slice] = SDL_max(radii[slice], SDL_sqrtf(dx * dx + dz * dz));
					}
				}
			}
		}
		for (int slice = 0; slice < slice_count && pass == 0; slice++)
		{
			centers[slice][0] = 0.5f * (boxes[slice][0] + boxes[slice][2]);
			centers[slice][1] = 0.5f * (boxes[slice][1] + boxes[slice][3]);
		}
	}
	for (int slice = 0; slice < slice_count; slice++)
	{
		if (filled[slice])
		{
			float slice_high = SDL_min(low + (float)(slice + 1) * SHAPE_SLICE_METERS, high);

			first = first < 0 ? slice : first;
			last = slice;
			slices_volume += SDL_PI_F * radii[slice] * radii[slice] * SDL_max(slice_high - (low + (float)slice * SHAPE_SLICE_METERS), 0.0f);
		}
	}
	if (first < 0)
	{
		return;
	}
	closed_volume = SDL_fabsf(closed_volume) / 6.0f;
	shape->volume = closed_volume > 0.2f * slices_volume && closed_volume <= slices_volume ? closed_volume : SHAPE_FULL_SHARE * slices_volume;
	runs[0][0] = first;
	runs[0][1] = last;
	run_count = 1;
	for (;;)
	{
		int worst_run = -1;
		float worst_over = 0.0f;

		for (int run = 0; run < run_count; run++)
		{
			const float *from = centers[runs[run][0]];
			const float *to = centers[runs[run][1]];
			float radius = 0.0f;
			float widest = 0.0f;

			for (int slice = runs[run][0]; slice <= runs[run][1]; slice++)
			{
				float t = runs[run][1] > runs[run][0] ? (float)(slice - runs[run][0]) / (float)(runs[run][1] - runs[run][0]) : 0.0f;
				float dx = centers[slice][0] - (from[0] + (to[0] - from[0]) * t);
				float dz = centers[slice][1] - (from[1] + (to[1] - from[1]) * t);

				if (filled[slice])
				{
					radius = SDL_max(radius, SDL_sqrtf(dx * dx + dz * dz) + radii[slice]);
					widest = SDL_max(widest, radii[slice]);
				}
			}
			shape->capsules[run][0] = from[0];
			shape->capsules[run][1] = low + (float)runs[run][0] * SHAPE_SLICE_METERS;
			shape->capsules[run][2] = from[1];
			shape->capsules[run][3] = to[0];
			shape->capsules[run][4] = SDL_min(low + (float)(runs[run][1] + 1) * SHAPE_SLICE_METERS, high);
			shape->capsules[run][5] = to[1];
			shape->capsules[run][6] = radius;
			if (runs[run][1] > runs[run][0] && radius - (widest * SHAPE_FIT_SHARE + SHAPE_FIT_METERS) > worst_over)
			{
				worst_over = radius - (widest * SHAPE_FIT_SHARE + SHAPE_FIT_METERS);
				worst_run = run;
			}
		}
		if (worst_run < 0 || run_count >= NAVIGATION_MODEL_CAPSULES_MOST)
		{
			break;
		}
		// (cut in the middle: the lower half keeps its place, the upper is new)
		runs[run_count][0] = (runs[worst_run][0] + runs[worst_run][1] + 1) / 2;
		runs[run_count][1] = runs[worst_run][1];
		runs[worst_run][1] = runs[run_count][0] - 1;
		run_count++;
	}
	shape->capsule_count = run_count;
}

// A model's shape, fitted the first time it is asked for (the regions are
// built side by side: one at a time here)
static void model_shape(navigation_t *navigation, const objects_t *objects, int model, navigation_model_shape_t *shape_out)
{
	SDL_zerop(shape_out);
#pragma omp critical(navigation_model_shape)
	{
		if (model >= navigation->model_shape_count)
		{
			int count = SDL_max(objects->model_count, model + 1);
			navigation_model_shape_t *grown = SDL_realloc(navigation->model_shapes, sizeof(navigation_model_shape_t) * (size_t)count);

			if (grown != NULL)
			{
				SDL_memset(grown + navigation->model_shape_count, 0,
					sizeof(navigation_model_shape_t) * (size_t)(count - navigation->model_shape_count));
				navigation->model_shapes = grown;
				navigation->model_shape_count = count;
			}
		}
		if (model < navigation->model_shape_count)
		{
			if (!navigation->model_shapes[model].fitted)
			{
				fit_model_shape(&objects->models[model], &navigation->model_shapes[model]);
			}
			*shape_out = navigation->model_shapes[model];
		}
	}
}

// One of a model's capsules as its object stands in the world
static void place_capsule(const object_instance_t *instance, const float capsule[7], navigation_capsule_t *out)
{
	float base_y = objects_instance_base_y(instance);

	for (int end = 0; end < 2; end++)
	{
		float scaled[3] = { capsule[3 * end] * instance->scale, capsule[3 * end + 1] * instance->scale, capsule[3 * end + 2] * instance->scale };
		float turned[3];
		float *point = end == 0 ? out->a : out->b;

		rotate_point(instance->rotation, scaled, turned);
		point[0] = instance->x + turned[0];
		point[1] = base_y + turned[1];
		point[2] = instance->z + turned[2];
	}
	out->radius = capsule[6] * instance->scale;
	out->type = (int16_t)instance->type;
	out->soft = 0;
	out->padding = 0;
	out->id = instance->id;
}

// The part of a capsule's axis between two heights; false: none of it
static bool capsule_between(const navigation_capsule_t *capsule, float low, float high, float a_out[2], float b_out[2])
{
	float dy = capsule->b[1] - capsule->a[1];
	float from = 0.0f;
	float to = 1.0f;

	if (SDL_fabsf(dy) < 1e-5f)
	{
		if (capsule->a[1] < low || capsule->a[1] > high)
		{
			return false;
		}
	}
	else
	{
		float t_low = (low - capsule->a[1]) / dy;
		float t_high = (high - capsule->a[1]) / dy;

		from = SDL_max(from, SDL_min(t_low, t_high));
		to = SDL_min(to, SDL_max(t_low, t_high));
		if (from > to)
		{
			return false;
		}
	}
	a_out[0] = capsule->a[0] + (capsule->b[0] - capsule->a[0]) * from;
	a_out[1] = capsule->a[2] + (capsule->b[2] - capsule->a[2]) * from;
	b_out[0] = capsule->a[0] + (capsule->b[0] - capsule->a[0]) * to;
	b_out[1] = capsule->a[2] + (capsule->b[2] - capsule->a[2]) * to;
	return true;
}

// What an object is to those that walk (KIND_), and its capsules as it
// stands (capsules_out, NAVIGATION_MODEL_CAPSULES_MOST of them): what its
// type says (objects.ini: navigation), else a wall if it is a module's
// part or its footprint's longest reach, up to a man's height
// (KIND_REACH_HEIGHT_METERS), is over wall_reach_meters (a fence's segment is thin, but long: a
// wall; so is a trunk lying down); under it a small thing gone round, or
// a soft one if it is a body lighter than soft_kilograms
static int object_kind(navigation_t *navigation, const objects_t *objects, const object_instance_t *instance, int model,
	navigation_capsule_t *capsules_out, int *capsule_count_out)
{
	const object_type_t *type = &objects->types.types[instance->type];
	navigation_model_shape_t shape;
	float base_y = objects_instance_base_y(instance);
	float ends[2 * NAVIGATION_MODEL_CAPSULES_MOST][2];
	float radii[2 * NAVIGATION_MODEL_CAPSULES_MOST];
	int end_count = 0;
	float reach = 0.0f;
	int kind;

	*capsule_count_out = 0;
	if (type->navigation == OBJECT_NAVIGATION_WALL || (type->navigation == OBJECT_NAVIGATION_AUTO && (instance->state & OBJECT_STATE_PART) != 0))
	{
		return KIND_WALL;
	}
	model_shape(navigation, objects, model, &shape);
	for (int k = 0; k < shape.capsule_count; k++)
	{
		navigation_capsule_t *capsule = &capsules_out[*capsule_count_out];

		place_capsule(instance, shape.capsules[k], capsule);
		if (capsule_between(capsule, base_y, base_y + NAVIGATION_OBJECT_HEIGHT_METERS, ends[end_count], ends[end_count + 1]))
		{
			(*capsule_count_out)++;
		}
		if (capsule_between(capsule, base_y, base_y + KIND_REACH_HEIGHT_METERS, ends[end_count], ends[end_count + 1]))
		{
			radii[end_count] = radii[end_count + 1] = capsule->radius;
			end_count += 2;
		}
	}
	for (int i = 0; i < end_count; i++)
	{
		for (int j = i; j < end_count; j++)
		{
			float dx = ends[j][0] - ends[i][0];
			float dz = ends[j][1] - ends[i][1];

			reach = SDL_max(reach, SDL_sqrtf(dx * dx + dz * dz) + radii[i] + radii[j]);
		}
	}
	if (type->navigation == OBJECT_NAVIGATION_ROUND)
	{
		kind = KIND_ROUND;
	}
	else if (type->navigation == OBJECT_NAVIGATION_SOFT)
	{
		kind = KIND_SOFT;
	}
	else if (end_count == 0 || reach > navigation->wall_reach_meters)
	{
		// (nothing of it measured: as it always was)
		kind = KIND_WALL;
	}
	else
	{
		float size = instance->scale * instance->scale * instance->scale;
		float mass = type->mass_kilograms > 0.0f ? type->mass_kilograms * size
			: SDL_max(type->density_kilograms_per_cubic_meter, 1.0f) * shape.volume * size;

		kind = type->physics == OBJECT_PHYSICS_BODY && mass < navigation->soft_kilograms ? KIND_SOFT : KIND_ROUND;
	}
	for (int k = 0; k < *capsule_count_out; k++)
	{
		capsules_out[k].soft = kind == KIND_SOFT ? 1 : 0;
	}
	*capsule_count_out = kind == KIND_WALL ? 0 : *capsule_count_out;
	return kind;
}

// The square of a region's capsules a point is in, or nearest
static int capsule_square(const navigation_t *navigation, int32_t region_index, float x, float z)
{
	float square_meters = NAVIGATION_REGION_METERS / (float)NAVIGATION_CAPSULE_SQUARES;
	int square_x = (int)SDL_floorf((x - (float)(region_index % navigation->regions_per_side) * NAVIGATION_REGION_METERS) / square_meters);
	int square_z = (int)SDL_floorf((z - (float)(region_index / navigation->regions_per_side) * NAVIGATION_REGION_METERS) / square_meters);

	return SDL_clamp(square_z, 0, NAVIGATION_CAPSULE_SQUARES - 1) * NAVIGATION_CAPSULE_SQUARES
		+ SDL_clamp(square_x, 0, NAVIGATION_CAPSULE_SQUARES - 1);
}

// A region's capsules put in the order of their squares
static void sort_region_capsules(const navigation_t *navigation, int32_t region_index)
{
	navigation_region_t *region = &navigation->regions[region_index];
	const int squares = NAVIGATION_CAPSULE_SQUARES * NAVIGATION_CAPSULE_SQUARES;
	navigation_capsule_t *sorted;
	uint16_t next[NAVIGATION_CAPSULE_SQUARES * NAVIGATION_CAPSULE_SQUARES];

	if (region->capsule_count == 0)
	{
		SDL_free(region->capsule_first);
		region->capsule_first = NULL;
		return;
	}
	// (the squares' firsts are 16 bits)
	region->capsule_count = SDL_min(region->capsule_count, 65535);
	sorted = SDL_malloc(sizeof(navigation_capsule_t) * (size_t)region->capsule_count);
	if (region->capsule_first == NULL)
	{
		region->capsule_first = SDL_malloc(sizeof(uint16_t) * (size_t)(squares + 1));
	}
	if (sorted == NULL || region->capsule_first == NULL)
	{
		SDL_free(sorted);
		SDL_free(region->capsule_first);
		region->capsule_first = NULL;
		region->capsule_count = 0;
		return;
	}
	SDL_memset(region->capsule_first, 0, sizeof(uint16_t) * (size_t)(squares + 1));
	for (int32_t k = 0; k < region->capsule_count; k++)
	{
		region->capsule_first[capsule_square(navigation, region_index, region->capsules[k].a[0], region->capsules[k].a[2]) + 1]++;
	}
	for (int square = 0; square < squares; square++)
	{
		region->capsule_first[square + 1] = (uint16_t)(region->capsule_first[square + 1] + region->capsule_first[square]);
		next[square] = region->capsule_first[square];
	}
	for (int32_t k = 0; k < region->capsule_count; k++)
	{
		sorted[next[capsule_square(navigation, region_index, region->capsules[k].a[0], region->capsules[k].a[2])]++] = region->capsules[k];
	}
	SDL_memcpy(region->capsules, sorted, sizeof(navigation_capsule_t) * (size_t)region->capsule_count);
	SDL_free(sorted);
}

// A small thing's capsule kept with a region
static void append_region_capsule(navigation_region_t *region, const navigation_capsule_t *capsule)
{
	if (region->capsule_count == region->capsule_capacity)
	{
		int32_t capacity = SDL_max(2 * region->capsule_capacity, 16);
		navigation_capsule_t *grown = SDL_realloc(region->capsules, sizeof(navigation_capsule_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		region->capsules = grown;
		region->capsule_capacity = capacity;
	}
	region->capsules[region->capsule_count++] = *capsule;
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
		steps = clamp_int((int)SDL_ceilf(longest / SAMPLE_METERS), 1, 256);
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
				band = clamp_int((int32_t)SDL_floorf((y - base_y) / BAND_METERS), 0, BANDS - 1);
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
			navigation->small_only[cell] = 0;
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
	// (a thread's own: the regions are built side by side)
	static _Thread_local surface_candidate_t candidates[4096];

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
		// The ground's own quarters, where something stands on or over
		// only part of the cell: a top like any other, at the ground's
		// height, each quarter with its own room (the cell's ground as a
		// whole was taken by anything over it anywhere: the 75 cm beside
		// a porch's end were no one's)
		if (navigation->terrain_clearance[cell] != NAVIGATION_CLEARANCE_OPEN)
		{
			float room[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
			int free_count = 0;
			bool alike = true;

			for (int sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
			{
				room[sub] = 1e30f;
			}
			for (int32_t k = start; k < end; k++)
			{
				const object_cell_t *item = &list->items[k];

				if (item->top > terrain_height + MERGE_METERS)
				{
					room[item->sub] = SDL_min(room[item->sub], item->bottom - terrain_height);
				}
			}
			for (int sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
			{
				free_count += room[sub] >= SURFACE_LEAST_ROOM_METERS ? 1 : 0;
				alike = alike && room[sub] >= SURFACE_LEAST_ROOM_METERS
					&& (room[sub] >= 1e29f ? NAVIGATION_CLEARANCE_OPEN : clearance_units(room[sub])) == navigation->terrain_clearance[cell];
			}
			// (the same room over all of it, a roof: the cell's own says it)
			if (!alike && free_count >= MINIMUM_SURFACE_SUBCELLS)
			{
				for (int sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS && candidate_count < (int)SDL_arraysize(candidates); sub++)
				{
					if (room[sub] >= SURFACE_LEAST_ROOM_METERS)
					{
						candidates[candidate_count].height = terrain_height;
						candidates[candidate_count].clearance = room[sub] >= 1e29f ? NAVIGATION_CLEARANCE_OPEN : clearance_units(room[sub]);
						candidates[candidate_count].sub = (uint8_t)sub;
						candidate_count++;
					}
				}
				navigation->terrain_clearance[cell] = 0;
			}
		}
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
				// (as many as it has, not the whole block at its first: a
				// region with one tree took 180 KB)
				if (region->surface_count == region->surface_capacity)
				{
					int32_t capacity = SDL_min(SDL_max(64, region->surface_capacity * 2), NAVIGATION_REGION_EXTRA_SLOTS);
					navigation_surface_t *grown = SDL_realloc(region->surfaces, sizeof(navigation_surface_t) * (size_t)capacity);

					if (grown == NULL)
					{
						break;
					}
					region->surfaces = grown;
					region->surface_capacity = capacity;
				}
				SDL_qsort(clearances, (size_t)covered, 1, compare_clearances);
				surface = &region->surfaces[region->surface_count];
				surface->height = height;
				surface->cell = cell;
				surface->clearance = clearances[MINIMUM_SURFACE_SUBCELLS - 1];
				surface->ground_below = NAVIGATION_SUBCELL_NONE;
				for (int sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
				{
					// (the nearest unit: a tread 0.34 under its terrace is 0.34
					// under it, not 0.36 by a hair's rounding: a step of 0.35
					// takes it)
					surface->below[sub] = sub_height[sub] < -1e29f ? NAVIGATION_SUBCELL_NONE
						: (uint8_t)SDL_clamp(SDL_floorf((height - sub_height[sub]) / NAVIGATION_SUBCELL_UNIT_METERS + 0.5f), 0.0f,
							(float)(NAVIGATION_SUBCELL_BLOCKED - 1));
					surface->room[sub] = sub_height[sub] < -1e29f ? 0 : sub_clearance[sub];
					if (sub_height[sub] > -1e29f && sub_height[sub] <= terrain_height + 1e-4f)
					{
						surface->ground_below = surface->below[sub];
					}
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
		int32_t grid_first_x = clamp_int((int32_t)SDL_floorf(((float)first_x * NAVIGATION_CELL_METERS - margin) / OBJECTS_GRID_CELL_METERS), 0,
			objects->grid_size - 1);
		int32_t grid_first_z = clamp_int((int32_t)SDL_floorf(((float)first_z * NAVIGATION_CELL_METERS - margin) / OBJECTS_GRID_CELL_METERS), 0,
			objects->grid_size - 1);
		int32_t grid_last_x = clamp_int((int32_t)SDL_floorf(((float)(last_x + 1) * NAVIGATION_CELL_METERS + margin) / OBJECTS_GRID_CELL_METERS), 0,
			objects->grid_size - 1);
		int32_t grid_last_z = clamp_int((int32_t)SDL_floorf(((float)(last_z + 1) * NAVIGATION_CELL_METERS + margin) / OBJECTS_GRID_CELL_METERS), 0,
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
static void keep_print(navigation_region_t *region, uint32_t id, int type, int model, const float fields[8], float reach_meters, uint8_t kind)
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
	region->prints[region->print_count].kind = kind;
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
		int walked_model;
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
		walked_model = navigation_model(navigation, objects, instance, &unused_radius);
		stood_on = walked_model >= 0;
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
			navigation_capsule_t capsules[NAVIGATION_MODEL_CAPSULES_MOST];
			int capsule_count;

			// (what it is to the walkers: the models' shapes are fitted
			// once and kept, navigation_t's own)
			keep_print(prints_out, instance->id, instance->type, model, fields, radius, (uint8_t)(stood_on
				? object_kind((navigation_t *)navigation, objects, instance, walked_model, capsules, &capsule_count) : KIND_NONE));
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

// Whether what changed among a region's objects since it was last built
// (now: as they are) leaves the rectangles as they are: only small things
// (gone round, soft) or ones that are nothing to the walkers came, went
// or moved; each one gone round with only open ground or other small
// things in the cells round it (not by a wall: there it is in the
// rectangles), and no other one near enough to be welded to it (a row of
// trunks is a wall: mesh_weld_small)
static bool changes_are_small(const navigation_t *navigation, int32_t region_index, const navigation_region_t *now)
{
	const navigation_region_t *before = &navigation->regions[region_index];
	float widest = 0.0f;
	int32_t a = 0;
	int32_t b = 0;

	for (int c = 0; c < NAVIGATION_MESH_CLASSES; c++)
	{
		widest = SDL_max(widest, SDL_min(navigation->mesh.classes[c].radius, 2.0f));
	}
	while (a < before->print_count || b < now->print_count)
	{
		const navigation_object_print_t *old_print = a < before->print_count ? &before->prints[a] : NULL;
		const navigation_object_print_t *new_print = b < now->print_count ? &now->prints[b] : NULL;
		const navigation_object_print_t *changed[2] = { NULL, NULL };

		if (new_print == NULL || (old_print != NULL && old_print->id < new_print->id))
		{
			changed[0] = old_print;
			a++;
		}
		else if (old_print == NULL || new_print->id < old_print->id)
		{
			changed[0] = new_print;
			b++;
		}
		else
		{
			if (SDL_memcmp(old_print->fields, new_print->fields, sizeof(old_print->fields)) != 0 || old_print->model != new_print->model)
			{
				changed[0] = old_print;
				changed[1] = new_print;
			}
			a++;
			b++;
		}
		for (int k = 0; k < 2 && changed[k] != NULL; k++)
		{
			const navigation_object_print_t *print = changed[k];
			int32_t box[4] = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
			float reach = print->reach_meters + 2.0f * MESH_WELD_SHARE * widest + 0.05f;
			int32_t first_region_x = clamp_int((int32_t)SDL_floorf((print->fields[0] - reach - 2.0f) / NAVIGATION_REGION_METERS), 0,
				navigation->regions_per_side - 1);
			int32_t first_region_z = clamp_int((int32_t)SDL_floorf((print->fields[1] - reach - 2.0f) / NAVIGATION_REGION_METERS), 0,
				navigation->regions_per_side - 1);
			int32_t last_region_x = clamp_int((int32_t)SDL_floorf((print->fields[0] + reach + 2.0f) / NAVIGATION_REGION_METERS), 0,
				navigation->regions_per_side - 1);
			int32_t last_region_z = clamp_int((int32_t)SDL_floorf((print->fields[1] + reach + 2.0f) / NAVIGATION_REGION_METERS), 0,
				navigation->regions_per_side - 1);

			if (print->kind == KIND_WALL)
			{
				return false;
			}
			if (print->kind != KIND_ROUND)
			{
				continue;
			}
			grow_box_by_print(navigation, print, box);
			for (int32_t cell_z = SDL_max(box[1], 0); cell_z <= SDL_min(box[3], navigation->size - 1); cell_z++)
			{
				for (int32_t cell_x = SDL_max(box[0], 0); cell_x <= SDL_min(box[2], navigation->size - 1); cell_x++)
				{
					int32_t cell = cell_z * navigation->size + cell_x;

					if (navigation->small_only[cell] == 0 && navigation->terrain_clearance[cell] != NAVIGATION_CLEARANCE_OPEN)
					{
						return false;
					}
				}
			}
			for (int32_t region_z = first_region_z; region_z <= last_region_z; region_z++)
			{
				for (int32_t region_x = first_region_x; region_x <= last_region_x; region_x++)
				{
					const navigation_region_t *region = &navigation->regions[region_z * navigation->regions_per_side + region_x];

					for (int32_t n = 0; n < region->capsule_count; n++)
					{
						const navigation_capsule_t *capsule = &region->capsules[n];
						float dx = capsule->b[0] - capsule->a[0];
						float dz = capsule->b[2] - capsule->a[2];
						float length_squared = dx * dx + dz * dz;
						float along = length_squared > 1e-8f ? SDL_clamp(((print->fields[0] - capsule->a[0]) * dx
							+ (print->fields[1] - capsule->a[2]) * dz) / length_squared, 0.0f, 1.0f) : 0.0f;
						float off_x = capsule->a[0] + dx * along - print->fields[0];
						float off_z = capsule->a[2] + dz * along - print->fields[1];
						float near = reach + capsule->radius;

						if (capsule->soft == 0 && capsule->id != print->id && off_x * off_x + off_z * off_z < near * near)
						{
							return false;
						}
					}
				}
			}
		}
	}
	return true;
}

// One region's objects into the cells of box (in it, cleared): the room
// they take, their surfaces, the small ones' heights, the flyers' tops
static void build_box(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects, int32_t region_index, const int32_t box[4],
	object_scratch_t *scratch)
{
	navigation_region_t *region = &navigation->regions[region_index];
	object_cells_t list = { NULL, 0, 0 };
	object_cells_t round_cells = { NULL, 0, 0 };
	object_cells_t soft_cells = { NULL, 0, 0 };
	int32_t count = objects != NULL ? gather_region_objects(navigation, objects, region_index, scratch) : 0;

	// (the capsules of all its small things again, whatever the box)
	region->capsule_count = 0;
	for (int32_t object = 0; object < count; object++)
	{
		const object_instance_t *instance = &objects->instances[scratch->items[object]];
		navigation_capsule_t capsules[NAVIGATION_MODEL_CAPSULES_MOST];
		int capsule_count;
		float radius;
		int model;
		int kind;

		raise_object_tops(navigation, objects, instance, box);
		model = navigation_model(navigation, objects, instance, &radius);
		if (model < 0)
		{
			continue;
		}
		kind = object_kind(navigation, objects, instance, model, capsules, &capsule_count);
		for (int k = 0; k < capsule_count; k++)
		{
			append_region_capsule(region, &capsules[k]);
		}
		if (!object_reaches_box(instance, radius, box))
		{
			continue;
		}
		rasterize_object(navigation, objects, instance, model,
			SDL_max(box[0], (int32_t)SDL_floorf((instance->x - radius) / NAVIGATION_CELL_METERS)),
			SDL_max(box[1], (int32_t)SDL_floorf((instance->z - radius) / NAVIGATION_CELL_METERS)),
			SDL_min(box[2], (int32_t)SDL_floorf((instance->x + radius) / NAVIGATION_CELL_METERS)),
			SDL_min(box[3], (int32_t)SDL_floorf((instance->z + radius) / NAVIGATION_CELL_METERS)),
			kind == KIND_WALL ? &list : kind == KIND_ROUND ? &round_cells : &soft_cells);
		if (kind == KIND_SOFT)
		{
			// Soft things are no surface and take no room: each cell
			// keeps the height of the highest one standing in it
			for (int32_t item_index = 0; item_index < soft_cells.count; item_index++)
			{
				const object_cell_t *item = &soft_cells.items[item_index];
				float above = item->top - navigation->ground_height[item->cell];
				uint8_t units = (uint8_t)SDL_clamp(SDL_ceilf(above / SMALL_UNIT_METERS), 0.0f, 255.0f);

				navigation->small_height[item->cell] = SDL_max(navigation->small_height[item->cell], units);
			}
			soft_cells.count = 0;
		}
	}
	sort_region_capsules(navigation, region_index);
	// The cells only small things stand in: open ground to the rectangles,
	// if they are to be left out of those (small_out); a wall there too,
	// a barrel on a porch: as the surfaces have it. The surfaces keep
	// them all
	for (int32_t item_index = 0; item_index < round_cells.count && navigation->small_out; item_index++)
	{
		navigation->small_only[round_cells.items[item_index].cell] = 1;
	}
	for (int32_t item_index = 0; item_index < list.count; item_index++)
	{
		navigation->small_only[list.items[item_index].cell] = 0;
	}
	for (int32_t item_index = 0; item_index < round_cells.count; item_index++)
	{
		append_object_cell(&list, &round_cells.items[item_index]);
	}
	finish_region(navigation, terrain, region_index, &list);
	SDL_free(list.items);
	SDL_free(round_cells.items);
	SDL_free(soft_cells.items);
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
		// (only small things changed: its tiles that were current still
		// are; the walkers' ways through it are looked at again)
		for (int c = 0; c < NAVIGATION_MESH_CLASSES && region->small_change; c++)
		{
			navigation_mesh_tile_t *tiles = navigation->mesh.classes[c].tiles;

			if (tiles != NULL && tiles[r].built && tiles[r].generation == region->small_change_from)
			{
				tiles[r].generation = navigation->generation;
			}
		}
		region->small_change = false;
		SDL_free(scratch.items);
	}
	for (int32_t k = 0; k < list_count; k++)
	{
		navigation->regions[region_list[k]].dirty = false;
	}
	navigation->last_rebuilt_regions = list_count;
	frame_profile_count(FRAME_NAVIGATION_REGIONS, list_count);
	frame_profile_count(FRAME_NAVIGATION_BUILD_MICROSECONDS,
		(int32_t)((double)(SDL_GetPerformanceCounter() - start) * 1000000.0 / (double)SDL_GetPerformanceFrequency()));
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
					// (small things only, none by a wall or by another: the
					// rectangles stay)
					region->small_change = region->stale && navigation->region_generation[r] != 0 && changes_are_small(navigation, r, &now);
					region->small_change_from = navigation->region_generation[r];
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

	box_out[0] = clamp_int((int32_t)SDL_floorf((blocker->x - reach_x) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
	box_out[1] = clamp_int((int32_t)SDL_floorf((blocker->z - reach_z) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
	box_out[2] = clamp_int((int32_t)SDL_floorf((blocker->x + reach_x) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
	box_out[3] = clamp_int((int32_t)SDL_floorf((blocker->z + reach_z) / NAVIGATION_CELL_METERS), 0, navigation->size - 1);
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
				navigation->blocker_zone[z * navigation->size + x] = 1;
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

// A region whose blockers' groups changed and nothing else (a door
// opened or shut: what its doorway stops; the doorway is where it was):
// stamped newer (the paths through it are looked at again), its tiles
// that were current still are (their rectangles ask the cells as they
// are: NAVIGATION_RECT_ZONE)
static void region_state_changed(navigation_t *navigation, int32_t region)
{
	uint32_t before = navigation->region_generation[region];

	navigation->generation++;
	navigation->region_generation[region] = navigation->generation;
	for (int c = 0; c < NAVIGATION_MESH_CLASSES; c++)
	{
		navigation_mesh_tile_t *tiles = navigation->mesh.classes[c].tiles;

		if (tiles != NULL && tiles[region].built && tiles[region].generation == before)
		{
			tiles[region].generation = navigation->generation;
		}
	}
}

// The doorways of the doors: the cells between each link's two ends (not
// their own). Of the shut ones stamped with NAVIGATION_GROUP_SHUT_DOOR
// (set) or cleared, their regions stamped newer when anything changed
// (state_only: the doorways are where they were, the tiles stay);
// zone: all the doors' doorways marked in blocker_zone instead
static void stamp_doors(navigation_t *navigation, bool set, bool state_only, bool zone)
{
	for (int k = 0; k < navigation->link_count && navigation->blocked != NULL; k++)
	{
		const navigation_link_t *link = &navigation->links[k];
		float dx = link->b[0] - link->a[0];
		float dz = link->b[1] - link->a[1];
		float length = SDL_sqrtf(dx * dx + dz * dz);
		int steps = SDL_max(1, (int)SDL_ceilf(length / 0.25f));

		if (link->open && set && !zone)
		{
			continue;
		}
		for (int step = 1; step < steps; step++)
		{
			int32_t cell;
			float amount = (float)step / (float)steps;
			bool changed = false;

			if (!cell_of_point(navigation, link->a[0] + dx * amount, link->a[1] + dz * amount, &cell) || cell == navigation->link_cells[k][0]
				|| cell == navigation->link_cells[k][1])
			{
				continue;
			}
			if (zone)
			{
				navigation->blocker_zone[cell] = 1;
				continue;
			}
			if (set && (navigation->blocked[cell] & NAVIGATION_GROUP_SHUT_DOOR) == 0)
			{
				navigation->blocked[cell] |= NAVIGATION_GROUP_SHUT_DOOR;
				changed = true;
			}
			else if (!set && (navigation->blocked[cell] & NAVIGATION_GROUP_SHUT_DOOR) != 0)
			{
				navigation->blocked[cell] &= (uint8_t)~NAVIGATION_GROUP_SHUT_DOOR;
				changed = true;
			}
			if (changed && state_only)
			{
				region_state_changed(navigation, region_of_cell(navigation, cell));
			}
			else if (changed)
			{
				navigation->generation++;
				navigation->region_generation[region_of_cell(navigation, cell)] = navigation->generation;
			}
		}
	}
}

// The doorways' regions stamped newer (a door new, gone or moved: their
// tiles are built again)
static void touch_doors(navigation_t *navigation)
{
	for (int k = 0; k < navigation->link_count; k++)
	{
		const navigation_link_t *link = &navigation->links[k];
		float dx = link->b[0] - link->a[0];
		float dz = link->b[1] - link->a[1];
		int steps = SDL_max(1, (int)SDL_ceilf(SDL_sqrtf(dx * dx + dz * dz) / 0.25f));

		for (int step = 1; step < steps; step++)
		{
			int32_t cell;
			float amount = (float)step / (float)steps;

			if (cell_of_point(navigation, link->a[0] + dx * amount, link->a[1] + dz * amount, &cell))
			{
				navigation->generation++;
				navigation->region_generation[region_of_cell(navigation, cell)] = navigation->generation;
			}
		}
	}
}

// blocker_zone made again: under the blockers, in the doors' doorways
static void stamp_zone(navigation_t *navigation)
{
	SDL_memset(navigation->blocker_zone, 0, (size_t)navigation->size * (size_t)navigation->size);
	for (int i = 0; i < navigation->blocker_count; i++)
	{
		navigation_blocker_t zone = navigation->blockers[i];

		// (its cells marked; its groups are blocked's)
		zone.groups = 0;
		stamp_blocker(navigation, &zone);
	}
	stamp_doors(navigation, true, false, true);
}

void navigation_set_links(navigation_t *navigation, const navigation_link_t *links, int count)
{
	// (the doorways where they were, only doors opened or shut: no tile
	// is built again)
	bool same_doorways;

	// (the doors as they were: their doorways cleared, the new ones stamped
	// after; nothing changed, nothing done)
	{
		bool same = count == navigation->links_given_count;

		count = SDL_clamp(count, 0, NAVIGATION_LINKS_MAX);
		same_doorways = same;
		for (int k = 0; k < count && same_doorways; k++)
		{
			same_doorways = SDL_memcmp(links[k].a, navigation->links_given[k].a, sizeof(links[k].a)) == 0
				&& SDL_memcmp(links[k].b, navigation->links_given[k].b, sizeof(links[k].b)) == 0;
			same = same && links[k].open == navigation->links_given[k].open;
		}
		same = same && same_doorways;
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
	stamp_doors(navigation, false, same_doorways, false);
	if (!same_doorways)
	{
		touch_doors(navigation);
	}
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
	stamp_doors(navigation, true, same_doorways, false);
	if (!same_doorways)
	{
		touch_doors(navigation);
		stamp_zone(navigation);
	}
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
	stamp_doors(navigation, true, false, false);
	stamp_zone(navigation);
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
		SDL_free(navigation->regions[r].capsules);
		SDL_free(navigation->regions[r].capsule_first);
	}
	SDL_free(navigation->regions);
	SDL_free(navigation->ground_height);
	SDL_free(navigation->terrain_clearance);
	SDL_free(navigation->terrain_blocker);
	SDL_free(navigation->object_top);
	SDL_free(navigation->ground_slope);
	SDL_free(navigation->small_only);
	SDL_free(navigation->model_shapes);
	SDL_free(navigation->small_height);
	SDL_free(navigation->blocked);
	navigation->blocked = NULL;
	SDL_free(navigation->blocker_zone);
	navigation->blocker_zone = NULL;
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
	navigation->small_only = NULL;
	navigation->model_shapes = NULL;
	navigation->model_shape_count = 0;
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
	navigation->small_only = SDL_calloc(cells, 1);
	navigation->small_height = SDL_calloc(cells, 1);
	navigation->blocked = SDL_calloc(cells, 1);
	navigation->blocker_zone = SDL_calloc(cells, 1);
	navigation->blocker_count = 0;
	navigation->extra_count = SDL_calloc(cells, 1);
	navigation->extra_first = SDL_calloc(cells, sizeof(uint16_t));
	navigation->region_generation = SDL_calloc((size_t)region_count, sizeof(uint32_t));
	navigation->build_list = SDL_calloc((size_t)region_count, sizeof(int32_t));
	if (navigation->build_list == NULL || navigation->regions == NULL || navigation->ground_height == NULL || navigation->terrain_clearance == NULL
		|| navigation->terrain_blocker == NULL || navigation->object_top == NULL || navigation->ground_slope == NULL || navigation->small_height == NULL || navigation->small_only == NULL || navigation->blocked == NULL
		|| navigation->blocker_zone == NULL
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
	navigation->wall_reach_meters = 1.2f;
	navigation->soft_kilograms = 20.0f;
	navigation->small_out = true;
	navigation->follow_mesh = true;
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

	// (the ground's own quarter: the ground as it lies there, not its
	// cell's one height)
	if (below == surface->ground_below && below < NAVIGATION_SUBCELL_BLOCKED && navigation->terrain != NULL)
	{
		return terrain_sample_height_meters(navigation->terrain, x, z);
	}
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

	// (by the rectangles: the level there, or the nearest by it; none
	// built there (a wanderer where no way was ever searched): the
	// terrain, and the physics' rays where anyone looks)
	if (navigation->follow_mesh)
	{
		mesh_follow_height(navigation, terrain, agent, x, z, near_height, &best, &on_object);
		if (object_out != NULL)
		{
			*object_out = on_object;
		}
		return best;
	}
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

	if (navigation->follow_mesh)
	{
		return mesh_follow_level(navigation, terrain, water, agent, x, z, near_height, MESH_FOLLOW_SLACK_METERS, NULL, NULL);
	}
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
static bool quarter_stand_on(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t cell, int32_t sub_x, int32_t sub_z, float near_height, float *height_out, bool *ground_out)
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
				*ground_out = true;
				return true;
			}
			continue;
		}
		if (node_sub_height(navigation, nodes[k], cell, sub_x, sub_z, room_needed, &height, &blocked))
		{
			// (the ground's own quarter of a cell partly taken: the ground,
			// as it slopes)
			bool ground = extra_surface(navigation, nodes[k])->below[sub_z * NAVIGATION_SUBCELLS + sub_x]
				== extra_surface(navigation, nodes[k])->ground_below;

			// (an object's top: a step up onto it, no slope; higher than a
			// step over it: its side, in the way, a terrace's face; lower:
			// a drop the path never takes)
			if (SDL_fabsf(height - near_height) <= (ground ? reach : agent->step_meters + 0.05f))
			{
				*height_out = height;
				*ground_out = ground;
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
	*ground_out = true;
	return !in_the_way && SDL_fabsf(navigation->ground_height[cell] - near_height) <= agent->step_meters + 0.05f;
}

static bool quarter_stand(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int32_t cell, int32_t sub_x, int32_t sub_z, float near_height, float *height_out)
{
	bool ground;

	return quarter_stand_on(navigation, terrain, water, agent, cell, sub_x, sub_z, near_height, height_out, &ground);
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

	if (navigation->follow_mesh)
	{
		return mesh_follow_level(navigation, terrain, water, agent, x, z, near_height, MESH_FOLLOW_SLACK_METERS, NULL, NULL);
	}
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
	bool ground;
	float reach = SDL_max(agent->step_meters, agent->maximum_slope * NAVIGATION_CELL_METERS);

	if (!quarter_of_point(navigation, x, z, &cell, &sub_x, &sub_z)
		|| !quarter_stand_on(navigation, terrain, water, agent, cell, sub_x, sub_z, near_height, &height, &ground))
	{
		return -1;
	}
	for (int k = 0; k < BODY_POINTS; k++)
	{
		float angle = (float)k * (6.2831853f / (float)BODY_POINTS);
		float point_x = x + SDL_sinf(angle) * agent->radius_meters * BODY_REACH;
		float point_z = z + SDL_cosf(angle) * agent->radius_meters * BODY_REACH;
		float point_height;
		bool point_ground;

		// (a step off where its middle is; the ground from the ground: as
		// it slopes)
		if (!quarter_of_point(navigation, point_x, point_z, &cell, &sub_x, &sub_z)
			|| !quarter_stand_on(navigation, terrain, water, agent, cell, sub_x, sub_z, height, &point_height, &point_ground)
			|| SDL_fabsf(point_height - height) > (ground && point_ground ? reach : agent->step_meters + 0.05f))
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

	// (on the rectangles a body has all its room, by how they are made;
	// off them it has none that is known: as tight as it gets, but not
	// "no standing there": a walker off them is let walk on, back to them)
	if (navigation->follow_mesh)
	{
		return mesh_follow_level(navigation, terrain, water, agent, x, z, near_height, MESH_FOLLOW_SLACK_METERS, NULL, NULL) ? 0 : BODY_POINTS;
	}
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

	// (over the rectangles: where a body's middle goes with all its room)
	if (navigation->follow_mesh)
	{
		return navigation_line_passable(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, NULL, NULL, NULL);
	}
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

	// (no way over the quarter meters: there are none to the follower; it
	// searches again over the rectangles)
	if (navigation->follow_mesh)
	{
		return 0;
	}
	return fine_path_search(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, points_out, most, &end_height);
}

bool navigation_line_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float from_x, float from_z, float from_height, float to_x, float to_z, float *blocked_x,
	float *blocked_z, float *blocked_height)
{
	// Over the rectangles: they are built as needed (the navigation is only
	// read, but its mesh kept up); from where they have nothing (a walker
	// pushed off them), the cells sampled along the line
	return mesh_line_passable((navigation_t *)navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, blocked_x,
		blocked_z, blocked_height) == 1;
}

// ---------------------------------------------------------------------------
// A*

// The flyers' search's nodes: blocks of FLIGHT_BLOCK_CELLS cells a side
// (the walkers' is over the mesh's rectangles, its state in them)
static int32_t node_count(const navigation_t *navigation)
{
	int32_t blocks = (navigation->size + FLIGHT_BLOCK_CELLS - 1) / FLIGHT_BLOCK_CELLS;

	return blocks * blocks;
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

// Octile distance: what the trip costs at best (dry, flat)
static float estimate(int32_t from_x, int32_t from_z, int32_t to_x, int32_t to_z)
{
	int32_t dx = to_x > from_x ? to_x - from_x : from_x - to_x;
	int32_t dz = to_z > from_z ? to_z - from_z : from_z - to_z;

	return ((float)SDL_max(dx, dz) + 0.41421356f * (float)SDL_min(dx, dz)) * NAVIGATION_CELL_METERS;
}

navigation_result_t navigation_find_path(navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float from_x, float from_z, float from_height, float to_x, float to_z,
	float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2], int *waypoint_count_out, bool *complete_out)
{
	float goal_distance = SDL_sqrtf((to_x - from_x) * (to_x - from_x) + (to_z - from_z) * (to_z - from_z));
	int32_t budget = (int32_t)SDL_min((float)navigation->maximum_expansions,
		(float)SEARCH_BASE_EXPANSIONS + (float)SEARCH_EXPANSIONS_PER_METER * goal_distance);
	int result;

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
	// Over the rectangles. Nothing of them anywhere near the start (in the
	// middle of a rock, of deep water it does not swim): no way
	result = mesh_find_path(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, waypoints_out, waypoint_count_out,
		complete_out);
	if (result < 0)
	{
		*waypoint_count_out = 0;
		*complete_out = true;
		return NAVIGATION_PATH_NONE;
	}
	return (navigation_result_t)result;
}

// Whether a walker that keeps to the rectangles may make a step of its
// middle: from off them (pushed there, started there) any, back to them;
// else its end on them, not deeper into a small thing than its start, and
// the line there over them by the agent's rules (a step up it can take, a
// door open), the line's ends moved together up to the rectangles' slack
// any way (a way's corner pushed off a small thing's berth, or kept of
// two a hair apart, leaves its stretch that far off them)
bool navigation_step_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float height, float to_x, float to_z)
{
	static const int8_t offsets[9][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
	navigation_agent_t center = *agent;

	center.line_center_only = true;
	if (!mesh_follow_level(navigation, terrain, water, agent, from_x, from_z, height, MESH_FOLLOW_SLACK_METERS, NULL, NULL))
	{
		return true;
	}
	if (!mesh_follow_level(navigation, terrain, water, agent, to_x, to_z, height, MESH_FOLLOW_SLACK_METERS, NULL, NULL)
		|| navigation_small_blocks_step(navigation, agent, from_x, from_z, to_x, to_z, NULL, NULL))
	{
		return false;
	}
	for (int k = 0; k < 9; k++)
	{
		float off_x = (float)offsets[k][0] * MESH_FOLLOW_SLACK_METERS;
		float off_z = (float)offsets[k][1] * MESH_FOLLOW_SLACK_METERS;

		if (mesh_line((navigation_t *)navigation, terrain, water, &center, from_x + off_x, from_z + off_z, height, to_x + off_x, to_z + off_z, NULL,
			NULL, NULL) == 1)
		{
			return true;
		}
	}
	return false;
}

bool navigation_follows_mesh(const navigation_t *navigation)
{
	return navigation->follow_mesh;
}

bool navigation_objects_near(const navigation_t *navigation, float x, float z)
{
	int32_t cell_x = (int32_t)SDL_floorf(x / NAVIGATION_CELL_METERS);
	int32_t cell_z = (int32_t)SDL_floorf(z / NAVIGATION_CELL_METERS);

	if (navigation->size <= 0 || cell_x < 1 || cell_z < 1 || cell_x >= navigation->size - 1 || cell_z >= navigation->size - 1)
	{
		return true;
	}
	for (int32_t dz = -1; dz <= 1; dz++)
	{
		for (int32_t dx = -1; dx <= 1; dx++)
		{
			int32_t cell = (cell_z + dz) * navigation->size + cell_x + dx;

			// (the cells' highest object: kept for the whole map, whatever
			// becomes of the cells' surfaces)
			if (navigation->object_top[cell] != 0 || navigation->regions[region_of_cell(navigation, cell)].stale)
			{
				return true;
			}
		}
	}
	return false;
}

bool navigation_detail_near(const navigation_t *navigation, float x, float z)
{
	float dx = x - navigation->detail_x;
	float dz = z - navigation->detail_z;

	return navigation->detail_meters <= 0.0f || dx * dx + dz * dz <= navigation->detail_meters * navigation->detail_meters;
}

bool navigation_path_small(const navigation_t *navigation, int waypoint)
{
	return waypoint >= 0 && waypoint < NAVIGATION_MAX_WAYPOINTS && navigation->path_small[waypoint] != 0;
}

void navigation_set_detail(navigation_t *navigation, float x, float z, float meters)
{
	navigation->detail_x = x;
	navigation->detail_z = z;
	navigation->detail_meters = meters;
}

bool navigation_path_jump(const navigation_t *navigation, int waypoint, float *height_out)
{
	if (waypoint < 0 || waypoint >= NAVIGATION_MAX_WAYPOINTS || navigation->path_jumps[waypoint] == 0)
	{
		return false;
	}
	*height_out = navigation->path_jump_heights[waypoint];
	return true;
}

// ---------------------------------------------------------------------------
// The navigation mesh (see navigation.h)

#define MESH_PORTAL_RISE_MOST 2.0f     // a step higher than this between two rectangles: no portal
#define MESH_WATER_CHECK_MS 250u       // a tile's water's cells compared again at most this often
#define MESH_EXPANSIONS_DIVISOR 8      // a rectangle search's budget: the cells' over this
#define MESH_MINIMUM_EXPANSIONS 2048
#define MESH_RAY_MOST_STEPS 512
#define MESH_START_NEAR_QUARTERS 8     // a start on no rectangle: the nearest one this far round
#define MESH_START_FAR_QUARTERS 32     // none: this far round (the way back onto them)
#define MESH_GOAL_NEAR_QUARTERS 3      // a goal on no rectangle (by a wall, where a body's middle cannot be): the nearest quarter this far round is the goal
#define MESH_GOAL_MORE_EXPANSIONS 32   // got to one of those: so many more rectangles looked at for a nearer one
#define MESH_TILE_CELLS_MOST 4096      // a region's cells at most (64 x 64)

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

// An agent seen: its class takes its reach, height, step and width in
// (the least reach and height, the least step and the highest, the
// widest); a change builds the class's tiles again (lazily)
static void mesh_note_agent(navigation_t *navigation, const navigation_agent_t *agent)
{
	navigation_mesh_class_t *class = &navigation->mesh.classes[mesh_class_of(agent)];
	float reach = agent_reach(agent);
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
	if (agent->step_meters > class->step_most)
	{
		class->step_most = agent->step_meters;
		changed = true;
	}
	if (agent->radius_meters > class->radius)
	{
		class->radius = agent->radius_meters;
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
	SDL_free(tile->neighbors);
	for (int side = 0; side < 4; side++)
	{
		SDL_free(tile->border[side]);
	}
	SDL_free(tile->bucket_rects);
	SDL_free(tile->uneven_rects);
	SDL_free(tile->uneven_first);
	SDL_free(tile->uneven_heights);
	SDL_free(tile->fringe);
	SDL_free(tile->fringe_row_first);
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
	SDL_free(navigation->mesh.visits);
	navigation->mesh.visits = NULL;
	navigation->mesh.visit_capacity = 0;
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
		class->step_most = 0.0f;
		class->radius = 0.0f;
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

// The room a class needs over a quarter meter (its lowest agent's
// height, NAVIGATION_CLEARANCE_UNIT_METERS each)
static uint8_t mesh_room_needed(const navigation_mesh_class_t *class)
{
	return (uint8_t)SDL_clamp(SDL_ceilf(SDL_min(class->height, 50.0f) / NAVIGATION_CLEARANCE_UNIT_METERS), 0.0f,
		(float)(NAVIGATION_CLEARANCE_OPEN - 1));
}

// The mesh is built from the quarter meters: each of a cell's has up to
// MESH_LEVELS levels one can be on (the ground, the cell's objects'
// tops). A level of a quarter is a rectangle's where a body of the class
// can have its middle there: it stands there, and so does every point
// round it (the points the walkers' own guard measures a body by,
// quarter_tightness; the guard lets a brush or two by, a way is not
// planned through one). The rectangles are the space of the bodies'
// middles: a way inside them is one the body fits.
//
// A body an odd number of quarters wide has its middle in the middle of
// a quarter meter, one an even number wide where four of them meet (a
// man two quarters wide in a doorway two quarters wide: on the line
// between them): an even class's quarter is the one whose low corner the
// middle is at, and its rectangles lie half a quarter low of the quarter
// meters (mesh_offset)
#define MESH_LEVELS (1 + NAVIGATION_MAX_EXTRA_SURFACES)
#define MESH_TILE_QUARTERS_MOST (64 * NAVIGATION_SUBCELLS)   // (a region: 64 cells a side)
#define MESH_MARGIN_QUARTERS NAVIGATION_SUBCELLS    // a cell more round the tile: where the bodies' points reach
#define MESH_SPAN_MOST (MESH_TILE_QUARTERS_MOST + 2 * MESH_MARGIN_QUARTERS)
#define MESH_LEVEL_STAND 1             // a body's middle stands here
#define MESH_LEVEL_POINT 2             // a point of a body does (also the ground where its cell's is taken but nothing is in the way there)
#define MESH_LEVEL_GROUND 4            // the ground (a slope's reach from the next; an object's top: a step)
#define MESH_LEVEL_SURFACE 8           // a surface is there, with the class's room over it or not
#define MESH_KEY_FIT 0x80000000u       // a rectangle's (a key of 0: not)
#define MESH_KEY_WET 0x40000000u
#define MESH_KEY_TOP 0x20000000u
#define MESH_KEY_ZONE 0x100u           // a cell under a blocker or in a doorway: its own rectangles
#define MESH_JUMP_GAP_QUARTERS 4       // an edge to jump: this far across what is neither rectangle's, at most

// The build's working state for one tile: its quarter meters and the
// margin's, row by row (span of them across)
typedef struct
{
	int32_t first_x;                   // the tile's first cell
	int32_t first_z;
	int32_t width;                     // its cells
	int32_t depth;
	int32_t span;
	int32_t region;
	bool wet[MESH_TILE_CELLS_MOST];
	float height[MESH_LEVELS][MESH_SPAN_MOST * MESH_SPAN_MOST];
	uint8_t flags[MESH_LEVELS][MESH_SPAN_MOST * MESH_SPAN_MOST];
	uint8_t room[MESH_LEVELS][MESH_SPAN_MOST * MESH_SPAN_MOST];
	uint8_t fit[MESH_SPAN_MOST * MESH_SPAN_MOST];                   // which levels are a rectangle's (a bit each)
	bool plain[(64 + 2) * (64 + 2)];                                // per cell, the margin's too: open ground (mesh_cell_plain)
	uint32_t key[MESH_LEVELS][MESH_SPAN_MOST * MESH_SPAN_MOST];     // what a rectangle's quarters share (0: not one's)
	int16_t rect[MESH_LEVELS][MESH_SPAN_MOST * MESH_SPAN_MOST];     // the rectangle it is in (-1: none yet)
} mesh_build_t;

// A cell's quarter meters' levels to a class, into the working state
// (the cell inside the map, and within the tile or its margin; the
// state's flags cleared before): as quarter_stand has them for an agent
static void mesh_build_cell(const navigation_t *navigation, const navigation_mesh_class_t *class, mesh_build_t *build, int32_t cell_x,
	int32_t cell_z)
{
	int32_t cell = cell_z * navigation->size + cell_x;
	int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
	int count = cell_nodes(navigation, cell, nodes);
	uint8_t room_needed = mesh_room_needed(class);
	float reach = SDL_min(class->reach, 50.0f);
	float ground = navigation->ground_height[cell];
	uint8_t ground_room = navigation->terrain_clearance[cell];
	bool ground_roomy = ground_room == NAVIGATION_CLEARANCE_OPEN || ground_room >= room_needed;
	int32_t first = ((cell_z - build->first_z) * NAVIGATION_SUBCELLS + MESH_MARGIN_QUARTERS) * build->span
		+ (cell_x - build->first_x) * NAVIGATION_SUBCELLS + MESH_MARGIN_QUARTERS;

	// (the margin's, in a region not built yet: open ground as the terrain
	// lies; the tile is built again when the region is, mesh_margin_signature)
	if (navigation->regions[region_of_cell(navigation, cell)].stale)
	{
		count = 1;
		ground = navigation->terrain != NULL ? terrain_sample_height_meters(navigation->terrain, ((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS,
			((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS) : ground;
		ground_room = NAVIGATION_CLEARANCE_OPEN;
		ground_roomy = true;
	}
	// (only small things in it, a trunk: the rectangles lie through them,
	// a way is led round them after)
	if (navigation->small_only[cell] != 0)
	{
		count = 1;
		ground_room = NAVIGATION_CLEARANCE_OPEN;
		ground_roomy = true;
	}
	for (int32_t sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
	{
		int32_t at = first + (sub / NAVIGATION_SUBCELLS) * build->span + sub % NAVIGATION_SUBCELLS;
		bool in_the_way = false;

		for (int k = 1; k < count; k++)
		{
			const navigation_surface_t *surface = extra_surface(navigation, nodes[k]);
			uint8_t below = surface->below[sub];

			build->flags[k][at] = 0;
			if (below < NAVIGATION_SUBCELL_BLOCKED && surface->room[sub] >= room_needed)
			{
				float height = surface->height - (float)below * NAVIGATION_SUBCELL_UNIT_METERS;

				build->height[k][at] = height;
				build->room[k][at] = surface->room[sub];
				build->flags[k][at] = MESH_LEVEL_STAND | MESH_LEVEL_POINT | MESH_LEVEL_SURFACE
					| (below == surface->ground_below ? MESH_LEVEL_GROUND : 0);
				// (over the ground, not far: its side is in the ground's way)
				in_the_way = in_the_way || (height > ground + SDL_min(class->step, 50.0f) + 0.05f
					&& height - ground < reach + NAVIGATION_OBJECT_HEIGHT_METERS);
			}
			else if (below != NAVIGATION_SUBCELL_NONE
				&& SDL_fabsf(surface->height - (below < NAVIGATION_SUBCELL_BLOCKED ? (float)below * NAVIGATION_SUBCELL_UNIT_METERS : 0.0f) - ground)
					<= reach)
			{
				// (something stands there at its height, or it has no room)
				in_the_way = true;
			}
			// (a surface with no room for the class: only that it is there)
			if (build->flags[k][at] == 0 && below < NAVIGATION_SUBCELL_BLOCKED)
			{
				build->height[k][at] = surface->height - (float)below * NAVIGATION_SUBCELL_UNIT_METERS;
				build->flags[k][at] = MESH_LEVEL_SURFACE | (below == surface->ground_below ? MESH_LEVEL_GROUND : 0);
			}
		}
		// (the ground: its cell's, where that has the room; where it is
		// taken, a body's point still rests on it if nothing is in the way
		// there: quarter_stand's last resort)
		build->height[0][at] = ground;
		build->room[0][at] = ground_room;
		build->flags[0][at] = (uint8_t)(MESH_LEVEL_GROUND | (ground_room != 0 ? MESH_LEVEL_SURFACE : 0)
			| (ground_roomy ? MESH_LEVEL_STAND | MESH_LEVEL_POINT : in_the_way ? 0 : MESH_LEVEL_POINT));
	}
}

// Whether a body's point rests in a quarter, its middle on a level of
// this height (the ground's or not): a level there within a step of it
// (the class's highest: whether an agent takes a step is its own, at the
// portals; between the ground and the ground: the slope's reach)
static bool mesh_point_rests(const mesh_build_t *build, int32_t at, float height, bool ground, float reach, float step)
{
	for (int level = 0; level < MESH_LEVELS; level++)
	{
		uint8_t flags = build->flags[level][at];

		if ((flags & MESH_LEVEL_POINT) != 0
			&& SDL_fabsf(build->height[level][at] - height) <= (ground && (flags & MESH_LEVEL_GROUND) != 0 ? reach : step + 0.05f))
		{
			return true;
		}
	}
	return false;
}

// Is a cell open ground: nothing on it, nothing over it (its region built)?
static bool mesh_cell_plain(const navigation_t *navigation, int32_t cell_x, int32_t cell_z)
{
	int32_t cell;

	if (cell_x < 0 || cell_z < 0 || cell_x >= navigation->size || cell_z >= navigation->size)
	{
		return false;
	}
	cell = cell_z * navigation->size + cell_x;
	return ((navigation->extra_count[cell] == 0 && navigation->terrain_clearance[cell] == NAVIGATION_CLEARANCE_OPEN)
		|| navigation->small_only[cell] != 0) && !navigation->regions[region_of_cell(navigation, cell)].stale;
}

// The level of the next quarter (at next) that carries on the rectangle
// of a level of this one: a rectangle's, in none yet, alike it (its key),
// within a step of it (the ground: the slope's reach), the nearest in
// height; -1 if none
static int mesh_match(const mesh_build_t *build, int level, int32_t at, int32_t next, float reach, float step)
{
	uint32_t key = build->key[level][at];
	float height = build->height[level][at];
	float most = ((key & MESH_KEY_TOP) != 0 ? step : reach) + 1e-4f;
	int best = -1;
	float best_difference = 1e30f;
	uint8_t fit = build->fit[next];

	for (int other = 0; fit >> other != 0; other++)
	{
		float difference;

		if (build->key[other][next] != key || build->rect[other][next] >= 0)
		{
			continue;
		}
		difference = SDL_fabsf(build->height[other][next] - height);
		if (difference <= most && difference < best_difference)
		{
			best_difference = difference;
			best = other;
		}
	}
	return best;
}

// A quarter meter's level that is in a rectangle
typedef struct
{
	int32_t rect;                      // (global id)
	float height;
	bool ground;                       // the ground's (not an object's top)
} mesh_level_t;

// How far low of the quarter meters a class's rectangles lie: half a
// quarter for a width of an even number of them (see above)
static float mesh_offset(int c)
{
	return (c + 1) % 2 == 0 ? 0.5f * NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS : 0.0f;
}

// The quarter of a class's mesh a point is in (x, z over the whole map,
// not kept inside it)
static void mesh_quarter_at(int c, float x, float z, int32_t *quarter_x_out, int32_t *quarter_z_out)
{
	*quarter_x_out = (int32_t)SDL_floorf((x + mesh_offset(c)) * ((float)NAVIGATION_SUBCELLS / NAVIGATION_CELL_METERS));
	*quarter_z_out = (int32_t)SDL_floorf((z + mesh_offset(c)) * ((float)NAVIGATION_SUBCELLS / NAVIGATION_CELL_METERS));
}

// Where a quarter of a class's mesh begins along an axis, and its middle
// (where a body's middle was tried), meters
static float mesh_quarter_low(int c, int32_t quarter)
{
	return (float)quarter * (NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS) - mesh_offset(c);
}

static float mesh_quarter_middle(int c, int32_t quarter)
{
	return ((float)quarter + 0.5f) * (NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS) - mesh_offset(c);
}

// A rectangle's quarter meters over the whole map: its first (x, z), how
// many across and deep
typedef struct
{
	int32_t x;
	int32_t z;
	int32_t width;
	int32_t depth;
} mesh_box_t;

static mesh_box_t mesh_box(const navigation_t *navigation, int c, int32_t id)
{
	const navigation_rect_t *rect = mesh_rect(navigation, c, id);
	int32_t region = id >> 16;
	mesh_box_t box;

	box.x = (region % navigation->regions_per_side) * navigation->region_size * NAVIGATION_SUBCELLS + rect->x;
	box.z = (region / navigation->regions_per_side) * navigation->region_size * NAVIGATION_SUBCELLS + rect->z;
	box.width = rect->width;
	box.depth = rect->depth;
	return box;
}

// A rectangle's box, meters
static void mesh_rect_box(const navigation_t *navigation, int c, int32_t id, float low_out[2], float high_out[2])
{
	mesh_box_t box = mesh_box(navigation, c, id);

	low_out[0] = mesh_quarter_low(c, box.x);
	low_out[1] = mesh_quarter_low(c, box.z);
	high_out[0] = mesh_quarter_low(c, box.x + box.width);
	high_out[1] = mesh_quarter_low(c, box.z + box.depth);
}

// Whether a rectangle is over a quarter of its class's mesh
static bool rect_has_quarter(const navigation_t *navigation, int c, int32_t id, int32_t quarter_x, int32_t quarter_z)
{
	mesh_box_t box = mesh_box(navigation, c, id);

	return quarter_x >= box.x && quarter_z >= box.z && quarter_x < box.x + box.width && quarter_z < box.z + box.depth;
}

// The point of a rectangle nearest a point (the middles of its outer
// quarters at the furthest: where a body's middle was tried)
static void rect_nearest(const navigation_t *navigation, int c, int32_t id, float x, float z, float out[2])
{
	mesh_box_t box = mesh_box(navigation, c, id);

	out[0] = SDL_clamp(x, mesh_quarter_middle(c, box.x), mesh_quarter_middle(c, box.x + box.width - 1));
	out[1] = SDL_clamp(z, mesh_quarter_middle(c, box.z), mesh_quarter_middle(c, box.z + box.depth - 1));
}

static float rect_height_high(const navigation_rect_t *rect)
{
	return rect->height + (float)rect->height_span * NAVIGATION_RECT_SPAN_UNIT_METERS;
}

// The height of a rectangle's surface at one of its quarter meters: the
// ground's is its cell's; a flat top's its own; an uneven top's (a ramp,
// treads the class takes in its stride) the one kept for that quarter
// when its tile was built (navigation_mesh_tile_t.uneven_heights)
static float mesh_height_at(const navigation_t *navigation, int c, int32_t id, int32_t quarter_x, int32_t quarter_z)
{
	const navigation_rect_t *rect = mesh_rect(navigation, c, id);
	const navigation_mesh_tile_t *tile;
	mesh_box_t box;
	int32_t index = id & 0xFFFF;
	int32_t low = 0;
	int32_t high;

	if ((rect->flags & NAVIGATION_RECT_GROUND) != 0)
	{
		int32_t cell_x = SDL_clamp(quarter_x >> 2, 0, navigation->size - 1);
		int32_t cell_z = SDL_clamp(quarter_z >> 2, 0, navigation->size - 1);

		return navigation->ground_height[cell_z * navigation->size + cell_x];
	}
	if (rect->height_span == 0)
	{
		return rect->height;
	}
	tile = &navigation->mesh.classes[c].tiles[id >> 16];
	high = tile->uneven_count - 1;
	while (low < high)
	{
		int32_t middle = (low + high) / 2;

		if ((int32_t)tile->uneven_rects[middle] < index)
		{
			low = middle + 1;
		}
		else
		{
			high = middle;
		}
	}
	if (low >= tile->uneven_count || (int32_t)tile->uneven_rects[low] != index)
	{
		return rect->height + 0.5f * (rect_height_high(rect) - rect->height);
	}
	box = mesh_box(navigation, c, id);
	return tile->uneven_heights[tile->uneven_first[low] + (uint32_t)(clamp_int(quarter_z - box.z, 0, box.depth - 1) * box.width
		+ clamp_int(quarter_x - box.x, 0, box.width - 1))];
}

// The rectangles of a class over a quarter meter (x, z in quarters over
// the whole map), of built tiles: each with its height there; how many
static int mesh_levels_at(const navigation_t *navigation, int c, int32_t quarter_x, int32_t quarter_z, mesh_level_t levels_out[MESH_LEVELS])
{
	int32_t cell_x = quarter_x >> 2;
	int32_t cell_z = quarter_z >> 2;
	int32_t region;
	const navigation_mesh_tile_t *tile;
	int32_t local_x;
	int32_t local_z;
	int32_t bucket;
	int count = 0;

	if (quarter_x < 0 || quarter_z < 0 || cell_x >= navigation->size || cell_z >= navigation->size)
	{
		return 0;
	}
	region = region_of_cell(navigation, cell_z * navigation->size + cell_x);
	tile = &navigation->mesh.classes[c].tiles[region];
	if (!tile->built)
	{
		return 0;
	}
	local_x = quarter_x - (region % navigation->regions_per_side) * navigation->region_size * NAVIGATION_SUBCELLS;
	local_z = quarter_z - (region / navigation->regions_per_side) * navigation->region_size * NAVIGATION_SUBCELLS;
	bucket = (local_z >> 4) * NAVIGATION_MESH_BUCKETS + (local_x >> 4);
	for (uint32_t k = tile->bucket_first[bucket]; k < tile->bucket_first[bucket + 1] && count < MESH_LEVELS; k++)
	{
		const navigation_rect_t *rect = &tile->rects[tile->bucket_rects[k]];
		int32_t id = (region << 16) | tile->bucket_rects[k];

		if (local_x < rect->x || local_z < rect->z || local_x >= rect->x + rect->width || local_z >= rect->z + rect->depth)
		{
			continue;
		}
		levels_out[count].rect = id;
		levels_out[count].height = mesh_height_at(navigation, c, id, quarter_x, quarter_z);
		levels_out[count].ground = (rect->flags & NAVIGATION_RECT_GROUND) != 0;
		count++;
	}
	return count;
}

// The level of a rectangle at a quarter meter (false: it is not there)
static bool mesh_level_of(const navigation_t *navigation, int c, int32_t id, int32_t quarter_x, int32_t quarter_z, mesh_level_t *level_out)
{
	if (!rect_has_quarter(navigation, c, id, quarter_x, quarter_z))
	{
		return false;
	}
	level_out->rect = id;
	level_out->height = mesh_height_at(navigation, c, id, quarter_x, quarter_z);
	level_out->ground = (mesh_rect(navigation, c, id)->flags & NAVIGATION_RECT_GROUND) != 0;
	return true;
}

// The fringe of a class over a quarter meter (navigation_fringe_t), of a
// built tile: the surfaces there a body's middle cannot be on
// (stand_only: those one stands on, a foot's rest on the ground too);
// how many (their rect -1)
static int mesh_fringe_at(const navigation_t *navigation, int c, int32_t quarter_x, int32_t quarter_z, bool stand_only,
	mesh_level_t levels_out[MESH_LEVELS])
{
	int32_t cell_x = quarter_x >> 2;
	int32_t cell_z = quarter_z >> 2;
	int32_t region;
	const navigation_mesh_tile_t *tile;
	int32_t local_x;
	int32_t local_z;
	int count = 0;

	if (quarter_x < 0 || quarter_z < 0 || cell_x >= navigation->size || cell_z >= navigation->size)
	{
		return 0;
	}
	region = region_of_cell(navigation, cell_z * navigation->size + cell_x);
	tile = &navigation->mesh.classes[c].tiles[region];
	if (!tile->built || tile->fringe_row_first == NULL)
	{
		return 0;
	}
	local_x = quarter_x - (region % navigation->regions_per_side) * navigation->region_size * NAVIGATION_SUBCELLS;
	local_z = quarter_z - (region / navigation->regions_per_side) * navigation->region_size * NAVIGATION_SUBCELLS;
	for (uint32_t k = tile->fringe_row_first[local_z]; k < tile->fringe_row_first[local_z + 1] && count < MESH_LEVELS; k++)
	{
		const navigation_fringe_t *run = &tile->fringe[k];

		if (local_x >= (int32_t)run->x && local_x < (int32_t)run->x + (int32_t)run->count
			&& (stand_only ? run->stand != NAVIGATION_FRINGE_SURFACE : run->stand != NAVIGATION_FRINGE_POINT))
		{
			levels_out[count].rect = -1;
			levels_out[count].height = run->height;
			levels_out[count].ground = run->ground != 0;
			count++;
		}
	}
	return count;
}

// A tile's buckets made from its rectangles: which of them reach into
// each 4 m of it
static bool mesh_fill_buckets(navigation_mesh_tile_t *tile)
{
	uint32_t total = 0;
	static _Thread_local uint32_t counts[NAVIGATION_MESH_BUCKETS * NAVIGATION_MESH_BUCKETS];

	SDL_memset(counts, 0, sizeof(counts));
	for (int pass = 0; pass < 2; pass++)
	{
		for (int32_t r = 0; r < tile->rect_count; r++)
		{
			const navigation_rect_t *rect = &tile->rects[r];

			for (int32_t bucket_z = rect->z >> 4; bucket_z <= (rect->z + rect->depth - 1) >> 4; bucket_z++)
			{
				for (int32_t bucket_x = rect->x >> 4; bucket_x <= (rect->x + rect->width - 1) >> 4; bucket_x++)
				{
					int32_t bucket = bucket_z * NAVIGATION_MESH_BUCKETS + bucket_x;

					if (pass == 0)
					{
						counts[bucket]++;
					}
					else
					{
						tile->bucket_rects[tile->bucket_first[bucket] + counts[bucket]++] = (uint16_t)r;
					}
				}
			}
		}
		if (pass == 0)
		{
			for (int32_t bucket = 0; bucket < NAVIGATION_MESH_BUCKETS * NAVIGATION_MESH_BUCKETS; bucket++)
			{
				tile->bucket_first[bucket] = total;
				total += counts[bucket];
				counts[bucket] = 0;
			}
			tile->bucket_first[NAVIGATION_MESH_BUCKETS * NAVIGATION_MESH_BUCKETS] = total;
			if (!mesh_grow((void **)&tile->bucket_rects, &tile->bucket_capacity, (int32_t)SDL_max(total, 1u), sizeof(uint16_t)))
			{
				return false;
			}
		}
	}
	return true;
}

// The valid neighbors of the rectangle id over one of its sides (0: +x,
// 1: -x, 2: +z, 3: -z), added to a list (each once): along the side,
// quarter by quarter, the rectangle beyond nearest in height (within
// MESH_PORTAL_RISE_MOST). Where the quarter beyond is no rectangle's (no
// place for a body's middle: by a terrace's face, on its edge), the first
// one further out that is (MESH_JUMP_GAP_QUARTERS at most), if it is more
// than a step off: an edge to jump (mesh_jump finds what is between). Only
// in built tiles, and only in the tile keep_region (the rectangle's own,
// or the one over that side: the border's)
#define MESH_SIDE_NEIGHBORS_MOST 512
static void mesh_side_neighbors(const navigation_t *navigation, int c, int32_t id, int side, int32_t keep_region, int32_t *list, int *count)
{
	mesh_box_t box = mesh_box(navigation, c, id);
	int32_t along = side < 2 ? box.depth : box.width;
	int32_t out_x = side == 0 ? 1 : side == 1 ? -1 : 0;
	int32_t out_z = side == 2 ? 1 : side == 3 ? -1 : 0;
	float step = SDL_min(navigation->mesh.classes[c].step, 50.0f);
	int32_t last = -1;

	for (int32_t k = 0; k < along; k++)
	{
		int32_t inside_x = side == 0 ? box.x + box.width - 1 : side == 1 ? box.x : box.x + k;
		int32_t inside_z = side == 2 ? box.z + box.depth - 1 : side == 3 ? box.z : box.z + k;
		float inside_height = mesh_height_at(navigation, c, id, inside_x, inside_z);
		int32_t to = -1;

		for (int32_t out = 1; out <= 1 + MESH_JUMP_GAP_QUARTERS && to < 0; out++)
		{
			mesh_level_t levels[MESH_LEVELS];
			int level_count = mesh_levels_at(navigation, c, inside_x + out_x * out, inside_z + out_z * out, levels);
			float best = MESH_PORTAL_RISE_MOST;
			bool any = false;

			for (int n = 0; n < level_count; n++)
			{
				float difference = SDL_fabsf(levels[n].height - inside_height);

				if (levels[n].rect == id || difference > best)
				{
					continue;
				}
				any = true;
				best = difference;
				to = levels[n].rect;
			}
			// (a rectangle within a step across what a body does not get
			// through: no way; and nothing is looked for behind it)
			if (any && out > 1 && best <= step + 1e-4f)
			{
				to = -1;
			}
			if (any)
			{
				break;
			}
		}
		if (to < 0 || (to >> 16) != keep_region || to == last)
		{
			continue;
		}
		last = to;
		{
			bool known = false;

			for (int n = 0; n < *count && !known; n++)
			{
				known = list[n] == to;
			}
			if (!known && *count < MESH_SIDE_NEIGHBORS_MOST)
			{
				list[(*count)++] = to;
			}
		}
	}
}

static void region_neighbor(const navigation_t *navigation, int32_t region, int side, int32_t *neighbor_out)
{
	int32_t region_x = region % navigation->regions_per_side + (side == 0 ? 1 : side == 1 ? -1 : 0);
	int32_t region_z = region / navigation->regions_per_side + (side == 2 ? 1 : side == 3 ? -1 : 0);

	*neighbor_out = region_x >= 0 && region_z >= 0 && region_x < navigation->regions_per_side && region_z < navigation->regions_per_side
		? region_z * navigation->regions_per_side + region_x : -1;
}

// Is a rectangle by its tile's side: on it, or as near as an edge is
// jumped over (its neighbors may be in the tile beyond)?
static bool rect_by_side(const navigation_t *navigation, const navigation_rect_t *rect, int32_t region, int side)
{
	int32_t first_x;
	int32_t first_z;
	int32_t last_x;
	int32_t last_z;

	region_bounds(navigation, region, &first_x, &first_z, &last_x, &last_z);
	return side == 0 ? rect->x + rect->width + MESH_JUMP_GAP_QUARTERS >= (last_x - first_x + 1) * NAVIGATION_SUBCELLS
		: side == 1 ? rect->x <= MESH_JUMP_GAP_QUARTERS
		: side == 2 ? rect->z + rect->depth + MESH_JUMP_GAP_QUARTERS >= (last_z - first_z + 1) * NAVIGATION_SUBCELLS
		: rect->z <= MESH_JUMP_GAP_QUARTERS;
}

// The neighbors between a tile and the next one over a side, both ways,
// made again (either built anew: the other's into it are stale)
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
		int32_t to_region = pass == 0 ? neighbor : region;
		int from_side = pass == 0 ? side : opposite;
		navigation_mesh_tile_t *tile = &tiles[from_region];

		for (int32_t r = 0; r < tile->rect_count; r++)
		{
			int32_t list[MESH_SIDE_NEIGHBORS_MOST];
			int count = 0;

			if (!rect_by_side(navigation, &tile->rects[r], from_region, from_side))
			{
				continue;
			}
			mesh_side_neighbors(navigation, c, (from_region << 16) | r, from_side, to_region, list, &count);
			if (!mesh_grow((void **)&tile->border[from_side], &tile->border_capacity[from_side], tile->border_count[from_side] + count,
				sizeof(navigation_border_t)))
			{
				return;
			}
			for (int n = 0; n < count; n++)
			{
				navigation_border_t *entry = &tile->border[from_side][tile->border_count[from_side]++];

				entry->from = (uint16_t)r;
				entry->to = (uint16_t)(list[n] & 0xFFFF);
			}
		}
	}
}

// Where one rectangle leads into another that is over one of its sides:
// the part of that side they share (its ends on the first one's side,
// meters), how many quarter meters lie between them (0: they touch), and
// along which quarters (first to last, of the whole map, along the side).
// False if they share none
typedef struct
{
	int side;
	int32_t first;                     // quarters along the side they share: from this one
	int32_t last;                      // to before this one
	int32_t inside;                    // the first one's quarter across the side (x for sides 0 and 1, z for 2 and 3)
	int32_t beyond;                    // the other's
	int32_t gap;
} mesh_shared_t;

static bool mesh_shared(const navigation_t *navigation, int c, int32_t from, int32_t to, int side, mesh_shared_t *out)
{
	mesh_box_t a = mesh_box(navigation, c, from);
	mesh_box_t b = mesh_box(navigation, c, to);
	int32_t a_low = side < 2 ? a.z : a.x;
	int32_t a_high = side < 2 ? a.z + a.depth : a.x + a.width;
	int32_t b_low = side < 2 ? b.z : b.x;
	int32_t b_high = side < 2 ? b.z + b.depth : b.x + b.width;
	// (the first one's edge quarter, the first quarter past it, and where
	// the other one is nearest going out)
	int32_t edge = side == 0 ? a.x + a.width - 1 : side == 1 ? a.x : side == 2 ? a.z + a.depth - 1 : a.z;
	int32_t direction = side == 0 || side == 2 ? 1 : -1;
	int32_t b_near = side == 0 ? b.x : side == 1 ? b.x + b.width - 1 : side == 2 ? b.z : b.z + b.depth - 1;
	int32_t b_far = side == 0 ? b.x + b.width - 1 : side == 1 ? b.x : side == 2 ? b.z + b.depth - 1 : b.z;
	int32_t next = edge + direction;

	out->side = side;
	out->first = SDL_max(a_low, b_low);
	out->last = SDL_min(a_high, b_high);
	out->inside = edge;
	// (the other one over the quarter past the edge: they touch; else as
	// far out as it begins)
	if ((b_near - next) * direction <= 0 && (b_far - next) * direction >= 0)
	{
		out->beyond = next;
	}
	else
	{
		out->beyond = b_near;
	}
	out->gap = (out->beyond - next) * direction;
	return out->last > out->first && out->gap >= 0 && out->gap <= MESH_JUMP_GAP_QUARTERS;
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

// The regions round a region as they are, in a number: their generations
// (one not built yet: 0). A tile looks a cell into them (where the bodies'
// points reach from its edge): it is built again when one changes or is
// built
#define MESH_EDGE_CELLS 3              // a region's cells this far in from an edge are its neighbors' business
#define MESH_EDGE_METERS 3.0f          // and its small things this near it (welded to ones across it)

static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t count)
{
	const uint8_t *at = bytes;

	for (size_t k = 0; k < count; k++)
	{
		hash = (hash ^ at[k]) * 1099511628211ull;
	}
	return hash;
}

// What a region's cells along an edge hold, as the rectangles read them
static uint64_t region_edge_hash(const navigation_t *navigation, int32_t region_index, int edge)
{
	const navigation_region_t *region = &navigation->regions[region_index];
	int32_t box[4];
	uint64_t hash = 1469598103934665603ull;
	float edge_at;

	region_box(navigation, region_index, box);
	edge_at = (float)(edge == 0 ? box[0] : edge == 1 ? box[2] + 1 : edge == 2 ? box[1] : box[3] + 1) * NAVIGATION_CELL_METERS;
	if (edge == 0)
	{
		box[2] = SDL_min(box[2], box[0] + MESH_EDGE_CELLS - 1);
	}
	else if (edge == 1)
	{
		box[0] = SDL_max(box[0], box[2] - MESH_EDGE_CELLS + 1);
	}
	else if (edge == 2)
	{
		box[3] = SDL_min(box[3], box[1] + MESH_EDGE_CELLS - 1);
	}
	else
	{
		box[1] = SDL_max(box[1], box[3] - MESH_EDGE_CELLS + 1);
	}
	for (int32_t cell_z = box[1]; cell_z <= box[3]; cell_z++)
	{
		for (int32_t cell_x = box[0]; cell_x <= box[2]; cell_x++)
		{
			int32_t cell = cell_z * navigation->size + cell_x;
			int32_t nodes[1 + NAVIGATION_MAX_EXTRA_SURFACES];
			int count = cell_nodes(navigation, cell, nodes);
			// (a cell only small things stand in is open ground to the
			// rectangles, whatever its surfaces: as one with nothing in it;
			// the soft things' heights are nothing to them either)
			bool open = navigation->small_only[cell] != 0;
			uint8_t bytes[4] = { open ? NAVIGATION_CLEARANCE_OPEN : navigation->terrain_clearance[cell], navigation->blocker_zone[cell],
				navigation->ground_slope[cell], (uint8_t)(open ? 1 : count) };

			count = open ? 1 : count;

			hash = hash_bytes(hash, &navigation->ground_height[cell], sizeof(float));
			hash = hash_bytes(hash, bytes, sizeof(bytes));
			for (int k = 1; k < count; k++)
			{
				const navigation_surface_t *surface = extra_surface(navigation, nodes[k]);

				hash = hash_bytes(hash, &surface->height, sizeof(float));
				hash = hash_bytes(hash, &surface->clearance, 1);
				hash = hash_bytes(hash, surface->below, sizeof(surface->below));
				hash = hash_bytes(hash, surface->room, sizeof(surface->room));
				hash = hash_bytes(hash, &surface->ground_below, 1);
			}
		}
	}
	for (int32_t k = 0; k < region->capsule_count; k++)
	{
		const navigation_capsule_t *capsule = &region->capsules[k];
		float off = SDL_fabsf((edge < 2 ? capsule->a[0] : capsule->a[2]) - edge_at);

		if (off <= MESH_EDGE_METERS + capsule->radius)
		{
			hash = hash_bytes(hash, capsule->a, sizeof(capsule->a));
			hash = hash_bytes(hash, capsule->b, sizeof(capsule->b));
			hash = hash_bytes(hash, &capsule->radius, sizeof(float));
			hash = hash_bytes(hash, &capsule->soft, 1);
		}
	}
	return hash;
}

// What the regions round one hold along the edges toward it, as far as
// its tiles care (their margins: a body's points by a tile's edge reach
// into the next region): changed, and the tiles are built again. A
// region built again for something in its middle changes nothing here
// (each of the eight used to build all the tiles round it again: nine
// tiles for a felled tree)
static uint32_t mesh_margin_signature(navigation_t *navigation, int32_t region)
{
	int32_t region_x = region % navigation->regions_per_side;
	int32_t region_z = region / navigation->regions_per_side;
	uint32_t signature = 0;

	for (int32_t dz = -1; dz <= 1; dz++)
	{
		for (int32_t dx = -1; dx <= 1; dx++)
		{
			int32_t near_x = region_x + dx;
			int32_t near_z = region_z + dz;
			int32_t near;
			navigation_region_t *neighbor;

			if ((dx == 0 && dz == 0) || near_x < 0 || near_z < 0 || near_x >= navigation->regions_per_side || near_z >= navigation->regions_per_side)
			{
				continue;
			}
			near = near_z * navigation->regions_per_side + near_x;
			neighbor = &navigation->regions[near];
			if (neighbor->stale)
			{
				signature = signature * 31u;
				continue;
			}
			if (neighbor->edge_checked_generation != navigation->region_generation[near])
			{
				for (int edge = 0; edge < 4; edge++)
				{
					uint64_t hash = region_edge_hash(navigation, near, edge);

					if (hash != neighbor->edge_hash[edge] || neighbor->edge_generation[edge] == 0)
					{
						neighbor->edge_hash[edge] = hash;
						neighbor->edge_generation[edge] = navigation->region_generation[near];
					}
				}
				neighbor->edge_checked_generation = navigation->region_generation[near];
			}
			// (its edges toward this one: x high of the one at x low, ...)
			signature = signature * 31u + (dx < 0 ? neighbor->edge_generation[1] : dx > 0 ? neighbor->edge_generation[0] : 0u);
			signature = signature * 31u + (dz < 0 ? neighbor->edge_generation[3] : dz > 0 ? neighbor->edge_generation[2] : 0u);
		}
	}
	return signature;
}

// A small thing as it lies in an agent's way: its capsule from its foot
// up to the agent's height, laid on the ground, a circle round that (a
// leaning trunk goes over a hen's head by its root, not over a man's)
typedef struct
{
	float x;
	float z;
	float radius;                      // its own
	float a[2];                        // the capsule's own ends on the ground
	float b[2];
	float capsule_radius;
} small_circle_t;

static int small_circles(const navigation_t *navigation, float height, bool near_only, float margin, float minimum_x, float minimum_z,
	float maximum_x, float maximum_z, const float *along_from, const float *along_to, small_circle_t *circles_out, int most);
static void mesh_quarter_at(int c, float x, float z, int32_t *quarter_x_out, int32_t *quarter_z_out);

#define SMALL_MARGIN_METERS 2.0f       // things this far round a stretch are gathered for it (its ways round them stay within that)
#define SMALL_LINE_MARGIN_METERS 1.0f  // and this far round a line only asked about, or a point
#define MESH_WELD_CIRCLES_MOST 2048
#define MESH_WELD_STEP_METERS 0.1f

static int compare_small_circles(const void *a, const void *b)
{
	float first = ((const small_circle_t *)a)->x;
	float second = ((const small_circle_t *)b)->x;

	return first < second ? -1 : first > second ? 1 : 0;
}

// Small things too near one another for a body of the class to pass
// between (a row of trunks, a palisade): no body's middle on the line
// from one to the other. Too near: less than MESH_WELD_SHARE of its
// width between them (bodies squeeze through what is a little narrower
// than they are, as the rectangles themselves let them: BODY_REACH; by
// the whole width and the circles round the capsules a forest was a
// maze of walls, and a search through it went over two thousand
// rectangles for 14 m). Each alone is not in the rectangles (a way is
// led round it, mesh_lead_round); a row of them is a wall, and the
// search is to know it
static void mesh_weld_small(const navigation_t *navigation, const navigation_mesh_class_t *class, int c, mesh_build_t *build)
{
	static _Thread_local small_circle_t circles[MESH_WELD_CIRCLES_MOST];
	float first_x = (float)build->first_x * NAVIGATION_CELL_METERS;
	float first_z = (float)build->first_z * NAVIGATION_CELL_METERS;
	int count = small_circles(navigation, class->height, false, SMALL_MARGIN_METERS, first_x, first_z, first_x + (float)build->width * NAVIGATION_CELL_METERS,
		first_z + (float)build->depth * NAVIGATION_CELL_METERS, NULL, NULL, circles, MESH_WELD_CIRCLES_MOST);

	float widest = 0.0f;

	// (in the order of their x: each one against the few just after it)
	SDL_qsort(circles, (size_t)count, sizeof(small_circle_t), compare_small_circles);
	for (int i = 0; i < count; i++)
	{
		widest = SDL_max(widest, circles[i].radius);
	}
	for (int i = 0; i < count; i++)
	{
		for (int j = i + 1; j < count && circles[j].x - circles[i].x < 2.0f * widest + 2.0f * MESH_WELD_SHARE * class->radius; j++)
		{
			float dx = circles[j].x - circles[i].x;
			float dz = circles[j].z - circles[i].z;
			float distance = SDL_sqrtf(dx * dx + dz * dz);
			float least = 1e30f;
			float from[2] = { circles[i].x, circles[i].z };
			float to[2] = { circles[j].x, circles[j].z };
			int steps;

			if (distance >= circles[i].radius + circles[j].radius + 2.0f * MESH_WELD_SHARE * class->radius)
			{
				continue;
			}
			// (their capsules as they lie on the ground, a leaning trunk's
			// a line: the nearest two points of them, sampled)
			for (int m = 0; m <= 4; m++)
			{
				for (int n = 0; n <= 4; n++)
				{
					float mine[2] = { circles[i].a[0] + (circles[i].b[0] - circles[i].a[0]) * 0.25f * (float)m,
						circles[i].a[1] + (circles[i].b[1] - circles[i].a[1]) * 0.25f * (float)m };
					float other[2] = { circles[j].a[0] + (circles[j].b[0] - circles[j].a[0]) * 0.25f * (float)n,
						circles[j].a[1] + (circles[j].b[1] - circles[j].a[1]) * 0.25f * (float)n };
					float apart = SDL_sqrtf((other[0] - mine[0]) * (other[0] - mine[0]) + (other[1] - mine[1]) * (other[1] - mine[1]));

					if (apart < least)
					{
						least = apart;
						SDL_memcpy(from, mine, sizeof(from));
						SDL_memcpy(to, other, sizeof(to));
					}
				}
			}
			if (least >= circles[i].capsule_radius + circles[j].capsule_radius + 2.0f * MESH_WELD_SHARE * class->radius)
			{
				continue;
			}
			dx = to[0] - from[0];
			dz = to[1] - from[1];
			steps = SDL_max((int)SDL_ceilf(least / MESH_WELD_STEP_METERS), 1);
			for (int k = 0; k <= steps; k++)
			{
				int32_t quarter_x;
				int32_t quarter_z;
				int32_t local_x;
				int32_t local_z;

				mesh_quarter_at(c, from[0] + dx * (float)k / (float)steps, from[1] + dz * (float)k / (float)steps, &quarter_x, &quarter_z);
				local_x = quarter_x - build->first_x * NAVIGATION_SUBCELLS;
				local_z = quarter_z - build->first_z * NAVIGATION_SUBCELLS;
				if (local_x < 0 || local_z < 0 || local_x >= build->width * NAVIGATION_SUBCELLS || local_z >= build->depth * NAVIGATION_SUBCELLS)
				{
					continue;
				}
				build->flags[0][(local_z + MESH_MARGIN_QUARTERS) * build->span + local_x + MESH_MARGIN_QUARTERS] &= (uint8_t)~MESH_LEVEL_STAND;
				build->plain[(local_z / NAVIGATION_SUBCELLS + 1) * (build->width + 2) + local_x / NAVIGATION_SUBCELLS + 1] = false;
			}
		}
	}
}

// A tile of a class built anew from its region's quarter meters (the
// region up to date): its rectangles, the portals among them, then the
// ones to the tiles round it
static bool mesh_build_tile(navigation_t *navigation, int c, int32_t region, const water_t *water)
{
	// (a thread's own: navigation_bake builds tiles side by side)
	static _Thread_local mesh_build_t build;
	static _Thread_local uint8_t grid[NAVIGATION_MESH_RECT_MOST][NAVIGATION_MESH_RECT_MOST];
	navigation_mesh_class_t *class = &navigation->mesh.classes[c];
	navigation_mesh_tile_t *tile = &class->tiles[region];
	int32_t last_x;
	int32_t last_z;
	uint64_t start_ticks = SDL_GetPerformanceCounter();
	float reach = SDL_min(class->reach, 50.0f);
	float step = SDL_min(class->step, 50.0f);
	float step_most = SDL_max(step, class->step_most);
	int32_t quarters_wide;
	int32_t quarters_deep;
	int32_t point_offsets[BODY_POINTS];

	region_bounds(navigation, region, &build.first_x, &build.first_z, &last_x, &last_z);
	build.width = last_x - build.first_x + 1;
	build.depth = last_z - build.first_z + 1;
	build.region = region;
	quarters_wide = build.width * NAVIGATION_SUBCELLS;
	quarters_deep = build.depth * NAVIGATION_SUBCELLS;
	build.span = quarters_wide + 2 * MESH_MARGIN_QUARTERS;
	tile->built = false;
	tile->rect_count = 0;
	tile->neighbor_count = 0;
	tile->uneven_count = 0;
	tile->uneven_height_count = 0;
	tile->fringe_count = 0;
	if (build.width > REGION_CELLS || build.depth > REGION_CELLS)
	{
		return false;
	}
	// The quarter meters' levels, the margin's too (off the map: none);
	// the water on the tile's cells now
	for (int level = 0; level < MESH_LEVELS; level++)
	{
		size_t quarters = (size_t)build.span * (size_t)(quarters_deep + 2 * MESH_MARGIN_QUARTERS);

		SDL_memset(build.flags[level], 0, quarters);
		SDL_memset(build.key[level], 0, quarters * sizeof(uint32_t));
		SDL_memset(build.rect[level], 0xFF, quarters * sizeof(int16_t));
	}
	SDL_memset(build.fit, 0, (size_t)build.span * (size_t)(quarters_deep + 2 * MESH_MARGIN_QUARTERS));
	SDL_memset(build.plain, 0, sizeof(build.plain));
	for (int32_t local_z = -1; local_z <= build.depth; local_z++)
	{
		for (int32_t local_x = -1; local_x <= build.width; local_x++)
		{
			int32_t cell_x = build.first_x + local_x;
			int32_t cell_z = build.first_z + local_z;

			if (cell_x < 0 || cell_z < 0 || cell_x >= navigation->size || cell_z >= navigation->size)
			{
				continue;
			}
			mesh_build_cell(navigation, class, &build, cell_x, cell_z);
			build.plain[(local_z + 1) * (build.width + 2) + local_x + 1] = mesh_cell_plain(navigation, cell_x, cell_z);
			if (local_x >= 0 && local_z >= 0 && local_x < build.width && local_z < build.depth)
			{
				build.wet[local_z * build.width + local_x] = water != NULL && !water->iced && !water_dry_at(navigation, water, cell_x, cell_z,
					((float)cell_x + 0.5f) * NAVIGATION_CELL_METERS, ((float)cell_z + 0.5f) * NAVIGATION_CELL_METERS);
			}
		}
	}
	// Where a body's points are from its middle, in quarter meters: the
	// class's widest, as quarter_tightness has them (its middle in the
	// quarter's middle, or at its low corner: mesh_offset)
	for (int k = 0; k < BODY_POINTS; k++)
	{
		const float quarter = NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS;
		float angle = (float)k * (6.2831853f / (float)BODY_POINTS);
		float out = SDL_min(class->radius, 1.0f) * BODY_REACH;
		float out_x = SDL_sinf(angle) * out;
		float out_z = SDL_cosf(angle) * out;
		int32_t off_x;
		int32_t off_z;

		// (straight along an axis: on it, not a hair to one side)
		out_x = SDL_fabsf(out_x) < 1e-4f ? 0.0f : out_x;
		out_z = SDL_fabsf(out_z) < 1e-4f ? 0.0f : out_z;
		off_x = (int32_t)SDL_floorf((0.5f * quarter - mesh_offset(c) + out_x) / quarter);
		off_z = (int32_t)SDL_floorf((0.5f * quarter - mesh_offset(c) + out_z) / quarter);

		point_offsets[k] = SDL_clamp(off_z, -MESH_MARGIN_QUARTERS, MESH_MARGIN_QUARTERS) * build.span
			+ SDL_clamp(off_x, -MESH_MARGIN_QUARTERS, MESH_MARGIN_QUARTERS);
	}
	mesh_weld_small(navigation, class, c, &build);
	// Which levels are a rectangle's (a body's middle can be there), and
	// what its quarters must share (its key)
	for (int32_t local_z = 0; local_z < build.depth; local_z++)
	{
		for (int32_t local_x = 0; local_x < build.width; local_x++)
		{
			int32_t cell_x = build.first_x + local_x;
			int32_t cell_z = build.first_z + local_z;
			int32_t cell = cell_z * navigation->size + cell_x;
			int32_t first = (local_z * NAVIGATION_SUBCELLS + MESH_MARGIN_QUARTERS) * build.span + local_x * NAVIGATION_SUBCELLS + MESH_MARGIN_QUARTERS;
			float cost = navigation->walk_factors[navigation->ground_slope[cell]];
			uint32_t cell_key = MESH_KEY_FIT | (build.wet[local_z * build.width + local_x] ? MESH_KEY_WET : 0)
				| (navigation->blocker_zone[cell] != 0 ? MESH_KEY_ZONE : 0);
			uint32_t ground_key = cell_key | ((uint32_t)SDL_clamp((int)((cost - 1.0f) * 4.0f), 0, 31) << 24);
			bool plain = true;

			// (open ground, and all round it, within reach: a body has
			// nothing to brush against there)
			for (int32_t dz = -1; dz <= 1 && plain; dz++)
			{
				for (int32_t dx = -1; dx <= 1 && plain; dx++)
				{
					plain = build.plain[(local_z + dz + 1) * (build.width + 2) + local_x + dx + 1]
						&& SDL_fabsf(navigation->ground_height[(cell_z + dz) * navigation->size + cell_x + dx] - navigation->ground_height[cell]) <= reach;
				}
			}
			for (int32_t sub = 0; sub < NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS; sub++)
			{
				int32_t at = first + (sub / NAVIGATION_SUBCELLS) * build.span + sub % NAVIGATION_SUBCELLS;

				if (plain)
				{
					build.key[0][at] = ground_key | (255u << 16);
					build.fit[at] = 1;
					continue;
				}
				for (int level = 0; level < MESH_LEVELS; level++)
				{
					uint8_t flags = build.flags[level][at];
					uint8_t room = build.room[level][at];
					int tight = 0;

					if ((flags & MESH_LEVEL_STAND) == 0)
					{
						continue;
					}
					for (int k = 0; k < BODY_POINTS && tight == 0; k++)
					{
						tight += mesh_point_rests(&build, at + point_offsets[k], build.height[level][at], (flags & MESH_LEVEL_GROUND) != 0, reach,
							step_most) ? 0 : 1;
					}
					if (tight > 0)
					{
						continue;
					}
					build.key[level][at] = ((flags & MESH_LEVEL_GROUND) != 0 ? ground_key : cell_key | MESH_KEY_TOP)
						| ((uint32_t)(room == NAVIGATION_CLEARANCE_OPEN ? 255 : room / 5) << 16);
					build.fit[at] |= (uint8_t)(1 << level);
				}
			}
		}
	}
	// The fringe: the surfaces that are no rectangle's (a body's middle
	// does not fit there), in runs along x of one height
	for (int32_t quarter_z = 0; quarter_z < quarters_deep; quarter_z++)
	{
		if (tile->fringe_row_first != NULL)
		{
			tile->fringe_row_first[quarter_z] = (uint32_t)tile->fringe_count;
		}
		for (int level = 0; level < MESH_LEVELS; level++)
		{
			navigation_fringe_t *run = NULL;

			for (int32_t quarter_x = 0; quarter_x < quarters_wide; quarter_x++)
			{
				int32_t at = (quarter_z + MESH_MARGIN_QUARTERS) * build.span + quarter_x + MESH_MARGIN_QUARTERS;
				uint8_t flags = build.flags[level][at];
				uint8_t ground = (flags & MESH_LEVEL_GROUND) != 0 ? 1 : 0;

				uint8_t stand = (flags & MESH_LEVEL_STAND) != 0 ? NAVIGATION_FRINGE_STAND
					: (flags & MESH_LEVEL_POINT) != 0 ? NAVIGATION_FRINGE_POINT : NAVIGATION_FRINGE_SURFACE;

				if ((flags & (MESH_LEVEL_SURFACE | MESH_LEVEL_POINT)) == 0 || (build.fit[at] >> level & 1) != 0)
				{
					run = NULL;
					continue;
				}
				if (run != NULL && run->count < 255 && run->ground == ground && run->stand == stand
					&& SDL_fabsf(run->height - build.height[level][at]) < 0.005f)
				{
					run->count++;
					continue;
				}
				if (tile->fringe_row_first == NULL)
				{
					// (its first: the rows before have none)
					tile->fringe_row_first = SDL_calloc((size_t)MESH_TILE_QUARTERS_MOST + 1, sizeof(uint32_t));
					if (tile->fringe_row_first == NULL)
					{
						return false;
					}
				}
				if (!mesh_grow((void **)&tile->fringe, &tile->fringe_capacity, tile->fringe_count + 1, sizeof(navigation_fringe_t)))
				{
					return false;
				}
				run = &tile->fringe[tile->fringe_count++];
				run->x = (uint8_t)quarter_x;
				run->count = 1;
				run->ground = ground;
				run->stand = stand;
				run->height = build.height[level][at];
			}
		}
	}
	if (tile->fringe_row_first != NULL)
	{
		for (int32_t quarter_z = quarters_deep; quarter_z <= MESH_TILE_QUARTERS_MOST; quarter_z++)
		{
			tile->fringe_row_first[quarter_z] = (uint32_t)tile->fringe_count;
		}
	}
	// The rectangles: from each quarter's level not yet in one, as wide as
	// its row goes on alike, then as deep as whole rows go on alike (the
	// water's: within their cell, looked at with the water as it is)
	for (int32_t quarter_z = 0; quarter_z < quarters_deep; quarter_z++)
	{
		for (int32_t quarter_x = 0; quarter_x < quarters_wide; quarter_x++)
		{
			int32_t at = (quarter_z + MESH_MARGIN_QUARTERS) * build.span + quarter_x + MESH_MARGIN_QUARTERS;

			for (int level = 0; build.fit[at] >> level != 0; level++)
			{
				uint32_t key = build.key[level][at];
				// (the water's and the blockers': within their cell)
				bool wet = (key & (MESH_KEY_WET | MESH_KEY_ZONE)) != 0;
				int32_t width = 1;
				int32_t depth = 1;
				navigation_rect_t *rect;
				int32_t index;
				float cost_sum = 0.0f;

				if (key == 0 || build.rect[level][at] >= 0 || tile->rect_count >= NAVIGATION_MESH_TILE_RECTS_MOST)
				{
					continue;
				}
				grid[0][0] = (uint8_t)level;
				// Along x
				while (width < NAVIGATION_MESH_RECT_MOST && quarter_x + width < quarters_wide
					&& !(wet && (quarter_x + width) % NAVIGATION_SUBCELLS == 0))
				{
					int next = mesh_match(&build, grid[0][width - 1], at + width - 1, at + width, reach, step);

					if (next < 0)
					{
						break;
					}
					grid[0][width++] = (uint8_t)next;
				}
				// Down z, whole rows
				while (depth < NAVIGATION_MESH_RECT_MOST && quarter_z + depth < quarters_deep
					&& !(wet && (quarter_z + depth) % NAVIGATION_SUBCELLS == 0))
				{
					bool whole = true;

					for (int32_t i = 0; i < width && whole; i++)
					{
						int32_t above = at + (depth - 1) * build.span + i;
						int next = mesh_match(&build, grid[depth - 1][i], above, above + build.span, reach, step);

						// (and within a step of its neighbor along the row)
						if (next >= 0 && i > 0 && SDL_fabsf(build.height[next][above + build.span] - build.height[grid[depth][i - 1]][above + build.span - 1])
							> ((key & MESH_KEY_TOP) != 0 ? step : reach) + 1e-4f)
						{
							next = -1;
						}
						whole = next >= 0;
						grid[depth][i] = (uint8_t)SDL_max(next, 0);
					}
					if (!whole)
					{
						break;
					}
					depth++;
				}
				if (!mesh_grow((void **)&tile->rects, &tile->rect_capacity, tile->rect_count + 1, sizeof(navigation_rect_t)))
				{
					return false;
				}
				index = tile->rect_count++;
				rect = &tile->rects[index];
				SDL_zerop(rect);
				float height_low = 1e30f;
				float height_high = -1e30f;

				rect->x = (uint8_t)quarter_x;
				rect->z = (uint8_t)quarter_z;
				rect->width = (uint8_t)width;
				rect->depth = (uint8_t)depth;
				rect->flags = (uint8_t)(((key & MESH_KEY_TOP) != 0 ? 0 : NAVIGATION_RECT_GROUND) | ((key & MESH_KEY_WET) != 0 ? NAVIGATION_RECT_WET : 0)
					| ((key & MESH_KEY_ZONE) != 0 ? NAVIGATION_RECT_ZONE : 0));
				rect->small = 0;
				rect->clearance = NAVIGATION_CLEARANCE_OPEN;
				for (int32_t j = 0; j < depth; j++)
				{
					for (int32_t i = 0; i < width; i++)
					{
						int32_t there = at + j * build.span + i;
						int there_level = grid[j][i];
						float height = build.height[there_level][there];

						build.rect[there_level][there] = (int16_t)index;
						rect->clearance = SDL_min(rect->clearance, build.room[there_level][there]);
						height_low = SDL_min(height_low, height);
						height_high = SDL_max(height_high, height);
						if ((key & MESH_KEY_TOP) == 0)
						{
							cost_sum += navigation->walk_factors[navigation->ground_slope[(build.first_z + (quarter_z + j) / NAVIGATION_SUBCELLS)
								* navigation->size + build.first_x + (quarter_x + i) / NAVIGATION_SUBCELLS]];
						}
						else
						{
							cost_sum += 1.0f;
						}
					}
				}
				rect->height = height_low;
				// (flat: its heights all one; else how far apart, at least a unit)
				rect->height_span = height_high - height_low < 0.005f ? 0
					: (uint8_t)SDL_clamp(SDL_ceilf((height_high - height_low) / NAVIGATION_RECT_SPAN_UNIT_METERS), 1.0f, 255.0f);
				rect->cost = (uint8_t)SDL_clamp((cost_sum / (float)(width * depth) - 1.0f) * 32.0f + 0.5f, 0.0f, 255.0f);
				// (an uneven top: its heights kept, a quarter each; the
				// ground's are the terrain's)
				if (rect->height_span != 0 && (key & MESH_KEY_TOP) != 0)
				{
					if (!mesh_grow((void **)&tile->uneven_rects, &tile->uneven_capacity, tile->uneven_count + 1, sizeof(uint16_t))
						|| !mesh_grow((void **)&tile->uneven_first, &tile->uneven_first_capacity, tile->uneven_count + 1, sizeof(uint32_t))
						|| !mesh_grow((void **)&tile->uneven_heights, &tile->uneven_height_capacity, tile->uneven_height_count + width * depth,
							sizeof(float)))
					{
						return false;
					}
					tile->uneven_rects[tile->uneven_count] = (uint16_t)index;
					tile->uneven_first[tile->uneven_count++] = (uint32_t)tile->uneven_height_count;
					for (int32_t j = 0; j < depth; j++)
					{
						for (int32_t i = 0; i < width; i++)
						{
							tile->uneven_heights[tile->uneven_height_count++] = build.height[grid[j][i]][at + j * build.span + i];
						}
					}
				}
			}
		}
	}
	if (!mesh_fill_buckets(tile))
	{
		return false;
	}
	tile->built = true;
	tile->generation = navigation->region_generation[region];
	// The rectangles small things stand in (within a body's half width of
	// one): a way through them looks out for them
	for (int32_t k = 0; k < navigation->regions[region].capsule_count; k++)
	{
		const navigation_capsule_t *capsule = &navigation->regions[region].capsules[k];
		float reach = capsule->radius + class->radius;
		float capsule_low[2] = { SDL_min(capsule->a[0], capsule->b[0]) - reach, SDL_min(capsule->a[2], capsule->b[2]) - reach };
		float capsule_high[2] = { SDL_max(capsule->a[0], capsule->b[0]) + reach, SDL_max(capsule->a[2], capsule->b[2]) + reach };

		// (the rectangles of the squares it reaches into: a forest's tile
		// has a thousand of each)
		float tile_x = (float)(region % navigation->regions_per_side) * NAVIGATION_REGION_METERS;
		float tile_z = (float)(region / navigation->regions_per_side) * NAVIGATION_REGION_METERS;
		float bucket_meters = NAVIGATION_REGION_METERS / (float)NAVIGATION_MESH_BUCKETS;
		int first_bucket_x = clamp_int((int)SDL_floorf((capsule_low[0] - tile_x) / bucket_meters) - 1, 0, NAVIGATION_MESH_BUCKETS - 1);
		int first_bucket_z = clamp_int((int)SDL_floorf((capsule_low[1] - tile_z) / bucket_meters) - 1, 0, NAVIGATION_MESH_BUCKETS - 1);
		int last_bucket_x = clamp_int((int)SDL_floorf((capsule_high[0] - tile_x) / bucket_meters) + 1, 0, NAVIGATION_MESH_BUCKETS - 1);
		int last_bucket_z = clamp_int((int)SDL_floorf((capsule_high[1] - tile_z) / bucket_meters) + 1, 0, NAVIGATION_MESH_BUCKETS - 1);

		if (capsule_high[0] < tile_x || capsule_high[1] < tile_z || capsule_low[0] > tile_x + NAVIGATION_REGION_METERS
			|| capsule_low[1] > tile_z + NAVIGATION_REGION_METERS)
		{
			continue;
		}
		for (int bucket_z = first_bucket_z; bucket_z <= last_bucket_z; bucket_z++)
		{
			for (int bucket_x = first_bucket_x; bucket_x <= last_bucket_x; bucket_x++)
			{
				int bucket = bucket_z * NAVIGATION_MESH_BUCKETS + bucket_x;

				for (uint32_t at = tile->bucket_first[bucket]; at < tile->bucket_first[bucket + 1]; at++)
				{
					int32_t r = tile->bucket_rects[at];
					navigation_rect_t *rect = &tile->rects[r];
					float low[2];
					float high[2];

					mesh_rect_box(navigation, c, (region << 16) | r, low, high);
					if (low[0] <= capsule_high[0] && high[0] >= capsule_low[0] && low[1] <= capsule_high[1] && high[1] >= capsule_low[1]
						&& rect->height <= SDL_max(capsule->a[1], capsule->b[1])
						&& rect_height_high(rect) + class->height >= SDL_min(capsule->a[1], capsule->b[1]))
					{
						rect->small = 1;
					}
				}
			}
		}
	}
	tile->margin_signature = mesh_margin_signature(navigation, region);
	tile->class_epoch = class->epoch;
	tile->water_signature = mesh_water_signature(navigation, water, region);
	tile->water_checked_ticks = SDL_GetTicks();
	// Each rectangle's valid neighbors within the tile, together (the ones
	// in the tiles round it are the border's)
	for (int32_t r = 0; r < tile->rect_count; r++)
	{
		int32_t list[MESH_SIDE_NEIGHBORS_MOST];
		int count = 0;

		tile->rects[r].first_neighbor = (uint32_t)tile->neighbor_count;
		for (int side = 0; side < 4; side++)
		{
			int before = count;

			mesh_side_neighbors(navigation, c, (region << 16) | r, side, region, list, &count);
			if (!mesh_grow((void **)&tile->neighbors, &tile->neighbor_capacity, tile->neighbor_count + count - before, sizeof(uint16_t)))
			{
				return false;
			}
			for (int n = before; n < count; n++)
			{
				tile->neighbors[tile->neighbor_count++] = (uint16_t)((list[n] & NAVIGATION_MESH_TILE_RECTS_MOST) | (side << 14));
			}
		}
		tile->rects[r].neighbor_count = (uint16_t)((uint32_t)tile->neighbor_count - tile->rects[r].first_neighbor);
	}
	for (int side = 0; side < 4; side++)
	{
		mesh_link_border(navigation, c, region, side);
	}
#pragma omp critical(navigation_tile_built)
	{
		navigation->mesh.tiles_built++;
		tile->build_serial = (uint32_t)navigation->mesh.tiles_built;
		navigation->mesh.last_build_milliseconds = (float)((double)(SDL_GetPerformanceCounter() - start_ticks) * 1000.0
			/ (double)SDL_GetPerformanceFrequency());
		frame_profile_count(FRAME_NAVIGATION_TILES, 1);
		frame_profile_count(FRAME_NAVIGATION_BUILD_MICROSECONDS, (int32_t)(navigation->mesh.last_build_milliseconds * 1000.0f));
	}
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
	// (the regions round it changed, or built since: a body's points by
	// its edge reach into them)
	needed = needed || tile->margin_signature != mesh_margin_signature(navigation, region);
	if (needed && !mesh_build_tile(navigation, c, region, water))
	{
		tile->built = false;
		return false;
	}
	tile->checked_search = navigation->mesh.search_number;
	return true;
}

// What a rectangle is to an agent now: as node_info's; one with water
// on it at the build (a cell's own: they are not joined) with the water
// over that cell as it is
static void mesh_rect_info(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int c, int32_t id, node_info_t *info)
{
	const navigation_rect_t *rect = mesh_rect(navigation, c, id);

	info->passable = roomy(rect->clearance, agent);
	// (under a blocker, in a doorway: whom its cell stops now)
	if ((rect->flags & NAVIGATION_RECT_ZONE) != 0 && info->passable)
	{
		mesh_box_t box = mesh_box(navigation, c, id);

		info->passable = (navigation->blocked[SDL_clamp(box.z >> 2, 0, navigation->size - 1) * navigation->size
			+ SDL_clamp(box.x >> 2, 0, navigation->size - 1)] & agent->groups) == 0;
	}
	info->water_class = CLASS_DRY;
	info->cost_factor = 1.0f + (float)rect->cost / 32.0f;
	info->height = rect->height;
	info->small = false;
	info->surface = (rect->flags & NAVIGATION_RECT_GROUND) == 0;
	info->extra_cost = 0.0f;
	if ((rect->flags & NAVIGATION_RECT_WET) != 0)
	{
		mesh_box_t box = mesh_box(navigation, c, id);
		int32_t cell_x = SDL_clamp(box.x >> 2, 0, navigation->size - 1);
		int32_t cell_z = SDL_clamp(box.z >> 2, 0, navigation->size - 1);
		float terrain_height = navigation->ground_height[cell_z * navigation->size + cell_x];

		if (!info->surface)
		{
			info->height = terrain_height;
		}
		info_water(navigation, terrain, water, agent, cell_x, cell_z, terrain_height, info);
	}
}

// The rectangle of a class at a point, on the level nearest the height
// that the agent can be on (tiles readied); -1 if none. reach_quarters:
// if there is none at the point's own quarter (its body's middle by a
// wall, pushed where it does not fit), the nearest this far round, one
// its middle gets to in a straight line if there is one near (not the
// one beyond the wall)
#define MESH_LOCATE_LINES_MOST 6
#define MESH_LOCATE_KEPT 8
static bool center_line_free(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	const float from[2], const float to[2], float height);
static int32_t mesh_locate(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float x, float z, float height, int32_t reach_quarters)
{
	int c = mesh_class_of(agent);
	int32_t quarter_x;
	int32_t quarter_z;
	int32_t tile_quarters = navigation->region_size * NAVIGATION_SUBCELLS;
	int32_t first_region_x;
	int32_t first_region_z;
	int32_t last_region_x;
	int32_t last_region_z;
	// (the best few, nearest first)
	int32_t kept[MESH_LOCATE_KEPT];
	float kept_score[MESH_LOCATE_KEPT];
	float kept_point[MESH_LOCATE_KEPT][2];
	int kept_count = 0;
	float from[2] = { x, z };
	int lines = 0;

	mesh_quarter_at(c, x, z, &quarter_x, &quarter_z);
	first_region_x = SDL_clamp((quarter_x - reach_quarters) / tile_quarters, 0, navigation->regions_per_side - 1);
	first_region_z = SDL_clamp((quarter_z - reach_quarters) / tile_quarters, 0, navigation->regions_per_side - 1);
	last_region_x = SDL_clamp((quarter_x + reach_quarters) / tile_quarters, 0, navigation->regions_per_side - 1);
	last_region_z = SDL_clamp((quarter_z + reach_quarters) / tile_quarters, 0, navigation->regions_per_side - 1);
	for (int32_t region_z = first_region_z; region_z <= last_region_z; region_z++)
	{
		for (int32_t region_x = first_region_x; region_x <= last_region_x; region_x++)
		{
			int32_t region = region_z * navigation->regions_per_side + region_x;
			const navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[region];
			int32_t local_x = quarter_x - region_x * tile_quarters;
			int32_t local_z = quarter_z - region_z * tile_quarters;
			int32_t first_bucket_x = SDL_clamp((local_x - reach_quarters) >> 4, 0, NAVIGATION_MESH_BUCKETS - 1);
			int32_t first_bucket_z = SDL_clamp((local_z - reach_quarters) >> 4, 0, NAVIGATION_MESH_BUCKETS - 1);
			int32_t last_bucket_x = SDL_clamp((local_x + reach_quarters) >> 4, 0, NAVIGATION_MESH_BUCKETS - 1);
			int32_t last_bucket_z = SDL_clamp((local_z + reach_quarters) >> 4, 0, NAVIGATION_MESH_BUCKETS - 1);

			if (!mesh_tile_ready(navigation, c, region, water))
			{
				continue;
			}
			for (int32_t bucket_z = first_bucket_z; bucket_z <= last_bucket_z; bucket_z++)
			{
				for (int32_t bucket_x = first_bucket_x; bucket_x <= last_bucket_x; bucket_x++)
				{
					int32_t bucket = bucket_z * NAVIGATION_MESH_BUCKETS + bucket_x;

					for (uint32_t k = tile->bucket_first[bucket]; k < tile->bucket_first[bucket + 1]; k++)
					{
						const navigation_rect_t *rect = &tile->rects[tile->bucket_rects[k]];
						int32_t id = (region << 16) | tile->bucket_rects[k];
						// (its quarter nearest the point's)
						int32_t near_x = SDL_clamp(local_x, rect->x, rect->x + rect->width - 1);
						int32_t near_z = SDL_clamp(local_z, rect->z, rect->z + rect->depth - 1);
						bool own = near_x == local_x && near_z == local_z;
						float point[2] = { own ? x : mesh_quarter_middle(c, near_x + region_x * tile_quarters),
							own ? z : mesh_quarter_middle(c, near_z + region_z * tile_quarters) };
						float off_squared = (point[0] - x) * (point[0] - x) + (point[1] - z) * (point[1] - z);
						node_info_t info;
						float difference;
						float score;
						int at;
						bool known = false;

						if (SDL_abs(near_x - local_x) > reach_quarters || SDL_abs(near_z - local_z) > reach_quarters)
						{
							continue;
						}
						for (int n = 0; n < kept_count && !known; n++)
						{
							known = kept[n] == id;
						}
						if (known)
						{
							continue;
						}
						mesh_rect_info(navigation, terrain, water, agent, c, id, &info);
						if (!info.passable)
						{
							continue;
						}
						// (out of its reach in height last: a deck a meter up over
						// it is not where it stands; then how far, then in height)
						difference = SDL_fabsf(mesh_height_at(navigation, c, id, near_x + region_x * tile_quarters, near_z + region_z * tile_quarters)
							- height);
						score = (difference > agent_reach(agent) ? 10000.0f : 0.0f) + off_squared * 100.0f + difference;
						at = kept_count;
						while (at > 0 && kept_score[at - 1] > score)
						{
							at--;
						}
						if (at >= MESH_LOCATE_KEPT)
						{
							continue;
						}
						for (int n = SDL_min(kept_count, MESH_LOCATE_KEPT - 1); n > at; n--)
						{
							kept[n] = kept[n - 1];
							kept_score[n] = kept_score[n - 1];
							kept_point[n][0] = kept_point[n - 1][0];
							kept_point[n][1] = kept_point[n - 1][1];
						}
						kept[at] = id;
						kept_score[at] = score;
						kept_point[at][0] = point[0];
						kept_point[at][1] = point[1];
						kept_count = SDL_min(kept_count + 1, MESH_LOCATE_KEPT);
					}
				}
			}
		}
	}
	for (int n = 0; n < kept_count && kept_score[n] < 10000.0f; n++)
	{
		// (its own quarter's: there. Else the nearest it has a straight
		// line to, of the near ones)
		if ((kept_point[n][0] == x && kept_point[n][1] == z) || (lines < MESH_LOCATE_LINES_MOST
			&& center_line_free(navigation, terrain, water, agent, from, kept_point[n], height)))
		{
			return kept[n];
		}
		lines++;
	}
	return kept_count > 0 ? kept[0] : -1;
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

// A rectangle's valid neighbors: the ones in its own tile and the ones
// over its tile's sides (their tiles built), each with the side of the
// rectangle it is over; how many
#define MESH_NEIGHBORS_MOST 1024
static int mesh_neighbors(const navigation_t *navigation, int c, int32_t id, int32_t *ids_out, uint8_t *sides_out)
{
	int32_t region = id >> 16;
	const navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[region];
	const navigation_rect_t *rect = &tile->rects[id & 0xFFFF];
	int count = 0;

	for (uint32_t k = 0; k < rect->neighbor_count && count < MESH_NEIGHBORS_MOST; k++)
	{
		uint16_t entry = tile->neighbors[rect->first_neighbor + k];

		ids_out[count] = (region << 16) | (entry & NAVIGATION_MESH_TILE_RECTS_MOST);
		sides_out[count++] = (uint8_t)(entry >> 14);
	}
	for (int side = 0; side < 4; side++)
	{
		int32_t neighbor;

		if (tile->border_count[side] == 0 || !rect_by_side(navigation, rect, region, side))
		{
			continue;
		}
		region_neighbor(navigation, region, side, &neighbor);
		if (neighbor < 0 || !navigation->mesh.classes[c].tiles[neighbor].built)
		{
			continue;
		}
		for (int32_t k = 0; k < tile->border_count[side] && count < MESH_NEIGHBORS_MOST; k++)
		{
			if (tile->border[side][k].from == (uint16_t)(id & 0xFFFF)
				&& tile->border[side][k].to < navigation->mesh.classes[c].tiles[neighbor].rect_count)
			{
				ids_out[count] = (neighbor << 16) | tile->border[side][k].to;
				sides_out[count++] = (uint8_t)side;
			}
		}
	}
	return count;
}

// The step from a rectangle to its neighbor at one quarter along the side
// they share
static float mesh_step_at(const navigation_t *navigation, int c, int32_t from, int32_t to, const mesh_shared_t *shared, int32_t along)
{
	bool by_x = shared->side < 2;

	return SDL_fabsf(mesh_height_at(navigation, c, to, by_x ? shared->beyond : along, by_x ? along : shared->beyond)
		- mesh_height_at(navigation, c, from, by_x ? shared->inside : along, by_x ? along : shared->inside));
}

// The crossing from a rectangle to a neighbor over one of its sides, for
// an agent: the part of the side they share (portal_out: its ends, the
// gap between the two), where along it the agent crosses (between the
// points of it nearest two given points: where it came in and where it
// is going) and the step there. Whether it walks across there
// (walk_out): they touch and the step is one it takes (an object's top:
// its step; the ground to the ground: its slope); where the step changes
// along the side (the ground along a terrace) the crossing is narrowed to
// the stretch it takes, the nearest such to where it would cross. False:
// they share nothing
static bool mesh_crossing(const navigation_t *navigation, int c, const navigation_agent_t *agent, int32_t from, int32_t to, int side,
	const float near_a[2], const float near_b[2], navigation_portal_t *portal_out, float crossing_out[2], bool *walk_out)
{
	const float quarter = NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS;
	const navigation_rect_t *from_rect = mesh_rect(navigation, c, from);
	const navigation_rect_t *to_rect = mesh_rect(navigation, c, to);
	mesh_shared_t shared;
	bool cross = (from_rect->flags & NAVIGATION_RECT_GROUND) == 0 || (to_rect->flags & NAVIGATION_RECT_GROUND) == 0;
	bool level = cross && from_rect->height_span == 0 && to_rect->height_span == 0 && (from_rect->flags & NAVIGATION_RECT_GROUND) == 0
		&& (to_rect->flags & NAVIGATION_RECT_GROUND) == 0;
	float limit = cross ? agent->step_meters : agent_reach(agent);
	float edge;
	float toward_a[2];
	float toward_b[2];
	float wanted[2];
	int32_t at;
	int32_t run_first;
	int32_t run_last;
	float rise;
	bool walk;

	if (!mesh_shared(navigation, c, from, to, side, &shared))
	{
		return false;
	}
	edge = mesh_quarter_low(c, side == 0 || side == 2 ? shared.inside + 1 : shared.inside);
	portal_out->from = from;
	portal_out->to = to;
	portal_out->cross_surface = cross;
	portal_out->gap = (uint8_t)shared.gap;
	portal_out->a[side < 2 ? 0 : 1] = edge;
	portal_out->b[side < 2 ? 0 : 1] = edge;
	portal_out->a[side < 2 ? 1 : 0] = mesh_quarter_low(c, shared.first);
	portal_out->b[side < 2 ? 1 : 0] = mesh_quarter_low(c, shared.last);
	segment_nearest(portal_out->a, portal_out->b, near_a, toward_a);
	segment_nearest(portal_out->a, portal_out->b, near_b, toward_b);
	wanted[0] = 0.5f * (toward_a[0] + toward_b[0]);
	wanted[1] = 0.5f * (toward_a[1] + toward_b[1]);
	at = clamp_int((int32_t)SDL_floorf((wanted[side < 2 ? 1 : 0] + mesh_offset(c)) / quarter), shared.first, shared.last - 1);
	run_first = shared.first;
	run_last = shared.last;
	rise = mesh_step_at(navigation, c, from, to, &shared, at);
	walk = shared.gap == 0 && rise <= limit;
	// (all of both within the step of each other: any of it is taken)
	level = level || SDL_max(rect_height_high(from_rect), rect_height_high(to_rect)) - SDL_min(from_rect->height, to_rect->height) <= limit;
	// (the step not the same all along: the stretch of it it takes)
	if (shared.gap == 0 && !level)
	{
		if (!walk)
		{
			// (the nearest quarter along the side that it does take)
			for (int32_t off = 1; off < shared.last - shared.first && !walk; off++)
			{
				for (int sign = -1; sign <= 1 && !walk; sign += 2)
				{
					int32_t k = at + sign * off;

					if (k >= shared.first && k < shared.last && mesh_step_at(navigation, c, from, to, &shared, k) <= limit)
					{
						at = k;
						rise = mesh_step_at(navigation, c, from, to, &shared, k);
						walk = true;
					}
				}
			}
		}
		if (walk)
		{
			run_first = at;
			run_last = at + 1;
			while (run_first > shared.first && mesh_step_at(navigation, c, from, to, &shared, run_first - 1) <= limit)
			{
				run_first--;
			}
			while (run_last < shared.last && mesh_step_at(navigation, c, from, to, &shared, run_last) <= limit)
			{
				run_last++;
			}
			portal_out->a[side < 2 ? 1 : 0] = mesh_quarter_low(c, run_first);
			portal_out->b[side < 2 ? 1 : 0] = mesh_quarter_low(c, run_last);
		}
	}
	portal_out->rise = rise;
	segment_nearest(portal_out->a, portal_out->b, wanted, crossing_out);
	*walk_out = walk;
	return true;
}

// The searches' notes (navigation_mesh_visit_t): the one of a rectangle
// in the search going on, NULL if it has none
static navigation_mesh_visit_t *mesh_visit_find(const navigation_t *navigation, int32_t id)
{
	const navigation_mesh_t *mesh = &navigation->mesh;
	uint32_t mask = (uint32_t)mesh->visit_capacity - 1u;

	if (mesh->visits == NULL)
	{
		return NULL;
	}
	for (uint32_t at = ((uint32_t)id * 2654435761u) & mask;; at = (at + 1u) & mask)
	{
		navigation_mesh_visit_t *visit = &mesh->visits[at];

		if (visit->search != mesh->visit_search)
		{
			return NULL;
		}
		if (visit->rect == id)
		{
			return visit;
		}
	}
}

// The note of a rectangle in the search going on, made if it has none
// (not reached yet: a cost beyond any); NULL if there is no memory. A
// note made may move the others: none is kept over this
static navigation_mesh_visit_t *mesh_visit_make(navigation_t *navigation, int32_t id)
{
	navigation_mesh_t *mesh = &navigation->mesh;
	navigation_mesh_visit_t *visit = mesh_visit_find(navigation, id);
	uint32_t mask;

	if (visit != NULL)
	{
		return visit;
	}
	// (half full: twice as many, this search's put in again)
	if (mesh->visits == NULL || 2 * (mesh->visit_count + 1) > mesh->visit_capacity)
	{
		int32_t capacity = mesh->visits == NULL ? 1024 : mesh->visit_capacity * 2;
		navigation_mesh_visit_t *grown = SDL_calloc((size_t)capacity, sizeof(navigation_mesh_visit_t));

		if (grown == NULL)
		{
			return NULL;
		}
		for (int32_t k = 0; k < mesh->visit_capacity; k++)
		{
			if (mesh->visits[k].search == mesh->visit_search)
			{
				uint32_t at = ((uint32_t)mesh->visits[k].rect * 2654435761u) & ((uint32_t)capacity - 1u);

				while (grown[at].search == mesh->visit_search)
				{
					at = (at + 1u) & ((uint32_t)capacity - 1u);
				}
				grown[at] = mesh->visits[k];
			}
		}
		SDL_free(mesh->visits);
		mesh->visits = grown;
		mesh->visit_capacity = capacity;
	}
	mask = (uint32_t)mesh->visit_capacity - 1u;
	for (uint32_t at = ((uint32_t)id * 2654435761u) & mask;; at = (at + 1u) & mask)
	{
		visit = &mesh->visits[at];
		if (visit->search != mesh->visit_search)
		{
			SDL_zerop(visit);
			visit->rect = id;
			visit->search = mesh->visit_search;
			visit->cost = 1e30f;
			visit->came_from = -1;
			mesh->visit_count++;
			return visit;
		}
	}
}

// A search begins: no rectangle has a note
static void mesh_visits_begin(navigation_t *navigation)
{
	navigation_mesh_t *mesh = &navigation->mesh;

	mesh->visit_search++;
	mesh->visit_count = 0;
	// (the numbers gone round: the table cleared, an old note not taken
	// for this search's)
	if (mesh->visit_search == 0)
	{
		if (mesh->visits != NULL)
		{
			SDL_memset(mesh->visits, 0, sizeof(navigation_mesh_visit_t) * (size_t)mesh->visit_capacity);
		}
		mesh->visit_search = 1;
	}
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
// failed_leg_out). The rectangles are where a body of its class fits (its
// widest, lowest); this agent's own body, by its own guard's measure, is
// asked here: a net under the search, seldom needed
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

		// (a jump: not walked; on from where it lands)
		if (navigation->path_jumps[leg] != 0)
		{
			x = waypoints[leg][0];
			z = waypoints[leg][1];
			height = navigation->path_jump_heights[leg];
			leg++;
			continue;
		}
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
		// (by small things, which the rectangles do not have: the way was
		// led round them as well as it goes, mesh_lead_round; what is left
		// is the walker's own, as it comes to them. No fine way looked for
		// here at every trunk of a forest: that was most of a search's time)
		if (navigation->path_small[leg] != 0)
		{
			x += dx * share;
			z += dz * share;
			height = reached > 0.0f ? reached_height : height;
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

// An edge jumped (mesh_jump): this far from it on either side (and its
// radius), a drop further out by this much of its height; what it costs
// a search over the same way walked (meters, and per meter of height: a
// stair a few meters off is the way)
#define MESH_JUMP_EDGE_METERS 0.2f
#define MESH_JUMP_DROP_OUT_SHARE 0.25f
#define MESH_JUMP_COST_METERS 2.0f
#define MESH_JUMP_COST_PER_METER 2.0f
#define MESH_JUMPS_MOST 16             // in one way
#define MESH_JUMP_LEVEL_METERS 0.1f    // an object's top this near a level is that level's (the ground: within the slope's reach)

// Whether the agent's center can go from a point straight to another at
// about a height (the quarter meters: stood on all the way)
static bool center_line_free(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	const float from[2], const float to[2], float height)
{
	float dx = to[0] - from[0];
	float dz = to[1] - from[1];
	int samples = (int)SDL_ceilf(SDL_sqrtf(dx * dx + dz * dz) / BODY_LINE_STEP_METERS);

	for (int k = 0; k <= samples; k++)
	{
		float t = samples > 0 ? (float)k / (float)samples : 0.0f;

		if (!navigation_quarter_free(navigation, terrain, water, agent, from[0] + dx * t, from[1] + dz * t, height))
		{
			return false;
		}
	}
	return true;
}

// Whether the agent stands at a point on a level: the surface there it
// would be on from that height is the level's own (the same top; the
// ground as it slopes, if the level is the ground's), not the next one
// up within a step, nor the ground by a terrace's face for the terrace
// (within the slope's reach of it: the edge was found a quarter meter
// early, and the bear landed with its front paws on it)
static bool stands_on_level(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float x, float z, float level, bool level_ground)
{
	int32_t cell;
	int32_t sub_x;
	int32_t sub_z;
	float height;
	bool ground;

	// (by the rectangles: what is there is its class's rectangle or its
	// tile's fringe, as the quarter meters had it when the tile was built;
	// the ground's first, as quarter_stand_on looks)
	if (navigation->follow_mesh)
	{
		int c = mesh_class_of(agent);
		mesh_level_t levels[2 * MESH_LEVELS];
		int32_t quarter_x;
		int32_t quarter_z;
		int count;

		mesh_quarter_at(c, x, z, &quarter_x, &quarter_z);
		count = mesh_levels_at(navigation, c, quarter_x, quarter_z, levels);
		count += mesh_fringe_at(navigation, c, quarter_x, quarter_z, true, levels + count);
		for (int pass = 0; pass < 2; pass++)
		{
			for (int k = 0; k < count; k++)
			{
				if (levels[k].ground == (pass == 0)
					&& SDL_fabsf(levels[k].height - level) <= (levels[k].ground ? agent_reach(agent) : agent->step_meters + 0.05f))
				{
					return SDL_fabsf(levels[k].height - level) <= (levels[k].ground && level_ground ? agent_reach(agent) : MESH_JUMP_LEVEL_METERS);
				}
			}
		}
		return false;
	}
	return quarter_of_point(navigation, x, z, &cell, &sub_x, &sub_z)
		&& quarter_stand_on(navigation, terrain, water, agent, cell, sub_x, sub_z, level, &height, &ground)
		&& SDL_fabsf(height - level) <= (ground && level_ground ? agent_reach(agent) : MESH_JUMP_LEVEL_METERS);
}

// Whether a spot is one the agent's body has the room at, on a level (as
// where it walks: a brush or two)
static bool jump_spot(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	const float spot[2], float level, bool level_ground)
{
	float height;
	int tight;

	// (by the rectangles: on one of its class at that level, or a hair
	// off its edge)
	if (navigation->follow_mesh)
	{
		bool ground;

		return mesh_follow_level(navigation, terrain, water, agent, spot[0], spot[1], level, MESH_FOLLOW_SLACK_METERS, &height, &ground)
			&& SDL_fabsf(height - level) <= (ground && level_ground ? agent_reach(agent) : MESH_JUMP_LEVEL_METERS)
			&& stands_on_level(navigation, terrain, water, agent, spot[0], spot[1], level, level_ground);
	}
	tight = quarter_tightness(navigation, terrain, water, agent, spot[0], spot[1], level, &height);
	return tight >= 0 && tight <= MESH_VERIFY_BRUSHES && stands_on_level(navigation, terrain, water, agent, spot[0], spot[1], level, level_ground);
}

// Whether the agent jumps a portal it does not walk (a terrace's edge,
// up or down), crossing it by a point of it: where it leaves the ground
// and where it lands (straight across the edge, its radius and a little
// off it on either side, or the rectangles' own nearest quarters where
// it has no room there; a drop further out), the height there and how
// much more than a walk it costs. Between the two rectangles (the
// portal's gap: where a body's middle cannot be) every quarter is one it
// stands on at the level it leaves, then at the one it lands on: no post,
// no balustrade, no bench against the terrace's face; the edge is where
// the one becomes the other. It stands at both spots with room for its
// body, and its middle gets from each straight to the edge on its level
static bool mesh_jump(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int c, int32_t from, int32_t to, const navigation_portal_t *portal, const float crossing[2], float takeoff_out[2], float landing_out[2],
	float *landing_height_out, float *cost_out)
{
	const float quarter = NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS;
	float along[2] = { portal->b[0] - portal->a[0], portal->b[1] - portal->a[1] };
	float length = SDL_sqrtf(along[0] * along[0] + along[1] * along[1]);
	float across[2];
	float low[2];
	float high[2];
	float at;
	float edge[2];
	float inside[2];
	float outside[2];
	float rise;
	float off = agent->radius_meters + MESH_JUMP_EDGE_METERS;
	float most = SDL_max(agent->jump_up_meters, agent->jump_down_meters);
	int32_t quarter_x;
	int32_t quarter_z;
	int32_t from_quarters = 0;
	bool landing_side = false;
	mesh_level_t from_level;
	mesh_level_t to_level;

	if (most <= 0.0f || portal->rise > most || length < 1e-3f)
	{
		return false;
	}
	along[0] /= length;
	along[1] /= length;
	// (across the edge, out of the rectangle it leaves)
	mesh_rect_box(navigation, c, from, low, high);
	across[0] = -along[1];
	across[1] = along[0];
	if ((portal->a[0] - 0.5f * (low[0] + high[0])) * across[0] + (portal->a[1] - 0.5f * (low[1] + high[1])) * across[1] < 0.0f)
	{
		across[0] = -across[0];
		across[1] = -across[1];
	}
	// (by the middle of one of the portal's quarters)
	at = (crossing[0] - portal->a[0]) * along[0] + (crossing[1] - portal->a[1]) * along[1];
	at = (SDL_floorf(SDL_clamp(at, 0.0f, length - 0.01f) / quarter) + 0.5f) * quarter;
	edge[0] = portal->a[0] + along[0] * at;
	edge[1] = portal->a[1] + along[1] * at;
	inside[0] = edge[0] - across[0] * 0.5f * quarter;
	inside[1] = edge[1] - across[1] * 0.5f * quarter;
	outside[0] = edge[0] + across[0] * ((float)portal->gap + 0.5f) * quarter;
	outside[1] = edge[1] + across[1] * ((float)portal->gap + 0.5f) * quarter;
	mesh_quarter_at(c, inside[0], inside[1], &quarter_x, &quarter_z);
	if (!mesh_level_of(navigation, c, from, quarter_x, quarter_z, &from_level))
	{
		return false;
	}
	mesh_quarter_at(c, outside[0], outside[1], &quarter_x, &quarter_z);
	if (!mesh_level_of(navigation, c, to, quarter_x, quarter_z, &to_level))
	{
		return false;
	}
	rise = to_level.height - from_level.height;
	if (rise > agent->jump_up_meters || -rise > agent->jump_down_meters)
	{
		return false;
	}
	// The quarters between: the level it leaves, then the one it lands on
	// (one it stands in on both, the lower under the upper: the upper's)
	for (int32_t k = 0; k < (int32_t)portal->gap; k++)
	{
		float x = edge[0] + across[0] * ((float)k + 0.5f) * quarter;
		float z = edge[1] + across[1] * ((float)k + 0.5f) * quarter;
		bool on_from = stands_on_level(navigation, terrain, water, agent, x, z, from_level.height, from_level.ground);
		bool on_to = stands_on_level(navigation, terrain, water, agent, x, z, to_level.height, to_level.ground);

		if (!on_from && !on_to)
		{
			return false;
		}
		if (rise > 0.0f ? !on_to : on_from)
		{
			if (landing_side)
			{
				return false;
			}
			from_quarters++;
		}
		else
		{
			landing_side = true;
		}
	}
	// (the edge itself: between two quarter meters, half a quarter on from
	// where an even class's quarters meet)
	edge[0] += across[0] * (float)from_quarters * quarter + SDL_fabsf(across[0]) * mesh_offset(c);
	edge[1] += across[1] * (float)from_quarters * quarter + SDL_fabsf(across[1]) * mesh_offset(c);
	takeoff_out[0] = edge[0] - across[0] * off;
	takeoff_out[1] = edge[1] - across[1] * off;
	if (!jump_spot(navigation, terrain, water, agent, takeoff_out, from_level.height, from_level.ground))
	{
		takeoff_out[0] = inside[0];
		takeoff_out[1] = inside[1];
		if (!jump_spot(navigation, terrain, water, agent, takeoff_out, from_level.height, from_level.ground))
		{
			return false;
		}
	}
	landing_out[0] = edge[0] + across[0] * off;
	landing_out[1] = edge[1] + across[1] * off;
	if (!jump_spot(navigation, terrain, water, agent, landing_out, to_level.height, to_level.ground))
	{
		landing_out[0] = outside[0];
		landing_out[1] = outside[1];
		if (!jump_spot(navigation, terrain, water, agent, landing_out, to_level.height, to_level.ground))
		{
			return false;
		}
	}
	// (down: further out, the way it falls; if it stands there too)
	if (rise < 0.0f)
	{
		float out = off - rise * MESH_JUMP_DROP_OUT_SHARE;
		float further[2] = { edge[0] + across[0] * out, edge[1] + across[1] * out };

		if ((further[0] - edge[0]) * across[0] + (further[1] - edge[1]) * across[1]
			> (landing_out[0] - edge[0]) * across[0] + (landing_out[1] - edge[1]) * across[1]
			&& jump_spot(navigation, terrain, water, agent, further, to_level.height, to_level.ground)
			&& center_line_free(navigation, terrain, water, agent, landing_out, further, to_level.height))
		{
			landing_out[0] = further[0];
			landing_out[1] = further[1];
		}
	}
	// (to the edge on its own level, each side: half a quarter short of it)
	{
		float by_edge[2] = { edge[0] - across[0] * 0.5f * quarter, edge[1] - across[1] * 0.5f * quarter };

		if (!center_line_free(navigation, terrain, water, agent, takeoff_out, by_edge, from_level.height))
		{
			return false;
		}
		by_edge[0] = edge[0] + across[0] * 0.5f * quarter;
		by_edge[1] = edge[1] + across[1] * 0.5f * quarter;
		if (!center_line_free(navigation, terrain, water, agent, landing_out, by_edge, to_level.height))
		{
			return false;
		}
	}
	*landing_height_out = to_level.height;
	*cost_out = MESH_JUMP_COST_METERS + MESH_JUMP_COST_PER_METER * SDL_fabsf(rise);
	return true;
}

static int mesh_search(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2],
	int *waypoint_count_out, bool *complete_out);

// ---------------------------------------------------------------------------
// Small things on a way: led round them

#define SMALL_CIRCLES_MOST 512
#define SMALL_SAMPLE_REACH_METERS 3.0f // of the stretches to and from a thing gone round, this much by it is looked at
#define SMALL_CLEAR_METERS 0.02f       // a body passes a small thing by this much at least
#define SMALL_ROUND_METERS 0.2f        // and is led round it this much further out
#define SMALL_LINE_METERS 0.1f         // a straight line (navigation_line_passable) may come this much nearer than a body's half width
#define SMALL_BY_METERS 0.5f           // a stretch passing a small thing this near is by it (path_small)
#define SMALL_ROUND_POINTS_MOST 64     // new points in one way
#define SMALL_ARC_RADIANS (0.5f * SDL_PI_F) // round a thing: a point for every quarter turn
#define SMALL_SAMPLE_METERS 0.25f      // the new stretches are looked at this often: on the rectangles?

// The small things round a box, for one so high (those out of the
// rectangles: small_out; near_only: none if the box is further than
// detail_meters from where anyone looks); along_from, along_to: only
// those within margin of the line from one to the other (a long way's
// box holds a forest)
static int small_circles(const navigation_t *navigation, float height, bool near_only, float margin, float minimum_x, float minimum_z,
	float maximum_x, float maximum_z, const float *along_from, const float *along_to, small_circle_t *circles_out, int most)
{
	// (a thing's capsule is kept by every region it reaches into; told by
	// the one its first end is in: margin further round than asked, which
	// is to be more than a thing's reach from its first end)
	int32_t first_x = clamp_int((int32_t)SDL_floorf((minimum_x - margin) / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int32_t first_z = clamp_int((int32_t)SDL_floorf((minimum_z - margin) / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int32_t last_x = clamp_int((int32_t)SDL_floorf((maximum_x + margin) / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int32_t last_z = clamp_int((int32_t)SDL_floorf((maximum_z + margin) / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int count = 0;

	if (!navigation->small_out)
	{
		return 0;
	}
	if (near_only && navigation->detail_meters > 0.0f)
	{
		float off_x = SDL_max(SDL_max(minimum_x - navigation->detail_x, navigation->detail_x - maximum_x), 0.0f);
		float off_z = SDL_max(SDL_max(minimum_z - navigation->detail_z, navigation->detail_z - maximum_z), 0.0f);

		if (off_x * off_x + off_z * off_z > navigation->detail_meters * navigation->detail_meters)
		{
			return 0;
		}
	}
	for (int32_t region_z = first_z; region_z <= last_z; region_z++)
	{
		for (int32_t region_x = first_x; region_x <= last_x; region_x++)
		{
			int32_t region_index = region_z * navigation->regions_per_side + region_x;
			const navigation_region_t *region = &navigation->regions[region_index];
			int first_square;
			int last_square;

			if (region->capsule_first == NULL || region->stale)
			{
				continue;
			}
			first_square = capsule_square(navigation, region_index, minimum_x - margin, minimum_z - margin);
			last_square = capsule_square(navigation, region_index, maximum_x + margin, maximum_z + margin);
			for (int square_z = first_square / NAVIGATION_CAPSULE_SQUARES; square_z <= last_square / NAVIGATION_CAPSULE_SQUARES; square_z++)
			{
				int first_column = first_square % NAVIGATION_CAPSULE_SQUARES;
				int last_column = last_square % NAVIGATION_CAPSULE_SQUARES;

				// (by a line: of this row of squares, only those the line
				// passes, margin to either side)
				if (along_from != NULL && along_to != NULL)
				{
					const float square_meters = NAVIGATION_REGION_METERS / (float)NAVIGATION_CAPSULE_SQUARES;
					float row_low = ((float)region_z * (float)NAVIGATION_CAPSULE_SQUARES + (float)square_z) * square_meters - margin;
					float row_high = row_low + square_meters + 2.0f * margin;
					float dz = along_to[1] - along_from[1];
					float from_share = 0.0f;
					float to_share = 1.0f;
					float x_from;
					float x_to;

					if (SDL_fabsf(dz) > 1e-5f)
					{
						float low_share = (row_low - along_from[1]) / dz;
						float high_share = (row_high - along_from[1]) / dz;

						from_share = SDL_max(from_share, SDL_min(low_share, high_share));
						to_share = SDL_min(to_share, SDL_max(low_share, high_share));
					}
					else if (along_from[1] < row_low || along_from[1] > row_high)
					{
						continue;
					}
					if (from_share > to_share)
					{
						continue;
					}
					x_from = along_from[0] + (along_to[0] - along_from[0]) * from_share;
					x_to = along_from[0] + (along_to[0] - along_from[0]) * to_share;
					first_column = SDL_max(first_column, capsule_square(navigation, region_index, SDL_min(x_from, x_to) - margin,
						0.0f) % NAVIGATION_CAPSULE_SQUARES);
					last_column = SDL_min(last_column, capsule_square(navigation, region_index, SDL_max(x_from, x_to) + margin,
						0.0f) % NAVIGATION_CAPSULE_SQUARES);
				}
				for (int square_x = first_column; square_x <= last_column; square_x++)
				{
					int square = square_z * NAVIGATION_CAPSULE_SQUARES + square_x;

					for (int32_t k = region->capsule_first[square]; k < region->capsule_first[square + 1] && count < most; k++)
					{
						const navigation_capsule_t *capsule = &region->capsules[k];
						int32_t own_x = clamp_int((int32_t)SDL_floorf(capsule->a[0] / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
						int32_t own_z = clamp_int((int32_t)SDL_floorf(capsule->a[2] / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
						float foot = SDL_min(capsule->a[1], capsule->b[1]);
						float a[2];
						float b[2];
						small_circle_t *circle = &circles_out[count];

						// (a soft thing: the walker's own business, as it goes)
						if (own_x != region_x || own_z != region_z || capsule->soft != 0
							|| !capsule_between(capsule, foot, foot + height, a, b))
						{
							continue;
						}
						circle->x = 0.5f * (a[0] + b[0]);
						circle->z = 0.5f * (a[1] + b[1]);
						SDL_memcpy(circle->a, a, sizeof(a));
						SDL_memcpy(circle->b, b, sizeof(b));
						circle->capsule_radius = capsule->radius;
						circle->radius = capsule->radius + 0.5f * SDL_sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
						if (circle->x + circle->radius + margin < minimum_x || circle->x - circle->radius - margin > maximum_x
							|| circle->z + circle->radius + margin < minimum_z || circle->z - circle->radius - margin > maximum_z)
						{
							continue;
						}
						if (along_from != NULL && along_to != NULL)
						{
							float dx = along_to[0] - along_from[0];
							float dz = along_to[1] - along_from[1];
							float length_squared = dx * dx + dz * dz;
							float share = length_squared > 1e-8f
								? SDL_clamp(((circle->x - along_from[0]) * dx + (circle->z - along_from[1]) * dz) / length_squared, 0.0f, 1.0f) : 0.0f;
							float off_x = along_from[0] + dx * share - circle->x;
							float off_z = along_from[1] + dz * share - circle->z;

							if (off_x * off_x + off_z * off_z > (circle->radius + margin) * (circle->radius + margin))
							{
								continue;
							}
						}
						count++;
					}
				}
			}
		}
	}
	return count;
}

// The first of the circles a stretch comes nearer than reach (over each
// one's own radius) to, neither end already that near it (nothing to
// lead round: it stands by it, or is to end by it), nor one of skip; -1:
// none
static int small_first_crossed(const small_circle_t *circles, int count, const float from[2], const float to[2], float reach, const int *skip,
	int skip_count)
{
	float dx = to[0] - from[0];
	float dz = to[1] - from[1];
	float length_squared = dx * dx + dz * dz;
	int first = -1;
	float first_along = 1e30f;

	for (int k = 0; k < count && length_squared > 1e-8f; k++)
	{
		const small_circle_t *circle = &circles[k];
		float radius = circle->radius + reach;
		float along = SDL_clamp(((circle->x - from[0]) * dx + (circle->z - from[1]) * dz) / length_squared, 0.0f, 1.0f);
		float near_x = from[0] + dx * along - circle->x;
		float near_z = from[1] + dz * along - circle->z;
		bool skipped = false;

		if (near_x * near_x + near_z * near_z >= radius * radius || along >= first_along
			|| (from[0] - circle->x) * (from[0] - circle->x) + (from[1] - circle->z) * (from[1] - circle->z) < radius * radius
			|| (to[0] - circle->x) * (to[0] - circle->x) + (to[1] - circle->z) * (to[1] - circle->z) < radius * radius)
		{
			continue;
		}
		for (int s = 0; s < skip_count && !skipped; s++)
		{
			skipped = skip[s] == k;
		}
		if (!skipped)
		{
			first = k;
			first_along = along;
		}
	}
	return first;
}

// Whether a stretch lies on the rectangles of a class all the way
static bool stretch_on_mesh(const navigation_t *navigation, int c, const float from[2], const float to[2])
{
	float dx = to[0] - from[0];
	float dz = to[1] - from[1];
	int samples = SDL_max((int)SDL_ceilf(SDL_sqrtf(dx * dx + dz * dz) / SMALL_SAMPLE_METERS), 1);

	for (int k = 1; k <= samples; k++)
	{
		mesh_level_t levels[MESH_LEVELS];
		int32_t quarter_x;
		int32_t quarter_z;

		mesh_quarter_at(c, from[0] + dx * (float)k / (float)samples, from[1] + dz * (float)k / (float)samples, &quarter_x, &quarter_z);
		if (mesh_levels_at(navigation, c, quarter_x, quarter_z, levels) == 0)
		{
			return false;
		}
	}
	return true;
}

// A stretch's far end brought to within reach of its near end
static void stretch_end_near(const float near[2], float far[2], float reach)
{
	float dx = far[0] - near[0];
	float dz = far[1] - near[1];
	float length = SDL_sqrtf(dx * dx + dz * dz);

	if (length > reach)
	{
		far[0] = near[0] + dx * reach / length;
		far[1] = near[1] + dz * reach / length;
	}
}

// The points that lead a stretch round a circle on one side (side 1:
// the angles growing): from where the line from its start touches the
// circle to where the line to its end does, a point for every quarter
// turn at most, each on the lines that touch the circle a half piece to
// either side of it (so the stretches between them touch it too, and
// the first and last lie on the lines from the start and to the end).
// How many (0: more than three quarters round, not this way); how far
// round, radians
static int round_points(const small_circle_t *circle, float radius, const float from[2], const float to[2], float side, float points_out[4][2],
	float *turn_out)
{
	float from_distance = SDL_sqrtf((from[0] - circle->x) * (from[0] - circle->x) + (from[1] - circle->z) * (from[1] - circle->z));
	float to_distance = SDL_sqrtf((to[0] - circle->x) * (to[0] - circle->x) + (to[1] - circle->z) * (to[1] - circle->z));
	float start = SDL_atan2f(from[1] - circle->z, from[0] - circle->x) + side * (from_distance > radius ? SDL_acosf(radius / from_distance) : 0.0f);
	float end = SDL_atan2f(to[1] - circle->z, to[0] - circle->x) - side * (to_distance > radius ? SDL_acosf(radius / to_distance) : 0.0f);
	float turn = SDL_fmodf(side * (end - start), 2.0f * SDL_PI_F);
	int pieces;
	float piece;
	float out;

	turn = turn < 0.0f ? turn + 2.0f * SDL_PI_F : turn;
	*turn_out = turn;
	if (turn > 1.5f * SDL_PI_F)
	{
		return 0;
	}
	pieces = clamp_int((int)SDL_ceilf(turn / SMALL_ARC_RADIANS), 1, 4);
	piece = turn / (float)pieces;
	out = radius / SDL_cosf(0.5f * piece);
	for (int k = 0; k < pieces; k++)
	{
		float angle = start + side * piece * ((float)k + 0.5f);

		points_out[k][0] = circle->x + SDL_cosf(angle) * out;
		points_out[k][1] = circle->z + SDL_sinf(angle) * out;
	}
	return pieces;
}

// A way led round the small things it crosses (they are not in the
// rectangles: the search went straight through them): for each stretch,
// the first thing in a body's way of it, gone round on the side with
// less of a turn if the new stretches lie on the rectangles, else on the
// other, else left (the walker's guard and its fine way, as it goes);
// the new stretches looked at again. path_small: the stretches by small
// things (a walker keeps its eyes open on them). False: no room left in
// the way for the points, cut_out waypoints of it kept (led round as far
// as that)
static bool mesh_lead_round(navigation_t *navigation, const navigation_agent_t *agent, float from_x, float from_z,
	float waypoints[NAVIGATION_MAX_WAYPOINTS][2], int *count, int *cut_out)
{
	static small_circle_t circles[SMALL_CIRCLES_MOST];
	int c = mesh_class_of(agent);
	float reach = agent->radius_meters + SMALL_CLEAR_METERS;
	int circle_count = 0;
	int added = 0;
	int skip[8];
	int skip_count = 0;
	int leg = 0;
	int gathered_for = 0;              // stretches from here on that the circles gathered are for

	SDL_memset(navigation->path_small, 0, sizeof(navigation->path_small));
	if (!navigation->small_out)
	{
		return true;
	}
	// A corner of the way inside a thing's berth (the end of a row of
	// them, turned round as close as the rectangles let it): out of it
	// first, straight away from the thing
	for (int k = 0; k + 1 < *count; k++)
	{
		if (navigation->path_jumps[k] != 0 || navigation->path_jumps[k + 1] != 0)
		{
			continue;
		}
		circle_count = small_circles(navigation, agent->height_meters, true, SMALL_LINE_MARGIN_METERS, waypoints[k][0], waypoints[k][1],
			waypoints[k][0], waypoints[k][1], NULL, NULL, circles, SMALL_CIRCLES_MOST);
		for (int n = 0; n < circle_count; n++)
		{
			float radius = circles[n].radius + reach + SMALL_ROUND_METERS;
			float dx = waypoints[k][0] - circles[n].x;
			float dz = waypoints[k][1] - circles[n].z;
			float distance = SDL_sqrtf(dx * dx + dz * dz);
			float moved[2];
			mesh_level_t levels[MESH_LEVELS];
			int32_t quarter_x;
			int32_t quarter_z;

			if (distance >= radius || distance < 1e-3f)
			{
				continue;
			}
			moved[0] = circles[n].x + dx * radius / distance;
			moved[1] = circles[n].z + dz * radius / distance;
			mesh_quarter_at(c, moved[0], moved[1], &quarter_x, &quarter_z);
			if (mesh_levels_at(navigation, c, quarter_x, quarter_z, levels) > 0)
			{
				waypoints[k][0] = moved[0];
				waypoints[k][1] = moved[1];
				navigation->path_small[k] = 1;
			}
			break;
		}
	}
	while (leg < *count)
	{
		float start[2] = { leg > 0 ? waypoints[leg - 1][0] : from_x, leg > 0 ? waypoints[leg - 1][1] : from_z };
		int crossed = -1;

		// (the things by this stretch of the way as it was found: once,
		// for it and for the stretches it is cut into round them, which
		// stay by it)
		if (gathered_for == 0)
		{
			circle_count = navigation->path_jumps[leg] != 0 ? 0 : small_circles(navigation, agent->height_meters, true, SMALL_MARGIN_METERS,
				SDL_min(start[0], waypoints[leg][0]), SDL_min(start[1], waypoints[leg][1]), SDL_max(start[0], waypoints[leg][0]),
				SDL_max(start[1], waypoints[leg][1]), start, waypoints[leg], circles, SMALL_CIRCLES_MOST);
			gathered_for = 1;
		}
		if (navigation->path_jumps[leg] == 0)
		{
			crossed = small_first_crossed(circles, circle_count, start, waypoints[leg], reach, skip, skip_count);
			// (by a small thing, if not through it: eyes open there too)
			if (crossed < 0 && small_first_crossed(circles, circle_count, start, waypoints[leg], reach + SMALL_BY_METERS, NULL, 0) >= 0)
			{
				navigation->path_small[leg] = 1;
			}
		}
		float points[2][4][2];
		float turns[2];
		int point_counts[2];
		int taken = -1;

		if (crossed < 0)
		{
			leg++;
			gathered_for--;
			skip_count = 0;
			continue;
		}
		navigation->path_small[leg] = 1;
		point_counts[0] = round_points(&circles[crossed], circles[crossed].radius + reach + SMALL_ROUND_METERS, start, waypoints[leg], 1.0f,
			points[0], &turns[0]);
		point_counts[1] = round_points(&circles[crossed], circles[crossed].radius + reach + SMALL_ROUND_METERS, start, waypoints[leg], -1.0f,
			points[1], &turns[1]);
		// (a point in the berth of the next thing over, a gap narrower
		// than two berths but wide enough for a body: through the middle
		// of the gap, as far from the one as from the other)
		for (int side = 0; side < 2; side++)
		{
			for (int k = 0; k < point_counts[side]; k++)
			{
				for (int n = 0; n < circle_count; n++)
				{
					float dx = circles[n].x - circles[crossed].x;
					float dz = circles[n].z - circles[crossed].z;
					float apart = SDL_sqrtf(dx * dx + dz * dz);
					float off_x = points[side][k][0] - circles[n].x;
					float off_z = points[side][k][1] - circles[n].z;
					float along;

					if (n == crossed || apart < 1e-3f || off_x * off_x + off_z * off_z >= (circles[n].radius + reach) * (circles[n].radius + reach))
					{
						continue;
					}
					along = circles[crossed].radius + 0.5f * (apart - circles[crossed].radius - circles[n].radius);
					points[side][k][0] = circles[crossed].x + dx * along / apart;
					points[side][k][1] = circles[crossed].z + dz * along / apart;
					break;
				}
			}
		}
		for (int choice = 0; choice < 2 && taken < 0; choice++)
		{
			int side = (turns[0] <= turns[1]) == (choice == 0) ? 0 : 1;
			bool on_mesh = point_counts[side] > 0;

			for (int k = 0; k <= point_counts[side] && on_mesh; k++)
			{
				float near_from[2];
				float near_to[2];

				// (of the stretches to and from the thing, the part by it:
				// further off they lie along the old one, which was on
				// the rectangles)
				SDL_memcpy(near_from, k == 0 ? start : points[side][k - 1], sizeof(near_from));
				SDL_memcpy(near_to, k == point_counts[side] ? waypoints[leg] : points[side][k], sizeof(near_to));
				if (k == 0)
				{
					stretch_end_near(near_to, near_from, SMALL_SAMPLE_REACH_METERS);
				}
				if (k == point_counts[side])
				{
					stretch_end_near(near_from, near_to, SMALL_SAMPLE_REACH_METERS);
				}
				on_mesh = stretch_on_mesh(navigation, c, near_from, near_to);
			}
			taken = on_mesh ? side : -1;
		}
		if (taken < 0 || added + point_counts[taken] > SMALL_ROUND_POINTS_MOST)
		{
			// (not gone round: the next one on this stretch)
			if (skip_count < (int)SDL_arraysize(skip))
			{
				skip[skip_count++] = crossed;
			}
			else
			{
				leg++;
				gathered_for--;
				skip_count = 0;
			}
			continue;
		}
		if (*count + point_counts[taken] > NAVIGATION_MAX_WAYPOINTS)
		{
			*cut_out = leg;
			return false;
		}
		SDL_memmove(waypoints[leg + point_counts[taken]], waypoints[leg], sizeof(float) * 2 * (size_t)(*count - leg));
		SDL_memmove(&navigation->path_jumps[leg + point_counts[taken]], &navigation->path_jumps[leg], (size_t)(*count - leg));
		SDL_memmove(&navigation->path_jump_heights[leg + point_counts[taken]], &navigation->path_jump_heights[leg],
			sizeof(float) * (size_t)(*count - leg));
		SDL_memmove(&navigation->path_small[leg + point_counts[taken]], &navigation->path_small[leg], (size_t)(*count - leg));
		for (int k = 0; k < point_counts[taken]; k++)
		{
			waypoints[leg + k][0] = points[taken][k][0];
			waypoints[leg + k][1] = points[taken][k][1];
			navigation->path_jumps[leg + k] = 0;
			navigation->path_jump_heights[leg + k] = 0.0f;
			navigation->path_small[leg + k] = 1;
		}
		*count += point_counts[taken];
		added += point_counts[taken];
		gathered_for += point_counts[taken];
		navigation->mesh.led_round++;
		// (the first new stretch looked at again; what was skipped on the
		// old one may be in the way of the new)
		skip_count = 0;
	}
	return true;
}

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
		int32_t failed_quarter_x;
		int32_t failed_quarter_z;
		int32_t at = -1;
		float at_off = 1e30f;
		int cut = 0;

		if (result > 0 && *waypoint_count_out > 0
			&& !mesh_lead_round(navigation, agent, from_x, from_z, waypoints_out, waypoint_count_out, &cut) && cut > 0)
		{
			// (no room in it for all the points round the small things on
			// it: as far as it is led round them, more after its last)
			*waypoint_count_out = cut;
			*complete_out = false;
		}
		// (a walker that keeps to the rectangles goes the way as it is
		// found: nothing of it is asked of the quarter meters)
		if (result <= 0 || *waypoint_count_out <= 0 || navigation->follow_mesh
			|| mesh_way_holds(navigation, terrain, water, agent, from_x, from_z, from_height, waypoints_out, *waypoint_count_out, failed,
				&failed_height, &failed_leg))
		{
			return result;
		}
		if ((failed[0] - from_x) * (failed[0] - from_x) + (failed[1] - from_z) * (failed[1] - from_z) < MESH_VERIFY_OWN_METERS * MESH_VERIFY_OWN_METERS
			|| !cell_of_point(navigation, failed[0], failed[1], &cell))
		{
			return result;
		}
		// The way's rectangle it got no further in: the one over that
		// point nearest its height there (a terrace over the ground: two of
		// them), the earlier of two as near
		mesh_quarter_at(c, failed[0], failed[1], &failed_quarter_x, &failed_quarter_z);
		for (int32_t k = 0; k < mesh_chain_length; k++)
		{
			const navigation_rect_t *rect = mesh_rect(navigation, c, mesh_chain[k]);
			float off;

			if (!rect_has_quarter(navigation, c, mesh_chain[k], failed_quarter_x, failed_quarter_z))
			{
				continue;
			}
			off = SDL_fabsf(SDL_clamp(failed_height, rect->height, rect_height_high(rect)) - failed_height);
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
			navigation->path_jumps[failed_leg] = 0;
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

// The rectangle of a door's end (the nearest to its point, a few quarter
// meters round: a door's ends are by its wall, where a body's middle may
// not be), found once a search. near: only if the end is by this
// rectangle (-1: wherever it is) at all (the others' ends are not looked
// for)
static int32_t mesh_link_rect(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	int link, int end, int32_t near)
{
	static int32_t rects[NAVIGATION_LINKS_MAX][2];
	static uint32_t searches[NAVIGATION_LINKS_MAX][2];
	const float *point = end == 0 ? navigation->links[link].a : navigation->links[link].b;

	if (near >= 0)
	{
		float low[2];
		float high[2];

		mesh_rect_box(navigation, mesh_class_of(agent), near, low, high);
		if (point[0] < low[0] - NAVIGATION_CELL_METERS || point[1] < low[1] - NAVIGATION_CELL_METERS || point[0] > high[0] + NAVIGATION_CELL_METERS
			|| point[1] > high[1] + NAVIGATION_CELL_METERS)
		{
			return -1;
		}
	}
	if (searches[link][end] != navigation->mesh.search_number)
	{
		searches[link][end] = navigation->mesh.search_number;
		rects[link][end] = mesh_locate(navigation, terrain, water, agent, point[0], point[1], navigation->ground_height[navigation->link_cells[link][end]],
			NAVIGATION_SUBCELLS);
	}
	return rects[link][end];
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
	int32_t goal_x;
	int32_t goal_z;
	int32_t start;
	int32_t best;
	float best_distance;
	bool found = false;
	int32_t chain_length = 0;
	float goal[2] = { to_x, to_z };
	// (the rectangles by the goal, each its point nearest it; the one got
	// to that is nearest, and how many more were looked at since)
	static struct
	{
		int32_t rect;
		float off;
		float point[2];
	} goal_near[4 * (2 * MESH_GOAL_NEAR_QUARTERS + 1) * (2 * MESH_GOAL_NEAR_QUARTERS + 1)];
	int goal_near_count;
	float goal_heights[MESH_LEVELS];
	bool goal_grounds[MESH_LEVELS];
	int goal_height_count = 0;
	int goal_at = -1;
	int32_t goal_more = 0;

	mesh_chain_length = 0;
	SDL_memset(navigation->path_jumps, 0, sizeof(navigation->path_jumps));
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
	start = mesh_locate(navigation, terrain, water, agent, from_x, from_z, from_height, MESH_START_NEAR_QUARTERS);
	// (on no rectangle, none beside it: pushed off them, onto a rock, into
	// a corner the class is kept out of; the nearest farther round, straight
	// to it: its way back onto them)
	if (start < 0)
	{
		start = mesh_locate(navigation, terrain, water, agent, from_x, from_z, from_height, MESH_START_FAR_QUARTERS);
	}
	navigation->mesh.searches++;
	if (start < 0)
	{
		navigation->mesh.search_fallbacks++;
		return -1;
	}
	// The goal: its quarter, and the ones a little way round it that are
	// a rectangle's (a goal by a wall, by a table: where a body's middle
	// cannot be; the nearest place it can is where it goes). Any level's:
	// the one it gets to (a roof over the goal is not it); round it only
	// what is level with something at the goal itself (the porch's floor
	// further from its wall, not the ground beside the porch)
	mesh_quarter_at(c, to_x, to_z, &goal_x, &goal_z);
	goal_near_count = 0;
	// (what is at the goal itself: the rectangles' levels there, and the
	// surfaces there a body does not fit on: a goal by a wall, under a
	// table; none: the levels nearest it)
	if (goal_x >= 0 && goal_z >= 0 && (goal_x >> 2) < navigation->size && (goal_z >> 2) < navigation->size
		&& mesh_tile_ready(navigation, c, region_of_cell(navigation, (goal_z >> 2) * navigation->size + (goal_x >> 2)), water))
	{
		mesh_level_t levels[2 * MESH_LEVELS];
		int count = mesh_levels_at(navigation, c, goal_x, goal_z, levels);

		count += mesh_fringe_at(navigation, c, goal_x, goal_z, false, levels + count);
		for (int k = 0; k < count && goal_height_count < MESH_LEVELS; k++)
		{
			goal_heights[goal_height_count] = levels[k].height;
			goal_grounds[goal_height_count++] = levels[k].ground;
		}
	}
	if (goal_height_count == 0)
	{
		float least = 1e30f;

		for (int pass = 0; pass < 2; pass++)
		{
			for (int32_t dz = -MESH_GOAL_NEAR_QUARTERS; dz <= MESH_GOAL_NEAR_QUARTERS; dz++)
			{
				for (int32_t dx = -MESH_GOAL_NEAR_QUARTERS; dx <= MESH_GOAL_NEAR_QUARTERS; dx++)
				{
					int32_t here_x = goal_x + dx;
					int32_t here_z = goal_z + dz;
					mesh_level_t levels[MESH_LEVELS];
					float off_x = mesh_quarter_middle(c, here_x) - to_x;
					float off_z = mesh_quarter_middle(c, here_z) - to_z;
					float off = off_x * off_x + off_z * off_z;
					int count;

					if (here_x < 0 || here_z < 0 || (here_x >> 2) >= navigation->size || (here_z >> 2) >= navigation->size
						|| (pass == 1 && off > least + 1e-4f)
						|| !mesh_tile_ready(navigation, c, region_of_cell(navigation, (here_z >> 2) * navigation->size + (here_x >> 2)), water))
					{
						continue;
					}
					count = mesh_levels_at(navigation, c, here_x, here_z, levels);
					if (pass == 0)
					{
						least = count > 0 ? SDL_min(least, off) : least;
						continue;
					}
					for (int k = 0; k < count && goal_height_count < MESH_LEVELS; k++)
					{
						goal_heights[goal_height_count] = levels[k].height;
						goal_grounds[goal_height_count++] = levels[k].ground;
					}
				}
			}
		}
	}
	for (int32_t dz = -MESH_GOAL_NEAR_QUARTERS; dz <= MESH_GOAL_NEAR_QUARTERS; dz++)
	{
		for (int32_t dx = -MESH_GOAL_NEAR_QUARTERS; dx <= MESH_GOAL_NEAR_QUARTERS; dx++)
		{
			int32_t here_x = goal_x + dx;
			int32_t here_z = goal_z + dz;
			mesh_level_t levels[MESH_LEVELS];
			int count;
			// (its own quarter: the goal itself)
			float off_x = dx == 0 && dz == 0 ? 0.0f : mesh_quarter_middle(c, here_x) - to_x;
			float off_z = dx == 0 && dz == 0 ? 0.0f : mesh_quarter_middle(c, here_z) - to_z;

			if (here_x < 0 || here_z < 0 || (here_x >> 2) >= navigation->size || (here_z >> 2) >= navigation->size
				|| !mesh_tile_ready(navigation, c, region_of_cell(navigation, (here_z >> 2) * navigation->size + (here_x >> 2)), water))
			{
				continue;
			}
			count = mesh_levels_at(navigation, c, here_x, here_z, levels);
			for (int k = 0; k < count; k++)
			{
				int at = -1;
				bool level = goal_height_count == 0 || (dx == 0 && dz == 0);

				for (int n = 0; n < goal_height_count && !level; n++)
				{
					level = SDL_fabsf(levels[k].height - goal_heights[n])
						<= (levels[k].ground && goal_grounds[n] ? agent_reach(agent) : agent->step_meters + 0.05f);
				}
				if (!level)
				{
					continue;
				}
				for (int n = 0; n < goal_near_count && at < 0; n++)
				{
					at = goal_near[n].rect == levels[k].rect ? n : -1;
				}
				if (at < 0 && goal_near_count < (int)SDL_arraysize(goal_near))
				{
					at = goal_near_count++;
					goal_near[at].rect = levels[k].rect;
					goal_near[at].off = 1e30f;
				}
				if (at >= 0 && off_x * off_x + off_z * off_z < goal_near[at].off)
				{
					goal_near[at].off = off_x * off_x + off_z * off_z;
					goal_near[at].point[0] = to_x + off_x;
					goal_near[at].point[1] = to_z + off_z;
				}
			}
		}
	}
	mesh_visits_begin(navigation);
	{
		navigation_mesh_visit_t *visit = mesh_visit_make(navigation, start);

		if (visit == NULL)
		{
			return -1;
		}
		visit->cost = 0.0f;
		visit->entry[0] = from_x;
		visit->entry[1] = from_z;
	}
	navigation->heap_count = 0;
	if (!heap_push(navigation, start, goal_distance))
	{
		return -1;
	}
	best = start;
	{
		float nearest[2];

		rect_nearest(navigation, c, start, to_x, to_z, nearest);
		best_distance = distance_2d(nearest, goal);
	}
	while (navigation->heap_count > 0)
	{
		static int32_t neighbor_ids[MESH_NEIGHBORS_MOST];
		static uint8_t neighbor_sides[MESH_NEIGHBORS_MOST];
		int32_t id = heap_pop(navigation);
		int32_t region = id >> 16;
		const navigation_rect_t *rect = mesh_rect(navigation, c, id);
		// (its note as it is now: making the next ones' may move it)
		navigation_mesh_visit_t visit;
		node_info_t here;
		int neighbor_count;

		{
			navigation_mesh_visit_t *own = mesh_visit_find(navigation, id);

			if (own == NULL || own->closed)
			{
				continue;
			}
			own->closed = true;
			visit = *own;
		}
		// By the goal: there, if at the goal itself; else a little further
		// is looked for one nearer it (MESH_GOAL_MORE_EXPANSIONS)
		for (int n = 0; n < goal_near_count; n++)
		{
			if (goal_near[n].rect == id && (goal_at < 0 || goal_near[n].off < goal_near[goal_at].off))
			{
				goal_at = n;
			}
		}
		if (goal_at >= 0 && (goal_near[goal_at].off < 1e-6f || ++goal_more > MESH_GOAL_MORE_EXPANSIONS))
		{
			break;
		}
		{
			float nearest[2];
			float distance;

			rect_nearest(navigation, c, id, to_x, to_z, nearest);
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
		mesh_rect_info(navigation, terrain, water, agent, c, id, &here);
		// The tiles round it that it touches, current before its neighbors
		// there are read (building one links its border both ways)
		for (int side = 0; side < 4; side++)
		{
			int32_t neighbor;

			if (rect_by_side(navigation, rect, region, side))
			{
				region_neighbor(navigation, region, side, &neighbor);
				if (neighbor >= 0)
				{
					mesh_tile_ready(navigation, c, neighbor, water);
				}
			}
		}
		neighbor_count = mesh_neighbors(navigation, c, id, neighbor_ids, neighbor_sides);
		for (int n = 0; n < neighbor_count; n++)
		{
			int32_t to = neighbor_ids[n];
			navigation_portal_t portal;
			navigation_mesh_visit_t *next = mesh_visit_find(navigation, to);
			node_info_t there;
			float crossing[2];
			float cost;
			float takeoff[2];
			float landing[2];
			float landing_height = 0.0f;
			float jump_cost = 0.0f;
			bool walk;
			bool jump = false;

			if (next != NULL && next->closed)
			{
				continue;
			}
			mesh_rect_info(navigation, terrain, water, agent, c, to, &there);
			if (!there.passable || (navigation->mesh.shut_count > 0 && mesh_is_shut(navigation, c, agent, id, to)))
			{
				continue;
			}
			// Where it crosses: between the points of the side they share
			// nearest where it came in and nearest the goal
			if (!mesh_crossing(navigation, c, agent, id, to, neighbor_sides[n], visit.entry, goal, &portal, crossing, &walk))
			{
				continue;
			}
			// (swimming on: any step; an edge it does not walk: one it
			// jumps, if it does)
			if (!walk && !(portal.gap == 0 && here.water_class == CLASS_SWIM && there.water_class == CLASS_SWIM))
			{
				if (here.water_class == CLASS_SWIM || there.water_class == CLASS_SWIM
					|| !mesh_jump(navigation, terrain, water, agent, c, id, to, &portal, crossing, takeoff, landing, &landing_height, &jump_cost))
				{
					continue;
				}
				jump = true;
			}
			cost = visit.cost + distance_2d(visit.entry, jump ? takeoff : crossing) * here.cost_factor + there.extra_cost + (jump ? jump_cost : 0.0f);
			if (next == NULL || cost < next->cost)
			{
				next = mesh_visit_make(navigation, to);
				if (next == NULL)
				{
					navigation->heap_count = 0;
					break;
				}
				next->cost = cost;
				next->came_from = id;
				next->entry[0] = jump ? landing[0] : crossing[0];
				next->entry[1] = jump ? landing[1] : crossing[1];
				next->came_a[0] = jump ? takeoff[0] : portal.a[0];
				next->came_a[1] = jump ? takeoff[1] : portal.a[1];
				next->came_b[0] = jump ? landing[0] : portal.b[0];
				next->came_b[1] = jump ? landing[1] : portal.b[1];
				next->came_by_door = false;
				next->came_by_jump = jump;
				next->came_height = jump ? landing_height : 0.0f;
				if (!heap_push(navigation, to, cost + HEURISTIC_WEIGHT * distance_2d(crossing, goal)))
				{
					navigation->heap_count = 0;
					break;
				}
			}
		}
		// Through a door from here (its ends on the ground), open or one it
		// opens, to the rectangle of its other end
		if (navigation->link_count > 0 && (rect->flags & NAVIGATION_RECT_GROUND) != 0 && here.passable)
		{
			for (int k = 0; k < navigation->link_count; k++)
			{
				int end = -1;
				int32_t other_cell;
				int32_t other;
				navigation_mesh_visit_t *next;
				node_info_t there;
				float cost;
				float door_a[2];
				float door_b[2];

				if (!navigation->links[k].open && !agent->opens_doors)
				{
					continue;
				}
				for (int e = 0; e < 2; e++)
				{
					if (mesh_link_rect(navigation, terrain, water, agent, k, e, id) == id)
					{
						end = e;
					}
				}
				if (end < 0)
				{
					continue;
				}
				other_cell = navigation->link_cells[k][1 - end];
				other = mesh_link_rect(navigation, terrain, water, agent, k, 1 - end, -1);
				if (other < 0 || other == id)
				{
					continue;
				}
				next = mesh_visit_find(navigation, other);
				if (next != NULL && next->closed)
				{
					continue;
				}
				mesh_rect_info(navigation, terrain, water, agent, c, other, &there);
				if (!there.passable || SDL_fabsf(navigation->ground_height[other_cell] - navigation->ground_height[navigation->link_cells[k][end]])
					> SDL_max(agent->step_meters, 0.6f) || (navigation->mesh.shut_count > 0 && mesh_is_shut(navigation, c, agent, id, other)))
				{
					continue;
				}
				door_a[0] = end == 0 ? navigation->links[k].a[0] : navigation->links[k].b[0];
				door_a[1] = end == 0 ? navigation->links[k].a[1] : navigation->links[k].b[1];
				door_b[0] = end == 0 ? navigation->links[k].b[0] : navigation->links[k].a[0];
				door_b[1] = end == 0 ? navigation->links[k].b[1] : navigation->links[k].a[1];
				cost = visit.cost + distance_2d(visit.entry, door_a) * here.cost_factor + navigation->link_meters[k]
					+ (navigation->links[k].open ? 0.0f : LINK_SHUT_METERS);
				if (next == NULL || cost < next->cost)
				{
					next = mesh_visit_make(navigation, other);
					if (next == NULL)
					{
						navigation->heap_count = 0;
						break;
					}
					next->cost = cost;
					next->came_from = id;
					next->entry[0] = door_b[0];
					next->entry[1] = door_b[1];
					next->came_a[0] = door_a[0];
					next->came_a[1] = door_a[1];
					next->came_b[0] = door_b[0];
					next->came_b[1] = door_b[1];
					next->came_by_door = true;
					next->came_by_jump = false;
					if (!heap_push(navigation, other, cost + HEURISTIC_WEIGHT * distance_2d(door_b, goal)))
					{
						navigation->heap_count = 0;
						break;
					}
				}
			}
		}
	}
	if (goal_at >= 0)
	{
		found = true;
		best = goal_near[goal_at].rect;
		to_x = goal_near[goal_at].point[0];
		to_z = goal_near[goal_at].point[1];
	}
	navigation->mesh.last_search_ticks = SDL_GetPerformanceCounter() - search_start;
	navigation->last_search_ticks = navigation->mesh.last_search_ticks;
	navigation->last_expansions = expansions;
	// The rectangles from the end back to the start
	for (int32_t id = best; id >= 0 && chain_length < (int32_t)SDL_arraysize(mesh_chain);)
	{
		const navigation_mesh_visit_t *visit = mesh_visit_find(navigation, id);

		chain[chain_length++] = id;
		id = visit != NULL ? visit->came_from : -1;
	}
	mesh_chain_length = chain_length;
	if (chain_length == 1 && !found)
	{
		// Nowhere nearer than its own rectangle: across it towards the goal,
		// if that is worth a step
		float end[2];
		float here[2] = { from_x, from_z };

		rect_nearest(navigation, c, start, to_x, to_z, end);
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
	// (looking from the rectangle it leaves into the next), half a quarter
	// meter off both ends; a door its two ends, one after the other
	{
		// (a jump's two ends are corners whatever the funnel makes of them:
		// the way is pulled tight up to where it leaves the ground, and
		// again on from where it lands; they are not merged with corners
		// beside them either)
		static uint8_t corner_jump[NAVIGATION_MAX_WAYPOINTS + 1];
		static uint8_t corner_fixed[NAVIGATION_MAX_WAYPOINTS + 1];
		static float corner_height[NAVIGATION_MAX_WAYPOINTS + 1];
		int portal_count = 0;
		float end[2];
		int corner_count = 0;
		bool more = false;

		SDL_memset(corner_jump, 0, sizeof(corner_jump));
		SDL_memset(corner_fixed, 0, sizeof(corner_fixed));
		lefts[portal_count][0] = from_x;
		lefts[portal_count][1] = from_z;
		rights[portal_count][0] = from_x;
		rights[portal_count][1] = from_z;
		portal_count++;
		for (int32_t k = chain_length - 2; k >= 0 && portal_count < (int)SDL_arraysize(lefts) - 3 && !more; k--)
		{
			// (the note of the rectangle it comes into: how it came there)
			const navigation_mesh_visit_t *to_rect = mesh_visit_find(navigation, chain[k]);
			float from_low[2];
			float from_high[2];

			if (to_rect == NULL)
			{
				break;
			}
			mesh_rect_box(navigation, c, chain[k + 1], from_low, from_high);

			if (to_rect->came_by_jump)
			{
				int count;

				lefts[portal_count][0] = to_rect->came_a[0];
				lefts[portal_count][1] = to_rect->came_a[1];
				rights[portal_count][0] = to_rect->came_a[0];
				rights[portal_count][1] = to_rect->came_a[1];
				portal_count++;
				count = mesh_funnel(lefts, rights, portal_count, corners + corner_count, NAVIGATION_MAX_WAYPOINTS - corner_count);
				if (count < 0 || corner_count + count >= NAVIGATION_MAX_WAYPOINTS)
				{
					// (no room for the landing: the path ends before the jump,
					// searched on from there)
					corner_count = count < 0 ? NAVIGATION_MAX_WAYPOINTS : corner_count + count;
					more = true;
					break;
				}
				corner_count += count;
				if (count > 0)
				{
					corner_fixed[corner_count - 1] = 1;
				}
				corners[corner_count][0] = to_rect->came_b[0];
				corners[corner_count][1] = to_rect->came_b[1];
				corner_jump[corner_count] = 1;
				corner_fixed[corner_count] = 1;
				corner_height[corner_count] = to_rect->came_height;
				corner_count++;
				portal_count = 0;
				lefts[portal_count][0] = to_rect->came_b[0];
				lefts[portal_count][1] = to_rect->came_b[1];
				rights[portal_count][0] = to_rect->came_b[0];
				rights[portal_count][1] = to_rect->came_b[1];
				portal_count++;
				continue;
			}
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
				float from_middle[2] = { 0.5f * (from_low[0] + from_high[0]), 0.5f * (from_low[1] + from_high[1]) };
				float portal_middle[2] = { 0.5f * (to_rect->came_a[0] + to_rect->came_b[0]), 0.5f * (to_rect->came_a[1] + to_rect->came_b[1]) };
				float direction[2] = { portal_middle[0] - from_middle[0], portal_middle[1] - from_middle[1] };
				float to_a[2] = { to_rect->came_a[0] - portal_middle[0], to_rect->came_a[1] - portal_middle[1] };
				// (a on the left: direction x (a - middle) above zero)
				bool a_left = direction[0] * to_a[1] - direction[1] * to_a[0] > 0.0f;
				float left[2];
				float right[2];
				float length = distance_2d(to_rect->came_a, to_rect->came_b);
				// (the middles of its end quarters: where a body's middle was tried)
				float inset = SDL_min(0.5f * NAVIGATION_CELL_METERS / (float)NAVIGATION_SUBCELLS, 0.5f * length);

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
			rect_nearest(navigation, c, best, to_x, to_z, end);
		}
		if (!more)
		{
			int count;

			lefts[portal_count][0] = end[0];
			lefts[portal_count][1] = end[1];
			rights[portal_count][0] = end[0];
			rights[portal_count][1] = end[1];
			portal_count++;
			count = mesh_funnel(lefts, rights, portal_count, corners + corner_count, NAVIGATION_MAX_WAYPOINTS - corner_count);
			more = count < 0;
			corner_count = count < 0 ? NAVIGATION_MAX_WAYPOINTS : corner_count + count;
		}
		if (more)
		{
			// More corners than a path holds: the first ones (written), searched
			// on from the last
			*complete_out = false;
		}
		// (corners a hair apart, where a corridor's portals meet at a
		// tile's edge: the later kept; a jump's ends always, the other one
		// left out)
		{
			static uint8_t kept_fixed[NAVIGATION_MAX_WAYPOINTS + 1];
			int kept = 0;

			for (int k = 0; k < corner_count; k++)
			{
				if (kept > 0 && distance_2d(waypoints_out[kept - 1], corners[k]) < 0.3f)
				{
					// (a walker that keeps to the rectangles, follow_mesh:
					// the earlier one stays if the way straight past it to
					// the later is not over them, a wall's corner between)
					float start[2] = { from_x, from_z };

					if (kept_fixed[kept - 1] == 0 && navigation->follow_mesh
						&& !stretch_on_mesh(navigation, c, kept > 1 ? waypoints_out[kept - 2] : start, corners[k]))
					{
					}
					else if (kept_fixed[kept - 1] == 0)
					{
						kept--;
					}
					else if (corner_fixed[k] == 0)
					{
						continue;
					}
				}
				waypoints_out[kept][0] = corners[k][0];
				waypoints_out[kept][1] = corners[k][1];
				kept_fixed[kept] = corner_fixed[k];
				navigation->path_jumps[kept] = corner_jump[k];
				navigation->path_jump_heights[kept] = corner_height[k];
				kept++;
			}
			for (int k = kept; k < NAVIGATION_MAX_WAYPOINTS; k++)
			{
				navigation->path_jumps[k] = 0;
			}
			*waypoint_count_out = kept;
		}
	}
	return found ? NAVIGATION_PATH_FOUND : NAVIGATION_PATH_PARTIAL;
}

// The straight line over the rectangles: from one to the next where it
// leaves it, through the portal there (the agent's rules for the next one
// and the step); inside one nothing is looked at: its body fits there.
// Off the rectangles it may be for its first stretch and its last (it
// stands by a wall, pushed where a body's middle is not planned; the line
// ends by a table), MESH_LINE_OFF_METERS at most: over the quarter meters
// there, each one stood on from the last
#define MESH_LINE_OFF_METERS 1.0f
static int mesh_line(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent, float from_x,
	float from_z, float from_height, float to_x, float to_z, float *blocked_x, float *blocked_z, float *blocked_height)
{
	int c = mesh_class_of(agent);
	int32_t id;
	float direction[2] = { to_x - from_x, to_z - from_z };
	float length = SDL_sqrtf(direction[0] * direction[0] + direction[1] * direction[1]);
	float travelled = 0.0f;
	float height = from_height;
	float stop[2] = { from_x, from_z };
	bool started_off;

	mesh_note_agent(navigation, agent);
	navigation->mesh.search_number++;
	if (length < 1e-5f)
	{
		return 1;
	}
	id = mesh_locate(navigation, terrain, water, agent, from_x, from_z, from_height, 0);
	// (one over or under where it is, not its own: off them)
	if (id >= 0)
	{
		const navigation_rect_t *rect = mesh_rect(navigation, c, id);

		if (SDL_fabsf(SDL_clamp(from_height, rect->height, rect_height_high(rect)) - from_height) > agent_reach(agent))
		{
			id = -1;
		}
	}
	started_off = id < 0;
	for (int step = 0; step < MESH_RAY_MOST_STEPS; step++)
	{
		const navigation_rect_t *rect;
		float low[2];
		float high[2];
		float leave = 1e30f;
		int side = -1;
		float exit[2];
		int32_t next = -1;
		node_info_t here;
		node_info_t there;
		int32_t region;
		bool any_beyond = false;

		if (id < 0)
		{
			float limit = SDL_min(1.0f, travelled + MESH_LINE_OFF_METERS / length);

			while (id < 0)
			{
				float amount = SDL_min(travelled + BODY_LINE_STEP_METERS / length, 1.0f);
				float x = from_x + direction[0] * amount;
				float z = from_z + direction[1] * amount;
				int32_t cell;
				int32_t sub_x;
				int32_t sub_z;
				float there_height;
				mesh_level_t levels[MESH_LEVELS];
				int count;
				int32_t quarter_x;
				int32_t quarter_z;

				// (by the rectangles alone, follow_mesh: off them only one
				// that started off them, on its way back; what is under it
				// there is not asked of the quarter meters)
				there_height = height;
				if (travelled >= limit || !quarter_of_point(navigation, x, z, &cell, &sub_x, &sub_z)
					|| (navigation->follow_mesh ? !started_off
						: !quarter_stand(navigation, terrain, water, agent, cell, sub_x, sub_z, height, &there_height)))
				{
					stop[0] = x;
					stop[1] = z;
					break;
				}
				height = there_height;
				travelled = amount;
				if (travelled >= 1.0f)
				{
					return 1;
				}
				// (onto a rectangle again, at the level it stands on)
				mesh_quarter_at(c, x, z, &quarter_x, &quarter_z);
				if (!mesh_tile_ready(navigation, c, region_of_cell(navigation, cell), water))
				{
					continue;
				}
				count = mesh_levels_at(navigation, c, quarter_x, quarter_z, levels);
				for (int k = 0; k < count && id < 0; k++)
				{
					mesh_rect_info(navigation, terrain, water, agent, c, levels[k].rect, &there);
					if (there.passable && SDL_fabsf(levels[k].height - height) <= (levels[k].ground ? agent_reach(agent) : agent->step_meters + 0.05f))
					{
						id = levels[k].rect;
					}
				}
			}
			if (id < 0)
			{
				break;
			}
			continue;
		}
		rect = mesh_rect(navigation, c, id);
		region = id >> 16;
		mesh_rect_box(navigation, c, id, low, high);
		if (to_x >= low[0] && to_x <= high[0] && to_z >= low[1] && to_z <= high[1])
		{
			return 1;
		}
		// Where the line leaves it (from the part left of it)
		if (direction[0] > 1e-6f)
		{
			float t = (high[0] - from_x) / direction[0];

			if (t < leave)
			{
				leave = t;
				side = 0;
			}
		}
		else if (direction[0] < -1e-6f)
		{
			float t = (low[0] - from_x) / direction[0];

			if (t < leave)
			{
				leave = t;
				side = 1;
			}
		}
		if (direction[1] > 1e-6f)
		{
			float t = (high[1] - from_z) / direction[1];

			if (t < leave)
			{
				leave = t;
				side = 2;
			}
		}
		else if (direction[1] < -1e-6f)
		{
			float t = (low[1] - from_z) / direction[1];

			if (t < leave)
			{
				leave = t;
				side = 3;
			}
		}
		if (side < 0 || leave < travelled - 1e-4f || leave >= 1.0f)
		{
			return 1;
		}
		travelled = leave;
		exit[0] = from_x + direction[0] * leave;
		exit[1] = from_z + direction[1] * leave;
		height = SDL_clamp(height, rect->height, rect_height_high(rect));
		// The neighbor over that side the point crosses into (the tile
		// beyond made current first): the one it walks into there, the
		// least step if there are two (the ground and a deck over it)
		if (rect_by_side(navigation, rect, region, side))
		{
			int32_t neighbor;

			region_neighbor(navigation, region, side, &neighbor);
			if (neighbor >= 0)
			{
				mesh_tile_ready(navigation, c, neighbor, water);
			}
		}
		mesh_rect_info(navigation, terrain, water, agent, c, id, &here);
		{
			static int32_t neighbor_ids[MESH_NEIGHBORS_MOST];
			static uint8_t neighbor_sides[MESH_NEIGHBORS_MOST];
			int neighbor_count = mesh_neighbors(navigation, c, id, neighbor_ids, neighbor_sides);
			float least = 1e30f;

			for (int n = 0; n < neighbor_count; n++)
			{
				navigation_portal_t portal;
				float crossing[2];
				bool walk;
				bool along_x = side < 2;

				if (neighbor_sides[n] != side
					|| !mesh_crossing(navigation, c, agent, id, neighbor_ids[n], side, exit, exit, &portal, crossing, &walk))
				{
					continue;
				}
				any_beyond = any_beyond || (portal.gap == 0
					&& (along_x ? exit[1] >= SDL_min(portal.a[1], portal.b[1]) - 1e-3f && exit[1] <= SDL_max(portal.a[1], portal.b[1]) + 1e-3f
						: exit[0] >= SDL_min(portal.a[0], portal.b[0]) - 1e-3f && exit[0] <= SDL_max(portal.a[0], portal.b[0]) + 1e-3f));
				mesh_rect_info(navigation, terrain, water, agent, c, neighbor_ids[n], &there);
				if (!there.passable || !(walk || (portal.gap == 0 && here.water_class == CLASS_SWIM && there.water_class == CLASS_SWIM)))
				{
					continue;
				}
				// (the stretch it walks across has the point in it)
				if (along_x ? (exit[1] >= SDL_min(portal.a[1], portal.b[1]) - 1e-3f && exit[1] <= SDL_max(portal.a[1], portal.b[1]) + 1e-3f)
					: (exit[0] >= SDL_min(portal.a[0], portal.b[0]) - 1e-3f && exit[0] <= SDL_max(portal.a[0], portal.b[0]) + 1e-3f))
				{
					if (portal.rise < least)
					{
						least = portal.rise;
						next = neighbor_ids[n];
					}
				}
			}
		}
		if (next >= 0)
		{
			id = next;
			continue;
		}
		// (no rectangle beyond: its last stretch, if that is all that is
		// left; by the rectangles alone a line ends on them)
		if (!navigation->follow_mesh && !any_beyond && (1.0f - travelled) * length <= MESH_LINE_OFF_METERS)
		{
			id = -1;
			continue;
		}
		stop[0] = exit[0] + (side == 0 ? 0.125f : side == 1 ? -0.125f : 0.0f);
		stop[1] = exit[1] + (side == 2 ? 0.125f : side == 3 ? -0.125f : 0.0f);
		break;
	}
	if (blocked_x != NULL)
	{
		int32_t cell;

		cell_of_point(navigation, stop[0], stop[1], &cell);
		*blocked_x = ((float)(cell % navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
		*blocked_z = ((float)(cell / navigation->size) + 0.5f) * NAVIGATION_CELL_METERS;
		*blocked_height = height;
	}
	return 0;
}

static int mesh_line_passable(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float *blocked_x, float *blocked_z, float *blocked_height)
{
	// (its body's middle's line: the rectangles are where its body fits)
	static small_circle_t circles[64];
	float from[2] = { from_x, from_z };
	float to[2] = { to_x, to_z };
	int result;
	int circle_count;
	int crossed;

	navigation->mesh.lines++;
	result = mesh_line(navigation, terrain, water, agent, from_x, from_z, from_height, to_x, to_z, blocked_x, blocked_z, blocked_height);
	if (result != 1)
	{
		return result;
	}
	// A small thing on it (they are not in the rectangles): not passable,
	// a way is to be found and led round it
	circle_count = small_circles(navigation, agent->height_meters, true, SMALL_LINE_MARGIN_METERS, SDL_min(from_x, to_x), SDL_min(from_z, to_z),
		SDL_max(from_x, to_x), SDL_max(from_z, to_z), from, to, circles, (int)SDL_arraysize(circles));
	crossed = small_first_crossed(circles, circle_count, from, to, SDL_max(agent->radius_meters - SMALL_LINE_METERS, 0.0f), NULL, 0);
	if (crossed < 0)
	{
		return 1;
	}
	if (blocked_x != NULL && blocked_z != NULL)
	{
		*blocked_x = circles[crossed].x;
		*blocked_z = circles[crossed].z;
	}
	if (blocked_height != NULL)
	{
		*blocked_height = from_height;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// The follower by the rectangles (follow_mesh): a walker on its way asks
// where it may step and how high the ground is of the rectangles of its
// class, not of the quarter meters they were made from

// Whether a step of an agent's middle goes into a small thing (they are
// not in the rectangles; a way is led round them, but a walker cuts its
// corners): its end nearer the thing's capsule than the body's half
// width less SMALL_STEP_METERS, and nearer than its start (out of one it
// may always step); away: the way straight off the thing from the step's
// start, a length of one (it goes round along the other)
#define SMALL_STEP_METERS 0.05f
static float point_segment_distance(const float point[2], const float a[2], const float b[2])
{
	float dx = b[0] - a[0];
	float dz = b[1] - a[1];
	float length_squared = dx * dx + dz * dz;
	float along = length_squared > 1e-8f ? SDL_clamp(((point[0] - a[0]) * dx + (point[1] - a[1]) * dz) / length_squared, 0.0f, 1.0f) : 0.0f;
	float off_x = a[0] + dx * along - point[0];
	float off_z = a[1] + dz * along - point[1];

	return SDL_sqrtf(off_x * off_x + off_z * off_z);
}

bool navigation_small_blocks_step(const navigation_t *navigation, const navigation_agent_t *agent, float from_x, float from_z, float to_x,
	float to_z, float *away_x_out, float *away_z_out)
{
	small_circle_t circles[32];
	float from[2] = { from_x, from_z };
	float to[2] = { to_x, to_z };
	int count = small_circles(navigation, agent->height_meters, true, agent->radius_meters, SDL_min(from_x, to_x), SDL_min(from_z, to_z),
		SDL_max(from_x, to_x), SDL_max(from_z, to_z), NULL, NULL, circles, (int)SDL_arraysize(circles));

	for (int k = 0; k < count; k++)
	{
		float reach = circles[k].capsule_radius + SDL_max(agent->radius_meters - SMALL_STEP_METERS, 0.0f);
		float to_distance = point_segment_distance(to, circles[k].a, circles[k].b);

		if (to_distance < reach && to_distance < point_segment_distance(from, circles[k].a, circles[k].b) - 1e-4f)
		{
			if (away_x_out != NULL && away_z_out != NULL)
			{
				// (from the capsule's point nearest the step's start)
				float dx = circles[k].b[0] - circles[k].a[0];
				float dz = circles[k].b[1] - circles[k].a[1];
				float length_squared = dx * dx + dz * dz;
				float along = length_squared > 1e-8f
					? SDL_clamp(((from_x - circles[k].a[0]) * dx + (from_z - circles[k].a[1]) * dz) / length_squared, 0.0f, 1.0f) : 0.0f;
				float off_x = from_x - (circles[k].a[0] + dx * along);
				float off_z = from_z - (circles[k].a[1] + dz * along);
				float off = SDL_sqrtf(off_x * off_x + off_z * off_z);

				*away_x_out = off > 1e-5f ? off_x / off : 1.0f;
				*away_z_out = off > 1e-5f ? off_z / off : 0.0f;
			}
			return true;
		}
	}
	return false;
}

// The class's level at a point nearest a height, if it is one the agent
// can be on (passable for it, within its reach of the height, or water
// to swim); slack: also the levels of the points that far round it (a
// way's corner lies on a rectangle's edge, a walker a hair over it)
static bool mesh_follow_level(const navigation_t *navigation_const, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z, float near_height, float slack, float *height_out, bool *ground_out)
{
	navigation_t *navigation = (navigation_t *)navigation_const;
	int c = mesh_class_of(agent);
	float reach = SDL_max(agent_reach(agent), MESH_FOLLOW_LEVEL_METERS);
	float best_off = 1e30f;
	bool found = false;

	mesh_note_agent(navigation, agent);
	navigation->mesh.search_number++;
	for (int point = 0; point < (slack > 0.0f ? 9 : 1) && !found; point++)
	{
		// (the point itself first, then the eight round it)
		static const int8_t offsets[9][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
		mesh_level_t levels[MESH_LEVELS];
		int32_t quarter_x;
		int32_t quarter_z;
		int32_t cell_x;
		int32_t cell_z;
		int count;

		mesh_quarter_at(c, x + (float)offsets[point][0] * slack, z + (float)offsets[point][1] * slack, &quarter_x, &quarter_z);
		cell_x = quarter_x >> 2;
		cell_z = quarter_z >> 2;
		if (quarter_x < 0 || quarter_z < 0 || cell_x >= navigation->size || cell_z >= navigation->size
			|| !mesh_tile_ready(navigation, c, region_of_cell(navigation, cell_z * navigation->size + cell_x), water))
		{
			continue;
		}
		count = mesh_levels_at(navigation, c, quarter_x, quarter_z, levels);
		for (int k = 0; k < count; k++)
		{
			node_info_t info;
			float height = levels[k].ground ? terrain_sample_height_meters(terrain, x, z) : levels[k].height;
			float off = SDL_fabsf(height - near_height);

			mesh_rect_info(navigation, terrain, water, agent, c, levels[k].rect, &info);
			if (!info.passable || (info.water_class != CLASS_SWIM && off > reach) || off >= best_off)
			{
				continue;
			}
			best_off = off;
			found = true;
			if (height_out != NULL)
			{
				*height_out = height;
			}
			if (ground_out != NULL)
			{
				*ground_out = levels[k].ground;
			}
		}
	}
	return found;
}

// The height of what is underfoot at a point, by the rectangles: the
// level nearest near_height of the agent's class (any class's, with no
// agent) there, or of the nearest point with one within
// MESH_FOLLOW_NEAR_QUARTERS (a foot by a wall, past a deck's edge: off
// where the body's middle can be). The ground's: the terrain, smooth;
// an object's top: its rectangle's. False: none near (the terrain's,
// the caller's)
static bool mesh_follow_height(const navigation_t *navigation, const terrain_t *terrain, const navigation_agent_t *agent, float x, float z,
	float near_height, float *height_out, bool *object_out)
{
	int c = -1;
	float best_off = 1e30f;
	bool found = false;
	int32_t quarter_x;
	int32_t quarter_z;
	float reach = agent != NULL ? SDL_max(agent_reach(agent), MESH_FOLLOW_LEVEL_METERS) : 0.0f;

	if (agent != NULL)
	{
		c = mesh_class_of(agent);
	}
	for (int k = 0; c < 0 && k < NAVIGATION_MESH_CLASSES; k++)
	{
		c = navigation_mesh_class_used(navigation, k) ? k : -1;
	}
	if (c < 0 || navigation->mesh.classes[c].tiles == NULL)
	{
		return false;
	}
	mesh_quarter_at(c, x, z, &quarter_x, &quarter_z);
	// (rings round the point: the first with a level ends it)
	for (int ring = 0; ring <= MESH_FOLLOW_NEAR_QUARTERS && !found; ring++)
	{
		for (int dz = -ring; dz <= ring; dz++)
		{
			for (int dx = -ring; dx <= ring; dx++)
			{
				mesh_level_t levels[MESH_LEVELS];
				int count;

				if (SDL_max(SDL_abs(dx), SDL_abs(dz)) != ring)
				{
					continue;
				}
				count = mesh_levels_at(navigation, c, quarter_x + dx, quarter_z + dz, levels);
				for (int k = 0; k < count; k++)
				{
					float height = levels[k].ground ? terrain_sample_height_meters(terrain, x, z) : levels[k].height;
					// (a little for how far off it is: the nearer of two alike)
					float off = SDL_fabsf(height - near_height) + 0.01f * (float)(SDL_abs(dx) + SDL_abs(dz));

					// (one it could not be on from where it is, a roof over
					// its head: not what is underfoot)
					if (agent != NULL && SDL_fabsf(height - near_height) > reach)
					{
						continue;
					}
					if (off < best_off)
					{
						best_off = off;
						found = true;
						*height_out = height;
						*object_out = !levels[k].ground;
					}
				}
			}
		}
	}
	return found;
}

int navigation_capsules(const navigation_t *navigation, float minimum_x, float minimum_z, float maximum_x, float maximum_z,
	navigation_capsule_t *capsules_out, int most)
{
	int32_t first_x = clamp_int((int32_t)SDL_floorf(minimum_x / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int32_t first_z = clamp_int((int32_t)SDL_floorf(minimum_z / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int32_t last_x = clamp_int((int32_t)SDL_floorf(maximum_x / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int32_t last_z = clamp_int((int32_t)SDL_floorf(maximum_z / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
	int count = 0;

	for (int32_t region_z = first_z; region_z <= last_z; region_z++)
	{
		for (int32_t region_x = first_x; region_x <= last_x; region_x++)
		{
			const navigation_region_t *region = &navigation->regions[region_z * navigation->regions_per_side + region_x];

			for (int32_t k = 0; k < region->capsule_count && !region->stale && count < most; k++)
			{
				const navigation_capsule_t *capsule = &region->capsules[k];
				// (one that reaches into a few regions: the one its first end is in tells it)
				int32_t own_x = clamp_int((int32_t)SDL_floorf(capsule->a[0] / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);
				int32_t own_z = clamp_int((int32_t)SDL_floorf(capsule->a[2] / NAVIGATION_REGION_METERS), 0, navigation->regions_per_side - 1);

				if (own_x == region_x && own_z == region_z && capsule->a[0] >= minimum_x && capsule->a[0] <= maximum_x
					&& capsule->a[2] >= minimum_z && capsule->a[2] <= maximum_z)
				{
					capsules_out[count++] = *capsule;
				}
			}
		}
	}
	return count;
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
			float low[2];
			float high[2];
			float x0;
			float z0;
			float x1;
			float z1;

			mesh_rect_box(navigation, mesh_class_of(agent), (r << 16) | k, low, high);
			x0 = low[0];
			z0 = low[1];
			x1 = high[0];
			z1 = high[1];
			if (x1 < minimum_x || z1 < minimum_z || x0 > maximum_x || z0 > maximum_z)
			{
				continue;
			}
			rects_out[count][0] = x0;
			rects_out[count][1] = z0;
			rects_out[count][2] = x1 - x0;
			rects_out[count][3] = z1 - z0;
			rects_out[count][4] = (rect->flags & NAVIGATION_RECT_GROUND) != 0 ? 0.0f : 1.0f;
			rects_out[count][5] = rect_height_high(rect);
			count++;
		}
	}
	return count;
}

void navigation_bake(navigation_t *navigation, const water_t *water, const navigation_agent_t *agents, int agent_count,
	int32_t *regions_out, int32_t *tiles_out)
{
	int32_t region_count = navigation->regions_per_side * navigation->regions_per_side;
	int32_t regions = 0;
	int32_t tiles = 0;

	for (int k = 0; k < agent_count; k++)
	{
		mesh_note_agent(navigation, &agents[k]);
	}
	for (int32_t r = 0; r < region_count; r++)
	{
		regions += navigation->regions[r].stale ? 1 : 0;
	}
	prepare_regions(navigation, 0, 0, navigation->regions_per_side - 1, navigation->regions_per_side - 1);
	// (what each region's edges hold, looked at here, one after another:
	// the tiles built side by side below only read it)
	for (int32_t r = 0; r < region_count; r++)
	{
		mesh_margin_signature(navigation, r);
	}
	for (int c = 0; c < NAVIGATION_MESH_CLASSES; c++)
	{
		navigation_mesh_class_t *class = &navigation->mesh.classes[c];

		if (!navigation_mesh_class_used(navigation, c))
		{
			continue;
		}
		// A tile's build links it with the four beside it (their borders
		// written): side by side only tiles that share no neighbor, every
		// third each way, nine rounds
		for (int round = 0; round < 9; round++)
		{
			int32_t built = 0;

#pragma omp parallel for schedule(dynamic, 1) reduction(+ : built)
			for (int32_t r = 0; r < region_count; r++)
			{
				navigation_mesh_tile_t *tile = &class->tiles[r];

				if ((r % navigation->regions_per_side) % 3 != round % 3 || (r / navigation->regions_per_side) % 3 != round / 3
					|| navigation->regions[r].stale)
				{
					continue;
				}
				if (tile->built && tile->generation == navigation->region_generation[r] && tile->class_epoch == class->epoch
					&& tile->margin_signature == mesh_margin_signature(navigation, r))
				{
					continue;
				}
				built += mesh_build_tile(navigation, c, r, water) ? 1 : 0;
			}
			tiles += built;
		}
	}
	if (regions_out != NULL)
	{
		*regions_out = regions;
	}
	if (tiles_out != NULL)
	{
		*tiles_out = tiles;
	}
}

bool navigation_mesh_class_used(const navigation_t *navigation, int class_index)
{
	return class_index >= 0 && class_index < NAVIGATION_MESH_CLASSES && navigation->mesh.classes[class_index].tiles != NULL
		&& navigation->mesh.classes[class_index].radius > 0.0f;
}

void navigation_mesh_debug(navigation_t *navigation, const water_t *water, int class_index, float minimum_x, float minimum_z, float maximum_x,
	float maximum_z, navigation_debug_rect_t *rects_out, int rects_most, int *rect_count_out, navigation_debug_portal_t *portals_out,
	int portals_most, int *portal_count_out)
{
	float region_meters = (float)navigation->region_size * NAVIGATION_CELL_METERS;
	int32_t first_x = clamp_int((int32_t)SDL_floorf(minimum_x / region_meters), 0, navigation->regions_per_side - 1);
	int32_t first_z = clamp_int((int32_t)SDL_floorf(minimum_z / region_meters), 0, navigation->regions_per_side - 1);
	int32_t last_x = clamp_int((int32_t)SDL_floorf(maximum_x / region_meters), 0, navigation->regions_per_side - 1);
	int32_t last_z = clamp_int((int32_t)SDL_floorf(maximum_z / region_meters), 0, navigation->regions_per_side - 1);
	int c = class_index;

	*rect_count_out = 0;
	*portal_count_out = 0;
	if (!navigation_mesh_class_used(navigation, class_index))
	{
		return;
	}
	// (as a search does: the tiles current, each looked at once)
	navigation->mesh.search_number++;
	for (int32_t region_z = first_z; region_z <= last_z; region_z++)
	{
		for (int32_t region_x = first_x; region_x <= last_x; region_x++)
		{
			mesh_tile_ready(navigation, c, region_z * navigation->regions_per_side + region_x, water);
		}
	}
	for (int32_t region_z = first_z; region_z <= last_z; region_z++)
	{
		for (int32_t region_x = first_x; region_x <= last_x; region_x++)
		{
			int32_t region = region_z * navigation->regions_per_side + region_x;
			const navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[region];

			for (int32_t r = 0; tile->built && r < tile->rect_count; r++)
			{
				const navigation_rect_t *rect = &tile->rects[r];
				float low[2];
				float high[2];

				mesh_rect_box(navigation, c, (region << 16) | r, low, high);
				if (high[0] < minimum_x || high[1] < minimum_z || low[0] > maximum_x || low[1] > maximum_z)
				{
					continue;
				}
				if (*rect_count_out < rects_most)
				{
					navigation_debug_rect_t *out = &rects_out[(*rect_count_out)++];

					out->x0 = low[0];
					out->z0 = low[1];
					out->x1 = high[0];
					out->z1 = high[1];
					out->height_low = rect->height;
					out->height_high = rect_height_high(rect);
					out->ground = (rect->flags & NAVIGATION_RECT_GROUND) != 0;
					out->id = (region << 16) | r;
				}
				// (its neighbors: the part of its sides it shares with each)
				{
					static int32_t neighbor_ids[MESH_NEIGHBORS_MOST];
					static uint8_t neighbor_sides[MESH_NEIGHBORS_MOST];
					int neighbor_count = mesh_neighbors(navigation, c, (region << 16) | r, neighbor_ids, neighbor_sides);

					for (int n = 0; n < neighbor_count && *portal_count_out < portals_most; n++)
					{
						mesh_shared_t shared;
						navigation_debug_portal_t *out;
						int side = neighbor_sides[n];
						float edge;

						if (!mesh_shared(navigation, c, (region << 16) | r, neighbor_ids[n], side, &shared))
						{
							continue;
						}
						edge = mesh_quarter_low(c, side == 0 || side == 2 ? shared.inside + 1 : shared.inside);
						out = &portals_out[(*portal_count_out)++];
						out->a[side < 2 ? 0 : 1] = edge;
						out->b[side < 2 ? 0 : 1] = edge;
						out->a[side < 2 ? 1 : 0] = mesh_quarter_low(c, shared.first);
						out->b[side < 2 ? 1 : 0] = mesh_quarter_low(c, shared.last);
						out->height = rect_height_high(rect);
						out->ground = (rect->flags & NAVIGATION_RECT_GROUND) != 0;
						out->jump = shared.gap != 0;
						out->rise = mesh_step_at(navigation, c, (region << 16) | r, neighbor_ids[n], &shared, (shared.first + shared.last) / 2);
					}
				}
			}
		}
	}
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
			portals[c] += tile->neighbor_count;
			bytes += sizeof(navigation_mesh_tile_t) + sizeof(navigation_rect_t) * (size_t)tile->rect_capacity
				+ sizeof(uint16_t) * (size_t)(tile->neighbor_capacity + tile->bucket_capacity);
			for (int side = 0; side < 4; side++)
			{
				portals[c] += tile->border_count[side];
				bytes += sizeof(navigation_border_t) * (size_t)tile->border_capacity[side];
			}
		}
	}
	{
		size_t used = (size_t)SDL_snprintf(text_out, text_size, "Navmesh:");

		for (int c = 0; c < NAVIGATION_MESH_CLASSES && used < text_size; c++)
		{
			const navigation_mesh_class_t *class = &navigation->mesh.classes[c];

			used += (size_t)SDL_snprintf(text_out + used, text_size - used, " %d quarter%s wide: %lld tiles, %lld rectangles, %lld neighbors (reach %.2f m, "
				"height %.2f m, radius %.2f m);", c + 1, c == 0 ? "" : "s", (long long)tiles[c], (long long)rects[c], (long long)portals[c],
				(double)(class->reach < 1e8f ? class->reach : 0.0f), (double)(class->height < 1e8f ? class->height : 0.0f), (double)class->radius);
		}
		if (used < text_size)
		{
			SDL_snprintf(text_out + used, text_size - used,
				" %.1f MB; %lld tiles built, the last in %.2f ms; the last search %.2f ms (%d expanded); searches %lld (%lld with no rectangle near, "
				"%lld ways that did not hold, %lld cut short), lines %lld",
				(double)bytes / (1024.0 * 1024.0), (long long)navigation->mesh.tiles_built, (double)navigation->mesh.last_build_milliseconds,
				(double)navigation->mesh.last_search_ticks * 1000.0 / (double)SDL_GetPerformanceFrequency(), navigation->last_expansions,
				(long long)navigation->mesh.searches, (long long)navigation->mesh.search_fallbacks, (long long)navigation->mesh.ways_refused,
				(long long)navigation->mesh.ways_cut,
				(long long)navigation->mesh.lines);
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
	{
		int c = mesh_class_of(agent);

		for (int32_t r = 0; r < navigation->regions_per_side * navigation->regions_per_side; r++)
		{
			const navigation_mesh_tile_t *tile = &navigation->mesh.classes[c].tiles[r];

			for (int32_t k = 0; tile->built && k < tile->rect_count; k++)
			{
				const navigation_rect_t *rect = &tile->rects[k];
				mesh_box_t box = mesh_box(navigation, c, (r << 16) | k);

				if ((rect->flags & NAVIGATION_RECT_GROUND) == 0)
				{
					continue;
				}
				// (the cells its outer quarter meters are in)
				for (int32_t z = box.z; z < box.z + box.depth; z++)
				{
					for (int32_t x = box.x; x < box.x + box.width; x++)
					{
						if (x == box.x || z == box.z || x == box.x + box.width - 1 || z == box.z + box.depth - 1)
						{
							overlay_out[(z / NAVIGATION_SUBCELLS) * navigation->size + x / NAVIGATION_SUBCELLS] |= NAVIGATION_OVERLAY_MESH_EDGE;
						}
					}
				}
			}
		}
	}
}
