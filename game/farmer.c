// The farmer (for now, to try the doors): one man going round the farm
// buildings near the first one the animals live at (a byre, a pigsty, a
// hen house, a stana), opening a door that is shut and shutting one that
// is open, a breather between. The doors are the buildings' own (their
// modules' door parts: potop_door_set), the same the player swings with E.
//
// A door is opened from the side it swings to, standing beyond its latch
// edge (the leaf swings past him, short of where he stands), facing the
// latch: door_pull, the leaf set swinging as his hands take it. It is shut
// from beside the open leaf's end, behind it (the side it swings away
// from), facing the way it goes: door_push. To get there: by its paths to
// a point a little farther out on the swing side, then straight onto the
// spot, turned to the door.

#include "game.h"

#include <SDL3/SDL.h>

#define FARMER_ACTOR_TYPE "farmer"
#define FARMER_FARM_METERS 200.0f      // the buildings this near his first one are his
#define FARMER_DOORS_PER_BUILDING 12   // door1 .. door12 looked for
#define FARMER_APPROACH_METERS 1.2f    // the walk there ends this much farther out on the door's swing side
#define FARMER_ARRIVED_METERS 1.5f     // a walk there that ended this near: there (else another door)
#define FARMER_GOING_SECONDS 60.0f     // going there at most this long, then another door
#define FARMER_STEP_SECONDS 6.0f       // onto his spot at most this long
#define FARMER_CLIP_SECONDS 2.667f     // door_pull, door_push: their length
#define FARMER_HANDS_SECONDS 0.6f      // ...his hands on the leaf this far in: the door set swinging
#define FARMER_REACH_METERS 0.8f      // a point his paths end this near: reached
#define FARMER_PATH_MAX 2048          // a way found this long at most (its points)
#define FARMER_REST_LEAST 2.0f         // a breather between doors (seconds)
#define FARMER_REST_MOST 6.0f

static float farmer_random(game_t *game)
{
	uint32_t value = game->farmer.random != 0 ? game->farmer.random : 0x2545f491u;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	game->farmer.random = value;
	return (float)(value >> 8) / 16777216.0f;
}

// His doors: those of the animals' buildings near his first one
static void gather_doors(game_t *game, potop_t *potop)
{
	farmer_t *farmer = &game->farmer;
	potop_door_info_t first;

	farmer->door_count = 0;
	if (game->animal_home_count <= 0 || !potop_door_info(potop, game->animal_homes[0].object_id, potop_door_find(potop,
		game->animal_homes[0].object_id, "door1"), &first))
	{
		return;
	}
	for (int h = 0; h < game->animal_home_count; h++)
	{
		uint32_t building = game->animal_homes[h].object_id;

		for (int d = 1; d <= FARMER_DOORS_PER_BUILDING && farmer->door_count < FARMER_DOORS_MAX; d++)
		{
			char name[16];
			int door;
			potop_door_info_t info;
			bool known = false;

			SDL_snprintf(name, sizeof(name), "door%d", d);
			door = potop_door_find(potop, building, name);
			if (door < 0 || !potop_door_info(potop, building, door, &info))
			{
				continue;
			}
			if ((info.hinge[0] - first.hinge[0]) * (info.hinge[0] - first.hinge[0]) + (info.hinge[2] - first.hinge[2]) * (info.hinge[2] - first.hinge[2])
				> FARMER_FARM_METERS * FARMER_FARM_METERS)
			{
				continue;
			}
			// (a building two homes share, a pigsty's pens: its doors once)
			for (int k = 0; k < farmer->door_count; k++)
			{
				known = known || (farmer->doors[k].building == building && farmer->doors[k].door == door);
			}
			if (!known)
			{
				farmer->doors[farmer->door_count].building = building;
				farmer->doors[farmer->door_count].door = door;
				farmer->door_count++;
			}
		}
	}
}

// Where he stands to swing a door (see the top), the way he faces there,
// and a point a little farther out to walk to first. from_swing_side:
// on the side it swings to (opening: pulled; shutting: pushed from
// behind the open leaf), else on the other (opening: pushed, from beyond
// the latch there; shutting from there is not done)
static void door_spot(const potop_door_info_t *info, bool open_it, bool from_swing_side, float *x_out, float *z_out, float *yaw_out,
	float *approach_x_out, float *approach_z_out)
{
	float w = info->width;
	float side = from_swing_side ? 1.0f : -1.0f;

	if (open_it)
	{
		// (beyond the latch edge, facing the latch)
		float latch_x = info->hinge[0] + info->along[0] * (w - 0.05f);
		float latch_z = info->hinge[2] + info->along[2] * (w - 0.05f);

		*x_out = info->hinge[0] + info->along[0] * (w + 0.3f) + info->swing[0] * 0.35f * side;
		*z_out = info->hinge[2] + info->along[2] * (w + 0.3f) + info->swing[2] * 0.35f * side;
		*yaw_out = SDL_atan2f(latch_x - *x_out, latch_z - *z_out);
	}
	else
	{
		// (by the open leaf's end, behind it, facing the way it shuts)
		*x_out = info->hinge[0] + info->swing[0] * (w - 0.15f) - info->along[0] * 0.45f;
		*z_out = info->hinge[2] + info->swing[2] * (w - 0.15f) - info->along[2] * 0.45f;
		*yaw_out = SDL_atan2f(info->along[0], info->along[2]);
	}
	*approach_x_out = *x_out + info->swing[0] * FARMER_APPROACH_METERS * side;
	*approach_z_out = *z_out + info->swing[2] * FARMER_APPROACH_METERS * side;
}

// Whether his paths reach a point from where he is (or come within
// FARMER_REACH_METERS of it: a point by a wall is off them, his width
// off it); where they end, on open ground, into end_x, end_z
static bool reachable(potop_t *potop, const potop_actor_info_t *actor, float x, float z, float *end_x, float *end_z)
{
	static float points[FARMER_PATH_MAX][2];
	bool complete = false;
	int count = potop_find_path(potop, FARMER_ACTOR_TYPE, actor->x, actor->z, x, z, points, FARMER_PATH_MAX, &complete);
	float dx;
	float dz;

	if (count <= 0)
	{
		return false;
	}
	dx = points[count - 1][0] - x;
	dz = points[count - 1][1] - z;
	*end_x = points[count - 1][0];
	*end_z = points[count - 1][1];
	return dx * dx + dz * dz < FARMER_REACH_METERS * FARMER_REACH_METERS;
}

// The next door: any of his but the last one, shut or open, that his
// paths reach a side of (the one it swings to first); none: a breather
static void pick_door(game_t *game, potop_t *potop)
{
	farmer_t *farmer = &game->farmer;
	potop_actor_info_t actor;
	int first;

	farmer->state = FARMER_RESTING;
	farmer->seconds = 5.0f;
	if (farmer->door_count <= 0 || !potop_actor_get(potop, farmer->actor, &actor))
	{
		return;
	}
	first = (int)(farmer_random(game) * (float)farmer->door_count) % farmer->door_count;
	for (int k = 0; k < farmer->door_count; k++)
	{
		int pick = (first + k) % farmer->door_count;
		potop_door_info_t info;

		if ((pick == farmer->current && farmer->door_count > 1) || !potop_door_info(potop, farmer->doors[pick].building, farmer->doors[pick].door, &info))
		{
			continue;
		}
		for (int side = 0; side < 2; side++)
		{
			bool from_swing_side = side == 0;

			// (shut from the swing side only)
			if (!from_swing_side && info.open)
			{
				continue;
			}
			door_spot(&info, !info.open, from_swing_side, &farmer->spot_x, &farmer->spot_z, &farmer->spot_yaw, &farmer->approach_x,
				&farmer->approach_z);
			// (the walk there ends where his paths end by it: open ground,
			// where his next walk can start from)
			if (!reachable(potop, &actor, farmer->approach_x, farmer->approach_z, &farmer->approach_x, &farmer->approach_z))
			{
				continue;
			}
			farmer->current = pick;
			farmer->opening = !info.open;
			// (pulled open from the swing side, pushed from the other; pushed shut)
			farmer->pulling = farmer->opening && from_swing_side;
			potop_actor_stop_looking(potop, farmer->actor);
			potop_actor_play(potop, farmer->actor, "idle");
			potop_actor_go_to(potop, farmer->actor, farmer->approach_x, farmer->approach_z);
			farmer->state = FARMER_GOING;
			farmer->seconds = 0.0f;
			return;
		}
		{
			char text[48];

			SDL_snprintf(text, sizeof(text), "farmer_no_way %u/%d", farmer->doors[pick].building, farmer->doors[pick].door);
			game_trace_event(game, potop, farmer->actor, text);
		}
	}
	// No way to any: he stands where his paths do not start (by a wall,
	// on a step): a step or two out to where they do, then he looks again
	for (int ring = 1; ring <= 3; ring++)
	{
		for (int k = 0; k < 16; k++)
		{
			float angle = (float)k * (2.0f * SDL_PI_F / 16.0f);
			float x = actor.x + SDL_sinf(angle) * 0.8f * (float)ring;
			float z = actor.z + SDL_cosf(angle) * 0.8f * (float)ring;
			static float points[FARMER_PATH_MAX][2];
			potop_door_info_t info;

			if (!potop_actor_can_stand(potop, farmer->actor, x, z)
				|| !potop_door_info(potop, farmer->doors[first].building, farmer->doors[first].door, &info)
				|| potop_find_path(potop, FARMER_ACTOR_TYPE, x, z, info.hinge[0] + info.swing[0] * 2.0f, info.hinge[2] + info.swing[2] * 2.0f, points,
					FARMER_PATH_MAX, NULL) <= 0)
			{
				continue;
			}
			{
				float step[1][2] = { { x, z } };

				game_trace_event(game, potop, farmer->actor, "farmer_steps_out");
				potop_actor_walk_straight(potop, farmer->actor, (const float (*)[2])step, 1);
				farmer->state = FARMER_LEAVING;
				farmer->seconds = 0.0f;
				return;
			}
		}
	}
}

void game_update_farmer(game_t *game, potop_t *potop, float seconds)
{
	farmer_t *farmer = &game->farmer;
	potop_actor_info_t actor;

	// Not here yet: once the animals' buildings are known, by the first's
	// first door
	if (farmer->actor == 0)
	{
		potop_door_info_t info;

		if (game->animal_home_count <= 0 || farmer->gave_up)
		{
			return;
		}
		gather_doors(game, potop);
		if (farmer->door_count <= 0 || !potop_door_info(potop, farmer->doors[0].building, farmer->doors[0].door, &info))
		{
			// (buildings without moving doors: no farmer)
			farmer->gave_up = true;
			return;
		}
		// (out before his first door, as far out as he can stand)
		{
			float x = info.hinge[0] + info.swing[0] * 3.0f;
			float z = info.hinge[2] + info.swing[2] * 3.0f;

			farmer->actor = potop_actor_spawn(potop, FARMER_ACTOR_TYPE, x, z, SDL_atan2f(-info.swing[0], -info.swing[2]));
		}
		if (farmer->actor == 0)
		{
			farmer->gave_up = true;
			return;
		}
		potop_actor_set_name(potop, farmer->actor, "Farmer", false);
		for (int k = 0; k < farmer->door_count; k++)
		{
			potop_door_info_t d;
			char text[160];

			potop_door_info(potop, farmer->doors[k].building, farmer->doors[k].door, &d);
			SDL_snprintf(text, sizeof(text), "farmer_door %u/%d hinge %.2f %.2f %.2f along %.2f %.2f swing %.2f %.2f w %.2f", farmer->doors[k].building,
				farmer->doors[k].door, d.hinge[0], d.hinge[1], d.hinge[2], d.along[0], d.along[2], d.swing[0], d.swing[2], d.width);
			game_trace_event(game, potop, farmer->actor, text);
		}
		farmer->current = -1;
		farmer->state = FARMER_RESTING;
		farmer->seconds = 1.0f;
		return;
	}
	if (!potop_actor_get(potop, farmer->actor, &actor) || actor.dead)
	{
		farmer->actor = 0;
		farmer->gave_up = true;
		return;
	}
	farmer->seconds += seconds;
	switch (farmer->state)
	{
		case FARMER_RESTING:
		{
			farmer->seconds -= 2.0f * seconds;
			if (farmer->seconds <= 0.0f)
			{
				pick_door(game, potop);
			}
			break;
		}
		case FARMER_GOING:
		{
			float dx = actor.x - farmer->approach_x;
			float dz = actor.z - farmer->approach_z;

			if (farmer->seconds > FARMER_GOING_SECONDS)
			{
				game_trace_event(game, potop, farmer->actor, "farmer_lost");
				pick_door(game, potop);
				break;
			}
			if (actor.moving || farmer->seconds < 0.5f)
			{
				break;
			}
			// (stopped short of it: another door; no walk straight on from
			// wherever he is, through what is in the way)
			if (dx * dx + dz * dz > FARMER_ARRIVED_METERS * FARMER_ARRIVED_METERS)
			{
				game_trace_event(game, potop, farmer->actor, "farmer_short");
				pick_door(game, potop);
				break;
			}
			// There (or as near as his way took him): straight onto his
			// spot, turned to the door
			{
				float points[1][2] = { { farmer->spot_x, farmer->spot_z } };

				potop_actor_walk_straight_exact(potop, farmer->actor, (const float (*)[2])points, 1, farmer->spot_yaw);
				farmer->state = FARMER_STEPPING;
				farmer->seconds = 0.0f;
			}
			break;
		}
		case FARMER_STEPPING:
		{
			float turn = actor.yaw_radians - farmer->spot_yaw;

			while (turn > SDL_PI_F)
			{
				turn -= 2.0f * SDL_PI_F;
			}
			while (turn < -SDL_PI_F)
			{
				turn += 2.0f * SDL_PI_F;
			}
			if ((actor.moving || SDL_fabsf(turn) > 0.1f) && farmer->seconds < FARMER_STEP_SECONDS)
			{
				break;
			}
			if (farmer->seconds >= FARMER_STEP_SECONDS)
			{
				char text[96];

				SDL_snprintf(text, sizeof(text), "farmer_step_short %.2fm %.0fdeg %s", SDL_sqrtf((actor.x - farmer->spot_x) * (actor.x - farmer->spot_x)
					+ (actor.z - farmer->spot_z) * (actor.z - farmer->spot_z)), turn * 57.3f, actor.moving ? "moving" : "still");
				game_trace_event(game, potop, farmer->actor, text);
			}
			potop_actor_play(potop, farmer->actor, farmer->pulling ? "door_pull" : "door_push");
			farmer->state = FARMER_WORKING;
			farmer->seconds = 0.0f;
			farmer->door_set = false;
			break;
		}
		case FARMER_WORKING:
		{
			if (!farmer->door_set && farmer->seconds >= FARMER_HANDS_SECONDS)
			{
				potop_door_set(potop, farmer->doors[farmer->current].building, farmer->doors[farmer->current].door, farmer->opening);
				farmer->door_set = true;
				{
					char text[64];

					SDL_snprintf(text, sizeof(text), "%s %u/%d %s", farmer->opening ? "farmer_opened" : "farmer_shut", farmer->doors[farmer->current].building,
						farmer->doors[farmer->current].door, farmer->pulling ? "pull" : "push");
					game_trace_event(game, potop, farmer->actor, text);
				}
			}
			if (farmer->seconds >= FARMER_CLIP_SECONDS)
			{
				// Back out the way he came (off the wall: his paths start
				// on open ground again), then a breather
				float points[1][2] = { { farmer->approach_x, farmer->approach_z } };

				potop_actor_play(potop, farmer->actor, "idle");
				potop_actor_walk_straight(potop, farmer->actor, (const float (*)[2])points, 1);
				farmer->state = FARMER_LEAVING;
				farmer->seconds = 0.0f;
			}
			break;
		}
		case FARMER_LEAVING:
		{
			if (actor.moving && farmer->seconds < FARMER_STEP_SECONDS)
			{
				break;
			}
			{
				static const char *const breathers[] = { "idle", "idle_look", "idle_wipe", "idle_stretch", "idle" };

				potop_actor_play(potop, farmer->actor, breathers[(int)(farmer_random(game) * 5.0f) % 5]);
				farmer->state = FARMER_RESTING;
				farmer->seconds = FARMER_REST_LEAST + (FARMER_REST_MOST - FARMER_REST_LEAST) * farmer_random(game);
			}
			break;
		}
	}
}
