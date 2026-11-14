// The particle effects objects carry (object_effects.h)

#include "object_effects.h"

#include <SDL3/SDL.h>

// How often the objects are looked over (seconds): what comes near starts a
// little late, what moves is followed in steps (neither shows at this pace)
#define OBJECT_EFFECTS_LOOK_SECONDS 0.25f

// A building laid down by the flood this far no longer burns or smokes
#define OBJECT_EFFECTS_FLATTENED 0.2f

void object_effects_initialize(object_effects_t *effects)
{
	SDL_zerop(effects);
}

void object_effects_clear(object_effects_t *effects, particles_t *particles)
{
	for (int i = 0; i < effects->playing_count; i++)
	{
		particles_stop(particles, effects->playing[i].emitter);
	}
	effects->playing_count = 0;
}

static void rotate_by_quaternion(const float rotation[4], const float vector[3], float out[3])
{
	// v + 2w (q x v) + 2 q x (q x v)
	float twice_cross[3] =
	{
		2.0f * (rotation[1] * vector[2] - rotation[2] * vector[1]),
		2.0f * (rotation[2] * vector[0] - rotation[0] * vector[2]),
		2.0f * (rotation[0] * vector[1] - rotation[1] * vector[0]),
	};

	out[0] = vector[0] + rotation[3] * twice_cross[0] + (rotation[1] * twice_cross[2] - rotation[2] * twice_cross[1]);
	out[1] = vector[1] + rotation[3] * twice_cross[1] + (rotation[2] * twice_cross[0] - rotation[0] * twice_cross[2]);
	out[2] = vector[2] + rotation[3] * twice_cross[2] + (rotation[0] * twice_cross[1] - rotation[1] * twice_cross[0]);
}

static object_effect_playing_t *find_playing(object_effects_t *effects, uint32_t object_id, int effect_index)
{
	for (int i = 0; i < effects->playing_count; i++)
	{
		if (effects->playing[i].object_id == object_id && effects->playing[i].effect_index == effect_index)
		{
			return &effects->playing[i];
		}
	}
	return NULL;
}

// One of an object's effects, wanted where it now is: started, or moved
// there if it plays already
static void want_effect(object_effects_t *effects, particles_t *particles, const object_instance_t *instance,
	const object_file_effect_t *effect, int effect_index, const float center[3], const float extents[3])
{
	object_effect_playing_t *playing = find_playing(effects, instance->id, effect_index);
	int particles_effect = particles_find_effect(particles, effect->effect);
	particles_emitter_t *emitter = NULL;
	float rate_scale = effect->rate_scale > 0.0f ? effect->rate_scale : 1.0f;

	if (particles_effect < 0)
	{
		return;
	}
	if (playing != NULL)
	{
		emitter = particles_find_emitter(particles, playing->emitter);
		// (another effect by that name after particles.ini was read
		// again, or stopped by someone else: started again)
		if (emitter == NULL || playing->particles_effect != particles_effect)
		{
			particles_stop(particles, playing->emitter);
			emitter = NULL;
		}
	}
	if (emitter == NULL)
	{
		uint32_t id = particles_play_on(particles, particles_effect, center, instance->rotation, extents);

		emitter = particles_find_emitter(particles, id);
		if (emitter == NULL)
		{
			return;
		}
		if (playing == NULL)
		{
			if (effects->playing_count >= OBJECT_EFFECTS_PLAYING_MAX)
			{
				particles_stop(particles, id);
				return;
			}
			playing = &effects->playing[effects->playing_count++];
			playing->object_id = instance->id;
			playing->effect_index = effect_index;
		}
		playing->emitter = id;
		playing->particles_effect = particles_effect;
	}
	SDL_memcpy(emitter->position, center, sizeof(emitter->position));
	SDL_memcpy(emitter->rotation, instance->rotation, sizeof(emitter->rotation));
	SDL_memcpy(emitter->extents, extents, sizeof(emitter->extents));
	emitter->rate_scale = rate_scale;
	playing->seen_stamp = effects->stamp;
}

typedef struct
{
	object_effects_t *effects;
	particles_t *particles;
	const water_t *water;
} effects_look_t;

// One object near the eye: its model's effects wanted (or not, if flooded)
static bool look_at_object(const objects_t *objects, int i, void *user)
{
	effects_look_t *look = user;
	const object_instance_t *instance = &objects->instances[i];
	int model_index = objects_instance_model(objects, instance);
	const object_model_t *model;
	float water_depth = 0.0f;
	float velocity_x;
	float velocity_z;
	float placement[3];

	if (model_index < 0)
	{
		return true;
	}
	model = &objects->models[model_index];
	if (model->effect_count == 0 || instance->damage == OBJECT_DAMAGE_DESTROYED || instance->flatten > OBJECT_EFFECTS_FLATTENED)
	{
		return true;
	}
	placement[0] = instance->x;
	placement[1] = objects_instance_base_y(instance);
	placement[2] = instance->z;
	if (look->water != NULL && !water_sample(look->water, instance->x, instance->z, &water_depth, &velocity_x, &velocity_z))
	{
		water_depth = 0.0f;
	}
	for (int e = 0; e < model->effect_count; e++)
	{
		const object_file_effect_t *effect = &model->effects[e];
		float scaled[3] = { effect->center[0] * instance->scale, effect->center[1] * instance->scale, effect->center[2] * instance->scale };
		float turned[3];
		float center[3];
		float extents[3] = { effect->extents[0] * instance->scale, effect->extents[1] * instance->scale, effect->extents[2] * instance->scale };

		rotate_by_quaternion(instance->rotation, scaled, turned);
		for (int k = 0; k < 3; k++)
		{
			center[k] = placement[k] + turned[k];
		}
		// (the water over its foot: a flooded forge goes out)
		if (water_depth > 0.02f && instance->ground_height + water_depth > center[1] - extents[1])
		{
			continue;
		}
		want_effect(look->effects, look->particles, instance, effect, e, center, extents);
	}
	return true;
}

void object_effects_update(object_effects_t *effects, particles_t *particles, const objects_t *objects, const water_t *water,
	const float eye[3], float radius_meters, float frame_seconds)
{
	int kept = 0;

	effects->seconds_to_next_look -= frame_seconds;
	if (effects->seconds_to_next_look > 0.0f)
	{
		return;
	}
	effects->seconds_to_next_look = OBJECT_EFFECTS_LOOK_SECONDS;
	effects->stamp++;
	// (the objects within the radius, by the grid)
	if (radius_meters > 0.0f)
	{
		effects_look_t look = { effects, particles, water };

		objects_visit_circle(objects, eye[0], eye[2], radius_meters, look_at_object, &look);
	}
	// What was not wanted this time: stopped
	for (int i = 0; i < effects->playing_count; i++)
	{
		if (effects->playing[i].seen_stamp != effects->stamp)
		{
			particles_stop(particles, effects->playing[i].emitter);
			continue;
		}
		effects->playing[kept++] = effects->playing[i];
	}
	effects->playing_count = kept;
}
