// The boredom walk (game.h, wander_t): an animal with nowhere to go just
// goes on, the way it faces, as a cow in a yard does. It looks ahead every
// WANDER_TICK_SECONDS (each walker at its own moment, not all in one
// frame):
//
//   free ahead       on it goes; now and then (drift_chance) a little to
//                    one side (drift_degrees), if that way is free too
//   blocked ahead    the nearest free way to one side or the other, ten
//                    degrees at a time out to search_degrees (once it has
//                    turned the last such: no running ahead of its own
//                    body, a spin on the spot in tenths of a second)
//   none             a corner: the nearest free way farther round (to 180);
//                    it stops and turns there on the spot (its turn clips)
//   none at all      a step back, if free behind: the last resort
//   not even that    it stands and tries on a while (give_up_seconds),
//                    then gives up: the walk is over
//
// What is free is what is there: the physics' bodies and the others
// (potop_actor_heading_free), no grid, no area. The engine keeps it going
// between looks (potop_actor_wander: a body in motion stays in motion).
//
// Each look that turns it says so in the trace (wander_drift, wander_turn,
// wander_back, wander_gave_up) for tools/animal_trace.py's colors.

#include "game.h"

#include <SDL3/SDL.h>

static const float WANDER_TICK_SECONDS = 0.1f;
static const float WANDER_BACK_LOOK_METERS = 0.4f;   // behind it: this much free for a step back
static const float WANDER_TURNED_RADIANS = 0.09f;    // its body this near its heading (5 degrees): done turning
static const float WANDER_STUCK_METERS = 0.15f;      // got on less than this...
static const float WANDER_STUCK_SECONDS = 1.5f;      // ...in this long: caught, a step back (or it gives up)

static float wander_random(wander_t *wander)
{
	uint32_t value = wander->random != 0 ? wander->random : 0x9e3779b9u;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	wander->random = value;
	return (float)(value >> 8) / 16777216.0f;
}

static float wrap_radians(float radians)
{
	while (radians > 3.14159265f)
	{
		radians -= 6.2831853f;
	}
	while (radians < -3.14159265f)
	{
		radians += 6.2831853f;
	}
	return radians;
}

void wander_start(wander_t *wander, potop_t *potop, uint32_t actor_id, const wander_style_t *style, float yaw_radians)
{
	uint32_t seed = wander->random != 0 ? wander->random : actor_id * 2654435761u + 1u;

	SDL_zerop(wander);
	wander->random = seed;
	wander->active = true;
	wander->heading = yaw_radians;
	// (its first look takes where it stands)
	wander->stuck_x = 1e9f;
	wander->seconds_left = style->least_seconds + (style->most_seconds - style->least_seconds) * wander_random(wander);
	// (its first look within a tick, at its own moment)
	wander->tick_in = WANDER_TICK_SECONDS * wander_random(wander);
	potop_actor_wander(potop, actor_id, wander->heading, style->speed_share);
}

void wander_stop(wander_t *wander, potop_t *potop, uint32_t actor_id)
{
	if (wander->active)
	{
		potop_actor_stop(potop, actor_id);
	}
	wander->active = false;
}

// Whether its body is still coming round (its yaw moved since the last
// look); keeps the yaw for the next
static bool wander_body_turning(wander_t *wander, const potop_actor_info_t *actor)
{
	bool turning = SDL_fabsf(wrap_radians(actor->yaw_radians - wander->last_yaw)) > 0.005f;

	wander->last_yaw = actor->yaw_radians;
	return turning;
}

// A step back, if free behind and it has some left this walk
static bool wander_step_back(wander_t *wander, game_t *game, potop_t *potop, uint32_t actor_id, const wander_style_t *style)
{
	if (wander->backs >= style->backs_most || !potop_actor_heading_free(potop, actor_id, wander->heading + 3.14159265f, WANDER_BACK_LOOK_METERS))
	{
		return false;
	}
	wander->backs++;
	wander->back_seconds = style->back_seconds;
	game_trace_event(game, potop, actor_id, "wander_back");
	potop_actor_wander(potop, actor_id, wander->heading, -style->back_share);
	return true;
}

// One look ahead (see the top); false: the walk is over
static bool wander_look(wander_t *wander, game_t *game, potop_t *potop, uint32_t actor_id, const wander_style_t *style, const potop_actor_info_t *actor)
{
	float drift = style->drift_degrees * 0.017453293f;
	float side = wander_random(wander) < 0.5f ? 1.0f : -1.0f;
	int steps = SDL_max(1, (int)(style->search_degrees / SDL_max(style->drift_degrees, 1.0f) + 0.5f));
	bool stuck;

	wander->looks++;
	// Stepping back: on till its time is up or something is behind it
	if (wander->back_seconds > 0.0f)
	{
		wander->back_seconds -= WANDER_TICK_SECONDS;
		if (wander->back_seconds > 0.0f && potop_actor_heading_free(potop, actor_id, wander->heading + 3.14159265f, WANDER_BACK_LOOK_METERS))
		{
			return true;
		}
		wander->back_seconds = 0.0f;
		potop_actor_wander(potop, actor_id, wander->heading, 0.0f);
	}
	if (wander->seconds_left <= 0.0f)
	{
		return false;
	}
	// Turning on the spot in a corner: till its body has come round (or
	// it gives up, its body not coming round at all)
	if (wander->cornering)
	{
		if (SDL_fabsf(wrap_radians(wander->heading - actor->yaw_radians)) > WANDER_TURNED_RADIANS)
		{
			if (!wander_body_turning(wander, actor))
			{
				wander->blocked_seconds += WANDER_TICK_SECONDS;
				if (wander->blocked_seconds >= 2.0f * style->give_up_seconds)
				{
					game_trace_event(game, potop, actor_id, "wander_gave_up");
					return false;
				}
			}
			return true;
		}
		wander->cornering = false;
		wander->blocked_seconds = 0.0f;
	}
	// (its legs going and its body not getting on, its mind seeing the way
	// free (its body not fitting round, another pushing it): caught, taken
	// as blocked ahead)
	if ((actor->x - wander->stuck_x) * (actor->x - wander->stuck_x) + (actor->z - wander->stuck_z) * (actor->z - wander->stuck_z)
		> WANDER_STUCK_METERS * WANDER_STUCK_METERS || !actor->wandering)
	{
		wander->stuck_x = actor->x;
		wander->stuck_z = actor->z;
		wander->stuck_seconds = 0.0f;
	}
	else
	{
		wander->stuck_seconds += WANDER_TICK_SECONDS;
	}
	stuck = wander->stuck_seconds >= WANDER_STUCK_SECONDS;
	if (!stuck && potop_actor_heading_free(potop, actor_id, wander->heading, style->look_meters))
	{
		wander->blocked_seconds = 0.0f;
		if (wander_random(wander) < style->drift_chance && potop_actor_heading_free(potop, actor_id, wander->heading + side * drift, style->look_meters))
		{
			wander->heading = wrap_radians(wander->heading + side * drift);
			game_trace_event(game, potop, actor_id, "wander_drift");
		}
		potop_actor_wander(potop, actor_id, wander->heading, style->speed_share);
		return true;
	}
	// Blocked ahead: the nearest free way to one side or the other, ten
	// degrees at a time out to search_degrees, once its body has come
	// round to the last turn (meanwhile it goes on as it is: the engine
	// stops its step if something is right at its nose)
	if (!stuck && SDL_fabsf(wrap_radians(wander->heading - actor->yaw_radians)) > WANDER_TURNED_RADIANS)
	{
		// (its body not coming round: caught, it gives up in the end)
		if (!wander_body_turning(wander, actor))
		{
			wander->blocked_seconds += WANDER_TICK_SECONDS;
			if (wander->blocked_seconds >= 2.0f * style->give_up_seconds)
			{
				game_trace_event(game, potop, actor_id, "wander_gave_up");
				return false;
			}
		}
		return true;
	}
	wander->stuck_seconds = 0.0f;
	wander->stuck_x = 1e9f;
	// (caught and its body not coming round to its heading either: its
	// rump against something, no turn and no way on; only back is left)
	if (stuck && SDL_fabsf(wrap_radians(wander->heading - actor->yaw_radians)) > WANDER_TURNED_RADIANS)
	{
		if (wander_step_back(wander, game, potop, actor_id, style))
		{
			return true;
		}
		game_trace_event(game, potop, actor_id, "wander_gave_up");
		return false;
	}
	for (int step = 1; step <= steps; step++)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			float heading = wander->heading + (pass == 0 ? side : -side) * drift * (float)step;

			if (potop_actor_heading_free(potop, actor_id, heading, style->look_meters))
			{
				wander->heading = wrap_radians(heading);
				wander->blocked_seconds = 0.0f;
				game_trace_event(game, potop, actor_id, "wander_turn");
				potop_actor_wander(potop, actor_id, wander->heading, style->speed_share);
				return true;
			}
		}
	}
	// Nothing free round it: set out facing a wall, another, there is no
	// walk at all (rather than back and on and back at it, walk after
	// walk); on the way, a step back, the last resort
	if (wander->looks <= 1)
	{
		game_trace_event(game, potop, actor_id, "wander_gave_up");
		return false;
	}
	// A corner: the nearest free way farther round, to 180; it stops and
	// turns there on the spot (its turn clips), then walks on
	for (int step = steps + 1; step * drift <= 3.14159265f + 0.001f; step++)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			float heading = wander->heading + (pass == 0 ? side : -side) * drift * (float)step;

			if (potop_actor_heading_free(potop, actor_id, heading, style->look_meters))
			{
				wander->heading = wrap_radians(heading);
				wander->cornering = true;
				wander->blocked_seconds = 0.0f;
				game_trace_event(game, potop, actor_id, "wander_corner");
				potop_actor_wander(potop, actor_id, wander->heading, 0.0f);
				return true;
			}
		}
	}
	if (wander_step_back(wander, game, potop, actor_id, style))
	{
		return true;
	}
	// Not even that: it stands, and tries on a while
	potop_actor_wander(potop, actor_id, wander->heading, 0.0f);
	wander->blocked_seconds += WANDER_TICK_SECONDS;
	if (wander->blocked_seconds >= style->give_up_seconds)
	{
		game_trace_event(game, potop, actor_id, "wander_gave_up");
		return false;
	}
	return true;
}

bool wander_update(wander_t *wander, game_t *game, potop_t *potop, uint32_t actor_id, const wander_style_t *style, float seconds)
{
	potop_actor_info_t actor;

	if (!wander->active)
	{
		return false;
	}
	wander->seconds_left -= seconds;
	wander->tick_in -= seconds;
	if (wander->tick_in > 0.0f)
	{
		return true;
	}
	// (its own moment in each tenth: the walkers spread over the frames)
	wander->tick_in += WANDER_TICK_SECONDS;
	if (wander->tick_in <= 0.0f)
	{
		wander->tick_in = WANDER_TICK_SECONDS * (0.5f + wander_random(wander));
	}
	if (!potop_actor_get(potop, actor_id, &actor) || actor.dead || !wander_look(wander, game, potop, actor_id, style, &actor))
	{
		wander_stop(wander, potop, actor_id);
		return false;
	}
	return true;
}
