// The game's areas: told when the player (or an actor) comes into one or
// goes out of it. Moș Martin gets a yard round him (a test of the areas);
// any area the map has (the editor's area tool) is said by its name.
// The console's area command lists them and turns them off and on; light
// says how lit the player is.

#include "game.h"

#include <SDL3/SDL.h>

static const char *const MARTIN_YARD = "martin_yard";

// How an area is shown: its name, a few of them as the game says them
static void shown_name(const potop_area_info_t *area, char *name_out, size_t name_out_size)
{
	if (SDL_strcmp(area->name, MARTIN_YARD) == 0)
	{
		SDL_strlcpy(name_out, "Moș Martin's yard", name_out_size);
		return;
	}
	SDL_strlcpy(name_out, area->name[0] != '\0' ? area->name : "an area", name_out_size);
	for (char *c = name_out; *c != '\0'; c++)
	{
		*c = *c == '_' ? ' ' : *c;
	}
}

// area list | area off <name> | area on <name> | area who <name>
static void area_command(void *user, const char *arguments)
{
	potop_t *potop = user;
	char verb[16] = "";
	char name[32] = "";
	uint32_t id;
	potop_area_info_t area;

	SDL_sscanf(arguments, "%15s %31s", verb, name);
	if (SDL_strcmp(verb, "list") == 0 || verb[0] == '\0')
	{
		uint32_t ids[64];
		int count = potop_area_list(potop, ids, 64);

		SDL_Log("%d areas", count);
		for (int i = 0; i < SDL_min(count, 64); i++)
		{
			if (potop_area_get(potop, ids[i], &area))
			{
				SDL_Log("  %s (id %u) at %.0f, %.0f, %.0f x %.0f m, for %s%s%s", area.name, area.id, area.x, area.z, area.width_meters,
					area.depth_meters, area.triggers == 0 ? "no one" : "", (area.triggers & POTOP_AREA_TRIGGER_PLAYER) ? "the player " : "",
					(area.triggers & POTOP_AREA_TRIGGER_ACTORS) ? "the actors" : "");
			}
		}
		return;
	}
	id = potop_area_find(potop, name);
	if (id == 0 || !potop_area_get(potop, id, &area))
	{
		SDL_Log("No area named %s (area list)", name);
		return;
	}
	if (SDL_strcmp(verb, "off") == 0)
	{
		potop_area_set_triggers(potop, id, POTOP_AREA_TRIGGER_NONE);
		SDL_Log("%s: for no one", name);
	}
	else if (SDL_strcmp(verb, "on") == 0)
	{
		potop_area_set_triggers(potop, id, POTOP_AREA_TRIGGER_PLAYER | POTOP_AREA_TRIGGER_ACTORS);
		SDL_Log("%s: for the player and the actors", name);
	}
	else if (SDL_strcmp(verb, "who") == 0)
	{
		uint32_t actors[32];
		bool player = false;
		int count = potop_area_occupants(potop, id, actors, 32, &player);

		SDL_Log("%s: the player %s, %d actors", name, player ? "in it" : "not in it", count);
		for (int i = 0; i < SDL_min(count, 32); i++)
		{
			potop_actor_info_t actor;

			if (potop_actor_get(potop, actors[i], &actor))
			{
				SDL_Log("  %s %u%s%s", actor.type_name, actor.id, actor.name[0] != '\0' ? ": " : "", actor.name);
			}
		}
	}
	else
	{
		SDL_Log("area list | area off <name> | area on <name> | area who <name>");
	}
}

// light: how lit the player is (and so how seen), and what of
static void light_command(void *user, const char *arguments)
{
	potop_t *potop = user;
	potop_camera_info_t camera;
	potop_light_info_t light;

	(void)arguments;
	potop_camera_get(potop, &camera);
	potop_light_at(potop, camera.eye_x, camera.eye_y - 0.7f, camera.eye_z, &light);
	SDL_Log("Light here: %.2f (the sky seen %.2f, the day %.2f, the lights near %.2f)", light.total, light.sky_open, light.daylight, light.local);
}

void game_start_places(game_t *game, potop_t *potop)
{
	(void)game;
	potop_console_add_command(potop, "area", "area list | off <name> | on <name> | who <name>",
		"The map's areas: listed, turned off or on (the player and the actors), who is in one", area_command, potop);
	potop_console_add_command(potop, "light", "light", "How lit the player is (the sky seen, the day, the lights near)", light_command, potop);
}

void game_update_places(game_t *game, potop_t *potop)
{
	potop_area_event_t event;
	potop_actor_info_t martin;

	// Moș Martin's yard, once he is there (the map may have kept it)
	if (game->martin != 0 && potop_area_find(potop, MARTIN_YARD) == 0 && potop_actor_get(potop, game->martin, &martin))
	{
		potop_area_add(potop, MARTIN_YARD, martin.x, martin.z, 16.0f, 12.0f, martin.yaw_radians,
			POTOP_AREA_TRIGGER_PLAYER | POTOP_AREA_TRIGGER_ACTORS);
	}
	while (potop_area_poll_event(potop, &event))
	{
		potop_area_info_t area;
		char name[48];
		char text[128];

		if (!potop_area_get(potop, event.area_id, &area))
		{
			continue;
		}
		shown_name(&area, name, sizeof(name));
		if (event.player)
		{
			if (event.type == POTOP_AREA_ENTER)
			{
				uint32_t actors[16];
				int others = potop_area_occupants(potop, event.area_id, actors, 16, NULL);

				SDL_snprintf(text, sizeof(text), others > 0 ? "Entered %s (%d here)" : "Entered %s", name, others);
			}
			else
			{
				SDL_snprintf(text, sizeof(text), "Left %s", name);
			}
			potop_notify(potop, text, 3.0f, POTOP_NOTIFY_INFO);
		}
		else
		{
			potop_actor_info_t actor;

			// (someone with a name: the hens and the wild bears about a
			// yard are not news)
			if (potop_actor_get(potop, event.actor_id, &actor) && actor.name[0] != '\0')
			{
				SDL_snprintf(text, sizeof(text), "%s %s %s", actor.name,
					event.type == POTOP_AREA_ENTER ? "came into" : "went out of", name);
				potop_notify(potop, text, 2.5f, POTOP_NOTIFY_INFO);
			}
		}
	}
}
