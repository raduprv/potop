// A test's trace of the animals' minds (game.h; main.c's --trace FILE):
// what tools/animal_trace.py draws from above and counts. A text file, a
// line a record:
//
//     a TIME ID KIND STATE X Z MOVING HOUR    an animal, sampled
//     e TIME ID KIND X Z TEXT                 something its mind did
//     s HOME KIND NAME X Z YAW HALF_W HALF_D  a home's spot (at the end)
//     g X Z                                   a walking test's goal (at the end)
//     r X Z WIDTH DEPTH KIND                  the navigation mesh's rectangles
//                                             of the walking test's type round
//                                             its trail (at the end; kind 0
//                                             the ground, 1 an object's top)
//     o X0 Z0 CELL CELLS ROWS...              the obstacles round the trails
//                                             (at the end; each row a line,
//                                             a digit a cell: 1 a body at
//                                             the feet, 2 at the middle, 3
//                                             both: where the walkers'
//                                             casts look, actors.c)
//
// The bears' minds too (kind "bear", their state's name).

#include "game.h"

#include <SDL3/SDL.h>

// Sampled this often (game seconds)
static const float TRACE_SAMPLE_SECONDS = 0.25f;
// The obstacles: this fine, this far round the trails, at most so many
// cells a side, at the heights a walker's casts look at (its feet, its
// middle: a sheep's)
static const float TRACE_CELL_METERS = 0.25f;
static const float TRACE_MARGIN_METERS = 6.0f;
static const int TRACE_CELLS_MOST = 1200;
static const float TRACE_FEET_HEIGHT = 0.08f;
static const float TRACE_MIDDLE_HEIGHT = 0.5f;

bool game_trace_start(game_t *game, const char *path)
{
	game->trace = SDL_IOFromFile(path, "w");
	if (game->trace == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write the trace %s: %s", path, SDL_GetError());
		return false;
	}
	SDL_IOprintf(game->trace, "# potop animal trace\n");
	return true;
}

static void sample(game_t *game, potop_t *potop, uint32_t actor_id, const char *kind, const char *state)
{
	potop_actor_info_t actor;

	if (!potop_actor_get(potop, actor_id, &actor))
	{
		return;
	}
	// (moving: going somewhere or wandering; then the hour, its yaw)
	SDL_IOprintf(game->trace, "a %.2f %u %s %s %.3f %.3f %d %.3f %.3f\n", potop_elapsed_seconds(potop), actor_id, kind, state, (double)actor.x,
		(double)actor.z, actor.moving || actor.wandering ? 1 : 0, (double)potop_day_hour(potop), (double)actor.yaw_radians);
}

void game_trace_update(game_t *game, potop_t *potop, float seconds)
{
	if (game->trace == NULL)
	{
		return;
	}
	game->trace_seconds -= seconds;
	if (game->trace_seconds > 0.0f)
	{
		return;
	}
	game->trace_seconds += TRACE_SAMPLE_SECONDS;
	for (int i = 0; i < game->animal_count; i++)
	{
		const animal_mind_t *animal = &game->animals[i];

		sample(game, potop, animal->actor_id, game_animal_kind_name(animal->kind), game_animal_state_name(animal->state));
	}
	for (int i = 0; i < game->bear_count; i++)
	{
		sample(game, potop, game->bears[i].actor_id, "bear", game_bear_state_name(game->bears[i].state));
	}
	if (game->farmer.actor != 0)
	{
		static const char *const states[] = { "resting", "going", "stepping", "working", "leaving" };

		sample(game, potop, game->farmer.actor, "farmer", states[game->farmer.state]);
	}
	// (the walking test's: going or still)
	if (game->walk_test_actor != 0)
	{
		potop_actor_info_t actor;

		if (potop_actor_get(potop, game->walk_test_actor, &actor))
		{
			sample(game, potop, game->walk_test_actor, game->walk_test_type, actor.moving ? "going" : "still");
		}
	}
}

void game_trace_event(game_t *game, potop_t *potop, uint32_t actor_id, const char *text)
{
	potop_actor_info_t actor;
	const char *kind = "?";

	if (game->trace == NULL || !potop_actor_get(potop, actor_id, &actor))
	{
		return;
	}
	for (int i = 0; i < game->animal_count; i++)
	{
		if (game->animals[i].actor_id == actor_id)
		{
			kind = game_animal_kind_name(game->animals[i].kind);
		}
	}
	if (actor_id == game->walk_test_actor && game->walk_test_actor != 0)
	{
		kind = game->walk_test_type;
	}
	SDL_IOprintf(game->trace, "e %.2f %u %s %.3f %.3f %s\n", potop_elapsed_seconds(potop), actor_id, kind, (double)actor.x, (double)actor.z, text);
}

void game_trace_finish(game_t *game, potop_t *potop)
{
	float minimum_x = 1e30f;
	float minimum_z = 1e30f;
	float maximum_x = -1e30f;
	float maximum_z = -1e30f;

	if (game->trace == NULL)
	{
		return;
	}
	// The homes' spots (world: turned and placed as the object stands)
	for (int h = 0; h < game->animal_home_count; h++)
	{
		const animal_home_t *home = &game->animal_homes[h];
		int count = potop_object_spot_count(potop, home->object_id);

		for (int k = 0; k < count; k++)
		{
			potop_spot_t spot;

			if (potop_object_spot_get(potop, home->object_id, k, &spot))
			{
				SDL_IOprintf(game->trace, "s %d %s %s %.3f %.3f %.4f %.3f %.3f\n", h, game_animal_kind_name(home->kind), spot.name, (double)spot.x,
					(double)spot.z, (double)spot.yaw_radians, (double)spot.half_width, (double)spot.half_depth);
				minimum_x = SDL_min(minimum_x, spot.x - SDL_max(spot.half_width, spot.half_depth));
				maximum_x = SDL_max(maximum_x, spot.x + SDL_max(spot.half_width, spot.half_depth));
				minimum_z = SDL_min(minimum_z, spot.z - SDL_max(spot.half_width, spot.half_depth));
				maximum_z = SDL_max(maximum_z, spot.z + SDL_max(spot.half_width, spot.half_depth));
			}
		}
	}
	// (and where they are now: the trails go about there)
	for (int i = 0; i < game->animal_count; i++)
	{
		potop_actor_info_t actor;

		if (potop_actor_get(potop, game->animals[i].actor_id, &actor))
		{
			minimum_x = SDL_min(minimum_x, actor.x);
			maximum_x = SDL_max(maximum_x, actor.x);
			minimum_z = SDL_min(minimum_z, actor.z);
			maximum_z = SDL_max(maximum_z, actor.z);
		}
	}
	// The walking test: its goals, its trail's box, the mesh round it
	if (game->walk_test_actor != 0)
	{
		static float rects[8192][5];
		int count;

		for (int g = 0; g < game->walk_test_goal_count; g++)
		{
			SDL_IOprintf(game->trace, "g %.3f %.3f\n", (double)game->walk_test_goals[g][0], (double)game->walk_test_goals[g][1]);
		}
		minimum_x = SDL_min(minimum_x, game->walk_test_trail[0][0]);
		minimum_z = SDL_min(minimum_z, game->walk_test_trail[0][1]);
		maximum_x = SDL_max(maximum_x, game->walk_test_trail[1][0]);
		maximum_z = SDL_max(maximum_z, game->walk_test_trail[1][1]);
		count = potop_debug_navmesh_rects(potop, game->walk_test_type, minimum_x - TRACE_MARGIN_METERS, minimum_z - TRACE_MARGIN_METERS,
			maximum_x + TRACE_MARGIN_METERS, maximum_z + TRACE_MARGIN_METERS, rects, (int)SDL_arraysize(rects));
		for (int k = 0; k < count; k++)
		{
			SDL_IOprintf(game->trace, "r %.2f %.2f %.2f %.2f %d\n", (double)rects[k][0], (double)rects[k][1], (double)rects[k][2], (double)rects[k][3],
				(int)rects[k][4]);
		}
	}
	if (minimum_x < maximum_x)
	{
		float x0 = minimum_x - TRACE_MARGIN_METERS;
		float z0 = minimum_z - TRACE_MARGIN_METERS;
		float side = SDL_max(maximum_x - minimum_x, maximum_z - minimum_z) + 2.0f * TRACE_MARGIN_METERS;
		int cells = SDL_min((int)SDL_ceilf(side / TRACE_CELL_METERS), TRACE_CELLS_MOST);
		float cell = side / (float)cells;
		uint8_t *feet = SDL_malloc((size_t)cells * (size_t)cells);
		uint8_t *middle = SDL_malloc((size_t)cells * (size_t)cells);
		char *line = SDL_malloc((size_t)cells + 2);

		if (feet != NULL && middle != NULL && line != NULL)
		{
			potop_debug_obstacles(potop, x0, z0, cell, cells, TRACE_FEET_HEIGHT, feet);
			potop_debug_obstacles(potop, x0, z0, cell, cells, TRACE_MIDDLE_HEIGHT, middle);
			SDL_IOprintf(game->trace, "o %.3f %.3f %.4f %d\n", (double)x0, (double)z0, (double)cell, cells);
			for (int row = 0; row < cells; row++)
			{
				for (int column = 0; column < cells; column++)
				{
					line[column] = (char)('0' + feet[row * cells + column] + 2 * middle[row * cells + column]);
				}
				line[cells] = '\n';
				line[cells + 1] = '\0';
				SDL_IOprintf(game->trace, "%s", line);
			}
		}
		SDL_free(feet);
		SDL_free(middle);
		SDL_free(line);
	}
	SDL_CloseIO(game->trace);
	game->trace = NULL;
}
