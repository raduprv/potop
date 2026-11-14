// The Hunt (game.h): shoot a bear and gather five falcon feathers, then
// bring the pelt to the camp. Its state is the game's own; the journal and
// the notices are told what changed

#include "game.h"

#include <SDL3/SDL.h>

static const char *const HUNT_QUEST = "hunt";

// The quest's places follow the nearest living animal of each kind (on
// the ground, flying), Moș Martin left out
static void follow_the_animals(game_t *game, potop_t *potop, const potop_camera_info_t *camera)
{
	uint32_t ids[256];
	int count = potop_actors_near(potop, camera->eye_x, camera->eye_z, 2.0f * potop_world_extent_meters(potop), ids, 256);
	bool found[2] = { false, false };

	// (the nearest first: the first of each kind)
	for (int i = 0; i < count; i++)
	{
		potop_actor_info_t actor;
		int kind;

		if (ids[i] == game->martin || !potop_actor_get(potop, ids[i], &actor) || actor.dead)
		{
			continue;
		}
		kind = actor.flies ? 1 : 0;
		if (!found[kind])
		{
			found[kind] = true;
			potop_journal_set_place(potop, HUNT_QUEST, kind == 0 ? "bear" : "feathers", actor.x, actor.z);
		}
	}
	for (int kind = 0; kind < 2; kind++)
	{
		if (!found[kind])
		{
			potop_journal_clear_place(potop, HUNT_QUEST, kind == 0 ? "bear" : "feathers");
		}
	}
	if (!game->hunt_focused)
	{
		game->hunt_focused = potop_journal_focus(potop, HUNT_QUEST, NULL);
	}
}

void game_update_hunt(game_t *game, potop_t *potop, float seconds)
{
	int feather = potop_item_find(potop, "falcon_feather");
	int pelt = potop_item_find(potop, "bear_pelt");
	int coins = potop_item_find(potop, "coins");
	uint32_t pack = potop_player_inventory(potop);
	int feathers = potop_inventory_count(potop, pack, feather, false);
	potop_objective_info_t bear_line;
	potop_objective_info_t feather_line;
	potop_camera_info_t camera;
	char progress[24];

	potop_camera_get(potop, &camera);
	if (!potop_journal_has_quest(potop, HUNT_QUEST))
	{
		if (game->hunt_stage != 0 || pelt < 0 || feather < 0)
		{
			return;
		}
		potop_journal_set_quest(potop, HUNT_QUEST, "The Hunt",
			"The camp is short of fur and fletching before the winter. Bring down a bear and gather falcon feathers for the arrows, then take "
			"the pelt to the camp by the water.", POTOP_QUEST_ACTIVE);
		potop_journal_set_objective(potop, HUNT_QUEST, "bear", "Shoot a bear", POTOP_OBJECTIVE_OPEN, "0/1");
		potop_journal_set_objective(potop, HUNT_QUEST, "feathers", "Gather falcon feathers", POTOP_OBJECTIVE_OPEN, "0/5");
		potop_journal_track(potop, HUNT_QUEST, true);
		game->hunt_place_seconds = 0.0f;
		return;
	}
	if (game->hunt_stage == 2)
	{
		return;
	}
	game->hunt_place_seconds -= seconds;
	if (game->hunt_place_seconds <= 0.0f && game->hunt_stage == 0)
	{
		game->hunt_place_seconds = 0.5f;
		follow_the_animals(game, potop, &camera);
	}
	if (game->hunt_stage == 0 && potop_journal_get_objective(potop, HUNT_QUEST, "bear", &bear_line)
		&& potop_journal_get_objective(potop, HUNT_QUEST, "feathers", &feather_line))
	{
		if (bear_line.state == POTOP_OBJECTIVE_OPEN && game->hunt_bears > 0)
		{
			potop_journal_set_objective(potop, HUNT_QUEST, "bear", NULL, POTOP_OBJECTIVE_DONE, "1/1");
			potop_notify(potop, "The Hunt: a bear brought down", 5.0f, POTOP_NOTIFY_QUEST);
		}
		SDL_snprintf(progress, sizeof(progress), "%d/5", SDL_min(feathers, 5));
		if (feather_line.state == POTOP_OBJECTIVE_OPEN && SDL_strcmp(progress, feather_line.progress) != 0)
		{
			potop_journal_set_objective(potop, HUNT_QUEST, "feathers", NULL, feathers >= 5 ? POTOP_OBJECTIVE_DONE : POTOP_OBJECTIVE_OPEN, progress);
			if (feathers >= 5)
			{
				potop_notify(potop, "The Hunt: enough feathers", 5.0f, POTOP_NOTIFY_QUEST);
			}
		}
		potop_journal_get_objective(potop, HUNT_QUEST, "bear", &bear_line);
		potop_journal_get_objective(potop, HUNT_QUEST, "feathers", &feather_line);
		if (bear_line.state == POTOP_OBJECTIVE_DONE && feather_line.state == POTOP_OBJECTIVE_DONE)
		{
			// The objective that was not known: the camp, a little way off
			float extent = potop_world_extent_meters(potop);

			game->hunt_stage = 1;
			game->hunt_camp_x = SDL_clamp(camera.eye_x + 60.0f, 10.0f, extent - 10.0f);
			game->hunt_camp_z = SDL_clamp(camera.eye_z - 40.0f, 10.0f, extent - 10.0f);
			potop_journal_set_objective(potop, HUNT_QUEST, "camp", "Bring the bear pelt to the camp", POTOP_OBJECTIVE_OPEN, "");
			potop_journal_set_place(potop, HUNT_QUEST, "camp", game->hunt_camp_x, game->hunt_camp_z);
			potop_journal_focus(potop, HUNT_QUEST, "camp");
			potop_notify(potop, "The Hunt: bring the pelt to the camp", 6.0f, POTOP_NOTIFY_QUEST);
		}
	}
	if (game->hunt_stage == 1)
	{
		float dx = game->hunt_camp_x - camera.eye_x;
		float dz = game->hunt_camp_z - camera.eye_z;

		if (dx * dx + dz * dz < 8.0f * 8.0f && potop_inventory_count(potop, pack, pelt, false) > 0)
		{
			potop_inventory_remove(potop, pack, pelt, 1);
			if (coins >= 0)
			{
				potop_inventory_add(potop, pack, coins, 150);
			}
			potop_journal_set_objective(potop, HUNT_QUEST, "camp", NULL, POTOP_OBJECTIVE_DONE, NULL);
			potop_journal_set_quest(potop, HUNT_QUEST, NULL, NULL, POTOP_QUEST_COMPLETED);
			potop_journal_track(potop, HUNT_QUEST, false);
			game->hunt_stage = 2;
			potop_notify(potop, "Quest completed: The Hunt (150 coins)", 6.0f, POTOP_NOTIFY_QUEST);
		}
	}
}
