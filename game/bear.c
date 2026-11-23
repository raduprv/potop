// The bears' minds (game.h): every bear but Moș Martin. What they see and
// hear is the engine's (potop_actor_sees_player, potop_noises_heard); what
// they make of it is here. A bear rests in the shade by day, wanders about
// (no home: a short way at a time, roughly on), stops and looks at what it
// hears or glimpses, goes to see what
// it was, growls at someone too near, and goes for them only if provoked:
// shot at, or the growl not heeded. Badly hurt, it runs.
//
// Each bear thinks a few times a second, not every frame, the bears spread
// over the frames.

#include "game.h"

#include <SDL3/SDL.h>

// How often a bear thinks (seconds, and a little more at random)
static const float THINK_SECONDS = 0.15f;
// Its wandering: each walk this far (least, most), turned at most this much
// from the way it went before; and how long it stands between walks
static const float WANDER_LEAST_METERS = 10.0f;
static const float WANDER_MOST_METERS = 25.0f;
static const float WANDER_TURN_RADIANS = 1.0f;
// Where it wants to go is no place for it (deep water): the nearest
// place by it that is, this far off at the most (go_to); none: it waits a
// while
static const float GO_NEAR_METERS = 6.0f;
static const float GO_AGAIN_SECONDS_LEAST = 4.0f;
static const float GO_AGAIN_SECONDS_MOST = 8.0f;
static const float STAND_SECONDS_LEAST = 3.0f;
static const float STAND_SECONDS_MOST = 10.0f;
// Seen this well (0..1, potop_actor_sees_player), it notices
static const float NOTICE_SIGHT = 0.15f;
// Suspicion: what seeing adds a second (times how well), what it loses a second
static const float SUSPICION_PER_SECOND = 2.0f;
static const float SUSPICION_LOSS_PER_SECOND = 0.1f;
// Alert: how long it stands looking before it goes to see, or lets it be
static const float ALERT_SECONDS = 4.0f;
// Going to see what it was: it gives up after this
static const float INVESTIGATE_SECONDS = 25.0f;
// Someone this near, clearly seen: it growls; nearer than the second, or
// still within the third after the warning's time: provoked
static const float WARN_METERS = 10.0f;
static const float WARN_TOO_NEAR_METERS = 4.0f;
static const float WARN_STAY_METERS = 7.0f;
static const float WARN_SECONDS = 4.0f;
// Calm again once they are this far
static const float CALM_METERS = 15.0f;
// Seen this well, it has made the player out: for this long it does not go
// to see what it is again (it watches, then goes its way: off, this far,
// away from them); its wandering keeps this far from the player meanwhile
static const float KNOWN_SIGHT = 0.6f;
static const float KNOWN_SECONDS = 60.0f;
static const float KNOWN_KEEP_AWAY_METERS = 20.0f;
static const float KNOWN_OFF_METERS = 25.0f;
// The attack: in reach, every so often, this much harm; it gives up when
// they are this far for that long
static const float ATTACK_REACH_METERS = 2.5f;
static const float ATTACK_SECONDS = 1.6f;
static const float ATTACK_DAMAGE = 25.0f;
static const float ATTACK_GIVE_UP_METERS = 40.0f;
static const float ATTACK_GIVE_UP_SECONDS = 10.0f;
// Hurt to less than this share of its health: it runs, this far
static const float FLEE_HEALTH = 0.3f;
static const float FLEE_METERS = 60.0f;
static const float FLEE_SECONDS = 20.0f;
// By day it may rest in the darkest place near it (looked at this many
// places, this far round it), this long
static const float REST_DAYLIGHT = 0.6f;
static const int REST_PLACES_LOOKED_AT = 10;
static const float REST_SEARCH_METERS = 20.0f;
static const float REST_SECONDS_LEAST = 60.0f;
static const float REST_SECONDS_MOST = 150.0f;
// Asleep, it hears only noises this much nearer (of their reach)
static const float ASLEEP_HEARING = 0.4f;
// Its type
static const char *const BEAR_TYPE = "bear";

static float random_unit(game_t *game)
{
	uint32_t value = game->bear_random != 0 ? game->bear_random : 0x9e3779b9u;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	game->bear_random = value;
	return (float)(value >> 8) / 16777216.0f;
}

static float random_between(game_t *game, float least, float most)
{
	return least + (most - least) * random_unit(game);
}

static float distance_squared_2d(float ax, float az, float bx, float bz)
{
	return (ax - bx) * (ax - bx) + (az - bz) * (az - bz);
}

static bear_mind_t *mind_of(game_t *game, uint32_t actor_id)
{
	for (int i = 0; i < game->bear_count; i++)
	{
		if (game->bears[i].actor_id == actor_id)
		{
			return &game->bears[i];
		}
	}
	return NULL;
}

static void set_state(bear_mind_t *bear, bear_state_t state)
{
	bear->state = state;
	bear->state_seconds = 0.0f;
	bear->going = false;
}

static void stand(potop_t *potop, bear_mind_t *bear)
{
	potop_actor_stop(potop, bear->actor_id);
	potop_actor_set_running(potop, bear->actor_id, false);
	bear->going = false;
}

// Sent somewhere it can stand: there if it can, else the nearest place by
// it that it can (potop_actor_stand_near: the bank before the water a
// splash was heard in; the same place each time it is asked, so after
// someone who stands where it cannot be it is not sent now here, now
// there); in deep water or inside a rock it does not even look for a way
// (the search for a way to somewhere it cannot get costs the most). None:
// it stays where it is and tries again a few seconds later
static void go_to(game_t *game, potop_t *potop, bear_mind_t *bear, float x, float z, bool running, int effort)
{
	potop_actor_info_t actor;
	double now = potop_world_time(potop);
	float near_x;
	float near_z;

	if (now < bear->go_again_at || !potop_actor_get(potop, bear->actor_id, &actor))
	{
		return;
	}
	if (potop_actor_stand_near(potop, bear->actor_id, x, z, GO_NEAR_METERS, &near_x, &near_z))
	{
		potop_actor_set_running(potop, bear->actor_id, running);
		potop_actor_set_effort(potop, bear->actor_id, effort);
		bear->going = potop_actor_go_to(potop, bear->actor_id, near_x, near_z);
		// (where it was sent after: the one who sends it asks whether that
		// has moved on, not the place by it)
		bear->goal_x = x;
		bear->goal_z = z;
		return;
	}
	stand(potop, bear);
	bear->go_again_at = now + (double)random_between(game, GO_AGAIN_SECONDS_LEAST, GO_AGAIN_SECONDS_MOST);
}

// Got where it went (or stopped on the way)
static bool arrived(const bear_mind_t *bear, const potop_actor_info_t *actor)
{
	return !actor->moving || distance_squared_2d(actor->x, actor->z, bear->goal_x, bear->goal_z) < 2.0f * 2.0f;
}

// The darkest of a few places round it (by day: shade)
static void find_shade(game_t *game, potop_t *potop, const potop_actor_info_t *actor, float *x_out, float *z_out)
{
	float darkest = 2.0f;

	*x_out = actor->x;
	*z_out = actor->z;
	for (int i = 0; i < REST_PLACES_LOOKED_AT; i++)
	{
		float x = actor->x + random_between(game, -REST_SEARCH_METERS, REST_SEARCH_METERS);
		float z = actor->z + random_between(game, -REST_SEARCH_METERS, REST_SEARCH_METERS);
		potop_light_info_t light;

		potop_light_at(potop, x, potop_world_ground_height(potop, x, z) + 0.5f, z, &light);
		if (light.total < darkest)
		{
			darkest = light.total;
			*x_out = x;
			*z_out = z;
		}
	}
}

static void wake(potop_t *potop, bear_mind_t *bear)
{
	if (bear->asleep)
	{
		potop_actor_play(potop, bear->actor_id, "get_up");
		bear->asleep = false;
	}
}

// Its attention to something (heard, glimpsed): it stops and looks
static void alert(potop_t *potop, bear_mind_t *bear, float x, float z)
{
	wake(potop, bear);
	stand(potop, bear);
	potop_actor_look_at(potop, bear->actor_id, x, z);
	bear->interest_x = x;
	bear->interest_z = z;
	bear->has_interest = true;
	set_state(bear, BEAR_ALERT);
}

static void provoke(potop_t *potop, bear_mind_t *bear)
{
	wake(potop, bear);
	potop_actor_stop_looking(potop, bear->actor_id);
	bear->provoked = true;
	set_state(bear, BEAR_ATTACK);
}

static void flee(game_t *game, potop_t *potop, bear_mind_t *bear, const potop_actor_info_t *actor, float player_x, float player_z)
{
	float away_x = actor->x - player_x;
	float away_z = actor->z - player_z;
	float length_squared = away_x * away_x + away_z * away_z;
	float scale = length_squared > 0.01f ? FLEE_METERS / SDL_sqrtf(length_squared) : 0.0f;

	wake(potop, bear);
	potop_actor_stop_looking(potop, bear->actor_id);
	bear->provoked = false;
	set_state(bear, BEAR_FLEE);
	go_to(game, potop, bear, actor->x + away_x * scale, actor->z + away_z * scale, true, POTOP_EFFORT_ERRAND);
}

// What it heard since it last listened: the nearest of the loud ones, the
// player's steps; true if anything (where in *x_out, *z_out)
static bool listen(potop_t *potop, bear_mind_t *bear, const potop_actor_info_t *actor, double now, float *x_out, float *z_out,
	bool *loud_out)
{
	potop_noise_t noises[16];
	int count = potop_noises_heard(potop, actor->x, actor->y + 1.0f, actor->z, bear->heard_until, noises, 16);
	bool heard = false;
	float nearest = 1e30f;

	bear->heard_until = now;
	*loud_out = false;
	for (int i = 0; i < count; i++)
	{
		const potop_noise_t *noise = &noises[i];
		float distance_squared = distance_squared_2d(actor->x, actor->z, noise->x, noise->z);
		float reach = noise->radius_meters * (bear->asleep ? ASLEEP_HEARING : 1.0f);

		if (noise->source == bear->actor_id || distance_squared >= reach * reach || distance_squared >= nearest)
		{
			continue;
		}
		nearest = distance_squared;
		*x_out = noise->x;
		*z_out = noise->z;
		*loud_out = noise->kind == POTOP_NOISE_SHOT || noise->kind == POTOP_NOISE_EXPLOSION;
		heard = true;
	}
	return heard;
}

static void think(game_t *game, potop_t *potop, bear_mind_t *bear, const potop_actor_info_t *actor, float seconds, double now,
	const potop_camera_info_t *camera)
{
	float sight = bear->asleep ? 0.0f : potop_actor_sees_player(potop, bear->actor_id);
	float player_squared = distance_squared_2d(actor->x, actor->z, camera->walker_x, camera->walker_z);
	float heard_x = 0.0f;
	float heard_z = 0.0f;
	bool loud = false;
	bool heard = listen(potop, bear, actor, now, &heard_x, &heard_z, &loud);
	bool knows_player;

	bear->state_seconds += seconds;
	bear->suspicion = SDL_clamp(bear->suspicion + (sight > 0.0f ? sight * SUSPICION_PER_SECOND : -SUSPICION_LOSS_PER_SECOND) * seconds, 0.0f, 1.0f);
	if (sight >= NOTICE_SIGHT)
	{
		bear->interest_x = camera->walker_x;
		bear->interest_z = camera->walker_z;
		bear->has_interest = true;
		bear->last_seen_seconds = now;
	}
	if (sight >= KNOWN_SIGHT)
	{
		bear->knows_player_until = now + KNOWN_SECONDS;
	}
	knows_player = now < bear->knows_player_until;
	switch (bear->state)
	{
		case BEAR_WANDER:
		case BEAR_REST:
		{
			// What it minds: someone it has not made out yet, or coming near;
			// a noise it does not know, a near one, or a loud one
			bool near = player_squared < WARN_METERS * WARN_METERS;
			bool minds_sight = sight >= NOTICE_SIGHT && (!knows_player || near);
			bool minds_noise = heard && (loud || !knows_player
				|| distance_squared_2d(actor->x, actor->z, heard_x, heard_z) < WARN_METERS * WARN_METERS);

			if (minds_sight || minds_noise)
			{
				alert(potop, bear, minds_sight ? camera->walker_x : heard_x, minds_sight ? camera->walker_z : heard_z);
				break;
			}
		}
			if (bear->state == BEAR_REST)
			{
				if (!bear->asleep && (!bear->going || arrived(bear, actor)))
				{
					stand(potop, bear);
					potop_actor_play(potop, bear->actor_id, "lie_down");
					bear->asleep = true;
					bear->sleep_seconds = 0.0f;
				}
				else if (bear->asleep)
				{
					bear->sleep_seconds += seconds;
					if (bear->sleep_seconds > 2.0f && bear->sleep_seconds - seconds <= 2.0f)
					{
						potop_actor_play(potop, bear->actor_id, "sleep_idle");
					}
				}
				if (bear->state_seconds > bear->rest_seconds)
				{
					wake(potop, bear);
					set_state(bear, BEAR_WANDER);
				}
				break;
			}
			// Wandering: a walk, a stand, another walk; by day now and then a rest
			if (bear->going && !arrived(bear, actor))
			{
				break;
			}
			if (bear->going)
			{
				bear->going = false;
				bear->stand_seconds = random_between(game, STAND_SECONDS_LEAST, STAND_SECONDS_MOST);
				bear->state_seconds = 0.0f;
				potop_actor_play(potop, bear->actor_id, random_unit(game) < 0.5f ? "idle_break" : "eat");
			}
			if (bear->state_seconds < bear->stand_seconds)
			{
				break;
			}
			{
				potop_light_info_t light;

				potop_light_at(potop, actor->x, actor->y + 0.5f, actor->z, &light);
				if (light.daylight >= REST_DAYLIGHT && random_unit(game) < 0.15f)
				{
					float x;
					float z;

					find_shade(game, potop, actor, &x, &z);
					set_state(bear, BEAR_REST);
					bear->rest_seconds = random_between(game, REST_SECONDS_LEAST, REST_SECONDS_MOST);
					go_to(game, potop, bear, x, z, false, POTOP_EFFORT_STROLL);
					break;
				}
			}
			{
				// A short way on, about the way it was going (away from the
				// player it made out)
				float x = actor->x;
				float z = actor->z;

				for (int attempt = 0; attempt < 4; attempt++)
				{
					float heading = bear->wander_heading + random_between(game, -WANDER_TURN_RADIANS, WANDER_TURN_RADIANS)
						+ (float)attempt * 1.6f;
					float length = random_between(game, WANDER_LEAST_METERS, WANDER_MOST_METERS);

					x = actor->x + SDL_sinf(heading) * length;
					z = actor->z + SDL_cosf(heading) * length;
					if (!knows_player || distance_squared_2d(x, z, camera->walker_x, camera->walker_z) > KNOWN_KEEP_AWAY_METERS * KNOWN_KEEP_AWAY_METERS)
					{
						bear->wander_heading = heading;
						break;
					}
				}
				go_to(game, potop, bear, x, z, false, POTOP_EFFORT_STROLL);
			}
			break;
		case BEAR_ALERT:
			if (heard)
			{
				bear->interest_x = heard_x;
				bear->interest_z = heard_z;
				potop_actor_look_at(potop, bear->actor_id, heard_x, heard_z);
				bear->state_seconds = 0.0f;
			}
			if (sight >= NOTICE_SIGHT)
			{
				potop_actor_look_at(potop, bear->actor_id, camera->walker_x, camera->walker_z);
				if (player_squared < WARN_METERS * WARN_METERS && bear->suspicion > 0.5f)
				{
					bear->knows_player_until = now + KNOWN_SECONDS;
					set_state(bear, BEAR_WARN);
					potop_actor_play(potop, bear->actor_id, "roar");
					potop_notify(potop, "A bear growls at you. Back off.", 3.0f, POTOP_NOTIFY_WARNING);
					break;
				}
			}
			if (bear->state_seconds > ALERT_SECONDS)
			{
				potop_actor_stop_looking(potop, bear->actor_id);
				// It looked at them long enough: it knows what they are
				if (now - bear->last_seen_seconds < 1.0)
				{
					bear->knows_player_until = now + KNOWN_SECONDS;
					knows_player = true;
				}
				if (knows_player && !loud)
				{
					// It knows what that is: off, its own way, away from them
					float away = SDL_atan2f(actor->x - camera->walker_x, actor->z - camera->walker_z)
						+ random_between(game, -0.5f * WANDER_TURN_RADIANS, 0.5f * WANDER_TURN_RADIANS);

					set_state(bear, BEAR_WANDER);
					bear->has_interest = false;
					bear->wander_heading = away;
					go_to(game, potop, bear, actor->x + SDL_sinf(away) * KNOWN_OFF_METERS, actor->z + SDL_cosf(away) * KNOWN_OFF_METERS, false, POTOP_EFFORT_ERRAND);
				}
				else if (bear->has_interest && (bear->suspicion > 0.2f || loud))
				{
					set_state(bear, BEAR_INVESTIGATE);
					go_to(game, potop, bear, bear->interest_x, bear->interest_z, false, POTOP_EFFORT_ERRAND);
				}
				else
				{
					set_state(bear, BEAR_WANDER);
				}
			}
			break;
		case BEAR_INVESTIGATE:
			if (sight >= NOTICE_SIGHT && (player_squared < WARN_METERS * WARN_METERS || knows_player))
			{
				alert(potop, bear, camera->walker_x, camera->walker_z);
				break;
			}
			if (heard)
			{
				go_to(game, potop, bear, heard_x, heard_z, false, POTOP_EFFORT_ERRAND);
			}
			if (bear->state_seconds > INVESTIGATE_SECONDS || (bear->going && arrived(bear, actor) && bear->state_seconds > 3.0f))
			{
				stand(potop, bear);
				bear->has_interest = false;
				bear->suspicion = 0.0f;
				set_state(bear, BEAR_WANDER);
			}
			break;
		case BEAR_WARN:
			potop_actor_look_at(potop, bear->actor_id, camera->walker_x, camera->walker_z);
			if (player_squared < WARN_TOO_NEAR_METERS * WARN_TOO_NEAR_METERS
				|| (bear->state_seconds > WARN_SECONDS && player_squared < WARN_STAY_METERS * WARN_STAY_METERS))
			{
				provoke(potop, bear);
			}
			else if (player_squared > CALM_METERS * CALM_METERS || bear->state_seconds > WARN_SECONDS)
			{
				// They backed off (far, or out of its way once the warning's
				// time is up): it watches them a while, then goes off
				bear->suspicion = 0.3f;
				bear->knows_player_until = now + KNOWN_SECONDS;
				potop_actor_play(potop, bear->actor_id, "idle_break");
				alert(potop, bear, camera->walker_x, camera->walker_z);
			}
			break;
		case BEAR_ATTACK:
			bear->attack_seconds -= seconds;
			if (player_squared > ATTACK_GIVE_UP_METERS * ATTACK_GIVE_UP_METERS)
			{
				bear->far_seconds += seconds;
				if (bear->far_seconds > ATTACK_GIVE_UP_SECONDS)
				{
					stand(potop, bear);
					bear->provoked = false;
					bear->far_seconds = 0.0f;
					set_state(bear, BEAR_WANDER);
				}
				break;
			}
			bear->far_seconds = 0.0f;
			if (player_squared < ATTACK_REACH_METERS * ATTACK_REACH_METERS)
			{
				if (bear->going)
				{
					stand(potop, bear);
				}
				potop_actor_look_at(potop, bear->actor_id, camera->walker_x, camera->walker_z);
				if (bear->attack_seconds <= 0.0f)
				{
					char message[64];

					bear->attack_seconds = ATTACK_SECONDS;
					potop_actor_play(potop, bear->actor_id, "attack");
					// (no health of the player's yet: told only)
					SDL_snprintf(message, sizeof(message), "The bear mauls you (%.0f)", ATTACK_DAMAGE);
					potop_notify(potop, message, 2.0f, POTOP_NOTIFY_WARNING);
				}
			}
			else if (!bear->going || distance_squared_2d(bear->goal_x, bear->goal_z, camera->walker_x, camera->walker_z) > 2.0f * 2.0f)
			{
				// After them, running (the path again when they moved on)
				potop_actor_stop_looking(potop, bear->actor_id);
				go_to(game, potop, bear, camera->walker_x, camera->walker_z, true, POTOP_EFFORT_ALL);
			}
			break;
		case BEAR_FLEE:
			if (bear->state_seconds > FLEE_SECONDS || (bear->going && arrived(bear, actor) && bear->state_seconds > 2.0f))
			{
				stand(potop, bear);
				bear->suspicion = 0.0f;
				set_state(bear, BEAR_WANDER);
			}
			break;
		case BEAR_STATE_COUNT:
			break;
	}
}

void game_bear_hurt(game_t *game, potop_t *potop, uint32_t actor_id, float health_share, bool by_player)
{
	bear_mind_t *bear = mind_of(game, actor_id);
	potop_actor_info_t actor;
	potop_camera_info_t camera;

	if (bear == NULL || !potop_actor_get(potop, actor_id, &actor))
	{
		return;
	}
	potop_camera_get(potop, &camera);
	if (health_share < FLEE_HEALTH)
	{
		flee(game, potop, bear, &actor, camera.walker_x, camera.walker_z);
	}
	else if (by_player)
	{
		provoke(potop, bear);
	}
}

// The bears there are now: a mind for each new one, none for the dead and
// gone
static void count_bears(game_t *game, potop_t *potop, const potop_camera_info_t *camera)
{
	uint32_t ids[GAME_BEARS_MAX];
	int count = potop_actors_near(potop, camera->eye_x, camera->eye_z, 2.0f * potop_world_extent_meters(potop), ids, GAME_BEARS_MAX);
	int kept = 0;

	for (int i = 0; i < game->bear_count; i++)
	{
		potop_actor_info_t actor;

		if (potop_actor_get(potop, game->bears[i].actor_id, &actor) && !actor.dead)
		{
			game->bears[kept++] = game->bears[i];
		}
	}
	game->bear_count = kept;
	for (int i = 0; i < count && game->bear_count < GAME_BEARS_MAX; i++)
	{
		potop_actor_info_t actor;
		bear_mind_t *bear;

		// (a named one is someone: Moș Martin, a game's own; the wild ones
		// have no names)
		if (ids[i] == game->martin || mind_of(game, ids[i]) != NULL || !potop_actor_get(potop, ids[i], &actor) || actor.dead
			|| SDL_strcmp(actor.type_name, BEAR_TYPE) != 0 || actor.name[0] != '\0')
		{
			continue;
		}
		bear = &game->bears[game->bear_count++];
		SDL_zerop(bear);
		bear->actor_id = ids[i];
		bear->wander_heading = random_between(game, 0.0f, 6.2831853f);
		bear->state = BEAR_WANDER;
		bear->think_in = random_between(game, 0.0f, THINK_SECONDS);
		bear->heard_until = potop_world_time(potop);
	}
}

void game_update_bears(game_t *game, potop_t *potop, float seconds)
{
	uint64_t start = SDL_GetPerformanceCounter();
	potop_camera_info_t camera;
	double now = potop_world_time(potop);

	potop_camera_get(potop, &camera);
	game->bear_count_seconds -= seconds;
	if (game->bear_count_seconds <= 0.0f)
	{
		game->bear_count_seconds = 1.0f;
		count_bears(game, potop, &camera);
	}
	for (int i = 0; i < game->bear_count; i++)
	{
		bear_mind_t *bear = &game->bears[i];
		potop_actor_info_t actor;

		bear->think_in -= seconds;
		bear->unthought_seconds += seconds;
		if (bear->think_in > 0.0f)
		{
			continue;
		}
		bear->think_in = THINK_SECONDS + random_between(game, 0.0f, 0.05f);
		if (potop_actor_get(potop, bear->actor_id, &actor) && !actor.dead)
		{
			think(game, potop, bear, &actor, bear->unthought_seconds, now, &camera);
		}
		bear->unthought_seconds = 0.0f;
	}
	{
		float milliseconds = 1000.0f * (float)(SDL_GetPerformanceCounter() - start) / (float)SDL_GetPerformanceFrequency();

		game->bear_milliseconds = game->bear_milliseconds + (milliseconds - game->bear_milliseconds) * 0.05f;
	}
}

// bears <count>: that many bears put round the player (a few hundred
// meters), each with its mind: how much the minds cost (F3)
static void bears_command(void *user, const char *arguments)
{
	game_t *game = user;
	potop_t *potop = game->potop;
	potop_camera_info_t camera;
	int count = SDL_clamp(SDL_atoi(arguments), 1, GAME_BEARS_MAX);
	int made = 0;

	potop_camera_get(potop, &camera);
	for (int i = 0; i < count; i++)
	{
		float angle = random_between(game, 0.0f, 2.0f * SDL_PI_F);
		float distance = random_between(game, 50.0f, 400.0f);
		float extent = potop_world_extent_meters(potop);
		// (on the map)
		float x = SDL_clamp(camera.walker_x + SDL_cosf(angle) * distance, 5.0f, extent - 5.0f);
		float z = SDL_clamp(camera.walker_z + SDL_sinf(angle) * distance, 5.0f, extent - 5.0f);

		made += potop_actor_spawn(potop, BEAR_TYPE, x, z, random_between(game, 0.0f, 2.0f * SDL_PI_F)) != 0 ? 1 : 0;
	}
	game->bear_count_seconds = 0.0f;
	SDL_Log("%d bears put down round you (F3: what their minds cost)", made);
}

void game_start_bears(game_t *game, potop_t *potop)
{
	game->potop = potop;
	potop_console_add_command(potop, "bears", "bears <count>", "That many bears round you, with their minds (a stress test; F3 shows the cost)",
		bears_command, game);
}

const char *game_bear_state_name(bear_state_t state)
{
	static const char *const names[BEAR_STATE_COUNT] = { "wander", "rest", "alert", "investigate", "warn", "attack", "flee" };

	return state >= 0 && state < BEAR_STATE_COUNT ? names[state] : "?";
}
