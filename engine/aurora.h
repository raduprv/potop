// The aurora: curtains of light hanging over the north on dark nights,
// green below (oxygen, about 100 km up), going red and violet above, their
// folds rolling slowly (data/shaders/aurora.glsl, drawn with the sky, so
// the water mirrors them too). It lights the land a little: a faint green
// in the light from the sky.
//
// The game decides when there is one (aurora_set); it shows only when it
// is dark (the sun well under the horizon), fading in after dusk and out
// before dawn, and stays on until put out. North is away from the noon sun
// (render.sun_azimuth_degrees).

#ifndef AURORA_H
#define AURORA_H

#include <stdbool.h>

typedef struct
{
	bool on;                       // the game wants one
	float strength;                // how bright (1: a good display)
	float shown;                   // 0..1, eased towards on (seconds, see aurora_update)
} aurora_t;

// Its faint green in the light from the sky, at full strength (added to
// the scene's sky color: the land's ambient light, the horizon)
#define AURORA_SKY_TINT_RED 0.002f
#define AURORA_SKY_TINT_GREEN 0.012f
#define AURORA_SKY_TINT_BLUE 0.005f

void aurora_set(aurora_t *aurora, bool on, float strength);

// Eases it in or out (over a few seconds)
void aurora_update(aurora_t *aurora, float seconds);

// How much it shows now (0: none): its strength, how far it has eased in,
// render.aurora_strength, and how dark it is (from the sun's height)
float aurora_intensity(const aurora_t *aurora, float settings_strength, float sun_elevation_degrees);

#endif
