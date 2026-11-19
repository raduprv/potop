// Objects placed on the map (trees, grass, rocks, houses) and their drawing.
//
// Every object is an instance of a type (see object_types.h): a position on
// the map, a turn around the vertical axis and a size. Objects stand on the
// ground: their height follows the terrain under them, so digging under a
// tree lowers it with the ground. Floating types (lily pads) ride the water
// surface instead. (Being carried off by the water comes later.)
//
// Objects are kept sorted into a coarse grid of square cells, so that
// drawing, culling and brushes only look at the cells that matter. Each
// type has a draw distance; over its last part the objects fade out with
// a screen space dither (no sorting or blending needed), so grass can stop
// at 60 meters while trees go on to the horizon.
//
// Drawing is instanced and GPU driven (see objects_t): the GPU picks the
// visible objects and groups them by model into one buffer of per object
// data (position, turn, size, fade), and every material of every model is
// drawn with a single indirect call, however many objects use it. Opaque materials go first; alpha tested materials
// (leaves) are cut out by their texture's alpha and drawn two sided.
// Objects are lit like the ground and cast shadows: big ones into both
// shadow maps, small ones (grass) only into the fine map near the camera's
// target, and some (lily pads) not at all.

#ifndef OBJECTS_H
#define OBJECTS_H

#include <stdbool.h>

#include "camera.h"
#include "hiz.h"
#include "impostors.h"
#include "object_model.h"
#include "object_types.h"
#include "settings.h"
#include "shadow_map.h"
#include "terrain.h"
#include "water.h"

// Side of the grid cells objects are sorted into
#define OBJECTS_GRID_CELL_METERS 32.0f
#define OBJECTS_ADDED_BEFORE_SORT 4096 // added since the last sort (objects_sort): more, sorted again
#define OBJECTS_STRAYS_BEFORE_SORT 65536 // come loose or turned since: more, sorted again
#define OBJECTS_WINDOWS_PER_FRAME 100000 // the windows' pass (objects_light_windows): objects looked at a frame
// Objects standing on the ground (all but loose bodies) are set this deep
// into it, so erosion of a centimeter or two does not show under them;
// they are put back on the ground once it wore away more than that
#define OBJECTS_SINK_METERS 0.02f
#define OBJECTS_SINK_UNITS ((int32_t)(OBJECTS_SINK_METERS * (float)TERRAIN_HEIGHT_UNITS_PER_METER))
// Objects put back on changed ground per frame (the rest follow in the
// next frames; a whole new terrain at once)
#define OBJECTS_FOLLOW_BUDGET 10000
#define OBJECTS_FOLLOW_METERS 0.002f   // an object's ground moving less than this leaves it where it is

// object_instance_t.state flags
#define OBJECT_STATE_FREE 0x01     // moved by the physics: position and rotation are its body's
#define OBJECT_STATE_UPROOTED 0x02 // was anchored and came loose (stays loose)
// The game can do something with it (what, the game decides): the
// interactive ray (OBJECTS_RAY_INTERACTIVE) and the player's target only
// see these. None is, until the game says (after each map load: a loaded
// map's are taken off)
#define OBJECT_STATE_INTERACTIVE 0x04
// A module's part (not its body: object_types.h, module): placed and moved
// with its module, never on its own, not kept in the map's file (its
// module puts it back); moved by its module's states (MODULE_PART_MOVES:
// off the grid, drawn as turning things are); not there in its module's
// state (MODULE_POSE_HIDDEN)
#define OBJECT_STATE_PART 0x08
#define OBJECT_STATE_MOVING 0x10
#define OBJECT_STATE_HIDDEN 0x20
// Gone in the game (into the silt, washed away, picked up, blown up): left
// where it is in the list, never drawn, hit, found by its id nor saved; the
// list is compacted only when a map is saved (objects_compact_dead), not
// sorted again for a pebble
#define OBJECT_STATE_DEAD 0x40

// What changed in the list since its last sort, for those who keep things
// by the objects' indices (the physics' bodies, the navigation's regions):
// an object added at its end, one gone (OBJECT_STATE_DEAD), one come loose
// (OBJECT_STATE_FREE), one turned into another type in its place
// Objects moved without the physics, a frame, followed by the near map's
// moving layer round each (objects_note_moved); more: the whole layer
#define OBJECTS_MOVED_BOXES_MAX 64

typedef enum
{
	OBJECT_CHANGE_ADDED,
	OBJECT_CHANGE_KILLED,
	OBJECT_CHANGE_FREED,
	OBJECT_CHANGE_RETYPED
} object_change_kind_t;

typedef struct
{
	int index;
	object_change_kind_t kind;
	float x;                       // where it was then
	float z;
} object_change_t;

// object_instance_t.damage
#define OBJECT_DAMAGE_INTACT 0
#define OBJECT_DAMAGE_DAMAGED 1
#define OBJECT_DAMAGE_DESTROYED 2

typedef struct
{
	// Its number, the same for as long as it is on the map (kept in the
	// map's file; never 0): what the editor shows and the game names it by.
	// Its index in objects_t.instances changes as others go
	uint32_t id;
	int type;                      // index into objects_t.types.types
	int group;                     // the layout it belongs to (0 = none), for removing whole layouts
	float x;                       // map position, meters
	float z;
	float ground_height;           // where it stands, kept up to date with the terrain
	float lift_meters;             // floating types: water depth under them
	float yaw_radians;             // as placed
	float scale;
	// How it is drawn: the rotation (quaternion x, y, z, w) and, for free
	// objects, the height of its origin (else ground_height + lift_meters)
	float rotation[4];
	float y;
	uint8_t state;                 // OBJECT_STATE_ flags
	uint8_t damage;                // OBJECT_DAMAGE_
	// Laid down by a flood: 0 standing .. 1 flat, towards flatten_yaw
	float flatten;
	float flatten_yaw;
	int body;                      // the physics body (see physics.h), -1 if none
	int gpu_slot;                  // its place on the GPU (see objects_t), which sorting does not move
	// Picked by hand (harvest.h): how many times so far, game hours until
	// it can be again, the type it was before it turned (-1: it did not)
	uint8_t harvested;
	float regrow_hours;
	int harvest_original_type;
	uint32_t owner;                // whose it is: the actor's game number (actors_set_game_id; 0: no one's)
	// Its emissive groups put out (object_format.h, OBJECT_FILE_EMISSIVE_GROUPS):
	// group g's bit g - 1 (0: lit, as without groups; objects_set_emissive)
	uint32_t emissive_off[2];
	// A module's (its type's module >= 0): the state it is in, the one it
	// goes to (the same: it is there), how far it got (0..1), how long it
	// has been there (its state's hold_seconds)
	uint8_t module_state;
	uint8_t module_target;
	float module_progress;
	float module_held_seconds;
	// A module's part: how far its spinning turned it (turns, 0..1)
	double module_turns;
	// A module's part (OBJECT_STATE_PART): its module's id and which part
	uint32_t module_id;
	uint8_t module_part;
} object_instance_t;

typedef struct
{
	object_types_t types;
	object_texture_cache_t textures;
	object_model_t *models;        // one per distinct model file
	int model_count;
	// Far away, the models worth it are drawn as pictures of themselves
	// (see impostors.h): one per model (none: no albedo), made outside the
	// frames (objects_update_impostors) when impostors_pending
	impostor_t *impostors;
	int impostor_capacity;
	impostor_baker_t impostor_baker;
	bool impostors_pending;
	bool impostors_enabled;        // objects.impostors (the settings)
	bool impostors_everywhere;     // the console's "objects impostors all": at any distance (to look at them)
	bool impostors_tinted;         // the World window: impostors tinted, to tell them from the models
	// The culling's switch to impostors: the projection's focal length
	// times the scene's height in pixels, over a picture's (0: none)
	float impostor_factor;
	SDL_GPUGraphicsPipeline *impostor_pipeline;
	SDL_GPUBuffer *impostor_index_buffer;  // a square: two triangles

	// The buildings' textures' level of detail bias this frame (the TAA's
	// render.taa_mip_bias while it is on, else 0)
	float texture_bias;

	object_instance_t *instances;  // sorted by grid cell unless grid_dirty
	int instance_count;
	int instance_capacity;
	object_instance_t *sort_scratch;

	// The grid: cell (i, j) holds instances grid_first[c] .. grid_first[c + 1] - 1
	// with c = j * grid_size + i; vertical extent and farthest draw distance
	// of what each cell holds, for culling whole cells. Free objects (moved
	// by the physics) are kept after the last cell, in grid_first[cells] ..
	// grid_first[cells + 1] - 1, and culled one by one
	int grid_size;
	int *grid_first;
	// Each cell's runs of one type (sorted by draw distance, a type's
	// objects lie together): cell c's are run_first[c] .. run_first[c + 1]
	// - 1, each where it starts in the instances and its type. The models
	// in view are marked by these, not object by object (objects_sort makes
	// them; run_cells: the cell count they were made for)
	int *run_first;
	int *run_start;
	int *run_type;
	int run_count;
	int run_capacity;
	int run_cells;
	// Near first: each model's nearest visible cell this frame (the
	// camera's pass; FLT_MAX: not seen in a cell, a loose one: last), and
	// the draws ordered by it (objects_draw: what is near hides what is
	// behind it from the depth test before that is shaded)
	float *model_distance;
	int model_distance_capacity;
	// Per type: the nearest visible cell it was marked for this pass
	// (find_cells: a type is in most cells in view, its models marked once
	// for the nearest, not once a cell)
	float *type_marked_distance;
	int type_marked_capacity;
	// The camera's visible cells counted by how far they are, to put
	// their ranges in order without sorting them (find_cells)
	uint32_t *near_cell_counts;
	uint32_t *near_cell_order;
	int near_cell_order_capacity;
	bool model_distance_valid;
	int *draw_order;
	void *draw_order_keys;
	int draw_order_capacity;
	bool draw_order_ready;         // draw_order made for this frame's camera draws
	float *grid_minimum_y;
	float *grid_maximum_y;
	float *grid_draw_distance;
	bool grid_dirty;               // instances were removed (or a module added): sort again before use
	// Not all of them where the sort put them (the game changes the list
	// a little and does not sort it all again for that, see
	// objects_add_lifted, objects_set_free, objects_kill): those from
	// sorted_count on were added since (each found by its own position);
	// strays, those within come loose (OBJECT_STATE_FREE: not in their
	// cell any more) or turned into another type (out of their cell's order
	// by draw distance). Whoever goes by the cells goes through these too
	int sorted_count;
	int *strays;
	int *stray_slots;              // each stray's GPU slot (kept by the slot: the indices move when the list is compacted)
	int stray_count;
	int stray_capacity;
	uint8_t *stray_marks;          // per instance (instance_capacity): 1 if in the strays (each listed once)
	uint32_t sort_serial;          // counts the sorts (and the lists forgotten): the culling's order goes up whole after one
	// The changes since the last sort (object_change_t): change_serial of
	// the first is change_base; who keeps things by index goes on from the
	// serial it got to (objects_changes_since); a sort (a new generation)
	// begins it again
	object_change_t *changes;
	int change_count;
	int change_capacity;
	uint32_t change_base;
	// Within a cell, objects are sorted by their type's draw distance, the
	// farthest first: past the cell's distance only the start of it is drawn
	int *type_rank;                // per type: its place in that order
	int *floating;                 // indices of floating instances (valid while sorted)
	int floating_count;

	// Drawing is GPU driven (objects_cull.comp). Every object has a slot
	// on the GPU, a record of where and how it is drawn, which stays put
	// when the instances are sorted; the CPU sends up only the slots whose
	// objects changed (objects_touch). For each pass (the camera, the far
	// shadow map, the near one) the GPU picks the objects to draw (their
	// grid cell, as the CPU found it visible; their draw distance; their
	// box against the frustum) and writes them, grouped by model, into the
	// pass's instance buffer, with the indirect draw arguments of every
	// material.
	SDL_GPUBuffer *record_buffer;      // per slot: 8 words
	SDL_GPUBuffer *slot_cell_buffer;   // per slot: grid cell and flags
	uint32_t slot_capacity;            // slots the two hold
	SDL_GPUBuffer *type_buffer;
	SDL_GPUBuffer *model_buffer;
	SDL_GPUBuffer *draw_buffer;
	SDL_GPUBuffer *update_buffer;
	uint32_t update_capacity;          // entries
	SDL_GPUBuffer *cell_visible_buffers[3];
	uint32_t cell_visible_words;       // the staging's room (words)
	uint32_t cell_words_used;          // the grid's cells' words now (the visible bits; the dry bits after them, then the ranges)
	// The water (objects_follow_water's, this frame): the camera's pass
	// marks the cells with no water anywhere near (by its tiles) dry, and
	// their objects' pixels skip the water (ground_water.glsl)
	const water_t *dry_water;
	SDL_GPUBuffer *count_buffers[3];
	SDL_GPUBuffer *instance_buffers[3];
	uint32_t instance_capacity_per_pass[3];
	// Per slot, what the counting decided (its fade, below 0: not drawn), so
	// that the writing draws exactly what was counted
	SDL_GPUBuffer *decision_buffers[3];
	uint32_t decision_capacity[3];     // floats (two per slot, one per object looked at)
	SDL_GPUBuffer *argument_storage[3];
	SDL_GPUBuffer *argument_buffers[3];   // copies of the above, for the indirect draws
	SDL_GPUComputePipeline *cull_pipeline;
	SDL_GPUComputePipeline *cull_subgroups_pipeline; // its subgroup variant (gpu.vulkan_11; NULL: none)
	bool use_subgroups;                // render.gpu_subgroups (taken at the draw: the next frame's culling)
	const hiz_t *occlusion;            // the terrain's depth pyramid (the game's), for the camera's pass
	SDL_GPUComputePipeline *update_pipeline;
	int prepared_pass;                 // the pass the instance buffer was last filled for, -1 none
	float far_shadow_least_height;     // the far shadow map's pass leaves out what is lower (objects_prepare_sky_occlusion)
	bool far_shadow_no_plants;         // the far shadow map's pass leaves out the plants (render.debug_far_shadow_no_plants)
	float far_shadow_small_meters;     // ...and always what is lower than this (stones, bushes: the near map has them)
	// The draws: each material of each model, in order
	int *draw_models;
	int *draw_materials;
	int *draw_levels;                  // the level of detail each is drawn at (objects_cull.comp)
	float *draw_drop_levels;           // the deepest branch level the next level keeps (0: none; object.vert)
	int draw_count;
	// The shadows' shared buffers: every model's vertices and indices (but
	// the plants', which the shadows draw each with its own wind) in one
	// vertex and one index buffer, so that every opaque material's shadow
	// is drawn by one indirect call of many draws, not one call each. The
	// draws that can (shadow_batch_count of them) come first in the table;
	// the culling writes their arguments into these buffers (DRAW_POOLED)
	SDL_GPUBuffer *pool_vertex_buffer;
	SDL_GPUBuffer *pool_index_buffer;
	int shadow_batch_count;
	// The camera's: the buildings' models first in those, their tangents and
	// weathering in two more buffers alike; their opaque materials' draws
	// first in the table (building_batch_count of them), sorted by material,
	// so that the draws of one material (its textures, its look) in every
	// building go in one call (DRAW_POOLED_CAMERA)
	SDL_GPUBuffer *pool_tangent_buffer;
	SDL_GPUBuffer *pool_weathering_buffer;
	int building_batch_count;
	// Levels of detail (object_format.h): the models that have them drawn
	// coarser the smaller they are on screen (objects.lod_*); lod_factor:
	// the projection's focal length times the scene's height in pixels
	bool lod_enabled;
	float lod_factor;
	float lod_distance_scale;
	float lod_foliage_kept;            // of a shuffled foliage, the share drawn at level 3 (halfway there at 2)
	float prop_lod_pixels;             // objects.prop_lod_pixels
	bool gpu_tables_ready;             // types, models and draws are up
	// Slots: the instance in each (-1: free), the free ones, the changed ones
	int *slot_instance;
	int slot_count;                    // slots in use or freed: 0 .. slot_count - 1
	int slot_array_capacity;
	int *free_slots;
	int free_slot_count;
	uint8_t *slot_dirty;
	int *dirty_slots;
	int dirty_count;
	uint32_t slot_map_generation;      // generation slot_instance was built for
	uint32_t *update_staging;
	uint32_t *cell_visible_staging;
	// The culling's order (objects_cull.comp): per instance, in the list's
	// order, its slot (ORDER_NONE: none, or a stray where the sort left it),
	// then the strays' slots; up whole after a sort, its tail (those added
	// since and the strays) when that changes. Each pass's ranges of it to
	// look at (the visible cells' objects within their draw distance, the
	// loose ones, the tail): the GPU looks at those, not at every slot
	SDL_GPUBuffer *order_buffer;
	uint32_t order_capacity_bytes;
	uint32_t *order_staging;
	uint32_t order_staging_capacity;   // entries
	uint32_t order_sort_serial;
	int order_instance_count;          // what the order on the GPU was made from
	int order_sorted_count;
	int order_stray_count;
	bool order_valid;
	uint32_t cell_visible_capacity_bytes[3];   // the visible cells' buffers hold the pass's ranges too, after the bits
	uint32_t *range_staging;           // two each: where in the order it starts, where in the work
	uint32_t range_staging_capacity;   // ranges
	// The camera's visible cells, their ranges kept to go up nearest first
	// (find_cells: the instances come out of each model roughly front to
	// back, less overdraw in a forest)
	struct objects_near_cell_s *near_cells;
	int near_cell_capacity;
	// What changed since the shadow maps were drawn: objects in the far map
	// (big ones) or only in the near one (grass)
	bool far_shadow_dirty;
	bool hidden;                       // not drawn (nor their shadows); everything else goes on
	bool no_terrain_occlusion;         // what the terrain (and objects) hide is drawn all the same (the World window, to compare)
	// The occlusion culling seen (console: objects occlusion show|depth|all):
	// the camera's culling keeps the boxes it hid (where on screen, and the
	// pyramid's texels it read for them), drawn over the scene in red and
	// yellow; with the pyramid's depth too; or those it let be drawn too, in
	// green
	bool occlusion_shown;
	bool occlusion_depth_shown;
	bool relief_off;                   // a thatch's relief not marched into (console: objects relief on|off)
	bool windows_by_hour;              // objects_light_windows sets the houses' windows (true from the start; console: objects windows auto|on|off)
	int windows_minute;                // the minute it last did (-1: never)
	int windows_cursor;                // the pass over the objects: the next one (OBJECTS_WINDOWS_PER_FRAME a frame)
	bool windows_pass;                 // under way
	bool age_overridden;               // every building as old as age_override, whatever its type's (console: objects age 0..1|off)
	float age_override;                //   (0 new .. 1 the oldest)
	bool occlusion_all_shown;          // those it let be drawn too (green)
	SDL_GPUBuffer *occlusion_debug_buffer;
	SDL_GPUGraphicsPipeline *occlusion_debug_pipeline;
	bool near_shadow_dirty;
	// Loose objects moved (falling, carried by the water): only the near
	// map's moving layer follows them (shadow_map_t.layer_texture)
	bool moving_shadows;
	// Objects moved without the physics (objects_note_moved): the moving
	// layer follows them too, but only round them (their boxes, world: two
	// each, the least and the most corner), not the whole layer copied again
	bool marked_shadows;
	vec3_t moved_boxes[2 * OBJECTS_MOVED_BOXES_MAX];
	int moved_box_count;
	double spin_seconds;               // the clock the turning ones turn by (objects_update_spin)
	bool any_spins;                    // some instance turns: its slot goes up every frame
	// The instances that turn (their type spins), found again when the list
	// changes (generation), not looked for among every object every frame
	// The objects picked out and waiting to grow again (their ids; harvest.c),
	// found among all of them once when not known (a map loaded)
	uint32_t *regrow_ids;
	int regrow_count;
	int regrow_capacity;
	bool regrow_known;
	int *spin_instances;
	int spin_count;
	int spin_capacity;
	uint32_t spin_generation;
	bool spin_known;
	int shadow_free_filter;            // the next near shadow pass: OBJECTS_SHADOW_* (which objects)
	// Putting objects back on changed ground, over a few frames: the next
	// grid cell to look at, while a pass is on
	bool follow_pass_active;
	bool follow_pass_everything;
	int follow_cursor;
	float draw_distance_scale;     // from the settings, multiplies every type's draw distance
	int next_group;                // the next layout gets this group number
	// The instances by id (objects_find_id): open addressing, index + 1 per
	// slot (0: empty); made again after instances went (they move)
	int *id_slots;
	int id_slot_capacity;
	int id_slot_used;              // slots taken (an id changed leaves its old slot behind)
	bool id_slots_stale;
	uint32_t id_random;            // where new ids come from
	// Counts every change to the instance list (adds, removals, sorting),
	// so the physics knows when its object indices are stale
	uint32_t generation;
	// Per model: whether any object on the map may show it (its type's
	// model, damaged or destroyed); the draws of the others are skipped
	// (made again when generation moves)
	uint8_t *model_used;
	int model_used_capacity;
	uint32_t model_used_generation;
	bool model_used_valid;
	// Per pass (the camera, the near shadow map): the models of the objects
	// in the grid cells found in view (find_cells), a narrower model_used;
	// the far shadow map has none (everything in view there)
	uint8_t *model_in_view[3];
	int model_in_view_capacity[3];
	bool model_in_view_valid[3];

	SDL_GPUGraphicsPipeline *pipeline;            // opaque materials, back faces culled
	SDL_GPUGraphicsPipeline *alpha_pipeline;      // alpha tested, two sided
	SDL_GPUGraphicsPipeline *alpha_culled_pipeline; // alpha tested, one sided (a building's details: its windows, seen from inside, were drawn for nothing)
	SDL_GPUGraphicsPipeline *shadow_pipeline;
	SDL_GPUGraphicsPipeline *shadow_alpha_pipeline;
	// The plants' (object_format.h, OBJECT_FILE_PLANT: plant.vert, plant.frag)
	SDL_GPUGraphicsPipeline *plant_pipeline;
	SDL_GPUGraphicsPipeline *plant_alpha_pipeline;
	SDL_GPUGraphicsPipeline *plant_shadow_pipeline;
	// The props' (triplanar or weathered materials, object_format.h:
	// prop.vert, prop.frag); their shadows are the others'
	SDL_GPUGraphicsPipeline *prop_pipeline;
	SDL_GPUGraphicsPipeline *building_pipeline;  // the buildings' aged materials' (building.vert, building.frag); NULL: drawn as the others
	SDL_GPUGraphicsPipeline *prop_shell_pipeline;   // the moss under water's shells (prop_shell.vert, prop_shell.frag; NULL: none)
	// The year in the plants, as they are drawn (render.month, season.h):
	// the month (1 .. 13), and the snow's months
	float month;
	float snow_first_month;
	float snow_last_month;
	// How much snow lies on things now (0..1): the bark, the branches' sag,
	// the props and the models (main.c: from the snowfall, as on the ground)
	float snow_amount;
	// Per type, the share of its objects there now (0..1): its season's
	// (object_type_t.months_first), not the snow's (a plant does not come
	// back for a thaw);
	// each object is there or not by a number of its own (objects_cull.comp).
	// objects_update_seasons works them out
	float *type_shares;
	float *type_table;             // the GPU's per type table, kept (type_buffer: 4 floats a type)
	float tips_first_month;                       // the new growth's months (the foliage's lighter tips)
	float tips_last_month;
	bool pbr_shading;                             // render.pbr_shading, for the frame being drawn
	int drawn_instance_count;      // last frame
	int draw_call_count;           // last frame
	// One object can be highlighted (picking, selection): its index, valid
	// while the instance list does not change (generation)
	// The ray's cells: stamped with ray_stamp when looked at (each once a ray)
	uint32_t *ray_cell_stamps;
	uint32_t ray_stamp;
	bool highlight_valid;
	int highlight_instance;
	uint32_t highlight_generation;
	float highlight_color[3];
	float highlight_uniform[4];     // color and time, as the shaders get it
	// The wind in the plants (object.vert): per model its sway and flutter
	// (from its type), and the wind's uniforms for the frame
	float *model_wind;
	int model_wind_count;
	float wind_uniform[8];
	float look[8];                     // the buildings' look (settings: roof_*, wood_dulled), for prop_body.glsl
	// The modules on the map (object_modules.c): each module's index, then
	// its parts' (as module_entries says where each starts), made again
	// when generation moves
	int *module_entries;               // per module: its first index in module_indices
	int *module_indices;               // the module's index, then its parts', for each module in turn
	int module_count;
	int module_capacity;
	int module_index_capacity;
	uint32_t module_generation;
	bool module_list_valid;
} objects_t;

// What a ray from the eye goes through: the object, how far along the ray
// (meters) and where
typedef struct
{
	int instance;
	float distance;
	vec3_t point;
} object_ray_hit_t;

typedef enum
{
	OBJECTS_RAY_INTERACTIVE = 0,   // only what the game can do something with (OBJECT_STATE_INTERACTIVE)
	OBJECTS_RAY_ALL                // everything, grass too (the editor, debugging)
} objects_ray_mode_t;

// The objects the ray (direction normalized) goes through before
// maximum_distance, nearest first: a ball around each first, then its box,
// then its triangles (a tree's crown counts, the air around it not). Only
// the grid cells along the ray (and their neighbours, for big objects
// reaching over) are looked at, and the loose objects. Returns the number
// of hits written (at most maximum_hits)
int objects_raycast(objects_t *objects, vec3_t origin, vec3_t direction, float maximum_distance, objects_ray_mode_t mode,
	object_ray_hit_t *hits, int maximum_hits);

// Highlights one object (-1: none) in a color (sRGB, 0..1); valid until the
// instance list changes
void objects_set_highlight(objects_t *objects, int instance, const float color[3]);

// Loads the shaders, data/objects.ini and every model it names. The
// terrain sets the size of the grid.
bool objects_create(objects_t *objects, const terrain_t *terrain, const settings_t *settings);
void objects_destroy(objects_t *objects);

// Models and textures written on disk since they were loaded (the
// converter or potop_tree run while the game is open) are loaded again and
// drawn from now on, wherever they stand. Their collision shapes stay as
// they were until the game starts again. True if anything changed. Cheap
// enough for about once a second (a look at each file's time)
bool objects_reload_changed(objects_t *objects);

// A map of another size: the grid is made anew for it and every object
// removed. Returns false if memory ran out.
bool objects_resize_map(objects_t *objects, const terrain_t *terrain);

// The game can do something with it, or not (OBJECT_STATE_INTERACTIVE);
// false if there is no such object
bool objects_set_interactive(objects_t *objects, uint32_t id, bool interactive);
// Its emissive groups (object_format.h, OBJECT_FILE_EMISSIVE_GROUPS: a
// house's windows, a panel's lamps), by its index in objects->instances:
// how many (0: none), one's description (group 1 .. the count; NULL: none
// such), lit or put out (group 0: all of them; false: no such group), and
// whether it is lit now
// Its places for the creatures and people (object_format.h, OBJECT_FILE_SPOTS):
// how many; one as it stands in the world (position, the way one faces
// there round Y, an area's half sizes along its own turned x and z, the
// spots it leads to); NULL if there is no such one
int objects_spot_count(const objects_t *objects, int instance_index);
const object_file_spot_t *objects_spot_world(const objects_t *objects, int instance_index, int spot, float position_out[3], float *yaw_radians_out,
	float extents_out[2]);
int objects_emissive_group_count(const objects_t *objects, int instance_index);
const object_file_emissive_group_t *objects_emissive_group(const objects_t *objects, int instance_index, int group);
bool objects_set_emissive(objects_t *objects, int instance_index, int group, bool lit);
bool objects_emissive_lit(const objects_t *objects, int instance_index, int group);
// The houses' windows lit by the hour, on their own (objects_t.windows_by_hour):
// from dusk on most, each its own time; put out one by one late in the
// evening, a few from before dawn; by day all lit (the day drowns them).
// Called every frame, does something only when the minute changes
void objects_light_windows(objects_t *objects, double hour, float night_amount);
// Whose it is (an actor's game number; 0: no one's); false if there is no
// such object
bool objects_set_owner(objects_t *objects, uint32_t id, uint32_t owner);
// No object is interactive any more
void objects_clear_interactive(objects_t *objects);

// The instance with that id (its index now), -1 if there is none
int objects_find_id(objects_t *objects, uint32_t id);

// An object gone (OBJECT_STATE_DEAD: see there), or those flagged (remove:
// one byte per instance; a module's parts with their module), or those of
// a group; how many
void objects_kill(objects_t *objects, int index);
int objects_kill_flagged(objects_t *objects, const uint8_t *remove);
int objects_kill_group(objects_t *objects, int group);
// The dead taken out of the list for good (a map saved): a sort follows
int objects_compact_dead(objects_t *objects);
// An object turned into another type where it stands, its id and the rest
// kept (picked out, grown again): false if there is no such type
bool objects_retype(objects_t *objects, int index, int type);
// The changes from serial on (those since the last sort): a pointer to the
// first, how many (*count_out); the serial to go on from next time
// (*next_out). NULL, 0 and the serial of the first if the list was sorted
// since (generation changed: go through it all)
const object_change_t *objects_changes_since(const objects_t *objects, uint32_t serial, int *count_out, uint32_t *next_out);

// Each instance within radius of (x, z) (its middle), visited (index; the
// visit says false to stop): by the grid's cells round the circle and the
// loose objects after them, all of them only while the grid is not sorted.
// Not a pass over every object (millions on a big map)
typedef bool (*objects_visit_t)(const objects_t *objects, int index, void *user);
void objects_visit_circle(const objects_t *objects, float x, float z, float radius, objects_visit_t visit, void *user);
// Gives an instance another id (a loaded map's); false if another has it
// (or it is 0)
bool objects_set_id(objects_t *objects, int index, uint32_t id);

// Adds an object standing on the terrain (with a new id); returns false
// if the type has no model or memory ran out
bool objects_add(objects_t *objects, const terrain_t *terrain, int type, float x, float z, float yaw_radians, float scale, int group);

// The same, lifted above the ground (a crystal ball on its stand)
bool objects_add_lifted(objects_t *objects, const terrain_t *terrain, int type, float x, float z, float yaw_radians, float scale,
	int group, float lift_meters);

// A new group number for the objects of one layout
int objects_new_group(objects_t *objects);

// Removes the objects of a group; returns how many
int objects_remove_group(objects_t *objects, int group);

// The group of the grouped object nearest to (x, z) within radius, 0 if none
int objects_nearest_group(const objects_t *objects, float x, float z, float radius);

// A circle to clear of vegetation
typedef struct
{
	float x;
	float z;
	float radius;
} objects_clear_circle_t;

// Removes the vegetation (types the map is populated with, not belonging
// to a group) touching any of the circles: within its radius plus half the
// plant's own reach, so no tree crown grows through a roof
int objects_clear_vegetation(objects_t *objects, const objects_clear_circle_t *circles, int circle_count);

// Each type's share of its objects there now (type_shares): the month
// says; the GPU's copy follows, and the shadows when it changed
void objects_update_seasons(objects_t *objects);
// The turning ones (a type's spin: a windmill's sails) turned to where
// the clock says: their slots go up again, the near map's moving layer
// follows them. Once a frame, before the shadows
void objects_update_spin(objects_t *objects, double seconds);
// An object moved without the physics (turning, a module's part: a door, a
// windmill's sails): the box it is drawn within whichever way it is turned
// kept for the near map's moving layer (too many: the whole layer again)
void objects_note_moved(objects_t *objects, int k);
// The boxes kept marked on the near map (shadow_map_mark_moving), and let go
void objects_mark_moved_shadows(objects_t *objects, shadow_map_t *shadow);

// Modules (module_format.h, object_modules.c). Placing a module's type
// places its parts with it (objects_add); removing any of it removes all of
// it. Once a frame its moving parts follow its state; seconds since the
// last frame (0: paused)
void objects_update_modules(objects_t *objects, float seconds);
// Sends module (an instance's id) towards the state named (or numbered,
// for state_name NULL); false if it is not a module or has no such state
bool objects_set_module_state(objects_t *objects, uint32_t id, const char *state_name, int state);
// The module's file and the state it is in (-1: id is not a module;
// going_to: the one it goes to, the same once there)
const module_file_t *objects_module_of(const objects_t *objects, int instance_index);
int objects_module_state(objects_t *objects, uint32_t id, int *going_to);
// The module nearest (x, z) within radius (an instance index, -1: none)
int objects_nearest_module(const objects_t *objects, float x, float z, float radius);

// Doors (a module's parts flagged MODULE_PART_DOOR: a farm's doors and
// gates), each closed or open on its own, by its module's id and its part
// number. Set: it swings there (its state's seconds; turned back on the
// way, back from where it got); false if there is no such door
bool objects_set_door(objects_t *objects, uint32_t module_id, int door_part, bool open);
// 1 open (or opening), 0 closed (or closing), -1 no such door
int objects_door(objects_t *objects, uint32_t module_id, int door_part);
// A module's door by its part's name ("door1"); -1 if none
int objects_door_find(objects_t *objects, uint32_t module_id, const char *name);
typedef struct
{
	float hinge[3];                // its hinges' foot (world)
	float along[3];                // from the hinges across the leaf, shut (horizontal, unit)
	float swing[3];                // the side it swings open to (horizontal, unit)
	float width;                   // the leaf's, meters
	float height;
	bool open;                     // open or opening
	bool moving;                   // swinging now
	bool passage;                  // a way through when open (not a door drawn on a wall)
} objects_door_info_t;

// Where a door is and how it swings; false if there is no such door
bool objects_door_info(objects_t *objects, uint32_t module_id, int door_part, objects_door_info_t *info_out);
// Every door on the map (its module's id, its part), at most maximum; how many
int objects_door_list(objects_t *objects, uint32_t *module_ids_out, int *door_parts_out, int maximum);
// The nearest door along a ray (its model's bounds as it stands now),
// within maximum_distance: its module's id, its part, how far; false if none
bool objects_door_at(objects_t *objects, vec3_t origin, vec3_t direction, float maximum_distance, uint32_t *module_id_out, int *door_part_out,
	float *distance_out);
// (objects.c's: the parts placed after a module's body was added, at the
// end of the list; a removal grown to whole modules; parts put back on
// their module's ground)
void object_modules_place_parts(objects_t *objects, const terrain_t *terrain, int module_index);
void object_modules_widen_removal(objects_t *objects, uint8_t *remove);
void object_modules_renumber(objects_t *objects, uint32_t old_id, uint32_t new_id);

// A random size and turn for a new object of the type
void objects_random_placement(const objects_t *objects, int type, float random_a, float random_b, float *yaw_out, float *scale_out);

// Removes every object within the radius (of the category, or of any with
// category -1); returns how many
int objects_remove_in_radius(objects_t *objects, float x, float z, float radius, int category);

// Removes the objects flagged (one byte per object, non-zero: remove),
// keeping the order of the rest; returns how many
int objects_remove_flagged(objects_t *objects, const uint8_t *remove);

// Removes the object nearest to (x, z) if it is within radius
bool objects_remove_nearest(objects_t *objects, float x, float z, float radius);

// Removes all objects (with only_spawned, only the vegetation: objects of
// types the map is populated with that belong to no layout)
void objects_clear(objects_t *objects, bool only_spawned);

// Sorts the objects into the grid if anything was added or removed
void objects_sort(objects_t *objects);

// Where the object's origin is drawn
static inline float objects_instance_base_y(const object_instance_t *instance)
{
	return (instance->state & OBJECT_STATE_FREE) ? instance->y : instance->ground_height + instance->lift_meters;
}

// The model the object is drawn with (its damaged or destroyed look); -1:
// not drawn (destroyed without a model for it)
int objects_instance_model(const objects_t *objects, const object_instance_t *instance);

// Recomputes the drawn rotation of an object that is not free: its yaw,
// tipped over by its flatten amount
void objects_update_rotation(object_instance_t *instance);

// A placed object starts moving with the physics (or stops): moves it out
// of the grid (or back into it)
void objects_set_free(objects_t *objects, int instance_index, bool free);

// Moves objects standing on changed terrain (chunks with objects_dirty) to
// the new ground height, OBJECTS_FOLLOW_BUDGET of them a frame
void objects_follow_terrain(objects_t *objects, terrain_t *terrain);

// The object's look or place changed (outside objects.c: the physics laying
// it down, damaging it): its GPU slot goes up again before the next draw
void objects_touch(objects_t *objects, int instance_index);

// Puts one object back on the ground under it (after the ground changed)
void objects_refresh_ground(objects_t *objects, const terrain_t *terrain, int instance_index);

// Lifts floating objects to the water surface (once per frame); keeps the
// water for the camera's pass (its dry cells)
void objects_follow_water(objects_t *objects, const water_t *water);

// Finds what the camera sees and uploads it, before the scene's pass
// begins (objects_draw draws it)
void objects_prepare(objects_t *objects, const camera_t *camera);
// Makes (or lets go of) the models' impostors when they changed: outside
// the frames (between gpu_end_frame and the next gpu_begin_frame)
void objects_update_impostors(objects_t *objects);

// Draws what objects_prepare found, in the scene's pass (its scene block
// already pushed). variation_texture is the terrain's noise texture, used
// for the cloud shadows.
void objects_draw(objects_t *objects, SDL_GPURenderPass *pass, const render_settings_t *settings, const shadow_map_t *shadow,
	const shadow_map_t *near_shadow, SDL_GPUTexture *variation_texture, float time_seconds);

// The same for a shadow map: prepared before shadow_map_begin, rendered
// in its pass. Without a camera: the far map, every object that casts
// shadows there. With one: the near map, culled to what the map sees and
// to the objects the camera draws.
void objects_prepare_shadow(objects_t *objects, const shadow_map_t *shadow, const camera_t *camera);
// Which objects a near shadow pass draws: all, the ones standing still
// (the near map's still layer), or only the loose ones (its moving layer:
// falling, floating, thrown)
typedef enum
{
	OBJECTS_SHADOW_ALL = 0,
	OBJECTS_SHADOW_STILL,
	OBJECTS_SHADOW_LOOSE
} objects_shadow_layer_t;
void objects_prepare_shadow_layer(objects_t *objects, const shadow_map_t *shadow, const camera_t *camera, objects_shadow_layer_t layer);
void objects_render_shadow(objects_t *objects, SDL_GPURenderPass *pass);
// The far map's objects for the sky's occlusion (sky_occlusion.h): only
// those at least least_height meters tall (a fern does not shade itself)
void objects_prepare_sky_occlusion(objects_t *objects, const shadow_map_t *shadow, float least_height);
// The occlusion culling seen (objects_t.occlusion_shown), over the scene
// drawn: the boxes the camera's culling hid, the pyramid's texels it read
// for them, and with occlusion_depth_shown the pyramid's level 0 (near
// and far: the camera's, for its depth made linear)
void objects_draw_occlusion_debug(objects_t *objects, SDL_GPURenderPass *pass, float near_distance, float far_distance);

#endif
