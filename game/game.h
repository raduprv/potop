// The game: what a game made with Potop decides, in C, through potop.h
// alone (main.c: the application around it). The player's kit, what E
// does with what the eye is on, the hits (health, death, loot), eating, a
// quest ("The Hunt") and a talker (Moș Martin, a bear who trades honey for
// tomatoes).
//
// It is an example and a test of the interface, not a game yet: a real
// game keeps its state and rules the same way.

#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "potop.h"

#define GAME_HEALTH_MAX 512
#define GAME_BEARS_MAX 512
#define GAME_LOOT_MAX 128
#define GAME_ANIMAL_HOMES_MAX 48
#define GAME_WORKERS_MAX 32
#define GAME_ANIMALS_MAX 384

// What the player did this frame, as the game needs it (the application
// reads the keys)
// What a bear is about (bear.c)
typedef enum
{
	BEAR_WANDER = 0,               // walking about (a short way at a time) and standing
	BEAR_REST,                     // by day, asleep in the shade
	BEAR_ALERT,                    // heard or glimpsed something: stands and looks
	BEAR_INVESTIGATE,              // goes to see what it was
	BEAR_WARN,                     // someone too near: it growls
	BEAR_ATTACK,                   // provoked: after them
	BEAR_FLEE,                     // badly hurt: away
	BEAR_STATE_COUNT
} bear_state_t;

typedef struct
{
	uint32_t actor_id;
	bear_state_t state;
	float state_seconds;           // in this state
	float think_in;                // till it thinks again
	float unthought_seconds;       // since it last thought
	float wander_heading;          // radians: which way it has been wandering (it keeps on roughly so)
	bool going;                    // on its way to goal
	float goal_x;
	float goal_z;
	float stand_seconds;           // wandering: standing this long between walks
	float rest_seconds;            // resting this long
	bool asleep;
	float sleep_seconds;
	float suspicion;               // 0..1: grows as it sees the player, fades
	bool has_interest;             // what it looks at, goes to see
	float interest_x;
	float interest_z;
	double last_seen_seconds;
	double knows_player_until;     // it has made the player out (seen clearly, growled at): it does not go to see it again till then
	double heard_until;            // the noises before this it heard already
	bool provoked;
	float attack_seconds;          // till its next blow
	float far_seconds;             // attacking: how long they have been far
	double go_again_at;            // nowhere it could stand near where it wanted to go: it tries again from then
} bear_mind_t;

// What a farm animal is about (animals.c)
typedef enum
{
	ANIMAL_ROAM = 0,               // about its ground: a walk, a bite, a stand
	ANIMAL_BUSY,                   // eating, or a stretch (an action playing)
	ANIMAL_GOING_IN,               // the evening: its way in (to a hole, a stall)
	ANIMAL_GOING_DOWN,             // lying down (its clip), then asleep
	ANIMAL_ASLEEP,
	ANIMAL_GETTING_UP,             // the morning: up (its clip), then out
	ANIMAL_GOING_OUT,              // from a stall the way back out
	ANIMAL_DEAD,                   // killed: it lies there a while, then is taken off (another comes later)
	ANIMAL_STATE_COUNT
} animal_state_t;

// The boredom walk (wander.c): nowhere to go, it just goes on. Every
// WANDER_TICK_SECONDS it looks ahead: free, it keeps on, now and then a
// little to one side if that is free too; not free, a little to one side
// or the other; neither, a step back; not even that, it stands and tries
// a while, then gives up. What stops it is only what is there: bodies,
// others (potop_actor_heading_free), no grid, no area
typedef struct
{
	float look_meters;             // how far past its nose it looks
	float drift_chance;            // each tick, the chance it turns a little while free ahead
	float drift_degrees;           // a little: this much, either side
	float search_degrees;          // blocked ahead: free ways looked for this far round, drift_degrees apart (walking on round them);
	                               // past that, round to 180 (stopping to turn on the spot, its turn clips)
	float speed_share;             // of its walk (turning too: no slowing down round a turn)
	float back_seconds;            // a step back: this long
	float back_share;              // at this share of its walk
	float give_up_seconds;         // nothing free at all this long: it stops
	int backs_most;                // so many steps back in one walk at most, then it stops
	float least_seconds;           // a walk this long (least, most)
	float most_seconds;
} wander_style_t;

typedef struct
{
	bool active;
	float heading;                 // the way it goes (0 = +z)
	float seconds_left;            // of this walk
	float tick_in;                 // its next look
	float blocked_seconds;         // nothing free for this long
	float back_seconds;            // stepping back: this much longer
	int backs;                     // steps back this walk
	int looks;                     // looks ahead this walk
	bool cornering;                // nothing free within search_degrees: stopped, turning on the spot to a free way
	float last_yaw;                // its body's yaw at the last look (still coming round, or not)
	float stuck_x;                 // where it was when it last got on (its legs going, its body not: caught)
	float stuck_z;
	float stuck_seconds;
	uint32_t random;               // its own dice
} wander_t;

// Where it keeps to: its home's area spot ("yard"), or round a point (one
// free about the land: no home)
typedef struct
{
	int home;                      // its home (game_t.animal_homes; -1: free)
	float x;                       // free: the middle and how far round
	float z;
	float radius;
} animal_ground_t;

typedef struct
{
	uint32_t actor_id;
	int kind;                      // animals.c's table
	animal_ground_t ground;
	animal_state_t state;
	float state_seconds;
	float wait_seconds;            // in it this long before it goes on
	float think_in;
	int feeder;                    // the spot it walks to to eat (-1: none)
	int in_step;                   // its way in: how far along (the kind's chain)
	int stall;                     // the spot it sleeps at inside (-1: outside, in the yard)
	bool sleeps_inside;            // tonight
	bool detour;                   // something in its way: off elsewhere first, then again
	// Its place for the night in the kind's sleep area (a sheep in the
	// fold): picked at dusk, the end of its way in
	bool has_sleep_point;
	float sleep_x;
	float sleep_z;
	// Where its walk about its ground was to end (a test's trace: stopped
	// short of it, something was in the way)
	bool walking;
	float walk_x;
	float walk_z;
	// Dusk seen: it goes in (or down) after this many seconds more (a
	// flock not all at once through its strunga)
	bool dusk_seen;
	float dusk_delay;
	// Its boredom walk (wander.c), when the kind has one
	wander_t wander;
	// Its pen, where its home has more than one (a pigsty's two): the nth
	// of the home's "yard" spots and of its kind's sleep area, its feeders
	// the nearest to that yard; 0 else
	int pen;
} animal_mind_t;

// The farmer (farmer.c): going round the farm's doors, for now
#define FARMER_DOORS_MAX 48

typedef enum
{
	FARMER_RESTING = 0,            // a breather (seconds: left of it)
	FARMER_GOING,                  // to a door, by his paths
	FARMER_STEPPING,               // onto his spot by it, turning to it
	FARMER_WORKING,                // swinging it (door_pull / door_push)
	FARMER_LEAVING                 // back out to where he came onto his spot from
} farmer_state_t;

typedef struct
{
	uint32_t actor;                // 0: none (yet)
	bool gave_up;                  // no farm with moving doors, or he is gone
	farmer_state_t state;
	float seconds;                 // in this state (resting: left of it)
	struct
	{
		uint32_t building;         // the object
		int door;                  // its door (potop_door_set)
	} doors[FARMER_DOORS_MAX];
	int door_count;
	int current;                   // the door he is at (-1: none)
	bool opening;                  // opening it (else shutting)
	bool pulling;                  // door_pull (else door_push)
	bool door_set;                 // set swinging this time
	float spot_x;                  // where he stands to do it, facing spot_yaw
	float spot_z;
	float spot_yaw;
	float approach_x;              // the walk there ends here first
	float approach_z;
	uint32_t random;
} farmer_t;

// A building animals live at (animals.c): a hen house, a cow farm
typedef struct
{
	uint32_t object_id;
	int kind;
	int out;                       // its animals on the map now
	float hatch_in;                // till the next comes out
	// A flock's (sheep): where on the yard they graze now, all round it;
	// moved on now and then, the flock after it
	bool graze_set;
	float graze_x;
	float graze_z;
	float graze_seconds;           // till it moves on
} animal_home_t;

typedef struct
{
	bool interact;                 // E
	int select_weapon;             // 1, 2...: weapons.ini's (-1: none)
	bool trigger;                  // the weapon's buttons
	bool aim;
	bool pointer_free;             // the pointer is out for the interface: the eye picks nothing
} game_input_t;

// A worker's mind (workers.c): the building he works, his actor, which
// step of his round, walking to it, turning to it or doing it
typedef enum
{
	WORKER_WALKING,
	WORKER_FACING,
	WORKER_WORKING
} worker_phase_t;

// The work in his building (workers.c): the fire (0 out .. 1 roaring), the
// iron's heat (1: white hot), whether it is in the fire, how much hammering
// the piece in hand still wants (seconds), pieces done since the last
// breather
typedef struct
{
	float fire;
	float heat;
	bool in_fire;
	bool has_piece;
	float piece_left;
	int pieces_done;
	float worked_seconds;          // at the work since his last proper break
	float break_after;             // a proper break due after this much
} workshop_t;

// A step of what he means to do: where (a spot by name, a point, or where
// he is), the clip and how long; state: the building's bellows pump the
// while
#define WORKER_STEPS_MAX 24
typedef struct
{
	int place;                     // workers.c place_kind_t
	char spot[32];
	float x;
	float z;
	float yaw;
	char action[32];
	float seconds;
	bool state;
} worker_step_t;

// Where a worker is in his day (workers.c): at his work, on his way
// somewhere, or in somewhere (his home, the inn: no actor)
typedef enum
{
	LIFE_WORK,
	LIFE_TRAVEL,
	LIFE_INSIDE
} life_phase_t;

// The places of his day
typedef enum
{
	LIFE_AT_WORK,
	LIFE_AT_HOME,
	LIFE_AT_INN
} life_place_t;

// His day's hours, drawn again each day: up and off to work, lunch (at
// home or at the inn) and back, the work's end, the inn till late (or not)
typedef struct
{
	float work_start;
	float lunch_start;
	float lunch_end;
	float work_end;
	float inn_end;
	bool lunch_at_inn;
	bool skips_inn;
} life_day_t;

#define WORKER_ROUTE_MAX 128

typedef struct
{
	uint32_t building;
	uint32_t actor;                // 0: none (far off, or in somewhere)
	workshop_t shop;
	worker_step_t steps[WORKER_STEPS_MAX];
	int step_count;
	int step_index;
	worker_phase_t phase;
	float seconds;                 // in this phase
	int tries;                     // walks to this place tried again
	bool outside;                  // out front (his breather): in again by a door
	uint32_t random_state;
	// His life: his home and his inn (0: none, his smithy stands for his
	// home), where he is in his day, where he is going (or is), where he
	// stands (kept while there is no actor), the way there (walked by
	// itself while there is no actor), his day's hours
	uint32_t home;
	uint32_t inn;
	life_phase_t life;
	life_place_t place;
	float x;
	float z;
	float yaw;
	float route[WORKER_ROUTE_MAX][2];
	int route_count;
	int route_index;
	bool at_door;                  // walking onto the door's spot, then in
	bool from_door;                // out of a door: a few steps straight out first (a porch, steps: no way found from on them)
	int exit_count;                // the first points of his route: those straight out of the door (down its stairs)
	int leaving;                   // 1: due off (the work wound up at the next pause), 2: winding up (the piece put away), then off
	float travel_seconds;          // on this leg of the way (a walk stuck: tried again, then he is put there)
	int travel_tries;
	float last_hour;
	uint32_t day;
	life_day_t hours;
} worker_mind_t;

typedef struct
{
	// The stand in for health (a hit's damage, a head shot's more): per
	// actor hit so far, what it has left
	uint32_t health_ids[GAME_HEALTH_MAX];
	float health[GAME_HEALTH_MAX];
	int health_count;
	// Each dead animal's container
	uint32_t loot_actor_ids[GAME_LOOT_MAX];
	uint32_t loot_containers[GAME_LOOT_MAX];
	int loot_count;
	uint32_t loot_random;
	// The hunting quest (the journal only shows it)
	int hunt_bears;
	int hunt_stage;                // 0 hunting, 1 to the camp, 2 done
	float hunt_camp_x;
	float hunt_camp_z;
	float hunt_place_seconds;
	bool hunt_focused;             // its first objective put in focus once (then the player's choice)
	// Moș Martin (the game's side of the conversation)
	uint32_t martin;               // his actor (0: not yet put down)
	bool martin_met;
	int martin_honey;              // 0 not asked, 1 asked (three tomatoes), 2 given
	bool martin_leaving;           // his last line said: the next answer closes it
	// His chest (a crate by him, locked: the key is his to give, for the
	// tomatoes) and what is in it; what grows round him is his
	uint32_t martin_chest;
	uint32_t martin_chest_container;
	bool martin_owns_round;
	float frame_seconds_average;   // the debug window's frame rate (debug.c)
	// The bears' minds (bear.c)
	bear_mind_t bears[GAME_BEARS_MAX];
	int bear_count;
	float bear_count_seconds;      // till the bears are counted again
	uint32_t bear_random;
	float bear_milliseconds;       // what their minds cost a frame (on average)
	// The farm animals (animals.c): their homes near the eye, their minds
	farmer_t farmer;
	animal_home_t animal_homes[GAME_ANIMAL_HOMES_MAX];
	int animal_home_count;
	animal_mind_t animals[GAME_ANIMALS_MAX];
	int animal_count;
	float animal_scan_seconds;
	uint32_t animal_random;
	// A test's trace (trace.c, --trace FILE): the minds' animals sampled
	// now and then, their events; NULL: none
	void *trace;                   // (an SDL_IOStream)
	float trace_seconds;           // till the next sample
	// A walking test (walk_test.c, --walk_test TYPE X,Z[;X,Z...]): an actor
	// of the type put down (at walk_test_from, or where the player stands)
	// and sent to each goal in turn, into the trace
	char walk_test_type[32];
	float walk_test_goals[64][2];
	int walk_test_goal_count;
	int walk_test_next;            // the goal it is going to
	bool walk_test_from_set;
	float walk_test_from[2];
	uint32_t walk_test_actor;      // 0: not put down yet
	float walk_test_wait;          // seconds till it is put down / still since it stopped
	bool walk_test_done;           // every goal tried
	bool walk_test_running;        // it runs to them (--walk_run)
	float walk_test_goal_seconds;  // since it was sent to this goal
	float walk_test_trail[2][2];   // the trail's box (minimum, maximum), for the trace's map
	// (a chase, --walk_chase SPEED: the goals are a runner's route, a
	// farmer's way from each to the next, gone along at that speed; the
	// actor after the runner as a bear after the player)
	float walk_chase_speed;        // meters a second; 0: no chase, the goals in turn
	float walk_chase_route[1024][2];
	int walk_chase_route_count;
	float walk_chase_meters;       // the runner's, along its route
	float walk_chase_pause;        // seconds the runner stands at each goal
	bool walk_chase_straight[64];  // a goal the runner goes straight to and back from if no farmer gets there ("X,Z!"); else it is left out
	float walk_chase_stops[64];    // the goals' meters along the route
	int walk_chase_stop_next;
	float walk_chase_pause_left;
	float walk_chase_sent[2];      // where the actor was last sent
	bool walk_chase_going;
	double walk_chase_again_at;    // not sent again before this (world time)
	float walk_chase_seconds;      // since the runner set out
	float walk_chase_idle_seconds; // the actor standing, the runner out of its reach
	int walk_chase_sends;
	// The workers (workers.c): a smith in each smithy near the eye
	worker_mind_t workers[GAME_WORKERS_MAX];
	int worker_count;
	float worker_scan_seconds;
	uint32_t worker_scan_generation; // the object list's (potop_objects_generation) when the smithies were last looked for
	bool worker_scanned;
	potop_t *potop;                // (for the console's commands)
	bool water_refused_told;       // the water spell on a baked map: said once
} game_t;

// Once the engine is ready: the kit in a pack of 40 kg, what can be picked
// off the map
void game_start(game_t *game, potop_t *potop);
// A map was loaded: who was who comes back by the game's numbers
void game_map_loaded(game_t *game, potop_t *potop);
// Once a frame, before the engine's update: the input, what the eye is on
// and what E does, the quests, the talk
void game_update(game_t *game, potop_t *potop, const game_input_t *input, float seconds);
// After the engine's update: what the projectiles hit
void game_after_update(game_t *game, potop_t *potop);

// (the game's parts: hunt.c, martin.c, places.c)
void game_update_hunt(game_t *game, potop_t *potop, float seconds);
void game_update_martin(game_t *game, potop_t *potop);
// choice: the answer's id; -1: the talk begins
void game_martin_talk(game_t *game, potop_t *potop, int choice);
// Whether the player may take what is owner's (an actor's game number):
// no one's, the owner dead or gone, or a friend (Moș Martin, once he got
// his tomatoes)
bool game_may_take(game_t *game, potop_t *potop, uint32_t owner);
// The name of whose it is ("Moș Martin"), for "Steal ... from"
void game_owner_name(potop_t *potop, uint32_t owner, char *name_out, size_t name_out_size);
// (debug.c: F3's window) toggle: F3 this frame; seconds: this frame's
void game_update_debug(game_t *game, potop_t *potop, bool toggle, float seconds);
// (bear.c: the bears' minds) once a frame; a bear was hurt (its health's
// share left; by the player: provoked)
void game_start_bears(game_t *game, potop_t *potop);
void game_update_bears(game_t *game, potop_t *potop, float seconds);
void game_bear_hurt(game_t *game, potop_t *potop, uint32_t actor_id, float health_share, bool by_player);
const char *game_bear_state_name(bear_state_t state);
// (trace.c: a test's trace of the animals' minds, for
// tools/animal_trace.py's picture and numbers) start: false if the file
// cannot be made; once a frame; an event of one (what its mind did: a
// detour, in late), at where it is; finish: the homes' spots and the
// obstacles round the trails, then closed
bool game_trace_start(game_t *game, const char *path);
// (walk_test.c) the test asked for (the command line's): false if it
// does not read
bool game_walk_test_set(game_t *game, const char *type_name, const char *goals, const char *from);
void game_walk_test_update(game_t *game, potop_t *potop, float seconds);
void game_trace_update(game_t *game, potop_t *potop, float seconds);
void game_trace_event(game_t *game, potop_t *potop, uint32_t actor_id, const char *text);

// Off on a boredom walk the way it faces (yaw_radians), for a while
void wander_start(wander_t *wander, potop_t *potop, uint32_t actor_id, const wander_style_t *style, float yaw_radians);
// Its frame (looks every WANDER_TICK_SECONDS, spread over the frames);
// false once the walk is over (its time up, or it gave up: it stands)
bool wander_update(wander_t *wander, game_t *game, potop_t *potop, uint32_t actor_id, const wander_style_t *style, float seconds);
// Stopped where it is
void wander_stop(wander_t *wander, potop_t *potop, uint32_t actor_id);

void game_trace_finish(game_t *game, potop_t *potop);
const char *game_animal_state_name(animal_state_t state);
const char *game_animal_kind_name(int kind);
// (animals.c: the farm animals round their homes, by the buildings'
// spots: the hens at their hen houses, the cows at their farms) once a
// frame
void game_update_animals(game_t *game, potop_t *potop, float seconds);
void game_update_farmer(game_t *game, potop_t *potop, float seconds);
void game_update_workers(game_t *game, potop_t *potop, float seconds);
void game_workers_map_loaded(game_t *game, potop_t *potop);
// One loose about the land, round (x, z) within radius, no home (a cow
// out to pasture, a hen gone astray); its kind by its actor type ("cow",
// "hen"); false if there is no such kind or no room
bool game_add_free_animal(game_t *game, potop_t *potop, const char *actor_type, float x, float z, float radius);
// (places.c: the areas)
void game_start_places(game_t *game, potop_t *potop);
void game_update_places(game_t *game, potop_t *potop);

#endif
