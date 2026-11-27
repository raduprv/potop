// Objects on fire: see burning.h

#include "burning.h"

#include <SDL3/SDL.h>

#include "ini_parser.h"

static void default_settings(burning_settings_t *settings)
{
	SDL_zerop(settings);
	settings->spread_meters_per_second = 0.05f;
	settings->char_seconds = 25.0f;
	settings->burn_seconds = 150.0f;
	settings->ash_seconds = 40.0f;
	settings->site_spacing_meters = 0.4f;
	SDL_strlcpy(settings->flame_effect, "burning_flames", sizeof(settings->flame_effect));
	SDL_strlcpy(settings->smoke_effect, "burning_smoke", sizeof(settings->smoke_effect));
	SDL_strlcpy(settings->embers_effect, "burning_embers", sizeof(settings->embers_effect));
}

static bool read_key(void *user, const char *section, const char *key, const char *value)
{
	burning_settings_t *settings = user;

	if (SDL_strcmp(section, "wood") != 0)
	{
		return false;
	}
	if (SDL_strcmp(key, "spread_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &settings->spread_meters_per_second);
	}
	if (SDL_strcmp(key, "char_seconds") == 0)
	{
		return ini_value_to_float(value, &settings->char_seconds);
	}
	if (SDL_strcmp(key, "burn_seconds") == 0)
	{
		return ini_value_to_float(value, &settings->burn_seconds);
	}
	if (SDL_strcmp(key, "ash_seconds") == 0)
	{
		return ini_value_to_float(value, &settings->ash_seconds);
	}
	if (SDL_strcmp(key, "site_spacing_meters") == 0)
	{
		return ini_value_to_float(value, &settings->site_spacing_meters);
	}
	if (SDL_strcmp(key, "flame_effect") == 0)
	{
		SDL_strlcpy(settings->flame_effect, value, sizeof(settings->flame_effect));
		return true;
	}
	if (SDL_strcmp(key, "smoke_effect") == 0)
	{
		SDL_strlcpy(settings->smoke_effect, value, sizeof(settings->smoke_effect));
		return true;
	}
	if (SDL_strcmp(key, "embers_effect") == 0)
	{
		SDL_strlcpy(settings->embers_effect, value, sizeof(settings->embers_effect));
		return true;
	}
	return false;
}

void burning_initialize(burning_t *burning, const particles_t *particles)
{
	SDL_zerop(burning);
	default_settings(&burning->settings);
	if (!ini_parse_data_file("burning.ini", read_key, &burning->settings))
	{
		SDL_Log("Burning: no data/burning.ini, the defaults");
	}
	burning->settings.spread_meters_per_second = SDL_max(burning->settings.spread_meters_per_second, 0.001f);
	burning->settings.char_seconds = SDL_max(burning->settings.char_seconds, 0.1f);
	burning->settings.burn_seconds = SDL_max(burning->settings.burn_seconds, burning->settings.char_seconds);
	burning->settings.ash_seconds = SDL_max(burning->settings.ash_seconds, 0.1f);
	burning->settings.site_spacing_meters = SDL_max(burning->settings.site_spacing_meters, 0.05f);
	burning->flame_effect = particles_find_effect(particles, burning->settings.flame_effect);
	burning->smoke_effect = particles_find_effect(particles, burning->settings.smoke_effect);
	burning->embers_effect = particles_find_effect(particles, burning->settings.embers_effect);
	if (burning->flame_effect < 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Burning: particles.ini has no [%s] (the flames)", burning->settings.flame_effect);
	}
}

// A quaternion (x, y, z, w) turning a vector
static void rotate_vector(const float rotation[4], const float vector[3], float out[3])
{
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

// Where a point of the model is in the world (as object.vert places it)
static void model_to_world(const object_instance_t *instance, const float model[3], float world[3])
{
	float scaled[3] = { model[0] * instance->scale, model[1] * instance->scale, model[2] * instance->scale };
	float turned[3];

	rotate_vector(instance->rotation, scaled, turned);
	world[0] = instance->x + turned[0];
	world[1] = objects_instance_base_y(instance) + turned[1];
	world[2] = instance->z + turned[2];
}

static void world_to_model(const object_instance_t *instance, const float world[3], float model[3])
{
	float undo[4] = { -instance->rotation[0], -instance->rotation[1], -instance->rotation[2], instance->rotation[3] };
	float offset[3] = { world[0] - instance->x, world[1] - objects_instance_base_y(instance), world[2] - instance->z };
	float turned[3];
	float inverse_scale = 1.0f / SDL_max(instance->scale, 1e-4f);

	rotate_vector(undo, offset, turned);
	model[0] = turned[0] * inverse_scale;
	model[1] = turned[1] * inverse_scale;
	model[2] = turned[2] * inverse_scale;
}

// a then b (quaternions x, y, z, w): b turned by a
static void multiply_rotations(const float a[4], const float b[4], float out[4])
{
	out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static float distance_3(const float a[3], const float b[3])
{
	float x = a[0] - b[0];
	float y = a[1] - b[1];
	float z = a[2] - b[2];

	return SDL_sqrtf(x * x + y * y + z * z);
}

static void stop_emitters(burning_fire_t *fire, particles_t *particles)
{
	for (int i = 0; i < fire->site_count; i++)
	{
		if (fire->flame_emitter[i] != 0)
		{
			particles_stop(particles, fire->flame_emitter[i]);
			fire->flame_emitter[i] = 0;
		}
	}
	if (fire->smoke_emitter != 0)
	{
		particles_stop(particles, fire->smoke_emitter);
		fire->smoke_emitter = 0;
	}
	if (fire->embers_emitter != 0)
	{
		particles_stop(particles, fire->embers_emitter);
		fire->embers_emitter = 0;
	}
}

bool burning_ignite(burning_t *burning, objects_t *objects, particles_t *particles, int instance_index, const float point[3])
{
	object_instance_t *instance;
	const object_model_t *model;
	burning_fire_t *fire = NULL;
	int slot = -1;
	int model_index;
	float minimum[3];
	float maximum[3];
	int axis = 0;
	float length;

	// (particles.ini may have been read again since)
	burning->flame_effect = particles_find_effect(particles, burning->settings.flame_effect);
	burning->smoke_effect = particles_find_effect(particles, burning->settings.smoke_effect);
	burning->embers_effect = particles_find_effect(particles, burning->settings.embers_effect);
	if (instance_index < 0 || instance_index >= objects->instance_count)
	{
		return false;
	}
	instance = &objects->instances[instance_index];
	if (instance->burn_slot != 0 || instance->id == 0)
	{
		return false;
	}
	model_index = objects_instance_model(objects, instance);
	if (model_index < 0)
	{
		return false;
	}
	for (int i = 0; i < OBJECTS_BURN_SLOTS; i++)
	{
		if (!burning->fires[i].used)
		{
			slot = i;
			break;
		}
	}
	if (slot < 0)
	{
		SDL_Log("Burning: %d fires already, no room for another", OBJECTS_BURN_SLOTS);
		return false;
	}
	model = &objects->models[model_index];
	fire = &burning->fires[slot];
	SDL_zerop(fire);
	fire->used = true;
	fire->object_id = instance->id;
	world_to_model(instance, point, fire->ignite_model);
	minimum[0] = model->bounds_minimum.x;
	minimum[1] = model->bounds_minimum.y;
	minimum[2] = model->bounds_minimum.z;
	maximum[0] = model->bounds_maximum.x;
	maximum[1] = model->bounds_maximum.y;
	maximum[2] = model->bounds_maximum.z;
	// (lit a little off its surface, the ray's point: kept inside its box)
	for (int k = 0; k < 3; k++)
	{
		fire->ignite_model[k] = SDL_clamp(fire->ignite_model[k], minimum[k], maximum[k]);
	}
	// The farthest corner of its box: past that the front has caught all
	for (int corner = 0; corner < 8; corner++)
	{
		float at[3] = { (corner & 1) ? maximum[0] : minimum[0], (corner & 2) ? maximum[1] : minimum[1], (corner & 4) ? maximum[2] : minimum[2] };

		fire->reach_meters = SDL_max(fire->reach_meters, distance_3(at, fire->ignite_model));
	}
	// The flames' sites: along its longest side, over its middle, a little
	// up from it (a lying log's top), from end to end
	for (int k = 1; k < 3; k++)
	{
		if (maximum[k] - minimum[k] > maximum[axis] - minimum[axis])
		{
			axis = k;
		}
	}
	length = maximum[axis] - minimum[axis];
	// (its longest side turned onto X for the shader: Y round Z by a
	// quarter turn back, Z round Y by a quarter turn)
	fire->to_grain[0] = 0.0f;
	fire->to_grain[1] = axis == 2 ? 0.70710678f : 0.0f;
	fire->to_grain[2] = axis == 1 ? -0.70710678f : 0.0f;
	fire->to_grain[3] = axis == 0 ? 1.0f : 0.70710678f;
	fire->site_count = SDL_clamp((int)SDL_ceilf(length / burning->settings.site_spacing_meters), 1, BURNING_SITES_MAX);
	for (int i = 0; i < fire->site_count; i++)
	{
		float along = ((float)i + 0.5f) / (float)fire->site_count;

		for (int k = 0; k < 3; k++)
		{
			fire->site_model[i][k] = 0.5f * (minimum[k] + maximum[k]);
		}
		fire->site_model[i][axis] = minimum[axis] + along * length;
		if (axis != 1)
		{
			fire->site_model[i][1] = minimum[1] + 0.75f * (maximum[1] - minimum[1]);
		}
		fire->site_distance[i] = distance_3(fire->site_model[i], fire->ignite_model);
	}
	instance->burn_slot = (uint8_t)(slot + 1);
	objects_touch(objects, instance_index);
	SDL_Log("Burning: object %u lit (fire %d), %d flames' sites, all of it caught in %.0f s, gone in %.0f s", (unsigned)instance->id, slot,
		fire->site_count, (double)(fire->reach_meters / burning->settings.spread_meters_per_second),
		(double)(fire->reach_meters / burning->settings.spread_meters_per_second + burning->settings.burn_seconds + burning->settings.ash_seconds));
	return true;
}

// How strong a point's flames are this long after the front reached it:
// flaring up, burning, dying down to embers
static float flame_strength(const burning_settings_t *settings, float since)
{
	float rise = settings->char_seconds * 0.3f;
	float fall_start = settings->burn_seconds * 0.45f;
	float fall_end = settings->burn_seconds * 0.8f;

	if (since <= 0.0f || since >= fall_end)
	{
		return 0.0f;
	}
	if (since < rise)
	{
		return since / rise;
	}
	if (since > fall_start)
	{
		return 1.0f - (since - fall_start) / (fall_end - fall_start);
	}
	return 1.0f;
}

// An emitter of this effect playing at world, at this strength (rate):
// started if it is not playing, stopped at 0
static void keep_emitter(particles_t *particles, int effect, uint32_t *emitter, const float world[3], float strength)
{
	particles_emitter_t *playing = *emitter != 0 ? particles_find_emitter(particles, *emitter) : NULL;

	if (strength <= 0.01f || effect < 0)
	{
		if (*emitter != 0)
		{
			particles_stop(particles, *emitter);
			*emitter = 0;
		}
		return;
	}
	if (playing == NULL)
	{
		*emitter = particles_play(particles, effect, world[0], world[1], world[2]);
		playing = *emitter != 0 ? particles_find_emitter(particles, *emitter) : NULL;
	}
	if (playing != NULL)
	{
		playing->position[0] = world[0];
		playing->position[1] = world[1];
		playing->position[2] = world[2];
		playing->rate_scale = strength;
	}
}

static void put_out(burning_t *burning, int slot, objects_t *objects, particles_t *particles, bool burnt_away)
{
	burning_fire_t *fire = &burning->fires[slot];
	int index = objects_find_id(objects, fire->object_id);

	stop_emitters(fire, particles);
	if (index >= 0)
	{
		if (burnt_away)
		{
			objects_kill(objects, index);
		}
		else
		{
			objects->instances[index].burn_slot = 0;
			objects_touch(objects, index);
		}
	}
	SDL_zerop(fire);
}

void burning_update(burning_t *burning, objects_t *objects, particles_t *particles, float seconds)
{
	const burning_settings_t *settings = &burning->settings;

	SDL_zeroa(objects->burn_uniforms);
	for (int slot = 0; slot < OBJECTS_BURN_SLOTS; slot++)
	{
		burning_fire_t *fire = &burning->fires[slot];
		object_instance_t *instance;
		int index;
		float front;
		float total = 0.0f;
		float middle[3] = { 0.0f, 0.0f, 0.0f };
		float *uniforms = &objects->burn_uniforms[slot * OBJECTS_BURN_FLOATS];

		if (!fire->used)
		{
			continue;
		}
		index = objects_find_id(objects, fire->object_id);
		if (index < 0 || (objects->instances[index].state & OBJECT_STATE_DEAD) != 0)
		{
			// (gone some other way: its fire with it)
			stop_emitters(fire, particles);
			SDL_zerop(fire);
			continue;
		}
		instance = &objects->instances[index];
		if (instance->burn_slot != slot + 1)
		{
			instance->burn_slot = (uint8_t)(slot + 1);
			objects_touch(objects, index);
		}
		fire->elapsed_seconds += seconds;
		front = fire->elapsed_seconds * settings->spread_meters_per_second;
		// All of it ash and crumbled away: off the map
		if (front > fire->reach_meters + 0.2f
			&& fire->elapsed_seconds > (fire->reach_meters + 0.2f) / settings->spread_meters_per_second + settings->burn_seconds + settings->ash_seconds)
		{
			SDL_Log("Burning: object %u burnt away", (unsigned)fire->object_id);
			put_out(burning, slot, objects, particles, true);
			continue;
		}
		// The flames at each site, as long ago as the front reached it
		for (int i = 0; i < fire->site_count; i++)
		{
			float since = (front - fire->site_distance[i]) / settings->spread_meters_per_second;
			float strength = flame_strength(settings, since);
			float world[3];

			model_to_world(instance, fire->site_model[i], world);
			keep_emitter(particles, burning->flame_effect, &fire->flame_emitter[i], world, strength);
			total += strength;
			for (int k = 0; k < 3; k++)
			{
				middle[k] += world[k] * strength;
			}
		}
		// The smoke and the embers over what burns, as much as burns
		{
			float share = total / (float)SDL_max(fire->site_count, 1);

			if (total > 0.0f)
			{
				for (int k = 0; k < 3; k++)
				{
					middle[k] /= total;
				}
			}
			keep_emitter(particles, burning->smoke_effect, &fire->smoke_emitter, middle, SDL_min(1.0f, 0.3f + share));
			keep_emitter(particles, burning->embers_effect, &fire->embers_emitter, middle, share);
			if (total <= 0.0f)
			{
				keep_emitter(particles, burning->smoke_effect, &fire->smoke_emitter, middle, 0.0f);
			}
		}
		// The shader's numbers: its placement undone, its front
		uniforms[0] = instance->x;
		uniforms[1] = objects_instance_base_y(instance);
		uniforms[2] = instance->z;
		uniforms[3] = 1.0f / SDL_max(instance->scale, 1e-4f);
		{
			// (into its model's space, then turned along the grain: where it
			// was lit as well)
			float undo[4] = { -instance->rotation[0], -instance->rotation[1], -instance->rotation[2], instance->rotation[3] };
			float into_grain[4];
			float ignite_grain[3];

			multiply_rotations(fire->to_grain, undo, into_grain);
			rotate_vector(fire->to_grain, fire->ignite_model, ignite_grain);
			SDL_memcpy(&uniforms[4], into_grain, sizeof(into_grain));
			SDL_memcpy(&uniforms[8], ignite_grain, sizeof(ignite_grain));
		}
		uniforms[11] = front;
		uniforms[12] = settings->spread_meters_per_second;
		uniforms[13] = settings->char_seconds;
		uniforms[14] = settings->burn_seconds;
		uniforms[15] = settings->ash_seconds;
	}
}

void burning_clear(burning_t *burning, objects_t *objects, particles_t *particles)
{
	for (int slot = 0; slot < OBJECTS_BURN_SLOTS; slot++)
	{
		if (burning->fires[slot].used)
		{
			put_out(burning, slot, objects, particles, false);
		}
	}
	SDL_zeroa(objects->burn_uniforms);
}

int burning_count(const burning_t *burning)
{
	int count = 0;

	for (int slot = 0; slot < OBJECTS_BURN_SLOTS; slot++)
	{
		count += burning->fires[slot].used ? 1 : 0;
	}
	return count;
}
