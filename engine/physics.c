// Physics: objects as Box3D bodies, pushed by the water. See physics.h.

#include "physics.h"
#include "profile.h"

#include <SDL3/SDL.h>

#define WATER_DENSITY 1000.0f
#define GRAVITY 9.81f
// Ground tiles are made this many tiles around every moving body
#define TILE_MARGIN 1
// A loose object farther than this below the lowest ground is lost
#define FALL_LIMIT_METERS 50.0f
// Bodies pushed by the water are damped harder when under water (water
// stops spinning things quickly), per second
#define WATER_ANGULAR_DAMPING 1.5f
#define AIR_ANGULAR_DAMPING 0.05f
#define AIR_LINEAR_DAMPING 0.02f
// A loose sleeping body this far above the ground under it has lost its
// support (the water under it drained) and is woken to fall
#define HOVER_METERS 0.15f

static float random_unit(physics_t *physics)
{
	physics->random_state = physics->random_state * 1664525u + 1013904223u;
	return (float)(physics->random_state >> 8) / 16777216.0f;
}

static b3Vec3 vector(float x, float y, float z)
{
	b3Vec3 result = { x, y, z };

	return result;
}

// ---------------------------------------------------------------------------
// World

static void create_world(physics_t *physics)
{
	b3WorldDef definition = b3DefaultWorldDef();

	definition.gravity = vector(0.0f, -GRAVITY, 0.0f);
	definition.workerCount = (uint32_t)SDL_max(physics->worker_count, 1);
	definition.enableSleep = true;
	// Hits slower than this do not tear anything loose
	definition.hitEventThreshold = 1.0f;
	physics->world = b3CreateWorld(&definition);
	physics->created = true;
}

bool physics_create(physics_t *physics, const objects_t *objects, const physics_settings_t *settings)
{
	SDL_zerop(physics);
	physics->worker_count = SDL_clamp(settings->threads, 1, 16);
	physics->random_state = 0x9E3779B9u;
	physics->type_count = objects->types.type_count;
	physics->types = SDL_calloc((size_t)SDL_max(physics->type_count, 1), sizeof(physics_type_t));
	if (physics->types == NULL)
	{
		return false;
	}
	create_world(physics);
	SDL_Log("Physics: Box3D with %d threads", physics->worker_count);
	return true;
}

static void free_tiles(physics_t *physics)
{
	int tile_count = physics->tiles_per_side * physics->tiles_per_side;

	for (int i = 0; i < tile_count && physics->tiles != NULL; i++)
	{
		if (physics->tiles[i].field != NULL)
		{
			b3DestroyHeightField(physics->tiles[i].field);
		}
	}
	SDL_free(physics->tiles);
	physics->tiles = NULL;
	physics->tiles_per_side = 0;
	physics->tile_count_made = 0;
}

void physics_destroy(physics_t *physics)
{
	if (physics->created)
	{
		b3DestroyWorld(physics->world);
	}
	free_tiles(physics);
	for (int t = 0; t < physics->type_count && physics->types != NULL; t++)
	{
		if (physics->types[t].hull != NULL)
		{
			b3DestroyHull(physics->types[t].hull);
		}
		if (physics->types[t].mesh != NULL)
		{
			b3DestroyMesh(physics->types[t].mesh);
		}
		if (physics->types[t].standing_mesh != NULL && physics->types[t].standing_mesh != physics->types[t].mesh)
		{
			b3DestroyMesh(physics->types[t].standing_mesh);
		}
	}
	SDL_free(physics->types);
	SDL_free(physics->bodies);
	SDL_free(physics->free_slots);
	SDL_free(physics->wall_rectangles);
	SDL_free(physics->remove);
	SDL_free(physics->moved_slots);
	SDL_free(physics->flattened_ids);
	SDL_free(physics->bucket_heads);
	SDL_free(physics->visit_slots);
	SDL_free(physics->awake_slots);
	SDL_free(physics->cell_states);
	SDL_free(physics->cell_queue);
	SDL_zerop(physics);
}

// A plant laid down by the water: its id kept, to stand it up again
static void remember_flattened(physics_t *physics, uint32_t id)
{
	if (physics->flattened_count >= physics->flattened_capacity)
	{
		int capacity = SDL_max(physics->flattened_capacity * 2, 256);
		uint32_t *grown = SDL_realloc(physics->flattened_ids, sizeof(uint32_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		physics->flattened_ids = grown;
		physics->flattened_capacity = capacity;
	}
	physics->flattened_ids[physics->flattened_count++] = id;
}

void physics_reset(physics_t *physics, objects_t *objects, terrain_t *terrain)
{
	int tile_count;

	if (!physics->created)
	{
		return;
	}
	// The world goes with every body and tile in it
	b3DestroyWorld(physics->world);
	free_tiles(physics);
	create_world(physics);
	physics->body_count = 0;
	physics->free_slot_count = 0;
	physics->flattened_count = 0;
	for (int i = 0; i < physics->body_capacity; i++)
	{
		physics->bodies[i].used = false;
		physics->free_slots[physics->free_slot_count++] = physics->body_capacity - 1 - i;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		objects->instances[i].body = -1;
		// A loaded map may have plants still lying down
		if (objects->instances[i].flatten > 0.0f)
		{
			remember_flattened(physics, objects->instances[i].id);
		}
	}
	physics->tiles_per_side = terrain->chunks_per_side;
	physics->tile_meters = (float)TERRAIN_CHUNK_SIZE * terrain->cell_size_meters;
	tile_count = physics->tiles_per_side * physics->tiles_per_side;
	physics->tiles = SDL_calloc((size_t)SDL_max(tile_count, 1), sizeof(physics_tile_t));
	SDL_free(physics->bucket_heads);
	physics->bucket_heads = SDL_malloc(sizeof(int) * (size_t)SDL_max(tile_count, 1));
	for (int i = 0; physics->bucket_heads != NULL && i < SDL_max(tile_count, 1); i++)
	{
		physics->bucket_heads[i] = -1;
	}
	physics->awake_count = 0;
	// Heights are stored in 16 bits over this range: a few millimeters
	physics->minimum_height = -100.0f;
	physics->maximum_height = SDL_max(terrain->max_height_meters, 50.0f) * 2.0f + 100.0f;
	physics->needs_full_sync = true;
	physics->bodies_pending = false;
	physics->bodies_cursor = 0;
	// (no cell's bodies made: wanted again as they were)
	SDL_free(physics->cell_states);
	physics->cell_states = NULL;
	physics->cell_grid_size = 0;
	physics->queue_head = 0;
	physics->queue_count = 0;
	physics->queue_cursor = -1;
	physics->walls_hash = 0;
	// Every chunk the tools touch from now on counts again
	for (int i = 0; i < tile_count; i++)
	{
		terrain->chunks[i].physics_dirty = false;
		terrain->chunks[i].physics_eroded = false;
	}
}

// ---------------------------------------------------------------------------
// Ground tiles

static b3Quat instance_rotation(const object_instance_t *instance);

// The ground is a one sided surface: a body whose bottom is under it falls
// through. Lifts a body so its bottom is on the ground (models are often
// sunk into the ground a little; the ground can also rise under a body).
// Returns how far the body's bottom was above the ground (negative: it was
// under it, and is lifted now)
static float lift_above_ground(b3BodyId id, const terrain_t *terrain)
{
	b3AABB box = b3Body_ComputeAABB(id);
	// The ground under its middle only: the highest ground anywhere under
	// a wide body (a tree's crown on a slope) would throw it up
	float ground = terrain_sample_height_meters(terrain, 0.5f * (box.lowerBound.x + box.upperBound.x),
		0.5f * (box.lowerBound.z + box.upperBound.z));

	if (box.lowerBound.y < ground - 0.01f)
	{
		b3WorldTransform transform = b3Body_GetTransform(id);

		transform.p.y += ground - box.lowerBound.y + 0.02f;
		b3Body_SetTransform(id, transform.p, transform.q);
	}
	return box.lowerBound.y - ground;
}

// A moving body whose middle got under the (one sided) ground would fall
// through it: back on top, without its downward speed. True if it was
// under (and is now back)
// Small things are no bigger than this (meters, their largest extent): a
// rock, a log end, a basket. Those the mud buries are gone for good.
#define SMALL_BODY_METERS 1.2f

// A small body sunk into silt (its middle under the ground, with mud on
// top): erosion dropped the mud over it, and digging it out would only
// make it pop up out of the mud. Big things (trees, houses) are never
// buried this way: they are lifted.
static bool buried_in_silt(b3BodyId id, const terrain_t *terrain)
{
	b3AABB box = b3Body_ComputeAABB(id);
	float size = SDL_max(SDL_max(box.upperBound.x - box.lowerBound.x, box.upperBound.y - box.lowerBound.y),
		box.upperBound.z - box.lowerBound.z);
	float middle_x = 0.5f * (box.lowerBound.x + box.upperBound.x);
	float middle_z = 0.5f * (box.lowerBound.z + box.upperBound.z);
	int32_t cell_x;
	int32_t cell_y;

	if (size > SMALL_BODY_METERS || !terrain_world_to_cell(terrain, middle_x, middle_z, &cell_x, &cell_y))
	{
		return false;
	}
	return 0.5f * (box.lowerBound.y + box.upperBound.y) < terrain_sample_height_meters(terrain, middle_x, middle_z)
		&& terrain->silt_thickness != NULL && terrain->silt_thickness[terrain_index(terrain, cell_x, cell_y)] > 0;
}

static bool rescue_from_ground(b3BodyId id, const terrain_t *terrain)
{
	b3AABB box = b3Body_ComputeAABB(id);
	float middle_y = 0.5f * (box.lowerBound.y + box.upperBound.y);
	float ground = terrain_sample_height_meters(terrain, 0.5f * (box.lowerBound.x + box.upperBound.x),
		0.5f * (box.lowerBound.z + box.upperBound.z));

	if (middle_y < ground)
	{
		b3WorldTransform transform = b3Body_GetTransform(id);
		b3Vec3 velocity = b3Body_GetLinearVelocity(id);

		transform.p.y += ground - box.lowerBound.y + 0.02f;
		b3Body_SetTransform(id, transform.p, transform.q);
		if (velocity.y < 0.0f)
		{
			velocity.y = 0.0f;
			b3Body_SetLinearVelocity(id, velocity);
		}
		// A body spinning through the ground would go straight back
		// through it: it comes out calmed down
		b3Body_SetAngularVelocity(id, b3MulSV(0.25f, b3Body_GetAngularVelocity(id)));
		return true;
	}
	return false;
}

// Whether the ground tile under a point is there
static bool tile_exists_at(const physics_t *physics, float x, float z)
{
	int tile_x = (int)SDL_floorf(x / physics->tile_meters);
	int tile_y = (int)SDL_floorf(z / physics->tile_meters);

	if (tile_x < 0 || tile_y < 0 || tile_x >= physics->tiles_per_side || tile_y >= physics->tiles_per_side)
	{
		return false;
	}
	return physics->tiles[tile_y * physics->tiles_per_side + tile_x].exists;
}

static void wake_body_in_rectangle(physics_t *physics, objects_t *objects, const terrain_t *terrain, int i, float minimum_x, float minimum_z,
	float maximum_x, float maximum_z);

// A ground tile's heights before it was made again for erosion (its
// corner cell, cells per side): the loose bodies there woken only where it
// moved under them. NULL: none (a tool's change: as before)
static struct
{
	float *heights;
	int32_t first_x;
	int32_t first_y;
	int count;
} previous_ground;

static float previous_ground_at(const terrain_t *terrain, float x, float z)
{
	float u = SDL_clamp(x / terrain->cell_size_meters - (float)previous_ground.first_x, 0.0f, (float)(previous_ground.count - 1));
	float v = SDL_clamp(z / terrain->cell_size_meters - (float)previous_ground.first_y, 0.0f, (float)(previous_ground.count - 1));
	int column = SDL_min((int)u, previous_ground.count - 2);
	int row = SDL_min((int)v, previous_ground.count - 2);
	float fraction_u = u - (float)column;
	float fraction_v = v - (float)row;
	const float *heights = previous_ground.heights;
	int count = previous_ground.count;
	float low = heights[row * count + column] + (heights[row * count + column + 1] - heights[row * count + column]) * fraction_u;
	float high = heights[(row + 1) * count + column]
		+ (heights[(row + 1) * count + column + 1] - heights[(row + 1) * count + column]) * fraction_u;

	return low + (high - low) * fraction_v;
}

// The bodies in a rectangle where the ground changed: the fixed ones put on
// the new ground, the loose ones woken where it went from under them. By
// the objects' grid (the cells over the rectangle) and the loose bodies'
// list, not every body
static void wake_bodies_in_rectangle(physics_t *physics, objects_t *objects, const terrain_t *terrain, float minimum_x, float minimum_z,
	float maximum_x, float maximum_z)
{
	int cells = objects->grid_size * objects->grid_size;

	if (objects->grid_dirty || objects->grid_first == NULL || cells <= 0)
	{
		for (int i = 0; i < physics->body_capacity; i++)
		{
			wake_body_in_rectangle(physics, objects, terrain, i, minimum_x, minimum_z, maximum_x, maximum_z);
		}
		return;
	}
	{
		int last = objects->grid_size - 1;
		int first_x = SDL_clamp((int)(minimum_x / OBJECTS_GRID_CELL_METERS) - 1, 0, last);
		int first_z = SDL_clamp((int)(minimum_z / OBJECTS_GRID_CELL_METERS) - 1, 0, last);
		int last_x = SDL_clamp((int)(maximum_x / OBJECTS_GRID_CELL_METERS) + 1, 0, last);
		int last_z = SDL_clamp((int)(maximum_z / OBJECTS_GRID_CELL_METERS) + 1, 0, last);

		for (int j = first_z; j <= last_z; j++)
		{
			for (int i = first_x; i <= last_x; i++)
			{
				int cell = j * objects->grid_size + i;

				for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1]; k++)
				{
					int slot = objects->instances[k].body;

					if (slot >= 0 && slot < physics->body_capacity && physics->bodies[slot].kind != PHYSICS_BODY_DYNAMIC)
					{
						wake_body_in_rectangle(physics, objects, terrain, slot, minimum_x, minimum_z, maximum_x, maximum_z);
					}
				}
			}
		}
		// (the loose ones by their ground tiles' lists)
		if (physics->bucket_heads != NULL && physics->tiles_per_side > 0)
		{
			int tile_last = physics->tiles_per_side - 1;
			int tile_first_x = SDL_clamp((int)(minimum_x / physics->tile_meters), 0, tile_last);
			int tile_first_z = SDL_clamp((int)(minimum_z / physics->tile_meters), 0, tile_last);
			int tile_last_x = SDL_clamp((int)(maximum_x / physics->tile_meters), 0, tile_last);
			int tile_last_z = SDL_clamp((int)(maximum_z / physics->tile_meters), 0, tile_last);

			for (int j = tile_first_z; j <= tile_last_z; j++)
			{
				for (int i = tile_first_x; i <= tile_last_x; i++)
				{
					for (int slot = physics->bucket_heads[j * physics->tiles_per_side + i]; slot >= 0; slot = physics->bodies[slot].list_next)
					{
						if (physics->bodies[slot].kind == PHYSICS_BODY_DYNAMIC)
						{
							wake_body_in_rectangle(physics, objects, terrain, slot, minimum_x, minimum_z, maximum_x, maximum_z);
						}
					}
				}
			}
		}
	}
}

static void wake_body_in_rectangle(physics_t *physics, objects_t *objects, const terrain_t *terrain, int i, float minimum_x, float minimum_z,
	float maximum_x, float maximum_z)
{
	physics_body_t *body = &physics->bodies[i];

	if (!body->used || body->in_ice)
	{
		return;
	}
	if (body->kind != PHYSICS_BODY_DYNAMIC)
	{
		// Fixed things stand on the new ground
		if (body->x >= minimum_x && body->x <= maximum_x && body->z >= minimum_z && body->z <= maximum_z)
		{
			object_instance_t *instance = &objects->instances[body->instance];

			objects_refresh_ground(objects, terrain, body->instance);
			b3Body_SetTransform(body->id, vector(instance->x, objects_instance_base_y(instance), instance->z), instance_rotation(instance));
		}
		return;
	}
	if (body->x >= minimum_x && body->x <= maximum_x && body->z >= minimum_z && body->z <= maximum_z)
	{
		float clearance;

		// A tile made again for erosion somewhere in it (64 m across): only
		// where the ground under this one moved. A loose rock rests a few
		// centimeters into the ground the middle of its box samples (on a
		// slope, its shape against the tile's triangles): it was lifted,
		// with a hop, every time water eroded anything in its tile
		if (previous_ground.heights != NULL)
		{
			b3AABB box = b3Body_ComputeAABB(body->id);
			float middle_x = 0.5f * (box.lowerBound.x + box.upperBound.x);
			float middle_z = 0.5f * (box.lowerBound.z + box.upperBound.z);
			float change = terrain_sample_height_meters(terrain, middle_x, middle_z) - previous_ground_at(terrain, middle_x, middle_z);

			if (SDL_fabsf(change) < 0.02f)
			{
				return;
			}
			// (it rose: the body goes up with it, no further; it sank: the
			// body is woken, it falls on its own)
			if (change > 0.0f)
			{
				b3WorldTransform transform = b3Body_GetTransform(body->id);

				transform.p.y += change;
				b3Body_SetTransform(body->id, transform.p, transform.q);
			}
			b3Body_SetAwake(body->id, true);
			body->awake = true;
			return;
		}
		clearance = lift_above_ground(body->id, terrain);

		// Only where the ground under it changed: it was lifted, or
		// the ground went away under it
		if (clearance < -0.01f || clearance > 0.05f)
		{
			b3Body_SetAwake(body->id, true);
			body->awake = true;
		}
	}
}

// The ground's height at a cell, as the tiles have it: on the ice that
// bears, its surface where there is water
static float tile_height(const physics_t *physics, const terrain_t *terrain, int32_t x, int32_t y)
{
	float height = terrain_height_meters_clamped(terrain, x, y);
	const water_t *water = physics->ice_water;

	float level;

	if (water != NULL && water->depth != NULL
		&& water_ice_level_at_cell(water, terrain, SDL_clamp(x, 0, water->size - 1), SDL_clamp(y, 0, water->size - 1), &level))
	{
		height = SDL_clamp(level, height, physics->maximum_height);
	}
	return height;
}

// A body pulled out of the ground this many times, each within this long
// of the one before: it stays under (removed, as the buried in silt)
#define RESCUES_BEFORE_BURIED 3
#define RESCUE_WINDOW_SECONDS 2.0f

// The erosion's tiles made again in one physics step, at most (each about
// a millisecond)
#define PHYSICS_RESYNC_TILES_PER_STEP 2

static void build_tile(physics_t *physics, const terrain_t *terrain, int tile_x, int tile_y)
{
	physics_tile_t *tile = &physics->tiles[tile_y * physics->tiles_per_side + tile_x];
	const int count = TERRAIN_CHUNK_SIZE + 1;
	float heights[(TERRAIN_CHUNK_SIZE + 1) * (TERRAIN_CHUNK_SIZE + 1)];
	int32_t start_x = tile_x * TERRAIN_CHUNK_SIZE;
	int32_t start_y = tile_y * TERRAIN_CHUNK_SIZE;
	b3HeightFieldDef definition;
	b3BodyDef body_definition = b3DefaultBodyDef();
	b3ShapeDef shape_definition = b3DefaultShapeDef();
	b3BodyId body;

	if (tile->exists)
	{
		b3DestroyBody(b3Shape_GetBody(tile->shape));
		b3DestroyHeightField(tile->field);
		tile->exists = false;
		tile->field = NULL;
		physics->tile_count_made--;
	}
	for (int row = 0; row < count; row++)
	{
		for (int column = 0; column < count; column++)
		{
			heights[row * count + column] = tile_height(physics, terrain, start_x + column, start_y + row);
		}
	}
	SDL_zero(definition);
	definition.heights = heights;
	definition.scale = vector(terrain->cell_size_meters, 1.0f, terrain->cell_size_meters);
	definition.countX = count;
	definition.countZ = count;
	definition.globalMinimumHeight = physics->minimum_height;
	definition.globalMaximumHeight = physics->maximum_height;
	tile->field = b3CreateHeightField(&definition);
	if (tile->field == NULL)
	{
		return;
	}
	body_definition.type = b3_staticBody;
	body_definition.position = vector((float)start_x * terrain->cell_size_meters, 0.0f, (float)start_y * terrain->cell_size_meters);
	body = b3CreateBody(physics->world, &body_definition);
	shape_definition.baseMaterial.friction = 0.8f;
	tile->shape = b3CreateHeightFieldShape(body, &shape_definition, tile->field);
	tile->exists = true;
	physics->tile_count_made++;
}

// Makes the ground tiles around a point (and a radius) if they are missing
static void ensure_tiles_around(physics_t *physics, const terrain_t *terrain, float x, float z, float radius)
{
	int first_x = (int)SDL_floorf((x - radius) / physics->tile_meters) - TILE_MARGIN;
	int first_y = (int)SDL_floorf((z - radius) / physics->tile_meters) - TILE_MARGIN;
	int last_x = (int)SDL_floorf((x + radius) / physics->tile_meters) + TILE_MARGIN;
	int last_y = (int)SDL_floorf((z + radius) / physics->tile_meters) + TILE_MARGIN;

	first_x = SDL_max(first_x, 0);
	first_y = SDL_max(first_y, 0);
	last_x = SDL_min(last_x, physics->tiles_per_side - 1);
	last_y = SDL_min(last_y, physics->tiles_per_side - 1);
	for (int tile_y = first_y; tile_y <= last_y; tile_y++)
	{
		for (int tile_x = first_x; tile_x <= last_x; tile_x++)
		{
			if (!physics->tiles[tile_y * physics->tiles_per_side + tile_x].exists)
			{
				build_tile(physics, terrain, tile_x, tile_y);
				frame_profile_count(FRAME_PHYSICS_REBUILDS, 1);
			}
		}
	}
}

// Largest difference between the ground and a tile's copy of it
static float tile_difference(const physics_t *physics, const terrain_t *terrain, int tile_x, int tile_y)
{
	const physics_tile_t *tile = &physics->tiles[tile_y * physics->tiles_per_side + tile_x];
	const uint16_t *compressed = b3GetHeightFieldCompressedHeights(tile->field);
	const int count = TERRAIN_CHUNK_SIZE + 1;
	float largest = 0.0f;

	for (int row = 0; row < count; row++)
	{
		for (int column = 0; column < count; column++)
		{
			float stored = tile->field->minHeight + tile->field->heightScale * (float)compressed[row * count + column];
			float actual = tile_height(physics, terrain, tile_x * TERRAIN_CHUNK_SIZE + column, tile_y * TERRAIN_CHUNK_SIZE + row);

			largest = SDL_max(largest, SDL_fabsf(stored - actual));
		}
	}
	return largest;
}

// Tools and stamps: the touched tiles at once. Erosion: now and then, only
// where it changed the ground noticeably, and a few tiles a step
// (PHYSICS_RESYNC_TILES_PER_STEP; a tile made again costs about a
// millisecond, a flood wears a dozen at once): the rest the steps after,
// going on where the last left off
static void follow_terrain(physics_t *physics, objects_t *objects, terrain_t *terrain, const physics_settings_t *settings, float step_seconds)
{
	int tile_count = physics->tiles_per_side * physics->tiles_per_side;
	bool check_erosion;
	int resynced = 0;

	physics->terrain_check_timer += step_seconds;
	check_erosion = physics->resync_unfinished || physics->terrain_check_timer >= settings->terrain_check_seconds;
	if (!terrain->any_chunk_physics_dirty && !check_erosion)
	{
		return;
	}
	if (check_erosion && !physics->resync_unfinished)
	{
		physics->terrain_check_timer = 0.0f;
		physics->resync_cursor = 0;
	}
	physics->resync_unfinished = false;
	for (int i = 0; i < tile_count; i++)
	{
		terrain_chunk_t *chunk = &terrain->chunks[i];
		physics_tile_t *tile = &physics->tiles[i];
		int tile_x = i % physics->tiles_per_side;
		int tile_y = i / physics->tiles_per_side;

		if (chunk->physics_dirty)
		{
			chunk->physics_dirty = false;
			chunk->physics_eroded = false;
			if (tile->exists)
			{
				build_tile(physics, terrain, tile_x, tile_y);
				frame_profile_count(FRAME_PHYSICS_REBUILDS, 1);
			}
		}
	}
	for (int i = physics->resync_cursor; check_erosion && i < tile_count; i++)
	{
		terrain_chunk_t *chunk = &terrain->chunks[i];
		physics_tile_t *tile = &physics->tiles[i];
		int tile_x = i % physics->tiles_per_side;
		int tile_y = i / physics->tiles_per_side;

		if (!chunk->physics_eroded)
		{
			continue;
		}
		if (resynced == PHYSICS_RESYNC_TILES_PER_STEP)
		{
			// (the rest next step, from here)
			physics->resync_cursor = i;
			physics->resync_unfinished = true;
			break;
		}
		chunk->physics_eroded = false;
		if (tile->exists && tile_difference(physics, terrain, tile_x, tile_y) > settings->terrain_resync_meters)
		{
			// (its heights as they were: the bodies woken where the ground
			// under them moved, not all in the tile)
			static float heights[(TERRAIN_CHUNK_SIZE + 1) * (TERRAIN_CHUNK_SIZE + 1)];
			const uint16_t *compressed = b3GetHeightFieldCompressedHeights(tile->field);
			const int count = TERRAIN_CHUNK_SIZE + 1;

			for (int k = 0; k < count * count; k++)
			{
				heights[k] = tile->field->minHeight + tile->field->heightScale * (float)compressed[k];
			}
			build_tile(physics, terrain, tile_x, tile_y);
			frame_profile_count(FRAME_PHYSICS_REBUILDS, 1);
			resynced++;
			// Bodies resting there have to notice the new ground
			previous_ground.heights = heights;
			previous_ground.first_x = tile_x * TERRAIN_CHUNK_SIZE;
			previous_ground.first_y = tile_y * TERRAIN_CHUNK_SIZE;
			previous_ground.count = count;
			wake_bodies_in_rectangle(physics, objects, terrain, (float)tile_x * physics->tile_meters, (float)tile_y * physics->tile_meters,
				(float)(tile_x + 1) * physics->tile_meters, (float)(tile_y + 1) * physics->tile_meters);
			previous_ground.heights = NULL;
		}
	}
	if (terrain->any_chunk_physics_dirty)
	{
		// Tools: the bodies on the cells they changed (and a little around)
		const int32_t *rectangle = terrain->physics_dirty_rectangle;
		float cell = terrain->cell_size_meters;

		wake_bodies_in_rectangle(physics, objects, terrain, (float)rectangle[0] * cell - 1.0f, (float)rectangle[1] * cell - 1.0f,
			(float)rectangle[2] * cell + 1.0f, (float)rectangle[3] * cell + 1.0f);
	}
	terrain->any_chunk_physics_dirty = false;
}

// ---------------------------------------------------------------------------
// Collision shapes of the types

static void add_sample(physics_type_t *physics_type, float x, float y, float z)
{
	if (physics_type->sample_count < PHYSICS_SAMPLES_MAX)
	{
		float *point = physics_type->sample_points[physics_type->sample_count++];

		point[0] = x;
		point[1] = y;
		point[2] = z;
	}
}

static b3HullData *box_hull(vec3_t minimum, vec3_t maximum)
{
	b3Vec3 corners[8];

	for (int i = 0; i < 8; i++)
	{
		corners[i] = vector((i & 1) ? maximum.x : minimum.x, (i & 2) ? maximum.y : minimum.y, (i & 4) ? maximum.z : minimum.z);
	}
	return b3CreateHull(corners, 8, 8);
}

static void build_type(physics_t *physics, const objects_t *objects, int type_index)
{
	physics_type_t *physics_type = &physics->types[type_index];
	const object_type_t *type = &objects->types.types[type_index];
	const object_model_t *model;
	vec3_t minimum;
	vec3_t maximum;
	float extent[3];
	float center[3];
	int longest = 0;

	physics_type->built = true;
	if (type->model < 0)
	{
		return;
	}
	model = &objects->models[type->model];
	minimum = model->bounds_minimum;
	maximum = model->bounds_maximum;
	// Flat models (lily pads, cobwebs) still get some thickness
	for (int axis = 0; axis < 3; axis++)
	{
		float *low = axis == 0 ? &minimum.x : (axis == 1 ? &minimum.y : &minimum.z);
		float *high = axis == 0 ? &maximum.x : (axis == 1 ? &maximum.y : &maximum.z);

		if (*high - *low < 0.05f)
		{
			float middle = 0.5f * (*high + *low);

			*low = middle - 0.025f;
			*high = middle + 0.025f;
		}
		extent[axis] = *high - *low;
		center[axis] = 0.5f * (*high + *low);
		if (extent[axis] > extent[longest])
		{
			longest = axis;
		}
	}
	physics_type->top = maximum.y;
	physics_type->bottom = minimum.y;
	physics_type->height = maximum.y - minimum.y;
	physics_type->footprint_center_x = center[0];
	physics_type->footprint_center_z = center[2];
	physics_type->footprint_half_width_x = 0.5f * extent[0];
	physics_type->footprint_half_width_z = 0.5f * extent[2];

	physics_type->shape = type->shape;
	if (physics_type->shape == OBJECT_SHAPE_AUTO)
	{
		float second = 0.0f;

		for (int axis = 0; axis < 3; axis++)
		{
			if (axis != longest)
			{
				second = SDL_max(second, extent[axis]);
			}
		}
		// Long things (logs, planks, boats) roll and drift as capsules
		physics_type->shape = extent[longest] >= 2.2f * second ? OBJECT_SHAPE_CAPSULE : OBJECT_SHAPE_HULL;
	}

	switch (physics_type->shape)
	{
		case OBJECT_SHAPE_SPHERE:
		{
			float radius = 0.5f * SDL_max(extent[0], SDL_max(extent[1], extent[2])) * 0.9f;

			physics_type->sphere.center = vector(center[0], center[1], center[2]);
			physics_type->sphere.radius = radius;
			physics_type->volume = 4.0f / 3.0f * SDL_PI_F * radius * radius * radius;
			physics_type->sample_height = radius;
			for (int i = 0; i < 7; i++)
			{
				float offset[3] = { 0.0f, 0.0f, 0.0f };

				if (i > 0)
				{
					offset[(i - 1) / 2] = ((i - 1) % 2 == 0 ? -0.5f : 0.5f) * radius;
				}
				add_sample(physics_type, center[0] + offset[0], center[1] + offset[1], center[2] + offset[2]);
			}
			break;
		}
		case OBJECT_SHAPE_CAPSULE:
		{
			float others = 0.0f;
			float radius;
			float half_length;
			float axis_direction[3] = { 0.0f, 0.0f, 0.0f };

			for (int axis = 0; axis < 3; axis++)
			{
				if (axis != longest)
				{
					others += 0.5f * extent[axis];
				}
			}
			radius = SDL_max(0.4f * others, 0.03f);
			half_length = SDL_max(0.5f * extent[longest] - radius, 0.01f);
			axis_direction[longest] = 1.0f;
			physics_type->capsule.center1 = vector(center[0] - axis_direction[0] * half_length, center[1] - axis_direction[1] * half_length,
				center[2] - axis_direction[2] * half_length);
			physics_type->capsule.center2 = vector(center[0] + axis_direction[0] * half_length, center[1] + axis_direction[1] * half_length,
				center[2] + axis_direction[2] * half_length);
			physics_type->capsule.radius = radius;
			physics_type->volume = SDL_PI_F * radius * radius * (2.0f * half_length + 4.0f / 3.0f * radius);
			physics_type->sample_height = 2.0f * radius;
			for (int i = 0; i < 9; i++)
			{
				float t = ((float)i / 8.0f * 2.0f - 1.0f) * (half_length + 0.5f * radius);

				add_sample(physics_type, center[0] + axis_direction[0] * t, center[1] + axis_direction[1] * t,
					center[2] + axis_direction[2] * t);
			}
			break;
		}
		case OBJECT_SHAPE_MESH:
		{
			b3MeshDef definition;

			SDL_zero(definition);
			definition.vertices = (b3Vec3 *)model->collision_positions;
			definition.indices = (int32_t *)model->collision_indices;
			definition.vertexCount = model->collision_vertex_count;
			definition.triangleCount = model->collision_index_count / 3;
			definition.weldVertices = true;
			definition.weldTolerance = 0.01f;
			if (definition.vertexCount >= 3 && definition.triangleCount >= 1)
			{
				physics_type->mesh = b3CreateMesh(&definition, NULL, 0);
			}
			if (physics_type->mesh == NULL)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Physics: %s: no mesh, using a hull", type->name);
				physics_type->shape = OBJECT_SHAPE_HULL;
			}
		}
			SDL_FALLTHROUGH;
		case OBJECT_SHAPE_HULL:
		default:
			if (physics_type->shape != OBJECT_SHAPE_MESH)
			{
				physics_type->shape = OBJECT_SHAPE_HULL;
				if (model->collision_vertex_count >= 4)
				{
					physics_type->hull = b3CreateHull((const b3Vec3 *)model->collision_positions, model->collision_vertex_count, 32);
				}
				if (physics_type->hull == NULL)
				{
					physics_type->hull = box_hull(minimum, maximum);
				}
				if (physics_type->hull == NULL)
				{
					return;
				}
				physics_type->volume = b3ComputeHullMass(physics_type->hull, 1.0f).mass;
			}
			else
			{
				physics_type->volume = extent[0] * extent[1] * extent[2] * 0.5f;
			}
			// A 3 x 3 x 3 grid over the inner part of the box
			physics_type->sample_height = extent[1] / 3.0f;
			for (int i = 0; i < 27; i++)
			{
				add_sample(physics_type, center[0] + (float)(i % 3 - 1) * extent[0] / 3.0f, center[1] + (float)(i / 3 % 3 - 1) * extent[1] / 3.0f,
					center[2] + (float)(i / 9 - 1) * extent[2] / 3.0f);
			}
			break;
	}
	physics_type->volume = SDL_max(physics_type->volume, 1e-4f);
	physics_type->sample_height = SDL_max(physics_type->sample_height, 0.05f);
	// The one point a small one is pushed at: its middle, all of its height
	SDL_memcpy(physics_type->center_point, center, sizeof(physics_type->center_point));
	physics_type->whole_height = SDL_max(physics_type->shape == OBJECT_SHAPE_SPHERE ? 2.0f * physics_type->sphere.radius
		: physics_type->shape == OBJECT_SHAPE_CAPSULE ? 2.0f * physics_type->capsule.radius : extent[1], 0.05f);
	physics_type->largest_extent = SDL_max(extent[0], SDL_max(extent[1], extent[2]));
	physics_type->usable = true;
}

// ---------------------------------------------------------------------------
// Bodies

static bool wants_body(const objects_t *objects, const object_instance_t *instance)
{
	const object_type_t *type = &objects->types.types[instance->type];

	if (type->physics != OBJECT_PHYSICS_STATIC && type->physics != OBJECT_PHYSICS_BODY)
	{
		return false;
	}
	// Destroyed and without a model: nothing left to collide with; gone
	return !(instance->damage == OBJECT_DAMAGE_DESTROYED && type->destroyed_model < 0) && (instance->state & OBJECT_STATE_DEAD) == 0;
}

static int allocate_slot(physics_t *physics)
{
	if (physics->free_slot_count == 0)
	{
		int capacity = SDL_max(physics->body_capacity * 2, 1024);
		physics_body_t *bodies = SDL_realloc(physics->bodies, sizeof(physics_body_t) * (size_t)capacity);
		int *free_slots;

		if (bodies == NULL)
		{
			return -1;
		}
		physics->bodies = bodies;
		free_slots = SDL_realloc(physics->free_slots, sizeof(int) * (size_t)capacity);
		if (free_slots == NULL)
		{
			return -1;
		}
		physics->free_slots = free_slots;
		for (int i = capacity - 1; i >= physics->body_capacity; i--)
		{
			SDL_zero(physics->bodies[i]);
			physics->free_slots[physics->free_slot_count++] = i;
		}
		physics->body_capacity = capacity;
	}
	return physics->free_slots[--physics->free_slot_count];
}

// Which list the water's push finds a body in (physics_body_t.list_bucket)
static int body_bucket_of(const physics_t *physics, const physics_body_t *body)
{
	int tile_x;
	int tile_y;

	if ((body->kind == PHYSICS_BODY_STATIC && body->damage_newtons <= 0.0f) || physics->bucket_heads == NULL || physics->tiles_per_side <= 0)
	{
		return -2;
	}
	tile_x = SDL_clamp((int)(body->x / physics->tile_meters), 0, physics->tiles_per_side - 1);
	tile_y = SDL_clamp((int)(body->z / physics->tile_meters), 0, physics->tiles_per_side - 1);
	return tile_y * physics->tiles_per_side + tile_x;
}

static void body_list_insert(physics_t *physics, int slot)
{
	physics_body_t *body = &physics->bodies[slot];
	int bucket = body_bucket_of(physics, body);
	int *head = bucket >= 0 ? &physics->bucket_heads[bucket] : NULL;

	body->list_bucket = bucket;
	body->list_previous = -1;
	body->list_next = -1;
	if (head == NULL)
	{
		return;
	}
	body->list_next = *head;
	if (*head >= 0)
	{
		physics->bodies[*head].list_previous = slot;
	}
	*head = slot;
}

static void body_list_remove(physics_t *physics, int slot)
{
	physics_body_t *body = &physics->bodies[slot];
	int *head = body->list_bucket >= 0 && physics->bucket_heads != NULL ? &physics->bucket_heads[body->list_bucket] : NULL;

	if (head != NULL)
	{
		if (body->list_previous >= 0)
		{
			physics->bodies[body->list_previous].list_next = body->list_next;
		}
		else if (*head == slot)
		{
			*head = body->list_next;
		}
		if (body->list_next >= 0)
		{
			physics->bodies[body->list_next].list_previous = body->list_previous;
		}
	}
	body->list_bucket = -2;
	body->list_previous = -1;
	body->list_next = -1;
}

static void destroy_body(physics_t *physics, int slot)
{
	physics_body_t *body = &physics->bodies[slot];

	if (!body->used)
	{
		return;
	}
	body_list_remove(physics, slot);
	b3DestroyBody(body->id);
	body->used = false;
	physics->body_count--;
	physics->free_slots[physics->free_slot_count++] = slot;
}

static b3Quat instance_rotation(const object_instance_t *instance)
{
	b3Quat rotation;

	rotation.v = vector(instance->rotation[0], instance->rotation[1], instance->rotation[2]);
	rotation.s = instance->rotation[3];
	return b3NormalizeQuat(rotation);
}

static void add_moving_shape(const physics_type_t *physics_type, b3BodyId id, const b3ShapeDef *shape_definition, float scale);

// The model's triangles as a static mesh, made once per type; NULL if it
// has none
static b3MeshData *standing_mesh(const objects_t *objects, physics_type_t *physics_type, int type_index)
{
	const object_type_t *type = &objects->types.types[type_index];
	const object_model_t *model;
	b3MeshDef definition;

	if (physics_type->standing_mesh_tried)
	{
		return physics_type->standing_mesh;
	}
	physics_type->standing_mesh_tried = true;
	if (physics_type->mesh != NULL)
	{
		physics_type->standing_mesh = physics_type->mesh;
		return physics_type->standing_mesh;
	}
	if (type->model < 0)
	{
		return NULL;
	}
	model = &objects->models[type->model];
	SDL_zero(definition);
	definition.vertices = (b3Vec3 *)model->collision_positions;
	definition.indices = (int32_t *)model->collision_indices;
	definition.vertexCount = model->collision_vertex_count;
	definition.triangleCount = model->collision_index_count / 3;
	definition.weldVertices = true;
	definition.weldTolerance = 0.01f;
	if (definition.vertexCount >= 3 && definition.triangleCount >= 1)
	{
		physics_type->standing_mesh = b3CreateMesh(&definition, NULL, 0);
	}
	return physics_type->standing_mesh;
}

static int create_body(physics_t *physics, objects_t *objects, const terrain_t *terrain, int instance_index)
{
	object_instance_t *instance = &objects->instances[instance_index];
	const object_type_t *type = &objects->types.types[instance->type];
	physics_type_t *physics_type = &physics->types[instance->type];
	float scale = instance->scale;
	float volume;
	float mass;
	b3BodyDef body_definition = b3DefaultBodyDef();
	b3ShapeDef shape_definition = b3DefaultShapeDef();
	physics_body_t *body;
	int slot;
	bool loose = (instance->state & (OBJECT_STATE_FREE | OBJECT_STATE_UPROOTED)) != 0;

	if (!physics_type->built)
	{
		build_type(physics, objects, instance->type);
	}
	if (!physics_type->usable)
	{
		return -1;
	}
	slot = allocate_slot(physics);
	if (slot < 0)
	{
		return -1;
	}
	body = &physics->bodies[slot];
	SDL_zerop(body);
	body->instance = instance_index;
	body->x = instance->x;
	body->z = instance->z;
	body->scale = scale;
	volume = physics_type->volume * scale * scale * scale;
	mass = type->mass_kilograms > 0.0f ? type->mass_kilograms * scale * scale * scale
		: SDL_max(type->density_kilograms_per_cubic_meter, 1.0f) * volume;
	body->mass = mass;
	// What it pushes out of the way under water: the solid part of it,
	// not the air in a tree's crown that the hull also wraps
	body->displaced_volume = mass / SDL_max(type->density_kilograms_per_cubic_meter, 1.0f);
	body->anchor_newtons = type->anchor_newtons * scale * scale;
	body->damage_newtons = type->damage_newtons * scale * scale;
	if (type->physics == OBJECT_PHYSICS_STATIC)
	{
		body->kind = PHYSICS_BODY_STATIC;
	}
	else if (type->anchor_newtons > 0.0f && !loose)
	{
		body->kind = PHYSICS_BODY_ANCHORED;
	}
	else
	{
		body->kind = PHYSICS_BODY_DYNAMIC;
	}

	body_definition.type = body->kind == PHYSICS_BODY_DYNAMIC ? b3_dynamicBody : b3_staticBody;
	body_definition.position = vector(instance->x, objects_instance_base_y(instance), instance->z);
	body_definition.rotation = instance_rotation(instance);
	// Loose objects that just came into being (debris) start awake; the
	// rest of the map sleeps until something reaches it
	body_definition.isAwake = (instance->state & OBJECT_STATE_FREE) != 0 && instance->body == -2;
	body_definition.linearDamping = AIR_LINEAR_DAMPING;
	body_definition.angularDamping = AIR_ANGULAR_DAMPING;
	body_definition.userData = (void *)(intptr_t)(slot + 1);
	body->id = b3CreateBody(physics->world, &body_definition);

	shape_definition.density = mass / SDL_max(volume, 1e-6f);
	shape_definition.baseMaterial.friction = 0.6f;
	shape_definition.enableHitEvents = true;
	if (body->kind != PHYSICS_BODY_DYNAMIC && standing_mesh(objects, physics_type, instance->type) != NULL)
	{
		// Standing: its real shape, triangle by triangle
		b3CreateMeshShape(body->id, &shape_definition, physics_type->standing_mesh, vector(scale, scale, scale));
	}
	else if (body->kind != PHYSICS_BODY_DYNAMIC && objects->models[type->model].collision_index_count == 0
		&& objects->models[type->model].pick_index_count > 0)
	{
		// Standing, and nothing of it solid (a flower, a tuft): in no one's
		// way, but still there for the water to tear out
		shape_definition.isSensor = true;
		add_moving_shape(physics_type, body->id, &shape_definition, scale);
	}
	else
	{
		add_moving_shape(physics_type, body->id, &shape_definition, scale);
	}
	if (body->kind == PHYSICS_BODY_DYNAMIC)
	{
		lift_above_ground(body->id, terrain);
	}
	body->used = true;
	body->awake = body_definition.isAwake && body->kind == PHYSICS_BODY_DYNAMIC;
	physics->body_count++;
	if (body->kind == PHYSICS_BODY_DYNAMIC && body_definition.isAwake)
	{
		ensure_tiles_around(physics, terrain, instance->x, instance->z, scale * objects->models[type->model].horizontal_radius);
	}
	body_list_insert(physics, slot);
	return slot;
}

// The shape a body moves with (a hull, a capsule, a sphere; a mesh only for
// a type that asks for one, which never moves)
static void add_moving_shape(const physics_type_t *physics_type, b3BodyId id, const b3ShapeDef *shape_definition, float scale)
{
	switch (physics_type->shape)
	{
		case OBJECT_SHAPE_SPHERE:
		{
			b3Sphere sphere = physics_type->sphere;

			sphere.center = b3MulSV(scale, sphere.center);
			sphere.radius *= scale;
			b3CreateSphereShape(id, shape_definition, &sphere);
			break;
		}
		case OBJECT_SHAPE_CAPSULE:
		{
			b3Capsule capsule = physics_type->capsule;

			capsule.center1 = b3MulSV(scale, capsule.center1);
			capsule.center2 = b3MulSV(scale, capsule.center2);
			capsule.radius *= scale;
			b3CreateCapsuleShape(id, shape_definition, &capsule);
			break;
		}
		case OBJECT_SHAPE_MESH:
			b3CreateMeshShape(id, shape_definition, physics_type->mesh, vector(scale, scale, scale));
			break;
		default:
			b3CreateTransformedHullShape(id, shape_definition, physics_type->hull, b3Transform_identity, vector(scale, scale, scale));
			break;
	}
}

// Bodies follow the object list: new objects get one, removed ones lose
// theirs, and every body learns where its object is now
void physics_set_focus(physics_t *physics, float x, float z)
{
	physics->focus_x = x;
	physics->focus_z = z;
	physics->focus_set = true;
}

// Bodies within this of the focus are made at once; beyond it no more
// than this many milliseconds a sync
static const float BODIES_AT_ONCE_METERS = 96.0f;
static const double BODIES_MILLISECONDS_PER_SYNC = 2.0;
static const int BODIES_LOOKED_AT_PER_STEP = 200000;
// The cells within this of the focus have their bodies made (a rifle's
// reach, the people seen); those within BODIES_AT_ONCE_METERS at once
static const float BODIES_RANGE_METERS = 1000.0f;

// The objects' grid cell of a point
static int body_cell_of(const objects_t *objects, float x, float z)
{
	int last = objects->grid_size - 1;
	int i = SDL_clamp((int)(x / OBJECTS_GRID_CELL_METERS), 0, last);
	int j = SDL_clamp((int)(z / OBJECTS_GRID_CELL_METERS), 0, last);

	return j * objects->grid_size + i;
}

// The cells' states sized to the objects' grid (a new map: none made)
static bool cell_states_ready(physics_t *physics, const objects_t *objects)
{
	if (objects->grid_size <= 0)
	{
		return false;
	}
	if (physics->cell_states == NULL || physics->cell_grid_size != objects->grid_size)
	{
		SDL_free(physics->cell_states);
		physics->cell_states = SDL_calloc((size_t)objects->grid_size * (size_t)objects->grid_size, 1);
		physics->cell_grid_size = physics->cell_states != NULL ? objects->grid_size : 0;
		physics->queue_head = 0;
		physics->queue_count = 0;
		physics->queue_cursor = -1;
	}
	return physics->cell_states != NULL;
}

// Whether an object stands where bodies are made (its cell's are; with no
// focus, the tests: everywhere)
static bool in_made_cell(const physics_t *physics, const objects_t *objects, const object_instance_t *instance)
{
	if (!physics->focus_set)
	{
		return true;
	}
	if (physics->cell_states == NULL || physics->cell_grid_size != objects->grid_size)
	{
		return false;
	}
	return physics->cell_states[body_cell_of(objects, instance->x, instance->z)] == 2;
}

// A cell's bodies wanted: in the queue, unless made or there already
static void want_cell(physics_t *physics, int cell)
{
	if (physics->cell_states[cell] != 0)
	{
		return;
	}
	if (physics->queue_count >= physics->queue_capacity)
	{
		int capacity = SDL_max(physics->queue_capacity * 2, 1024);
		int *grown = SDL_malloc(sizeof(int) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		// (the ring unrolled into the new one)
		for (int n = 0; n < physics->queue_count; n++)
		{
			grown[n] = physics->cell_queue[(physics->queue_head + n) % physics->queue_capacity];
		}
		SDL_free(physics->cell_queue);
		physics->cell_queue = grown;
		physics->queue_capacity = capacity;
		physics->queue_head = 0;
	}
	physics->cell_queue[(physics->queue_head + physics->queue_count) % physics->queue_capacity] = cell;
	physics->queue_count++;
	physics->cell_states[cell] = 1;
}

// The cells within radius of (x, z) wanted
static void want_cells_round(physics_t *physics, const objects_t *objects, float x, float z, float radius)
{
	int last = objects->grid_size - 1;
	int first_x = SDL_clamp((int)((x - radius) / OBJECTS_GRID_CELL_METERS), 0, last);
	int first_z = SDL_clamp((int)((z - radius) / OBJECTS_GRID_CELL_METERS), 0, last);
	int last_x = SDL_clamp((int)((x + radius) / OBJECTS_GRID_CELL_METERS), 0, last);
	int last_z = SDL_clamp((int)((z + radius) / OBJECTS_GRID_CELL_METERS), 0, last);
	float reach = radius + OBJECTS_GRID_CELL_METERS;

	for (int j = first_z; j <= last_z; j++)
	{
		for (int i = first_x; i <= last_x; i++)
		{
			float dx = ((float)i + 0.5f) * OBJECTS_GRID_CELL_METERS - x;
			float dz = ((float)j + 0.5f) * OBJECTS_GRID_CELL_METERS - z;

			if (dx * dx + dz * dz <= reach * reach)
			{
				want_cell(physics, j * objects->grid_size + i);
			}
		}
	}
}

void physics_request_bodies(physics_t *physics, const objects_t *objects, float x, float z, float radius)
{
	if (physics->created && cell_states_ready(physics, objects))
	{
		want_cells_round(physics, objects, x, z, radius);
	}
}

static void sync_bodies(physics_t *physics, objects_t *objects, const terrain_t *terrain)
{
	uint64_t start_counter = SDL_GetPerformanceCounter();
	uint64_t budget_ticks = (uint64_t)(BODIES_MILLISECONDS_PER_SYNC * 0.001 * (double)SDL_GetPerformanceFrequency());
	// (the time making bodies, not going through the objects: that alone
	// takes longer than the budget on a big map, and nothing far would
	// ever be made)
	uint64_t making_ticks = 0;
	int count_before = physics->body_count;
	bool incomplete = false;

	if (!physics->needs_full_sync && physics->synced_generation == objects->generation)
	{
		return;
	}
	physics->mark++;
	if (physics->mark == 0)
	{
		physics->mark = 1;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		object_instance_t *instance = &objects->instances[i];
		// (one that has a body keeps it a little farther than one gets it)
		// (a cell's bodies once made stay: one that has a body keeps it)
		bool wanted = wants_body(objects, instance) && (instance->body >= 0 || in_made_cell(physics, objects, instance));
		bool fresh = instance->body == -2;

		if (instance->body >= 0)
		{
			physics_body_t *body = instance->body < physics->body_capacity ? &physics->bodies[instance->body] : NULL;

			if (body != NULL && body->used && body->mark != physics->mark && wanted)
			{
				body->mark = physics->mark;
				body->instance = i;
				continue;
			}
			instance->body = -1;
		}
		if (wanted)
		{
			// (far from the focus, and the time is up: next time; without a
			// focus, the tests, all at once)
			if (making_ticks > budget_ticks)
			{
				float away_x = instance->x - physics->focus_x;
				float away_z = instance->z - physics->focus_z;

				if (physics->focus_set && away_x * away_x + away_z * away_z > BODIES_AT_ONCE_METERS * BODIES_AT_ONCE_METERS)
				{
					instance->body = fresh ? -2 : -1;
					incomplete = true;
					continue;
				}
			}
			instance->body = fresh ? -2 : -1;
			{
				uint64_t making_start = SDL_GetPerformanceCounter();

				instance->body = create_body(physics, objects, terrain, i);
				making_ticks += SDL_GetPerformanceCounter() - making_start;
			}
			if (instance->body >= 0)
			{
				physics->bodies[instance->body].mark = physics->mark;
			}
		}
		else
		{
			instance->body = -1;
		}
	}
	for (int slot = 0; slot < physics->body_capacity; slot++)
	{
		if (physics->bodies[slot].used && physics->bodies[slot].mark != physics->mark)
		{
			destroy_body(physics, slot);
		}
	}
	physics->synced_generation = objects->generation;
	// (the rest of them a slice a step: continue_bodies)
	physics->needs_full_sync = false;
	physics->bodies_pending = incomplete;
	physics->bodies_cursor = 0;
	// (every change so far seen to by this pass)
	physics->change_serial = objects->change_base + (uint32_t)objects->change_count;
	if (incomplete || physics->staggered_made > 0)
	{
		if (physics->staggered_made == 0)
		{
			physics->staggered_start = start_counter;
		}
		physics->staggered_made += physics->body_count - count_before;
		if (!incomplete)
		{
			SDL_Log("Physics: %d bodies made, a few milliseconds a step, over %.0f ms", physics->staggered_made,
				1000.0 * (double)(SDL_GetPerformanceCounter() - physics->staggered_start) / (double)SDL_GetPerformanceFrequency());
			physics->staggered_made = 0;
		}
	}
	else if (physics->body_count - count_before > 1000)
	{
		SDL_Log("Physics: %d bodies made in %llu ms", physics->body_count - count_before,
			(unsigned long long)((SDL_GetPerformanceCounter() - start_counter) * 1000 / SDL_GetPerformanceFrequency()));
	}
}

// ---------------------------------------------------------------------------
// Water walls

static uint64_t hash_mix(uint64_t hash, uint64_t value)
{
	hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
	return hash;
}

// One object looked at: a body made if it wants one and has none; the
// making's time added up
static void make_body_for(physics_t *physics, objects_t *objects, const terrain_t *terrain, int i, uint64_t *making_ticks)
{
	object_instance_t *instance = &objects->instances[i];
	uint64_t making_start;

	if (instance->body >= 0 || !wants_body(objects, instance))
	{
		return;
	}
	making_start = SDL_GetPerformanceCounter();
	instance->body = create_body(physics, objects, terrain, i);
	*making_ticks += SDL_GetPerformanceCounter() - making_start;
	if (instance->body >= 0)
	{
		physics->bodies[instance->body].mark = physics->mark;
	}
}

// The cells waiting for their bodies, a few milliseconds' making and at
// most so many objects looked at a step (the head cell from queue_cursor
// on); a cell done: made, with those added since the sort that stand in it
static void make_queued_cells(physics_t *physics, objects_t *objects, const terrain_t *terrain)
{
	uint64_t budget_ticks = (uint64_t)(BODIES_MILLISECONDS_PER_SYNC * 0.001 * (double)SDL_GetPerformanceFrequency());
	uint64_t making_ticks = 0;
	int looked = 0;
	int count_before = physics->body_count;

	if (objects->grid_dirty || objects->grid_first == NULL || physics->cell_states == NULL || physics->cell_grid_size != objects->grid_size)
	{
		return;
	}
	if (physics->queue_count > 0 && physics->staggered_made == 0 && physics->queue_cursor < 0)
	{
		physics->staggered_start = SDL_GetPerformanceCounter();
	}
	while (physics->queue_count > 0 && making_ticks <= budget_ticks && looked < BODIES_LOOKED_AT_PER_STEP)
	{
		int cell = physics->cell_queue[physics->queue_head];
		int last = objects->grid_first[cell + 1];

		// (made at once meanwhile, near the focus: off the queue)
		if (physics->cell_states[cell] == 2 && physics->queue_cursor < 0)
		{
			physics->queue_head = (physics->queue_head + 1) % physics->queue_capacity;
			physics->queue_count--;
			continue;
		}
		if (physics->queue_cursor < 0)
		{
			physics->queue_cursor = objects->grid_first[cell];
		}
		while (physics->queue_cursor < last && making_ticks <= budget_ticks && looked < BODIES_LOOKED_AT_PER_STEP)
		{
			make_body_for(physics, objects, terrain, physics->queue_cursor++, &making_ticks);
			looked++;
		}
		if (physics->queue_cursor < last)
		{
			break;
		}
		physics->cell_states[cell] = 2;
		for (int k = objects->sorted_count; k < objects->instance_count; k++)
		{
			if (body_cell_of(objects, objects->instances[k].x, objects->instances[k].z) == cell)
			{
				make_body_for(physics, objects, terrain, k, &making_ticks);
			}
		}
		physics->queue_head = (physics->queue_head + 1) % physics->queue_capacity;
		physics->queue_count--;
		physics->queue_cursor = -1;
	}
	physics->staggered_made += SDL_max(physics->body_count - count_before, 0);
	if (physics->queue_count == 0 && physics->staggered_made > 0)
	{
		SDL_Log("Physics: %d bodies made, a few milliseconds a step, over %.0f ms (%d in all)", physics->staggered_made,
			1000.0 * (double)(SDL_GetPerformanceCounter() - physics->staggered_start) / (double)SDL_GetPerformanceFrequency(), physics->body_count);
		physics->staggered_made = 0;
	}
}

// After a sync that ran out of time: the bodies not made yet (in the cells
// made), from the cursor on, a few milliseconds' making a step
static void continue_bodies(physics_t *physics, objects_t *objects, const terrain_t *terrain)
{
	uint64_t budget_ticks = (uint64_t)(BODIES_MILLISECONDS_PER_SYNC * 0.001 * (double)SDL_GetPerformanceFrequency());
	uint64_t making_ticks = 0;
	int looked = 0;

	while (physics->bodies_cursor < objects->instance_count && looked < BODIES_LOOKED_AT_PER_STEP && making_ticks <= budget_ticks)
	{
		int i = physics->bodies_cursor++;

		if (in_made_cell(physics, objects, &objects->instances[i]))
		{
			make_body_for(physics, objects, terrain, i, &making_ticks);
		}
		looked++;
	}
	if (physics->bodies_cursor >= objects->instance_count)
	{
		physics->bodies_pending = false;
	}
}

// The object list changed a little since the last pass (no sort: no new
// generation): a body for one added, none for one gone, another for one
// turned into another type
static void follow_changes(physics_t *physics, objects_t *objects, const terrain_t *terrain)
{
	int count = 0;
	uint32_t next;
	const object_change_t *changes = objects_changes_since(objects, physics->change_serial, &count, &next);

	for (int c = 0; c < count; c++)
	{
		int index = changes[c].index;
		object_instance_t *instance;

		if (index < 0 || index >= objects->instance_count)
		{
			continue;
		}
		instance = &objects->instances[index];
		if (changes[c].kind == OBJECT_CHANGE_FREED)
		{
			continue;
		}
		// (gone, or another type: its body goes)
		if (changes[c].kind != OBJECT_CHANGE_ADDED && instance->body >= 0)
		{
			if (instance->body < physics->body_capacity && physics->bodies[instance->body].used)
			{
				destroy_body(physics, instance->body);
			}
			instance->body = -1;
		}
		// (added, or another type: one made, if it wants one here)
		if (changes[c].kind != OBJECT_CHANGE_KILLED && instance->body < 0 && wants_body(objects, instance)
			&& in_made_cell(physics, objects, instance))
		{
			instance->body = create_body(physics, objects, terrain, index);
			if (instance->body >= 0)
			{
				physics->bodies[instance->body].mark = physics->mark;
			}
		}
	}
	physics->change_serial = next;
}

static bool blocks_water(const objects_t *objects, const object_instance_t *instance)
{
	const object_type_t *type = &objects->types.types[instance->type];

	return type->blocks_water && !(instance->state & OBJECT_STATE_FREE) && type->model >= 0
		&& !(instance->damage == OBJECT_DAMAGE_DESTROYED && type->destroyed_model < 0);
}

// Whether a point (x, z) is inside an outline (even-odd)
static bool inside_outline(const float points[][2], int count, float x, float z)
{
	bool inside = false;

	for (int i = 0, j = count - 1; i < count; j = i++)
	{
		if ((points[i][1] > z) != (points[j][1] > z)
			&& x < (points[j][0] - points[i][0]) * (z - points[i][1]) / (points[j][1] - points[i][1]) + points[i][0])
		{
			inside = !inside;
		}
	}
	return inside;
}

// A building's walls for the water only within its outline: the cells
// whose middles are inside one of its model's footprints that hide the
// terrain (OBJECT_FOOTPRINT_NO_TERRAIN: the building itself, tools/house_grow;
// a yard's cover is not one). Its fences, its yard, its eaves stand over
// the ground, not on it: the water flows past and under them. Then the
// roof's pits filled (its highest point over each cell leaves dips between
// a thatch's bundles, its boards; the water a flood left in them stood on
// the roof): each cell raised to the lowest of its four neighbors, again
// until nothing changes (a cell at the outline's edge has the open ground
// beside it: never raised). false: the model has no such footprint (all of
// it stands as it is)
static bool keep_within_outline(const object_model_t *model, const object_instance_t *instance, float cosine, float sine, const water_t *water,
	const int32_t rectangle[4], int32_t width, int32_t height, float *tops)
{
	bool any = false;
	bool *inside;

	for (int f = 0; f < model->footprint_count; f++)
	{
		any = any || ((model->footprints[f].flags & OBJECT_FOOTPRINT_NO_TERRAIN) != 0 && model->footprints[f].point_count >= 3);
	}
	if (!any)
	{
		return false;
	}
	inside = SDL_calloc((size_t)width * (size_t)height, sizeof(bool));
	if (inside == NULL)
	{
		return false;
	}
	for (int f = 0; f < model->footprint_count; f++)
	{
		const object_file_footprint_t *footprint = &model->footprints[f];
		float points[OBJECT_FILE_MAX_FOOTPRINT_POINTS][2];
		int count = (int)SDL_min(footprint->point_count, (uint32_t)OBJECT_FILE_MAX_FOOTPRINT_POINTS);

		if ((footprint->flags & OBJECT_FOOTPRINT_NO_TERRAIN) == 0 || count < 3)
		{
			continue;
		}
		// (turned as the triangles are: x' = x cos + z sin, z' = -x sin + z cos)
		for (int k = 0; k < count; k++)
		{
			float x = footprint->points[k][0];
			float z = footprint->points[k][1];

			points[k][0] = instance->x + (x * cosine + z * sine) * instance->scale;
			points[k][1] = instance->z + (-x * sine + z * cosine) * instance->scale;
		}
		for (int32_t y = 0; y < height; y++)
		{
			for (int32_t x = 0; x < width; x++)
			{
				if (!inside[y * width + x] && inside_outline((const float(*)[2])points, count, (float)(rectangle[0] + x) * water->cell_size_meters,
					(float)(rectangle[1] + y) * water->cell_size_meters))
				{
					inside[y * width + x] = true;
				}
			}
		}
	}
	for (int32_t i = 0; i < width * height; i++)
	{
		if (!inside[i])
		{
			tops[i] = -1e30f;
		}
	}
	// The pits filled (at most so many rounds: a pit is never that wide)
	for (int round = 0; round < 256; round++)
	{
		bool changed = false;

		for (int32_t y = 0; y < height; y++)
		{
			for (int32_t x = 0; x < width; x++)
			{
				int32_t at = y * width + x;
				float lowest;

				if (!inside[at])
				{
					continue;
				}
				// (off the rectangle, outside the outline: open ground, no fill)
				lowest = x > 0 && inside[at - 1] ? tops[at - 1] : -1e30f;
				lowest = SDL_min(lowest, x + 1 < width && inside[at + 1] ? tops[at + 1] : -1e30f);
				lowest = SDL_min(lowest, y > 0 && inside[at - width] ? tops[at - width] : -1e30f);
				lowest = SDL_min(lowest, y + 1 < height && inside[at + width] ? tops[at + width] : -1e30f);
				if (lowest > tops[at])
				{
					tops[at] = lowest;
					changed = true;
				}
			}
		}
		if (!changed)
		{
			break;
		}
	}
	SDL_free(inside);
	return true;
}

static void rebuild_walls(physics_t *physics, objects_t *objects, const terrain_t *terrain, water_t *water, bool force)
{
	uint64_t hash = 1469598103934665603ull;
	size_t cell_count = (size_t)water->size * (size_t)water->size;
	int wall_count = 0;

	// (baked water stands still: no walls to flow round)
	if (water->obstacle_centimeters == NULL)
	{
		return;
	}

	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		union
		{
			float f[4];
			uint64_t u[2];
		} values;

		if (!blocks_water(objects, instance))
		{
			continue;
		}
		values.f[0] = instance->x;
		values.f[1] = instance->z;
		values.f[2] = instance->yaw_radians;
		values.f[3] = instance->scale;
		hash = hash_mix(hash, values.u[0]);
		hash = hash_mix(hash, values.u[1]);
		hash = hash_mix(hash, (uint64_t)instance->type * 4u + instance->damage);
		wall_count++;
	}
	if (hash == physics->walls_hash && !force)
	{
		return;
	}
	physics->walls_hash = hash;

	// The water where the old walls stood must notice they are gone
	for (int k = 0; k < physics->wall_count; k++)
	{
		const int32_t *rectangle = &physics->wall_rectangles[4 * k];

		water_wake_cells(water, rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
	}
	SDL_memset(water->obstacle_centimeters, 0, cell_count * sizeof(uint16_t));
	if (wall_count > physics->wall_capacity)
	{
		int32_t *grown = SDL_realloc(physics->wall_rectangles, sizeof(int32_t) * 4 * (size_t)wall_count);

		if (grown == NULL)
		{
			return;
		}
		physics->wall_rectangles = grown;
		physics->wall_capacity = wall_count;
	}
	physics->wall_count = 0;
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		float scale = instance->scale;
		float cosine = SDL_cosf(instance->yaw_radians);
		float sine = SDL_sinf(instance->yaw_radians);
		const object_model_t *model;
		int model_index;
		float base_y;
		float reach;
		float *tops;
		int32_t rectangle_width;
		int32_t rectangle_height;
		int32_t rectangle[4];

		if (!blocks_water(objects, instance))
		{
			continue;
		}
		model_index = instance->damage == OBJECT_DAMAGE_DESTROYED ? objects->types.types[instance->type].destroyed_model
			: objects->types.types[instance->type].model;
		if (model_index < 0)
		{
			continue;
		}
		model = &objects->models[model_index];
		base_y = objects_instance_base_y(instance);
		reach = model->horizontal_radius * scale + water->cell_size_meters;
		rectangle[0] = clamp_int((int32_t)SDL_floorf((instance->x - reach) / water->cell_size_meters), 0, water->size - 1);
		rectangle[1] = clamp_int((int32_t)SDL_floorf((instance->z - reach) / water->cell_size_meters), 0, water->size - 1);
		rectangle[2] = clamp_int((int32_t)SDL_ceilf((instance->x + reach) / water->cell_size_meters), 0, water->size - 1);
		rectangle[3] = clamp_int((int32_t)SDL_ceilf((instance->z + reach) / water->cell_size_meters), 0, water->size - 1);
		rectangle_width = rectangle[2] - rectangle[0] + 1;
		rectangle_height = rectangle[3] - rectangle[1] + 1;
		tops = SDL_malloc(sizeof(float) * (size_t)rectangle_width * (size_t)rectangle_height);
		if (tops == NULL)
		{
			continue;
		}
		for (int i = 0; i < rectangle_width * rectangle_height; i++)
		{
			tops[i] = -1e30f;
		}
		// The highest point of the model over each cell: its roof, not its
		// box, so a pointed roof sheds the water. Cell centers inside each
		// triangle (seen from above), and points along its edges (walls
		// stand edge on, they have no inside seen from above).
		for (int t = 0; t + 2 < model->collision_index_count; t += 3)
		{
			float world_x[3];
			float world_y[3];
			float world_z[3];
			float minimum_x = 1e30f;
			float maximum_x = -1e30f;
			float minimum_z = 1e30f;
			float maximum_z = -1e30f;
			float area;

			for (int k = 0; k < 3; k++)
			{
				const float *position = &model->collision_positions[3 * model->collision_indices[t + k]];

				// Model to world: x' = x cos + z sin, z' = -x sin + z cos
				world_x[k] = instance->x + (position[0] * cosine + position[2] * sine) * scale;
				world_z[k] = instance->z + (-position[0] * sine + position[2] * cosine) * scale;
				world_y[k] = base_y + position[1] * scale;
				minimum_x = SDL_min(minimum_x, world_x[k]);
				maximum_x = SDL_max(maximum_x, world_x[k]);
				minimum_z = SDL_min(minimum_z, world_z[k]);
				maximum_z = SDL_max(maximum_z, world_z[k]);
			}
			// Along the edges
			for (int k = 0; k < 3; k++)
			{
				int next = (k + 1) % 3;
				float length = SDL_sqrtf((world_x[next] - world_x[k]) * (world_x[next] - world_x[k])
					+ (world_z[next] - world_z[k]) * (world_z[next] - world_z[k]));
				int steps = (int)(length / (0.5f * water->cell_size_meters)) + 1;

				for (int step = 0; step <= steps; step++)
				{
					float f = (float)step / (float)steps;
					float x = world_x[k] + (world_x[next] - world_x[k]) * f;
					float z = world_z[k] + (world_z[next] - world_z[k]) * f;
					float y = world_y[k] + (world_y[next] - world_y[k]) * f;
					int32_t cell_x = (int32_t)SDL_floorf(x / water->cell_size_meters + 0.5f) - rectangle[0];
					int32_t cell_y = (int32_t)SDL_floorf(z / water->cell_size_meters + 0.5f) - rectangle[1];

					if (cell_x >= 0 && cell_y >= 0 && cell_x < rectangle_width && cell_y < rectangle_height)
					{
						float *top = &tops[cell_y * rectangle_width + cell_x];

						*top = SDL_max(*top, y);
					}
				}
			}
			// Cell centers inside it
			area = (world_x[1] - world_x[0]) * (world_z[2] - world_z[0]) - (world_x[2] - world_x[0]) * (world_z[1] - world_z[0]);
			if (SDL_fabsf(area) < 1e-6f)
			{
				continue;
			}
			for (int32_t cell_y = SDL_max((int32_t)SDL_ceilf(minimum_z / water->cell_size_meters), rectangle[1]);
				cell_y <= SDL_min((int32_t)SDL_floorf(maximum_z / water->cell_size_meters), rectangle[3]); cell_y++)
			{
				for (int32_t cell_x = SDL_max((int32_t)SDL_ceilf(minimum_x / water->cell_size_meters), rectangle[0]);
					cell_x <= SDL_min((int32_t)SDL_floorf(maximum_x / water->cell_size_meters), rectangle[2]); cell_x++)
				{
					float x = (float)cell_x * water->cell_size_meters;
					float z = (float)cell_y * water->cell_size_meters;
					// Barycentric weights of the cell center
					float weight_1 = ((x - world_x[0]) * (world_z[2] - world_z[0]) - (world_x[2] - world_x[0]) * (z - world_z[0])) / area;
					float weight_2 = ((world_x[1] - world_x[0]) * (z - world_z[0]) - (x - world_x[0]) * (world_z[1] - world_z[0])) / area;
					float weight_0 = 1.0f - weight_1 - weight_2;
					float *top;

					if (weight_0 < 0.0f || weight_1 < 0.0f || weight_2 < 0.0f)
					{
						continue;
					}
					top = &tops[(cell_y - rectangle[1]) * rectangle_width + (cell_x - rectangle[0])];
					*top = SDL_max(*top, weight_0 * world_y[0] + weight_1 * world_y[1] + weight_2 * world_y[2]);
				}
			}
		}
		keep_within_outline(model, instance, cosine, sine, water, rectangle, rectangle_width, rectangle_height, tops);
		for (int32_t y = rectangle[1]; y <= rectangle[3]; y++)
		{
			for (int32_t x = rectangle[0]; x <= rectangle[2]; x++)
			{
				float top = tops[(y - rectangle[1]) * rectangle_width + (x - rectangle[0])];
				float height = top - terrain_height_meters_clamped(terrain, x, y);
				uint16_t *cell = &water->obstacle_centimeters[(size_t)y * (size_t)water->size + (size_t)x];
				uint16_t centimeters;

				if (height <= 0.0f)
				{
					continue;
				}
				centimeters = (uint16_t)SDL_min(height * 100.0f, 65535.0f);
				if (*cell < centimeters)
				{
					*cell = centimeters;
				}
			}
		}
		SDL_free(tops);
		SDL_memcpy(&physics->wall_rectangles[4 * physics->wall_count], rectangle, sizeof(rectangle));
		physics->wall_count++;
		water_wake_cells(water, rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
	}
	// The walls go up to the GPU's water
	water->any_obstacles_dirty = true;
	for (int32_t tile = 0; tile < water->tiles_per_side * water->tiles_per_side; tile++)
	{
		water->tile_obstacles_dirty[tile] = true;
	}
	{
		int64_t covered = 0;

		for (size_t i = 0; i < cell_count; i++)
		{
			covered += water->obstacle_centimeters[i] > 0 ? 1 : 0;
		}
		SDL_Log("Physics: %d walls for the water over %lld cells", physics->wall_count, (long long)covered);
	}
}

// ---------------------------------------------------------------------------
// Water on bodies

typedef struct
{
	b3Vec3 forces[PHYSICS_SAMPLES_MAX];
	b3Pos points[PHYSICS_SAMPLES_MAX];
	int count;
	float buoyancy;                // total upward push, newtons
	float horizontal_drag_x;
	float horizontal_drag_z;
	float fastest_flow;            // at the submerged points, m/s
	float submerged;               // 0..1, the share of the volume under water
	float lowest_clearance;        // height of the lowest point over the ground under it
	bool wet;                      // water under some point (deep enough to count), submerged or not
	float deepest;                 // the deepest water under its points (m)
} water_push_t;

static bool water_tile_active(const water_t *water, float x, float z)
{
	int32_t cell_x = (int32_t)(x / water->cell_size_meters);
	int32_t cell_y = (int32_t)(z / water->cell_size_meters);

	if (cell_x < 0 || cell_y < 0 || cell_x >= water->size || cell_y >= water->size)
	{
		return false;
	}
	return water->tile_state[water_tile_index_of_cell(water, cell_x, cell_y)] == WATER_TILE_ACTIVE;
}

// What the water does to a body placed by transform. Moving bodies count
// their own speed against the water's.
// Faster than this, flow seen by the physics is numerical noise (a nearly
// empty cell with a big flux left in its pipes), not water: Manning
// friction holds real water below about this anyway
#define PHYSICS_MAXIMUM_FLOW_SPEED 10.0f

// The water at a point as the bodies feel it: the depth interpolated
// between the four nearest cells (so a body drifting from cell to cell
// does not feel steps of a few millimeters and bob), the flow of the
// nearest cell with its speed capped, and nothing at all where the water is
// shallower than water_minimum_meters. False outside the map.
static bool sample_water(const water_t *water, const physics_settings_t *settings, float world_x, float world_z, float *depth_out,
	float *flow_x_out, float *flow_z_out)
{
	float cell = water->cell_size_meters;
	float grid_x = world_x / cell;
	float grid_z = world_z / cell;
	int32_t x0 = (int32_t)SDL_floorf(grid_x);
	int32_t z0 = (int32_t)SDL_floorf(grid_z);
	float u = grid_x - (float)x0;
	float v = grid_z - (float)z0;
	int32_t last = water->size - 1;
	int32_t x1 = clamp_int(x0 + 1, 0, last);
	int32_t z1 = clamp_int(z0 + 1, 0, last);
	float nearest_depth;
	float speed;

	if (!water_sample(water, world_x, world_z, &nearest_depth, flow_x_out, flow_z_out))
	{
		*depth_out = 0.0f;
		return false;
	}
	x0 = clamp_int(x0, 0, last);
	z0 = clamp_int(z0, 0, last);
	*depth_out = ((1.0f - u) * (1.0f - v) * (float)water->depth[z0 * water->size + x0]
		+ u * (1.0f - v) * (float)water->depth[z0 * water->size + x1]
		+ (1.0f - u) * v * (float)water->depth[z1 * water->size + x0]
		+ u * v * (float)water->depth[z1 * water->size + x1]) * TERRAIN_METERS_PER_HEIGHT_UNIT;
	if (*depth_out < settings->water_minimum_meters || nearest_depth < settings->water_minimum_meters)
	{
		*depth_out = 0.0f;
		*flow_x_out = 0.0f;
		*flow_z_out = 0.0f;
		return true;
	}
	speed = SDL_sqrtf(*flow_x_out * *flow_x_out + *flow_z_out * *flow_z_out);
	if (speed > PHYSICS_MAXIMUM_FLOW_SPEED)
	{
		*flow_x_out *= PHYSICS_MAXIMUM_FLOW_SPEED / speed;
		*flow_z_out *= PHYSICS_MAXIMUM_FLOW_SPEED / speed;
	}
	return true;
}

static void compute_water_push(const physics_t *physics, const physics_body_t *body, const physics_type_t *physics_type,
	const terrain_t *terrain, const water_t *water, const physics_settings_t *settings, b3WorldTransform transform, bool moving,
	float step_seconds, water_push_t *push)
{
	float scale = body->scale;
	// A small one: one point, its middle (a pebble needs no 27)
	bool single = physics_type->largest_extent * scale < settings->small_body_meters;
	int sample_count = single ? 1 : physics_type->sample_count;
	float share = body->displaced_volume / (float)sample_count;
	float area = SDL_powf(share, 2.0f / 3.0f);
	float point_mass = body->mass / (float)sample_count;
	float slab = (single ? physics_type->whole_height : physics_type->sample_height) * scale;

	SDL_zerop(push);
	push->lowest_clearance = 1e30f;
	// (on the ice that bears: the water under it pushes nothing)
	if (physics->ice_water != NULL)
	{
		return;
	}
	for (int i = 0; i < sample_count; i++)
	{
		const float *local = single ? physics_type->center_point : physics_type->sample_points[i];
		b3Vec3 offset = b3RotateVector(transform.q, vector(local[0] * scale, local[1] * scale, local[2] * scale));
		b3Pos point = vector(transform.p.x + offset.x, transform.p.y + offset.y, transform.p.z + offset.z);
		float depth;
		float flow_x;
		float flow_z;
		float ground;
		float submerged;
		b3Vec3 point_velocity = vector(0.0f, 0.0f, 0.0f);
		b3Vec3 relative;
		float speed;
		b3Vec3 force;

		if (!sample_water(water, settings, point.x, point.z, &depth, &flow_x, &flow_z))
		{
			continue;
		}
		ground = terrain_sample_height_meters(terrain, point.x, point.z);
		push->lowest_clearance = SDL_min(push->lowest_clearance, point.y - 0.5f * slab - ground);
		if (depth <= 0.0f)
		{
			continue;
		}
		push->wet = true;
		push->deepest = SDL_max(push->deepest, depth);
		submerged = SDL_clamp((ground + depth - point.y) / slab + 0.5f, 0.0f, 1.0f);
		if (submerged <= 0.0f)
		{
			continue;
		}
		if (moving)
		{
			point_velocity = b3Body_GetWorldPointVelocity(body->id, point);
		}
		relative = vector(flow_x - point_velocity.x, -point_velocity.y, flow_z - point_velocity.z);
		speed = b3Length(relative);
		force = b3MulSV(0.5f * WATER_DENSITY * settings->drag_coefficient * area * submerged * speed, relative);
		if (moving)
		{
			// Never more than stops the point against the water in a step
			// (a stiff push on a light point would overshoot and shake)
			float limit = 0.5f * point_mass * speed / SDL_max(step_seconds, 1e-3f);
			float magnitude = b3Length(force);

			if (magnitude > limit)
			{
				force = b3MulSV(limit / magnitude, force);
			}
		}
		push->horizontal_drag_x += force.x;
		push->horizontal_drag_z += force.z;
		// Floating things bob: buoyancy and weight make a spring, and drag,
		// growing with the square of the speed, hardly brakes slow motion.
		// A brake in proportion to the vertical speed settles them.
		if (moving)
		{
			force.y -= settings->float_damping_per_second * point_mass * submerged * point_velocity.y;
		}
		force.y += submerged * share * WATER_DENSITY * GRAVITY;
		push->buoyancy += submerged * share * WATER_DENSITY * GRAVITY;
		push->submerged += submerged / (float)sample_count;
		push->fastest_flow = SDL_max(push->fastest_flow, SDL_sqrtf(flow_x * flow_x + flow_z * flow_z));
		push->forces[push->count] = force;
		push->points[push->count] = point;
		push->count++;
	}
}

// The water against a wall (a house): it keeps the water out, so it is
// pushed from outside. Sampled just outside its four sides: the
// difference of the water's weight on opposite sides, and the flow
// against it. Returns the force along world X and Z.
static void compute_wall_push(const physics_type_t *physics_type, float scale, float x, float z, const terrain_t *terrain,
	const water_t *water, const physics_settings_t *settings, float *force_x_out, float *force_z_out)
{
	float reach = SDL_max(physics_type->footprint_half_width_x, physics_type->footprint_half_width_z) * scale;
	float width = 2.0f * reach * 0.85f;
	float offsets[4][2] = { { reach + 0.75f, 0.0f }, { -reach - 0.75f, 0.0f }, { 0.0f, reach + 0.75f }, { 0.0f, -reach - 0.75f } };
	float depths[4];
	float average_depth = 0.0f;
	float flow_x_sum = 0.0f;
	float flow_z_sum = 0.0f;
	float speed;

	(void)terrain;
	for (int side = 0; side < 4; side++)
	{
		float flow_x;
		float flow_z;

		sample_water(water, settings, x + offsets[side][0], z + offsets[side][1], &depths[side], &flow_x, &flow_z);
		average_depth += 0.25f * depths[side];
		flow_x_sum += 0.25f * flow_x;
		flow_z_sum += 0.25f * flow_z;
	}
	speed = SDL_sqrtf(flow_x_sum * flow_x_sum + flow_z_sum * flow_z_sum);
	// Hydrostatic: rho g h^2 / 2 per meter of wall, from the -X side to +X
	*force_x_out = 0.5f * WATER_DENSITY * GRAVITY * (depths[1] * depths[1] - depths[0] * depths[0]) * width
		+ 0.5f * WATER_DENSITY * settings->drag_coefficient * width * average_depth * speed * flow_x_sum;
	*force_z_out = 0.5f * WATER_DENSITY * GRAVITY * (depths[3] * depths[3] - depths[2] * depths[2]) * width
		+ 0.5f * WATER_DENSITY * settings->drag_coefficient * width * average_depth * speed * flow_z_sum;
}

static void apply_push(const physics_body_t *body, const water_push_t *push, bool wake)
{
	for (int i = 0; i < push->count; i++)
	{
		b3Body_ApplyForce(body->id, push->forces[i], push->points[i], wake);
	}
	// Water stops tumbling quickly
	b3Body_SetAngularDamping(body->id, AIR_ANGULAR_DAMPING + WATER_ANGULAR_DAMPING * push->submerged);
	b3Body_SetLinearDamping(body->id, AIR_LINEAR_DAMPING + 0.3f * push->submerged);
}

// An anchored object comes loose: it moves from now on, and a tree tips
// over in the direction it was pushed
static void uproot(physics_t *physics, objects_t *objects, const terrain_t *terrain, int slot, float push_x, float push_z)
{
	physics_body_t *body = &physics->bodies[slot];
	object_instance_t *instance = &objects->instances[body->instance];
	const object_type_t *type = &objects->types.types[instance->type];
	const physics_type_t *physics_type = &physics->types[instance->type];
	float length = SDL_sqrtf(push_x * push_x + push_z * push_z);

	ensure_tiles_around(physics, terrain, instance->x, instance->z, body->scale * physics_type->height);
	if (physics_type->standing_mesh != NULL && physics_type->shape != OBJECT_SHAPE_MESH)
	{
		// Standing it had its real shape; a mesh cannot move: it falls with
		// its moving shape, of the same mass
		b3ShapeId shapes[8];
		int count = b3Body_GetShapes(body->id, shapes, 8);
		b3ShapeDef shape_definition = b3DefaultShapeDef();
		float volume = physics_type->volume * body->scale * body->scale * body->scale;

		for (int k = 0; k < count; k++)
		{
			b3DestroyShape(shapes[k], false);
		}
		shape_definition.density = body->mass / SDL_max(volume, 1e-6f);
		shape_definition.baseMaterial.friction = 0.6f;
		shape_definition.enableHitEvents = true;
		add_moving_shape(physics_type, body->id, &shape_definition, body->scale);
	}
	b3Body_SetType(body->id, b3_dynamicBody);
	b3Body_ApplyMassFromShapes(body->id);
	body_list_remove(physics, slot);
	body->kind = PHYSICS_BODY_DYNAMIC;
	body_list_insert(physics, slot);
	lift_above_ground(body->id, terrain);
	instance->state |= OBJECT_STATE_UPROOTED;
	objects_set_free(objects, body->instance, true);
	b3Body_SetAwake(body->id, true);
	body->awake = true;
	if (type->falls_when_uprooted && length > 0.0f)
	{
		// A shove at the top, along the push
		b3Pos top = b3Body_GetWorldPoint(body->id, vector(0.0f, physics_type->top * body->scale, 0.0f));
		float strength = body->mass * 1.5f / length;

		b3Body_ApplyLinearImpulse(body->id, vector(push_x * strength, 0.0f, push_z * strength), top, true);
	}
	physics->uprooted_total++;
	SDL_Log("Physics: %s torn loose at (%.1f, %.1f)", type->name, (double)instance->x, (double)instance->z);
}

// Spawns the debris a destroyed object throws out, loose and awake
static void spawn_debris(physics_t *physics, objects_t *objects, const terrain_t *terrain, int instance_index)
{
	object_instance_t source = objects->instances[instance_index];
	const object_type_t *type = &objects->types.types[source.type];
	const physics_type_t *physics_type = &physics->types[source.type];
	int placed = 0;

	if (type->debris_type_count == 0)
	{
		return;
	}
	for (int k = 0; k < type->debris_count * 3 && placed < type->debris_count; k++)
	{
		int debris_type = type->debris_types[k % type->debris_type_count];
		float angle = random_unit(physics) * 2.0f * SDL_PI_F;
		float distance = random_unit(physics) * SDL_max(physics_type->footprint_half_width_x, physics_type->footprint_half_width_z) * source.scale;
		float x = source.x + SDL_cosf(angle) * distance;
		float z = source.z + SDL_sinf(angle) * distance;
		float yaw;
		float scale;
		object_instance_t *debris;

		if (debris_type < 0)
		{
			continue;
		}
		objects_random_placement(objects, debris_type, random_unit(physics), random_unit(physics), &yaw, &scale);
		if (!objects_add(objects, terrain, debris_type, x, z, yaw, scale, 0))
		{
			continue;
		}
		debris = &objects->instances[objects->instance_count - 1];
		debris->state |= OBJECT_STATE_FREE;
		debris->y = objects_instance_base_y(&source) + (0.3f + 0.5f * random_unit(physics)) * physics_type->height * source.scale;
		// -2: a body that starts awake (see create_body)
		debris->body = -2;
		placed++;
	}
}

// One step of damage; returns true if the object is gone for good
static bool take_damage(physics_t *physics, objects_t *objects, const terrain_t *terrain, int slot)
{
	physics_body_t *body = &physics->bodies[slot];
	object_instance_t *instance = &objects->instances[body->instance];
	const object_type_t *type = &objects->types.types[instance->type];

	if (physics->clock_seconds < body->damage_ready_seconds || instance->damage >= OBJECT_DAMAGE_DESTROYED)
	{
		return false;
	}
	body->damage_ready_seconds = physics->clock_seconds + 1.0;
	instance->damage++;
	objects_touch(objects, body->instance);
	physics->walls_dirty = true;
	objects->far_shadow_dirty = true;
	objects->near_shadow_dirty = true;
	SDL_Log("Physics: %s %s", type->name, instance->damage == OBJECT_DAMAGE_DESTROYED ? "destroyed" : "damaged");
	if (instance->damage == OBJECT_DAMAGE_DESTROYED)
	{
		spawn_debris(physics, objects, terrain, body->instance);
		// Re-read: spawning may have moved the instance array
		instance = &objects->instances[body->instance];
		return type->destroyed_model < 0;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Flood: grass and such

static void flatten_by_water(physics_t *physics, objects_t *objects, const water_t *water, uint8_t *remove, bool *any_removed,
	float check_seconds)
{
	float tile_meters = (float)WATER_TILE_SIZE * water->cell_size_meters;
	bool changed = false;
	bool any_flood = false;

	// (no type the water lays down or takes: nothing to look for)
	for (int t = 0; t < objects->types.type_count && !any_flood; t++)
	{
		any_flood = objects->types.types[t].physics == OBJECT_PHYSICS_FLOOD && objects->types.types[t].flood != OBJECT_FLOOD_STAY;
	}
	if (!any_flood)
	{
		return;
	}

	for (int32_t tile = 0; tile < water->tiles_per_side * water->tiles_per_side; tile++)
	{
		int first_i;
		int first_j;
		int last_i;
		int last_j;
		float minimum_x;
		float minimum_z;

		if (water->tile_state[tile] != WATER_TILE_ACTIVE)
		{
			continue;
		}
		minimum_x = (float)(tile % water->tiles_per_side) * tile_meters;
		minimum_z = (float)(tile / water->tiles_per_side) * tile_meters;
		first_i = SDL_clamp((int)(minimum_x / OBJECTS_GRID_CELL_METERS), 0, objects->grid_size - 1);
		first_j = SDL_clamp((int)(minimum_z / OBJECTS_GRID_CELL_METERS), 0, objects->grid_size - 1);
		last_i = SDL_clamp((int)((minimum_x + tile_meters) / OBJECTS_GRID_CELL_METERS), 0, objects->grid_size - 1);
		last_j = SDL_clamp((int)((minimum_z + tile_meters) / OBJECTS_GRID_CELL_METERS), 0, objects->grid_size - 1);
		for (int j = first_j; j <= last_j; j++)
		{
			for (int i = first_i; i <= last_i; i++)
			{
				int cell = j * objects->grid_size + i;

				for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1]; k++)
				{
					object_instance_t *instance = &objects->instances[k];
					const object_type_t *type = &objects->types.types[instance->type];
					float depth;
					float flow_x;
					float flow_z;
					float speed;

					if (type->physics != OBJECT_PHYSICS_FLOOD || type->flood == OBJECT_FLOOD_STAY || remove[k])
					{
						continue;
					}
					// Only the part of the cell inside this water tile
					if (instance->x < minimum_x || instance->z < minimum_z || instance->x >= minimum_x + tile_meters
						|| instance->z >= minimum_z + tile_meters)
					{
						continue;
					}
					water_sample(water, instance->x, instance->z, &depth, &flow_x, &flow_z);
					speed = SDL_sqrtf(flow_x * flow_x + flow_z * flow_z);
					if (depth < type->flood_depth_meters || speed < type->flood_speed_meters_per_second)
					{
						continue;
					}
					if (type->flood == OBJECT_FLOOD_DESTROY)
					{
						remove[k] = 1;
						*any_removed = true;
						continue;
					}
					if (instance->flatten < 1.0f)
					{
						if (instance->flatten <= 0.0f)
						{
							instance->flatten_yaw = SDL_atan2f(flow_x, flow_z);
							remember_flattened(physics, instance->id);
						}
						// Down within a fraction of a second
						instance->flatten = SDL_min(instance->flatten + check_seconds * 4.0f, 1.0f);
						objects_update_rotation(instance);
						objects_touch(objects, k);
						changed = true;
					}
				}
			}
		}
	}
	if (changed)
	{
		objects->near_shadow_dirty = true;
	}
}

// Laid down plants stand up again once the water is gone
static void recover_flattened(physics_t *physics, objects_t *objects, const water_t *water, float elapsed_seconds)
{
	int still_down = 0;
	bool changed = false;

	if (physics->flattened_count <= 0)
	{
		return;
	}
	// (those laid down, by their ids: gone or up again, off the list)
	for (int n = 0; n < physics->flattened_count; n++)
	{
		uint32_t id = physics->flattened_ids[n];
		int k = objects_find_id(objects, id);
		object_instance_t *instance;
		const object_type_t *type;
		float depth;
		float flow_x;
		float flow_z;

		if (k < 0)
		{
			continue;
		}
		instance = &objects->instances[k];
		type = &objects->types.types[instance->type];
		if (instance->flatten <= 0.0f)
		{
			continue;
		}
		if (type->flood_recover_seconds > 0.0f)
		{
			water_sample(water, instance->x, instance->z, &depth, &flow_x, &flow_z);
			if (depth < type->flood_depth_meters)
			{
				instance->flatten = SDL_max(instance->flatten - elapsed_seconds / type->flood_recover_seconds, 0.0f);
				objects_update_rotation(instance);
				objects_touch(objects, k);
				changed = true;
			}
		}
		if (instance->flatten > 0.0f)
		{
			physics->flattened_ids[still_down++] = id;
		}
	}
	physics->flattened_count = still_down;
	if (changed)
	{
		objects->near_shadow_dirty = true;
	}
}

// ---------------------------------------------------------------------------
// The step

static int slot_of_body(b3BodyId id)
{
	return (int)(intptr_t)b3Body_GetUserData(id) - 1;
}

// ---------------------------------------------------------------------------
// The walker pushing things

#define WALKER_HEAVY_KILOGRAMS 2000.0f  // loose things heavier than this are not pushed at all
#define WALKER_LIGHT_KILOGRAMS 150.0f   // loose things lighter than this do not stop it (it kicks them away)
#define WALKER_RADIUS_METERS 0.35f
#define WALKER_PUSH_SPIN_MAXIMUM 2.0f     // radians per second a push leaves a thing spinning at most
#define WALKER_PUSH_FROM_METERS 0.3f    // the capsule's lowest point over the feet (small steps are no hit)
#define WALKER_PUSH_TO_METERS 1.6f      // and its highest

typedef struct
{
	physics_t *physics;
	float x;                       // the walker
	float z;
	float feet_height;
	float velocity_x;
	float velocity_z;
	float walker_kilograms;
	int pushed;
} walker_push_t;

static bool walker_push_plane(b3ShapeId shape, const b3PlaneResult *planes, int plane_count, void *context)
{
	walker_push_t *push = context;
	b3BodyId id = b3Shape_GetBody(shape);
	int slot = slot_of_body(id);
	physics_body_t *body;

	// Only light loose bodies: the ground, anchored and static ones, and
	// heavy ones (a boulder: it stops the walker) stay put
	if (slot < 0 || slot >= push->physics->body_capacity || !push->physics->bodies[slot].used
		|| push->physics->bodies[slot].kind != PHYSICS_BODY_DYNAMIC || push->physics->bodies[slot].mass >= WALKER_HEAVY_KILOGRAMS)
	{
		return true;
	}
	body = &push->physics->bodies[slot];
	for (int p = 0; p < plane_count; p++)
	{
		// The plane points from the body to the walker: the push goes the
		// other way, along the ground
		float direction_x = -planes[p].plane.normal.x;
		float direction_z = -planes[p].plane.normal.z;
		float length = SDL_sqrtf(direction_x * direction_x + direction_z * direction_z);
		b3Vec3 velocity;
		float walking;
		float moving;
		float effective_mass;
		float impulse;

		if (length < 1e-3f)
		{
			continue;
		}
		direction_x /= length;
		direction_z /= length;
		// Away from the walker, whichever side of the shape it touches (past
		// a log's middle the planes face the other way)
		{
			b3Vec3 center = b3ToVec3(b3Body_GetWorldCenter(id));

			if ((center.x - push->x) * direction_x + (center.z - push->z) * direction_z < 0.0f)
			{
				direction_x = -direction_x;
				direction_z = -direction_z;
			}
		}
		walking = push->velocity_x * direction_x + push->velocity_z * direction_z;
		if (walking <= 0.0f)
		{
			continue;                  // walking away from it
		}
		velocity = b3Body_GetLinearVelocity(id);
		moving = velocity.x * direction_x + velocity.z * direction_z;
		if (moving >= walking)
		{
			continue;                  // already going faster than the walker
		}
		// A walker brings something light up to its own speed at once, and
		// shoves something heavy only as hard as it can (its own weight's
		// worth of momentum, a few times over)
		effective_mass = SDL_min(body->mass, 3.0f * push->walker_kilograms);
		impulse = effective_mass * (walking - moving) / (float)plane_count;
		// Where it touches (the plane's point is relative to the walker),
		// but at the height of the thing's middle: a shove at one end of a
		// log swings it round, it does not lever it up into the air
		{
			b3Vec3 middle = b3ToVec3(b3Body_GetWorldCenter(id));

			b3Body_ApplyLinearImpulse(id, vector(direction_x * impulse, 0.0f, direction_z * impulse),
				b3ToPos(vector(push->x + planes[p].point.x, middle.y, push->z + planes[p].point.z)), true);
		}
		// A shove, not a launch: nothing leaves faster than twice the walk,
		// or spins wildly (a light thing hit off center)
		{
			b3Vec3 after = b3Body_GetLinearVelocity(id);
			b3Vec3 spin = b3Body_GetAngularVelocity(id);
			float horizontal = SDL_sqrtf(after.x * after.x + after.z * after.z);
			float most = 2.0f * walking + 0.5f;
			float spin_length = SDL_sqrtf(spin.x * spin.x + spin.y * spin.y + spin.z * spin.z);

			if (horizontal > most)
			{
				after.x *= most / horizontal;
				after.z *= most / horizontal;
				b3Body_SetLinearVelocity(id, after);
			}
			if (spin_length > WALKER_PUSH_SPIN_MAXIMUM)
			{
				b3Body_SetAngularVelocity(id, b3MulSV(WALKER_PUSH_SPIN_MAXIMUM / spin_length, spin));
			}
		}
		push->pushed++;
	}
	return true;
}

#define WALKER_MOVE_RADIUS_METERS 0.3f
#define WALKER_MOVE_TOP_METERS 1.7f
#define WALKER_WALL_LEAST_LEAN 0.5f      // a plane stops the walker if its normal is at least this much sideways
#define WALKER_HEAD_FROM_METERS 1.2f     // over the feet: what it meets there only stops it if upright
#define WALKER_HEAD_LEAST_LEAN 0.25f     // ...that is, its normal's upward part under this
#define WALKER_MOVE_STEP_METERS 0.15f   // a move is cut into steps this long (no going through a plank)
#define WALKER_PLANES_MAX 32

typedef struct
{
	physics_t *physics;
	b3CollisionPlane planes[WALKER_PLANES_MAX];
	int count;
} walker_planes_t;

static bool walker_collect_plane(b3ShapeId shape, const b3PlaneResult *planes, int plane_count, void *context)
{
	walker_planes_t *collected = context;
	int slot = slot_of_body(b3Shape_GetBody(shape));
	const physics_body_t *body;

	// The ground is the caller's; light loose things get kicked away, not
	// leaned on. Heavier ones stop it and are pushed meanwhile: it follows
	// as fast as they give way (a big log, broadside, hardly at all)
	if (slot < 0 || slot >= collected->physics->body_capacity || !collected->physics->bodies[slot].used)
	{
		return true;
	}
	body = &collected->physics->bodies[slot];
	if (body->kind == PHYSICS_BODY_DYNAMIC && body->mass < WALKER_LIGHT_KILOGRAMS)
	{
		return true;
	}
	for (int p = 0; p < plane_count && collected->count < WALKER_PLANES_MAX; p++)
	{
		b3CollisionPlane *plane = &collected->planes[collected->count++];
		float length;

		// Walls push sideways only: the height is the ground's business
		plane->plane = planes[p].plane;
		plane->plane.normal.y = 0.0f;
		length = SDL_sqrtf(plane->plane.normal.x * plane->plane.normal.x + plane->plane.normal.z * plane->plane.normal.z);
		// (a floor, a ceiling, the rounded top edge of a deck: mostly
		// level, the ground's business too. Pushed out sideways it would
		// be by its depth over how little it leans: a plane leaning 1 in
		// 50 threw the walker 50 times as far as it went in, off the end
		// of a terrace 10 or 20 m)
		// (nor a slope met with the head: the underside of a low roof over
		// a gallery, leaning, pushed the walker along it as it went, out
		// toward its eave; walls are upright, at any height)
		if (length < WALKER_WALL_LEAST_LEAN
			|| (planes[p].point.y > WALKER_HEAD_FROM_METERS && SDL_fabsf(planes[p].plane.normal.y) > WALKER_HEAD_LEAST_LEAN))
		{
			collected->count--;
			collected->physics->walker_planes_skipped++;
			continue;
		}
		collected->physics->walker_planes_kept++;
		// (the deepest one, for the debug window)
		if (SDL_fabsf(planes[p].plane.offset) > collected->physics->walker_worst_depth)
		{
			collected->physics->walker_worst_depth = SDL_fabsf(planes[p].plane.offset);
			collected->physics->walker_worst_normal[0] = planes[p].plane.normal.x;
			collected->physics->walker_worst_normal[1] = planes[p].plane.normal.y;
			collected->physics->walker_worst_normal[2] = planes[p].plane.normal.z;
			collected->physics->walker_worst_height = planes[p].point.y;
		}
		plane->plane.normal.x /= length;
		plane->plane.normal.z /= length;
		plane->plane.offset = planes[p].plane.offset / length;
		plane->pushLimit = FLT_MAX;
		plane->push = 0.0f;
		plane->clipVelocity = true;
	}
	return true;
}

void physics_move_walker(physics_t *physics, float *x, float *z, float feet_height, float move_x, float move_z, float step_meters)
{
	float length = SDL_sqrtf(move_x * move_x + move_z * move_z);
	int steps;
	b3Capsule capsule;
	b3QueryFilter filter = b3DefaultQueryFilter();

	if (!physics->created)
	{
		*x += move_x;
		*z += move_z;
		return;
	}
	// Its lowest part a step over the feet: what is lower it steps onto
	capsule.center1 = vector(0.0f, step_meters + WALKER_MOVE_RADIUS_METERS, 0.0f);
	capsule.center2 = vector(0.0f, SDL_max(WALKER_MOVE_TOP_METERS - WALKER_MOVE_RADIUS_METERS, step_meters + WALKER_MOVE_RADIUS_METERS),
		0.0f);
	capsule.radius = WALKER_MOVE_RADIUS_METERS;
	steps = SDL_max(1, (int)SDL_ceilf(length / WALKER_MOVE_STEP_METERS));
	physics->walker_planes_kept = 0;
	physics->walker_planes_skipped = 0;
	physics->walker_push_x = 0.0f;
	physics->walker_push_z = 0.0f;
	physics->walker_worst_depth = 0.0f;
	physics->walker_worst_height = 0.0f;
	SDL_zeroa(physics->walker_worst_normal);
	for (int step = 0; step <= steps; step++)
	{
		// Each step goes on, then whatever it overlaps pushes it back out
		// (a few rounds, for corners); step 0 only frees it where it stands
		if (step > 0)
		{
			*x += move_x / (float)steps;
			*z += move_z / (float)steps;
		}
		for (int round = 0; round < 3; round++)
		{
			walker_planes_t collected;
			b3PlaneSolverResult result;

			collected.physics = physics;
			collected.count = 0;
			b3World_CollideMover(physics->world, b3ToPos(vector(*x, feet_height, *z)), &capsule, filter, walker_collect_plane, &collected);
			if (collected.count == 0)
			{
				break;
			}
			result = b3SolvePlanes(vector(0.0f, 0.0f, 0.0f), collected.planes, collected.count);
			*x += result.delta.x;
			*z += result.delta.z;
			physics->walker_push_x += result.delta.x;
			physics->walker_push_z += result.delta.z;
			if (result.delta.x * result.delta.x + result.delta.z * result.delta.z < 1e-8f)
			{
				break;
			}
		}
	}
}

void physics_push_with_walker(physics_t *physics, float x, float feet_height, float z, float velocity_x, float velocity_z,
	float walker_kilograms)
{
	walker_push_t push = { physics, x, z, feet_height, velocity_x, velocity_z, walker_kilograms, 0 };
	b3Capsule capsule;
	b3QueryFilter filter = b3DefaultQueryFilter();

	if (!physics->created || velocity_x * velocity_x + velocity_z * velocity_z < 1e-4f)
	{
		return;
	}
	capsule.center1 = vector(0.0f, WALKER_PUSH_FROM_METERS + WALKER_RADIUS_METERS, 0.0f);
	capsule.center2 = vector(0.0f, WALKER_PUSH_TO_METERS - WALKER_RADIUS_METERS, 0.0f);
	capsule.radius = WALKER_RADIUS_METERS;
	b3World_CollideMover(physics->world, b3ToPos(vector(x, feet_height, z)), &capsule, filter, walker_push_plane, &push);
}

// Hits tear anchored things loose and damage breakable ones
static void handle_hits(physics_t *physics, objects_t *objects, const terrain_t *terrain, const physics_settings_t *settings,
	uint8_t *remove, bool *any_removed)
{
	b3ContactEvents events = b3World_GetContactEvents(physics->world);

	for (int e = 0; e < events.hitCount; e++)
	{
		const b3ContactHitEvent *hit = &events.hitEvents[e];
		b3BodyId body_a = b3Shape_GetBody(hit->shapeIdA);
		b3BodyId body_b = b3Shape_GetBody(hit->shapeIdB);
		int slots[2] = { slot_of_body(body_a), slot_of_body(body_b) };
		float masses[2];
		float effective_mass;
		float force;

		for (int side = 0; side < 2; side++)
		{
			masses[side] = slots[side] >= 0 && physics->bodies[slots[side]].used && physics->bodies[slots[side]].kind == PHYSICS_BODY_DYNAMIC
				? physics->bodies[slots[side]].mass : 0.0f;
		}
		effective_mass = masses[0] > 0.0f && masses[1] > 0.0f ? masses[0] * masses[1] / (masses[0] + masses[1]) : SDL_max(masses[0], masses[1]);
		force = effective_mass * hit->approachSpeed / SDL_max(settings->hit_seconds, 0.01f);
		for (int side = 0; side < 2; side++)
		{
			physics_body_t *body;

			if (slots[side] < 0 || !physics->bodies[slots[side]].used)
			{
				continue;
			}
			body = &physics->bodies[slots[side]];
			if (body->kind == PHYSICS_BODY_ANCHORED && force > body->anchor_newtons)
			{
				float direction = side == 0 ? -1.0f : 1.0f;

				uproot(physics, objects, terrain, slots[side], hit->normal.x * direction, hit->normal.z * direction);
			}
			if (body->damage_newtons > 0.0f && force > body->damage_newtons
				&& take_damage(physics, objects, terrain, slots[side]))
			{
				remove[body->instance] = 1;
				*any_removed = true;
			}
		}
	}
}

// Objects drawn in between (physics_interpolate) go back to where their
// bodies are, for the step to see them right
static void restore_poses(physics_t *physics, objects_t *objects)
{
	for (int k = 0; k < physics->moved_count; k++)
	{
		int slot = physics->moved_slots[k];
		physics_body_t *body = &physics->bodies[slot];
		object_instance_t *instance;

		if (slot >= physics->body_capacity || !body->used || body->instance >= objects->instance_count
			|| objects->instances[body->instance].body != slot)
		{
			continue;
		}
		instance = &objects->instances[body->instance];
		instance->x = body->pose_current[0];
		instance->y = body->pose_current[1];
		instance->z = body->pose_current[2];
		SDL_memcpy(instance->rotation, &body->pose_current[3], sizeof(float) * 4);
	}
	// (those that moved: awake, the water's push looks at them this step
	// wherever they are)
	if (physics->moved_count > physics->awake_capacity)
	{
		int *grown = SDL_realloc(physics->awake_slots, sizeof(int) * (size_t)physics->moved_count);

		if (grown != NULL)
		{
			physics->awake_slots = grown;
			physics->awake_capacity = physics->moved_count;
		}
	}
	physics->awake_count = SDL_min(physics->moved_count, physics->awake_capacity);
	if (physics->awake_count > 0)
	{
		SDL_memcpy(physics->awake_slots, physics->moved_slots, sizeof(int) * (size_t)physics->awake_count);
	}
	physics->moved_count = 0;
}

void physics_interpolate(physics_t *physics, objects_t *objects, float amount)
{
	amount = SDL_clamp(amount, 0.0f, 1.0f);
	for (int k = 0; k < physics->moved_count; k++)
	{
		int slot = physics->moved_slots[k];
		const physics_body_t *body = &physics->bodies[slot];
		object_instance_t *instance;
		const float *from = body->pose_previous;
		const float *to = body->pose_current;
		float sign;
		float rotation[4];
		float length = 0.0f;

		if (slot >= physics->body_capacity || !body->used || body->instance >= objects->instance_count
			|| objects->instances[body->instance].body != slot)
		{
			continue;
		}
		instance = &objects->instances[body->instance];
		instance->x = from[0] + (to[0] - from[0]) * amount;
		instance->y = from[1] + (to[1] - from[1]) * amount;
		instance->z = from[2] + (to[2] - from[2]) * amount;
		// The shorter way round, normalized
		sign = from[3] * to[3] + from[4] * to[4] + from[5] * to[5] + from[6] * to[6] < 0.0f ? -1.0f : 1.0f;
		for (int i = 0; i < 4; i++)
		{
			rotation[i] = from[3 + i] + (sign * to[3 + i] - from[3 + i]) * amount;
			length += rotation[i] * rotation[i];
		}
		length = SDL_max(SDL_sqrtf(length), 1e-6f);
		for (int i = 0; i < 4; i++)
		{
			instance->rotation[i] = rotation[i] / length;
		}
	}
}

// Sleeping neighbours of a moving body, to be woken
#define NEIGHBOUR_MARGIN_METERS 0.05f       // how far around a moving body sleeping ones are woken
#define NEIGHBOUR_WAKE_SPEED 0.1f           // meters (or radians) per second a body must move to wake them
#define NEIGHBOURS_MAX 64

typedef struct
{
	physics_t *physics;
	int slots[NEIGHBOURS_MAX];
	int count;
} neighbours_t;

static bool collect_sleeping_neighbour(b3ShapeId shape, void *context)
{
	neighbours_t *neighbours = context;
	b3BodyId id = b3Shape_GetBody(shape);
	int slot = slot_of_body(id);

	if (slot < 0 || slot >= neighbours->physics->body_capacity || !neighbours->physics->bodies[slot].used
		|| neighbours->physics->bodies[slot].kind != PHYSICS_BODY_DYNAMIC || b3Body_IsAwake(id))
	{
		return true;
	}
	for (int i = 0; i < neighbours->count; i++)
	{
		if (neighbours->slots[i] == slot)
		{
			return true;
		}
	}
	neighbours->slots[neighbours->count++] = slot;
	return neighbours->count < NEIGHBOURS_MAX;
}

// Bodies made asleep next to each other (a pile of logs) never get to touch
// in Box3D's eyes, so knocking one away does not wake the others: whatever
// sleeps against a body that moves is woken here, and falls if nothing
// holds it any more (or goes back to sleep if something does)
static void wake_neighbours(physics_t *physics, objects_t *objects, const terrain_t *terrain)
{
	for (int k = 0; k < physics->moved_count; k++)
	{
		const physics_body_t *body = &physics->bodies[physics->moved_slots[k]];
		neighbours_t neighbours;
		b3AABB box;
		b3Vec3 velocity;
		b3Vec3 spin;

		if (!body->used || body->kind != PHYSICS_BODY_DYNAMIC)
		{
			continue;
		}
		velocity = b3Body_GetLinearVelocity(body->id);
		spin = b3Body_GetAngularVelocity(body->id);
		if (b3Length(velocity) < NEIGHBOUR_WAKE_SPEED && b3Length(spin) < NEIGHBOUR_WAKE_SPEED)
		{
			continue;
		}
		box = b3Body_ComputeAABB(body->id);
		box.lowerBound = b3Sub(box.lowerBound, vector(NEIGHBOUR_MARGIN_METERS, NEIGHBOUR_MARGIN_METERS, NEIGHBOUR_MARGIN_METERS));
		box.upperBound = b3Add(box.upperBound, vector(NEIGHBOUR_MARGIN_METERS, NEIGHBOUR_MARGIN_METERS, NEIGHBOUR_MARGIN_METERS));
		neighbours.physics = physics;
		neighbours.count = 0;
		b3World_OverlapAABB(physics->world, box, b3DefaultQueryFilter(), collect_sleeping_neighbour, &neighbours);
		for (int i = 0; i < neighbours.count; i++)
		{
			physics_body_t *neighbour = &physics->bodies[neighbours.slots[i]];
			const object_instance_t *instance;

			if (neighbour->instance >= objects->instance_count || objects->instances[neighbour->instance].body != neighbours.slots[i])
			{
				continue;
			}
			instance = &objects->instances[neighbour->instance];
			// The ground under it first, or it falls through
			ensure_tiles_around(physics, terrain, instance->x, instance->z, neighbour->scale * physics->types[instance->type].height);
			b3Body_SetAwake(neighbour->id, true);
			neighbour->awake = true;
		}
	}
}

static void remember_moved(physics_t *physics, int slot)
{
	if (physics->moved_count == physics->moved_capacity)
	{
		int capacity = SDL_max(256, physics->moved_capacity * 2);
		int *grown = SDL_realloc(physics->moved_slots, sizeof(int) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		physics->moved_slots = grown;
		physics->moved_capacity = capacity;
	}
	physics->moved_slots[physics->moved_count++] = slot;
}

void physics_step(physics_t *physics, objects_t *objects, terrain_t *terrain, water_t *water, const physics_settings_t *settings,
	float step_seconds)
{
	uint64_t start_counter = SDL_GetPerformanceCounter();
	uint64_t section_start;
	uint8_t *remove;
	int count_before;
	bool any_removed = false;
	float map_meters = (float)terrain->size * terrain->cell_size_meters;

	if (!physics->created || !settings->enabled || physics->tiles == NULL)
	{
		return;
	}
	section_start = SDL_GetPerformanceCounter();
	restore_poses(physics, objects);
	objects_sort(objects);
	if (physics->needs_full_sync || physics->synced_generation != objects->generation || physics->walls_dirty)
	{
		// Walls only change with the objects (or their damage)
		physics->walls_dirty = false;
		sync_bodies(physics, objects, terrain);
		rebuild_walls(physics, objects, terrain, water, false);
	}
	else
	{
		// (the bodies wanted: round the focus, the cells nearest it made at
		// once (a jump somewhere), the rest a little a step; where the water
		// moves; round the actors (physics_request_bodies). Never taken away)
		if (physics->focus_set && !objects->grid_dirty && objects->grid_first != NULL && cell_states_ready(physics, objects))
		{
			int tile_count = water != NULL && water->tile_state != NULL ? water->tiles_per_side * water->tiles_per_side : 0;
			float tile_meters = water != NULL ? (float)WATER_TILE_SIZE * water->cell_size_meters : 0.0f;
			uint64_t ignored = 0;

			want_cells_round(physics, objects, physics->focus_x, physics->focus_z, BODIES_RANGE_METERS);
			{
				int last = objects->grid_size - 1;
				int first_x = SDL_clamp((int)((physics->focus_x - BODIES_AT_ONCE_METERS) / OBJECTS_GRID_CELL_METERS), 0, last);
				int first_z = SDL_clamp((int)((physics->focus_z - BODIES_AT_ONCE_METERS) / OBJECTS_GRID_CELL_METERS), 0, last);
				int last_x = SDL_clamp((int)((physics->focus_x + BODIES_AT_ONCE_METERS) / OBJECTS_GRID_CELL_METERS), 0, last);
				int last_z = SDL_clamp((int)((physics->focus_z + BODIES_AT_ONCE_METERS) / OBJECTS_GRID_CELL_METERS), 0, last);

				for (int j = first_z; j <= last_z; j++)
				{
					for (int i = first_x; i <= last_x; i++)
					{
						int cell = j * objects->grid_size + i;

						if (physics->cell_states[cell] == 2)
						{
							continue;
						}
						// (the queue's head begun: left to it)
						if (physics->queue_count > 0 && physics->cell_queue[physics->queue_head] == cell && physics->queue_cursor >= 0)
						{
							continue;
						}
						for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1]; k++)
						{
							make_body_for(physics, objects, terrain, k, &ignored);
						}
						for (int k = objects->sorted_count; k < objects->instance_count; k++)
						{
							if (body_cell_of(objects, objects->instances[k].x, objects->instances[k].z) == cell)
							{
								make_body_for(physics, objects, terrain, k, &ignored);
							}
						}
						// (still in the queue, if it was: done there at once)
						physics->cell_states[cell] = 2;
					}
				}
			}
			for (int t = 0; t < tile_count; t++)
			{
				if (water->tile_state[t] == WATER_TILE_ACTIVE)
				{
					float x = ((float)(t % water->tiles_per_side) + 0.5f) * tile_meters;
					float z = ((float)(t / water->tiles_per_side) + 0.5f) * tile_meters;

					want_cells_round(physics, objects, x, z, tile_meters * 0.75f);
				}
			}
			make_queued_cells(physics, objects, terrain);
		}
		if (physics->bodies_pending)
		{
			continue_bodies(physics, objects, terrain);
		}
		follow_changes(physics, objects, terrain);
	}
	// The ice comes to bear, or goes: every ground tile made again (its
	// surface on the water, or not), every body to notice
	if ((physics->ice_water != NULL) != (water != NULL && water->iced))
	{
		int tile_count = physics->tiles_per_side * physics->tiles_per_side;

		physics->ice_water = water != NULL && water->iced ? water : NULL;
		// What is under the water then is held in the ice (the ground
		// tiles' surface over it would push it about); let go after
		for (int slot = 0; slot < physics->body_capacity; slot++)
		{
			physics_body_t *body = &physics->bodies[slot];

			if (!body->used || body->kind != PHYSICS_BODY_DYNAMIC)
			{
				continue;
			}
			if (physics->ice_water != NULL)
			{
				b3WorldTransform transform = b3Body_GetTransform(body->id);
				float level;

				if (water_ice_surface(water, terrain, transform.p.x, transform.p.z, &level) && transform.p.y < level)
				{
					b3Body_Disable(body->id);
					body->in_ice = true;
					body->awake = false;
				}
			}
			else if (body->in_ice)
			{
				b3Body_Enable(body->id);
				body->in_ice = false;
			}
		}
		for (int i = 0; i < tile_count; i++)
		{
			if (physics->tiles[i].exists)
			{
				build_tile(physics, terrain, i % physics->tiles_per_side, i / physics->tiles_per_side);
			}
		}
		wake_bodies_in_rectangle(physics, objects, terrain, 0.0f, 0.0f, map_meters, map_meters);
	}
	frame_profile_end(FRAME_PHYSICS_SYNC, section_start);
	section_start = SDL_GetPerformanceCounter();
	follow_terrain(physics, objects, terrain, settings, step_seconds);
	frame_profile_end(FRAME_PHYSICS_TERRAIN, section_start);
	count_before = objects->instance_count;
	if (physics->remove_capacity < count_before)
	{
		uint8_t *grown = SDL_realloc(physics->remove, (size_t)count_before);

		if (grown == NULL)
		{
			return;
		}
		physics->remove = grown;
		physics->remove_capacity = count_before;
		SDL_memset(physics->remove, 0, (size_t)count_before);
	}
	remove = physics->remove;

	// The water on the bodies: the loose ones, and the anchored or
	// breakable ones on the ground tiles where the water moves (listed:
	// body_list_insert); the rest, a big map's millions of them asleep,
	// not looked at
	section_start = SDL_GetPerformanceCounter();
	physics->clock_seconds += step_seconds;
	physics->dynamic_body_count = 0;
	physics->static_body_count = 0;
	{
		int visit_count = 0;
		bool same_tiles = water != NULL && water->tile_state != NULL && water->tiles_per_side == physics->tiles_per_side;
		int tile_count = physics->tiles_per_side * physics->tiles_per_side;

		for (int pass = 0; pass < 2; pass++)
		{
			// (counted, then written; each once: stamped)
			visit_count = 0;
			physics->visit_stamp++;
			for (int n = 0; n < physics->awake_count; n++)
			{
				int slot = physics->awake_slots[n];

				if (slot >= physics->body_capacity || physics->bodies[slot].visit_stamp == physics->visit_stamp)
				{
					continue;
				}
				physics->bodies[slot].visit_stamp = physics->visit_stamp;
				if (pass == 1)
				{
					physics->visit_slots[visit_count] = slot;
				}
				visit_count++;
			}
			for (int t = 0; physics->bucket_heads != NULL && t < tile_count; t++)
			{
				if (physics->bucket_heads[t] < 0 || (same_tiles && water->tile_state[t] != WATER_TILE_ACTIVE))
				{
					continue;
				}
				for (int slot = physics->bucket_heads[t]; slot >= 0; slot = physics->bodies[slot].list_next)
				{
					if (physics->bodies[slot].visit_stamp == physics->visit_stamp)
					{
						continue;
					}
					physics->bodies[slot].visit_stamp = physics->visit_stamp;
					if (pass == 1)
					{
						physics->visit_slots[visit_count] = slot;
					}
					visit_count++;
				}
			}
			if (pass == 0 && visit_count > physics->visit_capacity)
			{
				int capacity = SDL_max(visit_count * 2, 1024);
				int *grown = SDL_realloc(physics->visit_slots, sizeof(int) * (size_t)capacity);

				if (grown == NULL)
				{
					visit_count = 0;
					break;
				}
				physics->visit_slots = grown;
				physics->visit_capacity = capacity;
			}
		}
		physics->visit_count_this_step = visit_count;
	}
	for (int visit = 0; visit < physics->visit_count_this_step; visit++)
	{
		int slot = physics->visit_slots[visit];
		physics_body_t *body = &physics->bodies[slot];
		object_instance_t *instance;
		const physics_type_t *physics_type;
		water_push_t push;
		b3WorldTransform transform;
		bool awake;

		if (!body->used)
		{
			continue;
		}
		// (held in the ice: nothing moves it)
		if (body->in_ice)
		{
			continue;
		}
		// Most bodies sleep where no water moves: decided from the body
		// alone, without touching the (big) object array
		if (body->kind == PHYSICS_BODY_DYNAMIC)
		{
			physics->dynamic_body_count++;
			awake = body->awake;
			if (!awake && !water_tile_active(water, body->x, body->z))
			{
				continue;
			}
			instance = &objects->instances[body->instance];
			physics_type = &physics->types[instance->type];
			transform = b3Body_GetTransform(body->id);
			compute_water_push(physics, body, physics_type, terrain, water, settings, transform, awake, step_seconds, &push);
			body->water_submerged = push.submerged;
			body->water_buoyancy_newtons = push.buoyancy;
			body->water_drag_newtons = SDL_sqrtf(push.horizontal_drag_x * push.horizontal_drag_x + push.horizontal_drag_z * push.horizontal_drag_z);
			body->water_flow_meters_per_second = push.fastest_flow;
			if (!awake)
			{
				float weight = body->mass * GRAVITY;
				// (a film of water does not wake it: its speed, its flux over
				// next to no depth, can be anything, and a rock woken settles
				// a few centimeters onto the ground's collision tiles with a
				// hop: left alone till the water stands a real share of it)
				bool deep_enough = push.deepest >= SDL_max(0.05f, 0.15f * body->scale * physics_type->height);
				bool pushed = push.count > 0 && deep_enough
					&& (push.fastest_flow > settings->wake_flow_meters_per_second || push.buoyancy > 1.1f * weight);
				// (the ground washed away under it: only where there is
				// water. A rock on a dry bank sat a little over the ground
				// the samples see, on a slope, its middle low: woken and
				// settled with a jerk every time water anywhere in its tile
				// moved, a water gun's splash fifty meters off)
				bool unsupported = push.wet && push.lowest_clearance > HOVER_METERS && push.lowest_clearance < 1e29f;

				if (!pushed && !unsupported)
				{
					continue;
				}
				ensure_tiles_around(physics, terrain, transform.p.x, transform.p.z, body->scale * physics_type->height);
			}
			else
			{
				bool had_tile = tile_exists_at(physics, transform.p.x, transform.p.z);
				b3Vec3 velocity = b3Body_GetLinearVelocity(body->id);
				b3Vec3 spin = b3Body_GetAngularVelocity(body->id);

				ensure_tiles_around(physics, terrain, transform.p.x, transform.p.z, body->scale * physics_type->height);
				if (buried_in_silt(body->id, terrain))
				{
					// Under the mud for good: gone from the map, the
					// physics and the paths alike
					SDL_Log("Physics: %s buried in silt at (%.1f, %.1f), removed", objects->types.types[instance->type].name,
						(double)transform.p.x, (double)transform.p.z);
					remove[body->instance] = 1;
					any_removed = true;
					continue;
				}
				if (rescue_from_ground(body->id, terrain))
				{
					// Pulled out again and again (the water presses it back
					// into a hollow): under for good, as if buried
					if (physics->clock_seconds > body->rescue_until_seconds)
					{
						body->rescues = 0;
					}
					body->rescues++;
					body->rescue_until_seconds = physics->clock_seconds + RESCUE_WINDOW_SECONDS;
					if (body->rescues >= RESCUES_BEFORE_BURIED)
					{
						SDL_Log("Physics: %s kept sinking into the ground at (%.1f, %.1f), removed", objects->types.types[instance->type].name,
							(double)transform.p.x, (double)transform.p.z);
						remove[body->instance] = 1;
						any_removed = true;
						continue;
					}
					// What got under, for finding out why
					SDL_Log("Physics: %s under the ground at (%.1f, %.1f): %.1f m/s, %.1f rad/s, ground tile %s",
						objects->types.types[instance->type].name, (double)transform.p.x, (double)transform.p.z,
						(double)b3Length(velocity), (double)b3Length(spin), had_tile ? "was there" : "was missing");
				}
			}
			apply_push(body, &push, true);
			frame_profile_count(FRAME_BODIES_PUSHED, 1);
			body->awake = true;
			continue;
		}

		// Static and anchored: only where the water moves, and only if
		// the water can do anything to them
		physics->static_body_count++;
		if ((body->kind == PHYSICS_BODY_ANCHORED || body->damage_newtons > 0.0f) && water_tile_active(water, body->x, body->z))
		{
			float pull;

			instance = &objects->instances[body->instance];
			physics_type = &physics->types[instance->type];
			if (objects->types.types[instance->type].blocks_water)
			{
				// The water stays outside: it pushes on the walls
				float force_x;
				float force_z;

				compute_wall_push(physics_type, body->scale, instance->x, instance->z, terrain, water, settings, &force_x, &force_z);
				SDL_zero(push);
				push.horizontal_drag_x = force_x;
				push.horizontal_drag_z = force_z;
				pull = SDL_sqrtf(force_x * force_x + force_z * force_z);
			}
			else
			{
				transform.p = vector(instance->x, objects_instance_base_y(instance), instance->z);
				transform.q = instance_rotation(instance);
				compute_water_push(physics, body, physics_type, terrain, water, settings, transform, false, step_seconds, &push);
				if (push.count == 0)
				{
					continue;
				}
				pull = SDL_sqrtf(push.horizontal_drag_x * push.horizontal_drag_x + push.horizontal_drag_z * push.horizontal_drag_z)
					+ SDL_max(push.buoyancy - body->mass * GRAVITY, 0.0f);
			}
			if (body->kind == PHYSICS_BODY_ANCHORED && pull > body->anchor_newtons)
			{
				uproot(physics, objects, terrain, slot, push.horizontal_drag_x, push.horizontal_drag_z);
				apply_push(body, &push, true);
			}
			else if (body->damage_newtons > 0.0f && pull > body->damage_newtons && take_damage(physics, objects, terrain, slot))
			{
				remove[body->instance] = 1;
				any_removed = true;
			}
		}
	}

	frame_profile_end(FRAME_PHYSICS_WATER, section_start);
	// Grass and such
	section_start = SDL_GetPerformanceCounter();
	physics->flood_timer += step_seconds;
	if (physics->flood_timer >= settings->flood_check_seconds)
	{
		flatten_by_water(physics, objects, water, remove, &any_removed, physics->flood_timer);
		recover_flattened(physics, objects, water, physics->flood_timer);
		physics->flood_timer = 0.0f;
	}

	frame_profile_end(FRAME_PHYSICS_FLOOD, section_start);
	{
		uint64_t world_start = SDL_GetPerformanceCounter();

		b3World_Step(physics->world, step_seconds, SDL_clamp(settings->substeps, 1, 16));
		physics->world_step_milliseconds = 1000.0 * (double)(SDL_GetPerformanceCounter() - world_start) / (double)SDL_GetPerformanceFrequency();
		frame_profile_end(FRAME_PHYSICS_WORLD, world_start);
	}
	section_start = SDL_GetPerformanceCounter();
	handle_hits(physics, objects, terrain, settings, remove, &any_removed);
	frame_profile_end(FRAME_PHYSICS_HITS, section_start);
	section_start = SDL_GetPerformanceCounter();

	// Objects follow their bodies
	{
		b3BodyEvents events = b3World_GetBodyEvents(physics->world);

		frame_profile_count(FRAME_BODY_MOVES, events.moveCount);
		for (int e = 0; e < events.moveCount; e++)
		{
			const b3BodyMoveEvent *move = &events.moveEvents[e];
			int slot = (int)(intptr_t)move->userData - 1;
			physics_body_t *body;
			object_instance_t *instance;

			if (slot < 0 || slot >= physics->body_capacity || !physics->bodies[slot].used)
			{
				continue;
			}
			body = &physics->bodies[slot];
			if (body->instance >= objects->instance_count || objects->instances[body->instance].body != slot)
			{
				continue;
			}
			instance = &objects->instances[body->instance];
			body->awake = !move->fellAsleep;
			if (!(instance->state & OBJECT_STATE_FREE))
			{
				objects_set_free(objects, body->instance, true);
			}
			// For drawing in between: where it was (its object's place, if
			// it was not moving before) and where it is now
			if (body->pose_known)
			{
				SDL_memcpy(body->pose_previous, body->pose_current, sizeof(body->pose_current));
			}
			else
			{
				body->pose_previous[0] = instance->x;
				body->pose_previous[1] = instance->y;
				body->pose_previous[2] = instance->z;
				SDL_memcpy(&body->pose_previous[3], instance->rotation, sizeof(float) * 4);
			}
			body->pose_current[0] = move->transform.p.x;
			body->pose_current[1] = move->transform.p.y;
			body->pose_current[2] = move->transform.p.z;
			body->pose_current[3] = move->transform.q.v.x;
			body->pose_current[4] = move->transform.q.v.y;
			body->pose_current[5] = move->transform.q.v.z;
			body->pose_current[6] = move->transform.q.s;
			body->pose_known = !move->fellAsleep;
			if (!move->fellAsleep)
			{
				remember_moved(physics, slot);
			}
			instance->x = move->transform.p.x;
			instance->y = move->transform.p.y;
			instance->z = move->transform.p.z;
			body->x = instance->x;
			body->z = instance->z;
			// (into its new ground tile's list, if it went to another)
			if (body->list_bucket >= 0 && body_bucket_of(physics, body) != body->list_bucket)
			{
				body_list_remove(physics, slot);
				body_list_insert(physics, slot);
			}
			instance->rotation[0] = move->transform.q.v.x;
			instance->rotation[1] = move->transform.q.v.y;
			instance->rotation[2] = move->transform.q.v.z;
			instance->rotation[3] = move->transform.q.s;
			// Off the map: gone (the water carried it away)
			if (instance->x < 0.0f || instance->z < 0.0f || instance->x >= map_meters || instance->z >= map_meters
				|| instance->y < physics->minimum_height - FALL_LIMIT_METERS)
			{
				SDL_Log("Physics: %s left the map at (%.1f, %.1f, %.1f)", objects->types.types[instance->type].name, (double)instance->x,
					(double)instance->y, (double)instance->z);
				remove[body->instance] = 1;
				any_removed = true;
			}
		}
		// Bodies moving: only the near map's moving layer follows (in the
		// far one a log is a pixel of an already soft shadow)
		if (events.moveCount > 0)
		{
			objects->moving_shadows = true;
		}
	}
	frame_profile_end(FRAME_PHYSICS_FOLLOW, section_start);
	section_start = SDL_GetPerformanceCounter();
	wake_neighbours(physics, objects, terrain);
	physics->awake_body_count = b3World_GetAwakeBodyCount(physics->world);
	frame_profile_count(FRAME_AWAKE_BODIES, physics->awake_body_count);
	frame_profile_end(FRAME_PHYSICS_WAKE, section_start);
	section_start = SDL_GetPerformanceCounter();

	if (any_removed)
	{
		int removed;

		if (objects->instance_count > physics->remove_capacity)
		{
			// Debris was added meanwhile: it stays
			uint8_t *grown = SDL_realloc(remove, (size_t)objects->instance_count);

			if (grown == NULL)
			{
				return;
			}
			remove = grown;
			SDL_memset(remove + physics->remove_capacity, 0, (size_t)(objects->instance_count - physics->remove_capacity));
			physics->remove = remove;
			physics->remove_capacity = objects->instance_count;
		}
		else if (objects->instance_count > count_before)
		{
			SDL_memset(remove + count_before, 0, (size_t)(objects->instance_count - count_before));
		}
		// (gone in the game: marked dead where they are, not taken out of
		// the list (a sort of the whole map for a pebble in the silt); their
		// bodies go at once)
		removed = objects_kill_flagged(objects, remove);
		follow_changes(physics, objects, terrain);
		// Clean for the next step
		SDL_memset(remove, 0, (size_t)physics->remove_capacity);

		physics->removed_total += removed;
		frame_profile_count(FRAME_OBJECTS_REMOVED, removed);
	}
	frame_profile_end(FRAME_PHYSICS_REMOVE, section_start);
	physics->step_milliseconds = 1000.0 * (double)(SDL_GetPerformanceCounter() - start_counter) / (double)SDL_GetPerformanceFrequency();
}

// ---------------------------------------------------------------------------
// The walker's ground

#define WALKER_FOOT_RADIUS_METERS 0.2f  // the rays down: its middle and four round it this far out
#define WALKER_GROUND_LEAST_NORMAL_Y 0.6f // a top this level or more is something to stand on
#define WALKER_GROUND_REACH_METERS 30.0f  // how far down the rays look

bool physics_walker_ground(physics_t *physics, float x, float z, float feet_height, float step_meters, float *height_out)
{
	static const float around[5][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f } };
	bool found = false;
	float highest = -1e30f;

	if (!physics->created)
	{
		return false;
	}
	for (int k = 0; k < 5; k++)
	{
		b3Vec3 origin = vector(x + around[k][0] * WALKER_FOOT_RADIUS_METERS, feet_height + step_meters, z + around[k][1] * WALKER_FOOT_RADIUS_METERS);
		b3RayResult result = b3World_CastRayClosest(physics->world, origin, vector(0.0f, -WALKER_GROUND_REACH_METERS, 0.0f), b3DefaultQueryFilter());
		int slot;

		if (!result.hit || result.normal.y < WALKER_GROUND_LEAST_NORMAL_Y)
		{
			continue;
		}
		// (the ground's own tiles are the terrain: the caller has that)
		slot = slot_of_body(b3Shape_GetBody(result.shapeId));
		if (slot < 0 || slot >= physics->body_capacity || !physics->bodies[slot].used)
		{
			continue;
		}
		highest = SDL_max(highest, (float)result.point.y);
		found = true;
	}
	*height_out = highest;
	return found;
}

// ---------------------------------------------------------------------------
// Queries for the projectiles

bool physics_cast_segment(physics_t *physics, const float from[3], const float to[3], physics_cast_t *hit_out)
{
	b3RayResult result;
	b3Vec3 origin;
	b3Vec3 translation;

	if (!physics->created)
	{
		return false;
	}
	origin = vector(from[0], from[1], from[2]);
	translation = vector(to[0] - from[0], to[1] - from[1], to[2] - from[2]);
	result = b3World_CastRayClosest(physics->world, origin, translation, b3DefaultQueryFilter());
	if (!result.hit)
	{
		return false;
	}
	hit_out->point[0] = (float)result.point.x;
	hit_out->point[1] = (float)result.point.y;
	hit_out->point[2] = (float)result.point.z;
	hit_out->normal[0] = result.normal.x;
	hit_out->normal[1] = result.normal.y;
	hit_out->normal[2] = result.normal.z;
	hit_out->fraction = result.fraction;
	hit_out->body = -1;
	hit_out->instance = -1;
	{
		b3BodyId body_id = b3Shape_GetBody(result.shapeId);
		int slot = slot_of_body(body_id);

		if (slot >= 0 && slot < physics->body_capacity && physics->bodies[slot].used)
		{
			hit_out->body = slot;
			hit_out->instance = physics->bodies[slot].instance;
		}
	}
	return true;
}

void physics_set_body_mass(physics_t *physics, int body, float mass_kilograms)
{
	physics_body_t *target;
	b3MassData mass_data;
	float ratio;

	if (body < 0 || body >= physics->body_capacity || mass_kilograms <= 0.0f)
	{
		return;
	}
	target = &physics->bodies[body];
	if (!target->used || target->kind != PHYSICS_BODY_DYNAMIC || target->mass <= 0.0f)
	{
		return;
	}
	mass_data = b3Body_GetMassData(target->id);
	ratio = mass_kilograms / SDL_max(mass_data.mass, 1e-9f);
	mass_data.mass = mass_kilograms;
	mass_data.inertia.cx = b3MulSV(ratio, mass_data.inertia.cx);
	mass_data.inertia.cy = b3MulSV(ratio, mass_data.inertia.cy);
	mass_data.inertia.cz = b3MulSV(ratio, mass_data.inertia.cz);
	b3Body_SetMassData(target->id, mass_data);
	target->displaced_volume *= mass_kilograms / target->mass;
	target->mass = mass_kilograms;
}

void physics_push_body(physics_t *physics, int body, const float impulse[3], const float point[3])
{
	physics_body_t *target;

	if (!physics->created || body < 0 || body >= physics->body_capacity)
	{
		return;
	}
	target = &physics->bodies[body];
	if (!target->used || target->kind != PHYSICS_BODY_DYNAMIC || target->in_ice)
	{
		return;
	}
	b3Body_ApplyLinearImpulse(target->id, vector(impulse[0], impulse[1], impulse[2]), vector(point[0], point[1], point[2]), true);
}
