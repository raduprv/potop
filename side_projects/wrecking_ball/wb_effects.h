// How explosions look: a ball of fire that swells and dies in half a
// second, smoke that rises and thins over three, a flash lighting what
// is near for the first moment, and a shock wave bending the air as it
// runs outwards. Only looks: the world does the pushing (wb_world.h).

#ifndef WB_EFFECTS_H
#define WB_EFFECTS_H

#include <stdbool.h>

#include "wb_render.h"

#define WB_EXPLOSIONS_MAX 16

typedef struct
{
	bool used;
	float position[3];
	float kilograms;
	float age_seconds;
} wb_explosion_t;

typedef struct
{
	wb_explosion_t explosions[WB_EXPLOSIONS_MAX];
} wb_effects_t;

// An explosion's looks from now (the oldest goes when all are in use)
void wb_effects_start(wb_effects_t *effects, const float position[3], float kilograms);
// Older by seconds (of the world's time: none while it is paused)
void wb_effects_update(wb_effects_t *effects, float seconds);
// This frame's fire, smoke, flash and shock waves, between
// wb_render_begin_frame and wb_render_end_frame
void wb_effects_draw(const wb_effects_t *effects, wb_render_t *render);
void wb_effects_clear(wb_effects_t *effects);

#endif
