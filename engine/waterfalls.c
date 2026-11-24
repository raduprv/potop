// Waterfalls. See waterfalls.h.

#include "waterfalls.h"
#include "parallel.h"

#include "profile.h"
#include <SDL3/SDL.h>

// The water's surface drops at least this many cells' widths from a cell to
// the next downstream: steep (a fall); the way down is followed while it
// still drops this many
static const float STEEP_RISE = 1.5f;
static const float SLOPING_RISE = 0.3f;
// Water at least this deep counts; falls at least this high
static const float WET_METERS = 0.01f;
static const float LOWEST_FALL_METERS = 2.0f;
// ...and carrying at least this much (m^3/s): no trickles
static const float LEAST_FLOW = 0.005f;
// Lips within this of each other (and the same way) make one segment
static const float SEGMENT_METERS = 2.0f;
// Played within this of the player, stopped beyond the second
static const float PLAY_METERS = 250.0f;
static const float STOP_METERS = 320.0f;
// An active map's: looked for every this often, this far around
static const double SEARCH_SECONDS = 3.0;
static const float SEARCH_METERS = 400.0f;
static const int MOST_STEPS_DOWN = 4000;

static const char *effect_names[WATERFALLS_EFFECTS] = { "waterfall_curtain", "waterfall_mist", "waterfall_splash" };

static const int step_x[4] = { -1, 1, 0, 0 };
static const int step_y[4] = { 0, 0, -1, 1 };

typedef struct
{
	int32_t x;
	int32_t y;
	int direction;
	float level;                   // meters
	float base_x;                  // cells
	float base_y;
	float base_level;
	float depth;
	float velocity_x;
	float velocity_z;
} lip_t;

typedef struct
{
	lip_t *lips;
	int count;
	int capacity;
} lips_t;

void waterfalls_create(waterfalls_t *waterfalls)
{
	SDL_zerop(waterfalls);
	for (int k = 0; k < WATERFALLS_EFFECTS; k++)
	{
		waterfalls->effects[k] = -1;
	}
}

static void stop_all(waterfalls_t *waterfalls, particles_t *particles)
{
	for (int i = 0; i < waterfalls->count * WATERFALLS_EFFECTS && waterfalls->emitters != NULL; i++)
	{
		if (waterfalls->emitters[i] != 0)
		{
			particles_stop(particles, waterfalls->emitters[i]);
			waterfalls->emitters[i] = 0;
		}
	}
}

void waterfalls_destroy(waterfalls_t *waterfalls, particles_t *particles)
{
	if (particles != NULL)
	{
		stop_all(waterfalls, particles);
	}
	SDL_free(waterfalls->segments);
	SDL_free(waterfalls->emitters);
	SDL_zerop(waterfalls);
}

// Room for count segments (their emitters cleared)
static bool make_room(waterfalls_t *waterfalls, int count)
{
	if (count > waterfalls->capacity)
	{
		int capacity = SDL_max(count, SDL_max(16, waterfalls->capacity * 2));
		waterfall_segment_t *segments = SDL_realloc(waterfalls->segments, sizeof(waterfall_segment_t) * (size_t)capacity);
		uint32_t *emitters;

		if (segments == NULL)
		{
			return false;
		}
		waterfalls->segments = segments;
		emitters = SDL_realloc(waterfalls->emitters, sizeof(uint32_t) * WATERFALLS_EFFECTS * (size_t)capacity);
		if (emitters == NULL)
		{
			return false;
		}
		waterfalls->emitters = emitters;
		waterfalls->capacity = capacity;
	}
	if (waterfalls->emitters != NULL)
	{
		SDL_memset(waterfalls->emitters, 0, sizeof(uint32_t) * WATERFALLS_EFFECTS * (size_t)waterfalls->capacity);
	}
	return true;
}

// The water's surface at a cell (meters), and whether it is wet
static float level_at(const terrain_t *terrain, const water_t *water, int32_t x, int32_t y, bool *wet_out)
{
	size_t index = (size_t)y * (size_t)terrain->size + (size_t)x;
	float depth = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;

	*wet_out = depth >= WET_METERS;
	return (float)terrain->heights[index] * TERRAIN_METERS_PER_HEIGHT_UNIT + depth;
}

// The wet neighbor the surface drops to most (-1: none lower), and how much
static int steepest_down(const terrain_t *terrain, const water_t *water, int32_t x, int32_t y, float level, float *drop_out)
{
	int best = -1;
	float best_drop = 0.0f;

	for (int k = 0; k < 4; k++)
	{
		int32_t next_x = x + step_x[k];
		int32_t next_y = y + step_y[k];
		bool wet;
		float drop;

		if (next_x < 0 || next_y < 0 || next_x >= terrain->size || next_y >= terrain->size)
		{
			continue;
		}
		drop = level - level_at(terrain, water, next_x, next_y, &wet);
		if (wet && drop > best_drop)
		{
			best = k;
			best_drop = drop;
		}
	}
	*drop_out = best_drop;
	return best;
}

// Lips further down another fall's way than this are on its face (a ledge
// it pours over), not falls of their own; its way is marked this many
// meters wide on each side
static const float ON_THE_FACE_METERS = 1.0f;
static const float MARKED_METERS = 1.5f;

static void mark_cell(uint8_t *marks, int32_t size, int32_t x, int32_t y)
{
	if (x >= 0 && y >= 0 && x < size && y < size)
	{
		size_t index = (size_t)y * (size_t)size + (size_t)x;

		marks[index >> 3] |= (uint8_t)(1u << (index & 7u));
	}
}

static bool marked(const uint8_t *marks, int32_t size, int32_t x, int32_t y)
{
	size_t index = (size_t)y * (size_t)size + (size_t)x;

	return (marks[index >> 3] & (1u << (index & 7u))) != 0;
}

// A kept lip's way down marked (from where it is a meter below the lip on)
static void mark_way_down(const terrain_t *terrain, const water_t *water, const lip_t *lip, uint8_t *marks)
{
	float sloping = SLOPING_RISE * terrain->cell_size_meters;
	int32_t at_x = lip->x;
	int32_t at_y = lip->y;
	float at_level = lip->level;
	int32_t reach = SDL_max(1, (int32_t)SDL_lroundf(MARKED_METERS / terrain->cell_size_meters));

	for (int steps = 0; steps < MOST_STEPS_DOWN; steps++)
	{
		float next_drop;
		int next = steepest_down(terrain, water, at_x, at_y, at_level, &next_drop);
		bool next_wet;

		if (next < 0 || next_drop < sloping)
		{
			break;
		}
		at_x += step_x[next];
		at_y += step_y[next];
		at_level = level_at(terrain, water, at_x, at_y, &next_wet);
		if (lip->level - at_level >= ON_THE_FACE_METERS)
		{
			for (int32_t dy = -reach; dy <= reach; dy++)
			{
				for (int32_t dx = -reach; dx <= reach; dx++)
				{
					mark_cell(marks, terrain->size, at_x + dx, at_y + dy);
				}
			}
		}
	}
}

static int compare_lips_by_level(const void *a, const void *b)
{
	const lip_t *first = a;
	const lip_t *second = b;

	return first->level > second->level ? -1 : (first->level < second->level ? 1 : 0);
}

static void add_lip(lips_t *lips, const lip_t *lip)
{
	if (lips->count == lips->capacity)
	{
		int capacity = SDL_max(256, lips->capacity * 2);
		lip_t *grown = SDL_realloc(lips->lips, sizeof(lip_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		lips->lips = grown;
		lips->capacity = capacity;
	}
	lips->lips[lips->count++] = *lip;
}

// The lips in one tile
static void search_tile(const terrain_t *terrain, const water_t *water, int32_t tile, lips_t *lips)
{
	float cell = terrain->cell_size_meters;
	float steep = STEEP_RISE * cell;
	float sloping = SLOPING_RISE * cell;
	int32_t start_x = (tile % water->tiles_per_side) * WATER_TILE_SIZE;
	int32_t start_y = (tile / water->tiles_per_side) * WATER_TILE_SIZE;

	for (int32_t y = start_y; y < start_y + WATER_TILE_SIZE; y++)
	{
		for (int32_t x = start_x; x < start_x + WATER_TILE_SIZE; x++)
		{
			bool wet;
			float level = level_at(terrain, water, x, y, &wet);
			float drop;
			int direction;
			int32_t back_x;
			int32_t back_y;
			lip_t lip;
			float depth;
			int32_t at_x = x;
			int32_t at_y = y;
			float at_level = level;

			if (!wet)
			{
				continue;
			}
			direction = steepest_down(terrain, water, x, y, level, &drop);
			if (direction < 0 || drop < steep)
			{
				continue;
			}
			// A lip: the water behind it did not fall into it (else it is
			// on the way down already)
			back_x = x - step_x[direction];
			back_y = y - step_y[direction];
			if (back_x >= 0 && back_y >= 0 && back_x < terrain->size && back_y < terrain->size)
			{
				bool back_wet;
				float back_level = level_at(terrain, water, back_x, back_y, &back_wet);

				if (back_wet && back_level - level >= steep)
				{
					continue;
				}
			}
			// Down to where it lands: the steepest way, while it slopes
			for (int steps = 0; steps < MOST_STEPS_DOWN; steps++)
			{
				float next_drop;
				int next = steepest_down(terrain, water, at_x, at_y, at_level, &next_drop);
				bool next_wet;

				if (next < 0 || next_drop < sloping)
				{
					break;
				}
				at_x += step_x[next];
				at_y += step_y[next];
				at_level = level_at(terrain, water, at_x, at_y, &next_wet);
			}
			if (level - at_level < LOWEST_FALL_METERS)
			{
				continue;
			}
			depth = (float)water->depth[(size_t)y * (size_t)terrain->size + (size_t)x] * TERRAIN_METERS_PER_HEIGHT_UNIT;
			SDL_zero(lip);
			lip.x = x;
			lip.y = y;
			lip.direction = direction;
			lip.level = level;
			lip.base_x = (float)at_x;
			lip.base_y = (float)at_y;
			lip.base_level = at_level;
			lip.depth = depth;
			{
				float sampled_depth;

				water_sample(water, (float)x * cell, (float)y * cell, &sampled_depth, &lip.velocity_x, &lip.velocity_z);
			}
#pragma omp critical(waterfall_lips)
			add_lip(lips, &lip);
		}
	}
}

// The segments' squares, in cells (for the sort below; set before it)
static int32_t sort_bucket_cells = 1;

// By the way the water runs, then the square, then the cell: the lips of a
// segment one after the other
static int compare_lips(const void *a, const void *b)
{
	const lip_t *first = a;
	const lip_t *second = b;
	int32_t first_key[5] = { first->direction, first->y / sort_bucket_cells, first->x / sort_bucket_cells, first->y, first->x };
	int32_t second_key[5] = { second->direction, second->y / sort_bucket_cells, second->x / sort_bucket_cells, second->y, second->x };

	for (int k = 0; k < 5; k++)
	{
		if (first_key[k] != second_key[k])
		{
			return first_key[k] < second_key[k] ? -1 : 1;
		}
	}
	return 0;
}

static bool same_bucket(const lip_t *a, const lip_t *b, int32_t bucket_cells)
{
	return a->direction == b->direction && a->x / bucket_cells == b->x / bucket_cells && a->y / bucket_cells == b->y / bucket_cells;
}

// The segments again much as they were (the same count, each lip within a
// meter): the emitters playing are kept
static bool much_the_same(const waterfalls_t *waterfalls, const waterfall_segment_t *segments, int count)
{
	if (count != waterfalls->count)
	{
		return false;
	}
	for (int i = 0; i < count; i++)
	{
		float away_x = segments[i].lip[0] - waterfalls->segments[i].lip[0];
		float away_z = segments[i].lip[2] - waterfalls->segments[i].lip[2];

		if (away_x * away_x + away_z * away_z > 1.0f)
		{
			return false;
		}
	}
	return true;
}

bool waterfalls_search(waterfalls_t *waterfalls, const terrain_t *terrain, const water_t *water, float x, float z, float radius_meters)
{
	float cell = terrain->cell_size_meters;
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;
	float tile_meters = (float)WATER_TILE_SIZE * cell;
	int32_t bucket_cells = SDL_max(1, (int32_t)SDL_lroundf(SEGMENT_METERS / cell));
	lips_t lips;
	waterfall_segment_t *segments;
	int segment_count = 0;
	uint64_t start = SDL_GetPerformanceCounter();
	bool changed = false;

	if (terrain->size != water->size || water->depth == NULL)
	{
		return false;
	}
	SDL_zero(lips);
	// The tiles to look in: wet, in reach, and (where the GPU counts them)
	// with cells whose surface drops steeply, the only places a lip can be
	// (a flood of still water: none, nothing looked at cell by cell)
	{
		int32_t *search = SDL_malloc(sizeof(int32_t) * (size_t)SDL_max(tile_count, 1));
		int32_t search_count = 0;

		for (int32_t tile = 0; tile < tile_count && search != NULL; tile++)
		{
			float middle_x = ((float)(tile % water->tiles_per_side) + 0.5f) * tile_meters;
			float middle_z = ((float)(tile / water->tiles_per_side) + 0.5f) * tile_meters;

			if (water->tile_state[tile] == WATER_TILE_DRY && water->tile_max_depth[tile] <= 0)
			{
				continue;
			}
			if (water->steep_cells_known && water->tile_steep_cells[tile] == 0)
			{
				continue;
			}
			if (radius_meters >= 0.0f && (SDL_fabsf(middle_x - x) > radius_meters + tile_meters || SDL_fabsf(middle_z - z) > radius_meters + tile_meters))
			{
				continue;
			}
			search[search_count++] = tile;
		}
		// (a few on this thread alone: waking the others costs more)
#pragma omp parallel for schedule(dynamic, 1) num_threads(parallel_threads_for_tiles(search_count))
		for (int32_t k = 0; k < search_count; k++)
		{
			search_tile(terrain, water, search[k], &lips);
		}
		SDL_free(search);
	}
	// Highest first: a lip on the face of a fall above it (a ledge the
	// water pours over on its way down) is dropped
	if (lips.count > 0)
	{
		size_t mark_bytes = ((size_t)terrain->size * (size_t)terrain->size + 7) / 8;
		uint8_t *marks = SDL_calloc(mark_bytes, 1);
		int kept = 0;

		if (marks != NULL)
		{
			SDL_qsort(lips.lips, (size_t)lips.count, sizeof(lip_t), compare_lips_by_level);
			for (int i = 0; i < lips.count; i++)
			{
				if (marked(marks, terrain->size, lips.lips[i].x, lips.lips[i].y))
				{
					continue;
				}
				mark_way_down(terrain, water, &lips.lips[i], marks);
				lips.lips[kept++] = lips.lips[i];
			}
			lips.count = kept;
			SDL_free(marks);
		}
	}
	sort_bucket_cells = bucket_cells;
	SDL_qsort(lips.lips, (size_t)lips.count, sizeof(lip_t), compare_lips);

	// Each square's lips (one after the other now) into a segment
	segments = SDL_malloc(sizeof(waterfall_segment_t) * (size_t)SDL_max(lips.count, 1));
	{
		int next = 0;

		for (int i = 0; i < lips.count && segments != NULL; i = next)
		{
			waterfall_segment_t segment;
			double sum_x = 0.0;
			double sum_y = 0.0;
			double sum_level = 0.0;
			double sum_base_x = 0.0;
			double sum_base_y = 0.0;
			double sum_base_level = 0.0;
			double sum_velocity_x = 0.0;
			double sum_velocity_z = 0.0;
			double flow = 0.0;
			int count = 0;

			for (next = i; next < lips.count && same_bucket(&lips.lips[i], &lips.lips[next], bucket_cells); next++)
			{
				const lip_t *lip = &lips.lips[next];

				sum_x += lip->x;
				sum_y += lip->y;
				sum_level += lip->level;
				sum_base_x += lip->base_x;
				sum_base_y += lip->base_y;
				sum_base_level += lip->base_level;
				sum_velocity_x += lip->velocity_x;
				sum_velocity_z += lip->velocity_z;
				flow += (double)lip->depth * SDL_sqrt((double)(lip->velocity_x * lip->velocity_x + lip->velocity_z * lip->velocity_z)) * cell;
				count++;
			}
			if (flow < LEAST_FLOW)
			{
				continue;
			}
			SDL_zero(segment);
			segment.lip[0] = (float)(sum_x / count) * cell;
			segment.lip[1] = (float)(sum_level / count);
			segment.lip[2] = (float)(sum_y / count) * cell;
			segment.base[0] = (float)(sum_base_x / count) * cell;
			segment.base[1] = (float)(sum_base_level / count);
			segment.base[2] = (float)(sum_base_y / count) * cell;
			segment.width_meters = SDL_min((float)count * cell, SEGMENT_METERS * 1.5f);
			segment.fall_meters = segment.lip[1] - segment.base[1];
			segment.flow = (float)flow;
			{
				float velocity_x = (float)(sum_velocity_x / count);
				float velocity_z = (float)(sum_velocity_z / count);
				float speed = SDL_sqrtf(velocity_x * velocity_x + velocity_z * velocity_z);

				segment.speed = speed;
				if (speed > 0.05f)
				{
					segment.direction[0] = velocity_x / speed;
					segment.direction[1] = velocity_z / speed;
				}
				else
				{
					segment.direction[0] = (float)step_x[lips.lips[i].direction];
					segment.direction[1] = (float)step_y[lips.lips[i].direction];
				}
			}
			segments[segment_count++] = segment;
		}
	}
	if (segments != NULL && !much_the_same(waterfalls, segments, segment_count) && make_room(waterfalls, segment_count))
	{
		// (the emitters cleared: the caller stops the old ones)
		SDL_memcpy(waterfalls->segments, segments, sizeof(waterfall_segment_t) * (size_t)segment_count);
		waterfalls->count = segment_count;
		changed = true;
		SDL_Log("Waterfalls: %d segments (of %d lips) in %.0f ms", segment_count, lips.count,
			1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency());
	}
	SDL_free(segments);
	SDL_free(lips.lips);
	return changed;
}

void waterfalls_set(waterfalls_t *waterfalls, particles_t *particles, const waterfall_segment_t *segments, int count, bool fixed)
{
	stop_all(waterfalls, particles);
	waterfalls->count = 0;
	waterfalls->fixed = fixed;
	waterfalls->next_search_seconds = 0.0;
	if (count > 0 && make_room(waterfalls, count))
	{
		SDL_memcpy(waterfalls->segments, segments, sizeof(waterfall_segment_t) * (size_t)count);
		waterfalls->count = count;
	}
}

// The three effects of a segment playing
static void start_segment(waterfalls_t *waterfalls, particles_t *particles, int index)
{
	const waterfall_segment_t *segment = &waterfalls->segments[index];
	float yaw = SDL_atan2f(segment->direction[0], segment->direction[1]);
	float rotation[4] = { 0.0f, SDL_sinf(0.5f * yaw), 0.0f, SDL_cosf(0.5f * yaw) };
	float width = SDL_max(segment->width_meters, 0.25f);
	// The water a meter of the lip carries, against a tenth of a cubic meter
	// a second: how thick its curtain and splash are
	float carried = SDL_clamp(segment->flow / width / 0.1f, 0.6f, 4.0f);
	float fall_seconds = SDL_sqrtf(2.0f * SDL_max(segment->fall_meters, 0.0f) / 9.81f);
	// (pushed out gently: the curtain follows the face; the water's own
	// speed at a lip, thin and fast, would throw it far out)
	float push_speed = SDL_clamp(segment->speed * 0.3f, 0.3f, 1.0f);

	for (int k = 0; k < WATERFALLS_EFFECTS; k++)
	{
		float position[3];
		float extents[3];
		particles_emitter_t *emitter;
		uint32_t id;

		// (playing still: nothing to do; stopped by someone else, the Stop
		// all button, a reload: again)
		if (waterfalls->effects[k] < 0 || (waterfalls->emitters[index * WATERFALLS_EFFECTS + k] != 0
			&& particles_find_emitter(particles, waterfalls->emitters[index * WATERFALLS_EFFECTS + k]) != NULL))
		{
			continue;
		}
		if (k == 0)
		{
			position[0] = segment->lip[0] + segment->direction[0] * 0.1f;
			position[1] = segment->lip[1] + 0.1f;
			position[2] = segment->lip[2] + segment->direction[1] * 0.1f;
			// (a little wider than the segment: the curtains of segments
			// side by side close up)
			extents[0] = 0.5f * width + 0.4f;
			extents[1] = 0.05f;
			extents[2] = 0.1f;
		}
		else
		{
			float spread = 0.05f * segment->fall_meters;

			position[0] = segment->base[0];
			position[1] = segment->base[1] + (k == 1 ? 0.5f : 0.1f);
			position[2] = segment->base[2];
			extents[0] = 0.5f * width + spread;
			extents[1] = k == 1 ? 0.3f : 0.1f;
			extents[2] = 0.5f + spread;
		}
		id = particles_play_on(particles, waterfalls->effects[k], position, rotation, extents);
		emitter = particles_find_emitter(particles, id);
		if (emitter == NULL)
		{
			continue;
		}
		if (k == 0)
		{
			// As long as sliding down the face takes (longer than falling),
			// and a little to spread below
			emitter->rate_scale = width * carried;
			emitter->life_scale = 1.4f * fall_seconds + 0.5f;
			emitter->push[0] = segment->direction[0] * push_speed;
			emitter->push[2] = segment->direction[1] * push_speed;
		}
		else if (k == 1)
		{
			emitter->rate_scale = width * SDL_clamp(segment->fall_meters / 20.0f, 0.2f, 3.0f);
		}
		else
		{
			emitter->rate_scale = width * carried;
			emitter->push[0] = segment->direction[0];
			emitter->push[2] = segment->direction[1];
		}
		waterfalls->emitters[index * WATERFALLS_EFFECTS + k] = id;
	}
}

static void stop_segment(waterfalls_t *waterfalls, particles_t *particles, int index)
{
	for (int k = 0; k < WATERFALLS_EFFECTS; k++)
	{
		uint32_t *id = &waterfalls->emitters[index * WATERFALLS_EFFECTS + k];

		if (*id != 0)
		{
			particles_stop(particles, *id);
			*id = 0;
		}
	}
}

void waterfalls_update(waterfalls_t *waterfalls, particles_t *particles, const terrain_t *terrain, const water_t *water, float x, float z,
	double now_seconds, bool frozen)
{
	bool any_effect = false;

	if (!particles->created)
	{
		return;
	}
	for (int k = 0; k < WATERFALLS_EFFECTS; k++)
	{
		waterfalls->effects[k] = particles_find_effect(particles, effect_names[k]);
		any_effect = any_effect || waterfalls->effects[k] >= 0;
	}
	if (!any_effect)
	{
		if (!waterfalls->warned)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Waterfalls: particles.ini has none of waterfall_curtain, waterfall_mist, waterfall_splash");
			waterfalls->warned = true;
		}
		return;
	}
	// An active map's, looked for again now and then; a new list: the
	// emitters of the old one stop
	if (!waterfalls->fixed && !water->baked && now_seconds >= waterfalls->next_search_seconds)
	{
		int playing_count = waterfalls->count * WATERFALLS_EFFECTS;
		uint32_t *playing = playing_count > 0 ? SDL_malloc(sizeof(uint32_t) * (size_t)playing_count) : NULL;

		waterfalls->next_search_seconds = now_seconds + SEARCH_SECONDS;
		frame_profile_count(FRAME_WATERFALL_SEARCHES, 1);
		if (playing != NULL)
		{
			SDL_memcpy(playing, waterfalls->emitters, sizeof(uint32_t) * (size_t)playing_count);
		}
		if (waterfalls_search(waterfalls, terrain, water, x, z, SEARCH_METERS))
		{
			for (int i = 0; i < playing_count && playing != NULL; i++)
			{
				if (playing[i] != 0)
				{
					particles_stop(particles, playing[i]);
				}
			}
		}
		SDL_free(playing);
	}
	for (int i = 0; i < waterfalls->count; i++)
	{
		const waterfall_segment_t *segment = &waterfalls->segments[i];
		float away_x = segment->lip[0] - x;
		float away_z = segment->lip[2] - z;
		float away = away_x * away_x + away_z * away_z;

		if (!frozen && away < PLAY_METERS * PLAY_METERS)
		{
			start_segment(waterfalls, particles, i);
		}
		else if (frozen || away > STOP_METERS * STOP_METERS)
		{
			stop_segment(waterfalls, particles, i);
		}
	}
}
