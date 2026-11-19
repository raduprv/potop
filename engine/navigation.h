// Navigation: finding paths for actors over the changing map.
//
// The map is seen through a grid of 1 m cells. A cell has one or more
// surfaces an actor may stand on: the terrain, and the tops of big objects
// (a bridge's deck, a dock, a flat rock), each with the free height above
// it up to the next solid thing. So a bear can cross a canyon on the bridge
// and a duck can swim under it, while the bear, too tall, cannot.
//
// The surfaces come from the big solid objects' collision triangles (what
// the physics uses, see object_model.h), turned and scaled as placed, and
// sampled every eighth of a meter: upward facing triangles give surfaces,
// the rest only take room. A surface keeps its height per quarter meter of
// the cell too (where a foot stands: beside a rock is the ground, not the
// rock's top), and one covering less than a quarter of its cell (a cone's
// tip) is no surface: the cell is in the way. Only the lowest NAVIGATION_OBJECT_HEIGHT_METERS of
// an object count (a tree's crown is no obstacle, its trunk is). Small
// objects (under [simulation] navigation_object_meters across, 0.3 m by
// default) are left out: actors walk through pebbles.
//
// What an object is to those that walk (object_kind): a wall, in the
// surfaces and in the rectangles (a module's parts; an object on its own
// whose footprint's longest reach is over navigation_wall_meters: a
// fence's segment is thin but long); a small thing gone round (a trunk, a
// boulder): in the surfaces still, for the walker's guard and its fine
// way, and, with [simulation] navigation_small_out, not in the rectangles,
// which lie through it (the cells only small things stand in are open
// ground to them: a forest is a few rectangles, not one per tree): a way
// found is led round the things it crosses (mesh_lead_round), and things
// too near one another for a body to pass between are a wall to its
// class (mesh_weld_small); or a soft one (a body lighter than
// navigation_soft_kilograms, a bucket): neither surface nor room taken,
// only a height per cell: each actor steps over, goes around (at a price,
// through when it must) or is stopped by it, by its own size. Small and
// soft things are kept as capsules too (navigation_capsule_t, fitted once
// per model from its collision triangles, placed in the world per
// region): what a way is led round.
//
// Whether an actor can be on a surface, and what it costs, depends on the
// actor (an agent: how tall it is, how high it steps, how steep it climbs,
// how deep it wades, whether it swims) and is worked out when needed, with
// the water as it is at that moment.
//
// A* runs over the surfaces (8 directions; a neighbor's surface is
// reachable if within a step, or within the slope the actor climbs; no
// cutting the corner of a blocked cell), with a cap on the surfaces it may
// expand: past it, or when the goal cannot be reached, the path leads to
// the reached cell nearest the goal. The path is then pulled tight into a
// few waypoints: straight lines wherever the ground along them is no worse.
//
// What changes: the map is split into regions of 64 m. A region is built
// again when the objects in it changed (moved, added, removed, broken; a
// standing one's height counts in steps of 15 cm), and is then stamped with
// a new generation number. The terrain moving alone (the water wearing it)
// builds nothing: the cells that moved more than 15 cm take their new
// height (the objects over them as they were), and the region is stamped
// newer only then. It is built lazily: only when a
// search is about to go through it (the regions along the straight line
// towards the goal and one either side), or an actor checks the stretch of
// path ahead of it. Erosion far from anybody costs nothing. An actor checks
// the stretch of path it can see when a region along it is newer than its
// last look, or has running water; only if the way there is actually
// blocked does it search again.
//
// Flyers (a falcon) see the map differently: per cell, the highest thing
// there, terrain or any standing object whole (a tree's crown too, from
// its box). They go anywhere under their ceiling; a flight path is the
// straight line when nothing on it rises above the ceiling, else A* over
// blocks of 4 x 4 cells around what does (a mountain), climbing costing a
// little. How high they fly along the way is theirs (see actors.h).
//
// The navigation mesh: where a walker's body fits, gathered into
// rectangles its searches go over. It is built from the quarter meters
// the objects are sampled at, not from the cells: each quarter meter of a
// cell has its levels (the ground, the cell's objects' tops, each with
// its own room above), and a level of a quarter is in the mesh of a class
// of agents where a body of the class can have its middle there: it
// stands there, and every point round it rests on something within a
// step (the points the walkers' own guard measures a body by,
// navigation_quarter_tightness). So the mesh is the space of the bodies'
// middles: by a wall it stops where the body would be in the wall, a gap
// is in it if the body gets through, a terrace's floor ends short of its
// open edge, and the 75 cm of ground beside a porch are ground. A class
// is its agents' width in quarter meters (a hen 1, a man 2, a cow or a
// bear 3 and more); a body an odd number of quarters wide is tried with
// its middle in a quarter's middle, an even one where four quarters meet
// (a man in a doorway two quarters wide), so an even class's rectangles
// lie half a quarter low of the quarter meters. Per region (a tile) and
// class, the quarters are merged greedily into rectangles (up to
// NAVIGATION_MESH_RECT_MOST of them a side) that are alike (the room
// above, the blockers' groups, a small object, the slope's cost, the
// ground or an object's top) with every step between neighbors one the
// whole class takes (the ground: its slope's reach). Cells with water at
// the build stay rectangles within their cell (looked at with the water
// as it is); the tile is built again when its water's cells change, when
// its region or one round it is (it looks a cell into them), or when an
// agent of a lesser reach or step (or a lower or a wider one) joins the
// class. A rectangle (20 bytes) keeps its corners, its height (and how
// far its highest is over it: 0 for a flat top), the room above, the
// slope's cost and where its neighbors are listed: the rectangles a body
// gets to from it, within the tile and across to the next (linked when
// either is built). Where it crosses to one is not kept: the part of
// their sides they share follows from their corners, the step from
// their heights there, and whether an agent takes it is the agent's
// (an object's top a step, the ground its slope: mesh_crossing). Where
// two rectangles more than a step apart have only quarters between them
// that are neither's (the ground by a terrace's face, the terrace's own
// edge) they are neighbors over a gap: an edge to jump, for an agent
// that jumps (mesh_jump finds where it leaves the ground and where it
// lands). Which rectangle a point is on is found among the tile's (by
// buckets of 4 m), not in a table per cell. A* over the rectangles, its
// notes on them in a small table of its own (the agent's own
// rules for the room, the groups, small objects and the water, at the
// time), then the corridor pulled tight (the funnel) through the middles
// of the portals' end quarters. A goal where no body's middle can be (by
// a wall, by a table) is taken to the nearest quarter that is level with
// it. The doors are links between the rectangles nearest their ends. The
// straight line (navigation_line_passable) walks the rectangles, looking
// only where it crosses from one to the next; off them (a walker by a
// wall, a line's end by a table) it goes over the quarter meters, for a
// meter at each end at most.
//
// The search's working memory belongs to the navigation and is reused
// between searches without clearing (nodes carry the number of the search
// that last touched them). Searches only read the map; for searches on
// several threads, each would get working memory of its own.

#ifndef NAVIGATION_H
#define NAVIGATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "objects.h"
#include "terrain.h"
#include "water.h"

#define NAVIGATION_CELL_METERS 1.0f
#define NAVIGATION_REGION_METERS 64.0f
#define NAVIGATION_MAX_WAYPOINTS 96
#define NAVIGATION_SLOPE_UNIT_DEGREES (90.0f / 255.0f)
#define NAVIGATION_MAX_EXTRA_SURFACES 3       // per cell, above the terrain's
#define NAVIGATION_REGION_EXTRA_SLOTS 4096    // extra surfaces per region
#define NAVIGATION_OBJECT_HEIGHT_METERS 4.0f  // the part of an object that can be in the way
#define NAVIGATION_CLEARANCE_UNIT_METERS 0.05f
#define NAVIGATION_CLEARANCE_OPEN 255         // nothing above, as far as anyone cares
#define NAVIGATION_OBJECT_TOP_UNIT_METERS 0.25f // object heights for flying, up to 63.75 m

// How an actor gets around (from its type, see actors.ini)
typedef struct
{
	float height_meters;               // needs this much free room above a surface
	float step_meters;                 // climbs this much from one cell to the next, however steep
	float maximum_slope;               // rise per meter it can still climb
	float wade_depth_meters;           // deeper than this slows it down
	float swim_depth_meters;           // deeper than this it swims (or cannot pass)
	bool can_swim;
	float swim_meters_per_second;      // a current faster than this is a wall
	float radius_meters;               // half its width: straight stretches of a path keep this far from what is in the way
	uint8_t groups;                    // bits: a blocker for one of these stops it (navigation_set_blockers; 0: none does); NAVIGATION_GROUP_SHUT_DOOR its own
	bool opens_doors;                  // goes through a shut door (navigation_set_links): it opens it (a person, not a cow)
	bool line_center_only;             // navigation_line_passable: its center's line, not its sides' too
	// Up onto and down off what it cannot step: a terrace's edge. A search
	// takes such an edge where the way round is longer (a stair a few
	// meters off: the stair), where it can stand on both sides of it and
	// nothing stands on it (a balustrade); the path's waypoint beyond it is
	// marked (navigation_path_jump). 0: it does not
	float jump_up_meters;
	float jump_down_meters;
} navigation_agent_t;

// A door: a way from one cell to another through what is in the way (a
// gate in a fence, a door in a wall, narrower than a cell: the cells by
// it are taken). Its two ends a little out either side of it; open, any
// walker goes through; shut, only one that opens doors. The world keeps
// them (the modules' doors: world_render.c)
enum
{
	NAVIGATION_LINKS_MAX = 512
};

typedef struct
{
	float a[2];                        // one end (x, z): out on one side of the door
	float b[2];                        // the other, out on the other side
	bool open;
} navigation_link_t;

// The blockers' groups' last bit is the doors': a shut door's doorway (the
// cells between its link's two ends) stops an agent with it in its groups,
// one that does not open doors (actors.c gives it them); the others go
// through, or by the link
#define NAVIGATION_GROUP_SHUT_DOOR 0x80u

// A blocker: a rectangle no one of its groups goes through (a locked
// gate, a fallen tree, a ward), whatever is there. The game sets them
// (the map's areas that block: areas.h); flyers go over them
enum
{
	NAVIGATION_BLOCKERS_MAX = 256
};

typedef struct
{
	float x;                           // its middle, meters
	float z;
	float half_width_meters;
	float half_depth_meters;
	float yaw_radians;                 // as areas.h's
	uint8_t groups;                    // bits: whom it stops (navigation_agent_t.groups)
} navigation_blocker_t;

typedef enum
{
	NAVIGATION_PATH_NONE = 0,          // not even a step could be made
	NAVIGATION_PATH_FOUND,             // to the goal
	NAVIGATION_PATH_PARTIAL            // as near the goal as it could get
} navigation_result_t;

// A surface above the terrain's
#define NAVIGATION_SUBCELLS 4                  // per side of a cell, for exact heights (feet)
#define NAVIGATION_SUBCELL_UNIT_METERS 0.02f
#define NAVIGATION_SUBCELL_NONE 255            // the surface does not reach this part of the cell
#define NAVIGATION_SUBCELL_BLOCKED 254         // nor does it, and something stands there at its height (a wall through the cell)

typedef struct
{
	float height;                      // meters: the highest of it in the cell (what paths go by)
	int32_t cell;
	uint8_t clearance;                 // free room above, NAVIGATION_CLEARANCE_UNIT_METERS each
	// Per quarter meter of the cell (row by row, x first): how far under
	// height it is there, NAVIGATION_SUBCELL_UNIT_METERS each, or
	// NAVIGATION_SUBCELL_NONE where it is not (a rock's top covers part of
	// a cell): where a foot stands
	uint8_t below[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
	// And the room above it there (as clearance; 0 where it is not): an
	// agent too tall for a quarter's room takes it for a wall (under a
	// balustrade's rail)
	uint8_t room[NAVIGATION_SUBCELLS * NAVIGATION_SUBCELLS];
	// The ground's own quarters in it, where something stands on or over
	// only part of its cell (the 75 cm beside a porch's end, the strip
	// along a wall): their below value (the ground's height under its
	// own), NAVIGATION_SUBCELL_NONE if it has none. The cell's ground as a
	// whole is taken then (terrain_clearance 0): this is where it is free
	uint8_t ground_below;
} navigation_surface_t;

// A node waiting in the search's open list, and its priority (the cost so
// far and the estimate of the rest): side by side, a heap's level in few
// cache lines
typedef struct
{
	float priority;
	int32_t node;
} navigation_heap_entry_t;

// The navigation mesh (see the top)
#define NAVIGATION_MESH_CLASSES 3               // by width in quarter meters: 1 (a hen), 2 (a man), 3 and more (a cow, a bear)
#define NAVIGATION_MESH_RECT_MOST 64            // quarter meters a side of a rectangle at most

// Where one rectangle leads into the next: the part of their sides they
// share. Not kept: worked out from the two rectangles when a search or a
// line gets there (mesh_crossing)
typedef struct
{
	int32_t from;                      // the rectangle it leaves (its tile * 65536 + its index)
	int32_t to;                        // the one it enters
	float a[2];                        // its ends (x, z, meters), on the side of the one it leaves
	float b[2];
	float rise;                        // the step across it where it is crossed (meters)
	bool cross_surface;                // an object's top on either side (a step only, no slope: that is the ground's)
	// Quarter meters between the two that are neither's (where a body's
	// middle cannot be: by a terrace's face below, on its edge above): not
	// walked through, only an edge to jump (navigation_agent_t's jumps). 0:
	// they touch
	uint8_t gap;
} navigation_portal_t;

// A rectangle of the mesh: quarter meters a body's middle can be on, all
// alike. 20 bytes; a search's own notes on it are the search's
// (navigation_mesh_visit_t)
#define NAVIGATION_RECT_GROUND 1                // the ground (its heights the terrain's); else an object's top
#define NAVIGATION_RECT_WET 2                   // a cell with water at the build: looked at with the water as it is
#define NAVIGATION_RECT_ZONE 4                  // a cell under a blocker or in a doorway: whom it stops is asked of the cell as it is
#define NAVIGATION_RECT_SPAN_UNIT_METERS 0.05f
#define NAVIGATION_MESH_TILE_RECTS_MOST 0x3FFF  // (a neighbor is 14 bits of index and its side)

typedef struct
{
	uint8_t x;                         // its first quarter meter within its tile (x, z)
	uint8_t z;
	uint8_t width;                     // quarter meters
	uint8_t depth;
	float height;                      // its surface's, where it is lowest
	uint8_t height_span;               // how much higher its highest is (NAVIGATION_RECT_SPAN_UNIT_METERS each; 0: flat)
	uint8_t clearance;                 // the least room above (NAVIGATION_CLEARANCE_UNIT_METERS each, or OPEN)
	uint8_t flags;                     // NAVIGATION_RECT_*
	uint8_t cost;                      // what a meter on it costs over 1 (its slope), in 32nds
	uint8_t unused;
	uint8_t small;                     // small or soft things stand in it: a way through it looks out for them
	uint16_t neighbor_count;
	uint32_t first_neighbor;           // in its tile's neighbors
} navigation_rect_t;

// A rectangle a search has got to: the search's notes on it. They live in
// a table of the navigation's, found by the rectangle, reused from search
// to search (one of another search's is as good as none)
typedef struct
{
	int32_t rect;
	uint32_t search;                   // the search that wrote it
	float cost;                        // so far
	int32_t came_from;                 // -1: the start
	float entry[2];                    // where the search came in
	float came_a[2];                   // the crossing it came through (or a door's two ends)
	float came_b[2];
	float came_height;                 // (a jump: the height it lands at)
	bool closed;                       // done with
	bool came_by_door;
	bool came_by_jump;                 // over an edge it jumps: came_a where it leaves the ground, came_b where it lands
} navigation_mesh_visit_t;

// A rectangle's neighbor in the next tile: a border's entry
typedef struct
{
	uint16_t from;                     // the rectangle in this tile
	uint16_t to;                       // the one in the tile over that side
} navigation_border_t;

#define NAVIGATION_MESH_BUCKETS 16              // a side: a tile's rectangles are found by the bucket of 4 m they reach into

// A run of quarter meters along x by a tile's rectangles where there is
// a surface but a body of the class does not fit with its middle there:
// by a wall, on a terrace's edge, at its foot (one stands there), under
// a table (not even that). Not walked on; what a goal there is level
// with and what an edge to jump is made of are asked of these
#define NAVIGATION_FRINGE_SURFACE 0   // a surface with no room for the class over it (a floor under a table)
#define NAVIGATION_FRINGE_STAND 1     // one stands there
#define NAVIGATION_FRINGE_POINT 2     // the ground where its cell's room is taken but nothing is in the way there: a foot rests on it

typedef struct
{
	uint8_t x;                         // its first quarter meter within its tile's row
	uint8_t count;                     // quarter meters
	uint8_t ground;                    // the ground's (else an object's top)
	uint8_t stand;                     // NAVIGATION_FRINGE_*
	float height;
} navigation_fringe_t;

typedef struct
{
	bool built;
	uint32_t generation;               // its region's at the build
	uint32_t margin_signature;         // the regions round it at the build (a cell of each is looked into)
	uint32_t class_epoch;              // its class's at the build
	uint64_t water_signature;          // its water's cells at the build
	uint64_t water_checked_ticks;      // when they were last compared (SDL_GetTicks)
	uint32_t checked_search;           // the search that last found it current
	uint32_t build_serial;             // which build of all the tiles' this is (a shut portal's rectangles are this build's)
	navigation_rect_t *rects;
	int32_t rect_count;
	int32_t rect_capacity;
	// Each rectangle's valid neighbors within the tile, together: its
	// index (14 bits) and the side of the rectangle it is over (the 2 above:
	// 0: +x, 1: -x, 2: +z, 3: -z). Only ones a body gets to: the next one
	// along, or one more than a step off over quarters that are neither's
	// (an edge to jump)
	uint16_t *neighbors;
	int32_t neighbor_count;
	int32_t neighbor_capacity;
	navigation_border_t *border[4];    // to the next tile (+x, -x, +z, -z)
	int32_t border_count[4];
	int32_t border_capacity[4];
	// Which rectangles reach into each bucket: bucket_rects from
	// bucket_first[b] to bucket_first[b + 1]
	uint32_t bucket_first[NAVIGATION_MESH_BUCKETS * NAVIGATION_MESH_BUCKETS + 1];
	uint16_t *bucket_rects;
	int32_t bucket_capacity;
	// The heights of its uneven tops (a ramp, treads taken in a stride:
	// height_span over 0, not the ground), a quarter meter each, row by
	// row: uneven_rects are those rectangles, in their order, each one's
	// heights from uneven_first on
	uint16_t *uneven_rects;
	uint32_t *uneven_first;
	int32_t uneven_count;
	int32_t uneven_capacity;
	int32_t uneven_first_capacity;
	float *uneven_heights;
	int32_t uneven_height_count;
	int32_t uneven_height_capacity;
	// Where one stands but a body does not fit (navigation_fringe_t), row
	// by row of quarter meters: a row's from fringe_row_first[z] to
	// fringe_row_first[z + 1] (NULL: the tile has none)
	int32_t wet_count;                 // its rectangles with water at the build (their tile is built again as the water moves)
	navigation_fringe_t *fringe;
	int32_t fringe_count;
	int32_t fringe_capacity;
	uint32_t *fringe_row_first;
} navigation_mesh_tile_t;

typedef struct
{
	float reach;                       // the least step (or slope over a cell) of its agents seen
	float height;                      // the lowest of them
	float step;                        // the least step of them (onto an object's top)
	float step_most;                   // and the highest (what a body's points rest on round its middle)
	float radius;                      // the widest of them (half its width): where its body's middle can be
	int32_t width_quarters;            // its agents' width in quarter meters (the class's own: its index + 1)
	uint32_t epoch;                    // a change of the above: its tiles built again
	navigation_mesh_tile_t *tiles;     // per region
} navigation_mesh_class_t;

// A portal shut to an agent: a way a search found through it did not
// hold over the quarter meters for it (mesh_way_holds: a stair's treads
// higher than its step, a beam lower than it is tall, inside cells the
// rectangles take whole). Its searches leave the portal out, till either
// rectangle's tile is built again or a while has gone by
#define NAVIGATION_MESH_SHUT_MOST 32

typedef struct
{
	int32_t from;                      // the rectangles (of the agent's class)
	int32_t to;
	uint32_t from_serial;              // their tiles' builds
	uint32_t to_serial;
	uint8_t class;
	float step;                        // the agent's: one alike is kept out
	float height;
	float radius;
	uint64_t until_ticks;              // SDL_GetTicks
} navigation_mesh_shut_t;

typedef struct
{
	navigation_mesh_class_t classes[NAVIGATION_MESH_CLASSES];
	navigation_mesh_shut_t shut[NAVIGATION_MESH_SHUT_MOST];
	int32_t shut_count;
	int32_t shut_next;                 // the one written next (the oldest, once all are)
	navigation_mesh_visit_t *visits;   // the searches' notes (a table found by the rectangle: a power of two of them)
	int32_t visit_capacity;
	int32_t visit_count;               // of the search going on
	uint32_t visit_search;             // its number
	int64_t ways_refused;              // statistics: ways found that did not hold, a portal shut for each
	int64_t ways_cut;                  // and those kept as far as they held
	int64_t led_round;                 // small things ways were led round
	uint32_t search_number;
	float last_build_milliseconds;     // statistics: the last tile's
	int64_t tiles_built;
	uint64_t last_search_ticks;
	int64_t searches;                  // statistics: over the rectangles
	int64_t search_fallbacks;          // of which none: no rectangle anywhere near the start
	int64_t lines;
} navigation_mesh_t;

// What a search knows of a node: all of it side by side (one cache line,
// not three)
typedef struct
{
	uint32_t tag;                      // search_number * 2 when reached, + 1 when closed
	float cost;                        // the cost so far
	int32_t came_from;                 // the node it was entered from
} navigation_search_node_t;

// An object as a region's signature last saw it (what changed is told
// when the region is built again for its objects)
typedef struct
{
	uint32_t id;
	int type;
	int model;                         // which of its models (whole, damaged)
	float fields[8];                   // x, z, its base's height step (0 for those the navigation does not stand on), scale, rotation
	float reach_meters;                // how far round x, z it reaches (not in the signature)
	uint8_t kind;                      // what it is to the walkers: a wall, a small thing gone round, a soft one, nothing (navigation.c's KIND_*)
} navigation_object_print_t;

// A small thing's shape in a body's way: capsules (two ends and a
// radius), in the world. A trunk is one, a crooked one a few end to end.
// Whoever asks cuts it to the height it takes itself (from the ground to
// its head) and lays that on the ground: a leaning trunk is over a hen's
// head half a meter from its root, not over a man's
typedef struct
{
	float a[3];
	float b[3];
	float radius;
	int16_t type;                      // the object's type
	uint8_t soft;                      // a soft thing's (a bucket: run through), not one gone round
	uint8_t padding;
	uint32_t id;                       // the object's
} navigation_capsule_t;

#define NAVIGATION_CAPSULE_SQUARES 16

// A model's capsules (its own axes, at size 1) and how much room it takes
#define NAVIGATION_MODEL_CAPSULES_MOST 4

typedef struct
{
	bool fitted;
	int capsule_count;                 // 0: nothing of it in a body's way
	float capsules[NAVIGATION_MODEL_CAPSULES_MOST][7]; // two ends, the radius
	float volume;                      // cubic meters (for the mass of those the ini gives none)
} navigation_model_shape_t;

typedef struct
{
	navigation_surface_t *surfaces;    // surface_capacity of them (NAVIGATION_REGION_EXTRA_SLOTS at most), grown as they come
	// Of the small and soft things that reach into it, in the order of
	// the squares (NAVIGATION_CAPSULE_SQUARES a side) their first ends are
	// in or nearest: capsule_first[square] is a square's first, its next
	// one's is its end (a line asks for the few by it, many times a frame)
	navigation_capsule_t *capsules;
	uint16_t *capsule_first;           // NAVIGATION_CAPSULE_SQUARES squared + 1 of them; NULL: no capsules
	// Its cells along each edge (x low, x high, z low, z high), which the
	// tiles of the regions round it read (a body's points by their edge
	// reach in): what they hold (a hash), the generation that last changed
	// it, and the region's generation they were last looked at for. A
	// tree felled in its middle builds none of the tiles round it again
	uint64_t edge_hash[4];
	uint32_t edge_generation[4];
	uint32_t edge_checked_generation;
	int32_t capsule_count;
	int32_t capsule_capacity;
	navigation_object_print_t *prints; // its objects as its signature last saw them (by id)
	int32_t print_count;
	int32_t print_capacity;
	int32_t surface_count;
	int32_t surface_capacity;
	uint64_t objects_signature;        // of the standing objects in it, to notice changes
	uint32_t objects_generation_checked; // objects_t.generation when the signature was last compared
	bool objects_touched;              // an object in it (or near) added, gone, come loose since (objects_changes_since): compared again
	bool stale;                        // to be built again before it is used
	bool terrain_touched;              // the terrain under it changed: by enough to matter?
	bool dirty;                        // being built right now
	// Stale for a few of its objects only: just the cells round them (x, z
	// first and last, inclusive) are built again
	bool partial;
	int32_t partial_box[4];
	// Stale for small things only, none by a wall or near another (the
	// rectangles are as they were: small things are not in them): built
	// again, its tiles that were current (at small_change_from, its
	// generation then) still are
	bool small_change;
	uint32_t small_change_from;
	// Its cells' surfaces let go (navigation_shed_cells): its tiles are
	// built, nothing asks the cells any more; what a cell holds for the
	// whole map stays (its ground, the room over it, the flyers' tops),
	// and the capsules. Built again whole when a tile of it is to be
	// (regrow: as it was, its generation kept); the tiles round it read
	// its margin from its own tile
	bool shed;
	bool regrow;
	bool shed_again;                   // built again for small things only: let go again after
} navigation_region_t;

typedef struct
{
	int32_t size;                      // cells per side
	int32_t region_size;               // cells per side of a region
	int32_t regions_per_side;
	navigation_region_t *regions;
	// Per cell: the free room above the terrain (0: something stands
	// there), and the extra surfaces (in the cell's region, from first on)
	float *ground_height;              // the terrain at the cell's center (as of the region's last build)
	uint8_t *terrain_clearance;
	int16_t *terrain_blocker;          // the object type that takes the room above the terrain, -1 none
	// How far above the cell's ground_height the highest object reaches,
	// all of it (a tree's crown too), NAVIGATION_OBJECT_TOP_UNIT_METERS each:
	// what a bird flies over
	uint8_t *object_top;
	// How steep the ground of each cell is, NAVIGATION_SLOPE_UNIT_DEGREES
	// each (worked out with its region): what walking on it costs, by
	// walk_factors (a road up a hill is hard whichever way it goes)
	uint8_t *ground_slope;
	float walk_factors[256];           // per ground_slope: what a meter on such ground costs (1: flat)
	// Soft things (object_kind: a bucket) are neither surfaces nor room
	// taken: per cell, the height of
	// the highest one above the ground, 5 cm each. An actor steps over one
	// lower than its step, goes around one lower than itself if that is
	// less than small_detour_meters longer (else through it), and never
	// through one taller than itself
	uint8_t *small_height;
	// Per cell: only small things gone round stand in it (a trunk): open
	// ground to the rectangles
	uint8_t *small_only;
	navigation_model_shape_t *model_shapes; // per model of the objects, fitted when first asked for
	int model_shape_count;
	// Per cell: the groups the blockers there stop (navigation_set_blockers)
	uint8_t *blocked;
	// Per cell: under a blocker or in a door's doorway (open or shut). The
	// rectangles there are a cell's own (NAVIGATION_RECT_ZONE), asked of
	// blocked as it is at each search: a door opened or shut builds no
	// tile again; only a blocker or a door that is new, gone or moved does
	uint8_t *blocker_zone;
	navigation_blocker_t blockers[NAVIGATION_BLOCKERS_MAX];
	int blocker_count;
	// The doors (navigation_set_links): their ends' cells, how far apart
	navigation_link_t links[NAVIGATION_LINKS_MAX];
	int32_t link_cells[NAVIGATION_LINKS_MAX][2];
	float link_meters[NAVIGATION_LINKS_MAX];
	int link_count;
	navigation_link_t links_given[NAVIGATION_LINKS_MAX]; // as last given (a change: the doorways stamped again)
	int links_given_count;             // -1: none yet
	float wall_reach_meters;           // [simulation] navigation_wall_meters
	float soft_kilograms;              // and navigation_soft_kilograms
	bool small_out;                    // navigation_small_out: small things gone round are not in the rectangles
	// navigation_shed_cells: a region's cells' surfaces are let go once
	// its tiles are built (with follow_mesh: nothing asks them then)
	bool shed_cells;
	bool baking;                       // navigation_bake at its tiles, side by side: no region is built or let go then
	// navigation_bake has run: the classes of every type of walker are
	// known and their tiles built. Only then are cells let go: before,
	// a region let go after a bear's tiles was built again whole (60 ms
	// by a church) for the first farmer's
	bool baked;
	int32_t shed_regions;              // how many are let go now
	int32_t shed_sweep;                // the region last looked at for letting go (navigation_set_detail)
	// navigation_follow_mesh: what a walker on its way asks (may it step
	// there, has its body the room on this line, how high is the ground)
	// is answered from the rectangles, not from the quarter meters they
	// were made from; no fine way over those either (it searches again)
	bool follow_mesh;
	float small_detour_meters;
	uint8_t *extra_count;
	uint16_t *extra_first;
	uint32_t generation;               // the newest stamp
	uint32_t *region_generation;
	float object_minimum_meters;       // smaller objects are left out
	bool build_everything;             // a new map: every region now, not when used
	bool whole_terrain_seen;           // the last update saw every chunk changed
	// What the regions are built from (navigation_update keeps these)
	const terrain_t *terrain;
	const objects_t *objects;
	float object_margin_meters;        // the farthest an object reaches from its center
	uint32_t object_change_serial;     // the object list's changes heard of so far (objects_changes_since)
	int32_t *build_list;               // scratch: regions to build
	int32_t *object_scratch;           // scratch: objects near a region
	int32_t object_scratch_capacity;
	int32_t last_rebuilt_regions;      // statistics
	int64_t rebuilt_region_total;
	int64_t partial_region_total;      // of those, built in part only (the cells round a few objects that changed)

	// The flyers' search's working memory, grown on first use: a node a
	// block of cells (navigation_find_flight_path). The walkers' is over
	// the mesh's rectangles, its state in them; its open list the heap too
	uint32_t search_number;
	navigation_search_node_t *search;  // per node, what the search knows of it
	navigation_heap_entry_t *heap;     // the open list: a 4-ary heap, cheapest first
	int32_t heap_count;
	int32_t heap_capacity;
	int32_t *node_path;                // the nodes of the found path, end first
	int32_t node_path_capacity;
	int32_t maximum_expansions;
	int32_t last_expansions;           // statistics: the last search
	// The water's cells per navigation cell, when whole (0: not), for the
	// water's cell size it was worked out for
	int32_t water_cells_per_cell;
	float water_cells_for_size;
	uint64_t last_search_ticks;        // its A* (performance counter ticks)
	// The navigation mesh (see the top): the walkers' searches and lines
	navigation_mesh_t mesh;
	// The last path found (navigation_find_path): the waypoints it gets to
	// by a jump, and their heights (navigation_path_jump)
	uint8_t path_jumps[NAVIGATION_MAX_WAYPOINTS];
	float path_jump_heights[NAVIGATION_MAX_WAYPOINTS];
	uint8_t path_small[NAVIGATION_MAX_WAYPOINTS]; // the stretch to it passes small things (navigation_path_small)
	// Small things are looked at only this near a point (where the player
	// is: navigation_set_detail); 0 meters: everywhere
	float detail_x;
	float detail_z;
	float detail_meters;
} navigation_t;

bool navigation_create(navigation_t *navigation, const terrain_t *terrain);
void navigation_destroy(navigation_t *navigation);

// Follows the map once per frame: a new size, a new map (everything is
// built at once) and changed terrain chunks (call before the terrain is
// uploaded, which clears their flags) mark their regions, to be looked at
// when they are used. Keeps the terrain and the objects for that.
void navigation_update(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects, double now_seconds);

// The height of the surface at (x, z) an agent at near_height would be on:
// the terrain (smooth, between cells) or an object's top, preferring one
// with room for it within its reach, then the nearest (agent may be NULL:
// just the nearest)
float navigation_surface_height(const navigation_t *navigation, const terrain_t *terrain, const navigation_agent_t *agent, float x,
	float z, float near_height);
// The same, and whether that is an object's top (not the terrain)
float navigation_surface_height_on(const navigation_t *navigation, const terrain_t *terrain, const navigation_agent_t *agent, float x,
	float z, float near_height, bool *object_out);

// Can the agent stand at this point without swimming: a surface with room
// for it (at any height), dry or shallow enough to wade? (a goal worth
// searching a way to; its region is brought up to date first)
bool navigation_standable(navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z);

// Can the agent be at this point: is there a surface with room for it,
// within its reach of near_height?
bool navigation_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z, float near_height);

// The same at the quarter meter of (x, z): false only where an object's
// top within reach has that quarter in the way (too little room for the
// agent: under a balustrade's rail; something standing there: its post);
// a quarter no surface knows is free if its ground is within a step (a
// stair's first tread, taken for the ground)
bool navigation_quarter_free(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z, float near_height);

// How tight an agent standing at (x, z) is: of eight points round it a
// little inside its radius, how many are on quarter meters it could not
// stand on within a step (a post, a balustrade's rail beside it, the drop
// off a ledge); -1 where it cannot stand at all
int navigation_quarter_tightness(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float x, float z, float near_height);

// Whether an agent's body can go straight from one point to another (it
// at from_height): every eighth of a meter of the line somewhere it stands
// (a step at a time on from the last) with room for its body round it
// (navigation_quarter_tightness: most_brushes of its points taken or
// fewer; or, out of a tighter place, no tighter than the step before).
// The quarter meters' truth about a way the cells' rectangles let through
// (navigation_line_passable: a square of its width fits somewhere in each
// cell, a post beside it or not): a walker asks both before it leaves its
// path's own corners (a point ahead, a corner cut, a turn's arc), and
// its steps are held to the same (its step guard)
bool navigation_body_line(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, int most_brushes);

// The fine way from (from_x, from_z) at from_height to (to_x, to_z), a few
// meters apart, over the quarter meters: round a post inside a cell, past
// a cell's corner, its body's radius off what it cannot stand on (where
// there is such a way; else brushing by, as it must). Its corners into
// points_out (at most most of them), the last by the goal: its own
// quarter, or one beside it (a clear one rather than a tight one). The
// count; 0 if there is none in the window (or the two too far apart)
int navigation_fine_path(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float from_height, float to_x, float to_z, float (*points_out)[2], int most);

// A path from (from_x, from_z), on the surface nearest from_height, towards
// (to_x, to_z): at most NAVIGATION_MAX_WAYPOINTS points (x, z), the start
// not included. If the path needs more, the first ones are given and
// *complete_out is false (search again from the last one).
navigation_result_t navigation_find_path(navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float from_x, float from_z, float from_height, float to_x, float to_z,
	float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2], int *waypoint_count_out, bool *complete_out);

// Whether a step of an agent's middle goes into a small thing (a trunk, a
// boulder: not in the rectangles), deeper than its start is; away (may be
// NULL): the way straight off the thing from the step's start, a length
// of one
bool navigation_small_blocks_step(const navigation_t *navigation, const navigation_agent_t *agent, float from_x, float from_z, float to_x,
	float to_z, float *away_x_out, float *away_z_out);

// Whether a walker that keeps to the rectangles (navigation_follows_mesh)
// may make a step of its middle: onto them or along them (a little slack
// at their edges), by the agent's rules, not into a small thing; one off
// them already may step any way, back to them
bool navigation_step_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	float from_x, float from_z, float height, float to_x, float to_z);

// Whether the last path found (navigation_find_path, just now) gets to
// its waypoint of that index by a jump from the one before (the start,
// for the first): up onto or down off an edge the agent does not step
// (navigation_agent_t.jump_up_meters, jump_down_meters), straight across
// it; and the height it lands at
// Whether a walker's questions are answered from the rectangles
// (navigation_t.follow_mesh): no fine way then, a new search instead
bool navigation_follows_mesh(const navigation_t *navigation);

// Whether anything stands in the cell of a point or the eight round it
// (an object's surface, room taken, a region not built yet: not known).
// False: bare ground there, nothing for a ray through the physics'
// shapes to find that the terrain does not say
bool navigation_objects_near(const navigation_t *navigation, float x, float z);

// Whether a point is where small things matter (navigation_set_detail:
// near the player, or no limit set)
bool navigation_detail_near(const navigation_t *navigation, float x, float z);

// Whether the stretch to a waypoint of the last path found passes small
// things it was led round, or could not be (a walker keeps its eyes open
// there)
bool navigation_path_small(const navigation_t *navigation, int waypoint);

// Where small things matter: within meters of (x, z), the player. Further
// off the ways are not led round them (0: everywhere)
void navigation_set_detail(navigation_t *navigation, float x, float z, float meters);

bool navigation_path_jump(const navigation_t *navigation, int waypoint, float *height_out);

// Flying. The highest thing at (x, z): the terrain, or the top of any
// object standing there (loose ones aside), crowns and roofs included, as
// of the region's last build (the terrain as it is now)
float navigation_obstacle_height(const navigation_t *navigation, const terrain_t *terrain, float x, float z);

// True if a flyer can go straight from one point to the other without
// meeting anything higher than ceiling_height (meters, absolute)
bool navigation_flight_line_clear(const navigation_t *navigation, const terrain_t *terrain, float from_x, float from_z, float to_x,
	float to_z, float ceiling_height);

// A flight path from (from_x, from_z) towards (to_x, to_z) that keeps under
// ceiling_height: around what rises above it (a mountain), over the rest.
// The straight line when it is clear, else A* over blocks of
// FLIGHT_BLOCK_CELLS cells, climbing costing a little, pulled tight. Same
// output as navigation_find_path.
navigation_result_t navigation_find_flight_path(navigation_t *navigation, const terrain_t *terrain, float from_x, float from_z,
	float to_x, float to_z, float ceiling_height, float waypoints_out[NAVIGATION_MAX_WAYPOINTS][2], int *waypoint_count_out,
	bool *complete_out);

// The cells' surfaces at a point there again, if their region let them go
// (navigation_shed_cells): for one who asks the cells themselves
// (navigation_describe; a region built whole, tens of milliseconds by a
// big house). A walker's log does not: it says what a let-go cell still
// holds (its ground, the room over it)
void navigation_ensure_cells(navigation_t *navigation, float x, float z);

// Debugging: what the agent finds at (x, z) on the surface nearest
// near_height, in words (room, what object takes it, water, steepness to
// the neighbors). Returns true if it can stand there.
bool navigation_describe(navigation_t *navigation, const terrain_t *terrain, const water_t *water, const objects_t *objects,
	const navigation_agent_t *agent, float x, float z, float near_height, char *text_out, size_t text_size);

// The overlay (F8): per cell, what the agent finds there
typedef enum
{
	NAVIGATION_OVERLAY_FREE = 0,
	NAVIGATION_OVERLAY_BLOCKED,        // an object stands there (no room at all)
	NAVIGATION_OVERLAY_LOW,            // room, but too little for the agent
	NAVIGATION_OVERLAY_STEEP,          // every neighbor is out of reach (a cliff)
	NAVIGATION_OVERLAY_SURFACE,        // an object's top it can stand on (a bridge)
	NAVIGATION_OVERLAY_WATER,          // water it cannot pass
	NAVIGATION_OVERLAY_PATH,           // filled in by the caller
	NAVIGATION_OVERLAY_WAYPOINT,
	NAVIGATION_OVERLAY_STEEP_SOME,     // some neighbors are out of reach (along a slope only)
	NAVIGATION_OVERLAY_SMALL           // a small object: around it if it can, through if it must
} navigation_overlay_t;

// (the navigation mesh on: the cells on the edge of a rectangle of the
// agent's class have NAVIGATION_OVERLAY_MESH_EDGE added)
#define NAVIGATION_OVERLAY_MESH_EDGE 128

// Fills size * size bytes of navigation_overlay_t for the agent
void navigation_fill_overlay(navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, uint8_t *overlay_out);

// True if a region the box touches changed after the generation, or has
// running water in it (the regions are brought up to date first)
bool navigation_changed_since(navigation_t *navigation, const water_t *water, float minimum_x, float minimum_z,
	float maximum_x, float maximum_z, uint32_t generation);

// True if the agent can walk the straight line, starting on the surface
// nearest from_height (every cell passable, every step within reach). If
// not, and blocked_x is not NULL: the center of the first cell it cannot
// step into, and the height it was at
bool navigation_line_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float from_x, float from_z, float from_height, float to_x, float to_z, float *blocked_x,
	float *blocked_z, float *blocked_height);

// A loaded map: every region built again, lazily, when a search or an
// actor first goes through it (not all of it at once, as build_all)
void navigation_forget(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects);

// Every region built now, each on a thread of its own (a new map: the
// start does it, on the loading screen)
void navigation_build_all(navigation_t *navigation, const terrain_t *terrain, const objects_t *objects);

// The regions around a point (its own and the eight next to it) brought
// up to date if they are stale: where somebody stands, whose feet the
// surface queries read (they do not build anything themselves)
void navigation_prepare_around(navigation_t *navigation, float x, float z);

// The navigation mesh as it is, for showing it (the game's overlay): a
// class's rectangles that reach into a box (meters), and their portals.
// The box's tiles are made first if the class has agents (false from
// navigation_mesh_class_used: nothing to show)
typedef struct
{
	float x0;                          // its box, meters
	float z0;
	float x1;
	float z1;
	float height_low;                  // of its surface
	float height_high;
	bool ground;                       // the ground (its height is the terrain's); or an object's top
	int32_t id;                        // its tile * 65536 + its index
} navigation_debug_rect_t;

typedef struct
{
	float a[2];                        // its ends (x, z, meters)
	float b[2];
	float height;                      // of the rectangle it leaves (an object's top)
	bool ground;                       // or the ground: the terrain's there
	bool jump;                         // over what is neither rectangle's: only jumped
	float rise;                        // its worst step
} navigation_debug_portal_t;

bool navigation_mesh_class_used(const navigation_t *navigation, int class_index);
void navigation_mesh_debug(navigation_t *navigation, const water_t *water, int class_index, float minimum_x, float minimum_z, float maximum_x,
	float maximum_z, navigation_debug_rect_t *rects_out, int rects_most, int *rect_count_out, navigation_debug_portal_t *portals_out,
	int portals_most, int *portal_count_out);

// The capsules of the small and soft things in the box (of the regions
// built), each once, at most most; how many
int navigation_capsules(const navigation_t *navigation, float minimum_x, float minimum_z, float maximum_x, float maximum_z,
	navigation_capsule_t *capsules_out, int most);

// The navigation mesh's rectangles of the agent's class (built ones) that
// reach into the box: x, z (their first corner, meters), width, depth
// (meters), kind (0 the ground, 1 an object's top), its highest cell's height, at most most; how many
int navigation_mesh_rects(const navigation_t *navigation, const navigation_agent_t *agent, float minimum_x, float minimum_z, float maximum_x,
	float maximum_z, float (*rects_out)[6], int most);

// All of it built now: every region's surfaces, and the rectangles of
// every region for the widths of the agents given (and of those seen
// before); the tiles side by side on the machine's threads. What a map
// editor's bake is to do, and the console's "bake paths": after it no
// search builds anything, till the map changes. regions_out, tiles_out
// (either may be NULL): how many it built
void navigation_bake(navigation_t *navigation, const water_t *water, const navigation_agent_t *agents, int agent_count,
	int32_t *regions_out, int32_t *tiles_out);

// The navigation mesh in words (the console's navmesh): the tiles built,
// their rectangles and portals, the memory, the last build and search
void navigation_mesh_describe(const navigation_t *navigation, char *text_out, size_t text_size);

// The blockers as they are now (each frame: only a change costs anything;
// the paths through where it changed are looked at again)
void navigation_set_blockers(navigation_t *navigation, const navigation_blocker_t *blockers, int count);
// The doors as they are now (all of them, each time: a door opened or shut,
// one added or gone): the searches go through them (see navigation_link_t)
void navigation_set_links(navigation_t *navigation, const navigation_link_t *links, int count);
// What walking on slopes costs: a meter of ground full_degrees steep or
// steeper costs 1 + extra_cost meters of flat ground, gentler ones less
// (in proportion to the slope); the steepness each actor can climb at all
// is its own (navigation_agent_t)
void navigation_set_slope_cost(navigation_t *navigation, float full_degrees, float extra_cost);

#endif
