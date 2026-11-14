// Moș Martin (game.h), an old bear who talks: put down a few meters ahead
// the first time the player is on foot; he trades a jar of honey for three
// tomatoes. The game's dialogs are C like this.

#include "game.h"

#include <SDL3/SDL.h>

// The game's number for Moș Martin (a game's own numbers for its actors)
static const uint32_t MARTIN_GAME_ID = 1;

typedef enum
{
	MARTIN_WHO = 1,                // (0: Continue, a line without answers)
	MARTIN_HONEY,
	MARTIN_BEARS,
	MARTIN_TOMATOES,
	MARTIN_BYE
} martin_action_t;

static void martin_menu(game_t *game, potop_t *potop, const char *text)
{
	int tomato = potop_item_find(potop, "tomato");

	potop_dialog_say(potop, text);
	potop_dialog_add_choice(potop, MARTIN_WHO, "Who are you, really?");
	potop_dialog_add_choice(potop, MARTIN_HONEY, game->martin_honey == 0 ? "I hear you have honey." : "About the honey...");
	potop_dialog_add_choice(potop, MARTIN_BEARS, "Seen any other bears around?");
	if (game->martin_honey == 1 && potop_inventory_count(potop, potop_player_inventory(potop), tomato, false) >= 3)
	{
		potop_dialog_add_choice(potop, MARTIN_TOMATOES, "Here, I brought you three tomatoes.");
	}
	potop_dialog_add_choice(potop, MARTIN_BYE, "Farewell, Moș Martin.");
	game->martin_leaving = false;
}

void game_martin_talk(game_t *game, potop_t *potop, int choice)
{
	char text[512];
	int tomato = potop_item_find(potop, "tomato");
	int honey = potop_item_find(potop, "honey");
	uint32_t pack = potop_player_inventory(potop);

	if (choice < 0)
	{
		potop_dialog_open(potop, game->martin, "Moș Martin");
		potop_actor_stop(potop, game->martin);
		if (!game->martin_met)
		{
			SDL_strlcpy(text, "Mmm... I smell a human. And gunpowder. Easy now, little one, I don't bite. Usually. I am Moș Martin. What wind blows you "
				"into my forest?", sizeof(text));
			game->martin_met = true;
		}
		else
		{
			SDL_strlcpy(text, "You again? Out with it, quick, my afternoon nap is getting away.", sizeof(text));
		}
		if (game->hunt_bears > 0)
		{
			SDL_strlcat(text, "\n\n...And you smell of a brother of mine. I won't touch you today. But the forest remembers.", sizeof(text));
		}
		martin_menu(game, potop, text);
		return;
	}
	if (game->martin_leaving || choice == 0)
	{
		potop_dialog_close(potop);
		potop_actor_stop_looking(potop, game->martin);
		return;
	}
	switch ((martin_action_t)choice)
	{
		case MARTIN_WHO:
			martin_menu(game, potop, "I've been here since these firs were buds. I know every hive, every spring, and every hunter who thought he was "
				"smarter than the forest. Most of them I buried.\n\n...Joking! Some of them.");
			break;
		case MARTIN_HONEY:
			if (game->martin_honey == 0)
			{
				game->martin_honey = 1;
				potop_journal_set_quest(potop, "martin", "Moș Martin's Honey",
					"Moș Martin will trade a jar of his best lime-tree honey for three juicy tomatoes. A fair trade, he says, not like the fox's.",
					POTOP_QUEST_ACTIVE);
				potop_journal_set_objective(potop, "martin", "tomatoes", "Bring Moș Martin three tomatoes", POTOP_OBJECTIVE_OPEN, "0/3");
				if (!potop_journal_track(potop, "martin", true))
				{
					potop_notify(potop, "New quest (not tracked: too many tracked)", 4.0f, POTOP_NOTIFY_INFO);
				}
				potop_notify(potop, "New quest: Moș Martin's Honey", 5.0f, POTOP_NOTIFY_QUEST);
				martin_menu(game, potop, "Honey? Hah! Everybody wants Moș Martin's honey. Fine. Bring me three tomatoes, the juicy kind, and I'll give you "
					"a jar of the best lime-tree honey in the land. A fair trade, not like the fox's.");
			}
			else if (game->martin_honey == 1)
			{
				martin_menu(game, potop, "Three tomatoes, little one. Three. Not two, not five. A bear counts well when it comes to food.");
			}
			else
			{
				martin_menu(game, potop, "I gave you the honey already. Want more? Come back next year, when the lime trees bloom again.");
			}
			break;
		case MARTIN_BEARS:
			martin_menu(game, potop, game->hunt_bears > 0
				? "You've seen enough of them already, by the smell of you. Put the gun down when you talk to me, yes?"
				: "Bears? I'm the only decent bear around here. The others... the others bite. And they don't stop to chat.");
			break;
		case MARTIN_TOMATOES:
			if (tomato >= 0 && potop_inventory_remove(potop, pack, tomato, 3) == 3)
			{
				int key = potop_item_find(potop, "martin_key");

				game->martin_honey = 2;
				if (honey >= 0)
				{
					potop_inventory_add(potop, pack, honey, 1);
				}
				if (key >= 0)
				{
					potop_inventory_add(potop, pack, key, 1);
				}
				potop_journal_set_objective(potop, "martin", "tomatoes", NULL, POTOP_OBJECTIVE_DONE, "3/3");
				potop_journal_set_quest(potop, "martin", NULL, NULL, POTOP_QUEST_COMPLETED);
				potop_journal_track(potop, "martin", false);
				potop_notify(potop, "Quest completed: Moș Martin's Honey", 5.0f, POTOP_NOTIFY_QUEST);
				martin_menu(game, potop, "Ahh, real tomatoes! They smell of summer. Here, the jar I promised. And don't eat it all at once, or your belly "
					"will ache. I speak from experience. And take this key: my chest by the tree is yours to open. Friends share.");
			}
			else
			{
				martin_menu(game, potop, "Where are the tomatoes? Don't fool me, little one, I have a good nose.");
			}
			break;
		case MARTIN_BYE:
		default:
			potop_dialog_say(potop, "Go well. And don't step on the blueberries!");
			game->martin_leaving = true;
			break;
	}
}

bool game_may_take(game_t *game, potop_t *potop, uint32_t owner)
{
	uint32_t actor = owner != 0 ? potop_actor_find_by_game_id(potop, owner) : 0;
	potop_actor_info_t info;

	if (actor == 0 || !potop_actor_get(potop, actor, &info) || info.dead)
	{
		return true;
	}
	// (the game's friends: only Moș Martin so far, for the tomatoes)
	return owner == MARTIN_GAME_ID && game->martin_honey == 2;
}

void game_owner_name(potop_t *potop, uint32_t owner, char *name_out, size_t name_out_size)
{
	uint32_t actor = potop_actor_find_by_game_id(potop, owner);
	potop_actor_info_t info;

	if (actor != 0 && potop_actor_get(potop, actor, &info))
	{
		SDL_strlcpy(name_out, info.name[0] != '\0' ? info.name : info.type_name, name_out_size);
	}
	else
	{
		SDL_strlcpy(name_out, "someone", name_out_size);
	}
}

// His chest by him (found again after a load: a crate of his near him),
// with ten grenades, locked; what grows round him made his
static void martin_things(game_t *game, potop_t *potop, const potop_actor_info_t *martin)
{
	potop_object_info_t object;
	uint32_t near[512];
	int count;

	if (game->martin_chest != 0 && !potop_object_get(potop, game->martin_chest, &object))
	{
		game->martin_chest = 0;
	}
	if (game->martin_chest == 0)
	{
		count = potop_objects_near(potop, martin->x, martin->z, 15.0f, near, 512);
		for (int i = 0; i < count && game->martin_chest == 0; i++)
		{
			if (potop_object_get(potop, near[i], &object) && object.owner == MARTIN_GAME_ID && SDL_strcmp(object.type_name, "crate") == 0)
			{
				game->martin_chest = near[i];
			}
		}
	}
	if (game->martin_chest == 0)
	{
		// To his right, three meters
		float x = martin->x + SDL_cosf(martin->yaw_radians) * 3.0f;
		float z = martin->z - SDL_sinf(martin->yaw_radians) * 3.0f;
		int crate = potop_object_type_find(potop, "crate");

		game->martin_chest = crate >= 0 ? potop_object_place(potop, crate, x, z, martin->yaw_radians, 1.0f, false) : 0;
		if (game->martin_chest != 0)
		{
			potop_object_set_owner(potop, game->martin_chest, MARTIN_GAME_ID);
		}
	}
	if (game->martin_chest != 0)
	{
		// (what the game can do something with, it says again after a load)
		potop_object_set_interactive(potop, game->martin_chest, true);
		if (game->martin_chest_container == 0)
		{
			int grenade = potop_item_find(potop, "grenade");

			game->martin_chest_container = potop_inventory_create(potop, "Moș Martin's chest", 0.0f);
			if (grenade >= 0)
			{
				potop_inventory_add(potop, game->martin_chest_container, grenade, 10);
			}
			potop_inventory_set_lock(potop, game->martin_chest_container, true, potop_item_find(potop, "martin_key"));
		}
	}
	if (!game->martin_owns_round)
	{
		// What can be picked round him is his
		game->martin_owns_round = true;
		count = potop_objects_near(potop, martin->x, martin->z, 12.0f, near, 512);
		for (int i = 0; i < count; i++)
		{
			if (potop_object_get(potop, near[i], &object))
			{
				potop_object_type_info_t type;

				if (potop_object_type_get(potop, object.type, &type) && type.harvest_item[0] != '\0')
				{
					potop_object_set_owner(potop, near[i], MARTIN_GAME_ID);
				}
			}
		}
	}
}

void game_update_martin(game_t *game, potop_t *potop)
{
	potop_dialog_event_t event;
	potop_actor_info_t martin;
	potop_camera_info_t camera;
	bool there;

	potop_camera_get(potop, &camera);
	// (after a load his id is new: found by the game's number for him)
	if (game->martin == 0 || !potop_actor_get(potop, game->martin, &martin))
	{
		game->martin = potop_actor_find_by_game_id(potop, MARTIN_GAME_ID);
	}
	there = game->martin != 0 && potop_actor_get(potop, game->martin, &martin);
	if (!there && camera.first_person)
	{
		float yaw = camera.walker_yaw_degrees * SDL_PI_F / 180.0f;
		float x = camera.walker_x - SDL_sinf(yaw) * 7.0f;
		float z = camera.walker_z - SDL_cosf(yaw) * 7.0f;

		game->martin = potop_actor_spawn(potop, "bear", x, z, 0.0f);
		if (game->martin != 0)
		{
			potop_actor_set_name(potop, game->martin, "Moș Martin", false);
			potop_actor_set_game_id(potop, game->martin, MARTIN_GAME_ID);
			potop_actor_look_at(potop, game->martin, camera.walker_x, camera.walker_z);
		}
		there = game->martin != 0 && potop_actor_get(potop, game->martin, &martin);
	}
	if (there)
	{
		potop_objective_info_t line;

		martin_things(game, potop, &martin);
		if (potop_journal_get_objective(potop, "martin", "tomatoes", &line) && line.state == POTOP_OBJECTIVE_OPEN)
		{
			char progress[24];
			int tomato = potop_item_find(potop, "tomato");

			SDL_snprintf(progress, sizeof(progress), "%d/3", SDL_min(potop_inventory_count(potop, potop_player_inventory(potop), tomato, false), 3));
			potop_journal_set_objective(potop, "martin", "tomatoes", NULL, POTOP_OBJECTIVE_OPEN, progress);
			potop_journal_set_place(potop, "martin", "tomatoes", martin.x, martin.z);
		}
	}
	while (potop_dialog_poll_event(potop, &event))
	{
		if (event.type == POTOP_DIALOG_CHOSEN && event.actor == game->martin)
		{
			game_martin_talk(game, potop, (int)event.choice);
		}
		else if (event.type == POTOP_DIALOG_CLOSED)
		{
			potop_actor_stop_looking(potop, game->martin);
		}
	}
	potop_player_set_held(potop, false);
	if (potop_dialog_is_open(potop))
	{
		// (he looks at the player; walking off ends it)
		if (there)
		{
			float dx = martin.x - camera.walker_x;
			float dz = martin.z - camera.walker_z;

			potop_actor_look_at(potop, game->martin, camera.walker_x, camera.walker_z);
			if (dx * dx + dz * dz > 6.0f * 6.0f || !camera.first_person)
			{
				potop_dialog_close(potop);
				potop_actor_stop_looking(potop, game->martin);
			}
		}
		else
		{
			potop_dialog_close(potop);
		}
		// (the player stays put while talking)
		potop_player_set_held(potop, true);
	}
}
