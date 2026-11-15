// Potop's interface for a game (potop.h): each call goes to the engine's
// part that does it

#include "potop_engine.h"

#include <SDL3/SDL.h>

// The interface's numbers are the engine's
_Static_assert((int)POTOP_ITEM_MONEY == (int)ITEM_CATEGORY_MONEY, "item categories");
_Static_assert((int)POTOP_INVENTORY_USED == (int)INVENTORY_EVENT_USED, "inventory events");
_Static_assert((int)POTOP_QUEST_FAILED == (int)JOURNAL_QUEST_FAILED, "quest states");
_Static_assert((int)POTOP_OBJECTIVE_FAILED == (int)JOURNAL_OBJECTIVE_FAILED, "objective states");
_Static_assert((int)POTOP_DIALOG_CLOSED == (int)DIALOG_EVENT_CLOSED, "dialog events");
_Static_assert((int)POTOP_NOTIFY_WARNING == (int)HUD_NOTIFY_WARNING, "notice styles");
_Static_assert((int)POTOP_ACTOR_NO_PATH == (int)ACTOR_EVENT_NO_PATH, "actor events");
_Static_assert((int)POTOP_TARGET_OBJECT == (int)PLAYER_TARGET_OBJECT, "targets");
_Static_assert((int)POTOP_PICK_TOO_HEAVY == (int)HARVEST_TOO_HEAVY, "pick results");
_Static_assert((int)POTOP_HIT_PLAYER == (int)PROJECTILE_HIT_PLAYER, "hit kinds");
_Static_assert((int)POTOP_PART_LIMB == (int)ACTOR_ZONE_LIMB, "body parts");
_Static_assert((int)POTOP_DAMAGE_MAGIC == (int)DAMAGE_MAGIC, "damage types");
_Static_assert((int)POTOP_NOISE_OTHER == (int)NOISE_OTHER, "noise kinds");
_Static_assert((int)POTOP_AREA_TRIGGER_ACTORS == (int)AREA_TRIGGER_ACTORS, "area triggers");
_Static_assert((int)POTOP_AREA_EXIT == (int)AREA_EVENT_EXIT, "area events");
_Static_assert((int)POTOP_BRUSH_SMOOTH == (int)EDIT_SMOOTH, "brushes");
_Static_assert((int)POTOP_SOIL_ROCK == (int)SOIL_ROCK, "soils");
_Static_assert((int)POTOP_STAMP_PLATEAU == (int)STAMP_PLATEAU, "stamps");

potop_t *potop_create(void)
{
	potop_t *potop = SDL_calloc(1, sizeof(potop_t));

	if (potop != NULL)
	{
		random_seed(&potop->edit_random, 0x9e3779b97f4a7c15ull);
	}
	return potop;
}

void potop_destroy(potop_t *potop)
{
	SDL_free(potop);
}

// ---------------------------------------------------------------------------
// The world

float potop_world_extent_meters(const potop_t *potop)
{
	return (float)potop->world.terrain.size * potop->world.terrain.cell_size_meters;
}

float potop_world_ground_height(const potop_t *potop, float x, float z)
{
	return terrain_sample_height_meters(&potop->world.terrain, x, z);
}

// ---------------------------------------------------------------------------
// Maps

bool potop_map_find(const char *name, char *file_name_out, int file_name_size)
{
	for (int attempt = 0; attempt < 3; attempt++)
	{
		char path[1024];

		if (attempt == 0)
		{
			SDL_strlcpy(file_name_out, name, (size_t)file_name_size);
		}
		else
		{
			map_file_kind_name(name, attempt == 2, file_name_out, (size_t)file_name_size);
		}
		if (map_file_path(file_name_out, path, sizeof(path)) && SDL_GetPathInfo(path, NULL))
		{
			return true;
		}
	}
	return false;
}

bool potop_map_save(potop_t *potop, const char *name, bool baked)
{
	char path[1024];
	char file_name[MAP_FILE_NAME_LENGTH + 16];

	if (potop->world.terrain.baked && !baked)
	{
		return false;
	}
	map_file_kind_name(name, baked, file_name, sizeof(file_name));
	if (!map_file_path(file_name, path, sizeof(path)) || !world_save(&potop->world, &potop->session, path, baked, &potop->camera))
	{
		return false;
	}
	SDL_strlcpy(potop->world.map_name, file_name, sizeof(potop->world.map_name));
	return true;
}

potop_load_result_t potop_map_load(potop_t *potop, const char *name)
{
	char path[1024];
	char file_name[MAP_FILE_NAME_LENGTH + 16];
	map_file_info_t info;
	bool complete;
	bool on_foot = potop->camera.first_person;
	camera_t *camera = &potop->camera;

	potop->session.world_map_stale = true;
	if (potop_map_find(name, file_name, (int)sizeof(file_name)))
	{
		name = file_name;
	}
	if (!map_file_path(name, path, sizeof(path)) || !SDL_GetPathInfo(path, NULL)
		|| !world_load(&potop->world, &potop->session, path, &info, &complete))
	{
		return POTOP_LOAD_FAILED;
	}
	// (on foot it stays on foot: standing where the map's camera looked)
	camera_reset(camera, &potop->settings.camera, &potop->world.terrain);
	if (info.has_camera)
	{
		camera->target_x = info.camera_target_x;
		camera->target_z = info.camera_target_z;
		camera->target_height = info.camera_target_height;
		camera->distance_meters = info.camera_distance_meters;
		camera->yaw_degrees = info.camera_yaw_degrees;
		camera->pitch_degrees = info.camera_pitch_degrees;
	}
	if (on_foot)
	{
		camera_enter_first_person(camera, &potop->world.terrain);
	}
	SDL_strlcpy(potop->world.map_name, name, sizeof(potop->world.map_name));
	return complete ? POTOP_LOAD_DONE : POTOP_LOAD_DAMAGED;
}

const char *potop_map_name(const potop_t *potop)
{
	return potop->world.map_name;
}

bool potop_map_new(potop_t *potop, uint32_t seed)
{
	bool on_foot = potop->camera.first_person;

	potop->session.world_map_stale = true;
	if (!world_resize(&potop->world, potop->settings.terrain.size, potop->settings.terrain.cell_size_meters, false))
	{
		return false;
	}
	if (!world_generate(&potop->world, seed != 0 ? seed : ((uint32_t)SDL_GetPerformanceCounter() | 1u)))
	{
		return false;
	}
	ground_items_clear(&potop->world.ground_items, &potop->session.inventories);
	camera_reset(&potop->camera, &potop->settings.camera, &potop->world.terrain);
	if (on_foot)
	{
		camera_enter_first_person(&potop->camera, &potop->world.terrain);
	}
	cloud_paint_reset(&potop->world.cloud_paint);
	potop->world.map_name[0] = '\0';
	potop->world.simulation_accumulator_seconds = 0.0;
	return true;
}

// ---------------------------------------------------------------------------
// Object types

int potop_object_type_count(const potop_t *potop)
{
	return potop->world.objects.types.type_count;
}

int potop_object_type_find(const potop_t *potop, const char *name)
{
	return object_types_find(&potop->world.objects.types, name);
}

bool potop_object_type_get(const potop_t *potop, int type, potop_object_type_info_t *info_out)
{
	const object_types_t *types = &potop->world.objects.types;
	const object_type_t *found;

	SDL_zerop(info_out);
	if (type < 0 || type >= types->type_count)
	{
		return false;
	}
	found = &types->types[type];
	SDL_strlcpy(info_out->name, found->name, sizeof(info_out->name));
	if (found->category >= 0 && found->category < types->category_count)
	{
		SDL_strlcpy(info_out->category, types->categories[found->category], sizeof(info_out->category));
	}
	SDL_strlcpy(info_out->harvest_item, found->harvest_item, sizeof(info_out->harvest_item));
	return true;
}

void potop_object_type_set_harvest(potop_t *potop, int type, const char *item, int least, int most, bool removes, int times,
	float regrow_hours)
{
	harvest_set(&potop->world.objects, type, item, least, most, removes, times, regrow_hours);
}

// ---------------------------------------------------------------------------
// Objects on the map

bool potop_object_get(potop_t *potop, uint32_t id, potop_object_info_t *info_out)
{
	objects_t *objects = &potop->world.objects;
	int index = objects_find_id(objects, id);
	const object_instance_t *instance;

	SDL_zerop(info_out);
	if (index < 0)
	{
		return false;
	}
	instance = &objects->instances[index];
	info_out->id = instance->id;
	info_out->type = instance->type;
	SDL_strlcpy(info_out->type_name, objects->types.types[instance->type].name, sizeof(info_out->type_name));
	info_out->x = instance->x;
	info_out->y = instance->state & OBJECT_STATE_FREE ? instance->y : instance->ground_height + instance->lift_meters;
	info_out->z = instance->z;
	info_out->yaw_radians = instance->yaw_radians;
	info_out->scale = instance->scale;
	info_out->group = instance->group;
	info_out->owner = instance->owner;
	return true;
}

uint32_t potop_object_place(potop_t *potop, int type, float x, float z, float yaw_radians, float scale, bool awake)
{
	return edit_place_object(&potop->world, type, x, z, yaw_radians, scale, awake);
}

bool potop_object_set_owner(potop_t *potop, uint32_t id, uint32_t owner)
{
	return objects_set_owner(&potop->world.objects, id, owner);
}

bool potop_object_set_state(potop_t *potop, uint32_t id, const char *state)
{
	return objects_set_module_state(&potop->world.objects, id, state, -1);
}

typedef struct
{
	uint32_t *ids;
	int count;
	int maximum;
	const uint8_t *has_spot;       // NULL: any object
} objects_found_t;

static bool found_one(const objects_t *objects, int index, void *user)
{
	objects_found_t *found = user;

	if (found->has_spot != NULL)
	{
		int model = objects_instance_model(objects, &objects->instances[index]);

		if (model < 0 || !found->has_spot[model])
		{
			return true;
		}
	}
	found->ids[found->count++] = objects->instances[index].id;
	return found->count < found->maximum;
}

uint32_t potop_objects_generation(const potop_t *potop)
{
	return potop->world.objects.generation;
}

int potop_objects_near(const potop_t *potop, float x, float z, float radius, uint32_t *ids_out, int maximum)
{
	objects_found_t found = { ids_out, 0, maximum, NULL };

	if (maximum <= 0)
	{
		return 0;
	}
	objects_visit_circle(&potop->world.objects, x, z, radius, found_one, &found);
	return found.count;
}

int potop_objects_with_spot(const potop_t *potop, float x, float z, float radius, const char *spot_name, uint32_t *ids_out, int maximum)
{
	const objects_t *objects = &potop->world.objects;
	uint8_t *has_spot;
	bool any = false;
	int count = 0;

	// (which models have it, once: then an object is a lookup, not its
	// model's spots compared by name, millions of times on a big map)
	has_spot = SDL_calloc((size_t)SDL_max(objects->model_count, 1), 1);
	if (has_spot == NULL)
	{
		return 0;
	}
	for (int m = 0; m < objects->model_count; m++)
	{
		for (int k = 0; k < objects->models[m].spot_count; k++)
		{
			if (SDL_strcmp(objects->models[m].spots[k].name, spot_name) == 0)
			{
				has_spot[m] = 1;
				any = true;
				break;
			}
		}
	}
	if (any && maximum > 0)
	{
		objects_found_t found = { ids_out, 0, maximum, has_spot };

		objects_visit_circle(objects, x, z, radius, found_one, &found);
		count = found.count;
	}
	SDL_free(has_spot);
	return count;
}

bool potop_object_set_interactive(potop_t *potop, uint32_t id, bool interactive)
{
	return objects_set_interactive(&potop->world.objects, id, interactive);
}

bool potop_object_remove(potop_t *potop, uint32_t id)
{
	objects_t *objects = &potop->world.objects;
	int index = objects_find_id(objects, id);

	if (index < 0)
	{
		return false;
	}
	// (gone where it stood: the list compacted only when a map is saved)
	objects_kill(objects, index);
	return true;
}

// ---------------------------------------------------------------------------
// The camera

void potop_camera_get(const potop_t *potop, potop_camera_info_t *info_out)
{
	const camera_t *camera = &potop->camera;

	SDL_zerop(info_out);
	info_out->first_person = camera->first_person;
	info_out->eye_x = camera->eye_position.x;
	info_out->eye_y = camera->eye_position.y;
	info_out->eye_z = camera->eye_position.z;
	info_out->heading_degrees = SDL_atan2f(-camera->view.m[2], camera->view.m[10]) * 180.0f / SDL_PI_F;
	info_out->walker_x = camera->walker_x;
	info_out->walker_z = camera->walker_z;
	info_out->walker_yaw_degrees = camera->walker_yaw_degrees;
	info_out->gait = (potop_gait_t)potop->gait;
	info_out->crawling = camera->walker_crawling;
	info_out->walker_feet = camera->walker_feet;
	info_out->walker_ground = camera->target_height;
	info_out->walker_terrain = terrain_sample_height_meters(&potop->world.terrain, camera->walker_x, camera->walker_z);
	info_out->walker_on_ground = camera->walker_on_ground;
	info_out->push_planes_kept = potop->world.physics.walker_planes_kept;
	info_out->push_planes_skipped = potop->world.physics.walker_planes_skipped;
	info_out->occlusion_culling = !potop->world.objects.no_terrain_occlusion;
	info_out->push_x = potop->world.physics.walker_push_x;
	info_out->push_z = potop->world.physics.walker_push_z;
	for (int k = 0; k < 3; k++)
	{
		info_out->push_normal[k] = potop->world.physics.walker_worst_normal[k];
	}
	info_out->push_height = potop->world.physics.walker_worst_height;
	info_out->push_depth = potop->world.physics.walker_worst_depth;
}

// ---------------------------------------------------------------------------
// Actors

uint32_t potop_actor_spawn(potop_t *potop, const char *type_name, float x, float z, float yaw_radians)
{
	actors_t *actors = &potop->world.actors;
	int type = actors_find_type(actors, type_name);

	if (type < 0 || actors->types[type].model < 0)
	{
		return 0;
	}
	return actors_add(actors, &potop->world.terrain, type, x, z, yaw_radians);
}

bool potop_actor_get(const potop_t *potop, uint32_t id, potop_actor_info_t *info_out)
{
	const actors_t *actors = &potop->world.actors;
	int index = actors_find(actors, id);
	const actor_t *actor;
	const actor_type_t *type;

	SDL_zerop(info_out);
	if (index < 0)
	{
		return false;
	}
	actor = &actors->actors[index];
	type = &actors->types[actor->type];
	info_out->id = actor->id;
	info_out->game_id = actor->game_id;
	SDL_strlcpy(info_out->type_name, type->name, sizeof(info_out->type_name));
	SDL_strlcpy(info_out->name, actor->name, sizeof(info_out->name));
	info_out->hostile = actor->hostile;
	info_out->dead = actors_is_dead(actors, id);
	info_out->flies = type->flies;
	// (stepping onto its place, or in a start's or stop's step: still going)
	info_out->moving = actor->moving || actor->sidestep || actor->gait >= 0;
	info_out->wandering = actor->wandering && actor->wander_share != 0.0f;
	info_out->x = actor->x;
	info_out->y = actor->y;
	info_out->z = actor->z;
	info_out->top_y = actor->y;
	if (type->model >= 0)
	{
		info_out->top_y = actor->y - actor->root_lift + actors->models[type->model].bounds_maximum.y * actor->scale;
	}
	info_out->yaw_radians = actor->yaw_radians;
	return true;
}

uint32_t potop_actor_find_by_game_id(const potop_t *potop, uint32_t game_id)
{
	return actors_find_by_game_id(&potop->world.actors, game_id);
}

int potop_actors_near(const potop_t *potop, float x, float z, float radius, uint32_t *ids_out, int maximum)
{
	const actors_t *actors = &potop->world.actors;
	float distances[256];
	int count = 0;

	maximum = SDL_min(maximum, (int)SDL_arraysize(distances));
	for (int a = 0; a < actors->count; a++)
	{
		float dx = actors->actors[a].x - x;
		float dz = actors->actors[a].z - z;
		float distance = dx * dx + dz * dz;
		int place;

		if (distance > radius * radius)
		{
			continue;
		}
		// Kept in order, the nearest first (the farthest falls off the end)
		place = count;
		while (place > 0 && distances[place - 1] > distance)
		{
			place--;
		}
		if (place >= maximum)
		{
			continue;
		}
		for (int k = SDL_min(count, maximum - 1); k > place; k--)
		{
			distances[k] = distances[k - 1];
			ids_out[k] = ids_out[k - 1];
		}
		distances[place] = distance;
		ids_out[place] = actors->actors[a].id;
		count = SDL_min(count + 1, maximum);
	}
	return count;
}

void potop_actor_set_name(potop_t *potop, uint32_t id, const char *name, bool hostile)
{
	actors_set_name(&potop->world.actors, id, name, hostile);
}

void potop_actor_set_game_id(potop_t *potop, uint32_t id, uint32_t game_id)
{
	actors_set_game_id(&potop->world.actors, id, game_id);
}

bool potop_actor_go_to(potop_t *potop, uint32_t id, float x, float z)
{
	return actors_move_to(&potop->world.actors, &potop->world.navigation, &potop->world.terrain, &potop->world.water, id, x, z);
}

bool potop_actor_walk_straight(potop_t *potop, uint32_t id, const float (*points)[2], int count)
{
	return actors_walk_straight(&potop->world.actors, id, points, count);
}

bool potop_actor_wander(potop_t *potop, uint32_t id, float heading_radians, float share)
{
	return actors_wander(&potop->world.actors, id, heading_radians, share);
}

bool potop_actor_heading_free(potop_t *potop, uint32_t id, float heading_radians, float meters)
{
	return actors_heading_free(&potop->world.actors, id, heading_radians, meters);
}

bool potop_actor_walk_straight_exact(potop_t *potop, uint32_t id, const float (*points)[2], int count, float face_yaw_radians)
{
	return actors_walk_straight_exact(&potop->world.actors, id, points, count, face_yaw_radians);
}

bool potop_actor_remove(potop_t *potop, uint32_t id)
{
	return actors_remove(&potop->world.actors, id);
}

int potop_object_spot_count(potop_t *potop, uint32_t id)
{
	return objects_spot_count(&potop->world.objects, objects_find_id(&potop->world.objects, id));
}

bool potop_object_spot_get(potop_t *potop, uint32_t id, int index, potop_spot_t *spot_out)
{
	float position[3];
	float extents[2];
	float yaw;
	const object_file_spot_t *found = objects_spot_world(&potop->world.objects, objects_find_id(&potop->world.objects, id), index, position, &yaw,
		extents);

	SDL_zerop(spot_out);
	if (found == NULL)
	{
		return false;
	}
	SDL_strlcpy(spot_out->name, found->name, sizeof(spot_out->name));
	spot_out->x = position[0];
	spot_out->y = position[1];
	spot_out->z = position[2];
	spot_out->yaw_radians = yaw;
	spot_out->half_width = extents[0];
	spot_out->half_depth = extents[1];
	for (int l = 0; l < POTOP_SPOT_LINKS && l < OBJECT_FILE_SPOT_LINKS; l++)
	{
		spot_out->links[l] = found->links[l];
	}
	return true;
}

int potop_object_spot_find(potop_t *potop, uint32_t id, const char *name, int from)
{
	int index = objects_find_id(&potop->world.objects, id);
	int count = objects_spot_count(&potop->world.objects, index);

	for (int k = SDL_max(from, 0); k < count; k++)
	{
		const object_instance_t *instance = &potop->world.objects.instances[index];
		const object_model_t *model = &potop->world.objects.models[objects_instance_model(&potop->world.objects, instance)];

		if (SDL_strcmp(model->spots[k].name, name) == 0)
		{
			return k;
		}
	}
	return -1;
}

int potop_find_path(potop_t *potop, const char *actor_type, float from_x, float from_z, float to_x, float to_z, float (*points_out)[2],
	int maximum, bool *complete_out)
{
	int type = actors_find_type(&potop->world.actors, actor_type);
	navigation_agent_t agent;
	float x = from_x;
	float z = from_z;
	int count = 0;

	if (complete_out != NULL)
	{
		*complete_out = false;
	}
	if (type < 0 || maximum <= 0)
	{
		return 0;
	}
	agent = actors_agent(&potop->world.actors, type);
	// (a search gives so many points at most: on from its last one, a few
	// times, while there is room)
	for (int search = 0; search < 16 && count < maximum; search++)
	{
		float waypoints[NAVIGATION_MAX_WAYPOINTS][2];
		int waypoint_count = 0;
		bool complete = false;
		navigation_result_t result = navigation_find_path(&potop->world.navigation, &potop->world.terrain, &potop->world.water, &agent, x, z,
			terrain_sample_height_meters(&potop->world.terrain, x, z), to_x, to_z, waypoints, &waypoint_count, &complete);

		if (result == NAVIGATION_PATH_NONE || waypoint_count == 0)
		{
			break;
		}
		for (int k = 0; k < waypoint_count; k++)
		{
			if (count >= maximum)
			{
				return count;
			}
			points_out[count][0] = waypoints[k][0];
			points_out[count][1] = waypoints[k][1];
			count++;
		}
		x = waypoints[waypoint_count - 1][0];
		z = waypoints[waypoint_count - 1][1];
		if (complete)
		{
			if (complete_out != NULL)
			{
				*complete_out = result == NAVIGATION_PATH_FOUND;
			}
			break;
		}
	}
	return count;
}

bool potop_actor_can_stand(potop_t *potop, uint32_t id, float x, float z)
{
	int index = actors_find(&potop->world.actors, id);
	navigation_agent_t agent;

	if (index < 0)
	{
		return false;
	}
	agent = actors_agent(&potop->world.actors, potop->world.actors.actors[index].type);
	return navigation_standable(&potop->world.navigation, &potop->world.terrain, &potop->world.water, &agent, x, z);
}

void potop_actor_stop(potop_t *potop, uint32_t id)
{
	actors_stop(&potop->world.actors, id);
}

void potop_actor_look_at(potop_t *potop, uint32_t id, float x, float z)
{
	actors_look_at(&potop->world.actors, id, x, z);
}

void potop_actor_stop_looking(potop_t *potop, uint32_t id)
{
	actors_stop_looking(&potop->world.actors, id);
}

bool potop_actor_kill(potop_t *potop, uint32_t id)
{
	return actors_kill(&potop->world.actors, id);
}

bool potop_actor_hurt(potop_t *potop, uint32_t id)
{
	return actors_hurt(&potop->world.actors, id);
}

bool potop_actor_poll_event(potop_t *potop, potop_actor_event_t *event_out)
{
	actor_event_t event;

	if (!actors_poll_event(&potop->world.actors, &event))
	{
		return false;
	}
	event_out->type = (potop_actor_event_type_t)event.type;
	event_out->id = event.id;
	event_out->game_id = event.game_id;
	event_out->x = event.x;
	event_out->z = event.z;
	return true;
}

// ---------------------------------------------------------------------------
// Items and containers

int potop_item_find(const potop_t *potop, const char *name)
{
	return items_find(&potop->session.items, name);
}

bool potop_item_get(const potop_t *potop, int item, potop_item_info_t *info_out)
{
	const items_t *items = &potop->session.items;
	const item_type_t *type;

	SDL_zerop(info_out);
	if (item < 0 || item >= items->type_count)
	{
		return false;
	}
	type = &items->types[item];
	SDL_strlcpy(info_out->name, type->name, sizeof(info_out->name));
	SDL_strlcpy(info_out->shown_name, type->shown_name, sizeof(info_out->shown_name));
	SDL_strlcpy(info_out->description, type->description, sizeof(info_out->description));
	info_out->category = (potop_item_category_t)type->category;
	info_out->weight_kilograms = type->weight_kilograms;
	info_out->value = type->value;
	info_out->stack = type->stack;
	info_out->usable = type->usable;
	info_out->slot = type->slot;
	info_out->weapon = type->weapon;
	info_out->ammo = type->ammo;
	return true;
}

int potop_item_of_weapon(const potop_t *potop, int weapon)
{
	return items_of_weapon(&potop->session.items, weapon);
}

uint32_t potop_inventory_create(potop_t *potop, const char *name, float max_weight_kilograms)
{
	return inventory_create(&potop->session.inventories, name, max_weight_kilograms);
}

void potop_inventory_destroy(potop_t *potop, uint32_t container)
{
	inventory_destroy(&potop->session.inventories, container);
}

uint32_t potop_player_inventory(const potop_t *potop)
{
	return potop->session.player_inventory;
}

void potop_set_player_inventory(potop_t *potop, uint32_t container)
{
	potop->session.player_inventory = container;
}

int potop_inventory_add(potop_t *potop, uint32_t container, int item, int count)
{
	return inventory_add(&potop->session.inventories, container, item, count);
}

int potop_inventory_remove(potop_t *potop, uint32_t container, int item, int count)
{
	return inventory_remove(&potop->session.inventories, container, item, count);
}

int potop_inventory_count(const potop_t *potop, uint32_t container, int item, bool worn_too)
{
	return item >= 0 ? inventory_count(&potop->session.inventories, container, item, worn_too) : 0;
}

bool potop_inventory_equip(potop_t *potop, uint32_t container, int item)
{
	const container_t *found = inventory_get_const(&potop->session.inventories, container);

	for (int i = 0; found != NULL && i < found->entry_count; i++)
	{
		if (found->entries[i].item == item)
		{
			return inventory_equip(&potop->session.inventories, container, i);
		}
	}
	return false;
}

void potop_inventory_set_lock(potop_t *potop, uint32_t container, bool locked, int key_item)
{
	inventory_set_lock(&potop->session.inventories, container, locked, key_item);
}

bool potop_inventory_get(const potop_t *potop, uint32_t container, potop_inventory_info_t *info_out)
{
	const container_t *found = inventory_get_const(&potop->session.inventories, container);

	SDL_zerop(info_out);
	if (found == NULL)
	{
		return false;
	}
	SDL_strlcpy(info_out->name, found->name, sizeof(info_out->name));
	info_out->entry_count = found->entry_count;
	info_out->weight_kilograms = inventory_weight(&potop->session.inventories, container);
	info_out->max_weight_kilograms = found->max_weight_kilograms;
	info_out->locked = found->locked;
	info_out->key_item = found->key_item;
	return true;
}

bool potop_inventory_entry(const potop_t *potop, uint32_t container, int row, int *item_out, int *count_out)
{
	const container_t *found = inventory_get_const(&potop->session.inventories, container);

	if (found == NULL || row < 0 || row >= found->entry_count)
	{
		return false;
	}
	*item_out = found->entries[row].item;
	*count_out = found->entries[row].count;
	return true;
}

bool potop_inventory_poll_event(potop_t *potop, potop_inventory_event_t *event_out)
{
	inventory_event_t event;

	if (!inventory_poll_event(&potop->session.inventories, &event))
	{
		return false;
	}
	event_out->type = (potop_inventory_event_type_t)event.type;
	event_out->container = event.container;
	event_out->item = event.item;
	event_out->count = event.count;
	return true;
}

// ---------------------------------------------------------------------------
// The journal

void potop_journal_set_quest(potop_t *potop, const char *quest, const char *title, const char *description, potop_quest_state_t state)
{
	journal_set_quest(&potop->session.journal, quest, title, description, (journal_quest_state_t)state);
}

void potop_journal_set_objective(potop_t *potop, const char *quest, const char *objective, const char *text, potop_objective_state_t state,
	const char *progress)
{
	journal_set_objective(&potop->session.journal, quest, objective, text, (journal_objective_state_t)state, progress);
}

void potop_journal_set_place(potop_t *potop, const char *quest, const char *objective, float x, float z)
{
	journal_set_place(&potop->session.journal, quest, objective, x, z);
}

void potop_journal_clear_place(potop_t *potop, const char *quest, const char *objective)
{
	journal_clear_place(&potop->session.journal, quest, objective);
}

bool potop_journal_track(potop_t *potop, const char *quest, bool tracked)
{
	return journal_track(&potop->session.journal, quest, tracked);
}

bool potop_journal_focus(potop_t *potop, const char *quest, const char *objective)
{
	return journal_focus(&potop->session.journal, quest, objective);
}

bool potop_journal_has_quest(const potop_t *potop, const char *quest)
{
	// (journal_find does not change it)
	return journal_find((journal_t *)&potop->session.journal, quest) != NULL;
}

bool potop_journal_get_objective(const potop_t *potop, const char *quest, const char *objective, potop_objective_info_t *info_out)
{
	const journal_objective_t *line = journal_find_objective((journal_t *)&potop->session.journal, quest, objective);

	SDL_zerop(info_out);
	if (line == NULL)
	{
		return false;
	}
	SDL_strlcpy(info_out->text, line->text, sizeof(info_out->text));
	info_out->state = (potop_objective_state_t)line->state;
	SDL_strlcpy(info_out->progress, line->progress, sizeof(info_out->progress));
	info_out->has_place = line->has_place;
	info_out->x = line->x;
	info_out->z = line->z;
	return true;
}

// ---------------------------------------------------------------------------
// Conversations

void potop_dialog_open(potop_t *potop, uint32_t actor, const char *speaker)
{
	dialog_open(&potop->session.dialog, actor, speaker);
}

void potop_dialog_say(potop_t *potop, const char *text)
{
	dialog_say(&potop->session.dialog, text);
}

void potop_dialog_add_choice(potop_t *potop, uint32_t id, const char *text)
{
	dialog_add_choice(&potop->session.dialog, id, text);
}

void potop_dialog_remove_choice(potop_t *potop, uint32_t id)
{
	dialog_remove_choice(&potop->session.dialog, id);
}

void potop_dialog_close(potop_t *potop)
{
	dialog_close(&potop->session.dialog);
}

bool potop_dialog_is_open(const potop_t *potop)
{
	return potop->session.dialog.open;
}

bool potop_dialog_poll_event(potop_t *potop, potop_dialog_event_t *event_out)
{
	dialog_event_t event;

	if (!dialog_poll_event(&potop->session.dialog, &event))
	{
		return false;
	}
	event_out->type = (potop_dialog_event_type_t)event.type;
	event_out->actor = event.actor;
	event_out->choice = event.choice;
	return true;
}

// ---------------------------------------------------------------------------
// The HUD

void potop_notify(potop_t *potop, const char *text, float seconds, potop_notify_style_t style)
{
	hud_notify(&potop->hud, text, seconds, (hud_notify_style_t)style);
}

bool potop_hud_is_open(const potop_t *potop, potop_hud_window_t window)
{
	switch (window)
	{
		case POTOP_HUD_INVENTORY:
			return potop->hud.inventory_open;
		case POTOP_HUD_JOURNAL:
			return potop->hud.journal_open;
		case POTOP_HUD_MAP:
			return potop->hud.map_open;
	}
	return false;
}

void potop_hud_set_open(potop_t *potop, potop_hud_window_t window, bool open)
{
	switch (window)
	{
		case POTOP_HUD_INVENTORY:
			potop->hud.inventory_open = open;
			break;
		case POTOP_HUD_JOURNAL:
			potop->hud.journal_open = open;
			break;
		case POTOP_HUD_MAP:
			potop->hud.map_open = open;
			break;
	}
}

void potop_hud_set_prompt(potop_t *potop, const char *text)
{
	SDL_strlcpy(potop->session.interaction_prompt, text != NULL ? text : "", sizeof(potop->session.interaction_prompt));
}

void potop_debug_show(potop_t *potop, bool shown)
{
	potop->hud.debug_open = shown;
}

bool potop_debug_shown(const potop_t *potop)
{
	return potop->hud.debug_open;
}

void potop_debug_clear(potop_t *potop)
{
	potop->hud.debug_line_count = 0;
}

void potop_debug_line(potop_t *potop, const char *text)
{
	if (potop->hud.debug_line_count < HUD_DEBUG_LINES)
	{
		SDL_strlcpy(potop->hud.debug_lines[potop->hud.debug_line_count++], text, HUD_DEBUG_LINE_LENGTH);
	}
}

void potop_hud_open_loot(potop_t *potop, uint32_t container)
{
	potop->hud.loot_container = container;
}

uint32_t potop_hud_loot(const potop_t *potop)
{
	return potop->hud.loot_container;
}

// ---------------------------------------------------------------------------
// The player

void potop_player_find_target(potop_t *potop, potop_target_t *target_out)
{
	const player_target_t *target = &potop->session.player.target;

	player_find_target(&potop->session.player, &potop->session, &potop->world, &potop->camera);
	SDL_zerop(target_out);
	target_out->kind = (potop_target_kind_t)target->kind;
	target_out->distance = target->distance;
	target_out->item = target->item;
	target_out->any_left = target->any_left;
	target_out->container = target->container;
	target_out->object_id = target->object_id;
	target_out->actor_id = target->actor_id;
	target_out->actor_dead = target->actor_dead;
	target_out->door = target->door;
	target_out->door_open = target->door_open;
	if (target->object_id != 0)
	{
		int index = objects_find_id(&potop->world.objects, target->object_id);

		target_out->owner = index >= 0 ? potop->world.objects.instances[index].owner : 0;
	}
}

bool potop_door_set(potop_t *potop, uint32_t object_id, int door, bool open)
{
	return objects_set_door(&potop->world.objects, object_id, door, open);
}

int potop_door_get(potop_t *potop, uint32_t object_id, int door)
{
	return objects_door(&potop->world.objects, object_id, door);
}

int potop_door_find(potop_t *potop, uint32_t object_id, const char *name)
{
	return objects_door_find(&potop->world.objects, object_id, name);
}

bool potop_door_info(potop_t *potop, uint32_t object_id, int door, potop_door_info_t *info_out)
{
	objects_door_info_t info;

	if (!objects_door_info(&potop->world.objects, object_id, door, &info))
	{
		return false;
	}
	SDL_memcpy(info_out->hinge, info.hinge, sizeof(info.hinge));
	SDL_memcpy(info_out->along, info.along, sizeof(info.along));
	SDL_memcpy(info_out->swing, info.swing, sizeof(info.swing));
	info_out->width = info.width;
	info_out->height = info.height;
	info_out->open = info.open;
	info_out->moving = info.moving;
	return true;
}

potop_pick_result_t potop_player_pick(potop_t *potop, int *item_out, int *count_out)
{
	harvest_outcome_t outcome = player_pick(&potop->session.player, &potop->session, &potop->world);

	*item_out = outcome.item;
	*count_out = outcome.count;
	return (potop_pick_result_t)outcome.result;
}

bool potop_player_pick_up(potop_t *potop)
{
	return player_pick_up(&potop->session.player, &potop->session, &potop->world);
}

int potop_weapon_find(const potop_t *potop, const char *name)
{
	for (int i = 0; i < potop->session.weapons.type_count; i++)
	{
		if (SDL_strcmp(potop->session.weapons.types[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

int potop_ammo_find(const potop_t *potop, const char *name)
{
	for (int i = 0; i < potop->session.weapons.ammo_count; i++)
	{
		if (SDL_strcmp(potop->session.weapons.ammo[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

bool potop_effect_play(potop_t *potop, const char *effect, float x, float y, float z)
{
	int index = particles_find_effect(&potop->world.particles, effect);

	if (index < 0)
	{
		return false;
	}
	particles_play(&potop->world.particles, index, x, y, z);
	return true;
}

bool potop_player_select_weapon(potop_t *potop, int weapon)
{
	return player_select_weapon(&potop->session.player, &potop->session, weapon);
}

bool potop_player_throw(potop_t *potop, int item)
{
	return player_throw(&potop->session.player, &potop->session, &potop->world, &potop->camera, item);
}

bool potop_explode(potop_t *potop, float x, float y, float z, const char *explosive)
{
	int type = explosives_find(&potop->session.explosives, explosive);
	float position[3] = { x, y, z };

	if (type < 0)
	{
		return false;
	}
	explosives_explode(&potop->session.explosives, &potop->session, &potop->world, type, position, -1);
	return true;
}

bool potop_explosive_set_off(potop_t *potop, uint32_t object_id)
{
	return explosives_set_off(&potop->session.explosives, &potop->session, &potop->world, object_id);
}

uint32_t potop_player_place(potop_t *potop, int item)
{
	return player_place(&potop->session.player, &potop->session, &potop->world, &potop->camera, item);
}

// The items.ini item that is an explosive of this type (-1: none)
static int item_of_explosive(const potop_t *potop, int type)
{
	for (int i = 0; i < potop->session.items.type_count; i++)
	{
		if (potop->session.items.types[i].explosive == type)
		{
			return i;
		}
	}
	return -1;
}

bool potop_explosive_get(const potop_t *potop, uint32_t object_id, potop_explosive_info_t *info_out)
{
	const explosives_t *explosives = &potop->session.explosives;
	int type;
	bool armed;
	int owner;

	SDL_zerop(info_out);
	info_out->item = -1;
	if (!explosives_find_object(explosives, object_id, &type, &armed, &owner))
	{
		return false;
	}
	SDL_strlcpy(info_out->explosive, explosives->types[type].name, sizeof(info_out->explosive));
	info_out->armed = armed;
	info_out->buried = explosives->types[type].buried;
	info_out->owner = owner;
	info_out->item = item_of_explosive(potop, type);
	for (int g = 0; g < EXPLOSIVES_GRENADES_MAX; g++)
	{
		if (explosives->grenades[g].used && explosives->grenades[g].object_id == object_id)
		{
			info_out->placed = explosives->grenades[g].placed;
		}
	}
	return true;
}

int potop_explosive_disarm(potop_t *potop, uint32_t object_id)
{
	int type = explosives_take_back(&potop->session.explosives, &potop->world, object_id);

	return type >= 0 ? item_of_explosive(potop, type) : -1;
}

void potop_player_set_buttons(potop_t *potop, bool trigger, bool aim)
{
	potop->session.player.trigger = trigger;
	potop->session.player.aim = aim;
}

int potop_take_hits(potop_t *potop, potop_hit_t *hits_out, int maximum)
{
	projectile_hit_t hits[PROJECTILE_HITS_MAX];
	int count = projectiles_take_hits(&potop->session.projectiles, hits, SDL_min(maximum, PROJECTILE_HITS_MAX));

	for (int i = 0; i < count; i++)
	{
		hits_out[i].kind = (potop_hit_kind_t)hits[i].kind;
		hits_out[i].x = hits[i].point[0];
		hits_out[i].y = hits[i].point[1];
		hits_out[i].z = hits[i].point[2];
		hits_out[i].actor_id = hits[i].actor_id;
		hits_out[i].object_id = hits[i].object_id;
		hits_out[i].part = (potop_body_part_t)hits[i].zone;
		hits_out[i].bone = hits[i].bone;
		hits_out[i].damage = hits[i].damage;
		hits_out[i].damage_type = (potop_damage_type_t)hits[i].damage_type;
		hits_out[i].ammo = hits[i].ammo;
		hits_out[i].owner = hits[i].owner;
	}
	return count;
}

// ---------------------------------------------------------------------------
// Senses

static const potop_stealth_t DEFAULT_STEALTH = { { 2.0f, 10.0f, 25.0f }, { 0.4f, 1.0f, 1.2f } };

static const potop_stealth_t *stealth_of(const potop_t *potop)
{
	return potop->stealth_set ? &potop->stealth : &DEFAULT_STEALTH;
}

void potop_actor_set_running(potop_t *potop, uint32_t id, bool running)
{
	actors_set_running(&potop->world.actors, id, running);
}

bool potop_actor_play(potop_t *potop, uint32_t id, const char *action)
{
	return actors_play(&potop->world.actors, id, action);
}

double potop_world_time(const potop_t *potop)
{
	return potop->world.elapsed_seconds;
}

float potop_actor_sight(potop_t *potop, uint32_t id, float x, float y, float z, float visibility)
{
	float target[3] = { x, y, z };

	return senses_sight(&potop->world, actors_find(&potop->world.actors, id), target, visibility);
}

float potop_actor_sees_player(potop_t *potop, uint32_t id)
{
	const camera_t *camera = &potop->camera;
	const potop_stealth_t *stealth = stealth_of(potop);
	float visibility;
	float target[3];

	if (!camera->first_person)
	{
		return 0.0f;
	}
	// (crawling or lying still: as crawling; else as it goes)
	visibility = camera->walker_crawling ? stealth->visibility[0] : (potop->gait == POTOP_GAIT_RUN ? stealth->visibility[2] : stealth->visibility[1]);
	target[0] = camera->eye_position.x;
	target[1] = camera->eye_position.y - 0.4f * (camera->eye_position.y - camera->walker_feet);
	target[2] = camera->eye_position.z;
	return senses_sight(&potop->world, actors_find(&potop->world.actors, id), target, visibility);
}

int potop_noises_heard(const potop_t *potop, float x, float y, float z, double since_seconds, potop_noise_t *noises_out, int maximum)
{
	noise_t found[64];
	int count = noises_heard(&potop->world.noises, x, y, z, since_seconds, found, SDL_min(maximum, 64));

	for (int i = 0; i < count; i++)
	{
		noises_out[i].kind = (potop_noise_kind_t)found[i].kind;
		noises_out[i].x = found[i].position[0];
		noises_out[i].y = found[i].position[1];
		noises_out[i].z = found[i].position[2];
		noises_out[i].radius_meters = found[i].radius_meters;
		noises_out[i].source = found[i].source;
		noises_out[i].time_seconds = found[i].time_seconds;
	}
	return count;
}

void potop_make_noise(potop_t *potop, float x, float y, float z, float radius_meters, uint32_t source)
{
	float position[3] = { x, y, z };

	noises_make(&potop->world.noises, position, radius_meters, NOISE_OTHER, source, potop->world.elapsed_seconds);
}

void potop_player_set_stealth(potop_t *potop, const potop_stealth_t *stealth)
{
	potop->stealth = *stealth;
	potop->stealth_set = true;
}

void potop_player_get_stealth(const potop_t *potop, potop_stealth_t *stealth_out)
{
	*stealth_out = *stealth_of(potop);
}

// The player's gait and steps, after its walk (the game's frame)
void potop_update_steps(potop_t *potop, float seconds)
{
	const camera_t *camera = &potop->camera;
	const potop_stealth_t *stealth = stealth_of(potop);
	float speed_squared = camera->walker_velocity_x * camera->walker_velocity_x + camera->walker_velocity_z * camera->walker_velocity_z;
	// A step this long (crawl, walk, run)
	static const float STEP_LENGTHS[3] = { 0.5f, 0.75f, 1.2f };
	int kind;

	if (!camera->first_person || !camera->walker_on_ground || camera->walker_swimming || speed_squared < 0.04f)
	{
		potop->gait = POTOP_GAIT_STILL;
		return;
	}
	potop->gait = camera->walker_crawling ? POTOP_GAIT_CRAWL : (potop->input.run_held ? POTOP_GAIT_RUN : POTOP_GAIT_WALK);
	kind = potop->gait - POTOP_GAIT_CRAWL;
	potop->step_meters_walked += SDL_sqrtf(speed_squared) * seconds;
	if (potop->step_meters_walked >= STEP_LENGTHS[kind])
	{
		float feet[3] = { camera->walker_x, camera->walker_feet, camera->walker_z };

		potop->step_meters_walked -= STEP_LENGTHS[kind];
		noises_make(&potop->world.noises, feet, stealth->step_meters[kind], NOISE_STEP, 0, potop->world.elapsed_seconds);
	}
}

// ---------------------------------------------------------------------------
// Light

float potop_sky_open_at(const potop_t *potop, float x, float z)
{
	return sky_occlusion_open_at(&potop->world.sky_occlusion, potop->world.terrain.cell_size_meters, x, z);
}

void potop_light_at(const potop_t *potop, float x, float y, float z, potop_light_info_t *light_out)
{
	light_probe_t probe;

	light_probe_at(&potop->world, x, y, z, &probe);
	light_out->sky_open = probe.sky_open;
	light_out->daylight = probe.daylight;
	light_out->local = probe.local;
	light_out->total = probe.total;
}

// ---------------------------------------------------------------------------
// Areas

// The player's middle, on foot (false: not on foot)
static bool player_middle(const potop_t *potop, float middle_out[3])
{
	if (!potop->camera.first_person)
	{
		return false;
	}
	middle_out[0] = potop->camera.eye_position.x;
	middle_out[1] = potop->camera.eye_position.y - 0.7f;
	middle_out[2] = potop->camera.eye_position.z;
	return true;
}

void potop_update_areas(potop_t *potop)
{
	float middle[3];
	bool on_foot = player_middle(potop, middle);

	areas_update(&potop->world.areas, &potop->world.actors, on_foot ? middle : NULL);
}

int potop_area_list(const potop_t *potop, uint32_t *ids_out, int maximum)
{
	for (int i = 0; i < potop->world.areas.count && i < maximum; i++)
	{
		ids_out[i] = potop->world.areas.areas[i].id;
	}
	return potop->world.areas.count;
}

uint32_t potop_area_find(const potop_t *potop, const char *name)
{
	for (int i = 0; i < potop->world.areas.count; i++)
	{
		if (SDL_strcmp(potop->world.areas.areas[i].name, name) == 0)
		{
			return potop->world.areas.areas[i].id;
		}
	}
	return 0;
}

bool potop_area_get(const potop_t *potop, uint32_t id, potop_area_info_t *info_out)
{
	int index = areas_find(&potop->world.areas, id);
	const area_t *area;

	if (index < 0)
	{
		return false;
	}
	area = &potop->world.areas.areas[index];
	SDL_zerop(info_out);
	info_out->id = area->id;
	SDL_strlcpy(info_out->name, area->name, sizeof(info_out->name));
	info_out->x = area->x;
	info_out->z = area->z;
	info_out->width_meters = 2.0f * area->half_width_meters;
	info_out->depth_meters = 2.0f * area->half_depth_meters;
	info_out->yaw_radians = area->yaw_radians;
	info_out->bottom_meters = area->bottom_meters;
	info_out->top_meters = area->top_meters;
	info_out->triggers = area->triggers;
	info_out->blocks = area->blocks;
	return true;
}

uint32_t potop_area_add(potop_t *potop, const char *name, float x, float z, float width_meters, float depth_meters, float yaw_radians,
	uint8_t triggers)
{
	return areas_add(&potop->world.areas, name, x, z, width_meters, depth_meters, yaw_radians, triggers);
}

bool potop_area_set(potop_t *potop, uint32_t id, const potop_area_info_t *info)
{
	int index = areas_find(&potop->world.areas, id);
	area_t *area;

	if (index < 0)
	{
		return false;
	}
	area = &potop->world.areas.areas[index];
	SDL_strlcpy(area->name, info->name, sizeof(area->name));
	area->x = info->x;
	area->z = info->z;
	area->half_width_meters = SDL_max(0.5f * info->width_meters, 0.05f);
	area->half_depth_meters = SDL_max(0.5f * info->depth_meters, 0.05f);
	area->yaw_radians = info->yaw_radians;
	area->bottom_meters = info->bottom_meters;
	area->top_meters = info->top_meters;
	area->triggers = info->triggers;
	area->blocks = info->blocks;
	return true;
}

bool potop_area_set_triggers(potop_t *potop, uint32_t id, uint8_t triggers)
{
	int index = areas_find(&potop->world.areas, id);

	if (index < 0)
	{
		return false;
	}
	potop->world.areas.areas[index].triggers = triggers;
	return true;
}

bool potop_area_set_blocks(potop_t *potop, uint32_t id, uint8_t blocks)
{
	int index = areas_find(&potop->world.areas, id);

	if (index < 0)
	{
		return false;
	}
	potop->world.areas.areas[index].blocks = blocks;
	return true;
}

bool potop_area_remove(potop_t *potop, uint32_t id)
{
	return areas_remove(&potop->world.areas, id);
}

bool potop_area_contains(const potop_t *potop, uint32_t id, float x, float y, float z)
{
	int index = areas_find(&potop->world.areas, id);

	return index >= 0 && areas_contains(&potop->world.areas.areas[index], x, y, z);
}

int potop_area_occupants(const potop_t *potop, uint32_t id, uint32_t *actor_ids_out, int maximum, bool *player_inside_out)
{
	float middle[3];
	bool on_foot = player_middle(potop, middle);

	return areas_occupants(&potop->world.areas, &potop->world.actors, id, on_foot ? middle : NULL, actor_ids_out, maximum, player_inside_out);
}

bool potop_area_poll_event(potop_t *potop, potop_area_event_t *event_out)
{
	area_event_t event;

	if (!areas_poll_event(&potop->world.areas, &event))
	{
		return false;
	}
	event_out->type = (potop_area_event_type_t)event.type;
	event_out->area_id = event.area_id;
	event_out->player = event.player;
	event_out->actor_id = event.actor_id;
	return true;
}

// ---------------------------------------------------------------------------
// Changing the world by hand

static stamp_t stamp_of(const potop_stamp_t *stamp)
{
	stamp_t made;

	SDL_zero(made);
	made.type = (stamp_type_t)stamp->type;
	made.soil = (soil_type_t)stamp->soil;
	made.radius_meters = stamp->radius_meters;
	made.height_meters = stamp->height_meters;
	made.start = vec3_make(stamp->start_x, stamp->start_y, stamp->start_z);
	made.end = vec3_make(stamp->end_x, stamp->end_y, stamp->end_z);
	return made;
}

bool potop_edit_terrain(potop_t *potop, potop_brush_t brush, float x, float z, float radius_meters, float rate, potop_soil_t soil,
	float seconds)
{
	return edit_terrain(&potop->world, (edit_brush_t)brush, x, z, radius_meters, rate, (soil_type_t)soil, seconds);
}

bool potop_edit_water(potop_t *potop, float x, float z, float radius_meters, float meters_per_second, float seconds)
{
	return edit_water(&potop->world, x, z, radius_meters, meters_per_second, seconds);
}

bool potop_edit_stamp(potop_t *potop, const potop_stamp_t *stamp)
{
	stamp_t made = stamp_of(stamp);

	return edit_stamp(&potop->world, &made);
}

float potop_stamp_footprint_meters(const potop_t *potop, const potop_stamp_t *stamp)
{
	stamp_t made = stamp_of(stamp);

	return stamps_footprint_meters(&potop->world.terrain, &made);
}

bool potop_edit_add_water(potop_t *potop, float x, float z, float cubic_meters, float radius_meters)
{
	return edit_add_water(&potop->world, x, z, cubic_meters, radius_meters);
}

bool potop_edit_toggle_spring(potop_t *potop, float x, float z)
{
	return edit_toggle_spring(&potop->world, x, z);
}

int potop_edit_plant(potop_t *potop, int category, float x, float z, float radius_meters, float seconds)
{
	return edit_plant(&potop->world, category, x, z, radius_meters, seconds, &potop->edit_random);
}

int potop_edit_clear(potop_t *potop, float x, float z, float radius_meters)
{
	return edit_clear(&potop->world, x, z, radius_meters);
}


bool potop_edit_place_layout(potop_t *potop, int layout, float x, float z)
{
	return edit_place_layout(&potop->world, layout, x, z, &potop->edit_random);
}

int potop_edit_remove_layout(potop_t *potop, float x, float z)
{
	return edit_remove_layout(&potop->world, x, z);
}

bool potop_edit_place_light(potop_t *potop, float x, float y, float z, const potop_light_t *light)
{
	float position[3] = { x, y, z };
	light_source_t source;

	light_source_reset(&source);
	SDL_memcpy(source.color, light->color, sizeof(source.color));
	source.intensity = light->intensity;
	source.radius_meters = light->radius_meters;
	source.flicker = light->flicker;
	source.core_meters = light->core_meters;
	source.switched = light->switched;
	return edit_place_light(&potop->world, position, &source);
}

bool potop_edit_remove_light(potop_t *potop, float x, float y, float z, float radius_meters)
{
	float position[3] = { x, y, z };

	return edit_remove_light(&potop->world, position, radius_meters);
}

void potop_debug_obstacles(potop_t *potop, float x0, float z0, float cell_meters, int cells, float height, uint8_t *out)
{
	world_t *world = &potop->world;

	for (int row = 0; row < cells; row++)
	{
		for (int column = 0; column < cells; column++)
		{
			float x = x0 + ((float)column + 0.5f) * cell_meters;
			float z = z0 + ((float)row + 0.5f) * cell_meters;
			float y = terrain_sample_height_meters(&world->terrain, x, z) + height;
			bool hit = false;

			for (int axis = 0; axis < 2 && !hit && !world->physics_off; axis++)
			{
				float from[3] = { x - (axis == 0 ? cell_meters * 0.5f : 0.0f), y, z - (axis == 1 ? cell_meters * 0.5f : 0.0f) };
				float to[3] = { x + (axis == 0 ? cell_meters * 0.5f : 0.0f), y, z + (axis == 1 ? cell_meters * 0.5f : 0.0f) };
				physics_cast_t cast;

				hit = physics_cast_segment(&world->physics, from, to, &cast) && cast.body >= 0;
			}
			out[row * cells + column] = hit ? 1u : 0u;
		}
	}
}
