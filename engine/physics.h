// Physics: the objects as rigid bodies (Box3D), pushed around by the water.
//
// Box3D knows nothing about our water. The water acts on bodies as forces,
// computed here every simulation step from the water grid:
//   - buoyancy (Archimedes): each body has a few sample points spread over
//     its shape, each standing for a share of the volume the body displaces
//     (its mass over its density). A point under the surface pushes up with
//     the weight of the water its share displaces, at that point, so long
//     things roll and tilt the way they float;
//   - drag: the water flowing past a point pushes it along, with the square
//     of the speed between the water and the point.
// What floats and what sinks follows from the density alone.
//
// Which objects become bodies is up to objects.ini (physics = static or
// body, see object_types.h). Bodies are made for the objects on the map and
// follow the object list: objects added or removed get or lose their body.
// Anchored objects (trees) are static bodies until the water or a hit
// pulls harder than their anchor; then they come loose, tip over if they
// are trees, and drift. Loose objects that stop moving go to sleep and cost
// next to nothing; a hit or flowing water wakes them again. A loose object
// is drawn where its body is (see OBJECT_STATE_FREE in objects.h); one that
// leaves the map is removed.
//
// Static objects that block water (houses, stone walls) are written into the
// water simulation as walls (water_t.obstacle_centimeters).
//
// Breakable objects (damage_newtons) take damage from the water and from
// hits: intact, damaged, destroyed, each with its own model if objects.ini
// names one, throwing out debris when destroyed.
//
// Objects with physics = flood have no body: water flowing fast enough lays
// them down in its direction (or tears them out); they stand up again once
// it is gone.
//
// The ground is a set of Box3D height fields, one per terrain chunk, made
// only where bodies move (most of a big map never needs one). Tools and
// stamps redo them at once; erosion only once it has changed the ground
// noticeably (terrain_resync_meters), checked now and then.

#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdbool.h>
#include <stdint.h>

#include "box3d/box3d.h"
#include "objects.h"
#include "settings.h"
#include "terrain.h"
#include "water.h"

#define PHYSICS_SAMPLES_MAX 27

// Collision data of an object type, made the first time a body needs it
typedef struct
{
	bool built;
	bool usable;                   // false: no model or no shape could be made
	object_shape_t shape;          // never OBJECT_SHAPE_AUTO
	b3HullData *hull;              // at size 1, in model space
	b3MeshData *mesh;
	// The model's triangles, for its body while it stands (static and
	// anchored ones: a house's walls, a tree with its branches); made the
	// first time one needs it. Once torn loose it moves with the shape above
	b3MeshData *standing_mesh;
	bool standing_mesh_tried;
	b3Capsule capsule;
	b3Sphere sphere;
	float volume;                  // of the shape, at size 1
	float sample_points[PHYSICS_SAMPLES_MAX][3];
	int sample_count;
	float sample_height;           // vertical reach of one sample point
	// A small one (largest_extent times its size under the settings'
	// small_body_meters) is pushed by the water at one point only: its
	// middle, its whole height
	float center_point[3];
	float whole_height;
	float largest_extent;
	float top;                     // highest point of the model (for tipping trees over)
	float bottom;
	float footprint_half_width_x;  // the model's box, for the water walls
	float footprint_half_width_z;
	float footprint_center_x;
	float footprint_center_z;
	float height;
} physics_type_t;

typedef enum
{
	PHYSICS_BODY_STATIC = 0,       // never moves
	PHYSICS_BODY_ANCHORED,         // static until torn loose
	PHYSICS_BODY_DYNAMIC
} physics_body_kind_t;

typedef struct
{
	bool used;
	b3BodyId id;
	int instance;                  // its object (index into objects_t.instances)
	float x;                       // where it is (the object's position, kept here too)
	float z;
	uint32_t mark;                 // for finding bodies whose object is gone
	physics_body_kind_t kind;
	float displaced_volume;        // cubic meters under water when fully submerged
	float mass;
	float anchor_newtons;
	float damage_newtons;
	double damage_ready_seconds;   // one step of damage at a time: none again before this (physics_t.clock_seconds)
	// Pulled out of the ground again and again (the water pressing it back
	// in): how many times lately, and until when that counts
	int rescues;
	double rescue_until_seconds;
	// Its place in the lists the water's push goes through (not every body
	// every step): the loose ones' list (bucket -1), the anchored or
	// breakable ones' bucket of their ground tile (0..), none (-2: static
	// and unbreakable, the water does nothing to it)
	int list_bucket;
	int list_previous;
	int list_next;
	uint32_t visit_stamp;          // looked at by the water's push this step already (physics_t.visit_stamp)
	float scale;
	bool awake;                    // as far as the move events tell
	// Woken by the water's flow: where it lay and how fast the water was;
	// asleep again where it lay, it is stuck (wedged, in a hollow), and
	// only a flow a good deal faster than stuck_flow wakes it again (0:
	// not stuck). Else it was woken every step, never moving
	float woken_at[3];
	float woken_flow;              // 0: not woken by the flow
	float stuck_flow;
	bool in_ice;                   // under the ice that bears when it came: held in it (disabled) until it goes
	// Where the last two steps left it (x, y, z, then the rotation x, y, z,
	// w): its object is drawn in between (see physics_interpolate)
	float pose_previous[7];
	float pose_current[7];
	bool pose_known;
	// What the water did to it the last time it was looked at (for the
	// inspector)
	float water_submerged;         // fraction under water
	float water_buoyancy_newtons;
	float water_drag_newtons;      // horizontal
	float water_flow_meters_per_second; // fastest flow at its points
} physics_body_t;

typedef struct
{
	b3HeightFieldData *field;
	b3ShapeId shape;
	bool exists;
} physics_tile_t;

struct wrecks_s;

typedef struct physics_s
{
	bool created;
	b3WorldId world;
	// The breakable buildings in the world (wrecks.h; NULL: none): told
	// round each step of it, their bodies none of the physics' own
	struct wrecks_s *wrecks;
	// What stopped or pushed the walker in its last move (the debug window)
	int walker_planes_kept;        // planes that pushed it
	int walker_planes_skipped;     // planes let be (level, or leaning at head height)
	float walker_push_x;           // how far they pushed it, all told (meters)
	float walker_push_z;
	float walker_worst_normal[3];  // the plane that pushed most: its normal (as box3d gave it)
	float walker_worst_height;     //   and the point touched, over the feet
	float walker_worst_depth;      //   and how deep it was in
	b3BodyId ground;               // the static body all ground tiles hang on

	// Ground tiles, one per terrain chunk
	physics_tile_t *tiles;
	int tiles_per_side;
	float tile_meters;
	float minimum_height;          // quantization range of the height fields
	float maximum_height;
	// The ice that bears (water_t.iced): the ground tiles are its surface
	// where there is water, and the water pushes nothing
	const water_t *ice_water;
	int tile_count_made;
	float terrain_check_timer;
	int resync_cursor;                 // the erosion's check goes on from this tile (it does a few a step)
	bool resync_unfinished;            // tiles left to check: the next step goes on with them

	physics_type_t *types;         // one per object type
	int type_count;

	physics_body_t *bodies;
	int body_capacity;
	int body_count;                // in use
	int *free_slots;
	int free_slot_count;

	uint32_t synced_generation;    // objects_t.generation at the last sync
	uint32_t mark;
	bool needs_full_sync;
	// Bodies are made near the focus (physics_set_focus) at once, the rest
	// a few milliseconds a step (a loaded map's thousands of them would
	// hold one frame up for long): how many so far, since when
	float focus_x;
	float focus_z;
	bool focus_set;
	int staggered_made;
	uint64_t staggered_start;
	// Bodies still to make after a sync ran out of time: gone on with a
	// slice of the objects a step (bodies_cursor: the next one), not a
	// pass over every object every step (millions on a big map)
	bool bodies_pending;
	int bodies_cursor;
	// Bodies are made a grid cell of the objects at a time (objects.h
	// OBJECTS_GRID_CELL_METERS), when something wants them there: the focus
	// near (BODIES_RANGE_METERS), the water moving in it, an actor in it
	// (physics_request_bodies); once made, a cell's stay. Per cell: 0 not
	// yet, 1 waiting in the queue, 2 made. The queue is made a little a step
	// (the next cell's objects from queue_cursor on)
	uint8_t *cell_states;
	int cell_grid_size;
	int *cell_queue;
	int queue_head;
	int queue_count;
	int queue_capacity;
	int queue_cursor;              // -1: the head cell not begun
	int worker_count;
	uint32_t random_state;

	// Water walls: rebuilt when the set of walls changes; the cells they
	// covered last time (to wake the water there when they go)
	uint64_t walls_hash;
	bool walls_dirty;
	int32_t *wall_rectangles;      // 4 per wall: inclusive cell rectangle
	int wall_count;
	int wall_capacity;

	// The bodies that moved in the last step: their objects are drawn
	// between where they were and where they are
	int *moved_slots;
	int moved_count;
	int moved_capacity;

	uint8_t *remove;               // per object: to be removed at the end of the step
	int remove_capacity;
	float flood_timer;
	int flattened_count;           // objects laid down by the water
	float recover_timer;
	// Their ids (flattened_count of them): stood up again by going through
	// these, not every object
	uint32_t *flattened_ids;
	int flattened_capacity;
	// The bodies the water may push, listed per ground tile
	// (physics_body_t.list_bucket): the loose ones (moved to their new tile
	// as they go), the anchored and the breakable ones; the step goes through
	// the tiles where the water moves and the bodies that moved the step
	// before (awake_slots: those awake on dry land), not every loose body
	int *bucket_heads;             // tiles_per_side squared, -1: none
	int *awake_slots;
	int awake_count;
	int awake_capacity;
	uint32_t visit_stamp;
	double clock_seconds;          // the steps' time added up
	uint32_t change_serial;        // the object list's changes heard of so far (objects_changes_since)
	int *visit_slots;              // scratch: the bodies the water's push looks at this step
	int visit_capacity;
	int visit_count_this_step;

	// Statistics
	int awake_body_count;
	int dynamic_body_count;
	int static_body_count;
	double step_milliseconds;
	double world_step_milliseconds; // of that, Box3D's own step
	int uprooted_total;
	int removed_total;
} physics_t;

bool physics_create(physics_t *physics, const objects_t *objects, const physics_settings_t *settings);
void physics_destroy(physics_t *physics);

// A new map (or one of another size): every body and ground tile goes;
// the bodies come back from the objects at the next step
void physics_reset(physics_t *physics, objects_t *objects, terrain_t *terrain);

// The walker (first person) as a capsule (feet at feet_height) moving by
// (move_x, move_z) from (*x, *z): what stands in the way (houses, trees,
// rocks, heavy loose things, by their real shapes) stops it, and it slides
// along; things lower than step_meters it steps over (the ground is the
// caller's). *x and *z get where it ends up
void physics_move_walker(physics_t *physics, float *x, float *z, float feet_height, float move_x, float move_z, float step_meters);

// The walker (first person) walks into loose bodies (barrels, logs, boats,
// small stones): each one its capsule (feet at feet_height, velocity_x and
// velocity_z in meters per second) touches gets pushed along, as much as a
// walker of walker_kilograms can push something that heavy. Anchored and
// static ones are left alone (the walker does not get through them, see
// the navigation)
// Where the player is (the camera's target, or the walker): bodies are made
// there first
void physics_set_focus(physics_t *physics, float x, float z);

void physics_push_with_walker(physics_t *physics, float x, float feet_height, float z, float velocity_x, float velocity_z,
	float walker_kilograms);

// The objects of the bodies that moved in the last step, drawn amount (0
// to 1) of the way from where the step before left them to where the last
// step did: the physics runs at a fixed rate (30 steps a second), frames
// come faster, and without this a moving barrel jumps every few frames.
// Every frame, before drawing; the next step puts them back where they are
void physics_interpolate(physics_t *physics, objects_t *objects, float amount);

// The bodies awake now, logged (the console's awake): how many of each
// type, and the first few with where they are, how fast they move and
// what the water does to them (for finding what keeps bodies from sleeping)
void physics_log_awake(const physics_t *physics, const objects_t *objects, int listed_most);

// One simulation step: bodies follow the object list, the water pushes
// them, Box3D moves them, objects follow their bodies. objects may get
// objects removed (gone off the map, destroyed, washed away).
void physics_step(physics_t *physics, objects_t *objects, terrain_t *terrain, water_t *water, const physics_settings_t *settings,
	float step_seconds);

// What a straight line from one point to another meets first (the
// projectiles: projectiles.h): the ground's tiles or a body (its object)
typedef struct
{
	float point[3];
	float normal[3];
	float fraction;                // of the way from the start
	int body;                      // index into physics_t.bodies (-1: the ground)
	int instance;                  // its object (-1: the ground)
} physics_cast_t;

bool physics_cast_segment(physics_t *physics, const float from[3], const float to[3], physics_cast_t *hit_out);

// An object's body gone at once (the editor takes it up: objects_t.carried_id
// keeps it without one until it is put down)
void physics_drop_body(physics_t *physics, objects_t *objects, int instance_index);

// The bodies of the objects within radius of (x, z) wanted (an actor walks
// there, a game's script): made soon (a few milliseconds a step), and kept
void physics_request_bodies(physics_t *physics, const objects_t *objects, float x, float z, float radius);
#define PHYSICS_ACTOR_BODIES_METERS 48.0f // round each actor (its feet stand on them, its straight walks stop at them)

// What the walker's feet stand on at (x, z): the highest top of a solid
// object (one level enough to stand on) under a step over the feet's last
// height, under its middle or the edge of its foot; false where there is
// none (the terrain then)
// around: the four rays round the middle too (a foot, the player); else
// the one (a body's sample)
bool physics_walker_ground(physics_t *physics, float x, float z, float feet_height, float step_meters, bool around, float *height_out);

// A push (newton seconds) on a body at a point (a loose one moves; one held
// fast, nothing)
void physics_push_body(physics_t *physics, int body, const float impulse[3], const float point[3]);
// A body made this heavy (its spin keeps its shape: the inertia scaled
// alike); for a thing whose model is another's made small (a grenade)
void physics_set_body_mass(physics_t *physics, int body, float mass_kilograms);

// A body's velocity now (meters a second; zeros if it is none)
void physics_body_velocity(const physics_t *physics, int body, float velocity_out[3]);

#endif
