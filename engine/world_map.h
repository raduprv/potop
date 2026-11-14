// The world map: the whole map seen from above, as a picture for the map
// window (M on foot). An artist's picture if there is one for the map
// (data/maps/NAME.png, NAME the map's name without its kind, else
// data/maps/world.png; north up, the map's whole extent), else one made
// from the land: its relief shaded from the north-west, the ground's
// materials in muted colors, the water blue by its depth, the woods
// darker, on a paper tone.

#ifndef WORLD_MAP_H
#define WORLD_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#include "objects.h"
#include "terrain.h"
#include "water.h"

typedef struct
{
	SDL_GPUTexture *texture;
	int width;
	int height;
	bool from_artist;
	float extent_meters;                   // the map's side the picture covers
	uint64_t made_ticks;                   // SDL_GetTicks when made (0: never)
	char made_for[64];                     // the map name it was made for
} world_map_t;

// Makes the picture (again): map_name the map's (without its kind; empty:
// an unsaved one). False if neither could be made
bool world_map_make(world_map_t *world_map, const terrain_t *terrain, const water_t *water, const objects_t *objects, const char *map_name);
void world_map_destroy(world_map_t *world_map);

#endif
