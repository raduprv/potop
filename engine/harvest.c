// Picking things off the map. See harvest.h.

#include "harvest.h"

#include <SDL3/SDL.h>

static float random_unit(uint32_t *state)
{
	*state = *state * 1664525u + 1013904223u;
	return (float)(*state >> 8) / 16777216.0f;
}

// The type whose harvest counts: what it was before it turned
static const object_type_t *harvest_type_of(const objects_t *objects, const object_instance_t *instance)
{
	int type = instance->harvest_original_type >= 0 ? instance->harvest_original_type : instance->type;

	return type >= 0 && type < objects->types.type_count ? &objects->types.types[type] : NULL;
}

int harvest_item_of(const objects_t *objects, const items_t *items, int instance, bool *any_left_out)
{
	const object_instance_t *object;
	const object_type_t *type;

	*any_left_out = false;
	if (instance < 0 || instance >= objects->instance_count)
	{
		return -1;
	}
	object = &objects->instances[instance];
	type = harvest_type_of(objects, object);
	if (type == NULL || type->harvest_item[0] == '\0')
	{
		return -1;
	}
	*any_left_out = object->harvested < type->harvest_times;
	return items_find(items, type->harvest_item);
}

// An object picked out, to grow again: its id on the list harvest_update
// goes through (not every object of the map)
static void wait_to_regrow(objects_t *objects, uint32_t id)
{
	if (!objects->regrow_known)
	{
		return;
	}
	if (objects->regrow_count >= objects->regrow_capacity)
	{
		int capacity = SDL_max(objects->regrow_capacity * 2, 64);
		uint32_t *grown = SDL_realloc(objects->regrow_ids, sizeof(uint32_t) * (size_t)capacity);

		if (grown == NULL)
		{
			objects->regrow_known = false;
			return;
		}
		objects->regrow_ids = grown;
		objects->regrow_capacity = capacity;
	}
	objects->regrow_ids[objects->regrow_count++] = id;
}

// Whether an object waits to grow again
static bool waits_to_regrow(const objects_t *objects, const object_instance_t *object)
{
	const object_type_t *type;

	if (object->harvested == 0)
	{
		return false;
	}
	type = harvest_type_of(objects, object);
	return type != NULL && object->harvested >= type->harvest_times && type->harvest_regrow_hours > 0.0f;
}

// The object as another type, where it stands (its id, the count, whose it
// is kept: objects_retype); its index (the same), -1 if there is no such
// type
static int retype(objects_t *objects, const terrain_t *terrain, int instance, int new_type)
{
	(void)terrain;
	if (new_type < 0 || new_type == objects->instances[instance].type)
	{
		return instance;
	}
	return objects_retype(objects, instance, new_type) ? instance : -1;
}

harvest_outcome_t harvest_pick(objects_t *objects, const terrain_t *terrain, inventories_t *inventories, int instance, uint32_t container,
	uint32_t *random_state)
{
	harvest_outcome_t outcome = { HARVEST_NOTHING, -1, 0, false };
	object_instance_t *object;
	const object_type_t *type;
	bool any_left;
	int wanted;

	outcome.item = harvest_item_of(objects, inventories->items, instance, &any_left);
	if (outcome.item < 0)
	{
		return outcome;
	}
	if (!any_left)
	{
		outcome.result = HARVEST_PICKED_OUT;
		return outcome;
	}
	object = &objects->instances[instance];
	type = harvest_type_of(objects, object);
	wanted = type->harvest_count_minimum
		+ (int)(random_unit(random_state) * (float)(type->harvest_count_maximum - type->harvest_count_minimum + 1));
	wanted = SDL_clamp(wanted, type->harvest_count_minimum, type->harvest_count_maximum);
	outcome.count = inventory_add(inventories, container, outcome.item, wanted);
	if (outcome.count == 0)
	{
		outcome.result = HARVEST_TOO_HEAVY;
		return outcome;
	}
	outcome.result = HARVEST_PICKED;
	if (type->harvest_removes)
	{
		// (gone where it stood: no sort of the list for it)
		objects_kill(objects, instance);
		outcome.removed = true;
		return outcome;
	}
	object->harvested++;
	if (object->harvested >= type->harvest_times)
	{
		int original = object->type;

		// Picked out: grows again later (or never), may look so meanwhile
		object->regrow_hours = type->harvest_regrow_hours;
		if (type->harvest_regrow_hours > 0.0f)
		{
			wait_to_regrow(objects, object->id);
		}
		if (type->harvest_change_type >= 0 && object->harvest_original_type < 0)
		{
			object->harvest_original_type = original;
			if (retype(objects, terrain, instance, type->harvest_change_type) < 0)
			{
				outcome.removed = true;
			}
		}
	}
	return outcome;
}

void harvest_update(objects_t *objects, const terrain_t *terrain, float hours)
{
	int kept = 0;

	if (hours <= 0.0f)
	{
		return;
	}
	// (not known (a map loaded): found among them all, once)
	if (!objects->regrow_known)
	{
		objects->regrow_known = true;
		objects->regrow_count = 0;
		for (int i = 0; i < objects->instance_count; i++)
		{
			if (waits_to_regrow(objects, &objects->instances[i]))
			{
				wait_to_regrow(objects, objects->instances[i].id);
			}
		}
	}
	for (int n = 0; n < objects->regrow_count; n++)
	{
		uint32_t id = objects->regrow_ids[n];
		int i = objects_find_id(objects, id);
		object_instance_t *object;

		if (i < 0 || !waits_to_regrow(objects, &objects->instances[i]))
		{
			continue;
		}
		object = &objects->instances[i];
		object->regrow_hours -= hours;
		if (object->regrow_hours > 0.0f)
		{
			objects->regrow_ids[kept++] = id;
			continue;
		}
		// Grown again: whole, as it was (its id kept by retype)
		object->harvested = 0;
		object->regrow_hours = 0.0f;
		if (object->harvest_original_type >= 0)
		{
			int original = object->harvest_original_type;
			int index = retype(objects, terrain, i, original);

			if (index >= 0)
			{
				objects->instances[index].harvest_original_type = -1;
			}
		}
	}
	objects->regrow_count = SDL_min(kept, objects->regrow_count);
}

void harvest_set(objects_t *objects, int type, const char *item, int least, int most, bool removes, int times, float regrow_hours)
{
	object_type_t *object_type;

	if (type < 0 || type >= objects->types.type_count)
	{
		return;
	}
	object_type = &objects->types.types[type];
	SDL_strlcpy(object_type->harvest_item, item != NULL ? item : "", sizeof(object_type->harvest_item));
	object_type->harvest_count_minimum = SDL_max(least, 0);
	object_type->harvest_count_maximum = SDL_max(most, object_type->harvest_count_minimum);
	object_type->harvest_removes = removes;
	object_type->harvest_times = SDL_max(times, 1);
	object_type->harvest_regrow_hours = SDL_max(regrow_hours, 0.0f);
}
