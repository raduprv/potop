// The people who work the buildings: a smith for each smithy on the map,
// with a life of his own. He lives in the nearest free dwelling (a "door"
// spot; none: he sleeps in his smithy) and drinks at the nearest inn
// ("inn_door"); his day (drawn again each day, a few minutes either way)
// takes him to work about seven, home or to the inn for lunch, back, home
// or to the inn after six, home late. He always is somewhere: within
// WORKER_SHOW_METERS of the eye he has an actor (it finds its way, goes in
// at a door onto its spot and is taken away), farther none and he walks
// his way by himself (found with potop_find_path); in a house or the inn
// there is none either.
//
// The smithy keeps the state of its work (workshop_t): how the fire burns
// (it dies down; the bellows and a shovel of coal bring it up), how hot the
// iron is (in the fire it heats as well as the fire burns; on the anvil it
// cools), how much hammering the piece still wants. The smith looks at it
// whenever he is done with something and picks what it needs: blow the
// fire up, heat the iron, hammer it while it is hot, look it over, quench
// it, hang the piece up and take the next, now and then a breather out
// front. So his day never runs the same twice.
//
// What he does is a short queue of steps: walk to a place (the smithy's
// spots: smith_forge, smith_bellows, smith_anvil, smith_trough, smith_rack
// (by smith_aisle), smith_rest, or a point in its smith_yard area), turn its
// way, play a clip
// there a while; the bellows (the smithy's module) pump while he is at
// their pole.

#include "game.h"

#include <SDL3/SDL.h>

#define WORKER_ACTOR_TYPE "blacksmith"
#define WORKER_MAP_METERS 5000.0f      // smithies this near the eye get their smith (the whole map)
#define WORKER_SHOW_METERS 480.0f      // nearer the eye than this he has an actor (seen, a rifle's scope away)
#define WORKER_HIDE_METERS 520.0f      // farther, none (he goes on by himself)
#define WORKER_HOME_METERS 400.0f      // his home: the nearest free dwelling this near his smithy
#define WORKER_INN_METERS 800.0f       // his inn: the nearest this near
#define WORKER_WALK_METERS_PER_SECOND 1.3f // with no actor, along his way (actors.ini's blacksmith walks so)
#define WORKER_ARRIVED_METERS 1.5f     // a walk somewhere ended this near: there
#define WORKER_DOOR_NEAR_METERS 4.0f   // a walk to a door ended this near (its porch, its steps in the way): the rest straight
#define WORKER_DOOR_OUT_METERS 3.0f    // out of a door, this far straight out first
#define WORKER_DOOR_FRONT_METERS 1.2f  // to a door: by a point this far straight before it
#define WORKER_DOOR_CONE_COSINE 0.55f  // before a door within this (about 57 degrees off straight out): straight up to it
#define WORKER_DOOR_SWITCH_METERS 2.6f // this near a door's spot and before it: straight up to it at once, no stop
#define WORKER_DOOR_SECONDS 10.0f      // onto a door's spot (up its stairs) at most this long, then in
#define WORKER_SCAN_SECONDS 5.0f      // those whose smithy is gone taken off this often
#define WORKER_IDLE_CUT_SECONDS 2.0f   // due off: idling cut short after this
#define LUNCH_AT_INN_SHARE 0.4f        // lunch at the inn this often (else at home)
#define SKIPS_INN_SHARE 0.25f          // an evening at home this often (else at the inn)
#define WORKER_FACE_SECONDS 3.0f       // turning to his work (or stepping onto it) at most this long
#define WORKER_ON_SPOT_METERS 0.06f    // on his place
#define WORKER_FACE_RADIANS 0.035f     // near enough facing it (2 degrees)
#define WORKER_THERE_METERS 0.15f      // a walk that ended this near: there (else tried again)

// The work (a share, 0..1)
#define FIRE_DIES_PER_SECOND 0.006f    // the fire burning down
#define FIRE_BLOWN_PER_SECOND 0.14f    // the bellows bringing it up
#define FIRE_COAL 0.25f                // a shovel of coal
#define FIRE_LOW 0.3f                  // lower: it wants the bellows
#define FIRE_GOOD 0.5f                 // the iron heating, a fire under this is blown up meanwhile
#define FIRE_UP 0.92f                  // blown up to this
#define HEAT_PER_SECOND 0.05f          // the iron heating in a fire burning at full
#define HEAT_COOLS_PER_SECOND 0.025f   // cooling out of it
#define HEAT_FORGE 0.85f               // hot enough to work
#define HEAT_NEAR_ENOUGH 0.2f          // this short of it: to the anvil all the same (no last few seconds at the fire)
#define HEAT_COLD 0.3f                 // too cold to hammer
#define PIECE_LEAST_SECONDS 20.0f      // a piece wants this much hammering, up to the most
#define PIECE_MOST_SECONDS 45.0f
#define YARD_STEP_LEAST_METERS 2.0f    // out front, from one place to the next at least this far
#define FORGE_LEAST_SECONDS 10.0f      // a heat at the fire no shorter
#define ANVIL_LEAST_SECONDS 12.0f      // nor at the anvil
#define BELLOWS_LEAST_SECONDS 8.0f     // nor at the bellows
#define BREATHER_SHARE 0.6f            // after a piece, a breather this often
#define BREAK_LEAST_SECONDS 120.0f     // after this much work (up to the most), a proper break
#define BREAK_MOST_SECONDS 240.0f
#define FIRST_BREAK_SECONDS 50.0f      // the first one sooner (up to twice this)

// Where a step is: a spot of the building, or a point (in its yard), or
// right where he stands
typedef enum
{
	PLACE_HERE,
	PLACE_SPOT,
	PLACE_POINT
} place_kind_t;

static float distance_squared_2d(float ax, float az, float bx, float bz)
{
	return (ax - bx) * (ax - bx) + (az - bz) * (az - bz);
}

static float wrap_angle(float radians)
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

// The worker's own dice
static float random_unit(worker_mind_t *worker)
{
	worker->random_state = worker->random_state * 1664525u + 1013904223u;
	return (float)(worker->random_state >> 8) / 16777216.0f;
}

static float random_between(worker_mind_t *worker, float least, float most)
{
	return least + (most - least) * random_unit(worker);
}

static int find_worker(const game_t *game, uint32_t building)
{
	for (int w = 0; w < game->worker_count; w++)
	{
		if (game->workers[w].building == building)
		{
			return w;
		}
	}
	return -1;
}

// A step on the end of his queue
static worker_step_t *push_step(worker_mind_t *worker, const char *action, float seconds)
{
	worker_step_t *step;

	if (worker->step_count >= WORKER_STEPS_MAX)
	{
		return NULL;
	}
	step = &worker->steps[worker->step_count++];
	SDL_zerop(step);
	SDL_strlcpy(step->action, action, sizeof(step->action));
	step->seconds = seconds;
	step->place = PLACE_HERE;
	return step;
}

static worker_step_t *push_at_spot(worker_mind_t *worker, const char *spot, const char *action, float seconds)
{
	worker_step_t *step = push_step(worker, action, seconds);

	if (step != NULL)
	{
		step->place = PLACE_SPOT;
		SDL_strlcpy(step->spot, spot, sizeof(step->spot));
	}
	return step;
}

// The building's spot by name; false if it has none
static bool find_spot(potop_t *potop, uint32_t building, const char *name, potop_spot_t *spot_out)
{
	int index = potop_object_spot_find(potop, building, name, 0);

	return index >= 0 && potop_object_spot_get(potop, building, index, spot_out);
}

// Where a step is: its place and the way to face there; false: right here
static bool step_place(potop_t *potop, const worker_mind_t *worker, const worker_step_t *step, float *x, float *z, float *yaw)
{
	potop_spot_t spot;

	if (step->place == PLACE_POINT)
	{
		*x = step->x;
		*z = step->z;
		*yaw = step->yaw;
		return true;
	}
	if (step->place == PLACE_SPOT && find_spot(potop, worker->building, step->spot, &spot))
	{
		*x = spot.x;
		*z = spot.z;
		*yaw = spot.yaw_radians;
		return true;
	}
	return false;
}

// A point in the smithy's yard (out front), a free one if he finds one;
// false: no yard
static bool yard_point(potop_t *potop, worker_mind_t *worker, float *x_out, float *z_out, float *yaw_out)
{
	potop_spot_t yard;

	if (!find_spot(potop, worker->building, "smith_yard", &yard))
	{
		return false;
	}
	for (int tries = 0; tries < 10; tries++)
	{
		float u = random_between(worker, -yard.half_width, yard.half_width);
		float v = random_between(worker, -yard.half_depth, yard.half_depth);
		float c = SDL_cosf(yard.yaw_radians);
		float s = SDL_sinf(yard.yaw_radians);

		*x_out = yard.x + u * c + v * s;
		*z_out = yard.z - u * s + v * c;
		// (looking out, or back at the smithy, or anywhere between)
		*yaw_out = yard.yaw_radians + random_between(worker, -2.5f, 2.5f);
		// (somewhere else than where he stands, a few steps off; the meter
		// grid is coarse by a building: the last try taken as it is)
		{
			potop_actor_info_t actor;
			bool far_enough = !potop_actor_get(potop, worker->actor, &actor)
				|| distance_squared_2d(actor.x, actor.z, *x_out, *z_out) > YARD_STEP_LEAST_METERS * YARD_STEP_LEAST_METERS;

			if ((far_enough && potop_actor_can_stand(potop, worker->actor, *x_out, *z_out)) || tries == 9)
			{
				return true;
			}
		}
	}
	return true;
}

// Something to do standing about: a look round, arms crossed a while, hands
// on his hips, a stretch, his mug, a wipe of the brow
static void push_pastime(worker_mind_t *worker)
{
	float pick = random_unit(worker);

	if (pick < 0.2f)
	{
		push_step(worker, "idle_look", 4.0f);
	}
	else if (pick < 0.45f)
	{
		push_step(worker, "arms_in", 0.6f);
		push_step(worker, "idle_arms", random_between(worker, 8.0f, 25.0f));
		push_step(worker, "arms_out", 0.6f);
	}
	else if (pick < 0.65f)
	{
		push_step(worker, "idle_hips", random_between(worker, 8.0f, 20.0f));
	}
	else if (pick < 0.8f)
	{
		push_step(worker, "drink", 2.5f);
		push_step(worker, "idle", random_between(worker, 3.0f, 8.0f));
		push_step(worker, "drink", 2.5f);
	}
	else if (pick < 0.9f)
	{
		push_step(worker, "idle_stretch", 3.5f);
	}
	else
	{
		push_step(worker, "idle_wipe", 2.5f);
	}
	push_step(worker, "idle", random_between(worker, 1.0f, 4.0f));
}

// A breather: out of the smithy into its yard, a while at one place and
// another, idling (arms crossed, hands on his hips, his mug, a look round);
// a minute or two, then back to the work
static void plan_breather(potop_t *potop, worker_mind_t *worker, bool long_break)
{
	int stops = long_break ? 3 + (int)(random_unit(worker) * 3.0f) : 1 + (int)(random_unit(worker) * 2.0f);

	for (int k = 0; k < stops && worker->step_count < WORKER_STEPS_MAX - 4; k++)
	{
		float x;
		float z;
		float yaw;
		worker_step_t *step;

		if (!yard_point(potop, worker, &x, &z, &yaw))
		{
			push_at_spot(worker, "smith_rest", "idle", random_between(worker, 1.0f, 2.5f));
			push_pastime(worker);
			return;
		}
		step = push_step(worker, "idle", random_between(worker, 1.0f, 3.0f));
		if (step != NULL)
		{
			step->place = PLACE_POINT;
			step->x = x;
			step->z = z;
			step->yaw = yaw;
		}
		push_pastime(worker);
		if ((long_break || random_unit(worker) < 0.4f) && worker->step_count < WORKER_STEPS_MAX - 4)
		{
			push_pastime(worker);
		}
	}
}

// A heat, in one go: the fire blown up first if it is low (the bellows a
// step from the hearth), then the iron in it the whole while it takes,
// passing the time at the fire (a shovel of coal, a wipe, a glance round),
// not back and forth
static void plan_heat(worker_mind_t *worker)
{
	workshop_t *shop = &worker->shop;
	float fire = shop->fire;
	float wait;

	if (fire < FIRE_GOOD)
	{
		float blow_seconds = SDL_max((FIRE_UP - fire) / FIRE_BLOWN_PER_SECOND, BELLOWS_LEAST_SECONDS) + random_between(worker, 0.0f, 3.0f);
		worker_step_t *blow;

		// (the iron in the fire first, then to the bellows)
		push_at_spot(worker, "smith_forge", "smith_forge", 2.0f);
		blow = push_at_spot(worker, "smith_bellows", "smith_bellows", blow_seconds);
		if (blow != NULL)
		{
			blow->state = true;
		}
		fire = FIRE_UP;
	}
	// (the fire dying meanwhile: reckoned at a little less than it is; a
	// heat already under way topped up, not begun again)
	wait = (HEAT_FORGE - shop->heat) / (HEAT_PER_SECOND * SDL_max(fire - 0.05f, 0.15f));
	wait = SDL_max(wait, shop->in_fire ? 2.0f : FORGE_LEAST_SECONDS);
	// (the wait shared out: at the fire, something else a moment (the iron
	// heating on), at the fire the rest of it)
	{
		float first = wait < 6.0f ? wait : wait * random_between(worker, 0.3f, 0.5f);
		float meanwhile = 0.0f;
		float pick = random_unit(worker);

		push_at_spot(worker, "smith_forge", "smith_forge", first);
		if (wait >= 6.0f && pick < 0.25f)
		{
			push_step(worker, "smith_coal", 3.0f);
			meanwhile = 3.0f;
		}
		else if (wait >= 6.0f && pick < 0.45f)
		{
			push_step(worker, "idle_wipe", 2.5f);
			meanwhile = 2.5f;
		}
		else if (wait >= 6.0f && pick < 0.6f)
		{
			push_step(worker, "idle_look", 4.0f);
			meanwhile = 4.0f;
		}
		if (wait - first - meanwhile > 0.5f)
		{
			push_step(worker, "smith_forge", wait - first - meanwhile);
		}
	}
}

// What the work wants now: a few steps on his queue
static void plan_next(potop_t *potop, worker_mind_t *worker)
{
	workshop_t *shop = &worker->shop;

	worker->step_count = 0;
	worker->step_index = 0;
	// A new piece: hang the last up if there was one (a breather now and
	// then after it), take the next and put it in the fire
	if (!shop->has_piece)
	{
		if (shop->pieces_done > 0)
		{
			push_at_spot(worker, "smith_rack", "smith_rack_put", 2.0f);
			// (worked long enough: a proper break out front, a few minutes;
			// else now and then a breather)
			if (shop->worked_seconds >= shop->break_after || random_unit(worker) < BREATHER_SHARE)
			{
				bool long_break = shop->worked_seconds >= shop->break_after;

				plan_breather(potop, worker, long_break);
				if (long_break)
				{
					shop->worked_seconds = 0.0f;
					shop->break_after = random_between(worker, BREAK_LEAST_SECONDS, BREAK_MOST_SECONDS);
				}
				shop->has_piece = false;
				shop->pieces_done = 0;
				return;
			}
		}
		push_at_spot(worker, "smith_rack", "smith_rack_take", 2.0f);
		shop->has_piece = true;
		shop->piece_left = random_between(worker, PIECE_LEAST_SECONDS, PIECE_MOST_SECONDS);
		shop->heat = 0.0f;
		shop->in_fire = false;
		plan_heat(worker);
		return;
	}
	// The piece hammered enough: quenched, done
	if (shop->piece_left <= 0.0f)
	{
		if (random_unit(worker) < 0.5f)
		{
			push_at_spot(worker, "smith_anvil", "smith_inspect", 3.0f);
		}
		push_at_spot(worker, "smith_trough", "smith_quench", 2.0f);
		shop->has_piece = false;
		shop->pieces_done++;
		return;
	}
	// Hot (or near enough: no trip for a few seconds' more heat): the whole
	// heat at the anvil, heavy blows and light, a look at it now and then
	if (shop->heat >= HEAT_FORGE - HEAT_NEAR_ENOUGH)
	{
		float seconds = SDL_max(SDL_min((shop->heat - HEAT_COLD) / HEAT_COOLS_PER_SECOND, shop->piece_left), ANVIL_LEAST_SECONDS);
		float heavy = seconds * random_between(worker, 0.4f, 0.8f);

		push_at_spot(worker, "smith_anvil", "smith_hammer", heavy);
		if (random_unit(worker) < 0.35f)
		{
			push_step(worker, "smith_inspect", 3.0f);
		}
		push_step(worker, "smith_hammer_light", SDL_max(seconds - heavy, 0.8f));
		return;
	}
	plan_heat(worker);
}

// Off to a place: walking there and turned its way at once (a step or two
// away: turned on the spot onto it); to the rack or from it, by the aisle
// (smith_aisle: the gap between the hearth, the trough and the bench)
static void go_to(potop_t *potop, worker_mind_t *worker, float x, float z, float yaw, bool outside)
{
	float points[3][2];
	int count = 0;
	potop_spot_t rack;
	potop_spot_t aisle;
	potop_spot_t door;
	potop_actor_info_t actor;
	bool have_rack = find_spot(potop, worker->building, "smith_rack", &rack);
	bool to_rack = have_rack && distance_squared_2d(x, z, rack.x, rack.z) < 0.01f;

	if (!potop_actor_get(potop, worker->actor, &actor))
	{
		return;
	}
	// (in or out by a door: right of the trough to or from the rack, left
	// of the anvil's stump otherwise)
	if (worker->outside != outside)
	{
		bool near_rack = have_rack && distance_squared_2d(actor.x, actor.z, rack.x, rack.z) < 0.64f;
		const char *name = (outside ? near_rack : to_rack) ? "smith_door_right" : "smith_door_left";

		if (find_spot(potop, worker->building, name, &door))
		{
			points[count][0] = door.x;
			points[count][1] = door.z;
			count++;
		}
	}
	// (inside, to the rack or from it: by the aisle, not round the trough)
	else if (!outside && have_rack && find_spot(potop, worker->building, "smith_aisle", &aisle))
	{
		bool from_rack = distance_squared_2d(actor.x, actor.z, rack.x, rack.z) < 0.64f;

		if ((to_rack || from_rack) && !(to_rack && from_rack) && distance_squared_2d(actor.x, actor.z, aisle.x, aisle.z) > 0.09f
			&& distance_squared_2d(x, z, aisle.x, aisle.z) > 0.09f)
		{
			points[count][0] = aisle.x;
			points[count][1] = aisle.z;
			count++;
		}
	}
	points[count][0] = x;
	points[count][1] = z;
	count++;
	worker->outside = outside;
	potop_actor_stop_looking(potop, worker->actor);
	potop_actor_play(potop, worker->actor, "idle");
	potop_actor_walk_straight_exact(potop, worker->actor, (const float (*)[2])points, count, yaw);
}

// The step he is on begun: walking to its place, or at once if it is here
static void begin_step(potop_t *potop, worker_mind_t *worker)
{
	const worker_step_t *step = &worker->steps[worker->step_index];
	float x;
	float z;
	float yaw;

	worker->tries = 0;
	worker->seconds = 0.0f;
	if (step_place(potop, worker, step, &x, &z, &yaw))
	{
		potop_actor_info_t actor;

		// (already there and facing so: straight to it)
		if (potop_actor_get(potop, worker->actor, &actor) && distance_squared_2d(actor.x, actor.z, x, z) < WORKER_ON_SPOT_METERS * WORKER_ON_SPOT_METERS
			&& SDL_fabsf(wrap_angle(actor.yaw_radians - yaw)) < WORKER_FACE_RADIANS)
		{
			worker->phase = WORKER_FACING;
			return;
		}
		go_to(potop, worker, x, z, yaw, step->place == PLACE_POINT);
		worker->phase = WORKER_WALKING;
		return;
	}
	worker->phase = WORKER_FACING;
}

static void start_travel(potop_t *potop, worker_mind_t *worker, life_place_t place);

// The work wound up, his day wanting him elsewhere: the piece in hand put
// on the rack (left as it is, for later), then straight off
static void wind_up(potop_t *potop, worker_mind_t *worker)
{
	(void)potop;
	worker->step_count = 0;
	worker->step_index = 0;
	worker->leaving = 2;
	if (worker->shop.has_piece)
	{
		push_at_spot(worker, "smith_rack", "smith_rack_put", 2.0f);
		worker->shop.has_piece = false;
		worker->shop.in_fire = false;
	}
	worker->shop.pieces_done = 0;
}

// The step done: the building's state put back, on to the next (or a new
// plan; or, his day wanting him elsewhere, the work wound up and off)
static void end_step(potop_t *potop, worker_mind_t *worker)
{
	if (worker->step_index < worker->step_count && worker->steps[worker->step_index].state)
	{
		potop_object_set_state(potop, worker->building, "still");
	}
	worker->step_index++;
	// (due off: the rest of his plan dropped, whatever it was, a break's
	// idling or the next heat)
	if (worker->leaving == 1)
	{
		wind_up(potop, worker);
	}
	else if (worker->step_index >= worker->step_count)
	{
		if (worker->leaving == 2)
		{
			start_travel(potop, worker, worker->place);
			return;
		}
		plan_next(potop, worker);
	}
	if (worker->step_count > 0)
	{
		begin_step(potop, worker);
	}
}

// His actor taken away (gone far off, or in somewhere): the bellows let
// be, where he stands kept
static void hide(potop_t *potop, worker_mind_t *worker)
{
	potop_actor_info_t actor;

	if (worker->actor == 0)
	{
		return;
	}
	if (worker->step_index < worker->step_count && worker->steps[worker->step_index].state)
	{
		potop_object_set_state(potop, worker->building, "still");
	}
	if (potop_actor_get(potop, worker->actor, &actor))
	{
		worker->x = actor.x;
		worker->z = actor.z;
		worker->yaw = actor.yaw_radians;
	}
	potop_actor_remove(potop, worker->actor);
	worker->actor = 0;
}

static void take_off(game_t *game, potop_t *potop, int index)
{
	hide(potop, &game->workers[index]);
	game->workers[index] = game->workers[--game->worker_count];
}

static void log_life(potop_t *potop, const worker_mind_t *worker, const char *what)
{
	static const char *const places[3] = { "work", "home", "the inn" };

	if (SDL_getenv("POTOP_DEBUG_WORKERS") != NULL)
	{
		SDL_Log("Worker of %u at %.2f h: %s %s (at %.1f, %.1f%s)", (unsigned)worker->building, (double)potop_day_hour(potop), what, places[worker->place],
			(double)worker->x, (double)worker->z, worker->actor != 0 ? ", seen" : "");
	}
}

// His day's hours, drawn for the day: off to work about seven, lunch about
// noon for an hour (at the inn now and then), the work's end about six, the
// inn till about nine (most evenings)
static void draw_day(worker_mind_t *worker)
{
	life_day_t *hours = &worker->hours;

	hours->work_start = 7.0f + random_between(worker, -0.4f, 0.4f);
	hours->lunch_start = 12.0f + random_between(worker, -0.3f, 0.3f);
	hours->lunch_end = 13.0f + random_between(worker, -0.25f, 0.25f);
	hours->work_end = 18.0f + random_between(worker, -0.4f, 0.4f);
	hours->inn_end = 21.0f + random_between(worker, -0.6f, 0.6f);
	hours->lunch_at_inn = random_unit(worker) < LUNCH_AT_INN_SHARE;
	hours->skips_inn = random_unit(worker) < SKIPS_INN_SHARE;
}

// Where his day wants him at this hour
static life_place_t wanted_place(const worker_mind_t *worker, float hour)
{
	const life_day_t *hours = &worker->hours;
	bool has_inn = worker->inn != 0;

	if (hour < hours->work_start)
	{
		return LIFE_AT_HOME;
	}
	if (hour < hours->lunch_start)
	{
		return LIFE_AT_WORK;
	}
	if (hour < hours->lunch_end)
	{
		return hours->lunch_at_inn && has_inn ? LIFE_AT_INN : LIFE_AT_HOME;
	}
	if (hour < hours->work_end)
	{
		return LIFE_AT_WORK;
	}
	if (hour < hours->inn_end)
	{
		return !hours->skips_inn && has_inn ? LIFE_AT_INN : LIFE_AT_HOME;
	}
	return LIFE_AT_HOME;
}

// Where a place of his day is: his smithy's yard; before his home's door
// (none: his smithy's corner, he sleeps there); before the inn's door.
// False if it is not there
static bool life_place_point(potop_t *potop, const worker_mind_t *worker, life_place_t place, float *x, float *z, float *yaw)
{
	potop_spot_t spot;
	bool found = false;

	if (place == LIFE_AT_WORK)
	{
		found = find_spot(potop, worker->building, "smith_yard", &spot) || find_spot(potop, worker->building, "smith_anvil", &spot);
	}
	else if (place == LIFE_AT_HOME)
	{
		found = worker->home != 0 ? find_spot(potop, worker->home, "door", &spot) : find_spot(potop, worker->building, "smith_rest", &spot);
	}
	else
	{
		found = worker->inn != 0 && find_spot(potop, worker->inn, "inn_door", &spot);
	}
	if (found)
	{
		*x = spot.x;
		*z = spot.z;
		*yaw = spot.yaw_radians;
	}
	return found;
}

// The top of a door's stairs (its "_sill" spot: before the door, up on a
// high floor's level or its porch); false: none, the door's spot is it
static bool life_place_sill(potop_t *potop, const worker_mind_t *worker, life_place_t place, float *x, float *z)
{
	potop_spot_t spot;
	bool found = false;

	if (place == LIFE_AT_HOME && worker->home != 0)
	{
		found = find_spot(potop, worker->home, "door_sill", &spot);
	}
	else if (place == LIFE_AT_INN && worker->inn != 0)
	{
		found = find_spot(potop, worker->inn, "inn_door_sill", &spot);
	}
	if (found)
	{
		*x = spot.x;
		*z = spot.z;
	}
	return found;
}

// Where his actor is sent to find his way to (potop_actor_go_to): his
// place, but for a door a meter before its spot (the foot of its stairs),
// so that he comes up to it from the front
static void travel_point(potop_t *potop, const worker_mind_t *worker, float x, float z, float yaw, float *x_out, float *z_out)
{
	(void)potop;
	*x_out = x;
	*z_out = z;
	if (worker->place != LIFE_AT_WORK)
	{
		*x_out = x - SDL_sinf(yaw) * WORKER_DOOR_FRONT_METERS;
		*z_out = z - SDL_cosf(yaw) * WORKER_DOOR_FRONT_METERS;
	}
}

// Whether he stands before the door (its spot at x, z, facing it by yaw):
// within a cone out from it, so that he walks straight up to it
static bool before_door(float actor_x, float actor_z, float x, float z, float yaw)
{
	float dx = actor_x - x;
	float dz = actor_z - z;
	float d = SDL_sqrtf(dx * dx + dz * dz);

	// (out from the door: against the way one faces it)
	return d < 0.3f || (-SDL_sinf(yaw) * dx - SDL_cosf(yaw) * dz) > d * WORKER_DOOR_CONE_COSINE;
}

// The work: a plan from where it stands (half the time he is found idling
// out front, as he comes into view)
static void begin_work(potop_t *potop, worker_mind_t *worker, bool maybe_breather)
{
	worker->step_count = 0;
	worker->step_index = 0;
	worker->leaving = 0;
	if (maybe_breather && random_unit(worker) < 0.5f)
	{
		plan_breather(potop, worker, false);
	}
	else
	{
		plan_next(potop, worker);
	}
	if (worker->step_count > 0)
	{
		begin_step(potop, worker);
	}
}

// There: at his work (out front, to it by a door), or in at a door (his
// actor gone in with him)
static void arrive(potop_t *potop, worker_mind_t *worker)
{
	float yaw;

	life_place_point(potop, worker, worker->place, &worker->x, &worker->z, &yaw);
	worker->route_count = 0;
	worker->at_door = false;
	if (worker->place == LIFE_AT_WORK)
	{
		worker->life = LIFE_WORK;
		worker->outside = true;
		worker->step_count = 0;
		worker->step_index = 0;
		log_life(potop, worker, "came to");
		if (worker->actor != 0)
		{
			begin_work(potop, worker, false);
		}
		return;
	}
	hide(potop, worker);
	worker->life = LIFE_INSIDE;
	life_place_point(potop, worker, worker->place, &worker->x, &worker->z, &worker->yaw);
	life_place_sill(potop, worker, worker->place, &worker->x, &worker->z);
	log_life(potop, worker, "went in at");
}

// Sent on his way there: his actor finds its way; with none, the way found
// now and walked by itself
static void start_travel(potop_t *potop, worker_mind_t *worker, life_place_t place)
{
	float x;
	float z;
	float yaw;
	bool leaving_work = worker->life == LIFE_WORK;

	if (worker->step_index < worker->step_count && worker->steps[worker->step_index].state)
	{
		potop_object_set_state(potop, worker->building, "still");
	}
	// (out of a door: facing away from it, a few steps straight out first)
	worker->from_door = worker->life == LIFE_INSIDE;
	if (worker->from_door)
	{
		worker->yaw = wrap_angle(worker->yaw + 3.14159265f);
	}
	worker->step_count = 0;
	worker->step_index = 0;
	worker->leaving = 0;
	worker->place = place;
	worker->life = LIFE_TRAVEL;
	worker->at_door = false;
	worker->travel_seconds = 0.0f;
	worker->travel_tries = 0;
	worker->route_count = 0;
	worker->route_index = 0;
	if (!life_place_point(potop, worker, place, &x, &z, &yaw))
	{
		arrive(potop, worker);
		return;
	}
	log_life(potop, worker, "off to");
	// (from inside his smithy: out by its nearer door first, then on his
	// way (no way is found from among the anvil, the trough and the bench))
	if (worker->actor != 0 && leaving_work && !worker->outside)
	{
		potop_spot_t left;
		potop_spot_t right;
		bool has_left = find_spot(potop, worker->building, "smith_door_left", &left);
		bool has_right = find_spot(potop, worker->building, "smith_door_right", &right);
		potop_actor_info_t actor;

		if ((has_left || has_right) && potop_actor_get(potop, worker->actor, &actor))
		{
			const potop_spot_t *door = !has_right || (has_left && distance_squared_2d(actor.x, actor.z, left.x, left.z)
				<= distance_squared_2d(actor.x, actor.z, right.x, right.z)) ? &left : &right;
			float points[1][2] = { { door->x, door->z } };

			worker->outside = true;
			worker->from_door = true;
			potop_actor_stop_looking(potop, worker->actor);
			potop_actor_play(potop, worker->actor, "idle");
			potop_actor_walk_straight(potop, worker->actor, (const float (*)[2])points, 1);
			return;
		}
	}
	if (worker->actor != 0)
	{
		potop_actor_stop_looking(potop, worker->actor);
		potop_actor_play(potop, worker->actor, "idle");
		travel_point(potop, worker, x, z, yaw, &x, &z);
		if (!potop_actor_go_to(potop, worker->actor, x, z))
		{
			worker->travel_tries++;
		}
		return;
	}
	if (worker->from_door)
	{
		float foot_x;
		float foot_z;
		float foot_yaw;
		float sill_x;
		float sill_z;

		// (the door he comes out of: where he was, not where he goes)
		worker->exit_count = 0;
		for (int p = LIFE_AT_HOME; p <= LIFE_AT_INN; p++)
		{
			if (life_place_sill(potop, worker, (life_place_t)p, &sill_x, &sill_z)
				&& distance_squared_2d(sill_x, sill_z, worker->x, worker->z) < 0.25f && life_place_point(potop, worker, (life_place_t)p, &foot_x, &foot_z, &foot_yaw))
			{
				// (down the stairs to their foot, a step on)
				worker->route[0][0] = foot_x;
				worker->route[0][1] = foot_z;
				worker->route[1][0] = foot_x - SDL_sinf(foot_yaw) * 1.0f;
				worker->route[1][1] = foot_z - SDL_cosf(foot_yaw) * 1.0f;
				worker->exit_count = 2;
			}
		}
		if (worker->exit_count == 0)
		{
			worker->route[0][0] = worker->x + SDL_sinf(worker->yaw) * WORKER_DOOR_OUT_METERS;
			worker->route[0][1] = worker->z + SDL_cosf(worker->yaw) * WORKER_DOOR_OUT_METERS;
			worker->exit_count = 1;
		}
		worker->route_count = worker->exit_count + potop_find_path(potop, WORKER_ACTOR_TYPE, worker->route[worker->exit_count - 1][0],
			worker->route[worker->exit_count - 1][1], x, z, worker->route + worker->exit_count, WORKER_ROUTE_MAX - worker->exit_count, NULL);
		return;
	}
	worker->route_count = potop_find_path(potop, WORKER_ACTOR_TYPE, worker->x, worker->z, x, z, worker->route, WORKER_ROUTE_MAX, NULL);
}

// His actor put down where he is (his day going on there): at his work
// (as before: on his place, a plan from there), on his way (on to where
// he was going)
static void show(potop_t *potop, worker_mind_t *worker)
{
	potop_spot_t anvil;
	float x;
	float z;
	float yaw;

	if (worker->life == LIFE_WORK)
	{
		if (!find_spot(potop, worker->building, "smith_anvil", &anvil))
		{
			return;
		}
		worker->actor = potop_actor_spawn(potop, WORKER_ACTOR_TYPE, anvil.x, anvil.z, anvil.yaw_radians);
		if (worker->actor == 0)
		{
			return;
		}
		worker->outside = false;
		worker->x = anvil.x;
		worker->z = anvil.z;
		begin_work(potop, worker, true);
		return;
	}
	if (worker->life != LIFE_TRAVEL)
	{
		return;
	}
	worker->actor = potop_actor_spawn(potop, WORKER_ACTOR_TYPE, worker->x, worker->z, worker->yaw);
	if (worker->actor == 0)
	{
		return;
	}
	worker->travel_seconds = 0.0f;
	worker->travel_tries = 0;
	worker->at_door = false;
	// (just out of his door: straight out first, then on his way)
	if (worker->from_door && worker->route_index == 0 && worker->route_count >= worker->exit_count && worker->exit_count > 0)
	{
		float points[2][2];

		for (int k = 0; k < worker->exit_count && k < 2; k++)
		{
			points[k][0] = worker->route[k][0];
			points[k][1] = worker->route[k][1];
		}
		worker->route_count = 0;
		potop_actor_walk_straight(potop, worker->actor, (const float (*)[2])points, SDL_min(worker->exit_count, 2));
		return;
	}
	worker->from_door = false;
	worker->route_count = 0;
	if (life_place_point(potop, worker, worker->place, &x, &z, &yaw))
	{
		travel_point(potop, worker, x, z, yaw, &x, &z);
		if (!potop_actor_go_to(potop, worker->actor, x, z))
		{
			worker->travel_tries++;
		}
	}
}

// Nearest of these places to (x, z) within radius, not one of those
// taken (0: none)
static uint32_t nearest_with_spot(potop_t *potop, float x, float z, float radius, const char *spot_name, const game_t *game, bool not_a_home)
{
	static uint32_t ids[4096];
	int count = potop_objects_with_spot(potop, x, z, radius, spot_name, ids, 4096);
	uint32_t best = 0;
	float best_distance = radius * radius;

	for (int i = 0; i < count; i++)
	{
		potop_object_info_t info;
		bool taken = false;
		float d;

		for (int w = 0; not_a_home && w < game->worker_count; w++)
		{
			taken = taken || game->workers[w].home == ids[i];
		}
		// (not a smithy: a smith does not live in another's)
		taken = taken || (not_a_home && potop_object_spot_find(potop, ids[i], "smith_anvil", 0) >= 0);
		if (taken || !potop_object_get(potop, ids[i], &info))
		{
			continue;
		}
		d = distance_squared_2d(info.x, info.z, x, z);
		if (d < best_distance)
		{
			best_distance = d;
			best = ids[i];
		}
	}
	return best;
}

// A smith for every smithy on the map that has none: his home (the
// nearest free dwelling), his inn, his day; he is where his day wants him
// at this hour. Those whose smithy is gone: taken off
static void scan(game_t *game, potop_t *potop, const potop_camera_info_t *camera)
{
	static uint32_t ids[1024];
	int count = potop_objects_with_spot(potop, camera->walker_x, camera->walker_z, WORKER_MAP_METERS, "smith_anvil", ids, 1024);
	float hour = potop_day_hour(potop);

	for (int i = 0; i < count && game->worker_count < GAME_WORKERS_MAX; i++)
	{
		potop_object_info_t smithy;
		worker_mind_t *worker;
		life_place_t place;

		if (find_worker(game, ids[i]) >= 0 || !potop_object_get(potop, ids[i], &smithy))
		{
			continue;
		}
		worker = &game->workers[game->worker_count];
		SDL_zerop(worker);
		worker->building = ids[i];
		// (his dice: the smithy's id (kept in the map's file) alone would
		// give him the same day at every start)
		worker->random_state = ids[i] * 2654435761u + 12345u + (uint32_t)SDL_GetPerformanceCounter();
		worker->home = nearest_with_spot(potop, smithy.x, smithy.z, WORKER_HOME_METERS, "door", game, true);
		worker->inn = nearest_with_spot(potop, smithy.x, smithy.z, WORKER_INN_METERS, "inn_door", game, false);
		game->worker_count++;
		// (the fire banked from the night, nothing in hand)
		worker->shop.fire = random_between(worker, 0.3f, 0.7f);
		worker->shop.break_after = random_between(worker, FIRST_BREAK_SECONDS, FIRST_BREAK_SECONDS * 2.0f);
		draw_day(worker);
		worker->last_hour = hour;
		place = wanted_place(worker, hour);
		worker->place = place;
		worker->life = place == LIFE_AT_WORK ? LIFE_WORK : LIFE_INSIDE;
		if (!life_place_point(potop, worker, place, &worker->x, &worker->z, &worker->yaw))
		{
			worker->x = smithy.x;
			worker->z = smithy.z;
		}
		if (SDL_getenv("POTOP_DEBUG_WORKERS") != NULL)
		{
			SDL_Log("Worker of smithy %u: home %u, inn %u; today off to work at %.2f h, lunch %.2f-%.2f h%s, done at %.2f h, %s", (unsigned)ids[i],
				(unsigned)worker->home, (unsigned)worker->inn, (double)worker->hours.work_start, (double)worker->hours.lunch_start,
				(double)worker->hours.lunch_end, worker->hours.lunch_at_inn ? " at the inn" : "", (double)worker->hours.work_end,
				worker->hours.skips_inn ? "no inn tonight" : "the inn till late");
		}
	}
	for (int w = game->worker_count - 1; w >= 0; w--)
	{
		potop_object_info_t building;

		if (!potop_object_get(potop, game->workers[w].building, &building))
		{
			take_off(game, potop, w);
		}
	}
}

// The work going on by itself as he does it: the fire burning down (or
// blown up), the iron heating in it or cooling out of it, the hammering it
// still wants
static void work_goes_on(worker_mind_t *worker, const worker_step_t *step, bool doing, float seconds)
{
	workshop_t *shop = &worker->shop;
	bool blowing = doing && SDL_strcmp(step->action, "smith_bellows") == 0;
	bool hammering = doing && (SDL_strcmp(step->action, "smith_hammer") == 0 || SDL_strcmp(step->action, "smith_hammer_light") == 0);

	shop->fire = SDL_clamp(shop->fire + (blowing ? FIRE_BLOWN_PER_SECOND : -FIRE_DIES_PER_SECOND) * seconds, 0.05f, 1.0f);
	if (shop->has_piece && shop->in_fire)
	{
		shop->heat = SDL_min(shop->heat + HEAT_PER_SECOND * shop->fire * seconds, 1.0f);
	}
	else if (shop->has_piece)
	{
		shop->heat = SDL_max(shop->heat - HEAT_COOLS_PER_SECOND * seconds, 0.0f);
	}
	if (hammering)
	{
		shop->piece_left -= seconds;
	}
	if (doing && SDL_strncmp(step->action, "smith_", 6) == 0)
	{
		shop->worked_seconds += seconds;
	}
}

// What a step does to the work as it begins: the iron into the fire or out
// of it onto the anvil, a shovel of coal on the fire, the piece quenched
static void step_starts(worker_mind_t *worker, const worker_step_t *step)
{
	workshop_t *shop = &worker->shop;

	if (SDL_strcmp(step->action, "smith_forge") == 0 && shop->has_piece)
	{
		shop->in_fire = true;
	}
	else if (SDL_strncmp(step->action, "smith_hammer", 12) == 0 || SDL_strcmp(step->action, "smith_inspect") == 0)
	{
		shop->in_fire = false;
	}
	else if (SDL_strcmp(step->action, "smith_coal") == 0)
	{
		shop->fire = SDL_min(shop->fire + FIRE_COAL, 1.0f);
	}
	else if (SDL_strcmp(step->action, "smith_quench") == 0)
	{
		shop->in_fire = false;
		shop->heat = 0.0f;
	}
}

// Along his way with no actor: walked by itself (straight on to its end
// if no way was found), there at its end
static void travel_by_itself(potop_t *potop, worker_mind_t *worker, float seconds)
{
	float left = WORKER_WALK_METERS_PER_SECOND * seconds;
	float goal_x;
	float goal_z;
	float yaw;

	if (!life_place_point(potop, worker, worker->place, &goal_x, &goal_z, &yaw))
	{
		arrive(potop, worker);
		return;
	}
	while (left > 0.0f)
	{
		bool last = worker->route_index >= worker->route_count;
		float to_x = last ? goal_x : worker->route[worker->route_index][0];
		float to_z = last ? goal_z : worker->route[worker->route_index][1];
		float dx = to_x - worker->x;
		float dz = to_z - worker->z;
		float d = SDL_sqrtf(dx * dx + dz * dz);

		if (d > 0.001f)
		{
			worker->yaw = SDL_atan2f(dx, dz);
		}
		if (d > left)
		{
			worker->x += dx / d * left;
			worker->z += dz / d * left;
			return;
		}
		worker->x = to_x;
		worker->z = to_z;
		left -= d;
		if (last)
		{
			arrive(potop, worker);
			return;
		}
		worker->route_index++;
	}
}

// Up to a door straight, and up its stairs if it has them (the sill), then
// in: from before it at once; from beside it (he came round the house) by
// a point straight before it first, not across the stairs
static void door_walk(potop_t *potop, worker_mind_t *worker, const potop_actor_info_t *actor, float x, float z, float yaw)
{
	float points[3][2] = { { x - SDL_sinf(yaw) * WORKER_DOOR_FRONT_METERS, z - SDL_cosf(yaw) * WORKER_DOOR_FRONT_METERS }, { x, z }, { x, z } };
	int first = before_door(actor->x, actor->z, x, z, yaw) ? 1 : 0;
	int count = 2;

	if (life_place_sill(potop, worker, worker->place, &points[2][0], &points[2][1]))
	{
		count = 3;
		// (on the stairs' foot already: straight up)
		if (distance_squared_2d(actor->x, actor->z, x, z) < 0.09f)
		{
			first = 2;
		}
	}
	worker->at_door = true;
	worker->travel_seconds = 0.0f;
	potop_actor_walk_straight_exact(potop, worker->actor, (const float (*)[2])(points + first), count - first, yaw);
}

// Along his way with his actor: it finds its way there; at a door, onto
// its spot facing it, then in; stopped short, sent again, and if that
// does not take him there either, put there
static void travel_seen(potop_t *potop, worker_mind_t *worker, const potop_actor_info_t *actor)
{
	float x;
	float z;
	float yaw;

	if (!life_place_point(potop, worker, worker->place, &x, &z, &yaw))
	{
		arrive(potop, worker);
		return;
	}
	if (worker->at_door)
	{
		if ((!actor->moving && worker->travel_seconds > 0.5f) || worker->travel_seconds > WORKER_DOOR_SECONDS)
		{
			arrive(potop, worker);
		}
		return;
	}
	// (coming up to a door from before it: straight on up to it, no stop)
	if (actor->moving && !worker->from_door && worker->place != LIFE_AT_WORK && worker->travel_seconds > 0.5f
		&& distance_squared_2d(actor->x, actor->z, x, z) < WORKER_DOOR_SWITCH_METERS * WORKER_DOOR_SWITCH_METERS && before_door(actor->x, actor->z, x, z, yaw))
	{
		door_walk(potop, worker, actor, x, z, yaw);
		return;
	}
	if (actor->moving || worker->travel_seconds < 0.5f)
	{
		return;
	}
	// (out of his door and the steps before it: now on his way)
	if (worker->from_door)
	{
		float to_x;
		float to_z;

		worker->from_door = false;
		worker->travel_seconds = 0.0f;
		travel_point(potop, worker, x, z, yaw, &to_x, &to_z);
		if (!potop_actor_go_to(potop, worker->actor, to_x, to_z))
		{
			worker->travel_tries++;
		}
		return;
	}
	if (worker->place == LIFE_AT_WORK && distance_squared_2d(actor->x, actor->z, x, z) < WORKER_ARRIVED_METERS * WORKER_ARRIVED_METERS)
	{
		arrive(potop, worker);
		return;
	}
	if (worker->place != LIFE_AT_WORK && distance_squared_2d(actor->x, actor->z, x, z) < WORKER_DOOR_NEAR_METERS * WORKER_DOOR_NEAR_METERS)
	{
		door_walk(potop, worker, actor, x, z, yaw);
		return;
	}
	if (SDL_getenv("POTOP_DEBUG_WORKERS") != NULL)
	{
		SDL_Log("Worker of %u: stopped at (%.1f, %.1f), %.1f m short of (%.1f, %.1f)", (unsigned)worker->building, (double)actor->x, (double)actor->z,
			(double)SDL_sqrtf(distance_squared_2d(actor->x, actor->z, x, z)), (double)x, (double)z);
	}
	if (worker->travel_tries < 3)
	{
		float to_x;
		float to_z;

		worker->travel_tries++;
		worker->travel_seconds = 0.0f;
		travel_point(potop, worker, x, z, yaw, &to_x, &to_z);
		if (!potop_actor_go_to(potop, worker->actor, to_x, to_z))
		{
			worker->travel_seconds = -2.0f;
		}
		return;
	}
	if (SDL_getenv("POTOP_DEBUG_WORKERS") != NULL)
	{
		SDL_Log("Worker of %u: found no way there, put there", (unsigned)worker->building);
	}
	hide(potop, worker);
	arrive(potop, worker);
}

// His work's round, with his actor: walking to a step's place, turning
// onto it, doing it
static void work_seen(potop_t *potop, worker_mind_t *worker, const potop_actor_info_t *actor, float seconds)
{
	worker_step_t *step = &worker->steps[worker->step_index];
	float x = 0.0f;
	float z = 0.0f;
	float yaw = 0.0f;
	bool has_place = step_place(potop, worker, step, &x, &z, &yaw);

	worker->seconds += seconds;
	work_goes_on(worker, step, worker->phase == WORKER_WORKING, seconds);
	switch (worker->phase)
	{
	case WORKER_WALKING:
		if (!actor->moving)
		{
			worker->phase = WORKER_FACING;
			worker->seconds = 0.0f;
		}
		break;
	case WORKER_FACING:
		// (the engine turns him there, or steps him onto it: done when
		// he stands on it facing its way; stopped short: once more)
		if (has_place && worker->seconds > WORKER_FACE_SECONDS && distance_squared_2d(actor->x, actor->z, x, z) > WORKER_THERE_METERS * WORKER_THERE_METERS
			&& worker->tries < 2)
		{
			worker->tries++;
			go_to(potop, worker, x, z, yaw, step->place == PLACE_POINT);
			worker->phase = WORKER_WALKING;
			break;
		}
		if (!has_place
			|| (SDL_fabsf(wrap_angle(actor->yaw_radians - yaw)) < WORKER_FACE_RADIANS
				&& distance_squared_2d(actor->x, actor->z, x, z) < WORKER_ON_SPOT_METERS * WORKER_ON_SPOT_METERS)
			|| worker->seconds > WORKER_FACE_SECONDS)
		{
			if (SDL_getenv("POTOP_DEBUG_WORKERS") != NULL)
			{
				SDL_Log("Worker %u at %.1f s: %s for %.1f s%s%s (fire %.2f, iron %.2f%s, %.1f s of work left)", (unsigned)worker->actor, potop_elapsed_seconds(potop), step->action, (double)step->seconds,
					step->place == PLACE_SPOT ? " at " : step->place == PLACE_POINT ? " out front" : "", step->place == PLACE_SPOT ? step->spot : "",
					(double)worker->shop.fire, (double)worker->shop.heat, worker->shop.in_fire ? " in the fire" : "", (double)worker->shop.piece_left);
			}
			potop_actor_play(potop, worker->actor, step->action);
			step_starts(worker, step);
			if (step->state)
			{
				potop_object_set_state(potop, worker->building, "pump_down");
			}
			worker->phase = WORKER_WORKING;
			worker->seconds = 0.0f;
		}
		break;
	case WORKER_WORKING:
		// (due off while idling a long while (arms crossed, out front): cut
		// short; the work's own steps are done to their end)
		if (worker->seconds >= step->seconds
			|| (worker->leaving == 1 && SDL_strncmp(step->action, "smith_", 6) != 0 && worker->seconds > WORKER_IDLE_CUT_SECONDS))
		{
			end_step(potop, worker);
		}
		break;
	}
}

// One worker's frame: a new day's hours drawn; off when his day wants him
// elsewhere; his actor put down near the eye and taken away far from it;
// on with his work or his way
static void update_worker(potop_t *potop, worker_mind_t *worker, const potop_camera_info_t *camera, float hour, float seconds)
{
	life_place_t wanted;
	potop_actor_info_t actor;
	float eye;

	if (hour < worker->last_hour - 6.0f)
	{
		worker->day++;
		draw_day(worker);
	}
	worker->last_hour = hour;
	wanted = wanted_place(worker, hour);
	// (where he stands: his actor's place, or his own)
	if (worker->actor != 0)
	{
		if (!potop_actor_get(potop, worker->actor, &actor))
		{
			worker->actor = 0;
		}
		else
		{
			worker->x = actor.x;
			worker->z = actor.z;
			worker->yaw = actor.yaw_radians;
		}
	}
	eye = distance_squared_2d(worker->x, worker->z, camera->walker_x, camera->walker_z);
	if (worker->actor != 0 && eye > WORKER_HIDE_METERS * WORKER_HIDE_METERS)
	{
		hide(potop, worker);
		if (worker->life == LIFE_TRAVEL)
		{
			float x;
			float z;
			float yaw;

			worker->route_index = 0;
			worker->route_count = life_place_point(potop, worker, worker->place, &x, &z, &yaw)
				? potop_find_path(potop, WORKER_ACTOR_TYPE, worker->x, worker->z, x, z, worker->route, WORKER_ROUTE_MAX, NULL) : 0;
		}
	}
	else if (worker->actor == 0 && worker->life != LIFE_INSIDE && eye < WORKER_SHOW_METERS * WORKER_SHOW_METERS)
	{
		show(potop, worker);
	}
	switch (worker->life)
	{
	case LIFE_INSIDE:
		if (wanted != worker->place)
		{
			start_travel(potop, worker, wanted);
		}
		break;
	case LIFE_WORK:
		if (wanted != LIFE_AT_WORK)
		{
			worker->place = wanted;
			// (with no actor: off at once; with one, at the next pause in
			// the work)
			if (worker->actor == 0 || worker->step_count == 0)
			{
				start_travel(potop, worker, wanted);
				break;
			}
			if (worker->leaving == 0)
			{
				worker->leaving = 1;
				log_life(potop, worker, "winding up, then off to");
			}
		}
		if (worker->actor != 0 && worker->step_count > 0 && potop_actor_get(potop, worker->actor, &actor))
		{
			work_seen(potop, worker, &actor, seconds);
		}
		break;
	case LIFE_TRAVEL:
		// (his day changed its mind on the way: the inn after all, home)
		if (wanted != worker->place)
		{
			start_travel(potop, worker, wanted);
			break;
		}
		worker->travel_seconds += seconds;
		if (worker->actor != 0 && potop_actor_get(potop, worker->actor, &actor))
		{
			travel_seen(potop, worker, &actor);
		}
		else
		{
			travel_by_itself(potop, worker, seconds);
		}
		break;
	}
}

void game_update_workers(game_t *game, potop_t *potop, float seconds)
{
	potop_camera_info_t camera;
	float hour = potop_day_hour(potop);

	potop_camera_get(potop, &camera);
	// (the smithies looked for among all the objects when the list was
	// sorted again (a map loaded, the editor), not on a clock: a pass over
	// the whole map; those whose smithy is gone, every few seconds)
	if (!game->worker_scanned || game->worker_scan_generation != potop_objects_generation(potop))
	{
		game->worker_scanned = true;
		game->worker_scan_generation = potop_objects_generation(potop);
		scan(game, potop, &camera);
	}
	game->worker_scan_seconds -= seconds;
	if (game->worker_scan_seconds <= 0.0f)
	{
		game->worker_scan_seconds = WORKER_SCAN_SECONDS;
		for (int w = game->worker_count - 1; w >= 0; w--)
		{
			potop_object_info_t building;

			if (!potop_object_get(potop, game->workers[w].building, &building))
			{
				take_off(game, potop, w);
			}
		}
	}
	for (int w = 0; w < game->worker_count; w++)
	{
		update_worker(potop, &game->workers[w], &camera, hour, seconds);
	}
}

void game_workers_map_loaded(game_t *game, potop_t *potop)
{
	(void)potop;
	// (their actors went with the map)
	game->worker_count = 0;
	game->worker_scan_seconds = 0.0f;
	game->worker_scanned = false;
}
