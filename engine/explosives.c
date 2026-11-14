// What blows up (explosives.h)

#include "explosives.h"

#include <SDL3/SDL.h>

#include "ini_parser.h"
#include "session.h"
#include "stamps.h"

// However light a thing the blast throws, no faster than this (a light
// body, a model made small: what a blast does to a matchbox is not shown)
static const float BLAST_FASTEST_METERS_PER_SECOND = 25.0f;

// A grenade whose body never came (no room in the physics): thrown as it is
static const float THROW_GIVE_UP_SECONDS = 1.0f;
// The flash and the push start a little above where it lies
static const float ABOVE_GROUND_METERS = 0.3f;
// Behind something (the ground, a wall, a trunk): this much of the damage
static const float SHELTERED_SHARE = 0.35f;
// A fire's hits come this often
static const float BURN_TICK_SECONDS = 0.5f;
// A proximity fuse: who is this far above or below it is not near
static const float PROXIMITY_HEIGHT_METERS = 2.0f;
// A buried mine: this much of it under the ground
static const float BURIED_SHARE = 0.7f;

typedef struct
{
	explosives_t *explosives;
	explosive_type_t *current;
} load_state_t;

static explosive_type_t default_type(void)
{
	explosive_type_t type;

	SDL_zero(type);
	type.fuse = EXPLOSIVE_FUSE_TIMER;
	type.fuse_seconds = 4.0f;
	type.arm_seconds = 2.0f;
	type.trigger_meters = 3.0f;
	type.radius_meters = 8.0f;
	type.damage = 200.0f;
	type.push_pascal_seconds = 150.0f;
	type.crater_meters = 1.2f;
	type.crater_depth_meters = 0.4f;
	light_source_reset(&type.light);
	type.light.color[0] = 1.0f;
	type.light.color[1] = 0.7f;
	type.light.color[2] = 0.35f;
	type.light.intensity = 30.0f;
	type.light.radius_meters = 30.0f;
	type.light.switched = false;
	type.light_seconds = 0.35f;
	type.throw_meters_per_second = 14.0f;
	SDL_strlcpy(type.model, "crate", sizeof(type.model));
	type.size_meters = 0.12f;
	type.mass_kilograms = 0.6f;
	type.damage_type = DAMAGE_EXPLOSIVE;
	type.noise_meters = 600.0f;
	type.burn_radius_meters = 3.0f;
	type.burn_damage_per_second = 10.0f;
	SDL_strlcpy(type.burn_effect, "campfire", sizeof(type.burn_effect));
	type.trigger_height_meters = 0.5f;
	return type;
}

static bool read_color(const char *value, float color[3])
{
	return SDL_sscanf(value, "%f , %f , %f", &color[0], &color[1], &color[2]) == 3;
}

static bool type_key(explosive_type_t *type, const char *key, const char *value)
{
	if (SDL_strcmp(key, "fuse") == 0)
	{
		type->fuse = SDL_strcmp(value, "proximity") == 0 ? EXPLOSIVE_FUSE_PROXIMITY : EXPLOSIVE_FUSE_TIMER;
		return SDL_strcmp(value, "proximity") == 0 || SDL_strcmp(value, "timer") == 0;
	}
	if (SDL_strcmp(key, "fuse_seconds") == 0)
	{
		return ini_value_to_float(value, &type->fuse_seconds);
	}
	if (SDL_strcmp(key, "arm_seconds") == 0)
	{
		return ini_value_to_float(value, &type->arm_seconds);
	}
	if (SDL_strcmp(key, "trigger_meters") == 0)
	{
		return ini_value_to_float(value, &type->trigger_meters);
	}
	if (SDL_strcmp(key, "radius_meters") == 0)
	{
		return ini_value_to_float(value, &type->radius_meters);
	}
	if (SDL_strcmp(key, "damage") == 0)
	{
		return ini_value_to_float(value, &type->damage);
	}
	if (SDL_strcmp(key, "push") == 0)
	{
		return ini_value_to_float(value, &type->push_pascal_seconds);
	}
	if (SDL_strcmp(key, "crater_meters") == 0)
	{
		return ini_value_to_float(value, &type->crater_meters);
	}
	if (SDL_strcmp(key, "crater_depth_meters") == 0)
	{
		return ini_value_to_float(value, &type->crater_depth_meters);
	}
	if (SDL_strcmp(key, "effect") == 0)
	{
		if (type->effect_count >= EXPLOSIVES_EFFECTS_MAX)
		{
			return false;
		}
		SDL_strlcpy(type->effects[type->effect_count++], value, EXPLOSIVES_NAME_LENGTH);
		return true;
	}
	if (SDL_strcmp(key, "light") == 0)
	{
		return read_color(value, type->light.color);
	}
	if (SDL_strcmp(key, "light_intensity") == 0)
	{
		return ini_value_to_float(value, &type->light.intensity);
	}
	if (SDL_strcmp(key, "light_radius") == 0)
	{
		return ini_value_to_float(value, &type->light.radius_meters);
	}
	if (SDL_strcmp(key, "light_seconds") == 0)
	{
		return ini_value_to_float(value, &type->light_seconds);
	}
	if (SDL_strcmp(key, "throw_speed") == 0)
	{
		return ini_value_to_float(value, &type->throw_meters_per_second);
	}
	if (SDL_strcmp(key, "model") == 0)
	{
		SDL_strlcpy(type->model, value, sizeof(type->model));
		return true;
	}
	if (SDL_strcmp(key, "size") == 0)
	{
		return ini_value_to_float(value, &type->size_meters);
	}
	if (SDL_strcmp(key, "mass") == 0)
	{
		return ini_value_to_float(value, &type->mass_kilograms);
	}
	if (SDL_strcmp(key, "damage_type") == 0)
	{
		return damage_type_from_name(value, &type->damage_type);
	}
	if (SDL_strcmp(key, "noise_meters") == 0)
	{
		return ini_value_to_float(value, &type->noise_meters);
	}
	if (SDL_strcmp(key, "burn_seconds") == 0)
	{
		return ini_value_to_float(value, &type->burn_seconds);
	}
	if (SDL_strcmp(key, "burn_radius_meters") == 0)
	{
		return ini_value_to_float(value, &type->burn_radius_meters);
	}
	if (SDL_strcmp(key, "burn_damage_per_second") == 0)
	{
		return ini_value_to_float(value, &type->burn_damage_per_second);
	}
	if (SDL_strcmp(key, "burn_effect") == 0)
	{
		SDL_strlcpy(type->burn_effect, value, sizeof(type->burn_effect));
		return true;
	}
	if (SDL_strcmp(key, "buried") == 0)
	{
		return ini_value_to_bool(value, &type->buried);
	}
	if (SDL_strcmp(key, "pressure") == 0)
	{
		return ini_value_to_bool(value, &type->pressure);
	}
	if (SDL_strcmp(key, "trigger_height_meters") == 0)
	{
		return ini_value_to_float(value, &type->trigger_height_meters);
	}
	if (SDL_strcmp(key, "spares_owner") == 0)
	{
		return ini_value_to_bool(value, &type->spares_owner);
	}
	return false;
}

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;
	explosives_t *explosives = state->explosives;

	if (state->current == NULL || SDL_strcmp(state->current->name, section) != 0)
	{
		if (explosives->type_count >= EXPLOSIVES_TYPES_MAX)
		{
			return false;
		}
		state->current = &explosives->types[explosives->type_count++];
		*state->current = default_type();
		SDL_strlcpy(state->current->name, section, sizeof(state->current->name));
	}
	return type_key(state->current, key, value);
}

void explosives_load(explosives_t *explosives)
{
	load_state_t state = { explosives, NULL };

	SDL_zerop(explosives);
	if (!ini_parse_data_file("explosives.ini", load_key, &state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Explosives: no data/explosives.ini");
	}
}

int explosives_find(const explosives_t *explosives, const char *name)
{
	for (int i = 0; name != NULL && i < explosives->type_count; i++)
	{
		if (SDL_strcmp(explosives->types[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

// Whether something stands between two points: the ground, or anything
// with a body (a wall, a trunk, a rock)
static bool blocked(world_t *world, const float from[3], const float to[3])
{
	vec3_t origin = vec3_make(from[0], from[1], from[2]);
	vec3_t way = vec3_make(to[0] - from[0], to[1] - from[1], to[2] - from[2]);
	float length = vec3_length(way);
	physics_cast_t cast;
	vec3_t hit;

	if (length < 0.01f)
	{
		return false;
	}
	if (terrain_raycast(&world->terrain, origin, vec3_scale(way, 1.0f / length), length - 0.2f, &hit))
	{
		return true;
	}
	return physics_cast_segment(&world->physics, from, to, &cast) && cast.fraction < 0.95f;
}

void explosives_explode(explosives_t *explosives, session_t *session, world_t *world, int type_index, const float position[3], int owner)
{
	const explosive_type_t *type;
	float ground;
	float center[3];

	if (type_index < 0 || type_index >= explosives->type_count)
	{
		return;
	}
	type = &explosives->types[type_index];
	ground = terrain_sample_height_meters(&world->terrain, position[0], position[2]);
	center[0] = position[0];
	center[1] = SDL_max(position[1], ground) + ABOVE_GROUND_METERS;
	center[2] = position[2];
	// The flash, the fire and the smoke
	if (light_source_gives_light(&type->light))
	{
		lights_flash(&world->lights, center, &type->light, type->light_seconds, world->elapsed_seconds);
	}
	for (int e = 0; e < type->effect_count; e++)
	{
		int effect = particles_find_effect(&world->particles, type->effects[e]);

		if (effect >= 0)
		{
			particles_play(&world->particles, effect, center[0], center[1], center[2]);
		}
	}
	// The ground scorched round it, if it went off near the ground (the
	// crater dug below stays inside the scorch's box: it reaches a meter
	// down)
	if (position[1] - ground < 2.0f)
	{
		float point[3] = { position[0], ground, position[2] };
		float up[3] = { 0.0f, 1.0f, 0.0f };
		float size = SDL_max(type->crater_meters * 3.0f, type->radius_meters * 0.5f);

		decals_add(&world->decals, decals_find_kind(&world->decals, "scorch"), point, up, size, world->elapsed_seconds);
	}
	// A crater, if it went off near the ground (not on a baked map)
	if (type->crater_meters > 0.0f && !world->terrain.baked && position[1] - ground < type->crater_meters)
	{
		stamp_t stamp;

		SDL_zero(stamp);
		stamp.type = STAMP_CRATER;
		stamp.soil = SOIL_EARTH;
		stamp.radius_meters = type->crater_meters;
		stamp.height_meters = type->crater_depth_meters;
		stamp.start = vec3_make(position[0], ground, position[2]);
		stamp.end = stamp.start;
		world_touch_water(world);
		stamps_apply(&world->terrain, &world->water, &stamp);
	}
	// The loose things round it pushed away (and a little up)
	for (int b = 0; b < world->physics.body_capacity; b++)
	{
		const physics_body_t *body = &world->physics.bodies[b];
		const object_instance_t *instance;
		float point[3];
		float away[3];
		float distance;
		float push;

		if (!body->used || body->kind != PHYSICS_BODY_DYNAMIC || body->instance < 0 || body->instance >= world->objects.instance_count)
		{
			continue;
		}
		instance = &world->objects.instances[body->instance];
		point[0] = instance->x;
		point[1] = objects_instance_base_y(instance) + 0.2f;
		point[2] = instance->z;
		away[0] = point[0] - center[0];
		away[1] = point[1] - center[1];
		away[2] = point[2] - center[2];
		distance = SDL_sqrtf(away[0] * away[0] + away[1] * away[1] + away[2] * away[2]);
		if (distance >= type->radius_meters)
		{
			continue;
		}
		{
			// What it gets: the blast's impulse per square meter there (as
			// 1 / distance, gone at the radius) times the face it shows the
			// blast; no faster than a thrown stone however light it is
			float radius = world->objects.models[world->objects.types.types[instance->type].model].horizontal_radius * body->scale;
			float face = MATH3D_PI * radius * radius;
			float impulse = type->push_pascal_seconds * face * (1.0f - distance / type->radius_meters) / SDL_max(distance, 0.5f);
			float speed = impulse / SDL_max(body->mass, 0.01f);

			if (speed > BLAST_FASTEST_METERS_PER_SECOND)
			{
				impulse *= BLAST_FASTEST_METERS_PER_SECOND / speed;
			}
			push = impulse / SDL_max(distance, 1e-3f);
			// (away, and a little up: the blast lifts off the ground)
			away[0] *= push;
			away[1] = away[1] * push + 0.4f * impulse;
			away[2] *= push;
			physics_push_body(&world->physics, b, away, point);
		}
	}
	// Who was caught in it: the actors, the player (the game decides what
	// it does)
	for (int a = 0; a < world->actors.count; a++)
	{
		const actor_t *actor = &world->actors.actors[a];
		projectile_hit_t hit;
		float target[3];
		float distance;

		if (actors_is_dead(&world->actors, actor->id))
		{
			continue;
		}
		target[0] = actor->x;
		target[1] = actor->y + 0.6f * actor->scale;
		target[2] = actor->z;
		distance = SDL_sqrtf((target[0] - center[0]) * (target[0] - center[0]) + (target[1] - center[1]) * (target[1] - center[1])
			+ (target[2] - center[2]) * (target[2] - center[2]));
		if (distance >= type->radius_meters)
		{
			continue;
		}
		SDL_zero(hit);
		hit.kind = PROJECTILE_HIT_ACTOR;
		SDL_memcpy(hit.point, target, sizeof(hit.point));
		hit.instance = -1;
		hit.actor_id = actor->id;
		hit.zone = ACTOR_ZONE_BODY;
		hit.bone = -1;
		hit.damage = type->damage * (1.0f - distance / type->radius_meters) * (blocked(world, center, target) ? SHELTERED_SHARE : 1.0f);
		hit.damage_type = type->damage_type;
		hit.ammo = -1;
		hit.owner = owner;
		projectiles_add_hit(&session->projectiles, &hit);
	}
	if (explosives->player_known)
	{
		const float *player = explosives->player;
		float distance = SDL_sqrtf((player[0] - center[0]) * (player[0] - center[0]) + (player[1] - center[1]) * (player[1] - center[1])
			+ (player[2] - center[2]) * (player[2] - center[2]));

		if (distance < type->radius_meters)
		{
			projectile_hit_t hit;

			SDL_zero(hit);
			hit.kind = PROJECTILE_HIT_PLAYER;
			SDL_memcpy(hit.point, player, sizeof(hit.point));
			hit.instance = -1;
			hit.bone = -1;
			hit.damage = type->damage * (1.0f - distance / type->radius_meters) * (blocked(world, center, player) ? SHELTERED_SHARE : 1.0f);
			hit.damage_type = type->damage_type;
			hit.ammo = -1;
			hit.owner = owner;
			projectiles_add_hit(&session->projectiles, &hit);
		}
	}
	// A fire left burning there
	if (type->burn_seconds > 0.0f)
	{
		burn_t *burn = NULL;

		for (int b = 0; b < EXPLOSIVES_BURNS_MAX && burn == NULL; b++)
		{
			burn = explosives->burns[b].used ? NULL : &explosives->burns[b];
		}
		if (burn != NULL)
		{
			int effect = particles_find_effect(&world->particles, type->burn_effect);

			SDL_zerop(burn);
			burn->used = true;
			burn->position[0] = center[0];
			burn->position[1] = ground;
			burn->position[2] = center[2];
			burn->radius_meters = type->burn_radius_meters;
			burn->seconds_left = type->burn_seconds;
			burn->damage_per_second = type->burn_damage_per_second;
			burn->damage_type = type->damage_type;
			burn->owner = owner;
			burn->emitter = effect >= 0 ? particles_play(&world->particles, effect, center[0], ground, center[2]) : 0;
		}
	}
	noises_make(&world->noises, center, type->noise_meters, NOISE_EXPLOSION, owner >= 0 ? (uint32_t)owner : 0, world->elapsed_seconds);
	SDL_Log("Explosion: %s at (%.1f, %.1f, %.1f)", type->name, (double)center[0], (double)center[1], (double)center[2]);
}

// A fire's hits this tick: whoever is within its reach
static void burn_hits(explosives_t *explosives, session_t *session, world_t *world, const burn_t *burn)
{
	float damage = burn->damage_per_second * BURN_TICK_SECONDS;

	for (int a = 0; a < world->actors.count; a++)
	{
		const actor_t *actor = &world->actors.actors[a];
		float dx = actor->x - burn->position[0];
		float dz = actor->z - burn->position[2];
		projectile_hit_t hit;

		if (dx * dx + dz * dz >= burn->radius_meters * burn->radius_meters || SDL_fabsf(actor->y - burn->position[1]) > 2.0f
			|| actors_is_dead(&world->actors, actor->id))
		{
			continue;
		}
		SDL_zero(hit);
		hit.kind = PROJECTILE_HIT_ACTOR;
		hit.point[0] = actor->x;
		hit.point[1] = actor->y;
		hit.point[2] = actor->z;
		hit.instance = -1;
		hit.actor_id = actor->id;
		hit.zone = ACTOR_ZONE_LIMB;
		hit.bone = -1;
		hit.damage = damage;
		hit.damage_type = burn->damage_type;
		hit.ammo = -1;
		hit.owner = burn->owner;
		projectiles_add_hit(&session->projectiles, &hit);
	}
	if (explosives->player_known)
	{
		float dx = explosives->player[0] - burn->position[0];
		float dz = explosives->player[2] - burn->position[2];

		if (dx * dx + dz * dz < burn->radius_meters * burn->radius_meters && SDL_fabsf(explosives->player[1] - burn->position[1]) < 2.5f)
		{
			projectile_hit_t hit;

			SDL_zero(hit);
			hit.kind = PROJECTILE_HIT_PLAYER;
			SDL_memcpy(hit.point, explosives->player, sizeof(hit.point));
			hit.instance = -1;
			hit.bone = -1;
			hit.damage = damage;
			hit.damage_type = burn->damage_type;
			hit.ammo = -1;
			hit.owner = burn->owner;
			projectiles_add_hit(&session->projectiles, &hit);
		}
	}
}

bool explosives_throw(explosives_t *explosives, world_t *world, int type_index, const float from[3], const float direction[3], int owner)
{
	const explosive_type_t *type;
	objects_t *objects = &world->objects;
	int model_type;
	int slot = -1;
	float scale;
	object_instance_t *instance;

	if (type_index < 0 || type_index >= explosives->type_count)
	{
		return false;
	}
	type = &explosives->types[type_index];
	model_type = object_types_find(&objects->types, type->model);
	for (int i = 0; i < EXPLOSIVES_GRENADES_MAX && slot < 0; i++)
	{
		slot = explosives->grenades[i].used ? -1 : i;
	}
	if (model_type < 0 || objects->types.types[model_type].model < 0 || slot < 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Explosives: %s cannot be thrown (no objects.ini type \"%s\", or too many in the air)", type->name,
			type->model);
		return false;
	}
	scale = type->size_meters / SDL_max(2.0f * objects->models[objects->types.types[model_type].model].horizontal_radius, 0.01f);
	if (!objects_add(objects, &world->terrain, model_type, from[0], from[2], 0.0f, scale, 0))
	{
		return false;
	}
	// Loose and awake, in the hand (its body is made at the next step)
	instance = &objects->instances[objects->instance_count - 1];
	instance->state |= OBJECT_STATE_FREE;
	instance->y = from[1];
	instance->body = -2;
	SDL_zero(explosives->grenades[slot]);
	explosives->grenades[slot].used = true;
	explosives->grenades[slot].type = type_index;
	explosives->grenades[slot].object_id = instance->id;
	explosives->grenades[slot].owner = owner;
	for (int k = 0; k < 3; k++)
	{
		explosives->grenades[slot].velocity[k] = direction[k] * type->throw_meters_per_second;
	}
	return true;
}

static void remove_object(world_t *world, int index)
{
	// (blown away: gone where it stood, no sort of the list for it)
	objects_kill(&world->objects, index);
}

// Whether an actor (alive) or the player is within meters of a point; a
// pressure fuse: a walker on the ground, tall enough (not a bird); its
// owner spared if its type says (owner -1: the player; else an actor's id)
static bool anyone_near(const explosives_t *explosives, const world_t *world, const explosive_type_t *type, int owner, const float point[3],
	float meters)
{
	for (int a = 0; a < world->actors.count; a++)
	{
		const actor_t *actor = &world->actors.actors[a];
		const actor_type_t *actor_type = &world->actors.types[actor->type];
		float dx = actor->x - point[0];
		float dz = actor->z - point[2];

		// (near along the ground; not far above or below it)
		if (dx * dx + dz * dz >= meters * meters || SDL_fabsf(actor->y - point[1]) > PROXIMITY_HEIGHT_METERS
			|| actors_is_dead(&world->actors, actor->id))
		{
			continue;
		}
		if (type->spares_owner && owner >= 0 && (uint32_t)owner == actor->id)
		{
			continue;
		}
		if (type->pressure && (actor_type->flies || actor_type->height_meters * actor->scale < type->trigger_height_meters))
		{
			continue;
		}
		return true;
	}
	if (type->spares_owner && owner == -1)
	{
		return false;
	}
	if (explosives->player_known)
	{
		float dx = explosives->player[0] - point[0];
		float dz = explosives->player[2] - point[2];

		return dx * dx + dz * dz < meters * meters && SDL_fabsf(explosives->player[1] - point[1]) < PROXIMITY_HEIGHT_METERS;
	}
	return false;
}

void explosives_update(explosives_t *explosives, session_t *session, world_t *world, const float player[3], float seconds)
{
	if (player != NULL)
	{
		SDL_memcpy(explosives->player, player, sizeof(explosives->player));
		explosives->player_known = true;
	}
	// The fires: their hits, till they go out
	for (int b = 0; b < EXPLOSIVES_BURNS_MAX; b++)
	{
		burn_t *burn = &explosives->burns[b];

		if (!burn->used)
		{
			continue;
		}
		burn->seconds_left -= seconds;
		burn->tick_seconds -= seconds;
		if (burn->tick_seconds <= 0.0f)
		{
			burn->tick_seconds += BURN_TICK_SECONDS;
			burn_hits(explosives, session, world, burn);
		}
		if (burn->seconds_left <= 0.0f)
		{
			if (burn->emitter != 0)
			{
				particles_stop(&world->particles, burn->emitter);
			}
			burn->used = false;
		}
	}
	for (int g = 0; g < EXPLOSIVES_GRENADES_MAX; g++)
	{
		grenade_t *grenade = &explosives->grenades[g];
		const explosive_type_t *type;
		int index;
		object_instance_t *instance;
		float position[3];
		bool goes_off;

		if (!grenade->used)
		{
			continue;
		}
		index = objects_find_id(&world->objects, grenade->object_id);
		if (index < 0)
		{
			// (taken away: a map loaded, cleared, or it flew off the map)
			SDL_Log("Explosives: a %s grenade is no longer on the map, forgotten", explosives->types[grenade->type].name);
			grenade->used = false;
			continue;
		}
		instance = &world->objects.instances[index];
		type = &explosives->types[grenade->type];
		grenade->age_seconds += seconds;
		// Its own weight, before anything pushes it
		if (!grenade->weighed && !grenade->placed && instance->body >= 0 && instance->body < world->physics.body_capacity)
		{
			physics_set_body_mass(&world->physics, instance->body, type->mass_kilograms);
			grenade->weighed = true;
		}
		// The throw, once its body is there (a mine: none)
		if (grenade->placed)
		{
			grenade->thrown = true;
		}
		else if (!grenade->thrown && instance->body >= 0 && instance->body < world->physics.body_capacity)
		{
			float mass = world->physics.bodies[instance->body].mass;
			float impulse[3] = { grenade->velocity[0] * mass, grenade->velocity[1] * mass, grenade->velocity[2] * mass };
			float point[3] = { instance->x, objects_instance_base_y(instance), instance->z };

			physics_push_body(&world->physics, instance->body, impulse, point);
			grenade->thrown = true;
		}
		else if (!grenade->thrown && grenade->age_seconds > THROW_GIVE_UP_SECONDS)
		{
			grenade->thrown = true;
		}
		position[0] = instance->x;
		position[1] = objects_instance_base_y(instance);
		position[2] = instance->z;
		goes_off = type->fuse == EXPLOSIVE_FUSE_TIMER ? grenade->age_seconds >= type->fuse_seconds
			: grenade->age_seconds >= type->arm_seconds && anyone_near(explosives, world, type, grenade->owner, position, type->trigger_meters);
		if (goes_off)
		{
			int owner = grenade->owner;
			int kind = grenade->type;

			grenade->used = false;
			remove_object(world, index);
			explosives_explode(explosives, session, world, kind, position, owner);
		}
	}
}

bool explosives_set_off(explosives_t *explosives, session_t *session, world_t *world, uint32_t object_id)
{
	if (object_id == 0)
	{
		return false;
	}
	for (int g = 0; g < EXPLOSIVES_GRENADES_MAX; g++)
	{
		grenade_t *grenade = &explosives->grenades[g];
		int index;
		float position[3];
		int owner;
		int kind;

		if (!grenade->used || grenade->object_id != object_id)
		{
			continue;
		}
		index = objects_find_id(&world->objects, object_id);
		grenade->used = false;
		if (index < 0)
		{
			return false;
		}
		position[0] = world->objects.instances[index].x;
		position[1] = objects_instance_base_y(&world->objects.instances[index]);
		position[2] = world->objects.instances[index].z;
		owner = grenade->owner;
		kind = grenade->type;
		remove_object(world, index);
		explosives_explode(explosives, session, world, kind, position, owner);
		return true;
	}
	return false;
}

uint32_t explosives_place(explosives_t *explosives, world_t *world, int type_index, float x, float z, float yaw_radians, int owner)
{
	const explosive_type_t *type;
	objects_t *objects = &world->objects;
	int model_type;
	int slot = -1;
	float scale;
	object_instance_t *instance;

	if (type_index < 0 || type_index >= explosives->type_count)
	{
		return 0;
	}
	type = &explosives->types[type_index];
	model_type = object_types_find(&objects->types, type->model);
	for (int i = 0; i < EXPLOSIVES_GRENADES_MAX && slot < 0; i++)
	{
		slot = explosives->grenades[i].used ? -1 : i;
	}
	if (model_type < 0 || objects->types.types[model_type].model < 0 || slot < 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Explosives: %s cannot be put down (no objects.ini type \"%s\", or too many)", type->name, type->model);
		return 0;
	}
	scale = type->size_meters / SDL_max(2.0f * objects->models[objects->types.types[model_type].model].horizontal_radius, 0.01f);
	// Standing where it is put (asleep: what pushes it moves it); buried,
	// most of it under the ground
	if (!objects_add_lifted(objects, &world->terrain, model_type, x, z, yaw_radians, scale, 0,
		type->buried ? -BURIED_SHARE * type->size_meters : 0.0f))
	{
		return 0;
	}
	instance = &objects->instances[objects->instance_count - 1];
	SDL_zero(explosives->grenades[slot]);
	explosives->grenades[slot].used = true;
	explosives->grenades[slot].type = type_index;
	explosives->grenades[slot].object_id = instance->id;
	explosives->grenades[slot].owner = owner;
	explosives->grenades[slot].placed = true;
	explosives->grenades[slot].thrown = true;
	return instance->id;
}

static grenade_t *grenade_of(explosives_t *explosives, uint32_t object_id)
{
	for (int g = 0; g < EXPLOSIVES_GRENADES_MAX && object_id != 0; g++)
	{
		if (explosives->grenades[g].used && explosives->grenades[g].object_id == object_id)
		{
			return &explosives->grenades[g];
		}
	}
	return NULL;
}

bool explosives_find_object(const explosives_t *explosives, uint32_t object_id, int *type_out, bool *armed_out, int *owner_out)
{
	const grenade_t *grenade = grenade_of((explosives_t *)explosives, object_id);
	const explosive_type_t *type;

	if (grenade == NULL)
	{
		return false;
	}
	type = &explosives->types[grenade->type];
	*type_out = grenade->type;
	*armed_out = type->fuse == EXPLOSIVE_FUSE_PROXIMITY && grenade->age_seconds >= type->arm_seconds;
	*owner_out = grenade->owner;
	return true;
}

int explosives_take_back(explosives_t *explosives, world_t *world, uint32_t object_id)
{
	grenade_t *grenade = grenade_of(explosives, object_id);
	int index = objects_find_id(&world->objects, object_id);
	int type;

	if (grenade == NULL)
	{
		return -1;
	}
	type = grenade->type;
	grenade->used = false;
	if (index >= 0)
	{
		remove_object(world, index);
	}
	return type;
}

void explosives_forget(explosives_t *explosives)
{
	for (int g = 0; g < EXPLOSIVES_GRENADES_MAX; g++)
	{
		explosives->grenades[g].used = false;
	}
	// (the fires' effects went with the old map's particles)
	for (int b = 0; b < EXPLOSIVES_BURNS_MAX; b++)
	{
		explosives->burns[b].used = false;
	}
}
