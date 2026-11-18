// The game's application: the engine started, then a frame at a time the
// player's input, the game's rules, the engine's frame (potop.h)
//
//     potop [--map NAME] [--seed N] [--width W --height H] [--houses [CATEGORY]]
//           [--set SECTION.KEY=VALUE]... [--hour H] [--sim SECONDS [--sim-step S]] [--trace FILE]
//           [--walk_test TYPE X,Z[;X,Z...] [--walk_from X,Z]]
//
// --houses: no map, small flat ground with every house (objects.ini's
// category "buildings", or "fable", or the one named) in rows
// --set: a setting over data/settings.ini's (render.day_length_minutes=24)
// --hour: the day starts at that hour
// --sim: a test without the picture: fixed steps (--sim-step, 0.05 s) for
// that many game seconds as fast as they go, then it quits
// --trace: the animals' minds into FILE (game/trace.c), for
// tools/animal_trace.py
// --walk_test: an actor of the type put down where the player stands (or
// at --walk_from) and sent to each goal in turn (game/walk_test.c); with
// --trace its trail, the goals and the navigation mesh round it; with
// --sim over when the last goal is (or the seconds are)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "game.h"
#include "system_monitor.h"

static const char *trace_path = NULL;
static const char *walk_test_type = NULL;
static const char *walk_test_goals = NULL;
static const char *walk_test_from = NULL;

static void read_command_line(potop_options_t *options, int argument_count, char **arguments)
{
	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		const char *value = i + 1 < argument_count ? arguments[i + 1] : NULL;

		// (its category is optional: the next argument, unless an option)
		if (SDL_strcmp(argument, "--houses") == 0)
		{
			options->houses = "";
			if (value != NULL && SDL_strncmp(value, "--", 2) != 0)
			{
				options->houses = value;
				i++;
			}
			continue;
		}
		if (value == NULL)
		{
			break;
		}
		if (SDL_strcmp(argument, "--map") == 0)
		{
			options->map = value;
			i++;
		}
		else if (SDL_strcmp(argument, "--seed") == 0)
		{
			options->seed = (uint32_t)SDL_strtoul(value, NULL, 10);
			i++;
		}
		else if (SDL_strcmp(argument, "--width") == 0)
		{
			options->width = SDL_atoi(value);
			i++;
		}
		else if (SDL_strcmp(argument, "--height") == 0)
		{
			options->height = SDL_atoi(value);
			i++;
		}
		else if (SDL_strcmp(argument, "--set") == 0)
		{
			if (options->settings_set_count < POTOP_OPTIONS_SETTINGS_MAX)
			{
				options->settings_set[options->settings_set_count++] = value;
			}
			i++;
		}
		else if (SDL_strcmp(argument, "--hour") == 0)
		{
			options->start_hour_set = true;
			options->start_hour = (float)SDL_atof(value);
			i++;
		}
		else if (SDL_strcmp(argument, "--sim") == 0)
		{
			options->sim_seconds = (float)SDL_atof(value);
			i++;
		}
		else if (SDL_strcmp(argument, "--sim-step") == 0)
		{
			options->sim_step = (float)SDL_atof(value);
			i++;
		}
		else if (SDL_strcmp(argument, "--trace") == 0)
		{
			trace_path = value;
			i++;
		}
		else if (SDL_strcmp(argument, "--walk_test") == 0 && i + 2 < argument_count)
		{
			walk_test_type = value;
			walk_test_goals = arguments[i + 2];
			i += 2;
		}
		else if (SDL_strcmp(argument, "--walk_from") == 0)
		{
			walk_test_from = value;
			i++;
		}
	}
}

int main(int argument_count, char **arguments)
{
	static game_t game;
	potop_t *potop = potop_create();
	potop_options_t options;
	potop_input_t input;
	float seconds;

	if (potop == NULL)
	{
		return 1;
	}
	SDL_zero(options);
	options.title = "Potop";
	read_command_line(&options, argument_count, arguments);
	if (!potop_start(potop, &options))
	{
		potop_stop(potop);
		potop_destroy(potop);
		return 1;
	}
	game_start(&game, potop);
	if (trace_path != NULL)
	{
		game_trace_start(&game, trace_path);
	}
	if (walk_test_type != NULL)
	{
		game_walk_test_set(&game, walk_test_type, walk_test_goals, walk_test_from);
	}
	while (potop_frame_begin(potop, &input, &seconds))
	{
		game_input_t game_input;

		// The HUD's windows the keys open (I, J, M)
		if (input.inventory)
		{
			potop_hud_set_open(potop, POTOP_HUD_INVENTORY, !potop_hud_is_open(potop, POTOP_HUD_INVENTORY));
		}
		if (input.journal)
		{
			potop_hud_set_open(potop, POTOP_HUD_JOURNAL, !potop_hud_is_open(potop, POTOP_HUD_JOURNAL));
		}
		if (input.map)
		{
			potop_hud_set_open(potop, POTOP_HUD_MAP, !potop_hud_is_open(potop, POTOP_HUD_MAP));
		}
		if (input.reload)
		{
			potop_player_reload(potop);
		}
		// G: a grenade on a timer, H: one with a proximity fuse
		if ((input.throw_primary || input.throw_secondary)
			&& !potop_player_throw(potop, potop_item_find(potop, input.throw_primary ? "grenade" : "proximity_grenade")))
		{
			potop_notify(potop, input.throw_primary ? "No grenades left" : "No proximity grenades left", 2.0f, POTOP_NOTIFY_WARNING);
		}
		// O: a landmine at the eye's spot, Shift+O: a fire mine
		if (input.place_primary || input.place_secondary)
		{
			const char *kind = input.place_primary ? "landmine" : "fire_mine";
			uint32_t mine = potop_player_place(potop, potop_item_find(potop, kind));

			if (mine != 0)
			{
				// (E on it disarms it: game.c)
				potop_object_set_interactive(potop, mine, true);
				potop_notify(potop, input.place_primary ? "Landmine set: armed in 5 s" : "Fire mine set: armed in 3 s", 2.5f, POTOP_NOTIFY_INFO);
			}
			else
			{
				potop_notify(potop, input.place_primary ? "No landmines left" : "No fire mines left", 2.0f, POTOP_NOTIFY_WARNING);
			}
		}
		// Escape with nothing open: the settings (until there is a menu)
		if (input.escape)
		{
			potop_settings_open(potop, true);
		}
		if (input.map_loaded)
		{
			game_map_loaded(&game, potop);
		}
		game_update_debug(&game, potop, input.debug, seconds);
		SDL_zero(game_input);
		game_input.interact = input.interact;
		game_input.select_weapon = input.select;
		game_input.trigger = input.trigger;
		game_input.aim = input.aim;
		game_input.pointer_free = input.pointer_free;
		game_update(&game, potop, &game_input, seconds);
		game_walk_test_update(&game, potop, seconds);
		game_trace_update(&game, potop, seconds);
		potop_frame_end(potop);
		game_after_update(&game, potop);
		// (a test without the picture: over when its walk is)
		if (game.walk_test_done && options.sim_seconds > 0.0f)
		{
			break;
		}
	}
	game_trace_finish(&game, potop);
	system_monitor_stop();
	potop_stop(potop);
	potop_destroy(potop);
	return 0;
}
