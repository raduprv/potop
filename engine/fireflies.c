// Fireflies. See fireflies.h.

#include <SDL3/SDL.h>

#include "fireflies.h"

#define SWARM_NEAREST_METERS 6.0f      // where new swarms go: this far from the eye ...
#define SWARM_FARTHEST_METERS 60.0f    // ... to this far
#define SWARM_GONE_METERS 90.0f        // farther than this, a swarm ends
#define SWARM_SECONDS_MINIMUM 25.0f
#define SWARM_SECONDS_MAXIMUM 60.0f
#define CHANGE_SECONDS 0.4f            // one swarm more or less at most this often
#define NIGHT_DARKNESS 0.8f            // darker than this (lights_darkness): their time
#define WATER_SEARCH_METERS 8.0f       // water this near makes a place likelier

static float random_unit(fireflies_t *fireflies)
{
	fireflies->random_state = fireflies->random_state * 1664525u + 1013904223u;
	return (float)(fireflies->random_state >> 8) / 16777216.0f;
}

void fireflies_initialize(fireflies_t *fireflies)
{
	SDL_zerop(fireflies);
	fireflies->random_state = 0x2545F491u;
	fireflies->effect = -1;
}

void fireflies_clear(fireflies_t *fireflies, particles_t *particles)
{
	for (int i = 0; i < FIREFLY_SWARMS_MAX; i++)
	{
		if (fireflies->swarms[i].used && particles != NULL)
		{
			particles_stop(particles, fireflies->swarms[i].emitter);
		}
		fireflies->swarms[i].used = false;
	}
	fireflies->effect = -1;
}

// Whether there is water standing at a point (more than a film)
static bool wet(const water_t *water, float x, float z)
{
	float depth = 0.0f;
	float velocity_x;
	float velocity_z;

	return water_sample(water, x, z, &depth, &velocity_x, &velocity_z) && depth > 0.02f;
}

// A place for a swarm near the eye: dry, on earth or clay (where grass
// grows), likelier near water; false if the tries found none
static bool find_place(fireflies_t *fireflies, const terrain_t *terrain, const water_t *water, const float eye[3], float *x_out,
	float *z_out)
{
	float extent = (float)terrain->size * terrain->cell_size_meters;

	for (int attempt = 0; attempt < 8; attempt++)
	{
		float angle = random_unit(fireflies) * 2.0f * SDL_PI_F;
		// Even over the ring's area, not bunched near the eye
		float reach = SDL_sqrtf(SWARM_NEAREST_METERS * SWARM_NEAREST_METERS + random_unit(fireflies)
			* (SWARM_FARTHEST_METERS * SWARM_FARTHEST_METERS - SWARM_NEAREST_METERS * SWARM_NEAREST_METERS));
		float x = eye[0] + SDL_cosf(angle) * reach;
		float z = eye[2] + SDL_sinf(angle) * reach;
		int32_t cell_x = (int32_t)(x / terrain->cell_size_meters + 0.5f);
		int32_t cell_z = (int32_t)(z / terrain->cell_size_meters + 0.5f);
		uint8_t soil;
		bool near_water = false;

		if (x < 0.0f || z < 0.0f || x >= extent || z >= extent || cell_x < 0 || cell_z < 0 || cell_x >= terrain->size
			|| cell_z >= terrain->size)
		{
			continue;
		}
		soil = terrain->soil_types[(size_t)cell_z * (size_t)terrain->size + (size_t)cell_x];
		if ((soil != SOIL_EARTH && soil != SOIL_CLAY) || (water != NULL && wet(water, x, z)))
		{
			continue;
		}
		for (int k = 0; k < 6 && water != NULL && !near_water; k++)
		{
			float around = (float)k / 6.0f * 2.0f * SDL_PI_F;

			near_water = wet(water, x + SDL_cosf(around) * WATER_SEARCH_METERS, z + SDL_sinf(around) * WATER_SEARCH_METERS);
		}
		if (near_water || random_unit(fireflies) < 0.3f)
		{
			*x_out = x;
			*z_out = z;
			return true;
		}
	}
	return false;
}

void fireflies_update(fireflies_t *fireflies, particles_t *particles, const terrain_t *terrain, const water_t *water, const float eye[3],
	float density, float darkness, float frame_seconds)
{
	int active = 0;
	int wanted;

	if (particles == NULL || !particles->created || terrain == NULL)
	{
		return;
	}
	// Looked up again if the effects were reloaded
	if (fireflies->effect >= 0 && (fireflies->effect >= particles->effect_count
		|| SDL_strcmp(particles->effects[fireflies->effect].name, "firefly") != 0))
	{
		fireflies->effect = -1;
	}
	if (fireflies->effect == -1)
	{
		fireflies->effect = particles_find_effect(particles, "firefly");
		fireflies->effect = fireflies->effect >= 0 ? fireflies->effect : -2;
	}
	// Their time: dark, dry, and as many as asked
	wanted = fireflies->effect >= 0 && darkness >= NIGHT_DARKNESS && (water == NULL || !water->raining)
		? (int)SDL_lroundf(SDL_clamp(density, 0.0f, 1.0f) * (float)FIREFLY_SWARMS_MAX) : 0;
	// The swarms that are done, or left behind
	for (int i = 0; i < FIREFLY_SWARMS_MAX; i++)
	{
		firefly_swarm_t *swarm = &fireflies->swarms[i];
		float dx;
		float dz;

		if (!swarm->used)
		{
			continue;
		}
		swarm->seconds_left -= frame_seconds;
		dx = swarm->x - eye[0];
		dz = swarm->z - eye[2];
		if (swarm->seconds_left <= 0.0f || dx * dx + dz * dz > SWARM_GONE_METERS * SWARM_GONE_METERS
			|| particles_find_emitter(particles, swarm->emitter) == NULL)
		{
			particles_stop(particles, swarm->emitter);
			swarm->used = false;
			continue;
		}
		active++;
	}
	fireflies->next_change_seconds -= frame_seconds;
	if (fireflies->next_change_seconds > 0.0f || active == wanted)
	{
		return;
	}
	fireflies->next_change_seconds = CHANGE_SECONDS * (0.5f + random_unit(fireflies));
	if (active > wanted)
	{
		// Dawn (or fewer asked): one goes out
		for (int i = 0; i < FIREFLY_SWARMS_MAX; i++)
		{
			if (fireflies->swarms[i].used)
			{
				particles_stop(particles, fireflies->swarms[i].emitter);
				fireflies->swarms[i].used = false;
				break;
			}
		}
		return;
	}
	for (int i = 0; i < FIREFLY_SWARMS_MAX; i++)
	{
		firefly_swarm_t *swarm = &fireflies->swarms[i];
		float x;
		float z;

		if (swarm->used)
		{
			continue;
		}
		if (find_place(fireflies, terrain, water, eye, &x, &z))
		{
			float y = terrain_sample_height_meters(terrain, x, z) + 0.6f + random_unit(fireflies) * 0.9f;

			swarm->emitter = particles_play(particles, fireflies->effect, x, y, z);
			swarm->used = swarm->emitter != 0;
			swarm->seconds_left = SWARM_SECONDS_MINIMUM + random_unit(fireflies) * (SWARM_SECONDS_MAXIMUM - SWARM_SECONDS_MINIMUM);
			swarm->x = x;
			swarm->z = z;
		}
		break;
	}
}
