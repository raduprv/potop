// The testbed's game (game.h): the kit, E, the hits, eating

#include "game.h"

#include <SDL3/SDL.h>

static uint32_t loot_random(game_t *game, uint32_t range)
{
	game->loot_random = game->loot_random * 1664525u + 1013904223u;
	return range > 0 ? (game->loot_random >> 8) % range : 0;
}

// The player: a pack of 40 kg (the game decides how much), the two rifles
// and their rounds, a little food and money; the AK in hand, the boots on
static void give_player_kit(potop_t *potop)
{
	static const struct
	{
		const char *name;
		int count;
	} kit[] = { { "ak47", 1 }, { "svd", 1 }, { "rounds_762x39", 150 }, { "rounds_762x54r", 40 }, { "bread", 2 }, { "apple", 3 },
		{ "coins", 50 }, { "knife", 1 }, { "boots", 1 }, { "grenade", 4 }, { "proximity_grenade", 3 },
		{ "landmine", 3 }, { "fire_mine", 2 } };
	uint32_t pack = potop_inventory_create(potop, "You", 40.0f);
	potop_inventory_event_t event;

	potop_set_player_inventory(potop, pack);
	for (size_t i = 0; i < SDL_arraysize(kit); i++)
	{
		int item = potop_item_find(potop, kit[i].name);

		if (item >= 0)
		{
			potop_inventory_add(potop, pack, item, kit[i].count);
		}
	}
	potop_inventory_equip(potop, pack, potop_item_of_weapon(potop, 0));
	potop_inventory_equip(potop, pack, potop_item_find(potop, "boots"));
	// (what the kit put in: no quest is waiting for it)
	while (potop_inventory_poll_event(potop, &event))
	{
	}
}

// What can be picked off the map, unless objects.ini says (its harvest
// keys win): tomatoes and other crops, a willow's twigs, flowers and herbs
// (they go)
static void set_harvests(potop_t *potop)
{
	int set = 0;

	for (int t = 0; t < potop_object_type_count(potop); t++)
	{
		potop_object_type_info_t type;

		if (!potop_object_type_get(potop, t, &type) || type.harvest_item[0] != '\0')
		{
			continue;
		}
		if (SDL_strcasestr(type.name, "tomato") != NULL && potop_item_find(potop, "tomato") >= 0)
		{
			potop_object_type_set_harvest(potop, t, "tomato", 1, 3, false, 3, 24.0f);
			set++;
		}
		else if (SDL_strcasestr(type.name, "willow") != NULL && potop_item_find(potop, "willow_twig") >= 0)
		{
			potop_object_type_set_harvest(potop, t, "willow_twig", 1, 2, false, 2, 48.0f);
			set++;
		}
		else if (SDL_strcasecmp(type.category, "flowers") == 0 && potop_item_find(potop, "flower") >= 0)
		{
			potop_object_type_set_harvest(potop, t, "flower", 1, 1, true, 1, 0.0f);
			set++;
		}
	}
	SDL_Log("Harvest: the game made %d object types pickable (objects.ini's own harvest keys win)", set);
}

void game_start(game_t *game, potop_t *potop)
{
	SDL_zerop(game);
	game->loot_random = 0x2545f491u;
	give_player_kit(potop);
	set_harvests(potop);
	game_start_places(game, potop);
	game_start_bears(game, potop);
}

void game_map_loaded(game_t *game, potop_t *potop)
{
	(void)potop;
	// The loot and the talker belonged to the actors there were
	game->loot_count = 0;
	game->martin = 0;
	game->martin_chest = 0;
	game->martin_owns_round = false;
	game->bear_count = 0;
	game->health_count = 0;
	potop_hud_open_loot(potop, 0);
	game_workers_map_loaded(game, potop);
}

// The loot tables: what a dead animal has
static void fill_loot(game_t *game, potop_t *potop, uint32_t actor_id)
{
	static const struct
	{
		const char *actor;
		const char *item;
		int least;
		int most;
	} table[] = {
		{ "bear", "bear_pelt", 1, 1 }, { "bear", "bear_meat", 2, 4 }, { "bear", "bear_claw", 4, 8 },
		{ "falcon", "falcon_feather", 4, 12 }, { "falcon", "falcon_meat", 1, 1 },
	};
	potop_actor_info_t actor;
	char shown[48];
	uint32_t container;
	int slot;

	if (!potop_actor_get(potop, actor_id, &actor))
	{
		return;
	}
	SDL_strlcpy(shown, actor.type_name, sizeof(shown));
	shown[0] = (char)SDL_toupper((unsigned char)shown[0]);
	container = potop_inventory_create(potop, shown, 0.0f);
	if (container == 0)
	{
		return;
	}
	for (size_t i = 0; i < SDL_arraysize(table); i++)
	{
		int item = potop_item_find(potop, table[i].item);

		if (item >= 0 && SDL_strcmp(table[i].actor, actor.type_name) == 0)
		{
			potop_inventory_add(potop, container, item, table[i].least + (int)loot_random(game, (uint32_t)(table[i].most - table[i].least + 1)));
		}
	}
	slot = game->loot_count < GAME_LOOT_MAX ? game->loot_count++ : (int)(actor_id % GAME_LOOT_MAX);
	if (game->loot_count == GAME_LOOT_MAX && game->loot_containers[slot] != 0)
	{
		potop_inventory_destroy(potop, game->loot_containers[slot]);
	}
	game->loot_actor_ids[slot] = actor_id;
	game->loot_containers[slot] = container;
}

static uint32_t loot_of_actor(const game_t *game, uint32_t actor_id)
{
	for (int i = 0; i < game->loot_count; i++)
	{
		if (game->loot_actor_ids[i] == actor_id)
		{
			return game->loot_containers[i];
		}
	}
	return 0;
}

// What the pack's rows did (a game's quest would watch them): food used is
// eaten (the game would heal, fill a hunger bar...); the log is told
static void inventory_events(potop_t *potop)
{
	static const char *const what[] = { "", "+", "-", "worn: ", "off: ", "used: " };
	potop_inventory_event_t event;

	while (potop_inventory_poll_event(potop, &event))
	{
		potop_item_info_t item;
		potop_inventory_info_t container;
		bool known = potop_item_get(potop, event.item, &item);

		if (event.type == POTOP_INVENTORY_USED && known)
		{
			char message[96];

			if (item.category == POTOP_ITEM_FOOD)
			{
				potop_inventory_remove(potop, event.container, event.item, 1);
				SDL_snprintf(message, sizeof(message), "You ate: %s", item.shown_name);
			}
			else
			{
				SDL_snprintf(message, sizeof(message), "Used: %s", item.shown_name);
			}
			potop_notify(potop, message, 3.0f, POTOP_NOTIFY_INFO);
		}
		SDL_Log("Inventory %s: %s%d %s", potop_inventory_get(potop, event.container, &container) ? container.name : "?",
			what[SDL_clamp((int)event.type, 0, 5)], event.count, known ? item.shown_name : "?");
	}
}

// The loot window closes when the eye walks off (4 m), or on E
static void follow_loot_window(game_t *game, potop_t *potop, const game_input_t *input, bool *interact)
{
	uint32_t open = potop_hud_loot(potop);
	potop_camera_info_t camera;
	bool near = false;

	if (open == 0)
	{
		return;
	}
	potop_camera_get(potop, &camera);
	for (int i = 0; i < game->loot_count; i++)
	{
		potop_actor_info_t actor;

		if (game->loot_containers[i] == open && potop_actor_get(potop, game->loot_actor_ids[i], &actor))
		{
			float dx = actor.x - camera.eye_x;
			float dz = actor.z - camera.eye_z;

			near = dx * dx + dz * dz < 4.0f * 4.0f;
		}
	}
	if (open == game->martin_chest_container)
	{
		potop_object_info_t chest;

		if (potop_object_get(potop, game->martin_chest, &chest))
		{
			float dx = chest.x - camera.eye_x;
			float dz = chest.z - camera.eye_z;

			near = dx * dx + dz * dz < 4.0f * 4.0f;
		}
	}
	if (!near || input->interact)
	{
		potop_hud_open_loot(potop, 0);
		*interact = false;
	}
}

// What the eye is on (the engine says), what E does with it (the game)
static void interact(game_t *game, potop_t *potop, bool pressed)
{
	potop_target_t target;
	char prompt[96];
	potop_explosive_info_t mine;
	bool is_mine;

	prompt[0] = '\0';
	potop_player_find_target(potop, &target);
	is_mine = target.kind == POTOP_TARGET_OBJECT && potop_explosive_get(potop, target.object_id, &mine);
	if (target.kind == POTOP_TARGET_HARVEST)
	{
		potop_item_info_t item;

		potop_item_get(potop, target.item, &item);
		if (target.any_left && !game_may_take(game, potop, target.owner))
		{
			char owner[48];

			game_owner_name(potop, target.owner, owner, sizeof(owner));
			SDL_snprintf(prompt, sizeof(prompt), "[E] Steal %s from %s", item.shown_name, owner);
		}
		else if (target.any_left)
		{
			SDL_snprintf(prompt, sizeof(prompt), "[E] Pick %s", item.shown_name);
		}
		else
		{
			SDL_strlcpy(prompt, "Nothing left to pick", sizeof(prompt));
		}
		if (pressed && target.any_left)
		{
			int picked_item;
			int count;
			potop_pick_result_t result = potop_player_pick(potop, &picked_item, &count);

			if (result == POTOP_PICK_TOO_HEAVY)
			{
				potop_notify(potop, "Too heavy to carry", 3.0f, POTOP_NOTIFY_WARNING);
			}
			else if (result == POTOP_PICK_PICKED && potop_item_get(potop, picked_item, &item))
			{
				char message[96];

				if (game_may_take(game, potop, target.owner))
				{
					SDL_snprintf(message, sizeof(message), "Picked %d %s", count, item.shown_name);
					potop_notify(potop, message, 3.0f, POTOP_NOTIFY_INFO);
				}
				else
				{
					char owner[48];

					game_owner_name(potop, target.owner, owner, sizeof(owner));
					SDL_snprintf(message, sizeof(message), "Stole %d %s from %s", count, item.shown_name, owner);
					potop_notify(potop, message, 3.0f, POTOP_NOTIFY_WARNING);
				}
			}
		}
	}
	else if (target.kind == POTOP_TARGET_GROUND_ITEM)
	{
		potop_inventory_info_t container;
		int first_item;
		int first_count;

		if (potop_inventory_get(potop, target.container, &container) && container.entry_count > 0
			&& potop_inventory_entry(potop, target.container, 0, &first_item, &first_count))
		{
			potop_item_info_t item;

			potop_item_get(potop, first_item, &item);
			const char *verb = game_may_take(game, potop, target.owner) ? "Pick up" : "Steal";

			if (container.entry_count == 1 && first_count > 1)
			{
				SDL_snprintf(prompt, sizeof(prompt), "[E] %s %s (%d)", verb, item.shown_name, first_count);
			}
			else
			{
				SDL_snprintf(prompt, sizeof(prompt), "[E] %s %s", verb, container.entry_count == 1 ? item.shown_name : container.name);
			}
			if (pressed && !potop_player_pick_up(potop))
			{
				potop_notify(potop, "Too heavy to carry all of it", 3.0f, POTOP_NOTIFY_WARNING);
			}
		}
	}
	else if (target.kind == POTOP_TARGET_DOOR)
	{
		// A door or a gate: E opens or shuts it (this game's rule: always;
		// a lock, the owner's say, would go here)
		SDL_strlcpy(prompt, target.door_open ? "[E] Close" : "[E] Open", sizeof(prompt));
		if (pressed)
		{
			potop_door_set(potop, target.object_id, target.door, !target.door_open);
		}
	}
	else if (is_mine)
	{
		// A mine: E takes it back (disarmed; this game's rule: always works)
		potop_item_info_t item;

		if (mine.item >= 0 && potop_item_get(potop, mine.item, &item))
		{
			SDL_snprintf(prompt, sizeof(prompt), mine.armed ? "[E] Disarm %s (armed)" : "[E] Take back %s", item.shown_name);
			if (pressed)
			{
				int back = potop_explosive_disarm(potop, target.object_id);
				char message[96];

				if (back >= 0)
				{
					potop_inventory_add(potop, potop_player_inventory(potop), back, 1);
				}
				SDL_snprintf(message, sizeof(message), "%s disarmed", item.shown_name);
				potop_notify(potop, message, 2.5f, POTOP_NOTIFY_INFO);
			}
		}
	}
	else if (target.kind == POTOP_TARGET_OBJECT && target.object_id == game->martin_chest && game->martin_chest != 0)
	{
		// Moș Martin's chest: locked until his key opens it
		potop_inventory_info_t chest;
		int key;

		if (potop_inventory_get(potop, game->martin_chest_container, &chest))
		{
			key = chest.key_item;
			if (chest.locked && key >= 0 && potop_inventory_count(potop, potop_player_inventory(potop), key, false) > 0)
			{
				SDL_snprintf(prompt, sizeof(prompt), "[E] Unlock %s", chest.name);
				if (pressed)
				{
					potop_inventory_set_lock(potop, game->martin_chest_container, false, key);
					potop_notify(potop, "Unlocked with Moș Martin's key", 3.0f, POTOP_NOTIFY_INFO);
					potop_hud_open_loot(potop, game->martin_chest_container);
				}
			}
			else if (chest.locked)
			{
				SDL_snprintf(prompt, sizeof(prompt), "%s (locked)", chest.name);
				if (pressed)
				{
					potop_notify(potop, "Locked. Moș Martin must have the key", 3.0f, POTOP_NOTIFY_WARNING);
				}
			}
			else
			{
				SDL_snprintf(prompt, sizeof(prompt), game_may_take(game, potop, target.owner) ? "[E] Open %s" : "[E] Steal from %s", chest.name);
				if (pressed)
				{
					potop_hud_open_loot(potop, game->martin_chest_container);
				}
			}
		}
	}
	else if (target.kind == POTOP_TARGET_ACTOR && !target.actor_dead)
	{
		potop_actor_info_t actor;

		// (only Moș Martin talks; the others are left alone)
		if (target.actor_id == game->martin && potop_actor_get(potop, target.actor_id, &actor) && actor.name[0] != '\0' && !actor.hostile)
		{
			SDL_snprintf(prompt, sizeof(prompt), "[E] Talk to %s", actor.name);
			if (pressed)
			{
				game_martin_talk(game, potop, -1);
			}
		}
	}
	else if (target.kind == POTOP_TARGET_ACTOR)
	{
		uint32_t loot = loot_of_actor(game, target.actor_id);
		potop_inventory_info_t container;

		if (potop_inventory_get(potop, loot, &container) && container.entry_count > 0)
		{
			SDL_snprintf(prompt, sizeof(prompt), "[E] Loot %s", container.name);
			if (pressed)
			{
				potop_hud_open_loot(potop, loot);
			}
		}
		else
		{
			SDL_strlcpy(prompt, "Nothing left", sizeof(prompt));
		}
	}
	potop_hud_set_prompt(potop, prompt);
}

void game_update(game_t *game, potop_t *potop, const game_input_t *input, float seconds)
{
	potop_camera_info_t camera;
	bool pressed = input->interact;

	potop_camera_get(potop, &camera);
	potop_hud_set_prompt(potop, NULL);
	// (key 8: the water spell, a rifle for now; the others: weapons.ini's
	// in their order)
	if (input->select_weapon == 7 && potop_weapon_find(potop, "water_rifle") >= 0)
	{
		potop_player_select_weapon(potop, potop_weapon_find(potop, "water_rifle"));
	}
	else if (input->select_weapon >= 0 && !potop_player_select_weapon(potop, input->select_weapon))
	{
		potop_notify(potop, "You do not carry that", 3.0f, POTOP_NOTIFY_WARNING);
	}
	potop_player_set_buttons(potop, input->trigger, input->aim);
	inventory_events(potop);
	game_update_hunt(game, potop, seconds);
	game_update_martin(game, potop);
	game_update_places(game, potop);
	game_update_bears(game, potop, seconds);
	game_update_animals(game, potop, seconds);
	game_update_workers(game, potop, seconds);
	if (!camera.first_person)
	{
		return;
	}
	follow_loot_window(game, potop, input, &pressed);
	// (talking: E is not for what the eye is on)
	if (input->pointer_free || potop_hud_is_open(potop, POTOP_HUD_INVENTORY) || potop_hud_loot(potop) != 0 || potop_dialog_is_open(potop))
	{
		return;
	}
	interact(game, potop, pressed);
}

// The game's part of the hits: actors have some health (flyers less), a
// head takes more, a leg or a wing less; hurt they flinch, out of it they
// die (and leave loot). The rest of the hits are let go
// The water spell: each bolt this much water, this wide
static const float WATER_BOLT_CUBIC_METERS = 1000.0f;
static const float WATER_BOLT_RADIUS_METERS = 8.0f;

void game_after_update(game_t *game, potop_t *potop)
{
	static const char *const part_names[] = { "body", "head", "limb" };
	static const float part_damage[] = { 1.0f, 2.5f, 0.6f };
	potop_hit_t hits[64];
	int count = potop_take_hits(potop, hits, 64);

	for (int i = 0; i < count; i++)
	{
		const potop_hit_t *hit = &hits[i];
		potop_actor_info_t actor;
		int slot = -1;
		float damage;

		// The water spell's bolt: water where it hit (on a bear too)
		if (hit->ammo >= 0 && hit->ammo == potop_ammo_find(potop, "water_bolt") && hit->kind != POTOP_HIT_PLAYER)
		{
			potop_effect_play(potop, "water_burst", hit->x, hit->y, hit->z);
			if (!potop_edit_add_water(potop, hit->x, hit->z, WATER_BOLT_CUBIC_METERS, WATER_BOLT_RADIUS_METERS) && !game->water_refused_told)
			{
				potop_notify(potop, "The water will not stay here (a baked map)", 3.0f, POTOP_NOTIFY_WARNING);
				game->water_refused_told = true;
			}
		}

		if (hit->kind == POTOP_HIT_PLAYER)
		{
			// (no health of the player's yet: told only)
			char message[64];

			SDL_snprintf(message, sizeof(message), hit->damage_type == POTOP_DAMAGE_FIRE ? "Burning (%.0f)" : "Caught in the blast (%.0f)",
				hit->damage);
			potop_notify(potop, message, 3.0f, POTOP_NOTIFY_WARNING);
			continue;
		}
		if (hit->kind == POTOP_HIT_OBJECT)
		{
			// A grenade or a mine shot goes off (this game's rule; its
			// blast's hits come next frame)
			potop_explosive_set_off(potop, hit->object_id);
			continue;
		}
		if (hit->kind != POTOP_HIT_ACTOR || !potop_actor_get(potop, hit->actor_id, &actor) || actor.dead)
		{
			continue;
		}
		if (hit->actor_id == game->martin)
		{
			// (the talker is not to be shot: he only minds it)
			potop_actor_hurt(potop, hit->actor_id);
			potop_notify(potop, "Moș Martin: Ow! What's with that gun, you?!", 4.0f, POTOP_NOTIFY_WARNING);
			continue;
		}
		for (int k = 0; k < game->health_count; k++)
		{
			if (game->health_ids[k] == hit->actor_id)
			{
				slot = k;
				break;
			}
		}
		if (slot < 0)
		{
			slot = game->health_count < GAME_HEALTH_MAX ? game->health_count++ : (int)(hit->actor_id % GAME_HEALTH_MAX);
			game->health_ids[slot] = hit->actor_id;
			game->health[slot] = actor.flies ? 30.0f : 100.0f;
		}
		damage = hit->damage * part_damage[SDL_clamp((int)hit->part, 0, 2)];
		game->health[slot] -= damage;
		SDL_Log("Hit %s %u in the %s (bone %d) for %.0f: %.0f left", actor.type_name, hit->actor_id, part_names[SDL_clamp((int)hit->part, 0, 2)],
			hit->bone, damage, SDL_max(game->health[slot], 0.0f));
		if (game->health[slot] <= 0.0f)
		{
			potop_actor_kill(potop, hit->actor_id);
			fill_loot(game, potop, hit->actor_id);
			game->hunt_bears += actor.flies ? 0 : 1;
		}
		else
		{
			potop_actor_hurt(potop, hit->actor_id);
			game_bear_hurt(game, potop, hit->actor_id, game->health[slot] / (actor.flies ? 30.0f : 100.0f), hit->owner == -1);
		}
	}
}
