// The player's side of the game. See session.h.

#include "session.h"

#include <SDL3/SDL.h>

#include "world.h"
#include "harvest.h"
#include "ground_items.h"

// The items for the map's file: the player's (in the pack and worn), and
// what lies on the ground (each by its object's group)
void session_gather_items(session_t *session, world_t *world, map_features_t *features)
{
	const container_t *player = inventory_get_const(&session->inventories, session->player_inventory);
	int capacity = INVENTORY_ENTRIES_MAX + ITEM_SLOTS_MAX;

	for (int i = 0; i < world->ground_items.count; i++)
	{
		const container_t *lying = inventory_get_const(&session->inventories, world->ground_items.list[i].container);

		capacity += lying != NULL ? lying->entry_count : 0;
	}
	features->items = SDL_calloc((size_t)capacity, sizeof(map_item_t));
	features->item_count = 0;
	if (features->items == NULL)
	{
		return;
	}
	for (int i = 0; player != NULL && i < player->entry_count; i++)
	{
		map_item_t *record = &features->items[features->item_count++];

		record->group = -1;
		SDL_strlcpy(record->item, session->items.types[player->entries[i].item].name, sizeof(record->item));
		record->count = player->entries[i].count;
		record->slot = -1;
	}
	for (int s = 0; player != NULL && s < ITEM_SLOTS_MAX; s++)
	{
		if (player->equipped[s] >= 0)
		{
			map_item_t *record = &features->items[features->item_count++];

			record->group = -1;
			SDL_strlcpy(record->item, session->items.types[player->equipped[s]].name, sizeof(record->item));
			record->count = 1;
			record->slot = s;
		}
	}
	ground_items_forget_missing(&world->ground_items, &world->objects, &session->inventories);
	for (int i = 0; i < world->ground_items.count; i++)
	{
		const container_t *lying = inventory_get_const(&session->inventories, world->ground_items.list[i].container);

		for (int k = 0; lying != NULL && k < lying->entry_count && features->item_count < capacity; k++)
		{
			map_item_t *record = &features->items[features->item_count++];

			record->group = world->ground_items.list[i].group;
			SDL_strlcpy(record->item, session->items.types[lying->entries[k].item].name, sizeof(record->item));
			record->count = lying->entries[k].count;
			record->slot = -1;
		}
	}
}

// A loaded map's items: what lies on the ground (its objects came with the
// map), and the player's pack if the map has it
void session_apply_items(session_t *session, world_t *world, const map_features_t *features)
{
	bool player_cleared = false;

	ground_items_clear(&world->ground_items, &session->inventories);
	for (int i = 0; i < features->item_count; i++)
	{
		const map_item_t *record = &features->items[i];
		int item = items_find(&session->items, record->item);

		if (item < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: items.ini has no %s any more: left out", record->item);
			continue;
		}
		if (record->group >= 0)
		{
			ground_items_adopt(&world->ground_items, &session->inventories, record->group, item, record->count);
			continue;
		}
		if (!player_cleared)
		{
			container_t *player = inventory_get(&session->inventories, session->player_inventory);

			player_cleared = true;
			if (player != NULL)
			{
				player->entry_count = 0;
				for (int s = 0; s < ITEM_SLOTS_MAX; s++)
				{
					player->equipped[s] = -1;
				}
			}
		}
		if (record->slot >= 0 && record->slot < ITEM_SLOTS_MAX)
		{
			container_t *player = inventory_get(&session->inventories, session->player_inventory);

			if (player != NULL)
			{
				player->equipped[record->slot] = item;
			}
		}
		else
		{
			inventory_add(&session->inventories, session->player_inventory, item, record->count);
		}
	}
	{
		inventory_event_t event;

		// (what the load put in is no news)
		while (inventory_poll_event(&session->inventories, &event))
		{
		}
	}
}

void session_update(session_t *session, world_t *world, float seconds)
{
	world->ground_items_check_seconds -= seconds;
	if (world->ground_items_check_seconds <= 0.0f)
	{
		world->ground_items_check_seconds = 1.0f;
		ground_items_forget_missing(&world->ground_items, &world->objects, &session->inventories);
		harvest_update(&world->objects, &world->terrain, (float)world->harvest_hours_pending);
		world->harvest_hours_pending = 0.0;
	}
}

void session_update_world_map(session_t *session, world_t *world, const char *shown_name)
{
	if (session->world_map_stale || session->world_map.texture == NULL || SDL_GetTicks() - session->world_map.made_ticks > 30000
		|| SDL_strcmp(shown_name, session->world_map.made_for) != 0)
	{
		terrain_renderer_bake_materials(&world->renderer, &world->terrain, &world->lit_render);
		world_map_make(&session->world_map, &world->terrain, &world->water, &world->objects, shown_name);
		session->world_map_stale = false;
	}
}

void session_create(session_t *session, world_t *world)
{
	weapons_load(&session->weapons);
	items_load(&session->items, &session->weapons);
	explosives_load(&session->explosives);
	for (int i = 0; i < session->items.type_count; i++)
	{
		item_type_t *item = &session->items.types[i];

		if (item->explosive_name[0] != '\0')
		{
			item->explosive = explosives_find(&session->explosives, item->explosive_name);
			if (item->explosive < 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Items: [%s] is explosive \"%s\", which explosives.ini does not have", item->name,
					item->explosive_name);
			}
		}
	}
	inventories_create(&session->inventories, &session->items);
	markers_create(&session->markers);
	journal_create(&session->journal);
	dialog_create(&session->dialog);
	session->world_map_stale = true;
	projectiles_create(&session->projectiles, &world->particles);
	viewmodel_create(&session->viewmodel);
	player_init(&session->player);
}

void session_destroy(session_t *session, world_t *world)
{
	viewmodel_destroy(&session->viewmodel);
	ground_items_destroy(&world->ground_items, &session->inventories);
	world_map_destroy(&session->world_map);
	inventories_destroy(&session->inventories);
}
