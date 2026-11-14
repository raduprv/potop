// Inventories. See inventory.h.

#include "inventory.h"

#include <SDL3/SDL.h>

void inventories_create(inventories_t *inventories, const items_t *items)
{
	SDL_zerop(inventories);
	inventories->items = items;
	inventories->next_id = 1;
}

void inventories_destroy(inventories_t *inventories)
{
	SDL_free(inventories->containers);
	SDL_zerop(inventories);
}

void inventories_clear(inventories_t *inventories)
{
	inventories->count = 0;
	inventories->event_count = 0;
}

static void push_event(inventories_t *inventories, inventory_event_type_t type, uint32_t container, int item, int count)
{
	inventory_event_t *event;

	if (count <= 0)
	{
		return;
	}
	if (inventories->event_count == INVENTORY_EVENT_QUEUE)
	{
		// (the oldest goes)
		inventories->event_first = (inventories->event_first + 1) % INVENTORY_EVENT_QUEUE;
		inventories->event_count--;
	}
	event = &inventories->events[(inventories->event_first + inventories->event_count) % INVENTORY_EVENT_QUEUE];
	event->type = type;
	event->container = container;
	event->item = item;
	event->count = count;
	inventories->event_count++;
}

bool inventory_poll_event(inventories_t *inventories, inventory_event_t *event_out)
{
	if (inventories->event_count == 0)
	{
		return false;
	}
	*event_out = inventories->events[inventories->event_first];
	inventories->event_first = (inventories->event_first + 1) % INVENTORY_EVENT_QUEUE;
	inventories->event_count--;
	return true;
}

uint32_t inventory_create(inventories_t *inventories, const char *name, float max_weight_kilograms)
{
	container_t *container;

	if (inventories->count == inventories->capacity)
	{
		int capacity = inventories->capacity > 0 ? inventories->capacity * 2 : 16;
		container_t *grown = SDL_realloc(inventories->containers, sizeof(container_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return 0;
		}
		inventories->containers = grown;
		inventories->capacity = capacity;
	}
	container = &inventories->containers[inventories->count++];
	SDL_zerop(container);
	container->id = inventories->next_id++;
	SDL_strlcpy(container->name, name != NULL ? name : "", sizeof(container->name));
	container->max_weight_kilograms = SDL_max(max_weight_kilograms, 0.0f);
	container->key_item = -1;
	for (int s = 0; s < ITEM_SLOTS_MAX; s++)
	{
		container->equipped[s] = -1;
	}
	return container->id;
}

void inventory_destroy(inventories_t *inventories, uint32_t id)
{
	for (int i = 0; i < inventories->count; i++)
	{
		if (inventories->containers[i].id == id)
		{
			inventories->containers[i] = inventories->containers[inventories->count - 1];
			inventories->count--;
			return;
		}
	}
}

container_t *inventory_get(inventories_t *inventories, uint32_t id)
{
	for (int i = 0; i < inventories->count && id != 0; i++)
	{
		if (inventories->containers[i].id == id)
		{
			return &inventories->containers[i];
		}
	}
	return NULL;
}

const container_t *inventory_get_const(const inventories_t *inventories, uint32_t id)
{
	return inventory_get((inventories_t *)inventories, id);
}

void inventory_set_lock(inventories_t *inventories, uint32_t id, bool locked, int key_item)
{
	container_t *container = inventory_get(inventories, id);

	if (container != NULL)
	{
		container->locked = locked;
		container->key_item = key_item;
	}
}

void inventory_set_max_weight(inventories_t *inventories, uint32_t id, float kilograms)
{
	container_t *container = inventory_get(inventories, id);

	if (container != NULL)
	{
		container->max_weight_kilograms = SDL_max(kilograms, 0.0f);
	}
}

static float item_weight(const inventories_t *inventories, int item)
{
	return item >= 0 && item < inventories->items->type_count ? inventories->items->types[item].weight_kilograms : 0.0f;
}

float inventory_weight(const inventories_t *inventories, uint32_t id)
{
	const container_t *container = inventory_get_const(inventories, id);
	float weight = 0.0f;

	if (container == NULL)
	{
		return 0.0f;
	}
	for (int i = 0; i < container->entry_count; i++)
	{
		weight += item_weight(inventories, container->entries[i].item) * (float)container->entries[i].count;
	}
	for (int s = 0; s < ITEM_SLOTS_MAX; s++)
	{
		weight += container->equipped[s] >= 0 ? item_weight(inventories, container->equipped[s]) : 0.0f;
	}
	return weight;
}

// Into the rows, no weight checked, no event; how many went in (the rows
// may be full)
static int put(container_t *container, const item_type_t *type, int item, int count)
{
	int left = count;

	// Onto the rows of it there are first, then new ones
	for (int i = 0; i < container->entry_count && left > 0; i++)
	{
		inventory_entry_t *entry = &container->entries[i];

		if (entry->item == item && entry->count < type->stack)
		{
			int room = SDL_min(type->stack - entry->count, left);

			entry->count += room;
			left -= room;
		}
	}
	while (left > 0 && container->entry_count < INVENTORY_ENTRIES_MAX)
	{
		inventory_entry_t *entry = &container->entries[container->entry_count++];

		entry->item = item;
		entry->count = SDL_min(type->stack, left);
		left -= entry->count;
	}
	return count - left;
}

int inventory_add(inventories_t *inventories, uint32_t id, int item, int count)
{
	container_t *container = inventory_get(inventories, id);
	const item_type_t *type;
	int fits = count;
	int added;

	if (container == NULL || item < 0 || item >= inventories->items->type_count || count <= 0)
	{
		return 0;
	}
	type = &inventories->items->types[item];
	if (container->max_weight_kilograms > 0.0f && type->weight_kilograms > 0.0f)
	{
		float room = container->max_weight_kilograms - inventory_weight(inventories, id);

		fits = SDL_min(count, (int)SDL_floorf((room + 1e-4f) / type->weight_kilograms));
	}
	added = fits > 0 ? put(container, type, item, fits) : 0;
	push_event(inventories, INVENTORY_EVENT_ADDED, id, item, added);
	return added;
}

static void remove_empty_rows(container_t *container)
{
	int kept = 0;

	for (int i = 0; i < container->entry_count; i++)
	{
		if (container->entries[i].count > 0)
		{
			container->entries[kept++] = container->entries[i];
		}
	}
	container->entry_count = kept;
}

int inventory_remove(inventories_t *inventories, uint32_t id, int item, int count)
{
	container_t *container = inventory_get(inventories, id);
	int left = count;

	if (container == NULL || count <= 0)
	{
		return 0;
	}
	// (the last rows first: the fuller ones stay)
	for (int i = container->entry_count - 1; i >= 0 && left > 0; i--)
	{
		inventory_entry_t *entry = &container->entries[i];

		if (entry->item == item)
		{
			int taken = SDL_min(entry->count, left);

			entry->count -= taken;
			left -= taken;
		}
	}
	remove_empty_rows(container);
	push_event(inventories, INVENTORY_EVENT_REMOVED, id, item, count - left);
	return count - left;
}

int inventory_count(const inventories_t *inventories, uint32_t id, int item, bool worn_too)
{
	const container_t *container = inventory_get_const(inventories, id);
	int count = 0;

	if (container == NULL)
	{
		return 0;
	}
	for (int i = 0; i < container->entry_count; i++)
	{
		count += container->entries[i].item == item ? container->entries[i].count : 0;
	}
	for (int s = 0; s < ITEM_SLOTS_MAX && worn_too; s++)
	{
		count += container->equipped[s] == item ? 1 : 0;
	}
	return count;
}

int inventory_move(inventories_t *inventories, uint32_t from, uint32_t to, int item, int count)
{
	int there = SDL_min(inventory_count(inventories, from, item, false), count);
	int moved;

	if (there <= 0 || from == to)
	{
		return 0;
	}
	moved = inventory_add(inventories, to, item, there);
	inventory_remove(inventories, from, item, moved);
	return moved;
}

int inventory_move_all(inventories_t *inventories, uint32_t from, uint32_t to)
{
	container_t *container = inventory_get(inventories, from);
	int moved = 0;
	int items[INVENTORY_ENTRIES_MAX];
	int item_count = 0;

	if (container == NULL || from == to)
	{
		return 0;
	}
	// (each item once: moving changes the rows)
	for (int i = 0; i < container->entry_count; i++)
	{
		bool seen = false;

		for (int k = 0; k < item_count && !seen; k++)
		{
			seen = items[k] == container->entries[i].item;
		}
		if (!seen)
		{
			items[item_count++] = container->entries[i].item;
		}
	}
	for (int k = 0; k < item_count; k++)
	{
		moved += inventory_move(inventories, from, to, items[k], 1 << 30);
	}
	return moved;
}

bool inventory_equip(inventories_t *inventories, uint32_t id, int entry)
{
	container_t *container = inventory_get(inventories, id);
	int item;
	int slot;

	if (container == NULL || entry < 0 || entry >= container->entry_count)
	{
		return false;
	}
	item = container->entries[entry].item;
	slot = inventories->items->types[item].slot;
	if (slot < 0 || slot >= ITEM_SLOTS_MAX)
	{
		return false;
	}
	container->entries[entry].count--;
	remove_empty_rows(container);
	if (container->equipped[slot] >= 0)
	{
		int old = container->equipped[slot];

		put(container, &inventories->items->types[old], old, 1);
		push_event(inventories, INVENTORY_EVENT_UNEQUIPPED, id, old, 1);
	}
	container->equipped[slot] = item;
	push_event(inventories, INVENTORY_EVENT_EQUIPPED, id, item, 1);
	return true;
}

bool inventory_unequip(inventories_t *inventories, uint32_t id, int slot)
{
	container_t *container = inventory_get(inventories, id);
	int item;

	if (container == NULL || slot < 0 || slot >= ITEM_SLOTS_MAX || container->equipped[slot] < 0)
	{
		return false;
	}
	item = container->equipped[slot];
	if (put(container, &inventories->items->types[item], item, 1) == 0)
	{
		return false;
	}
	container->equipped[slot] = -1;
	push_event(inventories, INVENTORY_EVENT_UNEQUIPPED, id, item, 1);
	return true;
}

bool inventory_use(inventories_t *inventories, uint32_t id, int entry)
{
	container_t *container = inventory_get(inventories, id);
	int item;

	if (container == NULL || entry < 0 || entry >= container->entry_count)
	{
		return false;
	}
	item = container->entries[entry].item;
	if (!inventories->items->types[item].usable)
	{
		return false;
	}
	push_event(inventories, INVENTORY_EVENT_USED, id, item, 1);
	return true;
}
