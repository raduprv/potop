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
		// (a chase's "X,Z!": see game_t.walk_chase_straight)
		game->walk_chase_straight[game->walk_test_goal_count - 1] = at != NULL ? at > goals && at[-1] == '!' : goals[SDL_strlen(goals) - 1] == '!';
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
		float points[512][2];
		bool complete = false;
		int count;

		if (potop_actor_get(potop, game->walk_test_actor, &actor))
		{
			count = potop_find_path(potop, game->walk_test_type, actor.x, actor.z, goal[0], goal[1], points, 512, &complete);
			SDL_Log("Walk test: its way, %d points%s", count, complete ? "" : " (not all the way)");
			for (int k = 0; k < count; k++)
			{
				SDL_Log("Walk test:   %.2f %.2f", (double)points[k][0], (double)points[k][1]);
			}
		}
	}
}

// The chase's route: a farmer's way from each goal to the next (a
// player's, about: round the corners close, up the stairs); a goal no
// farmer gets to left out, or ("X,Z!") gone to straight, and back
static void chase_route_build(game_t *game, potop_t *potop)
{
	int most = (int)SDL_arraysize(game->walk_chase_route);
	float meters = 0.0f;

	game->walk_chase_route[0][0] = game->walk_test_goals[0][0];
	game->walk_chase_route[0][1] = game->walk_test_goals[0][1];
	game->walk_chase_route_count = 1;
	for (int k = 1; k < game->walk_test_goal_count; k++)
	{
		const float *from = game->walk_chase_route[game->walk_chase_route_count - 1];
		const float *to = game->walk_test_goals[k];
		float points[512][2];
		bool complete = false;
		int count = potop_find_path(potop, "farmer", from[0], from[1], to[0], to[1], points, 512, &complete);

		// (nowhere a farmer gets, a table's top, a floor behind a shut
		// door: left out; or, asked for ("X,Z!"), the runner there all the
		// same, straight, and straight back after its stop: a player gets
		// where no animal does, and what the animal does about one it
		// cannot get to is the test's too)
		if (count <= 0 || !complete)
		{
			SDL_Log("Walk test: no farmer's way to the route's point %d (%.2f %.2f): %s", k, (double)to[0], (double)to[1],
				game->walk_chase_straight[k] ? "straight there and back" : "left out");
			if (game->walk_chase_straight[k] && game->walk_chase_route_count + 2 <= most)
			{
				float back_x = from[0];
				float back_z = from[1];
				float leg = SDL_sqrtf((to[0] - back_x) * (to[0] - back_x) + (to[1] - back_z) * (to[1] - back_z));

				game->walk_chase_route[game->walk_chase_route_count][0] = to[0];
				game->walk_chase_route[game->walk_chase_route_count][1] = to[1];
				game->walk_chase_route[game->walk_chase_route_count + 1][0] = back_x;
				game->walk_chase_route[game->walk_chase_route_count + 1][1] = back_z;
				game->walk_chase_route_count += 2;
				game->walk_chase_stops[k] = meters + leg;
				meters += 2.0f * leg;
				trail_take(game, to[0], to[1]);
			}
			else
			{
				game->walk_chase_stops[k] = -1.0f;
			}
			continue;
		}
		for (int j = 0; j < count && game->walk_chase_route_count < most; j++)
		{
			const float *last = game->walk_chase_route[game->walk_chase_route_count - 1];

			meters += SDL_sqrtf((points[j][0] - last[0]) * (points[j][0] - last[0]) + (points[j][1] - last[1]) * (points[j][1] - last[1]));
			game->walk_chase_route[game->walk_chase_route_count][0] = points[j][0];
			game->walk_chase_route[game->walk_chase_route_count][1] = points[j][1];
			game->walk_chase_route_count++;
			trail_take(game, points[j][0], points[j][1]);
		}
		game->walk_chase_stops[k] = meters;
	}
	game->walk_chase_stop_next = 1;
}

// Where the runner is, its meters along the route gone; whether at its end
static bool chase_runner_at(const game_t *game, float out[2])
{
	float left = game->walk_chase_meters;

	for (int k = 1; k < game->walk_chase_route_count; k++)
	{
		float dx = game->walk_chase_route[k][0] - game->walk_chase_route[k - 1][0];
		float dz = game->walk_chase_route[k][1] - game->walk_chase_route[k - 1][1];
		float length = SDL_sqrtf(dx * dx + dz * dz);

		if (left < length)
		{
			out[0] = game->walk_chase_route[k - 1][0] + dx * left / SDL_max(length, 1e-6f);
			out[1] = game->walk_chase_route[k - 1][1] + dz * left / SDL_max(length, 1e-6f);
			return false;
		}
		left -= length;
	}
	out[0] = game->walk_chase_route[game->walk_chase_route_count - 1][0];
	out[1] = game->walk_chase_route[game->walk_chase_route_count - 1][1];
	return true;
}

// The actor sent after the runner, as bear.c's go_to: to where the runner
// is, or the nearest place by it the actor can stand
static void chase_send(game_t *game, potop_t *potop, const float runner[2])
{
	// (bear.c's GO_NEAR_METERS)
	const float NEAR_METERS = 6.0f;
	potop_actor_info_t actor;
	double now = potop_world_time(potop);
	float near_x;
	float near_z;
	char text[96];

	if (now < game->walk_chase_again_at || !potop_actor_get(potop, game->walk_test_actor, &actor))
	{
		return;
	}
	if (potop_actor_stand_near(potop, game->walk_test_actor, runner[0], runner[1], NEAR_METERS, &near_x, &near_z))
	{
		bool aside = near_x != runner[0] || near_z != runner[1];

		potop_actor_set_running(potop, game->walk_test_actor, true);
		game->walk_chase_going = potop_actor_go_to(potop, game->walk_test_actor, near_x, near_z);
		game->walk_chase_sent[0] = runner[0];
		game->walk_chase_sent[1] = runner[1];
		game->walk_chase_sends++;
		SDL_snprintf(text, sizeof(text), "walk_chase_sent %.2f %.2f after %.2f %.2f%s%s", (double)near_x, (double)near_z, (double)runner[0],
			(double)runner[1], aside ? " aside" : "", game->walk_chase_going ? "" : " no_way");
		game_trace_event(game, potop, game->walk_test_actor, text);
		if (aside || !game->walk_chase_going)
		{
			SDL_Log("Walk test: %s", text);
		}
		return;
	}
	potop_actor_stop(potop, game->walk_test_actor);
	game->walk_chase_going = false;
	game->walk_chase_again_at = now + 4.0 + 4.0 * (double)SDL_randf();
	SDL_Log("Walk test: nowhere to stand by the runner at %.2f %.2f, again in a while", (double)runner[0], (double)runner[1]);
}

// The chase, a step: the runner on along its route, the actor sent after
// it again when the runner is off where it was sent (as bear.c's attack);
// over when the runner is at the route's end and the actor has stood
// still a while
static void chase_update(game_t *game, potop_t *potop, const potop_actor_info_t *actor, float seconds)
{
	// (bear.c's: the path again when they moved on this far; its reach)
	const float AGAIN_METERS = 2.0f;
	const float REACH_METERS = 1.5f;
	float runner[2];
	bool at_end;
	float distance;

	if (game->walk_chase_route_count == 0)
	{
		chase_route_build(game, potop);
	}
	game->walk_chase_seconds += seconds;
	// (the runner on, standing a while at each goal)
	if (game->walk_chase_pause_left > 0.0f)
	{
		game->walk_chase_pause_left -= seconds;
	}
	else
	{
		game->walk_chase_meters += game->walk_chase_speed * seconds;
		while (game->walk_chase_stop_next < game->walk_test_goal_count && game->walk_chase_stops[game->walk_chase_stop_next] < 0.0f)
		{
			game->walk_chase_stop_next++;
		}
		if (game->walk_chase_stop_next < game->walk_test_goal_count && game->walk_chase_meters >= game->walk_chase_stops[game->walk_chase_stop_next])
		{
			game->walk_chase_meters = game->walk_chase_stops[game->walk_chase_stop_next];
			game->walk_chase_stop_next++;
			game->walk_chase_pause_left = game->walk_chase_pause;
		}
	}
	at_end = chase_runner_at(game, runner);
	distance = SDL_sqrtf((runner[0] - actor->x) * (runner[0] - actor->x) + (runner[1] - actor->z) * (runner[1] - actor->z));
	game->walk_chase_going = game->walk_chase_going && actor->moving;
	if (distance < REACH_METERS)
	{
		if (game->walk_chase_going)
		{
			potop_actor_stop(potop, game->walk_test_actor);
			game->walk_chase_going = false;
		}
	}
	else
	{
		float off_x = game->walk_chase_sent[0] - runner[0];
		float off_z = game->walk_chase_sent[1] - runner[1];

		if (!game->walk_chase_going)
		{
			game->walk_chase_idle_seconds += seconds;
		}
		if (!game->walk_chase_going || off_x * off_x + off_z * off_z > AGAIN_METERS * AGAIN_METERS)
		{
			chase_send(game, potop, runner);
		}
	}
	if (!at_end || (actor->moving && game->walk_test_goal_seconds < WALK_TEST_GOAL_MOST_SECONDS))
	{
		game->walk_test_wait = WALK_TEST_STILL_SECONDS;
		if (at_end)
		{
			game->walk_test_goal_seconds += seconds;
		}
		return;
	}
	game->walk_test_wait -= seconds;
	if (game->walk_test_wait > 0.0f)
	{
		return;
	}
	{
		char text[128];

		SDL_snprintf(text, sizeof(text), "walk_chase_over %.2f_m sends %d idle %.1f_s late %.1f_s", (double)distance, game->walk_chase_sends,
			(double)game->walk_chase_idle_seconds, (double)game->walk_test_goal_seconds);
		SDL_Log("Walk test: %s, at %.2f %.2f, %.2f m up", text, (double)actor->x, (double)actor->z, (double)actor->y);
		game_trace_event(game, potop, game->walk_test_actor, text);
	}
	SDL_Log("Walk test: done");
	game->walk_test_next = game->walk_test_goal_count;
	game->walk_test_done = true;
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
		if (game->walk_chase_speed <= 0.0f)
		{
			send_on(game, potop);
		}
		return;
	}
	if (!potop_actor_get(potop, game->walk_test_actor, &actor))
	{
		game->walk_test_goal_count = 0;
		return;
	}
	trail_take(game, actor.x, actor.z);
	if (game->walk_chase_speed > 0.0f)
	{
		chase_update(game, potop, &actor, seconds);
		return;
	}
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
