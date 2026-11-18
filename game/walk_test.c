// A walking test (game.h; main.c's --walk_test TYPE X,Z[;X,Z...] and
// --walk_from X,Z): an actor of the type put down a moment after the start
// (where the player stands, or at the given point) and sent to each goal
// in turn (potop_actor_go_to: the search, the walk), a goal over when it
// has stood still a while. Into the trace (--trace FILE: its trail, an
// event at each goal, the goals, the navigation mesh round it), for
// tools/animal_trace.py.

#include "game.h"

#include <SDL3/SDL.h>

// Put down this long after the start (the map settled)
static const float WALK_TEST_START_SECONDS = 1.0f;
// Still this long: its walk over, the next goal
static const float WALK_TEST_STILL_SECONDS = 1.5f;
// Nearer than this to the goal: there
static const float WALK_TEST_THERE_METERS = 1.0f;
// Still walking after this long (going round in circles): given up on
static const float WALK_TEST_GOAL_MOST_SECONDS = 120.0f;

static bool read_point(const char *text, float out[2])
{
	char *end = NULL;

	out[0] = (float)SDL_strtod(text, &end);
	if (end == text || *end != ',')
	{
		return false;
	}
	text = end + 1;
	out[1] = (float)SDL_strtod(text, &end);
	return end != text;
}

bool game_walk_test_set(game_t *game, const char *type_name, const char *goals, const char *from)
{
	const char *at = goals;

	SDL_strlcpy(game->walk_test_type, type_name, sizeof(game->walk_test_type));
	game->walk_test_goal_count = 0;
	while (at != NULL && *at != '\0' && game->walk_test_goal_count < (int)SDL_arraysize(game->walk_test_goals))
	{
		if (!read_point(at, game->walk_test_goals[game->walk_test_goal_count]))
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "--walk_test: \"%s\" is not X,Z[;X,Z...]", goals);
			return false;
		}
		game->walk_test_goal_count++;
		at = SDL_strchr(at, ';');
		at = at != NULL ? at + 1 : NULL;
	}
	game->walk_test_from_set = from != NULL && read_point(from, game->walk_test_from);
	game->walk_test_next = 0;
	game->walk_test_actor = 0;
	game->walk_test_wait = WALK_TEST_START_SECONDS;
	game->walk_test_done = false;
	return game->walk_test_goal_count > 0;
}

static void trail_take(game_t *game, float x, float z)
{
	if (game->walk_test_trail[0][0] > game->walk_test_trail[1][0])
	{
		game->walk_test_trail[0][0] = game->walk_test_trail[1][0] = x;
		game->walk_test_trail[0][1] = game->walk_test_trail[1][1] = z;
	}
	game->walk_test_trail[0][0] = SDL_min(game->walk_test_trail[0][0], x);
	game->walk_test_trail[0][1] = SDL_min(game->walk_test_trail[0][1], z);
	game->walk_test_trail[1][0] = SDL_max(game->walk_test_trail[1][0], x);
	game->walk_test_trail[1][1] = SDL_max(game->walk_test_trail[1][1], z);
}

static void send_on(game_t *game, potop_t *potop)
{
	const float *goal = game->walk_test_goals[game->walk_test_next];
	char text[96];

	trail_take(game, goal[0], goal[1]);
	potop_actor_set_running(potop, game->walk_test_actor, game->walk_test_running);
	if (!potop_actor_go_to(potop, game->walk_test_actor, goal[0], goal[1]))
	{
		SDL_snprintf(text, sizeof(text), "walk_test_no_way %.2f %.2f", (double)goal[0], (double)goal[1]);
	}
	else
	{
		SDL_snprintf(text, sizeof(text), "walk_test_goal %.2f %.2f", (double)goal[0], (double)goal[1]);
	}
	SDL_Log("Walk test: %s", text);
	game_trace_event(game, potop, game->walk_test_actor, text);
	game->walk_test_wait = WALK_TEST_STILL_SECONDS;
	game->walk_test_goal_seconds = 0.0f;
	// The way a search finds from where it stands (the log's: what it
	// was told, not what it walks)
	{
		potop_actor_info_t actor;
		float points[64][2];
		bool complete = false;
		int count;

		if (potop_actor_get(potop, game->walk_test_actor, &actor))
		{
			count = potop_find_path(potop, game->walk_test_type, actor.x, actor.z, goal[0], goal[1], points, 64, &complete);
			SDL_Log("Walk test: its way, %d points%s", count, complete ? "" : " (not all the way)");
			for (int k = 0; k < count; k++)
			{
				SDL_Log("Walk test:   %.2f %.2f", (double)points[k][0], (double)points[k][1]);
			}
		}
	}
}

void game_walk_test_update(game_t *game, potop_t *potop, float seconds)
{
	potop_actor_info_t actor;

	if (game->walk_test_goal_count == 0 || game->walk_test_next >= game->walk_test_goal_count)
	{
		return;
	}
	if (game->walk_test_actor == 0)
	{
		float from[2];

		game->walk_test_wait -= seconds;
		if (game->walk_test_wait > 0.0f)
		{
			return;
		}
		if (game->walk_test_from_set)
		{
			from[0] = game->walk_test_from[0];
			from[1] = game->walk_test_from[1];
		}
		else
		{
			potop_camera_info_t camera;

			potop_camera_get(potop, &camera);
			from[0] = camera.first_person ? camera.walker_x : camera.eye_x;
			from[1] = camera.first_person ? camera.walker_z : camera.eye_z;
		}
		game->walk_test_trail[0][0] = 1.0f;
		game->walk_test_trail[1][0] = 0.0f;
		trail_take(game, from[0], from[1]);
		game->walk_test_actor = potop_actor_spawn(potop, game->walk_test_type, from[0], from[1], 0.0f);
		if (game->walk_test_actor == 0)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Walk test: no actor type %s", game->walk_test_type);
			game->walk_test_goal_count = 0;
			return;
		}
		// (named: no mind of the game's takes it over, a bear's wild one)
		potop_actor_set_name(potop, game->walk_test_actor, "walk test", false);
		SDL_Log("Walk test: a %s put down at %.2f %.2f", game->walk_test_type, (double)from[0], (double)from[1]);
		send_on(game, potop);
		return;
	}
	if (!potop_actor_get(potop, game->walk_test_actor, &actor))
	{
		game->walk_test_goal_count = 0;
		return;
	}
	trail_take(game, actor.x, actor.z);
	game->walk_test_goal_seconds += seconds;
	if (actor.moving && game->walk_test_goal_seconds < WALK_TEST_GOAL_MOST_SECONDS)
	{
		game->walk_test_wait = WALK_TEST_STILL_SECONDS;
		return;
	}
	game->walk_test_wait -= seconds;
	if (game->walk_test_wait > 0.0f)
	{
		return;
	}
	// Still a while: there, or as near as it got
	{
		const float *goal = game->walk_test_goals[game->walk_test_next];
		float dx = goal[0] - actor.x;
		float dz = goal[1] - actor.z;
		float distance = SDL_sqrtf(dx * dx + dz * dz);
		char text[96];

		SDL_snprintf(text, sizeof(text), distance < WALK_TEST_THERE_METERS ? "walk_test_there %.2f" : "walk_test_short %.2f_m", (double)distance);
		if (actor.moving)
		{
			SDL_snprintf(text, sizeof(text), "walk_test_timeout %.2f_m", (double)distance);
		}
		SDL_Log("Walk test: goal %d (%.2f %.2f): %s, at %.2f %.2f, %.2f m up", game->walk_test_next, (double)goal[0], (double)goal[1], text,
			(double)actor.x, (double)actor.z, (double)actor.y);
		game_trace_event(game, potop, game->walk_test_actor, text);
	}
	game->walk_test_next++;
	if (game->walk_test_next < game->walk_test_goal_count)
	{
		send_on(game, potop);
	}
	else
	{
		SDL_Log("Walk test: done");
		game->walk_test_done = true;
	}
}
