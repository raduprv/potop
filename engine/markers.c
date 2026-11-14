// Markers. See markers.h.

#include "markers.h"

#include <SDL3/SDL.h>

void markers_create(markers_t *markers)
{
	SDL_zerop(markers);
	markers->next_id = 1;
}

uint32_t markers_add(markers_t *markers, float x, float z, const float color[3], const char *label)
{
	marker_t *marker;

	if (markers->count >= MARKERS_MAX)
	{
		return 0;
	}
	marker = &markers->list[markers->count++];
	SDL_zerop(marker);
	marker->id = markers->next_id++;
	marker->x = x;
	marker->z = z;
	if (color != NULL)
	{
		SDL_memcpy(marker->color, color, sizeof(marker->color));
	}
	else
	{
		marker->color[0] = 1.0f;
		marker->color[1] = 0.85f;
		marker->color[2] = 0.2f;
	}
	SDL_strlcpy(marker->label, label != NULL ? label : "", sizeof(marker->label));
	marker->on_compass = true;
	marker->on_map = true;
	return marker->id;
}

marker_t *markers_get(markers_t *markers, uint32_t id)
{
	for (int i = 0; i < markers->count && id != 0; i++)
	{
		if (markers->list[i].id == id)
		{
			return &markers->list[i];
		}
	}
	return NULL;
}

bool markers_remove(markers_t *markers, uint32_t id)
{
	for (int i = 0; i < markers->count; i++)
	{
		if (markers->list[i].id == id)
		{
			// (the order kept: the lists show them as they came)
			SDL_memmove(&markers->list[i], &markers->list[i + 1], sizeof(marker_t) * (size_t)(markers->count - i - 1));
			markers->count--;
			if (markers->active == id)
			{
				markers->active = 0;
			}
			return true;
		}
	}
	return false;
}

bool markers_move(markers_t *markers, uint32_t id, float x, float z)
{
	marker_t *marker = markers_get(markers, id);

	if (marker == NULL)
	{
		return false;
	}
	marker->x = x;
	marker->z = z;
	return true;
}

void markers_set_active(markers_t *markers, uint32_t id)
{
	markers->active = markers_get(markers, id) != NULL ? id : 0;
}

void markers_clear(markers_t *markers, bool keep_custom)
{
	int kept = 0;

	for (int i = 0; i < markers->count; i++)
	{
		if (keep_custom && markers->list[i].custom)
		{
			markers->list[kept++] = markers->list[i];
		}
	}
	markers->count = kept;
	if (markers_get(markers, markers->active) == NULL)
	{
		markers->active = 0;
	}
}

uint32_t markers_nearest(const markers_t *markers, float x, float z, float radius)
{
	uint32_t found = 0;
	float best = radius * radius;

	for (int i = 0; i < markers->count; i++)
	{
		float dx = markers->list[i].x - x;
		float dz = markers->list[i].z - z;
		float distance = dx * dx + dz * dz;

		if (distance <= best)
		{
			best = distance;
			found = markers->list[i].id;
		}
	}
	return found;
}
