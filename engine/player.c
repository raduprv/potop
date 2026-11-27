// The player on foot (player.h)

#include "player.h"

#include <SDL3/SDL.h>

#include "ground_items.h"
#include "session.h"

void player_init(player_t *player)
{
	SDL_zerop(player);
	player->height_meters = 1.6f;
	player->step_meters = 0.5f;
	player->kilograms = 75.0f;
	player->reach_meters = 2.5f;
	player->talk_reach_meters = 3.0f;
	// A torch, unless the game says what it carries
	light_source_reset(&player->light);
	player->light.color[0] = 1.0f;
	player->light.color[1] = 0.62f;
	player->light.color[2] = 0.28f;
	player->light.intensity = 2.5f;
	player->light.radius_meters = 12.0f;
	player->light.flicker = 0.45f;
	player->light.core_meters = 0.0f;
	player->light.switched = false;
}

void player_update_light(player_t *player, world_t *world, const camera_t *camera)
{
	if (player->light_on && camera->first_person)
	{
		// In the left hand, a little ahead and below the eye
		vec3_t right = vec3_make(camera->view.m[0], camera->view.m[4], camera->view.m[8]);
		vec3_t up = vec3_make(camera->view.m[1], camera->view.m[5], camera->view.m[9]);
		vec3_t forward = vec3_make(-camera->view.m[2], -camera->view.m[6], -camera->view.m[10]);
		float position[3];

		for (int k = 0; k < 3; k++)
		{
			float eye = k == 0 ? camera->eye_position.x : k == 1 ? camera->eye_position.y : camera->eye_position.z;
			float r = k == 0 ? right.x : k == 1 ? right.y : right.z;
			float u = k == 0 ? up.x : k == 1 ? up.y : up.z;
			float f = k == 0 ? forward.x : k == 1 ? forward.y : forward.z;

			position[k] = eye - 0.35f * r - 0.25f * u + 0.4f * f;
		}
		lights_set_carried(&world->lights, PLAYER_LIGHT_KEY, position, &player->light);
		SDL_memcpy(player->light_position, position, sizeof(player->light_position));
	}
	else
	{
		lights_remove_carried(&world->lights, PLAYER_LIGHT_KEY);
	}
}

// What the walker stands on and runs into (camera_walker_world_t): it
// stands on what the navigation knows (terrain, decks, rocks' tops);
// houses, trees and big rocks stop it by their real shapes
// (physics_move_walker); loose light things (barrels, logs, boats, stones)
// do not: it pushes them (physics_push_with_walker)
typedef struct
{
	const player_t *player;
	world_t *world;
} walker_context_t;

// What the feet stand on: the terrain, or the top of what the physics has
// there under a step over them (a deck, a floor, a rock, a log). The
// navigation's surfaces are the animals': one a meter square per object,
// a deck under a gallery's beams was not among them
static float walker_ground_height(void *context, float x, float z, float near_height)
{
	walker_context_t *walker = context;
	float terrain = terrain_sample_height_meters(&walker->world->terrain, x, z);
	float object;

	if (physics_walker_ground(&walker->world->physics, x, z, near_height, walker->player->step_meters, true, &object))
	{
		return SDL_max(terrain, object);
	}
	return terrain;
}

static void walker_move(void *context, float *x, float *z, float feet_height, float move_x, float move_z)
{
	walker_context_t *walker = context;

	physics_move_walker(&walker->world->physics, x, z, feet_height, move_x, move_z, walker->player->step_meters);
}

void player_walk(player_t *player, world_t *world, camera_t *camera, const input_state_t *input, const camera_settings_t *settings,
	float seconds)
{
	walker_context_t context = { player, world };
	camera_walker_world_t walker_world = { &context, walker_ground_height, walker_move };

	camera_update_first_person(camera, input, settings, &world->terrain, &world->water, &walker_world, seconds);
	physics_push_with_walker(&world->physics, camera->walker_x, camera->walker_feet, camera->walker_z, camera->walker_velocity_x,
		camera->walker_velocity_z, player->kilograms);
}

bool player_throw(player_t *player, session_t *session, world_t *world, const camera_t *camera, int item)
{
	vec3_t forward = vec3_make(-camera->view.m[2], -camera->view.m[6], -camera->view.m[10]);
	vec3_t right = vec3_make(camera->view.m[0], camera->view.m[4], camera->view.m[8]);
	vec3_t direction;
	float from[3];
	float way[3];

	(void)player;
	if (item < 0 || item >= session->items.type_count || session->items.types[item].explosive < 0
		|| inventory_count(&session->inventories, session->player_inventory, item, false) == 0)
	{
		return false;
	}
	// A little upwards: thrown, not dropped
	direction = vec3_normalize(vec3_add(forward, vec3_make(0.0f, 0.25f, 0.0f)));
	from[0] = camera->eye_position.x + forward.x * 0.6f + right.x * 0.25f;
	from[1] = camera->eye_position.y + forward.y * 0.6f - 0.15f;
	from[2] = camera->eye_position.z + forward.z * 0.6f + right.z * 0.25f;
	way[0] = direction.x;
	way[1] = direction.y;
	way[2] = direction.z;
	if (!explosives_throw(&session->explosives, world, session->items.types[item].explosive, from, way, -1))
	{
		return false;
	}
	inventory_remove(&session->inventories, session->player_inventory, item, 1);
	return true;
}

uint32_t player_place(player_t *player, session_t *session, world_t *world, const camera_t *camera, int item)
{
	vec3_t forward = vec3_make(-camera->view.m[2], -camera->view.m[6], -camera->view.m[10]);
	vec3_t hit;
	float x;
	float z;
	uint32_t placed;

	(void)player;
	if (item < 0 || item >= session->items.type_count || session->items.types[item].explosive < 0
		|| inventory_count(&session->inventories, session->player_inventory, item, false) == 0)
	{
		return 0;
	}
	// Where the eye looks on the ground, within reach; else a step ahead
	if (terrain_raycast(&world->terrain, camera->eye_position, forward, 3.0f, &hit))
	{
		x = hit.x;
		z = hit.z;
	}
	else
	{
		float flat = SDL_sqrtf(forward.x * forward.x + forward.z * forward.z);

		x = camera->eye_position.x + (flat > 1e-3f ? forward.x / flat : 0.0f) * 0.8f;
		z = camera->eye_position.z + (flat > 1e-3f ? forward.z / flat : 0.0f) * 0.8f;
	}
	placed = explosives_place(&session->explosives, world, session->items.types[item].explosive, x, z,
		camera->walker_yaw_degrees * MATH3D_PI / 180.0f, -1);
	if (placed != 0)
	{
		inventory_remove(&session->inventories, session->player_inventory, item, 1);
	}
	return placed;
}

bool player_select_weapon(player_t *player, session_t *session, int weapon)
{
	int item = items_of_weapon(&session->items, weapon);
	container_t *container = inventory_get(&session->inventories, session->player_inventory);

	(void)player;
	if (item >= 0 && container != NULL)
	{
		int hands = session->items.types[item].slot;

		if (inventory_count(&session->inventories, session->player_inventory, item, true) == 0)
		{
			return false;
		}
		if (hands >= 0 && container->equipped[hands] != item)
		{
			for (int i = 0; i < container->entry_count; i++)
			{
				if (container->entries[i].item == item)
				{
					inventory_equip(&session->inventories, session->player_inventory, i);
					break;
				}
			}
		}
	}
	weapons_select(&session->weapons, weapon);
	return true;
}

// The rounds in the pack are what the weapons can load (before their
// update), and what a reload took comes out of the pack (after). A weapon
// no longer carried is put away
static void sync_weapon_ammo(session_t *session, bool before)
{
	for (int k = 0; k < session->weapons.ammo_count; k++)
	{
		int item = items_of_ammo(&session->items, k);

		if (item < 0)
		{
			continue;
		}
		if (before)
		{
			weapons_set_reserve(&session->weapons, k, inventory_count(&session->inventories, session->player_inventory, item, false));
		}
		else
		{
			int loaded = weapons_take_loaded(&session->weapons, k);

			if (loaded > 0)
			{
				inventory_remove(&session->inventories, session->player_inventory, item, loaded);
			}
		}
	}
	if (before && weapons_in_hand(&session->weapons) && session->weapons.state != WEAPON_STATE_LOWERING)
	{
		int item = items_of_weapon(&session->items, session->weapons.current);

		if (item >= 0 && inventory_count(&session->inventories, session->player_inventory, item, true) == 0)
		{
			weapons_toggle(&session->weapons);
		}
	}
}

void player_update_weapons(player_t *player, session_t *session, world_t *world, camera_t *camera, float seconds)
{
	weapon_shot_t shots[16];
	float kick_up;
	float kick_right;
	float eye[3] = { camera->eye_position.x, camera->eye_position.y, camera->eye_position.z };
	float right[3] = { camera->view.m[0], camera->view.m[4], camera->view.m[8] };
	float up[3] = { camera->view.m[1], camera->view.m[5], camera->view.m[9] };
	float forward[3] = { -camera->view.m[2], -camera->view.m[6], -camera->view.m[10] };
	int count;

	player->weapon_seconds = seconds;
	sync_weapon_ammo(session, true);
	count = weapons_update(&session->weapons, seconds, player->trigger && camera->first_person, player->aim, eye, forward, right, up, shots, 16,
		&kick_up, &kick_right);
	camera->zoom = camera->first_person ? weapons_zoom(&session->weapons) : 1.0f;
	sync_weapon_ammo(session, false);
	if (count > 0)
	{
		const weapon_type_t *type = &session->weapons.types[session->weapons.current];
		int flash = particles_find_effect(&world->particles, type->muzzle_effect);
		float muzzle[3];

		viewmodel_muzzle_position(&session->viewmodel, &session->weapons, camera, muzzle);
		// (heard far: a rifle's crack carries hundreds of meters)
		noises_make(&world->noises, muzzle, type->noise_meters, NOISE_SHOT, 0, world->elapsed_seconds);
		if (flash >= 0)
		{
			particles_play(&world->particles, flash, muzzle[0], muzzle[1], muzzle[2]);
		}
		for (int i = 0; i < count; i++)
		{
			const weapon_ammo_t *ammo = &session->weapons.ammo[shots[i].ammo];

			// A launcher's grenade, a ball: a thing of explosives.ini thrown
			// out of the barrel (from a little ahead of the eye, clear of
			// the one shooting), the physics flying it
			if (ammo->throws[0] != '\0')
			{
				float from[3];

				for (int k = 0; k < 3; k++)
				{
					from[k] = shots[i].origin[k] + shots[i].direction[k] * 0.9f;
				}
				explosives_throw(&session->explosives, world, explosives_find(&session->explosives, ammo->throws), from, shots[i].direction, -1);
				continue;
			}
			projectile_t *shot = projectiles_fire(&session->projectiles, &world->particles, shots[i].origin, shots[i].direction, shots[i].speed,
				ammo->mass_grams * 0.001f, ammo->drag, ammo->push, ammo->damage * shots[i].damage_scale, ammo->life_seconds, -1,
				shots[i].tracer ? type->tracer_effect : NULL, muzzle);

			shot->damage_type = ammo->damage_type;
			shot->ammo = shots[i].ammo;
		}
		camera->walker_pitch_degrees = SDL_clamp(camera->walker_pitch_degrees + kick_up, -89.0f, 89.0f);
		camera->walker_yaw_degrees = SDL_fmodf(camera->walker_yaw_degrees - kick_right + 360.0f, 360.0f);
	}
	{
		projectiles_world_t projectiles_world;
		float wind = world->settings->render.plant_wind_direction_degrees * SDL_PI_F / 180.0f;
		// (the plants' wind, 0..1, as meters a second for the bullets)
		float wind_speed = 10.0f * SDL_clamp(world->settings->render.plant_wind_strength, 0.0f, 1.0f);

		SDL_zero(projectiles_world);
		projectiles_world.terrain = &world->terrain;
		projectiles_world.water = &world->water;
		projectiles_world.physics = &world->physics;
		projectiles_world.objects = &world->objects;
		projectiles_world.actors = &world->actors;
		projectiles_world.particles = &world->particles;
		projectiles_world.decals = &world->decals;
		projectiles_world.now = world->elapsed_seconds;
		projectiles_world.wind[0] = SDL_cosf(wind) * wind_speed;
		projectiles_world.wind[1] = SDL_sinf(wind) * wind_speed;
		projectiles_update(&session->projectiles, seconds, &projectiles_world);
	}
}

void player_find_target(player_t *player, session_t *session, world_t *world, const camera_t *camera)
{
	player_target_t *target = &player->target;
	vec3_t eye = camera->eye_position;
	vec3_t forward = vec3_make(-camera->view.m[2], -camera->view.m[6], -camera->view.m[10]);
	object_ray_hit_t object_hits[8];
	int object_hit_count = objects_raycast(&world->objects, eye, forward, player->reach_meters, OBJECTS_RAY_ALL, object_hits, 8);
	actor_hit_t hit;
	bool actor_hit = actors_hit_test(&world->actors, eye, forward, SDL_max(player->reach_meters, player->talk_reach_meters), &hit);

	uint32_t door_module = 0;
	int door_part = -1;
	float door_distance = 0.0f;
	// (a door or a gate of a module within reach: its leaf as it stands)
	bool door_hit = objects_door_at(&world->objects, eye, forward, player->reach_meters, &door_module, &door_part, &door_distance);

	SDL_zerop(target);
	target->ground_item = -1;
	target->instance = -1;
	target->item = -1;
	target->door = -1;
	if (actor_hit)
	{
		bool dead = actors_is_dead(&world->actors, hit.id);

		// The dead only as near as things to pick, the living a little farther
		actor_hit = hit.distance <= (dead ? player->reach_meters : player->talk_reach_meters);
	}
	// The nearest thing lying there, or to pick off the map, unless an
	// actor stands before it
	for (int i = 0; i < object_hit_count; i++)
	{
		int lying = ground_items_of_instance(&world->ground_items, &world->objects, object_hits[i].instance);
		bool any_left = false;
		int item = lying < 0 ? harvest_item_of(&world->objects, &session->items, object_hits[i].instance, &any_left) : -1;
		bool interactive = (world->objects.instances[object_hits[i].instance].state & OBJECT_STATE_INTERACTIVE) != 0;

		if (lying < 0 && item < 0 && !interactive)
		{
			continue;
		}
		if (actor_hit && hit.distance <= object_hits[i].distance)
		{
			break;
		}
		if (door_hit && door_distance < object_hits[i].distance)
		{
			break;
		}
		target->distance = object_hits[i].distance;
		target->instance = object_hits[i].instance;
		target->object_id = world->objects.instances[object_hits[i].instance].id;
		if (lying >= 0)
		{
			target->kind = PLAYER_TARGET_GROUND_ITEM;
			target->ground_item = lying;
			target->container = world->ground_items.list[lying].container;
		}
		else if (item >= 0)
		{
			target->kind = PLAYER_TARGET_HARVEST;
			target->item = item;
			target->any_left = any_left;
		}
		else
		{
			target->kind = PLAYER_TARGET_OBJECT;
		}
		return;
	}
	if (door_hit && (!actor_hit || door_distance < hit.distance))
	{
		target->kind = PLAYER_TARGET_DOOR;
		target->distance = door_distance;
		target->object_id = door_module;
		target->instance = objects_find_id(&world->objects, door_module);
		target->door = door_part;
		target->door_open = objects_door(&world->objects, door_module, door_part) == 1;
		return;
	}
	if (actor_hit)
	{
		target->kind = PLAYER_TARGET_ACTOR;
		target->distance = hit.distance;
		target->actor_id = hit.id;
		target->actor_dead = actors_is_dead(&world->actors, hit.id);
	}
}

harvest_outcome_t player_pick(player_t *player, session_t *session, world_t *world)
{
	harvest_outcome_t nothing;

	if (player->target.kind != PLAYER_TARGET_HARVEST)
	{
		SDL_zero(nothing);
		nothing.result = HARVEST_NOTHING;
		return nothing;
	}
	return harvest_pick(&world->objects, &world->terrain, &session->inventories, player->target.instance, session->player_inventory,
		&world->harvest_random);
}

bool player_pick_up(player_t *player, session_t *session, world_t *world)
{
	const container_t *container;
	int lying = player->target.ground_item;

	if (player->target.kind != PLAYER_TARGET_GROUND_ITEM || lying < 0 || lying >= world->ground_items.count)
	{
		return false;
	}
	ground_items_pick_up(&world->ground_items, &world->objects, &session->inventories, lying, session->player_inventory);
	// (all of it taken: the ground item and its container are gone)
	container = inventory_get_const(&session->inventories, world->ground_items.count > lying ? world->ground_items.list[lying].container : 0);
	return container == NULL || container->entry_count == 0 || world->ground_items.list[lying].container != player->target.container;
}
