// Areas on the map. See areas.h.

#include "areas.h"

#include <SDL3/SDL.h>

// An actor counts as where its middle is: this high over the ground under it
static const float ACTOR_MIDDLE_METERS = 1.0f;

void areas_clear(areas_t *areas)
{
	uint32_t random = areas->id_random;

	SDL_zerop(areas);
	areas->id_random = random;
}

int areas_find(const areas_t *areas, uint32_t id)
{
	if (id == 0)
	{
		return -1;
	}
	for (int i = 0; i < areas->count; i++)
	{
		if (areas->areas[i].id == id)
		{
			return i;
		}
	}
	return -1;
}

// A random id no area has (xorshift: never 0)
static uint32_t new_id(areas_t *areas)
{
	if (areas->id_random == 0)
	{
		areas->id_random = (uint32_t)SDL_GetPerformanceCounter() | 1u;
	}
	for (;;)
	{
		uint32_t value = areas->id_random;

		value ^= value << 13;
		value ^= value >> 17;
		value ^= value << 5;
		areas->id_random = value;
		if (areas_find(areas, value) < 0)
		{
			return value;
		}
	}
}

uint32_t areas_add(areas_t *areas, const char *name, float x, float z, float width_meters, float depth_meters, float yaw_radians,
	uint8_t triggers)
{
	area_t *area;

	if (areas->count >= AREAS_MAX)
	{
		return 0;
	}
	area = &areas->areas[areas->count++];
	SDL_zerop(area);
	area->id = new_id(areas);
	SDL_strlcpy(area->name, name != NULL ? name : "", sizeof(area->name));
	area->x = x;
	area->z = z;
	area->half_width_meters = SDL_max(0.5f * width_meters, 0.05f);
	area->half_depth_meters = SDL_max(0.5f * depth_meters, 0.05f);
	area->yaw_radians = yaw_radians;
	area->triggers = triggers;
	return area->id;
}

bool areas_set_id(areas_t *areas, int index, uint32_t id)
{
	int other = areas_find(areas, id);

	if (index < 0 || index >= areas->count || id == 0 || (other >= 0 && other != index))
	{
		return false;
	}
	areas->areas[index].id = id;
	return true;
}

bool areas_remove(areas_t *areas, uint32_t id)
{
	int index = areas_find(areas, id);
	int kept = 0;

	if (index < 0)
	{
		return false;
	}
	SDL_memmove(&areas->areas[index], &areas->areas[index + 1], sizeof(area_t) * (size_t)(areas->count - index - 1));
	areas->count--;
	// Those in it are in it no more (without a word)
	for (int i = 0; i < areas->inside_count; i++)
	{
		if (areas->inside[i].area_id != id)
		{
			areas->inside[kept++] = areas->inside[i];
		}
	}
	areas->inside_count = kept;
	return true;
}

// A point in the area's own frame: along its width, along its depth
static void local_point(const area_t *area, float x, float z, float *along_width, float *along_depth)
{
	float c = SDL_cosf(area->yaw_radians);
	float s = SDL_sinf(area->yaw_radians);
	float dx = x - area->x;
	float dz = z - area->z;

	*along_width = dx * c - dz * s;
	*along_depth = dx * s + dz * c;
}

bool areas_contains(const area_t *area, float x, float y, float z)
{
	float along_width;
	float along_depth;

	local_point(area, x, z, &along_width, &along_depth);
	if (SDL_fabsf(along_width) > area->half_width_meters || SDL_fabsf(along_depth) > area->half_depth_meters)
	{
		return false;
	}
	return area->top_meters <= area->bottom_meters || (y >= area->bottom_meters && y <= area->top_meters);
}

void areas_corners(const area_t *area, float corners_out[4][2])
{
	static const float signs[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
	float c = SDL_cosf(area->yaw_radians);
	float s = SDL_sinf(area->yaw_radians);

	for (int k = 0; k < 4; k++)
	{
		float along_width = signs[k][0] * area->half_width_meters;
		float along_depth = signs[k][1] * area->half_depth_meters;

		// (local_point turned back)
		corners_out[k][0] = area->x + along_width * c + along_depth * s;
		corners_out[k][1] = area->z - along_width * s + along_depth * c;
	}
}

int areas_at(const areas_t *areas, float x, float z)
{
	for (int i = areas->count - 1; i >= 0; i--)
	{
		area_t flat = areas->areas[i];

		flat.top_meters = flat.bottom_meters;
		if (areas_contains(&flat, x, 0.0f, z))
		{
			return i;
		}
	}
	return -1;
}

static void tell(areas_t *areas, area_event_type_t type, uint32_t area_id, bool player, uint32_t actor_id)
{
	area_event_t *event;

	if (areas->event_count >= AREA_EVENTS_MAX)
	{
		// (the oldest dropped)
		areas->event_first = (areas->event_first + 1) % AREA_EVENTS_MAX;
		areas->event_count--;
	}
	event = &areas->events[(areas->event_first + areas->event_count) % AREA_EVENTS_MAX];
	event->type = type;
	event->area_id = area_id;
	event->player = player;
	event->actor_id = actor_id;
	areas->event_count++;
}

static int compare_occupants(const void *a, const void *b)
{
	const area_occupant_t *first = a;
	const area_occupant_t *second = b;

	if (first->area_id != second->area_id)
	{
		return first->area_id < second->area_id ? -1 : 1;
	}
	if (first->actor_id != second->actor_id)
	{
		return first->actor_id < second->actor_id ? -1 : 1;
	}
	return 0;
}

static bool area_tells(const areas_t *areas, uint32_t area_id, uint8_t trigger)
{
	int index = areas_find(areas, area_id);

	return index >= 0 && (areas->areas[index].triggers & trigger) != 0;
}

static bool actor_living(const actors_t *actors, uint32_t id)
{
	return actors_find(actors, id) >= 0 && !actors_is_dead(actors, id);
}

void areas_update(areas_t *areas, const actors_t *actors, const float player[3])
{
	int found_count = 0;
	int old_index = 0;
	int new_index = 0;

	// The player
	for (int i = 0; i < areas->count && player != NULL; i++)
	{
		area_t *area = &areas->areas[i];
		bool inside = areas_contains(area, player[0], player[1], player[2]);

		if (inside != area->player_inside)
		{
			area->player_inside = inside;
			if (area->triggers & AREA_TRIGGER_PLAYER)
			{
				tell(areas, inside ? AREA_EVENT_ENTER : AREA_EVENT_EXIT, area->id, true, 0);
			}
		}
	}
	if (areas->count == 0 && areas->inside_count == 0)
	{
		return;
	}

	// The actors: who is where now
	for (int a = 0; a < actors->count; a++)
	{
		const actor_t *actor = &actors->actors[a];

		if (actors_is_dead(actors, actor->id))
		{
			continue;
		}
		for (int i = 0; i < areas->count && found_count < AREA_OCCUPANTS_MAX; i++)
		{
			if (areas_contains(&areas->areas[i], actor->x, actor->y + ACTOR_MIDDLE_METERS, actor->z))
			{
				areas->found[found_count].area_id = areas->areas[i].id;
				areas->found[found_count].actor_id = actor->id;
				found_count++;
			}
		}
	}
	SDL_qsort(areas->found, (size_t)found_count, sizeof(area_occupant_t), compare_occupants);

	// What changed: both sorted, gone through together
	while (old_index < areas->inside_count || new_index < found_count)
	{
		int order = old_index >= areas->inside_count ? 1
			: (new_index >= found_count ? -1 : compare_occupants(&areas->inside[old_index], &areas->found[new_index]));

		if (order == 0)
		{
			old_index++;
			new_index++;
		}
		else if (order < 0)
		{
			// Gone out (dead or taken away: without a word)
			const area_occupant_t *gone = &areas->inside[old_index++];

			if (actor_living(actors, gone->actor_id) && area_tells(areas, gone->area_id, AREA_TRIGGER_ACTORS))
			{
				tell(areas, AREA_EVENT_EXIT, gone->area_id, false, gone->actor_id);
			}
		}
		else
		{
			const area_occupant_t *came = &areas->found[new_index++];

			if (area_tells(areas, came->area_id, AREA_TRIGGER_ACTORS))
			{
				tell(areas, AREA_EVENT_ENTER, came->area_id, false, came->actor_id);
			}
		}
	}
	SDL_memcpy(areas->inside, areas->found, sizeof(area_occupant_t) * (size_t)found_count);
	areas->inside_count = found_count;
}

bool areas_poll_event(areas_t *areas, area_event_t *event_out)
{
	if (areas->event_count == 0)
	{
		return false;
	}
	*event_out = areas->events[areas->event_first];
	areas->event_first = (areas->event_first + 1) % AREA_EVENTS_MAX;
	areas->event_count--;
	return true;
}

int areas_occupants(const areas_t *areas, const actors_t *actors, uint32_t id, const float player[3], uint32_t *ids_out, int maximum,
	bool *player_inside_out)
{
	int index = areas_find(areas, id);
	int count = 0;

	if (player_inside_out != NULL)
	{
		*player_inside_out = index >= 0 && player != NULL
			&& areas_contains(&areas->areas[index], player[0], player[1], player[2]);
	}
	if (index < 0)
	{
		return 0;
	}
	for (int a = 0; a < actors->count && count < maximum; a++)
	{
		const actor_t *actor = &actors->actors[a];

		if (!actors_is_dead(actors, actor->id) && areas_contains(&areas->areas[index], actor->x, actor->y + ACTOR_MIDDLE_METERS, actor->z))
		{
			ids_out[count++] = actor->id;
		}
	}
	return count;
}
