// What the person at the keys did to a scene: see damage_events.h

#include "damage_events.h"

#include <stdio.h>
#include <string.h>

#define LINE_LENGTH 256

static damage_event_t events[DAMAGE_EVENTS_MAX];
static int event_count;

int damage_events_count(void)
{
	return event_count;
}

const damage_event_t *damage_events_get(int event_index)
{
	if (event_index < 0 || event_index >= event_count)
	{
		return NULL;
	}
	return &events[event_index];
}

bool damage_events_add(const damage_event_t *event)
{
	if (event_count >= DAMAGE_EVENTS_MAX)
	{
		return false;
	}
	events[event_count++] = *event;
	return true;
}

bool damage_events_remove_last(void)
{
	if (event_count == 0)
	{
		return false;
	}
	event_count--;
	return true;
}

void damage_events_clear(void)
{
	event_count = 0;
}

bool damage_events_load(const char *path)
{
	FILE *file = fopen(path, "r");
	char line[LINE_LENGTH];

	damage_events_clear();
	if (file == NULL)
	{
		return false;
	}
	while (fgets(line, sizeof(line), file) != NULL)
	{
		damage_event_t event;
		size_t first = strspn(line, " \t\r\n");

		if (line[first] == '\0' || line[first] == '#')
		{
			continue;
		}
		memset(&event, 0, sizeof(event));
		if (sscanf(line + first, "ball %d %f %f %f %f %f %f", &event.frame_index, &event.position[0], &event.position[1], &event.position[2],
			&event.velocity[0], &event.velocity[1], &event.velocity[2]) == 7)
		{
			event.kind = DAMAGE_EVENT_BALL;
		}
		else if (sscanf(line + first, "blast %d %f %f %f %f", &event.frame_index, &event.position[0], &event.position[1], &event.position[2],
			&event.strength) == 5)
		{
			event.kind = DAMAGE_EVENT_BLAST;
		}
		else if (sscanf(line + first, "pour %d %f %f %f %f", &event.frame_index, &event.position[0], &event.position[2], &event.radius, &event.cubic_meters) == 5)
		{
			event.kind = DAMAGE_EVENT_POUR;
		}
		else if (sscanf(line + first, "flood %d %f", &event.frame_index, &event.height) == 2)
		{
			event.kind = DAMAGE_EVENT_FLOOD;
		}
		else if (sscanf(line + first, "dry %d", &event.frame_index) == 1)
		{
			event.kind = DAMAGE_EVENT_DRY;
		}
		else
		{
			fclose(file);
			damage_events_clear();
			return false;
		}
		if (!damage_events_add(&event))
		{
			break;
		}
	}
	fclose(file);
	return true;
}

bool damage_events_save(const char *path, const char *scene_name)
{
	FILE *file = fopen(path, "w");
	int i;

	if (file == NULL)
	{
		return false;
	}
	// (Nine digits: a float written and read again is the same float, so
	// the replay is the same to the last bit)
	fprintf(file, "# damage events, scene %s\n", scene_name);
	for (i = 0; i < event_count; i++)
	{
		const damage_event_t *event = &events[i];

		if (event->kind == DAMAGE_EVENT_BALL)
		{
			fprintf(file, "ball %d %.9g %.9g %.9g %.9g %.9g %.9g\n", event->frame_index,
				(double)event->position[0], (double)event->position[1], (double)event->position[2],
				(double)event->velocity[0], (double)event->velocity[1], (double)event->velocity[2]);
		}
		else if (event->kind == DAMAGE_EVENT_BLAST)
		{
			fprintf(file, "blast %d %.9g %.9g %.9g %.9g\n", event->frame_index,
				(double)event->position[0], (double)event->position[1], (double)event->position[2], (double)event->strength);
		}
		else if (event->kind == DAMAGE_EVENT_POUR)
		{
			fprintf(file, "pour %d %.9g %.9g %.9g %.9g\n", event->frame_index,
				(double)event->position[0], (double)event->position[2], (double)event->radius, (double)event->cubic_meters);
		}
		else if (event->kind == DAMAGE_EVENT_FLOOD)
		{
			fprintf(file, "flood %d %.9g\n", event->frame_index, (double)event->height);
		}
		else
		{
			fprintf(file, "dry %d\n", event->frame_index);
		}
	}
	return fclose(file) == 0;
}
