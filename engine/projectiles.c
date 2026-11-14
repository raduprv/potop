// Projectiles. See projectiles.h.

#include "projectiles.h"

#include <SDL3/SDL.h>

#include "ini_parser.h"
#include "material_map.h"
#include "materials.h"

static const float GRAVITY = 9.81f;
// How much thicker the water is than the air, for the drag
static const float WATER_DRAG_FACTOR = 600.0f;
// Slower than this in the water: it stops (it sinks, harmless)
static const float WATER_STOP_SPEED = 25.0f;
// Steps no longer than this (seconds): the drag is steep at the start
static const float LONGEST_STEP = 1.0f / 120.0f;

typedef struct
{
	projectiles_t *projectiles;
	const particles_t *particles;
} load_state_t;

static impact_kind_t *impact_named(projectiles_t *projectiles, const char *name)
{
	for (int i = 0; i < projectiles->impact_count; i++)
	{
		if (SDL_strcmp(projectiles->impacts[i].name, name) == 0)
		{
			return &projectiles->impacts[i];
		}
	}
	if (projectiles->impact_count >= IMPACT_KINDS_MAX)
	{
		return NULL;
	}
	{
		impact_kind_t *kind = &projectiles->impacts[projectiles->impact_count++];

		SDL_zerop(kind);
		SDL_strlcpy(kind->name, name, sizeof(kind->name));
		kind->effect = -1;
		kind->spray = 2.0f;
		return kind;
	}
}

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;
	impact_kind_t *kind;

	if (section == NULL || section[0] == '\0' || (kind = impact_named(state->projectiles, section)) == NULL)
	{
		return false;
	}
	if (SDL_strcmp(key, "effect") == 0)
	{
		kind->effect = particles_find_effect(state->particles, value);
		if (kind->effect < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Impacts: [%s] plays \"%s\", which particles.ini does not have", section, value);
		}
		return true;
	}
	if (SDL_strcmp(key, "spray") == 0)
	{
		return ini_value_to_float(value, &kind->spray);
	}
	if (SDL_strcmp(key, "materials") == 0)
	{
		SDL_strlcpy(kind->materials, value, sizeof(kind->materials));
		return true;
	}
	if (SDL_strcmp(key, "categories") == 0)
	{
		SDL_strlcpy(kind->categories, value, sizeof(kind->categories));
		return true;
	}
	if (SDL_strcmp(key, "water") == 0)
	{
		return ini_value_to_bool(value, &kind->water);
	}
	if (SDL_strcmp(key, "actors") == 0)
	{
		return ini_value_to_bool(value, &kind->actors);
	}
	if (SDL_strcmp(key, "fallback_ground") == 0)
	{
		return ini_value_to_bool(value, &kind->fallback_ground);
	}
	if (SDL_strcmp(key, "fallback_object") == 0)
	{
		return ini_value_to_bool(value, &kind->fallback_object);
	}
	if (SDL_strcmp(key, "decal") == 0)
	{
		SDL_strlcpy(kind->decal, value, sizeof(kind->decal));
		return true;
	}
	if (SDL_strcmp(key, "decal_size") == 0)
	{
		return ini_value_to_float(value, &kind->decal_size);
	}
	return false;
}

void projectiles_create(projectiles_t *projectiles, const particles_t *particles)
{
	load_state_t state = { projectiles, particles };

	SDL_zerop(projectiles);
	if (!ini_parse_data_file("impacts.ini", load_key, &state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Projectiles: no data/impacts.ini (hits show nothing)");
	}
}

void projectiles_clear(projectiles_t *projectiles)
{
	for (int i = 0; i < PROJECTILES_MAX; i++)
	{
		projectiles->list[i].used = false;
	}
	projectiles->hit_count = 0;
}

projectile_t *projectiles_fire(projectiles_t *projectiles, particles_t *particles, const float origin[3], const float direction[3], float speed,
	float mass_kilograms, float drag, float push, float damage, float life_seconds, int owner, const char *tracer_effect,
	const float tracer_origin[3])
{
	projectile_t *projectile = NULL;

	for (int tries = 0; tries < PROJECTILES_MAX; tries++)
	{
		projectile_t *candidate = &projectiles->list[projectiles->next];

		projectiles->next = (projectiles->next + 1) % PROJECTILES_MAX;
		if (!candidate->used)
		{
			projectile = candidate;
			break;
		}
	}
	if (projectile == NULL)
	{
		// (all of them flying: the oldest makes room)
		projectile = &projectiles->list[projectiles->next];
		projectiles->next = (projectiles->next + 1) % PROJECTILES_MAX;
	}
	SDL_zerop(projectile);
	projectile->used = true;
	for (int k = 0; k < 3; k++)
	{
		projectile->position[k] = origin[k];
		projectile->velocity[k] = direction[k] * speed;
	}
	projectile->mass_kilograms = mass_kilograms;
	projectile->drag = drag;
	projectile->push = push;
	projectile->damage = damage;
	projectile->muzzle_speed = SDL_max(speed, 1.0f);
	projectile->life_seconds = life_seconds;
	projectile->owner = owner;
	projectile->ammo = -1;
	if (tracer_effect != NULL && tracer_effect[0] != '\0' && particles != NULL)
	{
		int effect = particles_find_effect(particles, tracer_effect);

		if (effect >= 0)
		{
			// (a single streak flying off at the bullet's speed, falling as
			// it falls, a hand's breadth beside its way: near enough the
			// bullet's own for the eye)
			float start[3];
			uint32_t id;

			for (int k = 0; k < 3; k++)
			{
				start[k] = tracer_origin != NULL ? tracer_origin[k] : origin[k] + direction[k] * 2.0f;
			}
			id = particles_play(particles, effect, start[0], start[1], start[2]);
			particles_emitter_t *emitter = particles_find_emitter(particles, id);

			if (emitter != NULL)
			{
				for (int k = 0; k < 3; k++)
				{
					emitter->push[k] = projectile->velocity[k];
				}
				projectile->tracer = id;
			}
		}
	}
	return projectile;
}

static bool name_in_list(const char *list, const char *name)
{
	char copy[128];
	char *items[16];
	int count;

	SDL_strlcpy(copy, list, sizeof(copy));
	count = ini_split_list(copy, items, 16);
	for (int i = 0; i < count; i++)
	{
		if (SDL_strcasecmp(items[i], name) == 0)
		{
			return true;
		}
	}
	return false;
}

static const impact_kind_t *impact_for_ground(const projectiles_t *projectiles, const terrain_t *terrain, float x, float z)
{
	const impact_kind_t *fallback = NULL;
	const char *material = NULL;

	if (terrain->materials != NULL)
	{
		int32_t cell_x = SDL_clamp((int32_t)(x / terrain->cell_size_meters + 0.5f), 0, terrain->size - 1);
		int32_t cell_z = SDL_clamp((int32_t)(z / terrain->cell_size_meters + 0.5f), 0, terrain->size - 1);
		size_t index = (size_t)cell_z * (size_t)terrain->size + (size_t)cell_x;

		material = materials_name((material_type_t)terrain->materials[index * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_BASE]);
	}
	for (int i = 0; i < projectiles->impact_count; i++)
	{
		const impact_kind_t *kind = &projectiles->impacts[i];

		if (material != NULL && kind->materials[0] != '\0' && name_in_list(kind->materials, material))
		{
			return kind;
		}
		if (kind->fallback_ground && fallback == NULL)
		{
			fallback = kind;
		}
	}
	return fallback;
}

static const impact_kind_t *impact_for_object(const projectiles_t *projectiles, const objects_t *objects, int instance)
{
	const impact_kind_t *fallback = NULL;
	const char *category = NULL;

	if (objects != NULL && instance >= 0 && instance < objects->instance_count)
	{
		int type = objects->instances[instance].type;

		if (type >= 0 && type < objects->types.type_count)
		{
			int index = objects->types.types[type].category;

			if (index >= 0 && index < objects->types.category_count)
			{
				category = objects->types.categories[index];
			}
		}
	}
	for (int i = 0; i < projectiles->impact_count; i++)
	{
		const impact_kind_t *kind = &projectiles->impacts[i];

		if (category != NULL && kind->categories[0] != '\0' && name_in_list(kind->categories, category))
		{
			return kind;
		}
		if (kind->fallback_object && fallback == NULL)
		{
			fallback = kind;
		}
	}
	return fallback;
}

static const impact_kind_t *impact_for_water(const projectiles_t *projectiles)
{
	for (int i = 0; i < projectiles->impact_count; i++)
	{
		if (projectiles->impacts[i].water)
		{
			return &projectiles->impacts[i];
		}
	}
	return NULL;
}

static const impact_kind_t *impact_for_actor(const projectiles_t *projectiles)
{
	for (int i = 0; i < projectiles->impact_count; i++)
	{
		if (projectiles->impacts[i].actors)
		{
			return &projectiles->impacts[i];
		}
	}
	return NULL;
}

// The mark a hit leaves, if its kind leaves one (only on what stays put: a
// decal stays where it was laid)
static void leave_mark(const projectiles_world_t *world, const impact_kind_t *kind, const float point[3], const float normal[3])
{
	if (kind == NULL || kind->decal[0] == '\0' || world->decals == NULL)
	{
		return;
	}
	decals_add(world->decals, decals_find_kind(world->decals, kind->decal), point, normal, kind->decal_size, world->now);
}

static void play_impact(particles_t *particles, const impact_kind_t *kind, const float point[3], const float normal[3])
{
	uint32_t id;
	particles_emitter_t *emitter;

	if (kind == NULL || kind->effect < 0 || particles == NULL)
	{
		return;
	}
	id = particles_play(particles, kind->effect, point[0] + normal[0] * 0.03f, point[1] + normal[1] * 0.03f, point[2] + normal[2] * 0.03f);
	emitter = particles_find_emitter(particles, id);
	if (emitter != NULL)
	{
		for (int k = 0; k < 3; k++)
		{
			emitter->push[k] = normal[k] * kind->spray;
		}
	}
}

static void keep_hit(projectiles_t *projectiles, const projectile_t *projectile, projectile_hit_kind_t kind, const float point[3],
	const float normal[3], int instance)
{
	projectile_hit_t *hit;
	float speed_squared = projectile->velocity[0] * projectile->velocity[0] + projectile->velocity[1] * projectile->velocity[1]
		+ projectile->velocity[2] * projectile->velocity[2];

	if (projectiles->hit_count >= PROJECTILE_HITS_MAX)
	{
		SDL_memmove(&projectiles->hits[0], &projectiles->hits[1], sizeof(projectile_hit_t) * (PROJECTILE_HITS_MAX - 1));
		projectiles->hit_count = PROJECTILE_HITS_MAX - 1;
	}
	hit = &projectiles->hits[projectiles->hit_count++];
	hit->kind = kind;
	SDL_memcpy(hit->point, point, sizeof(hit->point));
	SDL_memcpy(hit->normal, normal, sizeof(hit->normal));
	SDL_memcpy(hit->velocity, projectile->velocity, sizeof(hit->velocity));
	hit->instance = instance;
	hit->object_id = 0;
	hit->actor_id = 0;
	hit->zone = ACTOR_ZONE_BODY;
	hit->bone = -1;
	hit->damage = projectile->damage * speed_squared / (projectile->muzzle_speed * projectile->muzzle_speed);
	hit->damage_type = projectile->damage_type;
	hit->ammo = projectile->ammo;
	hit->owner = projectile->owner;
}

static float surface_at(const projectiles_world_t *world, float x, float z, float *ground_out)
{
	float ground = terrain_sample_height_meters(world->terrain, x, z);
	float depth = 0.0f;
	float flow_x;
	float flow_z;

	if (world->water != NULL && !water_sample(world->water, x, z, &depth, &flow_x, &flow_z))
	{
		depth = 0.0f;
	}
	*ground_out = ground;
	return ground + (depth > 0.02f ? depth : 0.0f);
}

// Where along from..to (0..1) the way first goes under the ground (false:
// it does not), by steps of a cell, then halving
static bool ground_crossing(const projectiles_world_t *world, const float from[3], const float to[3], float *fraction_out)
{
	float length = SDL_sqrtf((to[0] - from[0]) * (to[0] - from[0]) + (to[2] - from[2]) * (to[2] - from[2]));
	int steps = SDL_max((int)(length / SDL_max(world->terrain->cell_size_meters, 0.05f)) + 1, 1);
	float previous = 0.0f;

	for (int i = 1; i <= steps; i++)
	{
		float t = (float)i / (float)steps;
		float x = from[0] + (to[0] - from[0]) * t;
		float y = from[1] + (to[1] - from[1]) * t;
		float z = from[2] + (to[2] - from[2]) * t;

		if (y <= terrain_sample_height_meters(world->terrain, x, z))
		{
			float low = previous;
			float high = t;

			for (int k = 0; k < 8; k++)
			{
				float middle = 0.5f * (low + high);
				float mx = from[0] + (to[0] - from[0]) * middle;
				float my = from[1] + (to[1] - from[1]) * middle;
				float mz = from[2] + (to[2] - from[2]) * middle;

				if (my <= terrain_sample_height_meters(world->terrain, mx, mz))
				{
					high = middle;
				}
				else
				{
					low = middle;
				}
			}
			*fraction_out = high;
			return true;
		}
		previous = t;
	}
	return false;
}

static void ground_normal(const terrain_t *terrain, float x, float z, float normal_out[3])
{
	float step = terrain->cell_size_meters;
	float dx = terrain_sample_height_meters(terrain, x + step, z) - terrain_sample_height_meters(terrain, x - step, z);
	float dz = terrain_sample_height_meters(terrain, x, z + step) - terrain_sample_height_meters(terrain, x, z - step);
	float length;

	normal_out[0] = -dx;
	normal_out[1] = 2.0f * step;
	normal_out[2] = -dz;
	length = SDL_sqrtf(normal_out[0] * normal_out[0] + normal_out[1] * normal_out[1] + normal_out[2] * normal_out[2]);
	for (int k = 0; k < 3; k++)
	{
		normal_out[k] /= SDL_max(length, 1e-6f);
	}
}

static void finish(projectile_t *projectile, particles_t *particles)
{
	if (projectile->tracer != 0 && particles != NULL)
	{
		particles_stop(particles, projectile->tracer);
	}
	projectile->used = false;
}

// One step of one projectile; false: it is gone
static bool step(projectiles_t *projectiles, projectile_t *projectile, float seconds, const projectiles_world_t *world)
{
	float from[3];
	float to[3];
	float relative[3];
	float speed;
	float drag;
	float extent = (float)world->terrain->size * world->terrain->cell_size_meters;
	float ground_fraction = 2.0f;
	physics_cast_t cast;
	bool cast_hit = false;

	SDL_memcpy(from, projectile->position, sizeof(from));
	// The air (or the water) against it, the wind carrying the air
	relative[0] = projectile->velocity[0] - (projectile->in_water ? 0.0f : world->wind[0]);
	relative[1] = projectile->velocity[1];
	relative[2] = projectile->velocity[2] - (projectile->in_water ? 0.0f : world->wind[1]);
	speed = SDL_sqrtf(relative[0] * relative[0] + relative[1] * relative[1] + relative[2] * relative[2]);
	drag = projectile->drag * (projectile->in_water ? WATER_DRAG_FACTOR : 1.0f);
	{
		// (the speed's loss, its exact form for a step: v / (1 + k v dt))
		float kept = 1.0f / (1.0f + drag * speed * seconds);

		for (int k = 0; k < 3; k++)
		{
			float wind_part = projectile->velocity[k] - relative[k];

			projectile->velocity[k] = wind_part + relative[k] * kept;
		}
	}
	projectile->velocity[1] -= GRAVITY * seconds;
	for (int k = 0; k < 3; k++)
	{
		to[k] = from[k] + projectile->velocity[k] * seconds;
	}
	projectile->age_seconds += seconds;
	if (projectile->age_seconds > projectile->life_seconds || to[0] < 0.0f || to[2] < 0.0f || to[0] > extent || to[2] > extent || to[1] < -500.0f)
	{
		return false;
	}
	// What it meets on the way: the ground, a shape
	if (ground_crossing(world, from, to, &ground_fraction))
	{
		// (found)
	}
	else
	{
		ground_fraction = 2.0f;
	}
	if (world->physics != NULL)
	{
		cast_hit = physics_cast_segment(world->physics, from, to, &cast);
	}
	// The water's surface on the way in
	if (!projectile->in_water && world->water != NULL)
	{
		float ground;
		float surface = surface_at(world, to[0], to[2], &ground);

		if (to[1] < surface && surface > ground + 0.02f)
		{
			float fraction = from[1] > to[1] ? SDL_clamp((from[1] - surface) / (from[1] - to[1]), 0.0f, 1.0f) : 1.0f;

			if (fraction < ground_fraction && (!cast_hit || fraction < cast.fraction))
			{
				float point[3];
				float up[3] = { 0.0f, 1.0f, 0.0f };

				for (int k = 0; k < 3; k++)
				{
					point[k] = from[k] + (to[k] - from[k]) * fraction;
				}
				point[1] = surface;
				play_impact(world->particles, impact_for_water(projectiles), point, up);
				keep_hit(projectiles, projectile, PROJECTILE_HIT_WATER, point, up, -1);
				projectile->in_water = true;
				if (projectile->tracer != 0 && world->particles != NULL)
				{
					particles_stop(world->particles, projectile->tracer);
					projectile->tracer = 0;
				}
			}
		}
	}
	// An actor's body before the rest: the bullet stays in it
	if (world->actors != NULL && world->actors->count > 0)
	{
		float way[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
		float length = SDL_sqrtf(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
		actor_hit_t actor_hit;

		if (length > 1e-6f
			&& actors_hit_test(world->actors, vec3_make(from[0], from[1], from[2]), vec3_make(way[0] / length, way[1] / length, way[2] / length),
				length, &actor_hit))
		{
			float fraction = actor_hit.distance / length;

			if (fraction <= ground_fraction && (!cast_hit || fraction <= cast.fraction))
			{
				if (!projectile->in_water)
				{
					play_impact(world->particles, impact_for_actor(projectiles), actor_hit.point, actor_hit.normal);
				}
				keep_hit(projectiles, projectile, PROJECTILE_HIT_ACTOR, actor_hit.point, actor_hit.normal, -1);
				projectiles->hits[projectiles->hit_count - 1].actor_id = actor_hit.id;
				projectiles->hits[projectiles->hit_count - 1].zone = actor_hit.zone;
				projectiles->hits[projectiles->hit_count - 1].bone = actor_hit.bone;
				return false;
			}
		}
	}
	if (cast_hit && cast.fraction <= ground_fraction)
	{
		bool object = cast.instance >= 0;

		if (!projectile->in_water)
		{
			const impact_kind_t *impact = object ? impact_for_object(projectiles, world->objects, cast.instance)
				: impact_for_ground(projectiles, world->terrain, cast.point[0], cast.point[2]);
			bool stays = !object || (world->objects->instances[cast.instance].state & OBJECT_STATE_FREE) == 0;

			play_impact(world->particles, impact, cast.point, cast.normal);
			if (stays)
			{
				leave_mark(world, impact, cast.point, cast.normal);
			}
		}
		if (cast.body >= 0)
		{
			float impulse[3];

			for (int k = 0; k < 3; k++)
			{
				impulse[k] = projectile->velocity[k] * projectile->mass_kilograms * projectile->push;
			}
			physics_push_body(world->physics, cast.body, impulse, cast.point);
		}
		keep_hit(projectiles, projectile, object ? PROJECTILE_HIT_OBJECT : PROJECTILE_HIT_GROUND, cast.point, cast.normal, cast.instance);
		if (object && cast.instance < world->objects->instance_count)
		{
			projectiles->hits[projectiles->hit_count - 1].object_id = world->objects->instances[cast.instance].id;
		}
		return false;
	}
	if (ground_fraction <= 1.0f)
	{
		float point[3];
		float normal[3];

		for (int k = 0; k < 3; k++)
		{
			point[k] = from[k] + (to[k] - from[k]) * ground_fraction;
		}
		ground_normal(world->terrain, point[0], point[2], normal);
		if (!projectile->in_water)
		{
			{
				const impact_kind_t *impact = impact_for_ground(projectiles, world->terrain, point[0], point[2]);

				play_impact(world->particles, impact, point, normal);
				leave_mark(world, impact, point, normal);
			}
		}
		keep_hit(projectiles, projectile, PROJECTILE_HIT_GROUND, point, normal, -1);
		return false;
	}
	SDL_memcpy(projectile->position, to, sizeof(to));
	if (projectile->in_water)
	{
		float now = SDL_sqrtf(projectile->velocity[0] * projectile->velocity[0] + projectile->velocity[1] * projectile->velocity[1]
			+ projectile->velocity[2] * projectile->velocity[2]);

		if (now < WATER_STOP_SPEED)
		{
			return false;
		}
	}
	return true;
}

void projectiles_update(projectiles_t *projectiles, float seconds, const projectiles_world_t *world)
{
	int steps;
	float step_seconds;

	if (seconds <= 0.0f || world->terrain == NULL || world->terrain->size <= 0)
	{
		return;
	}
	steps = SDL_max((int)SDL_ceilf(seconds / LONGEST_STEP), 1);
	step_seconds = seconds / (float)steps;
	for (int i = 0; i < PROJECTILES_MAX; i++)
	{
		projectile_t *projectile = &projectiles->list[i];

		if (!projectile->used)
		{
			continue;
		}
		for (int s = 0; s < steps; s++)
		{
			if (!step(projectiles, projectile, step_seconds, world))
			{
				finish(projectile, world->particles);
				break;
			}
		}
	}
}

int projectiles_take_hits(projectiles_t *projectiles, projectile_hit_t *hits_out, int max)
{
	int count = SDL_min(projectiles->hit_count, max);

	if (count > 0 && hits_out != NULL)
	{
		SDL_memcpy(hits_out, &projectiles->hits[projectiles->hit_count - count], sizeof(projectile_hit_t) * (size_t)count);
	}
	projectiles->hit_count = 0;
	return count;
}

void projectiles_add_hit(projectiles_t *projectiles, const projectile_hit_t *hit)
{
	if (projectiles->hit_count >= PROJECTILE_HITS_MAX)
	{
		SDL_memmove(&projectiles->hits[0], &projectiles->hits[1], sizeof(projectile_hit_t) * (PROJECTILE_HITS_MAX - 1));
		projectiles->hit_count = PROJECTILE_HITS_MAX - 1;
	}
	projectiles->hits[projectiles->hit_count++] = *hit;
}
