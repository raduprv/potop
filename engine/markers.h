// Markers: places the player is shown on the compass and the map (a
// quest's objective, a waypoint of their own). The game puts them and
// takes them away; one may be the active one (the objective followed): the
// compass shows how far it is.

#ifndef MARKERS_H
#define MARKERS_H

#include <stdbool.h>
#include <stdint.h>

#define MARKERS_MAX 64
#define MARKER_LABEL_LENGTH 48

typedef struct
{
	uint32_t id;                           // never 0
	float x;                               // meters, on the map
	float z;
	float color[3];                        // sRGB 0..1
	char label[MARKER_LABEL_LENGTH];
	bool on_compass;
	bool on_map;
	bool custom;                           // the player's own (put on the map)
} marker_t;

typedef struct
{
	marker_t list[MARKERS_MAX];
	int count;
	uint32_t next_id;
	uint32_t active;                       // 0: none
} markers_t;

void markers_create(markers_t *markers);
// A marker there, on the compass and the map; 0 if there is no room
uint32_t markers_add(markers_t *markers, float x, float z, const float color[3], const char *label);
bool markers_remove(markers_t *markers, uint32_t id);
marker_t *markers_get(markers_t *markers, uint32_t id);
bool markers_move(markers_t *markers, uint32_t id, float x, float z);
void markers_set_active(markers_t *markers, uint32_t id);
// Every one (keep_custom: but the player's own)
void markers_clear(markers_t *markers, bool keep_custom);
// The one nearest (x, z) within radius, 0 if none
uint32_t markers_nearest(const markers_t *markers, float x, float z, float radius);

#endif
