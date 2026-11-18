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
// default) are left out: actors walk through pebbles. Small ones (up to
// small_object_meters, a stone) are neither surfaces nor room taken, only a
// height per cell: each actor steps over, goes around (at a price, through
// when it must) or is stopped by them, by its own size.
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
// The navigation mesh: the same surfaces gathered into rectangles the
// walkers' searches go over (the cells themselves were searched once: a
// node each, tens of megabytes of search state on a big map). Per region (a tile) and per class of agent (by its width in quarter
// meters: a hen 1, a man 2, a cow or a bear 3), the usable surfaces are merged greedily into rectangles (up to
// NAVIGATION_MESH_RECT_MOST cells a side) where every cell is alike (the
// room above, the blockers' groups, a small object, the slope's cost, the
// same kind: ground or an object's top) and any step between neighbors is
// within the class's reach (the least of its agents': anyone of the class
// goes anywhere inside). Cells with water at the build stay rectangles of
// one cell (looked at with the water as it is, as the cells are); the
// tile is built again when its water's cells change, when its region is
// (its generation), or when an agent of a lesser reach (or a lower or a
// wider one) joins the class. Nothing here is fitted to a house: an
// object's top is usable to a class where a square of its quarter meters
// the class's width a side has the room of the class's lowest; a quarter
// with less room (under a rail) is a wall to it; a portal ends where the
// quarters on either side meet within a step and is left out if narrower
// than the class; ground cells keep away from what takes all the room, by
// the widest agent's radius in whole cells. Rectangles meet at
// portals (a run of the shared edge, its worst step: per agent at search
// time whether it climbs that), within the tile and across to the next
// (linked when either is built). A* over the rectangles (the agent's own
// rules for the room, the groups, small objects and the water, at the
// time), then the corridor pulled tight (the funnel), the agent's radius
// off the portals' ends. The doors are links between the rectangles of
// their ends' cells. The straight line (navigation_line_passable) walks the
// rectangles, looking only where it crosses from one to the next. Where
// the mesh has nothing (the start on a cell the large class keeps off, a
// map's edge) the cells' way is taken, as before. The classes are built
// from the 1 m cells for now (the small one at 0.5 m wants the region
// build at 0.5 m first: TODO.md).
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
#define NAVIGATION_MAX_WAYPOINTS 32
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
#define NAVIGATION_MESH_RECT_MOST 16            // cells a side of a rectangle at most

// Where one rectangle leads into the next: a run of the edge they share
typedef struct
{
	int32_t from;                      // the rectangle in this tile it leaves
	int32_t to;                        // the one it enters: its tile * 65536 + its index
	float a[2];                        // the run's ends (x, z, meters)
	float b[2];
	float rise;                        // the worst step across it (meters)
	bool cross_surface;                // between the ground and an object's top (a step only, no slope)
} navigation_portal_t;

typedef struct
{
	int32_t x;                         // its first cell (x, z)
	int32_t z;
	uint8_t width;                     // cells
	uint8_t depth;
	uint8_t kind;                      // 0: the ground, 1: an object's top
	uint8_t clearance;                 // the least room above (NAVIGATION_CLEARANCE_UNIT_METERS each, or OPEN)
	uint8_t blocked;                   // the blockers' groups over it
	uint8_t small;                     // a small object's height on it (as navigation_t.small_height)
	float cost;                        // what a meter on it costs (its slope; 1 on an object's top)
	float height_low;
	float height_high;
	int32_t wet_node;                  // a cell with water at the build: its node (the water looked at then); -1: dry
	int32_t first_portal;              // in its tile's portals
	int32_t portal_count;
	// The search's (tag as navigation_search_node_t's)
	uint32_t tag;
	float search_cost;
	int32_t came_from;                 // -1: the start
	float entry[2];                    // where the search came in
	float came_a[2];                   // the portal it came through (or a door's two ends)
	float came_b[2];
	bool came_by_door;
	bool came_by_jump;                 // or over an edge it jumps: came_a where it leaves the ground, came_b where it lands
	float came_height;                 // (and at what height)
} navigation_rect_t;

typedef struct
{
	bool built;
	uint32_t generation;               // its region's at the build
	uint32_t class_epoch;              // its class's at the build
	uint64_t water_signature;          // its water's cells at the build
	uint64_t water_checked_ticks;      // when they were last compared (SDL_GetTicks)
	uint32_t checked_search;           // the search that last found it current
	uint32_t build_serial;             // which build of all the tiles' this is (a shut portal's rectangles are this build's)
	navigation_rect_t *rects;
	int32_t rect_count;
	int32_t rect_capacity;
	navigation_portal_t *portals;      // within the tile, by rectangle
	int32_t portal_count;
	int32_t portal_capacity;
	navigation_portal_t *border[4];    // to the next tile (+x, -x, +z, -z)
	int32_t border_count[4];
	int32_t border_capacity[4];
	int16_t *ground_rect;              // per cell of the region: the rectangle its ground is in (-1: none)
	int16_t *extra_rect;               // per extra surface slot of the region
} navigation_mesh_tile_t;

typedef struct
{
	float reach;                       // the least step (or slope over a cell) of its agents seen
	float height;                      // the lowest of them
	float step;                        // the least step of them (onto an object's top)
	int32_t erosion_cells;             // cells kept off what takes all the room (its widest)
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
	int64_t ways_refused;              // statistics: ways found that did not hold, a portal shut for each
	int64_t ways_cut;                  // and those kept as far as they held
	uint32_t search_number;
	float last_build_milliseconds;     // statistics: the last tile's
	int64_t tiles_built;
	uint64_t last_search_ticks;
	int64_t searches;                  // statistics: over the rectangles
	int64_t search_fallbacks;          // of which none: no rectangle anywhere near the start
	int64_t lines;
	int64_t line_fallbacks;
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
} navigation_object_print_t;

typedef struct
{
	navigation_surface_t *surfaces;    // NAVIGATION_REGION_EXTRA_SLOTS of them
	navigation_object_print_t *prints; // its objects as its signature last saw them (by id)
	int32_t print_count;
	int32_t print_capacity;
	int32_t surface_count;
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
	// Small objects (navigation_object_meters to small_object_meters across,
	// a stone) are neither surfaces nor room taken: per cell, the height of
	// the highest one above the ground, 5 cm each. An actor steps over one
	// lower than its step, goes around one lower than itself if that is
	// less than small_detour_meters longer (else through it), and never
	// through one taller than itself
	uint8_t *small_height;
	// Per cell: the groups the blockers there stop (navigation_set_blockers)
	uint8_t *blocked;
	navigation_blocker_t blockers[NAVIGATION_BLOCKERS_MAX];
	int blocker_count;
	// The doors (navigation_set_links): their ends' cells, how far apart
	navigation_link_t links[NAVIGATION_LINKS_MAX];
	int32_t link_cells[NAVIGATION_LINKS_MAX][2];
	float link_meters[NAVIGATION_LINKS_MAX];
	int link_count;
	navigation_link_t links_given[NAVIGATION_LINKS_MAX]; // as last given (a change: the doorways stamped again)
	int links_given_count;             // -1: none yet
	float small_object_meters;
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

// Whether the last path found (navigation_find_path, just now) gets to
// its waypoint of that index by a jump from the one before (the start,
// for the first): up onto or down off an edge the agent does not step
// (navigation_agent_t.jump_up_meters, jump_down_meters), straight across
// it; and the height it lands at
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

// The navigation mesh's rectangles of the agent's class (built ones) that
// reach into the box: x, z (their first corner, meters), width, depth
// (meters), kind (0 the ground, 1 an object's top), its highest cell's height, at most most; how many
int navigation_mesh_rects(const navigation_t *navigation, const navigation_agent_t *agent, float minimum_x, float minimum_z, float maximum_x,
	float maximum_z, float (*rects_out)[6], int most);

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
