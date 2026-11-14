// Things lying on the ground. See ground_items.h.

#include "ground_items.h"

#include <SDL3/SDL.h>

// How high over the ground a dropped thing starts (it falls from there)
static const float DROP_LIFT_METERS = 0.35f;

void ground_items_destroy(ground_items_t *ground, inventories_t *inventories)
{
	ground_items_clear(ground, inventories);
	SDL_free(ground->list);
	SDL_zerop(ground);
}

void ground_items_clear(ground_items_t *ground, inventories_t *inventories)
{
	for (int i = 0; i < ground->count && inventories != NULL; i++)
	{
		inventory_destroy(inventories, ground->list[i].container);
	}
	ground->count = 0;
}

// The object type its item lies as: its model, else a sack, else a crate
static int type_for_item(const objects_t *objects, const item_type_t *type)
{
	static const char *const fallbacks[] = { "sack2", "sack1", "crate", "basket1" };
	int found = object_types_find(&objects->types, type->model);

	for (size_t i = 0; found < 0 && i < SDL_arraysize(fallbacks); i++)
	{
		found = object_types_find(&objects->types, fallbacks[i]);
	}
	if (found >= 0 && objects->types.types[found].model < 0)
	{
		found = -1;
	}
	return found;
}

// Its model made as big as the item says
static float scale_for_item(const objects_t *objects, int object_type, const item_type_t *type)
{
	const object_model_t *model = &objects->models[objects->types.types[object_type].model];
	float extent = SDL_max(model->bounds_maximum.x - model->bounds_minimum.x, model->bounds_maximum.z - model->bounds_minimum.z);

	extent = SDL_max(extent, model->bounds_maximum.y - model->bounds_minimum.y);
	return SDL_clamp(type->size_meters / SDL_max(extent, 0.01f), 0.05f, 3.0f);
}

int ground_items_place(ground_items_t *ground, objects_t *objects, const terrain_t *terrain, inventories_t *inventories, int item, int count,
	float x, float z, float yaw_radians, int group)
{
	const item_type_t *type;
	int object_type;
	uint32_t container;
	int placed;

	if (item < 0 || item >= inventories->items->type_count || count <= 0)
	{
		return 0;
	}
	type = &inventories->items->types[item];
	object_type = type_for_item(objects, type);
	if (object_type < 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Ground items: no object to lay %s down as (objects.ini: %s, or a sack, a crate)",
			type->name, type->model);
		return 0;
	}
	if (ground->count == ground->capacity)
	{
		int capacity = ground->capacity > 0 ? ground->capacity * 2 : 32;
		ground_item_t *grown = SDL_realloc(ground->list, sizeof(ground_item_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return 0;
		}
		ground->list = grown;
		ground->capacity = capacity;
	}
	container = inventory_create(inventories, type->shown_name, 0.0f);
	if (container == 0)
	{
		return 0;
	}
	if (group <= 0)
	{
		group = objects_new_group(objects);
	}
	if (!objects_add_lifted(objects, terrain, object_type, x, z, yaw_radians, scale_for_item(objects, object_type, type), group,
		DROP_LIFT_METERS))
	{
		inventory_destroy(inventories, container);
		return 0;
	}
	placed = inventory_add(inventories, container, item, count);
	ground->list[ground->count].group = group;
	ground->list[ground->count].container = container;
	ground->list[ground->count].object_id = objects->instances[objects->instance_count - 1].id;
	ground->count++;
	return placed;
}

int ground_items_drop(ground_items_t *ground, objects_t *objects, const terrain_t *terrain, inventories_t *inventories, uint32_t from,
	int item, int count, float x, float z, float yaw_radians)
{
	int there = SDL_min(inventory_count(inventories, from, item, false), count);
	int placed;

	if (there <= 0)
	{
		return 0;
	}
	placed = ground_items_place(ground, objects, terrain, inventories, item, there, x, z, yaw_radians, 0);
	inventory_remove(inventories, from, item, placed);
	return placed;
}

int ground_items_instance(ground_items_t *ground, objects_t *objects, int index)
{
	ground_item_t *entry;
	int found;

	if (index < 0 || index >= ground->count)
	{
		return -1;
	}
	entry = &ground->list[index];
	// (by its object's id; not known yet (a loaded map's), by its group
	// among all the objects, once)
	if (entry->object_id != 0)
	{
		found = objects_find_id(objects, entry->object_id);
		if (found >= 0 && objects->instances[found].group == entry->group)
		{
			return found;
		}
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		if (objects->instances[i].group == entry->group)
		{
			entry->object_id = objects->instances[i].id;
			return i;
		}
	}
	entry->object_id = 0;
	return -1;
}

int ground_items_of_instance(const ground_items_t *ground, const objects_t *objects, int instance)
{
	int group;

	if (instance < 0 || instance >= objects->instance_count)
	{
		return -1;
	}
	group = objects->instances[instance].group;
	for (int i = 0; i < ground->count && group != 0; i++)
	{
		if (ground->list[i].group == group)
		{
			return i;
		}
	}
	return -1;
}

static void forget(ground_items_t *ground, inventories_t *inventories, int index)
{
	inventory_destroy(inventories, ground->list[index].container);
	ground->list[index] = ground->list[ground->count - 1];
	ground->count--;
}

int ground_items_pick_up(ground_items_t *ground, objects_t *objects, inventories_t *inventories, int index, uint32_t to)
{
	const container_t *container;
	int moved;

	if (index < 0 || index >= ground->count)
	{
		return 0;
	}
	moved = inventory_move_all(inventories, ground->list[index].container, to);
	container = inventory_get_const(inventories, ground->list[index].container);
	if (container == NULL || container->entry_count == 0)
	{
		objects_kill_group(objects, ground->list[index].group);
		forget(ground, inventories, index);
	}
	return moved;
}

void ground_items_forget_missing(ground_items_t *ground, objects_t *objects, inventories_t *inventories)
{
	for (int i = ground->count - 1; i >= 0; i--)
	{
		if (ground_items_instance(ground, objects, i) < 0)
		{
			forget(ground, inventories, i);
		}
	}
}

bool ground_items_adopt(ground_items_t *ground, inventories_t *inventories, int group, int item, int count)
{
	uint32_t container = 0;

	for (int i = 0; i < ground->count; i++)
	{
		if (ground->list[i].group == group)
		{
			container = ground->list[i].container;
		}
	}
	if (container == 0)
	{
		const char *name = item >= 0 && item < inventories->items->type_count ? inventories->items->types[item].shown_name : "";

		if (ground->count == ground->capacity)
		{
			int capacity = ground->capacity > 0 ? ground->capacity * 2 : 32;
			ground_item_t *grown = SDL_realloc(ground->list, sizeof(ground_item_t) * (size_t)capacity);

			if (grown == NULL)
			{
				return false;
			}
			ground->list = grown;
			ground->capacity = capacity;
		}
		container = inventory_create(inventories, name, 0.0f);
		if (container == 0)
		{
			return false;
		}
		ground->list[ground->count].group = group;
		ground->list[ground->count].container = container;
		ground->list[ground->count].object_id = 0;
		ground->count++;
	}
	inventory_add(inventories, container, item, count);
	return true;
}
