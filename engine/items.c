// Items. See items.h.

#include "items.h"

#include <SDL3/SDL.h>

#include "ini_parser.h"

static const char *const category_names[ITEM_CATEGORY_COUNT] = { "misc", "weapon", "ammo", "food", "material", "clothing", "tool", "quest",
	"money" };

const char *items_category_name(item_category_t category)
{
	return category >= 0 && category < ITEM_CATEGORY_COUNT ? category_names[category] : "misc";
}

static void item_defaults(item_type_t *type, const char *name)
{
	SDL_zerop(type);
	SDL_strlcpy(type->name, name, sizeof(type->name));
	SDL_strlcpy(type->shown_name, name, sizeof(type->shown_name));
	type->category = ITEM_CATEGORY_MISC;
	type->weight_kilograms = 0.5f;
	type->stack = 1;
	type->slot = -1;
	type->weapon = -1;
	type->ammo = -1;
	type->explosive = -1;
	SDL_strlcpy(type->model, "sack2", sizeof(type->model));
	type->size_meters = 0.3f;
}

static item_type_t *item_named(items_t *items, const char *name)
{
	int index = items_find(items, name);

	if (index >= 0)
	{
		return &items->types[index];
	}
	if (items->type_count >= ITEM_TYPES_MAX)
	{
		return NULL;
	}
	item_defaults(&items->types[items->type_count], name);
	return &items->types[items->type_count++];
}

typedef struct
{
	items_t *items;
	// The slot names come before the items or after: an item's slot is
	// looked up once all is read
	char item_slots[ITEM_TYPES_MAX][24];
} load_state_t;

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;
	items_t *items = state->items;
	item_type_t *type;

	if (section == NULL || section[0] == '\0')
	{
		return false;
	}
	if (SDL_strcmp(section, "slots") == 0)
	{
		char copy[256];
		char *names[ITEM_SLOTS_MAX];
		int count;

		if (SDL_strcmp(key, "names") != 0)
		{
			return false;
		}
		SDL_strlcpy(copy, value, sizeof(copy));
		count = ini_split_list(copy, names, ITEM_SLOTS_MAX);
		items->slot_count = count;
		for (int i = 0; i < count; i++)
		{
			SDL_strlcpy(items->slot_names[i], names[i], sizeof(items->slot_names[i]));
		}
		return true;
	}
	type = item_named(items, section);
	if (type == NULL)
	{
		return false;
	}
	if (SDL_strcmp(key, "name") == 0)
	{
		SDL_strlcpy(type->shown_name, value, sizeof(type->shown_name));
		return true;
	}
	if (SDL_strcmp(key, "description") == 0)
	{
		SDL_strlcpy(type->description, value, sizeof(type->description));
		return true;
	}
	if (SDL_strcmp(key, "category") == 0)
	{
		for (int i = 0; i < ITEM_CATEGORY_COUNT; i++)
		{
			if (SDL_strcasecmp(value, category_names[i]) == 0)
			{
				type->category = (item_category_t)i;
				return true;
			}
		}
		return false;
	}
	if (SDL_strcmp(key, "weight") == 0)
	{
		return ini_value_to_float(value, &type->weight_kilograms);
	}
	if (SDL_strcmp(key, "value") == 0)
	{
		return ini_value_to_float(value, &type->value);
	}
	if (SDL_strcmp(key, "usable") == 0)
	{
		return ini_value_to_bool(value, &type->usable);
	}
	if (SDL_strcmp(key, "stack") == 0)
	{
		return ini_value_to_int(value, &type->stack);
	}
	if (SDL_strcmp(key, "slot") == 0)
	{
		SDL_strlcpy(state->item_slots[type - items->types], value, sizeof(state->item_slots[0]));
		return true;
	}
	if (SDL_strcmp(key, "weapon") == 0)
	{
		SDL_strlcpy(type->weapon_name, value, sizeof(type->weapon_name));
		return true;
	}
	if (SDL_strcmp(key, "explosive") == 0)
	{
		SDL_strlcpy(type->explosive_name, value, sizeof(type->explosive_name));
		return true;
	}
	if (SDL_strcmp(key, "ammo") == 0)
	{
		SDL_strlcpy(type->ammo_name, value, sizeof(type->ammo_name));
		return true;
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
	return false;
}

bool items_load(items_t *items, const weapons_t *weapons)
{
	load_state_t *state = SDL_calloc(1, sizeof(load_state_t));

	SDL_zerop(items);
	if (state == NULL)
	{
		return false;
	}
	state->items = items;
	if (!ini_parse_data_file("items.ini", load_key, state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Items: no data/items.ini");
	}
	if (items->slot_count == 0)
	{
		static const char *const default_slots[] = { "hands", "head", "body", "legs", "feet", "back" };

		items->slot_count = (int)SDL_arraysize(default_slots);
		for (int i = 0; i < items->slot_count; i++)
		{
			SDL_strlcpy(items->slot_names[i], default_slots[i], sizeof(items->slot_names[i]));
		}
	}
	for (int i = 0; i < items->type_count; i++)
	{
		item_type_t *type = &items->types[i];

		type->stack = SDL_max(type->stack, 1);
		if (state->item_slots[i][0] != '\0')
		{
			type->slot = items_find_slot(items, state->item_slots[i]);
			if (type->slot < 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Items: [%s] goes in \"%s\", which [slots] does not name", type->name, state->item_slots[i]);
			}
		}
		if (type->weapon_name[0] != '\0' && weapons != NULL)
		{
			for (int k = 0; k < weapons->type_count; k++)
			{
				if (SDL_strcmp(weapons->types[k].name, type->weapon_name) == 0)
				{
					type->weapon = k;
				}
			}
			if (type->weapon < 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Items: [%s] is weapon \"%s\", which weapons.ini does not have", type->name, type->weapon_name);
			}
		}
		if (type->ammo_name[0] != '\0' && weapons != NULL)
		{
			for (int k = 0; k < weapons->ammo_count; k++)
			{
				if (SDL_strcmp(weapons->ammo[k].name, type->ammo_name) == 0)
				{
					type->ammo = k;
				}
			}
			if (type->ammo < 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Items: [%s] is ammo \"%s\", which ammo.ini does not have", type->name, type->ammo_name);
			}
		}
	}
	SDL_free(state);
	SDL_Log("Items: %d, %d body slots", items->type_count, items->slot_count);
	return items->type_count > 0;
}

int items_find(const items_t *items, const char *name)
{
	for (int i = 0; i < items->type_count; i++)
	{
		if (SDL_strcmp(items->types[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

int items_of_weapon(const items_t *items, int weapon)
{
	for (int i = 0; i < items->type_count && weapon >= 0; i++)
	{
		if (items->types[i].weapon == weapon)
		{
			return i;
		}
	}
	return -1;
}

int items_of_ammo(const items_t *items, int ammo)
{
	for (int i = 0; i < items->type_count && ammo >= 0; i++)
	{
		if (items->types[i].ammo == ammo)
		{
			return i;
		}
	}
	return -1;
}

int items_find_slot(const items_t *items, const char *name)
{
	for (int i = 0; i < items->slot_count; i++)
	{
		if (SDL_strcasecmp(items->slot_names[i], name) == 0)
		{
			return i;
		}
	}
	return -1;
}
