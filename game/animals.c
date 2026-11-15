// The farm animals' minds (game.h): the creatures that live by a
// building's spots (potop_spot_t, written by the house generator), one mind
// for all of them, a kind table for what tells them apart. The building
// says where things are (its yard, where to eat, the way in, where to
// sleep), the mind says what to do there, and they walk straight
// (potop_actor_walk_straight: no search for a path; what is in the way
// stops them and they go elsewhere).
//
// By day they keep to their home's yard: a short walk, a bite at a feeder
// or off the ground, a stretch, a stand. At dusk they go in by the
// building's way (its chain of spots): a hen up its ramp and in at its
// hole (taken off the map: inside it is not drawn), a cow by the byre's
// open door to a stall, where it lies down and sleeps; some cows sleep out
// in the yard instead. In the morning the hens come out one by one, the
// cows get up and the ones inside walk out. Far from the eye a home's
// animals are taken off the map, and put back when it comes near.
//
// Sheep keep to their stana as a flock: the home has a grazing place on
// its pasture (the "yard" spot), moved on now and then, and each walks to
// somewhere round it; at dusk they go in by the strunga to the fold and
// lie down in it, each at its own place (the kind's sleep area).
//
// One with no home (game_add_free_animal) keeps round a point of the land
// the same way, and sleeps where it stands.

#include "game.h"

#include <SDL3/SDL.h>

#define ANIMAL_CHAIN_MAX 3
#define ANIMAL_FEEDERS_MAX 3
#define OBJECT_SPOTS_SEEN 32

// What tells one kind from another
typedef struct
{
	const char *actor_type;        // actors.ini
	const char *home_prefix;       // its home's object type name begins so
	int per_home;
	bool hatch_in_the_morning;     // off the map at night, out one by one at dawn (else they stay, asleep)
	// Eating: at its home's spots of these names (walked to that share of
	// the time), or off the ground after a walk (that share)
	const char *feeders[ANIMAL_FEEDERS_MAX];
	float feeder_share;
	float ground_eat_share;
	float eat_seconds;             // its eat clip
	// A stretch now and then standing (an action; NULL: none)
	const char *stretch_action;
	float stretch_seconds;
	float stretch_share;
	// The way in: the spots walked in turn (the first the nearest of its
	// name, each next the linked one of the next name), then: gone in
	// (off the map) if vanish_at_end, else to one of the sleep spots
	const char *chain[ANIMAL_CHAIN_MAX];
	int chain_length;
	bool vanish_at_end;
	const char *sleep_spot;        // inside, one each (NULL: none)
	const char *sleep_area;        // else an area spot they lie down anywhere in (a fold; NULL: none)
	float inside_share;            // of them sleep inside each night (the rest out in the yard)
	// Lying down, asleep, getting up (NULL: it just stands)
	const char *lie_down_action;
	float lie_down_seconds;
	const char *sleep_action;
	const char *get_up_action;
	float get_up_seconds;
	// Standing between walks (least, most)
	float stand_least;
	float stand_most;
	// Bites in a row (most; at least one): at a feeder, off the ground
	int feeder_bites_most;
	int ground_bites_most;
	// A flock (> 0): they graze within this of their home's grazing place,
	// moved on every so often (least, most seconds)
	float flock_radius;
	float flock_move_least;
	float flock_move_most;
	// Gone in: this near its last way point, or stopped this near it
	float in_meters;
	float in_near_meters;
	// Spread over this many seconds: in at dusk, up at dawn (one by one,
	// not the whole flock in one door at once)
	float dusk_spread;
	float dawn_spread;
	// Its boredom walk (wander.c) in place of a walk to a point of its
	// ground; NULL: to a point (point_ahead)
	const wander_style_t *wander;
} animal_kind_t;

// A cow's boredom walk: a meter past her nose, now and then (once a second
// or so) ten degrees aside, at most of her walk
static const wander_style_t COW_WANDER = {
	.look_meters = 1.0f,
	.drift_chance = 0.1f,
	.drift_degrees = 10.0f,
	.speed_share = 0.8f,
	.back_seconds = 0.8f,
	.back_share = 0.35f,
	.give_up_seconds = 1.0f,
	.backs_most = 2,
	.least_seconds = 8.0f,
	.most_seconds = 25.0f,
};

static const animal_kind_t ANIMAL_KINDS[] = {
	{
		.actor_type = "hen",
		.home_prefix = "hen_house",
		.per_home = 7,
		.hatch_in_the_morning = true,
		.feeders = { "peck", "peck", "drink" },
		.feeder_share = 0.35f,
		.ground_eat_share = 0.4f,
		.eat_seconds = 2.2f,
		.chain = { "hen_ramp_foot", "hen_hole" },
		.chain_length = 2,
		.vanish_at_end = true,
		.stand_least = 0.8f,
		.stand_most = 4.0f,
		.feeder_bites_most = 3,
		.ground_bites_most = 1,
		.in_meters = 0.5f,
		.in_near_meters = 1.5f,
	},
	{
		.actor_type = "cow",
		.home_prefix = "cow_farm",
		.per_home = 6,
		.hatch_in_the_morning = false,
		.feeders = { "hay", "hay", "trough" },
		.feeder_share = 0.25f,
		.ground_eat_share = 0.2f,
		.eat_seconds = 2.33f,
		.stretch_action = "idle_break",
		.stretch_seconds = 4.27f,
		.stretch_share = 0.08f,
		.chain = { "byre_door_out", "byre_door_in" },
		.chain_length = 2,
		.vanish_at_end = false,
		.sleep_spot = "byre_stall",
		.inside_share = 0.5f,
		.lie_down_action = "lie_down",
		.lie_down_seconds = 2.67f,
		.sleep_action = "sleep_idle",
		.get_up_action = "get_up",
		.get_up_seconds = 2.67f,
		.stand_least = 0.8f,
		.stand_most = 3.0f,
		.feeder_bites_most = 2,
		.ground_bites_most = 1,
		.in_meters = 0.7f,
		.in_near_meters = 2.0f,
		.dusk_spread = 30.0f,
		.dawn_spread = 40.0f,
		.wander = &COW_WANDER,
	},
	{
		// (a pigsty's: two pens, each its door into the shed; rooting about
		// the pen, at the trough, in the shed by night)
		.actor_type = "pig",
		.home_prefix = "pigsty",
		.per_home = 4,
		.hatch_in_the_morning = false,
		.feeders = { "trough", "trough", "trough" },
		.feeder_share = 0.3f,
		.ground_eat_share = 0.35f,
		.eat_seconds = 2.4f,
		.stretch_action = "idle_break",
		.stretch_seconds = 4.0f,
		.stretch_share = 0.08f,
		.chain = { "pig_door_out", "pig_door_in" },
		.chain_length = 2,
		.vanish_at_end = false,
		.sleep_area = "sty",
		.inside_share = 1.0f,
		.lie_down_action = "lie_down",
		.lie_down_seconds = 2.67f,
		.sleep_action = "sleep_idle",
		.get_up_action = "get_up",
		.get_up_seconds = 2.67f,
		.stand_least = 0.8f,
		.stand_most = 3.5f,
		.feeder_bites_most = 3,
		.ground_bites_most = 2,
		.in_meters = 0.5f,
		// (stopped this near its place: in, the other pig lying in the way)
		.in_near_meters = 1.7f,
		.dusk_spread = 30.0f,
		.dawn_spread = 40.0f,
	},
	{
		// (a stana's: grazing its pasture as a flock, in the fold by night)
		.actor_type = "ram",
		.home_prefix = "stana",
		.per_home = 12,
		.hatch_in_the_morning = false,
		.ground_eat_share = 0.65f,
		.eat_seconds = 2.4f,
		.stretch_action = "idle_break",
		.stretch_seconds = 4.0f,
		.stretch_share = 0.06f,
		.chain = { "fold_gate_out", "fold_gate_in" },
		.chain_length = 2,
		.vanish_at_end = false,
		.sleep_area = "fold",
		.inside_share = 1.0f,
		.lie_down_action = "lie_down",
		.lie_down_seconds = 2.67f,
		.sleep_action = "sleep_idle",
		.get_up_action = "get_up",
		.get_up_seconds = 2.67f,
		.stand_least = 0.6f,
		.stand_most = 2.5f,
		.ground_bites_most = 4,
		.flock_radius = 5.0f,
		.flock_move_least = 25.0f,
		.flock_move_most = 70.0f,
		.in_meters = 0.6f,
		.in_near_meters = 1.5f,
		.dusk_spread = 90.0f,
		.dawn_spread = 120.0f,
	},
};
static const int ANIMAL_KIND_COUNT = (int)(sizeof(ANIMAL_KINDS) / sizeof(ANIMAL_KINDS[0]));

// Homes looked for this far round the eye, this often; past the second,
// their animals are taken off the map
static const float HOME_NEAR_METERS = 120.0f;
static const float HOME_FAR_METERS = 160.0f;
static const float SCAN_SECONDS = 1.0f;
// Out one by one (least, most seconds between two)
static const float HATCH_SECONDS_LEAST = 1.5f;
static const float HATCH_SECONDS_MOST = 5.0f;
// Daylight at the home (potop_light_info_t.daylight) under which they go
// in, over which they come out
static const float DUSK_DAYLIGHT = 0.25f;
static const float DAWN_DAYLIGHT = 0.35f;
static const float THINK_SECONDS = 0.3f;
// Going in so long (detours and all) without getting there: in anyway (or
// down where it is)
static const float GOING_IN_SECONDS = 90.0f;
// Killed: it lies this long, then is taken off the map; its home brings
// another out only so long after
static const float CORPSE_SECONDS = 300.0f;
static const float REPLACE_SECONDS = 180.0f;

const char *game_animal_state_name(animal_state_t state)
{
	static const char *const names[ANIMAL_STATE_COUNT] = { "roam", "busy", "going_in", "going_down", "asleep", "getting_up", "going_out",
		"dead" };

	return (int)state >= 0 && (int)state < (int)ANIMAL_STATE_COUNT ? names[state] : "?";
}

const char *game_animal_kind_name(int kind)
{
	return kind >= 0 && kind < (int)(sizeof(ANIMAL_KINDS) / sizeof(ANIMAL_KINDS[0])) ? ANIMAL_KINDS[kind].actor_type : "?";
}

static float random_unit(game_t *game)
{
	uint32_t value = game->animal_random != 0 ? game->animal_random : 0x6c8e9cf5u;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	game->animal_random = value;
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

static uint32_t home_object(const game_t *game, const animal_mind_t *animal)
{
	return animal->ground.home >= 0 ? game->animal_homes[animal->ground.home].object_id : 0;
}

// The nth of a home's spots of a name (the first if it has fewer; -1:
// none)
static int nth_spot(potop_t *potop, uint32_t home, const char *name, int n)
{
	int first = potop_object_spot_find(potop, home, name, 0);
	int found = first;

	for (int k = 0; k < n && found >= 0; k++)
	{
		found = potop_object_spot_find(potop, home, name, found + 1);
	}
	return found >= 0 ? found : first;
}

// How many spots of a name a home has
static int spot_count(potop_t *potop, uint32_t home, const char *name)
{
	int count = 0;

	for (int k = potop_object_spot_find(potop, home, name, 0); k >= 0; k = potop_object_spot_find(potop, home, name, k + 1))
	{
		count++;
	}
	return count;
}

// A point at random in a home's area spot of a name, the nth of them (a
// pen's; its own turned x and z); false if it has none
static bool point_in_area(game_t *game, potop_t *potop, uint32_t home, const char *name, int n, float *x_out, float *z_out)
{
	int area_spot = nth_spot(potop, home, name, n);
	potop_spot_t area;
	float u;
	float v;
	float c;
	float s;

	if (area_spot < 0 || !potop_object_spot_get(potop, home, area_spot, &area))
	{
		return false;
	}
	u = random_between(game, -area.half_width, area.half_width);
	v = random_between(game, -area.half_depth, area.half_depth);
	c = SDL_cosf(area.yaw_radians);
	s = SDL_sinf(area.yaw_radians);
	// (x turned round Y by yaw: x along (cos, -sin), z along (sin, cos))
	*x_out = area.x + u * c + v * s;
	*z_out = area.z - u * s + v * c;
	return true;
}

// Whether (x, z) is in a home's area spot of a name, the nth of them
static bool inside_area(potop_t *potop, uint32_t home, const char *name, int n, float x, float z)
{
	int area_spot = nth_spot(potop, home, name, n);
	potop_spot_t area;
	float dx;
	float dz;
	float u;
	float v;

	if (area_spot < 0 || !potop_object_spot_get(potop, home, area_spot, &area))
	{
		return false;
	}
	dx = x - area.x;
	dz = z - area.z;
	// (its own x along (cos, -sin), z along (sin, cos))
	u = dx * SDL_cosf(area.yaw_radians) - dz * SDL_sinf(area.yaw_radians);
	v = dx * SDL_sinf(area.yaw_radians) + dz * SDL_cosf(area.yaw_radians);
	return SDL_fabsf(u) <= area.half_width && SDL_fabsf(v) <= area.half_depth;
}

// A point of its ground at random: in its home's yard (that spot's area,
// its own turned x and z), round its flock's grazing place, or round its
// point
static bool point_on_ground(game_t *game, potop_t *potop, const animal_mind_t *animal, float *x_out, float *z_out)
{
	const animal_kind_t *kind = &ANIMAL_KINDS[animal->kind];

	if (animal->ground.home >= 0 && kind->flock_radius > 0.0f)
	{
		animal_home_t *home = &game->animal_homes[animal->ground.home];
		float angle = random_between(game, 0.0f, 6.2831853f);
		float reach = kind->flock_radius * SDL_sqrtf(random_unit(game));

		if (!home->graze_set && !point_in_area(game, potop, home->object_id, "yard", 0, &home->graze_x, &home->graze_z))
		{
			return false;
		}
		home->graze_set = true;
		*x_out = home->graze_x + SDL_cosf(angle) * reach;
		*z_out = home->graze_z + SDL_sinf(angle) * reach;
		return true;
	}
	if (animal->ground.home < 0)
	{
		float angle = random_between(game, 0.0f, 6.2831853f);
		float reach = animal->ground.radius * SDL_sqrtf(random_unit(game));

		*x_out = animal->ground.x + SDL_cosf(angle) * reach;
		*z_out = animal->ground.z + SDL_sinf(angle) * reach;
		return true;
	}
	return point_in_area(game, potop, home_object(game, animal), "yard", animal->pen, x_out, z_out);
}

// Whether (x, z) is on its ground: its home's yard (its pen's), within
// its flock's reach of the grazing place, or round its point
static bool on_ground(game_t *game, potop_t *potop, const animal_mind_t *animal, float x, float z)
{
	const animal_kind_t *kind = &ANIMAL_KINDS[animal->kind];

	if (animal->ground.home >= 0 && kind->flock_radius > 0.0f)
	{
		const animal_home_t *home = &game->animal_homes[animal->ground.home];

		// (and in the yard: round a grazing place by its fence, not over it)
		return home->graze_set && distance_squared_2d(x, z, home->graze_x, home->graze_z) <= kind->flock_radius * kind->flock_radius
			&& inside_area(potop, home->object_id, "yard", 0, x, z);
	}
	if (animal->ground.home < 0)
	{
		return distance_squared_2d(x, z, animal->ground.x, animal->ground.z) <= animal->ground.radius * animal->ground.radius;
	}
	return inside_area(potop, home_object(game, animal), "yard", animal->pen, x, z);
}

enum
{
	AHEAD_NEIGHBORS_MAX = 64,
	AHEAD_TRIES_PER_CONE = 8
};

static const float AHEAD_ROOM_METERS = 2.5f;

// A point of its ground ahead of it: a few meters on, no more than
// AHEAD_CONES_DEGREES[0] off the way it faces (a cow does not turn round
// on the spot to walk off the other way); away from the others where it
// can. At a fence (nothing ahead on its ground) the cone wider, a step at
// a time, the least turn that finds ground; none at all (off its ground,
// its flock gone on): anywhere on it, as before
static bool point_ahead(game_t *game, potop_t *potop, const animal_mind_t *animal, const potop_actor_info_t *actor, float *x_out, float *z_out)
{
	static const float AHEAD_CONES_DEGREES[] = { 20.0f, 45.0f, 90.0f, 180.0f };
	float neighbors[AHEAD_NEIGHBORS_MAX][2];
	int neighbor_count = 0;

	// (a flock's grazing place found first: on_ground needs it)
	if (animal->ground.home >= 0 && ANIMAL_KINDS[animal->kind].flock_radius > 0.0f
		&& !game->animal_homes[animal->ground.home].graze_set)
	{
		return point_on_ground(game, potop, animal, x_out, z_out);
	}
	for (int k = 0; k < game->animal_count && neighbor_count < AHEAD_NEIGHBORS_MAX; k++)
	{
		const animal_mind_t *other = &game->animals[k];
		potop_actor_info_t there;

		if (other == animal || other->ground.home != animal->ground.home || !potop_actor_get(potop, other->actor_id, &there) || there.dead)
		{
			continue;
		}
		// (where it is going, if it is: that is where it will be)
		neighbors[neighbor_count][0] = other->walking ? other->walk_x : there.x;
		neighbors[neighbor_count][1] = other->walking ? other->walk_z : there.z;
		neighbor_count++;
	}
	for (int cone = 0; cone < (int)SDL_arraysize(AHEAD_CONES_DEGREES); cone++)
	{
		float half_angle = AHEAD_CONES_DEGREES[cone] * 0.017453293f;
		float best_room = -1.0f;
		float best_x = 0.0f;
		float best_z = 0.0f;

		for (int t = 0; t < AHEAD_TRIES_PER_CONE; t++)
		{
			float yaw = actor->yaw_radians + random_between(game, -half_angle, half_angle);
			// (the last tries shorter: in a small yard a step or two on
			// before a turn round)
			float reach = t < AHEAD_TRIES_PER_CONE - 3 ? random_between(game, 2.0f, 6.0f) : random_between(game, 1.2f, 2.5f);
			float x = actor->x + SDL_sinf(yaw) * reach;
			float z = actor->z + SDL_cosf(yaw) * reach;
			// (room enough at AHEAD_ROOM_METERS from the nearest: any such
			// as good, not off to the far edge away from all of them)
			float room = AHEAD_ROOM_METERS * AHEAD_ROOM_METERS;

			if (!on_ground(game, potop, animal, x, z))
			{
				continue;
			}
			for (int n = 0; n < neighbor_count; n++)
			{
				room = SDL_min(room, distance_squared_2d(x, z, neighbors[n][0], neighbors[n][1]));
			}
			if (room > best_room)
			{
				best_room = room;
				best_x = x;
				best_z = z;
			}
		}
		if (best_room >= 0.0f)
		{
			char text[32];
			SDL_snprintf(text, sizeof(text), "ahead_%d", cone);
			game_trace_event(game, potop, animal->actor_id, text);
			*x_out = best_x;
			*z_out = best_z;
			return true;
		}
	}
	game_trace_event(game, potop, animal->actor_id, "ahead_none");
	return point_on_ground(game, potop, animal, x_out, z_out);
}

// Somewhere a few meters round where it stands (out of a crowd's way at a
// gate, the flock's own going round)
static void point_near(game_t *game, const potop_actor_info_t *actor, float *x_out, float *z_out)
{
	float angle = random_between(game, 0.0f, 6.2831853f);
	float reach = random_between(game, 1.5f, 3.5f);

	*x_out = actor->x + SDL_cosf(angle) * reach;
	*z_out = actor->z + SDL_sinf(angle) * reach;
}

// The home's spots of a name, one at random (-1: none)
static int random_spot(game_t *game, potop_t *potop, uint32_t home, const char *name)
{
	int found[OBJECT_SPOTS_SEEN];
	int count = 0;

	for (int k = potop_object_spot_find(potop, home, name, 0); k >= 0 && count < OBJECT_SPOTS_SEEN; k = potop_object_spot_find(potop, home, name, k + 1))
	{
		found[count++] = k;
	}
	return count > 0 ? found[(int)(random_unit(game) * (float)count) % count] : -1;
}

// The nearest of the home's spots of a name to (x, z) (-1: none)
static int nearest_spot(potop_t *potop, uint32_t home, const char *name, float x, float z)
{
	int best = -1;
	float best_squared = 1e30f;

	for (int k = potop_object_spot_find(potop, home, name, 0); k >= 0; k = potop_object_spot_find(potop, home, name, k + 1))
	{
		potop_spot_t spot;

		if (potop_object_spot_get(potop, home, k, &spot) && distance_squared_2d(spot.x, spot.z, x, z) < best_squared)
		{
			best_squared = distance_squared_2d(spot.x, spot.z, x, z);
			best = k;
		}
	}
	return best;
}

// The spot linked to one, of a name (-1: none)
static int linked_spot(potop_t *potop, uint32_t home, int from, const char *name)
{
	potop_spot_t spot;

	if (from < 0 || !potop_object_spot_get(potop, home, from, &spot))
	{
		return -1;
	}
	for (int l = 0; l < POTOP_SPOT_LINKS; l++)
	{
		potop_spot_t linked;

		if (spot.links[l] >= 0 && potop_object_spot_get(potop, home, spot.links[l], &linked) && SDL_strcmp(linked.name, name) == 0)
		{
			return spot.links[l];
		}
	}
	return -1;
}

// The kind's way in from where it is, as points (the chain, then its
// stall); how many (0: none)
static int way_in(game_t *game, potop_t *potop, animal_mind_t *animal, const potop_actor_info_t *actor, float (*points)[2])
{
	const animal_kind_t *kind = &ANIMAL_KINDS[animal->kind];
	uint32_t home = home_object(game, animal);
	int count = 0;
	int at = -1;

	if (animal->ground.home < 0 || kind->chain_length == 0)
	{
		return 0;
	}
	for (int c = 0; c < kind->chain_length; c++)
	{
		potop_spot_t spot;

		at = c == 0 ? nearest_spot(potop, home, kind->chain[0], actor->x, actor->z) : linked_spot(potop, home, at, kind->chain[c]);
		if (at < 0 || !potop_object_spot_get(potop, home, at, &spot))
		{
			return 0;
		}
		points[count][0] = spot.x;
		points[count][1] = spot.z;
		count++;
	}
	if (!kind->vanish_at_end && animal->has_sleep_point)
	{
		points[count][0] = animal->sleep_x;
		points[count][1] = animal->sleep_z;
		count++;
	}
	else if (!kind->vanish_at_end && animal->stall >= 0)
	{
		potop_spot_t stall;

		if (potop_object_spot_get(potop, home, animal->stall, &stall))
		{
			points[count][0] = stall.x;
			points[count][1] = stall.z;
			count++;
		}
	}
	return count;
}

static int home_of(game_t *game, potop_t *potop, uint32_t object_id)
{
	potop_object_info_t object;

	for (int i = 0; i < game->animal_home_count; i++)
	{
		if (game->animal_homes[i].object_id == object_id)
		{
			return i;
		}
	}
	if (game->animal_home_count == GAME_ANIMAL_HOMES_MAX || !potop_object_get(potop, object_id, &object)
		|| potop_object_spot_find(potop, object_id, "yard", 0) < 0)
	{
		return -1;
	}
	for (int k = 0; k < ANIMAL_KIND_COUNT; k++)
	{
		if (SDL_strncmp(object.type_name, ANIMAL_KINDS[k].home_prefix, SDL_strlen(ANIMAL_KINDS[k].home_prefix)) == 0)
		{
			animal_home_t *home = &game->animal_homes[game->animal_home_count];

			SDL_zerop(home);
			home->object_id = object_id;
			home->kind = k;
			return game->animal_home_count++;
		}
	}
	return -1;
}

static void forget(game_t *game, int index)
{
	game->animals[index] = game->animals[--game->animal_count];
}

// Off the map (gone in for the night, or the eye went away)
static void take_off(game_t *game, potop_t *potop, int index, bool for_the_night)
{
	animal_mind_t *animal = &game->animals[index];

	potop_actor_remove(potop, animal->actor_id);
	if (animal->ground.home >= 0)
	{
		animal_home_t *home = &game->animal_homes[animal->ground.home];

		home->out--;
		if (home->out == 0 && for_the_night)
		{
			SDL_Log("Animals: all of %s's %ss in for the night", ANIMAL_KINDS[home->kind].home_prefix, ANIMAL_KINDS[home->kind].actor_type);
		}
	}
	forget(game, index);
}

static animal_mind_t *add_mind(game_t *game, uint32_t actor, int kind)
{
	animal_mind_t *animal;

	if (game->animal_count == GAME_ANIMALS_MAX)
	{
		return NULL;
	}
	animal = &game->animals[game->animal_count++];
	SDL_zerop(animal);
	animal->actor_id = actor;
	animal->kind = kind;
	animal->ground.home = -1;
	animal->feeder = -1;
	animal->stall = -1;
	animal->state = ANIMAL_ROAM;
	animal->think_in = random_between(game, 0.0f, THINK_SECONDS);
	animal->wait_seconds = random_between(game, 0.0f, 1.0f);
	return animal;
}

// One more of a home's on the map: a hen down its ramp, a cow somewhere in
// the yard (asleep there if it is night)
static void bring_out(game_t *game, potop_t *potop, int home_index, float daylight)
{
	animal_home_t *home = &game->animal_homes[home_index];
	const animal_kind_t *kind = &ANIMAL_KINDS[home->kind];
	animal_mind_t probe;
	float x;
	float z;
	float yaw = random_between(game, 0.0f, 6.2831853f);
	uint32_t actor;
	animal_mind_t *animal;
	bool in_fold = false;

	SDL_zero(probe);
	probe.kind = home->kind;
	probe.ground.home = home_index;
	// (its pen: the home's pens in turn)
	probe.pen = home->out % SDL_max(spot_count(potop, home->object_id, "yard"), 1);
	if (kind->hatch_in_the_morning)
	{
		int foot = random_spot(game, potop, home->object_id, kind->chain[0]);
		potop_spot_t spot;

		if (foot < 0 || !potop_object_spot_get(potop, home->object_id, foot, &spot))
		{
			return;
		}
		x = spot.x;
		z = spot.z;
		yaw = spot.yaw_radians + 3.14159265f;
	}
	else if (daylight < DUSK_DAYLIGHT && kind->sleep_area != NULL && kind->sleep_action != NULL)
	{
		// (at night a flock's in its fold)
		if (!point_in_area(game, potop, home->object_id, kind->sleep_area, probe.pen, &x, &z))
		{
			return;
		}
		in_fold = true;
	}
	else if (!point_on_ground(game, potop, &probe, &x, &z))
	{
		return;
	}
	actor = potop_actor_spawn(potop, kind->actor_type, x, z, yaw);
	if (actor == 0)
	{
		return;
	}
	animal = add_mind(game, actor, home->kind);
	if (animal == NULL)
	{
		potop_actor_remove(potop, actor);
		return;
	}
	animal->ground.home = home_index;
	animal->pen = probe.pen;
	home->out++;
	// (brought out at night, one that stays out: down in the yard; a
	// flock's in its fold, out by the strunga in the morning)
	if (!kind->hatch_in_the_morning && daylight < DUSK_DAYLIGHT && kind->sleep_action != NULL)
	{
		if (in_fold)
		{
			animal->sleep_x = x;
			animal->sleep_z = z;
			animal->has_sleep_point = true;
			animal->sleeps_inside = true;
		}
		potop_actor_play(potop, actor, kind->sleep_action);
		animal->state = ANIMAL_ASLEEP;
		animal->wait_seconds = 0.0f;
	}
}

static void scan_homes(game_t *game, potop_t *potop, const potop_camera_info_t *camera)
{
	uint32_t ids[256];
	int count = potop_objects_near(potop, camera->walker_x, camera->walker_z, HOME_NEAR_METERS, ids, 256);

	for (int i = 0; i < count; i++)
	{
		home_of(game, potop, ids[i]);
	}
	for (int h = 0; h < game->animal_home_count; h++)
	{
		potop_object_info_t object;

		if (!potop_object_get(potop, game->animal_homes[h].object_id, &object)
			|| distance_squared_2d(object.x, object.z, camera->walker_x, camera->walker_z) > HOME_FAR_METERS * HOME_FAR_METERS)
		{
			// (gone, or far: its animals off the map; looked for again
			// when near)
			for (int k = game->animal_count - 1; k >= 0; k--)
			{
				if (game->animals[k].ground.home == h)
				{
					take_off(game, potop, k, false);
				}
			}
			for (int k = 0; k < game->animal_count; k++)
			{
				if (game->animals[k].ground.home == game->animal_home_count - 1)
				{
					game->animals[k].ground.home = h;
				}
			}
			game->animal_homes[h] = game->animal_homes[--game->animal_home_count];
			h--;
		}
	}
}

static float daylight_at(potop_t *potop, float x, float y, float z)
{
	potop_light_info_t light;

	potop_light_at(potop, x, y + 2.0f, z, &light);
	return light.daylight;
}

static void walk_to(potop_t *potop, animal_mind_t *animal, float x, float z)
{
	float point[1][2] = { { x, z } };

	animal->walking = true;
	animal->walk_x = x;
	animal->walk_z = z;
	potop_actor_stop_looking(potop, animal->actor_id);
	potop_actor_walk_straight(potop, animal->actor_id, (const float (*)[2])point, 1);
}

static void set_state(animal_mind_t *animal, animal_state_t state, float wait_seconds)
{
	// (out of its roaming: its boredom walk over; what the new state has
	// it do (a walk, a clip, a stop) stops the engine's wandering)
	if (state != ANIMAL_ROAM)
	{
		animal->wander.active = false;
	}
	animal->state = state;
	animal->state_seconds = 0.0f;
	animal->wait_seconds = wait_seconds;
}

// The evening: in by the building's way, or down where it is (outside)
static void start_night(game_t *game, potop_t *potop, animal_mind_t *animal, const potop_actor_info_t *actor)
{
	const animal_kind_t *kind = &ANIMAL_KINDS[animal->kind];
	float way[ANIMAL_CHAIN_MAX + 1][2];
	int count;

	animal->detour = false;
	animal->feeder = -1;
	animal->stall = -1;
	animal->has_sleep_point = false;
	animal->sleeps_inside = animal->ground.home >= 0 && (kind->vanish_at_end || random_unit(game) < kind->inside_share);
	if (animal->sleeps_inside && kind->sleep_spot != NULL)
	{
		// (a stall no other of its home has taken tonight; none free: out)
		for (int attempt = 0; attempt < 8 && animal->stall < 0; attempt++)
		{
			int stall = random_spot(game, potop, home_object(game, animal), kind->sleep_spot);
			bool taken = false;

			for (int k = 0; k < game->animal_count; k++)
			{
				taken = taken || (&game->animals[k] != animal && game->animals[k].ground.home == animal->ground.home && game->animals[k].stall == stall);
			}
			animal->stall = taken ? -1 : stall;
		}
		animal->sleeps_inside = animal->stall >= 0;
	}
	else if (animal->sleeps_inside && kind->sleep_area != NULL)
	{
		animal->has_sleep_point = point_in_area(game, potop, home_object(game, animal), kind->sleep_area, animal->pen, &animal->sleep_x, &animal->sleep_z);
		animal->sleeps_inside = animal->has_sleep_point;
	}
	count = animal->sleeps_inside ? way_in(game, potop, animal, actor, way) : 0;
	if (count > 0)
	{
		potop_actor_stop_looking(potop, animal->actor_id);
		potop_actor_walk_straight(potop, animal->actor_id, (const float (*)[2])way, count);
		set_state(animal, ANIMAL_GOING_IN, 0.0f);
		return;
	}
	// (out: down where it stands)
	if (animal->sleeps_inside)
	{
		game_trace_event(game, potop, animal->actor_id, "no_way_in");
	}
	animal->sleeps_inside = false;
	potop_actor_stop(potop, animal->actor_id);
	if (kind->lie_down_action != NULL)
	{
		potop_actor_play(potop, animal->actor_id, kind->lie_down_action);
	}
	set_state(animal, ANIMAL_GOING_DOWN, kind->lie_down_seconds);
}

static void think(game_t *game, potop_t *potop, int index, float seconds)
{
	animal_mind_t *animal = &game->animals[index];
	const animal_kind_t *kind = &ANIMAL_KINDS[animal->kind];
	uint32_t home = home_object(game, animal);
	potop_actor_info_t actor;
	float daylight;

	if (!potop_actor_get(potop, animal->actor_id, &actor))
	{
		if (animal->ground.home >= 0)
		{
			game->animal_homes[animal->ground.home].out--;
		}
		forget(game, index);
		return;
	}
	animal->state_seconds += seconds;
	// Killed: it lies there a while (its place at home still taken), then
	// it is taken off and another comes out later
	if (actor.dead)
	{
		if (animal->state != ANIMAL_DEAD)
		{
			set_state(animal, ANIMAL_DEAD, CORPSE_SECONDS);
		}
		else if (animal->state_seconds >= animal->wait_seconds)
		{
			if (animal->ground.home >= 0)
			{
				game->animal_homes[animal->ground.home].hatch_in = REPLACE_SECONDS;
			}
			take_off(game, potop, index, false);
		}
		return;
	}
	daylight = daylight_at(potop, actor.x, actor.y, actor.z);
	// Dusk: in, or down (each after its own while: not all at once)
	if (daylight < DUSK_DAYLIGHT && (animal->state == ANIMAL_ROAM || animal->state == ANIMAL_BUSY))
	{
		if (!animal->dusk_seen)
		{
			animal->dusk_seen = true;
			animal->dusk_delay = random_between(game, 0.0f, kind->dusk_spread);
		}
		animal->dusk_delay -= seconds;
		if (animal->dusk_delay <= 0.0f)
		{
			animal->dusk_seen = false;
			start_night(game, potop, animal, &actor);
			return;
		}
	}
	switch (animal->state)
	{
		case ANIMAL_GOING_IN:
		{
			float way[ANIMAL_CHAIN_MAX + 1][2];
			int count = way_in(game, potop, animal, &actor, way);
			bool there = count == 0;

			if (count > 0)
			{
				float squared = distance_squared_2d(actor.x, actor.z, way[count - 1][0], way[count - 1][1]);

				// (at its last point; or stopped near it: a hen's hole is
				// too small to walk in, it is in)
				there = squared < kind->in_meters * kind->in_meters || (!actor.moving && squared < kind->in_near_meters * kind->in_near_meters);
				// (stopped anywhere in its sleep area: in; the others lying
				// there in the way of its own place, a pig's in a small sty)
				if (!there && !actor.moving && animal->has_sleep_point && kind->sleep_area != NULL)
				{
					there = inside_area(potop, home, kind->sleep_area, animal->pen, actor.x, actor.z);
				}
			}
			if (there || animal->state_seconds > GOING_IN_SECONDS)
			{
				if (!there)
				{
					game_trace_event(game, potop, animal->actor_id, "in_late");
				}
				if (kind->vanish_at_end)
				{
					take_off(game, potop, index, true);
					return;
				}
				potop_actor_stop(potop, animal->actor_id);
				if (count > 0 && animal->stall >= 0)
				{
					potop_spot_t stall;

					if (potop_object_spot_get(potop, home, animal->stall, &stall))
					{
						potop_actor_look_at(potop, animal->actor_id, stall.x + SDL_sinf(stall.yaw_radians), stall.z + SDL_cosf(stall.yaw_radians));
					}
				}
				if (kind->lie_down_action != NULL)
				{
					potop_actor_play(potop, animal->actor_id, kind->lie_down_action);
				}
				set_state(animal, ANIMAL_GOING_DOWN, kind->lie_down_seconds);
			}
			else if (!actor.moving && daylight >= DAWN_DAYLIGHT)
			{
				set_state(animal, ANIMAL_ROAM, 0.0f);
			}
			else if (!actor.moving && animal->detour)
			{
				// (round it now: the way in again from here)
				game_trace_event(game, potop, animal->actor_id, "way_in_again");
				animal->detour = false;
				potop_actor_walk_straight(potop, animal->actor_id, (const float (*)[2])way, count);
			}
			else if (!actor.moving)
			{
				// Something in the way: somewhere else on its ground first
				// (a flock's at a narrow gate: just aside, and again)
				float x;
				float z;

				game_trace_event(game, potop, animal->actor_id, "detour");
				// (a flock's, or one going in to lie with others: just aside,
				// not off across its yard)
				if (kind->flock_radius > 0.0f || kind->sleep_area != NULL)
				{
					point_near(game, &actor, &x, &z);
					walk_to(potop, animal, x, z);
				}
				else if (point_on_ground(game, potop, animal, &x, &z))
				{
					walk_to(potop, animal, x, z);
				}
				animal->detour = true;
			}
			return;
		}
		case ANIMAL_GOING_DOWN:
			if (animal->state_seconds >= animal->wait_seconds)
			{
				potop_actor_stop_looking(potop, animal->actor_id);
				if (kind->sleep_action != NULL)
				{
					potop_actor_play(potop, animal->actor_id, kind->sleep_action);
				}
				set_state(animal, ANIMAL_ASLEEP, 0.0f);
			}
			return;
		case ANIMAL_ASLEEP:
			// (up each after its own while)
			if (daylight >= DAWN_DAYLIGHT && animal->wait_seconds <= 0.0f)
			{
				animal->wait_seconds = animal->state_seconds + random_between(game, 0.01f, SDL_max(kind->dawn_spread, 0.02f));
			}
			if (daylight >= DAWN_DAYLIGHT && animal->state_seconds >= animal->wait_seconds)
			{
				if (kind->get_up_action != NULL)
				{
					potop_actor_play(potop, animal->actor_id, kind->get_up_action);
				}
				set_state(animal, ANIMAL_GETTING_UP, kind->get_up_seconds);
			}
			return;
		case ANIMAL_GETTING_UP:
			if (animal->state_seconds >= animal->wait_seconds)
			{
				float way[ANIMAL_CHAIN_MAX + 1][2];
				int count = animal->sleeps_inside ? way_in(game, potop, animal, &actor, way) : 0;

				// (inside: the way in backwards, out to the yard)
				if (count > 1)
				{
					float out[ANIMAL_CHAIN_MAX + 1][2];

					for (int k = 0; k < count - 1; k++)
					{
						out[k][0] = way[count - 2 - k][0];
						out[k][1] = way[count - 2 - k][1];
					}
					potop_actor_walk_straight(potop, animal->actor_id, (const float (*)[2])out, count - 1);
					set_state(animal, ANIMAL_GOING_OUT, 0.0f);
				}
				else
				{
					set_state(animal, ANIMAL_ROAM, random_between(game, kind->stand_least, kind->stand_most));
				}
				animal->sleeps_inside = false;
				animal->stall = -1;
				animal->has_sleep_point = false;
			}
			return;
		case ANIMAL_GOING_OUT:
			if (!actor.moving)
			{
				// Out: about its ground. Stopped on the way (others in the
				// door, the strunga): out again from the nearest point of the
				// way, a while (else it would roam from inside, at a fence)
				float way[ANIMAL_CHAIN_MAX + 1][2];
				int count = way_in(game, potop, animal, &actor, way);
				float squared = count > 0 ? distance_squared_2d(actor.x, actor.z, way[0][0], way[0][1]) : 0.0f;

				if (count == 0 || squared < kind->in_near_meters * kind->in_near_meters || animal->state_seconds > GOING_IN_SECONDS)
				{
					set_state(animal, ANIMAL_ROAM, random_between(game, kind->stand_least, kind->stand_most) * 0.5f);
				}
				else
				{
					float out[ANIMAL_CHAIN_MAX + 1][2];
					int nearest = count - 1;
					int out_count = 0;

					for (int k = count - 1; k >= 0; k--)
					{
						if (distance_squared_2d(actor.x, actor.z, way[k][0], way[k][1]) < distance_squared_2d(actor.x, actor.z, way[nearest][0], way[nearest][1]))
						{
							nearest = k;
						}
					}
					for (int k = nearest; k >= 0; k--)
					{
						out[out_count][0] = way[k][0];
						out[out_count][1] = way[k][1];
						out_count++;
					}
					game_trace_event(game, potop, animal->actor_id, "way_out_again");
					potop_actor_walk_straight(potop, animal->actor_id, (const float (*)[2])out, out_count);
				}
			}
			return;
		case ANIMAL_BUSY:
			if (animal->state_seconds >= animal->wait_seconds)
			{
				potop_actor_stop_looking(potop, animal->actor_id);
				set_state(animal, ANIMAL_ROAM, random_between(game, kind->stand_least, kind->stand_most) * 0.5f);
			}
			else if (animal->wait_seconds > kind->eat_seconds * 1.5f && SDL_fmodf(animal->state_seconds, kind->eat_seconds) < THINK_SECONDS
				&& animal->state_seconds > THINK_SECONDS)
			{
				// (a few bites in a row: the clip again each time it ends)
				potop_actor_play(potop, animal->actor_id, "eat");
			}
			return;
		case ANIMAL_ROAM:
		{
			float x;
			float z;

			// (on its boredom walk: wander_update keeps it, till it is over)
			if (actor.moving || animal->wander.active)
			{
				return;
			}
			// (its walk over: short of where it went, something stopped it)
			if (animal->walking)
			{
				animal->walking = false;
				if (distance_squared_2d(actor.x, actor.z, animal->walk_x, animal->walk_z) > 1.0f)
				{
					char text[64];

					SDL_snprintf(text, sizeof(text), "walk_short %.1f", (double)SDL_sqrtf(distance_squared_2d(actor.x, actor.z, animal->walk_x, animal->walk_z)));
					game_trace_event(game, potop, animal->actor_id, text);
				}
			}
			// At a feeder (or stopped short of it by it): turned to it, a
			// few bites
			if (animal->feeder >= 0)
			{
				potop_spot_t spot;

				if (potop_object_spot_get(potop, home, animal->feeder, &spot))
				{
					potop_actor_look_at(potop, animal->actor_id, spot.x + SDL_sinf(spot.yaw_radians), spot.z + SDL_cosf(spot.yaw_radians));
				}
				animal->feeder = -1;
				potop_actor_play(potop, animal->actor_id, "eat");
				set_state(animal, ANIMAL_BUSY, kind->eat_seconds * (float)(1 + (int)(random_unit(game) * (float)SDL_max(kind->feeder_bites_most, 1))));
				return;
			}
			if (animal->state_seconds < animal->wait_seconds)
			{
				return;
			}
			// Stood its while: a bite off the ground, a stretch, or a walk
			if (animal->state_seconds > 0.0f && random_unit(game) < kind->ground_eat_share)
			{
				potop_actor_play(potop, animal->actor_id, "eat");
				set_state(animal, ANIMAL_BUSY, kind->eat_seconds * (float)(1 + (int)(random_unit(game) * (float)SDL_max(kind->ground_bites_most, 1))));
				return;
			}
			if (kind->stretch_action != NULL && random_unit(game) < kind->stretch_share)
			{
				potop_actor_play(potop, animal->actor_id, kind->stretch_action);
				set_state(animal, ANIMAL_BUSY, kind->stretch_seconds);
				return;
			}
			set_state(animal, ANIMAL_ROAM, random_between(game, kind->stand_least, kind->stand_most));
			if (animal->ground.home >= 0 && random_unit(game) < kind->feeder_share)
			{
				const char *name = kind->feeders[(int)(random_unit(game) * (float)ANIMAL_FEEDERS_MAX) % ANIMAL_FEEDERS_MAX];
				int feeder = name != NULL ? random_spot(game, potop, home, name) : -1;
				potop_spot_t pen_yard;

				// (in pens: the feeder nearest its own pen's yard, not over the
				// fence in the next)
				if (name != NULL && spot_count(potop, home, "yard") > 1
					&& potop_object_spot_get(potop, home, nth_spot(potop, home, "yard", animal->pen), &pen_yard))
				{
					feeder = nearest_spot(potop, home, name, pen_yard.x, pen_yard.z);
				}
				potop_spot_t spot;

				if (feeder >= 0 && potop_object_spot_get(potop, home, feeder, &spot))
				{
					// (round it a little: they do not all stand on one spot)
					float aside = random_between(game, -0.35f, 0.35f);

					walk_to(potop, animal, spot.x + SDL_cosf(spot.yaw_radians) * aside, spot.z - SDL_sinf(spot.yaw_radians) * aside);
					animal->feeder = feeder;
					return;
				}
			}
			if (kind->wander != NULL)
			{
				wander_start(&animal->wander, potop, animal->actor_id, kind->wander, actor.yaw_radians);
				return;
			}
			if (point_ahead(game, potop, animal, &actor, &x, &z))
			{
				walk_to(potop, animal, x, z);
			}
			return;
		}
		case ANIMAL_DEAD:
		case ANIMAL_STATE_COUNT:
			return;
	}
}

bool game_add_free_animal(game_t *game, potop_t *potop, const char *actor_type, float x, float z, float radius)
{
	for (int k = 0; k < ANIMAL_KIND_COUNT; k++)
	{
		if (SDL_strcmp(ANIMAL_KINDS[k].actor_type, actor_type) == 0)
		{
			uint32_t actor = potop_actor_spawn(potop, actor_type, x, z, random_between(game, 0.0f, 6.2831853f));
			animal_mind_t *animal;

			if (actor == 0)
			{
				return false;
			}
			animal = add_mind(game, actor, k);
			if (animal == NULL)
			{
				potop_actor_remove(potop, actor);
				return false;
			}
			animal->ground.x = x;
			animal->ground.z = z;
			animal->ground.radius = SDL_max(radius, 1.0f);
			return true;
		}
	}
	return false;
}

void game_update_animals(game_t *game, potop_t *potop, float seconds)
{
	potop_camera_info_t camera;

	potop_camera_get(potop, &camera);
	game->animal_scan_seconds -= seconds;
	if (game->animal_scan_seconds <= 0.0f)
	{
		game->animal_scan_seconds = SCAN_SECONDS;
		scan_homes(game, potop, &camera);
	}
	// Brought out one by one: the hens by day, the cows whenever (asleep
	// in the yard if it is night)
	for (int h = 0; h < game->animal_home_count; h++)
	{
		animal_home_t *home = &game->animal_homes[h];
		const animal_kind_t *kind = &ANIMAL_KINDS[home->kind];

		home->hatch_in -= seconds;
		// (a flock's grazing place moved on, now and then)
		if (kind->flock_radius > 0.0f && (home->graze_seconds -= seconds) <= 0.0f)
		{
			home->graze_seconds = random_between(game, kind->flock_move_least, kind->flock_move_most);
			home->graze_set = false;
		}
		if (home->out < kind->per_home && home->hatch_in <= 0.0f)
		{
			potop_object_info_t object;
			float daylight = 1.0f;

			home->hatch_in = random_between(game, HATCH_SECONDS_LEAST, HATCH_SECONDS_MOST) * (kind->hatch_in_the_morning ? 1.0f : 0.2f);
			if (potop_object_get(potop, home->object_id, &object))
			{
				daylight = daylight_at(potop, object.x, object.y, object.z);
			}
			if (!kind->hatch_in_the_morning || daylight >= DAWN_DAYLIGHT)
			{
				bring_out(game, potop, h, daylight);
			}
		}
	}
	for (int i = 0; i < game->animal_count; i++)
	{
		animal_mind_t *animal = &game->animals[i];
		int before = game->animal_count;

		// Its boredom walk, every frame (it looks at its own moment); over,
		// it stands a while
		if (animal->wander.active && animal->state == ANIMAL_ROAM
			&& !wander_update(&animal->wander, game, potop, animal->actor_id, ANIMAL_KINDS[animal->kind].wander, seconds))
		{
			const animal_kind_t *kind = &ANIMAL_KINDS[animal->kind];

			set_state(animal, ANIMAL_ROAM, random_between(game, kind->stand_least, kind->stand_most));
		}
		animal->think_in -= seconds;
		if (animal->think_in > 0.0f)
		{
			continue;
		}
		animal->think_in = THINK_SECONDS + random_between(game, 0.0f, 0.1f);
		think(game, potop, i, THINK_SECONDS);
		// (taken off: the last one is in its place now, thought next)
		if (game->animal_count < before)
		{
			i--;
		}
	}
}
