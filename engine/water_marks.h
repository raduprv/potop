// What the water leaves on things: the flood's mark (a band of mud up to
// where the water stood, fading over a while), the wet that dries, the
// algae on what stays under water. Kept per 2 x 2 cells on the GPU
// (water_marks.comp), drawn by the objects' shaders (ground_water.glsl).

#ifndef WATER_MARKS_H
#define WATER_MARKS_H

#include <stdbool.h>

#include "gpu.h"

typedef struct
{
	SDL_GPUComputePipeline *pipeline;
	SDL_GPUTexture *texture;       // RGBA8, (map size + 1) / 2 per side: water_marks.comp
	int map_size;
	int size;
	bool cleared;
	float pending_seconds;
	// No water anywhere, and none for long enough that all of it has gone:
	// it rests (dry_seconds: since there was water)
	float dry_seconds;
	bool resting;
} water_marks_t;

bool water_marks_create(water_marks_t *marks);
void water_marks_destroy(water_marks_t *marks);

// A few times a second, before the objects are drawn, outside any pass.
// water_texture: RG32F, the water's depth (NULL: none); water_present: any
// water on the map at all
void water_marks_update(water_marks_t *marks, SDL_GPUTexture *water_texture, int map_size, float seconds, bool water_present);

// A new map: nothing left
void water_marks_clear(water_marks_t *marks);

// Turned off: its texture let go (the next update, turned on again, makes
// it anew, empty)
void water_marks_release(water_marks_t *marks);

#endif
