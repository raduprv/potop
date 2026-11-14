// The aurora's state. See aurora.h.

#include "aurora.h"

#include <SDL3/SDL.h>

// It comes on and goes out over this long
#define AURORA_EASE_SECONDS 4.0f
// The sun's height (degrees) where it begins to show, and where it is all there
#define AURORA_DARK_FROM -4.0f
#define AURORA_DARK_FULL -12.0f

void aurora_set(aurora_t *aurora, bool on, float strength)
{
	aurora->on = on;
	aurora->strength = SDL_max(strength, 0.0f);
}

void aurora_update(aurora_t *aurora, float seconds)
{
	float target = aurora->on ? 1.0f : 0.0f;
	float step = SDL_max(seconds, 0.0f) / AURORA_EASE_SECONDS;

	if (aurora->shown < target)
	{
		aurora->shown = SDL_min(aurora->shown + step, target);
	}
	else
	{
		aurora->shown = SDL_max(aurora->shown - step, target);
	}
}

float aurora_intensity(const aurora_t *aurora, float settings_strength, float sun_elevation_degrees)
{
	float dark = SDL_clamp((AURORA_DARK_FROM - sun_elevation_degrees) / (AURORA_DARK_FROM - AURORA_DARK_FULL), 0.0f, 1.0f);

	dark = dark * dark * (3.0f - 2.0f * dark);
	return aurora->shown * aurora->strength * SDL_max(settings_strength, 0.0f) * dark;
}
