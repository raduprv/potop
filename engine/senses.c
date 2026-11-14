// What an actor sees. See senses.h.

#include "senses.h"

#include <SDL3/SDL.h>

#include "light_probe.h"

// In the dark it still sees this much of its sight (a shape against the
// sky, something moving close by)
static const float DARK_SIGHT_SHARE = 0.15f;
// Its eyes this high on it (of its height)
static const float EYES_SHARE = 0.8f;

// Whether a point is within its view to the sides (its sight's width,
// facing yaw: 0 = +z); squares only
static bool within_view(float yaw_radians, float sight_degrees, float dx, float dz)
{
	float half = 0.5f * sight_degrees * MATH3D_PI / 180.0f;
	float cosine = SDL_cosf(half);
	float along = SDL_sinf(yaw_radians) * dx + SDL_cosf(yaw_radians) * dz;
	float squared = dx * dx + dz * dz;

	if (sight_degrees >= 360.0f)
	{
		return true;
	}
	// along >= cosine * length, without the length
	if (cosine >= 0.0f)
	{
		return along >= 0.0f && along * along >= cosine * cosine * squared;
	}
	return along >= 0.0f || along * along <= cosine * cosine * squared;
}

float senses_sight(world_t *world, int actor_index, const float target[3], float target_visibility)
{
	const actor_t *actor;
	const actor_type_t *type;
	float eye[3];
	float dx;
	float dy;
	float dz;
	float distance_squared;
	float reach;
	light_probe_t light;

	if (actor_index < 0 || actor_index >= world->actors.count || target_visibility <= 0.0f)
	{
		return 0.0f;
	}
	actor = &world->actors.actors[actor_index];
	type = &world->actors.types[actor->type];
	eye[0] = actor->x;
	eye[1] = actor->y + EYES_SHARE * type->height_meters * actor->scale;
	eye[2] = actor->z;
	dx = target[0] - eye[0];
	dy = target[1] - eye[1];
	dz = target[2] - eye[2];
	distance_squared = dx * dx + dy * dy + dz * dz;
	// Out of its reach even in full light, or not where it looks
	reach = type->sight_meters * target_visibility;
	if (distance_squared >= reach * reach || !within_view(actor->yaw_radians, type->sight_degrees, dx, dz))
	{
		return 0.0f;
	}
	// In the dark, nearer
	light_probe_at(world, target[0], target[1], target[2], &light);
	reach *= DARK_SIGHT_SHARE + (1.0f - DARK_SIGHT_SHARE) * light.total;
	if (distance_squared >= reach * reach)
	{
		return 0.0f;
	}
	// The ground, or something solid, between
	{
		float distance = SDL_sqrtf(distance_squared);
		vec3_t hit;
		physics_cast_t cast;

		if (distance > 0.01f && terrain_raycast(&world->terrain, vec3_make(eye[0], eye[1], eye[2]), vec3_make(dx / distance, dy / distance, dz / distance),
			distance - 0.2f, &hit))
		{
			return 0.0f;
		}
		if (physics_cast_segment(&world->physics, eye, target, &cast) && cast.fraction < 0.97f)
		{
			return 0.0f;
		}
	}
	return 1.0f - distance_squared / (reach * reach);
}
