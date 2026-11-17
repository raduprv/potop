// Actors: animated creatures on the map (a bear, later people, ducks...).
//
// Their kinds come from data/actors.ini, one section per type, each naming
// a .pact model (made from other formats by tools/potop_convert). An actor
// stands on the terrain at its place, turned by its yaw; it is drawn
// skinned and lit like the objects. Actors cast shadows only into the near
// shadow map (around the camera's target), which is redrawn often enough
// while they move (render.actor_shadow_update_seconds).
//
// Each actor plays one action of its model at a time (walk, idle, die...).
// Switching crossfades from the old pose to the new one. An action that
// plays once goes back to the type's idle action when it ends, except the
// ones named die...: those hold their last pose.
//
// Sent somewhere (actors_move_to), an actor finds its way with the
// navigation (see navigation.h), turns towards each waypoint and walks
// there, slower uphill and through water, swimming where it is deep; the
// walk animation keeps pace with its speed. It does not test every step:
// it follows its path, watches the stretch ahead it can see and searches
// again when that is blocked, when it is stuck, or when something marks
// its path dirty (a falling log that hits it). Running water pushes it,
// swimming or wading, walking or not. Caught by water too fast to get out
// of, it waits and tries again every second instead of giving up.
// What happens comes back as events (actors_poll_event).
//
// Feet stand on objects (inverse kinematics): each leg (a foot bone and
// the two above it) of the animated pose is bent so a foot on an object's
// top (a deck, a boulder) is as high above it as the animation lifts it
// above the body's ground plane at the same point, and the foot turns with
// the top's slope while it is down. On the terrain the legs move as
// animated (the body leans with the slope). What is under each foot is
// looked up in the move (the navigation's surfaces, where the foot was last
// frame); the bending is done with the pose.
//
// Flyers (flies = true in actors.ini, a falcon) start on the ground. Sent
// somewhere, they take off and follow a flight path (navigation.h): over
// everything under their ceiling, around what rises above it. How high
// they fly is their own: a cruising height above the ground, picked at
// random between fly_height_minimum_meters and fly_height_maximum_meters
// every few seconds, and never less than what they see ahead (they climb
// before a ridge, not into it). They bank into turns and point their nose
// where they go. At the goal they circle over it, if the ring around it is
// clear low enough (within fly_height_maximum_meters of the goal's
// ground); else they come down on the nearest dry, open spot and stand
// there. Water under them on the ground (a flood) makes them take off and
// circle. Dying (an action named die...), they drop.

#ifndef ACTORS_H
#define ACTORS_H

#include <stdbool.h>
#include <stdint.h>

#include "actor_model.h"
#include "camera.h"
#include "settings.h"
#include "shadow_map.h"
#include "navigation.h"
#include "terrain.h"
#include "water.h"

#define ACTOR_TYPES_MAX 64
#define ACTOR_TYPE_NAME_LENGTH 48
#define ACTOR_MAX_LEGS 8
#define ACTOR_IK_FEET_LENGTH 256

// Steps between standing and walking (actor_type_t.gait_actions)
enum
{
	ACTOR_GAIT_START,                  // walk_start: off, straight on
	ACTOR_GAIT_START_180,              // walk_start_180: off, turning about
	ACTOR_GAIT_STOP_LEFT,              // walk_stop_l
	ACTOR_GAIT_STOP_RIGHT,             // walk_stop_r
	ACTOR_GAIT_STEP_LEFT,              // step_left
	ACTOR_GAIT_STEP_RIGHT,             // step_right
	ACTOR_GAIT_STEP_BACK,              // step_back
	ACTOR_GAIT_SHUFFLE,                // shuffle
	ACTOR_GAIT_COUNT
};

// A leg for the feet on the ground: two bones that bend (thigh and calf,
// or upper arm and forearm on all fours) and the foot at their end
typedef struct
{
	int upper;                         // bone indices in the model
	int lower;
	int foot;
	float bind_foot_height;            // the foot's height in the bind pose, model space: standing
} actor_leg_t;

typedef struct
{
	char name[ACTOR_TYPE_NAME_LENGTH];
	char model_path[ACTOR_MODEL_PATH_LENGTH]; // relative to data/
	int model;                         // index into actors_t.models, -1 if it did not load
	float scale;                       // the model's size is multiplied by this
	float front_degrees;               // where the model's head points: 0 = +Z, 90 = +X
	float walk_meters_per_second;      // for when they walk
	float run_meters_per_second;
	float blend_seconds;               // crossfade between actions
	// How much it leans with the ground under its feet: 1 lies along the
	// slope (four legged animals), 0 stays upright (people)
	float ground_align;
	char idle_action_name[ACTOR_FILE_NAME_LENGTH];
	int idle_action;                   // in the model's actions, -1 if it has none of that name
	char walk_action_name[ACTOR_FILE_NAME_LENGTH];
	int walk_action;                   // also used for swimming; -1 if none
	// Moving about
	float walk_cycle_meters;           // ground covered by one loop of the walk; 0: the walk at walk speed
	float turn_degrees_per_second;
	// Turning on the spot: these actions if the model has them (optional),
	// else the walk at the pace its feet go round
	char turn_left_action_name[ACTOR_FILE_NAME_LENGTH];
	int turn_left_action;              // -1 if none
	char turn_right_action_name[ACTOR_FILE_NAME_LENGTH];
	int turn_right_action;             // -1 if none
	// Turning while it walks: these walks round a curve if the model has
	// them (optional), made to turn walk_turn_degrees a cycle of the walk
	// (the same length, steps and speed as the walk: one goes into the
	// other at the same point of its step). A wanderer turns no faster
	// than they do (a curve of its own size, not a spin on the walk)
	char walk_left_action_name[ACTOR_FILE_NAME_LENGTH];
	int walk_left_action;              // -1 if none
	char walk_right_action_name[ACTOR_FILE_NAME_LENGTH];
	int walk_right_action;             // -1 if none
	float walk_turn_degrees;           // a cycle of walk_left / walk_right; also a wanderer's turn while walking without them
	// Walking straight (actors_walk_straight), it keeps this much of its
	// walk in a sharp turn and goes round in an arc (a cow) instead of
	// stopping to turn on the spot (0: it stops); on the spot still where
	// the arc's first step is blocked, or the goal is inside its circle
	float turn_walk_fraction;
	// Walking straight, how far it may turn standing (degrees; below 0, not
	// given: 20 for one on four legs, ground_align above 0, else no limit):
	// past it, it turns only going, forward round an arc or backing out
	// (a cow does not spin on the spot, hit by another or not)
	float spot_turn_degrees;
	// Its steps between standing and walking, if its model has them (people:
	// found by name, ACTOR_GAIT_): a walk's start (and one turning about as
	// it starts, its own turn in it), its stop on either foot (begun as that
	// foot lands: at those phases of the walk), steps aside and back, a
	// shuffle on the spot; how far each takes it (as made)
	int gait_actions[ACTOR_GAIT_COUNT];
	float walk_start_meters;
	float walk_start_180_meters;
	float walk_start_180_delay;        // of its length, before it moves
	float walk_stop_meters;
	float walk_stop_left_phase;        // of the walk, its left foot landing
	float walk_stop_right_phase;
	float sidestep_meters_per_second;
	float step_back_meters_per_second;
	float maximum_slope_degrees;       // steeper it does not climb
	float vision_meters;               // how far along its path it notices changes
	float sight_meters;                // how far it sees someone in full light (senses.h)
	float sight_degrees;               // how wide it looks (all of it: 360 sees behind too)
	bool can_swim;
	uint8_t path_groups;               // bits: the blockers (navigation_set_blockers) that stop it are those for one of these (1 by default)
	bool opens_doors;                  // its ways go through shut doors too (it opens them: a person); else only open ones
	float swim_meters_per_second;
	float wade_depth_meters;           // deeper water slows it
	float swim_depth_meters;           // deeper water lifts it: it swims, its body this deep
	float height_meters;               // room it needs to pass under things; 0: the model's height
	float step_meters;                 // climbs this much from one meter to the next, however steep
	float width_meters;                // across its flanks: its path keeps half that from what is in the way; 0: the model's
	// Flying (flies = true): walk_action is its flight, idle_action what it
	// does on the ground
	bool flies;
	float fly_meters_per_second;
	float fly_height_minimum_meters;   // above the ground: the lowest it cruises
	float fly_height_maximum_meters;   // the highest it cruises, and circles at
	float fly_ceiling_meters;          // above the higher of where it starts and where it goes: never higher
	float circle_radius_meters;        // over its goal
	float climb_meters_per_second;     // it dives twice as fast
	char glide_action_name[ACTOR_FILE_NAME_LENGTH];
	int glide_action;                  // played going down, -1 if none (it flaps)
	// Feet on the ground: ik_feet names the foot bones (comma separated),
	// empty: every bone whose name starts with foot; ik = false: none
	bool ik;
	char ik_feet[ACTOR_IK_FEET_LENGTH];
	actor_leg_t legs[ACTOR_MAX_LEGS];
	int leg_count;
} actor_type_t;

#define ACTOR_EVENT_QUEUE_SIZE 64

// Where a flyer is at
typedef enum
{
	ACTOR_FLIGHT_LANDED = 0,           // on the ground (walkers are always there)
	ACTOR_FLIGHT_FLYING,               // on its way
	ACTOR_FLIGHT_CIRCLING,             // over its goal
	ACTOR_FLIGHT_LANDING,              // coming down on a spot near its goal
	ACTOR_FLIGHT_FALLING               // dead, dropping
} actor_flight_t;

typedef enum
{
	ACTOR_EVENT_ARRIVED = 1,           // got where it was sent
	ACTOR_EVENT_GOT_NEAR,              // the goal cannot be reached: stopped as near as it could
	ACTOR_EVENT_NO_PATH                // could not move towards the goal at all
} actor_event_type_t;

typedef struct
{
	actor_event_type_t type;
	uint32_t id;
	uint32_t game_id;                  // the game's number for it (0: none)
	float x;                           // where it stands now
	float z;
} actor_event_t;

typedef struct
{
	uint32_t id;                       // stays the same while the actor lives; never 0
	int type;
	// The game can do something with it (what, the game decides; for now
	// every actor, until the game sets it through its API)
	bool interactive;
	// What the game calls it (shown over its head; empty: nothing shown),
	// and whether it is an enemy (the name in red)
	char name[32];
	bool hostile;
	// The game's own number for it (0: none): a quest's wolf, the
	// grandmother. Kept in the map's file, found again with
	// actors_find_by_game_id
	uint32_t game_id;
	// Turning to look at a place while it stands (actors_look_at)
	bool facing;
	float face_x;
	float face_z;
	float x;                           // meters, on the map
	float z;
	float y;                           // the ground under it, kept up to date
	float yaw_radians;                 // which way it faces: 0 = +Z
	float turn_rate;                   // radians a second it turns, eased (> 0: to its left, towards +X from +Z)
	float angular_speed;               // radians a second it turns now (its turns' inertia: inertial_turn)
	// Where its walk steers for (not straight walkers): a point a little
	// ahead along its path, the corner cut (steer_point); kept a while
	float steer_x;
	float steer_z;
	float steer_seconds;               // till it is found again (0: now)
	int steer_index;                   // the path_index it was found at
	float scale;
	float rotation[4];                 // the model's turn (yaw and lean on slopes), quaternion
	// Animation
	int action;                        // in the model's actions; -1: the bind pose
	float action_seconds;              // how far into it
	int previous_action;               // fading out, -1 if none
	float previous_seconds;
	float blend_left_seconds;          // of the crossfade
	float previous_rate;               // the one fading out goes on at its own pace
	// Its walk bent round a curve: -1 all walk_right, 1 all walk_left, 0
	// straight (the walk alone), mixed at the same point of the step;
	// eased toward how fast it turns against how fast those turn, so a
	// turn coming and going bends it smoothly (no switching of clips)
	float walk_curve;
	float bone_rows[ACTOR_FILE_MAX_BONES * 12]; // the pose's skinning matrices
	uint32_t pose_frame;               // the frame (actors_t.pose_clock) bone_rows were posed in (see ensure_pose)
	float action_rate;                 // the current action's playing speed (walk: with the pace)
	// How far the pose lifts the model's root above the bind pose. Eternal
	// Lands' birds fly by their animations, a meter or two up: a flyer is
	// drawn that much lower, so it is where it is (and stands on the ground)
	float root_lift;
	// Feet on the ground (inverse kinematics). Where the animation put each
	// foot last frame (the map, x and z), and what is under it there: the
	// surface's height and its normal. The legs bend so the feet stand on
	// it; ik_weight fades that out (swimming, flying)
	bool feet_known;
	// The ground at the four points around the feet the body leans by, as
	// last found (each point keeps to what it was on: a rock, a deck)
	bool ground_samples_known;
	float ground_samples[4];
	// The body's lean as last placed, and whether it was placed on the map
	// before (then it eases to the new place instead of jumping)
	float lean_pitch;
	float lean_roll;
	bool placed;
	float foot_x[ACTOR_MAX_LEGS];
	float foot_z[ACTOR_MAX_LEGS];
	float foot_ground[ACTOR_MAX_LEGS];
	float foot_height[ACTOR_MAX_LEGS]; // foot_ground eased: what the foot is put on
	float foot_weight[ACTOR_MAX_LEGS]; // how much the leg bends for it: 1 on an object's top, 0 on the terrain (eased)
	float foot_normal[ACTOR_MAX_LEGS][3];
	float ik_weight;
	// Moving
	bool moving;
	float goal_x;
	float goal_z;
	float path[NAVIGATION_MAX_WAYPOINTS][2];
	int path_count;
	int path_index;                    // the waypoint it heads for
	bool path_complete;                // the last waypoint is the goal (else: search on from there)
	bool path_partial;                 // the goal cannot be reached; the path ends as near as it gets
	uint32_t path_generation;          // the navigation's generation when it last looked ahead
	float look_seconds;                // until it looks ahead again
	float speed;                       // meters per second, its own (the current comes on top)
	float stuck_seconds;               // without getting anywhere
	float progress_x;                  // where it was when it last got somewhere
	float progress_z;
	int replans;                       // searches for this goal without getting anywhere
	bool swimming;
	bool path_dirty;                   // search again at once (something hit it, or fell across its way)
	// Walking given points straight (actors_walk_straight): no search for a
	// way, ever; a body in its way (physics) stops it there
	bool straight;
	// Where its yaw stood when it last went (spot_turn_degrees: standing,
	// it turns no farther from it than that)
	float spot_yaw_radians;
	// Walks stopped at once in a row (caught in something: a hen in a
	// feeder's trough, every way out a wall from inside); past a few, the
	// next walks out free for a while (straight_free_meters)
	int straight_blocks;
	float straight_free_meters;
	// Steering round what is in its way (a fence's corner, one asleep in
	// the yard): the side it goes round on (1 its left, -1 its right) for
	// a while once it chose it; how long nothing at all was free but
	// others in the way (it waits for them, then gives up)
	int avoid_side;
	float avoid_seconds;
	float crowd_seconds;
	// Backing out (meters left at most): its nose against something (a
	// trough, a fence) and its way behind it, it steps back clear before it
	// turns, not swinging its head through it
	float backing_meters;
	// Wandering (actors_wander: a mind's walk with no goal): it keeps
	// going the way wander_heading says at wander_share of its walk (below
	// 0: backing), turning to it at its turning speed, till told otherwise
	// ("a body in motion stays in motion"); no path, no waypoints, no
	// slowing down to arrive. A body or another just ahead stops its step
	// (the mind sees that and turns it)
	bool wandering;
	float wander_heading;
	float wander_share;
	float wander_hold_seconds;         // its step was blocked: it stands this long before it tries again (no stop-go every frame)
	// A straight walk that ends exactly (actors_walk_straight_exact: a
	// worker onto his place at the anvil): how near the end counts as there
	// (0: as any walk)
	float arrive_meters;
	// A step between standing and walking under way (ACTOR_GAIT_: its clip
	// moving it so far, that way, at that clip's pace; -1 none), how long it
	// has played, how far it has taken it; then turned by so much
	int gait;
	float gait_seconds;
	float gait_length_seconds;
	float gait_meters;
	float gait_done_meters;
	float gait_yaw;                    // the way it goes
	float gait_delay;                  // of its length, before it moves
	float gait_turn_after;             // its yaw turned so much at the end (its clip turned it)
	// Its walk, planned to end on a foot's landing just the stop's length
	// from the goal: the walk's stride stretched so much (1: as made), the
	// walk's phase last frame
	float stride_scale;
	float walk_phase_last;
	// Its way at the end of an exact walk (arrive_face: turned to it there,
	// straight from the walk); a move too short to walk: turned on the spot
	// to that way, stepping, sliding onto the point as it turns (sidestep)
	bool arrive_face;
	float arrive_yaw;
	bool sidestep;
	bool animation_held;               // this frame's clip chosen by its steps (choose_animation leaves it)
	bool start_held;                   // turning on the spot before its walk's start: not off yet
	float start_held_seconds;          // how long so (too long: off without its start, as any walk)
	float sidestep_x;
	float sidestep_z;
	bool body_fitted;                  // (a test's log: its body fitted last frame)
	// No way out of the water it is in (too fast to swim against): it keeps
	// its goal and tries again every second, until it can, or it is on dry
	// ground and still cannot (then it gives up)
	bool waiting;
	float wait_seconds;
	// Flying
	actor_flight_t flight;
	float cruise_height;               // above the ground, where it would like to be now
	float cruise_seconds;              // until it picks another height
	float ceiling_height;              // meters, absolute: never higher on this trip
	float vertical_speed;
	float pitch_radians;               // nose up
	float bank_radians;                // leaning into the turn, right side up
	float circle_floor_height;         // meters, absolute: the lowest it circles (over what is around)
	float circle_direction;            // 1: counterclockwise seen from above, -1: clockwise
	float land_x;                      // where it comes down
	float land_z;
	uint32_t random_state;
	bool running;                      // goes at its type's run speed (actors_set_running)
} actor_t;

typedef struct
{
	actor_type_t types[ACTOR_TYPES_MAX];
	int type_count;
	actor_model_t models[ACTOR_TYPES_MAX];
	int model_count;

	actor_t *actors;
	int count;
	// Poses are made only for what is drawn (ensure_pose): the frames
	// counted, and where the camera was the last time (how far away an
	// actor is: far ones posed every few frames)
	uint32_t pose_clock;
	vec3_t pose_eye;
	bool pose_eye_known;
	int capacity;
	uint32_t next_id;
	// Actors were added, removed or moved: the shadow maps must be redrawn
	bool shadow_dirty;

	SDL_GPUGraphicsPipeline *pipeline;          // back faces culled
	SDL_GPUGraphicsPipeline *two_sided_pipeline; // cut out textures (fur, feathers)
	SDL_GPUGraphicsPipeline *shadow_pipeline;
	int drawn_count;                   // last frame
	// Path finding tells the log what it does, per actor (debugging);
	// object type names come from here when set
	bool log_paths;
	const objects_t *objects;
	// The objects as bodies (set by the world): what stops a creature
	// walking straight (actors_walk_straight), its shapes as they are, not
	// the path finding's grid of meters
	struct physics_s *physics;
	actor_event_t events[ACTOR_EVENT_QUEUE_SIZE];
	int event_first;
	int event_count;
	// The actor drawn highlighted (0: none), and in which color (sRGB 0..1)
	uint32_t highlight_id;
	float highlight_color[3];
	bool pbr_shading;                  // render.pbr_shading (the game sets it before drawing)
} actors_t;

// Loads the shaders, data/actors.ini and the models it names. A missing
// actors.ini only means there are no actors.
bool actors_create(actors_t *actors, const settings_t *settings);
void actors_destroy(actors_t *actors);

// Index of the type with that name, -1 if there is none
int actors_find_type(const actors_t *actors, const char *name);

// Puts an actor of the type on the ground at (x, z). Returns its id, 0 if
// it could not be added.
uint32_t actors_add(actors_t *actors, const terrain_t *terrain, int type, float x, float z, float yaw_radians);
// Removes the actor nearest (x, z) within radius; false if there is none
bool actors_remove_nearest(actors_t *actors, float x, float z, float radius);
void actors_clear(actors_t *actors);

// Moves the actors along their paths, lets the water push them, keeps them
// on the ground (height and lean) or afloat. Every frame, before
// actors_update; seconds may be 0 (paused) and they are still placed.
void actors_move(actors_t *actors, navigation_t *navigation, const terrain_t *terrain, const water_t *water, float seconds);

// Sends the actor towards (x, z): the path is searched now. False if the
// actor is gone or cannot take a step that way (an ACTOR_EVENT_NO_PATH
// event is queued too). A flyer takes off if it is on the ground.
bool actors_move_to(actors_t *actors, navigation_t *navigation, const terrain_t *terrain, const water_t *water, uint32_t id,
	float x, float z);
// Stops where it is
void actors_stop(actors_t *actors, uint32_t id);
// Walks the points (x, z) in turn, straight, no search (see actor_t.straight);
// false if it cannot set out
bool actors_walk_straight(actors_t *actors, uint32_t id, const float (*points)[2], int count);
// The same, ending on the last point within a few centimeters (slowing,
// turning on the spot to it when it is not ahead)
bool actors_walk_straight_exact(actors_t *actors, uint32_t id, const float (*points)[2], int count, float face_yaw_radians);
// Wanders: goes on the way heading_radians says (0 = +z) at share of its
// walk (0: stands; below 0: backs, its walk backwards), till told
// otherwise or given a walk (see actor_t.wandering); false if it cannot
bool actors_wander(actors_t *actors, uint32_t id, float heading_radians, float share);
// Whether its way is free heading so, meters past its nose: no body (a
// fence, a feeder, a wall: the physics' shapes, along its middle and both
// its sides, at its feet and at its middle) and no other on the ground
// across that stretch, where it is or where it will be in half a second
bool actors_heading_free(const actors_t *actors, uint32_t id, float heading_radians, float meters);
// Takes it off the map; false if there was none
bool actors_remove(actors_t *actors, uint32_t id);
// Goes at its run speed (run_meters_per_second) or at its walk again
void actors_set_running(actors_t *actors, uint32_t id, bool running);
// Something hit the actor or fell across its way (for the physics, once
// actors have a body there): it searches its path again at the next move
void actors_mark_path_dirty(actors_t *actors, uint32_t id);
// The next event, false if there is none
bool actors_poll_event(actors_t *actors, actor_event_t *event_out);
// How the type gets around, for the navigation
navigation_agent_t actors_agent(const actors_t *actors, int type);
// The id of the actor nearest (x, z) within radius, 0 if none
uint32_t actors_nearest(const actors_t *actors, float x, float z, float radius);
// The id of the actor the ray passes nearest (through its middle, within
// 1.5 m or 2.5% of the distance, whichever is more: a bird far up is hard
// to hit), 0 if none
uint32_t actors_pick(const actors_t *actors, vec3_t origin, vec3_t direction);

// The actors whose box (the bind pose's, turned and scaled with them) the
// ray goes through before maximum_distance, nearest first; only_interactive:
// those the game can do something with. Ids and distances; returns the count
int actors_raycast(const actors_t *actors, vec3_t origin, vec3_t direction, float maximum_distance, bool only_interactive,
	uint32_t *ids, float *distances, int maximum_hits);

// A ray against the actors' bodies as they are posed now (each bone's box,
// actor_model.h), the nearest
typedef struct
{
	uint32_t id;
	float distance;                    // meters along the ray
	int bone;                          // in its model
	actor_zone_t zone;
	float point[3];
	float normal[3];                   // of the box's face it went in by
} actor_hit_t;

// direction normalized; false: none within maximum_distance
bool actors_hit_test(const actors_t *actors, vec3_t origin, vec3_t direction, float maximum_distance, actor_hit_t *hit_out);

// The game decided it dies (the hits only report the damage: what it does
// is the game's): it stops where it is and plays its first action named
// die... (a flyer drops). False if it is gone or already dying
bool actors_kill(actors_t *actors, uint32_t id);
bool actors_is_dead(const actors_t *actors, uint32_t id);
// It flinches (the game's say): its first action named pain..., then back
// to what it did. Nothing for the dead
bool actors_hurt(actors_t *actors, uint32_t id);
// Dead already, lying as its dying ends (a map being loaded)
void actors_set_dead(actors_t *actors, uint32_t id);

// The game's number for it (0: none); the actor with that number (its id,
// 0 if there is none)
void actors_set_game_id(actors_t *actors, uint32_t id, uint32_t game_id);
uint32_t actors_find_by_game_id(const actors_t *actors, uint32_t game_id);

// The game's name for it (NULL or empty: none) and whether it is an enemy
void actors_set_name(actors_t *actors, uint32_t id, const char *name, bool hostile);
// While it stands it turns to look at (x, z) (a talker at the player);
// actors_stop_looking lets it be
void actors_look_at(actors_t *actors, uint32_t id, float x, float z);
void actors_stop_looking(actors_t *actors, uint32_t id);

// Advances the animations by that much time and computes the poses
void actors_update(actors_t *actors, float seconds);

// Index of the actor with that id in actors->actors, -1 if it is gone
int actors_find(const actors_t *actors, uint32_t id);
// Starts an action of the actor's model (crossfading); false if the model
// has none of that name. Asking for the action already playing changes
// nothing.
bool actors_play(actors_t *actors, uint32_t id, const char *action_name);
// Debug: every actor to its model's next (step 1) or previous (-1)
// action. Returns the first actor's new action name, NULL without actors.
const char *actors_cycle_actions(actors_t *actors, int step);

// True if an actor stands within radius of (x, z): its shadow there moves
bool actors_any_near(const actors_t *actors, float x, float z, float radius);

// In the scene's pass (its scene block already pushed)
void actors_draw(actors_t *actors, SDL_GPURenderPass *pass, const camera_t *camera, const shadow_map_t *shadow,
	const shadow_map_t *near_shadow, SDL_GPUTexture *variation_texture, float time_seconds);
// Every actor the map's square holds marked as moving (the near map's
// moving layer: shadow_map_mark_moving), by the box it is drawn within
void actors_mark_shadow(const actors_t *actors, shadow_map_t *shadow);
// Into a shadow map's pass (between shadow_map_begin and shadow_map_end)
void actors_render_shadow(actors_t *actors, SDL_GPURenderPass *pass, const shadow_map_t *shadow);

#endif
