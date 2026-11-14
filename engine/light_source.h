// What a light-giving thing says of its light: a particle effect
// (particles.ini) or an object type (objects.ini). See lights.h.

#ifndef LIGHT_SOURCE_H
#define LIGHT_SOURCE_H

#include <stdbool.h>

typedef struct
{
	float color[3];                    // 0..1 (as the eye sees it: sRGB), all 0: no light
	float intensity;                   // how bright: 1 lights a white wall a meter away about as the noon sun
	float radius_meters;               // nothing beyond this
	float flicker;                     // 0: steady .. 1: dances (fires)
	float height_meters;               // above where the thing is (objects: below 0, that much of the model's height)
	float core_meters;                 // a glow that big where the light is (a flame, a lantern's glass); 0: none
	bool switched;                     // lit at dusk and put out at dawn (lamps), or by the game; else burns on (fires)
} light_source_t;

// Reads "light" (r, g, b), "light_intensity", "light_radius",
// "light_flicker", "light_height", "light_core" and "light_switched" into a
// source. False if the key is
// none of those; true with *ok false if it is one but the value does not read
bool light_source_parse(light_source_t *source, const char *key, const char *value, bool *ok);

// Its defaults: no light, not switched; when "light" is given, intensity 1,
// radius 6 m
void light_source_reset(light_source_t *source);

static inline bool light_source_gives_light(const light_source_t *source)
{
	return source->intensity > 0.0f && source->radius_meters > 0.0f
		&& (source->color[0] > 0.0f || source->color[1] > 0.0f || source->color[2] > 0.0f);
}

#endif
