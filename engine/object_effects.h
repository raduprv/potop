// The particle effects objects carry (object_format.h, OBJECT_FILE_EFFECTS):
// a forge's fire, a chimney's smoke, written into the model by its
// generator and played wherever an object of it stands, without anyone
// placing them by hand. Only near the eye (objects.effects_meters): each
// is started as its object comes near and stopped as it goes away, goes
// out, falls or is flooded, and follows it if it moves. Looked over a few
// times a second, not every frame.

#ifndef OBJECT_EFFECTS_H
#define OBJECT_EFFECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "objects.h"
#include "particles.h"
#include "water.h"

#define OBJECT_EFFECTS_PLAYING_MAX 256

typedef struct
{
	uint32_t object_id;            // the object's id (object_instance_t.id)
	int effect_index;              // which of its model's effects
	int particles_effect;          // particles.ini's, as started (another after a reload: started again)
	uint32_t emitter;              // the particles' emitter id
	uint32_t seen_stamp;           // the last look that found it still wanted
} object_effect_playing_t;

typedef struct
{
	object_effect_playing_t playing[OBJECT_EFFECTS_PLAYING_MAX];
	int playing_count;
	uint32_t stamp;
	float seconds_to_next_look;
} object_effects_t;

void object_effects_initialize(object_effects_t *effects);

// Once a frame (looks only every so often): eye where the effects are
// wanted around, radius_meters how far (0: none at all)
void object_effects_update(object_effects_t *effects, particles_t *particles, const objects_t *objects, const water_t *water,
	const float eye[3], float radius_meters, float frame_seconds);

// All stopped at once (a new map)
void object_effects_clear(object_effects_t *effects, particles_t *particles);

#endif
