// Making houses into the game's objects (.pobj): what tools/potop_house (the
// command line) and tools/house_view (the headless viewer) share. How they
// are made is told in house_grow.c.
//
// A house is made from a recipe (house_recipe_t): its style, its seed, its
// sizes and the numbers that shape it. Recipes are kept as text
// (house_recipe_load, house_recipe_save; house_plans/ holds them): one line
// per setting, "key = value". A preset (house_preset) sets the numbers of a
// sort of house (a timber framed town house, a stone cottage...), to start
// from.
//
// The house stands on the ground at y = 0, its middle over the origin, its
// door looking down +Z (as layouts.ini expects a layout's front), in
// meters.
//
// Its materials (house_material_t), at most HOUSE_MAX_MATERIALS, by what
// they are made of, not by where they are: the walls' plaster, the timber,
// the roof, the base's stone, the glowing glass of the windows and, last,
// a collision shell that is never drawn (OBJECT_MATERIAL_COLLISION_ONLY).
// Every material has texture coordinates (the hard stone may be triplanar
// too, millstone_triplanar: the impostor's bake goes by the coordinates), and the walls, the
// roof and the base are weathered: the game grows moss on their north
// sides and lays snow on them in winter (prop.frag).
//
// Levels of detail: three, meshes of their own after each material's own
// indices (OBJECT_MATERIAL_LOD_MESHES), made by construction: the first
// without the bevels and the small pieces, the second the walls and the
// roof alone, the third a box under a roof. The header's lod_errors say how
// far each is from the full one, from what was left out.
//
// Textures: four files under the data directory per material (color sRGB;
// normal, OpenGL's +Y; arm: R occlusion, G roughness, B metal; emissive
// sRGB), named in the recipe as the game names them (relative to data/).
// None named: the style's own, made here once
// (data/props/textures/house_<name>_color.png...) as stand-ins for real
// ones.

#ifndef HOUSE_GROW_H
#define HOUSE_GROW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "object_format.h"
#include "module_file.h"

// Solid, never drawn (not by the camera, not in the shadows, not in the
// impostor): a collision shell. When a model has one such material, only
// those are solid and the rest are passable for the physics and the
// navigation. Reserved with the engine (object_format.h gets it next)

#define HOUSE_PATH_LENGTH 512
#define HOUSE_NAME_LENGTH 32
// (more kinds than a file holds: a house writes only those it uses, at
// most OBJECT_FILE_MAX_MATERIALS)
#define HOUSE_MAX_MATERIALS 24
#define HOUSE_LEVELS 4                         // its own and three levels of detail
#define HOUSE_MAX_ANCHORS 48
#define HOUSE_MAX_BAYS 24
#define HOUSE_MAX_FLOORS 4

// What the walls are made of
typedef enum
{
	HOUSE_WALL_TIMBER,                     // timber framed: posts and beams with plastered panels between them
	HOUSE_WALL_STONE,                      // coursed stone to the eaves, timber only round the openings
	HOUSE_WALL_WATTLE,                     // wattle and daub: a few thin crooked posts, mud and lime between them
	HOUSE_WALL_LOG,                        // round logs laid flat, crossing at the corners
	HOUSE_WALL_RUBBLE,                     // rough field stones, low
	HOUSE_WALL_PLANK,                      // vertical boards over a sparse frame (a barn's)
	HOUSE_WALL_COUNT
} house_wall_t;

// What the building is: its kind sets the shape of everything else (sizes,
// walls, roof, openings, what is added on) at its size (1 small .. 3
// big); the recipe's other settings then override what they like
typedef enum
{
	HOUSE_KIND_HOVEL,                      // one low room under a big thatched roof, no glass, no chimney
	HOUSE_KIND_COTTAGE,                    // a peasant's: one floor and a loft, small shuttered windows
	HOUSE_KIND_CRAFTSMAN,                  // a workshop below with a wide shutter, the family above
	HOUSE_KIND_TOWNHOUSE,                  // a merchant's: several jettied floors, glass, a gable to the street
	HOUSE_KIND_INN,                        // a wide town house with a covered walk, a balcony, a sign
	HOUSE_KIND_BARN,                       // a big roof over a double door, no windows
	HOUSE_KIND_STONE_HOUSE,                // stone to the eaves
	HOUSE_KIND_COUNT
} house_kind_t;

typedef enum
{
	HOUSE_ROOF_GABLE,                      // two slopes, a gable at each end
	HOUSE_ROOF_HIP,                        // four slopes
	HOUSE_ROOF_HALF_HIP,                   // the gables cut short under a small slope (hip_fraction of the way down)
	HOUSE_ROOF_COUNT
} house_roof_t;

// The house's materials, in the order they are written
typedef enum
{
	HOUSE_MATERIAL_WALLS,                  // the plaster between the timbers, or the stone of the walls
	HOUSE_MATERIAL_TIMBER,                 // posts, beams, braces, frames, shutters, doors, rafters
	HOUSE_MATERIAL_ROOF,                   // tiles, shingles or thatch
	HOUSE_MATERIAL_BASE,                   // the base's stone, the chimney's
	HOUSE_MATERIAL_GLASS,                  // the dark behind the shutters and the boards, the barn's inside
	HOUSE_MATERIAL_DETAILS,                // the details atlas: window glass, the ends of beams, signs, ornaments (alpha tested)
	HOUSE_MATERIAL_COLLISION,              // the shell the physics sees (never drawn)
	HOUSE_MATERIAL_FRINGE,                 // a thatch's ragged edge: straws hanging from its eave roll (alpha tested, two sided)
	HOUSE_MATERIAL_ROOF_TIMBER,            // the roof's wood: rafters, purlins, barge boards, the boards under its eaves (old, unvarnished)
	HOUSE_MATERIAL_LOG,                    // a log wall's logs: bark and bare wood, wrapped round each log
	HOUSE_MATERIAL_IVY,                    // ivy up the walls: cards cut from house_ivy_color.png (4 x 4 cells: leaves, sprays, tips, vines), evergreen
	HOUSE_MATERIAL_CREEPER,                // Virginia creeper: the same, house_creeper_color.png, red in the autumn, bare vines in the winter
	HOUSE_MATERIAL_IRON,                   // a smithy's iron: the anvil, the tools, the bellows' nozzle (no texture: dark, a little metal)
	HOUSE_MATERIAL_EMBERS,                 // a forge's glowing coals (no texture: its own light)
	HOUSE_MATERIAL_LEATHER,                // the bellows' leather (no texture)
	HOUSE_MATERIAL_HOOD,                   // a smithy's hood over the forge: plaster, blackened by the smoke (soot baked per vertex)
	HOUSE_MATERIAL_CHIMNEY,                // a smithy's chimney: dark burnt brick or stone, sooted toward its top
	HOUSE_MATERIAL_FLOOR,                  // a smithy's floor: beaten earth, ash and slag; a church porch's boards (floor_color_texture named)
	HOUSE_MATERIAL_CEILING,                // boards under a roof seen from inside (ceiling_color_texture named: else the roof's wood there)
	HOUSE_MATERIAL_CARVED,                 // wood worked fine: turned and carved posts, arches, a portal (carved_color_texture named: else the timbers')
	HOUSE_MATERIAL_FOOTING,                // a church's stone footing (its base's textures; weathered as plaster: its stones' domes no moss film)
	HOUSE_MATERIAL_STONE,                  // loose and cut stone: blocks, rubble, quoins, sills, steps (stone_color_texture named: else limestone)
	HOUSE_MATERIAL_MILLSTONE,              // hard stone: millstones, grindstones (millstone_color_texture named: else granite)
	HOUSE_MATERIAL_COUNT
} house_material_kind_t;

// The cells of the details atlas (tools/house_atlas.c), in its order
typedef enum
{
	HOUSE_CELL_WINDOW_LEADED_DIAMOND,
	HOUSE_CELL_WINDOW_LEADED_SQUARE,
	HOUSE_CELL_WINDOW_FOUR_PANE,
	HOUSE_CELL_WINDOW_ROUNDELS,
	HOUSE_CELL_WINDOW_DIRTY,
	HOUSE_CELL_WINDOW_BOARDED,
	HOUSE_CELL_WINDOW_BARRED,
	HOUSE_CELL_WINDOW_FREE,
	HOUSE_CELL_SHUTTER_PLAIN,
	HOUSE_CELL_SHUTTER_HEART,
	HOUSE_CELL_SHUTTER_LOUVRED,
	HOUSE_CELL_SHUTTER_BRACED,
	HOUSE_CELL_DOOR_STRAPS,
	HOUSE_CELL_DOOR_STUDDED,
	HOUSE_CELL_DOOR_FREE,
	HOUSE_CELL_DOOR_FREE_2,
	HOUSE_CELL_JOIST_END_PLAIN,
	HOUSE_CELL_JOIST_END_CHAMFERED,
	HOUSE_CELL_BEAM_END_ROSETTE,
	HOUSE_CELL_CORBEL,
	HOUSE_CELL_POST_HEAD,
	HOUSE_CELL_END_GRAIN_CRACKED,
	HOUSE_CELL_CARVING_FREE,
	HOUSE_CELL_CARVING_FREE_2,
	HOUSE_CELL_LANTERN,
	HOUSE_CELL_FLOWER_BOX,
	HOUSE_CELL_PLAQUE,
	HOUSE_CELL_SUNDIAL,
	HOUSE_CELL_HORSESHOE,
	HOUSE_CELL_HOOK_ROPE,
	HOUSE_CELL_ORNAMENT_FREE,
	HOUSE_CELL_ORNAMENT_FREE_2,
	HOUSE_CELL_SIGN_INN,
	HOUSE_CELL_SIGN_SMITH,
	HOUSE_CELL_SIGN_BAKER,
	HOUSE_CELL_SIGN_TAILOR,
	HOUSE_CELL_SIGN_HERBALIST,
	HOUSE_CELL_SIGN_BOAR,
	HOUSE_CELL_SIGN_BLANK,
	HOUSE_CELL_SIGN_FREE,
	HOUSE_CELL_COUNT
} house_cell_t;

typedef struct
{
	house_kind_t kind;
	int size;                              // 1 small .. 3 big, within its kind
	house_wall_t wall;
	char preset[HOUSE_NAME_LENGTH];        // what it started from (only shown)
	uint32_t seed;
	char output[HOUSE_PATH_LENGTH];        // the .pobj (under the data directory)
	// A module (module_format.h) to make of it as well, if the building has
	// parts that move (a mill's sails): its name (data/modules/NAME.pmod,
	// its parts data/objects/NAME_PART.pobj; empty: none), and the recipes
	// of the parts that need their own (comma separated, next to this one:
	// the sails' canvas; the others are this recipe with a setting changed)
	char module[HOUSE_NAME_LENGTH];
	char module_recipes[HOUSE_PATH_LENGTH * 2];
	// The textures, as the game names them (relative to data/): color,
	// normal, arm, emissive; empty: the style's stand-ins
	char textures[HOUSE_MATERIAL_COUNT][4][HOUSE_PATH_LENGTH];
	float texture_meters[HOUSE_MATERIAL_COUNT];  // one repeat of each material's textures (meters)
	float moss;                            // how much moss grows on it (0..1)
	float lichen;                          // how much lichen (0..1)
	int interior;                          // 1: everything inside kept (a house being built, its walls not yet up); 0: what cannot be seen from outside left out (the floors, the timbers' and the roof's insides: painted for nothing)

	// The body
	float width;                           // along X, outside the ground floor's walls (meters)
	float depth;                           // along Z
	int floors;                            // 1..HOUSE_MAX_FLOORS
	float floor_height;                    // a floor's, floor to ceiling beam (meters)
	float base_height;                     // the stone base's, above the ground
	float wall_thickness;
	float jetty;                           // how far each upper floor stands out over the one below, at the front and the back (meters; 0: flush)
	int jetty_sides;                       // 1: at the sides too

	// The timber frame
	float post_spacing;                    // between the posts (meters; the bays)
	float post_width;                      // a post's or a beam's side
	float panel_recess;                    // how far the panels sit back from the timbers' faces
	int braces;                            // 1: diagonal braces in the corner bays
	float bevel;                           // the timbers' edges, chamfered this wide (meters; the first level of detail drops it)

	// The roof
	house_roof_t roof;
	float roof_pitch;                      // degrees
	float overhang;                        // the eaves and the gables past the walls (meters)
	float roof_thickness;                  // the slab's
	float roof_sag;                        // how far the ridge and the slopes sag in their middle (meters; an old roof)
	int chimney;                           // 1: a chimney
	float chimney_width;

	float eave_kick;                       // how far the slopes flatten up at the eaves (meters; a bell cast)
	float hip_fraction;                    // a half hip: how far down from the ridge the gable is cut (0..1 of the half span)
	int thatch;                            // 1: thatched: thick, its eaves and verges rolled round, a fat ridge, no boards
	int dormers;                           // dormers on each slope, at most (0: none; how many is the seed's)
	float dormer_width;
	int dormer_kind;                       // 0: gabled, 1: shed (a lid of boards sloping back), 2: either, the seed's
	float gable_jetty;                     // the gable stands out this far over the top floor, on joists' ends with brackets (meters; 0: flush)
	int oriel;                             // 1: an oriel window on the front of the first floor, roofed, on brackets

	// The openings. A timber framed wall gets windows in bands of lights
	// between mullions filling a bay (window_lights at most, each
	// light_width wide); a stone wall and a dormer get single windows
	// (window_width)
	float window_width;
	float window_height;
	float sill_height;                     // the sills above a floor's floor
	int window_lights;
	float light_width;
	int shutters;                          // 1: shutters beside the single windows
	int glazed;                            // 1: glass in the windows; 0: a plank shutter closes the opening (the poor had no glass)
	int windows;                           // 0: no windows at all (a barn)
	int sign;                              // a hanging sign by the door: 0 none, -1 by the kind (an inn's, a craftsman's), else the atlas cell (house_cell_t)
	int loft_door;                         // 1: a hatch high in the front gable, with a hoist beam over it (a barn's hayloft)
	int slits;                             // ventilation slits in the long walls, per wall (a barn's)
	float ivy;                             // how much ivy climbs the walls (0 none .. 1 a lot)
	int ivy_kind;                          // 0 ivy (evergreen), 1 Virginia creeper (red in the autumn, bare in the winter), 2 some of each
	int ivy_windows;                       // 1: over the windows and doors too, up to the eaves (an abandoned house, a ruin); 0: kept off them
	float footing_bulge;                   // a church's stone footing's height map: a stone's middle out (m)
	float footing_knobs;                   //   its faces' knobs (m)
	float footing_joint_depth;             //   the joints worn between them (m)
	float footing_joint_width;             //   the joints' valleys' width (m)
	float footing_joint_dark;              //   their bottoms' darkness (0..1)
	float footing_stone_size;              //   a stone's size along (m)
	float footing_cell;                    //   the surface's cells (m)
	float footing_relief;                  //   from its base texture's _height picture: black to white (m)
	float cabin_length;                    // the log cabin's (workshop 15) main block's length (log centers, m; 0: 10)
	int cabin_no_porch;                    // 1: the log cabin without its porch
	int cabin_stone;                       // 1: the log cabin's walls dry laid stone (its gables still logs)
	int millstone_triplanar;               // 1: the hard stone triplanar (millstone_meters a repeat), not by texture coordinates
	int floor_triplanar;                   // 1: the floor material triplanar (a heap's: no stretching down its slopes)
	int ceiling_triplanar;                 // 1: the ceiling material triplanar (a farm's hay)
	int timber_bands;                      // 1: each timber a band of the timbers' texture of its own (a picture repeating only along the grain)
	int church;                            // a wooden church, not a house: 1 the Transylvanian kind (logs, a tall roof over a porch, a tower and spire); its own sizes from width, depth, floor_height, overhang, porch_depth, roof_pitch
	float tower_height;                    // a church's tower to its cross's top (meters)
	int church_turrets;                    // little spires round a church's spire: 1, 0, -1 by the seed
	int mill;                              // a post mill: 1 its body, 2 its sails (a model of their own)
	float mill_height;                     // its body's floor over the ground (meters)
	float sail_length;                     // its sails, hub to tip (0: by its height)
	float sail_cloth;                      // how much of its sails is spread (0 .. 1)
	int mill_roundhouse;                   // its trestle in a roundhouse (1) or open on its piers (0)
	float weatherboard_lip;                // its boards' feet out over the next (meters; 0: flat, the texture's boards)
	int watermill;                         // a water mill: 1 its house, 2 its wheel (a model of its own), 3 both
	int workshop;                          // a trade's yard: 1 smelter, 2 lumber yard, 3 carpenter, 4 stonecutter, 5 quarry works
	int module_piece;                      // 0: the building; N: only the Nth of its module's moving pieces, its origin what it turns or hangs from (the module's parts)
	int charcoal_open;                     // a charcoal burner's clamp (workshop 6): 1 burnt out and being opened, its charcoal raked out
	int crib_size;                         // a corn crib (workshop 12): 1 a household's (4.8 m), 2 a village's (12 by 4 m, a landing, hatches)
	float watermill_stone_height;          // its stone ground floor (meters)
	float wheel_radius;                    // its wheel's (meters)
	float wheel_width;                     // its wheel across (meters)
	int smithy;                            // a smithy, not a house: 0 none, 1 a village smith's (a gable roof on posts, a board wall along the back, a plastered hood and a chimney; the forge and the tools are objects of their own); the tiers to come: 2, 3
	int floor_on_terrain;                  // a smithy's floor laid on the ground itself (the terrain's trampled dirt under it, its footprint's cover) instead of a slab
	int smithy_furniture;                  // 1: the forge and the tools built into an open front shed (open_front) instead (kept, not used: they are to be objects of their own)
	int open_front;                        // 1: no front wall on the ground floor (a shed open to the yard): two piers and a beam across, the inside walls lined
	int log_mapping;                       // a log's texture round it, its seam underneath: 0 in meters as along it (square texels), from a random height; 1 one of three half height bands (top, bottom, middle); 2 the whole height once round
	float door_width;
	float door_height;

	// The frame's look
	int ground_floor_stone;                // 1: the ground floor is masonry (the base's stone), the timber starts above it
	int studding;                          // 1: close studding: studs every half meter in the blank bays
	int figures;                           // 1: St Andrew's crosses and braces in the blank bays
	int plaster_tint;                      // 1: the plaster colored (a pastel from the seed)
	int plaster_color;                     // -1: the tint by the seed; else the tints in turn from this one, one seed the next (a recipe's variants each its own)

	// What is added on
	int porch;                             // 1: a covered walk along the front: the ground floor set back under the floor above, on posts (one floor: a lean-to roof on posts)
	float porch_depth;
	int balcony;                           // 1: a balcony on the front of the top floor, with a railing, roofed
	float balcony_depth;
	int annex;                             // 1: a one floor lean-to against the right side

	// Age
	float wear;                            // 0: new and square .. 1: timbers leaning, panels uneven, roof wavy

	int triangles;                         // a hint: about this many at most (0: as made)
	int variants;                          // made with the next seeds too, as name_1.pobj .. name_N.pobj (potop_house --all)
	int format;                            // the .pobj file's version: 7 with the weathering baked per vertex (OBJECT_FILE_WEATHERING,
	                                       //   OBJECT_MATERIAL_AGED), 6 without (OBJECT_MATERIAL_WEATHERED, for an older engine)
} house_recipe_t;

// One of the mesh's materials: its indices are a range of the mesh's, its
// own first, then its levels of detail's (OBJECT_MATERIAL_LOD_MESHES)
typedef struct
{
	char name[HOUSE_NAME_LENGTH];
	uint32_t flags;                        // OBJECT_MATERIAL_* (weathered, alpha tested, collision only...)
	float texture_meters;                  // one repeat of its textures (meters; what its texture coordinates were made with)
	char textures[4][HOUSE_PATH_LENGTH];   // the files it names (relative to data/): color, normal, arm, emissive; none: its factors alone
	float base_color[3];                   // linear, times its texture (1 with one)
	float emissive[3];                     // linear, its own light
	float metallic;                        // times its arm's (1 with one)
	float roughness;
	size_t first_index;
	size_t own_index_count;
	size_t lod_index_count[3];             // its levels' (0: no levels, all its own)
} house_material_t;

// A place on the house for the things that go with it (a door, lanterns
// by it, the chimney's top for the smoke), in the house's frame (meters,
// yaw in degrees round Y, 0 looking down +Z)
typedef struct
{
	char name[HOUSE_NAME_LENGTH];          // door, lantern, chimney
	float position[3];
	float yaw_degrees;
} house_anchor_t;

// The module a house makes (house_recipe_t.module): its parts, the body
// (the house itself) first, and its states, as the file has them (the
// parts' models named by potop_house as it writes them); and how each part
// after the body is grown: "@N" its recipe the Nth of module_recipes, else
// settings over the body's recipe ("quarry_piece=2", ";" between several)
typedef struct
{
	module_file_t file;
	char part_recipes[MODULE_FILE_MAX_PARTS][HOUSE_PATH_LENGTH];
} house_module_t;

// The house's mesh, in memory
typedef struct
{
	house_module_t *module;                // the module it makes (house_recipe_t.module; NULL: none)
	object_file_vertex_t *vertices;
	object_file_weathering_vertex_t *weathering;  // one per vertex: how open to the sky, how much of an edge, how sheltered, what kind
	size_t vertex_count;
	size_t vertex_capacity;
	uint32_t *indices;                     // counter clockwise; the materials' one after the other, each its own then its levels'
	size_t index_count;
	size_t index_capacity;
	house_material_t materials[HOUSE_MAX_MATERIALS];
	int material_count;
	float lod_errors[3];                   // how far off each level is (meters; the game picks them by it)
	float minimum[3];
	float maximum[3];
	float moss;
	float lichen;
	int format;                            // the file version it is written as (the recipe's)
	// Its windows, each an emissive group (object_format.h, OBJECT_FILE_EMISSIVE_GROUPS;
	// version 8): the game lights them or puts them out
	object_file_emissive_group_t emissive_groups[OBJECT_FILE_MAX_EMISSIVE_GROUPS];
	int emissive_group_count;
	int window_count;                      // the windows made (past the groups' most, they share them in turn)
	house_anchor_t anchors[HOUSE_MAX_ANCHORS];
	int anchor_count;
	// The particle effects it carries (object_format.h, OBJECT_FILE_EFFECTS;
	// version 9): the chimney's smoke, a forge's fire, played by the game
	// where the house stands
	object_file_effect_t effects[OBJECT_FILE_MAX_EFFECTS];
	int effect_count;
	// Where it covers the ground (object_format.h, OBJECT_FILE_FOOTPRINT;
	// version 10): no terrain drawn under its base or floor, no grass
	object_file_footprint_t footprints[OBJECT_FILE_MAX_FOOTPRINTS];
	int footprint_count;
	// Its places for the creatures and people (object_format.h, OBJECT_FILE_SPOTS:
	// version 12): where they go and what they do there, the ways between
	object_file_spot_t spots[OBJECT_FILE_MAX_SPOTS];
	int spot_count;
	float footprint_meters;                // the radius nothing solid grows in (objects.ini)
} house_mesh_t;

// Its triangles at a level of detail (0: its own), all its drawn materials'
size_t house_mesh_triangles(const house_mesh_t *mesh, int level);

const char *house_last_error(void);

const char *house_wall_name(house_wall_t wall);
int house_wall_find(const char *name);      // -1: none
const char *house_kind_name(house_kind_t kind);
int house_kind_find(const char *name);      // -1: none
// The kind's shape at its size, over the recipe (what a recipe's "kind ="
// and "size =" lines do; the settings after them override it)
void house_recipe_apply_kind(house_recipe_t *recipe);
const char *house_roof_name(house_roof_t roof);
int house_roof_find(const char *name);      // -1: none
const char *house_material_name(house_material_kind_t kind);

void house_recipe_defaults(house_recipe_t *recipe);
int house_preset_count(void);
const char *house_preset_name(int index);
// A preset's numbers (its style, its sizes); the seed, the output and the
// textures stay. False: no such preset
bool house_preset(house_recipe_t *recipe, const char *name);
// A roof of wooden shingles (the presets' for the thatched kinds, for now)
void house_recipe_shingles(house_recipe_t *recipe);
// A roof of slate (the presets': dressed stone's, some town houses')
void house_recipe_slate(house_recipe_t *recipe);
// A roof of stone slabs (the presets': a hovel of rubble or logs)
void house_recipe_lauze(house_recipe_t *recipe);
// One setting, by its recipe key (the command line's --set KEY=VALUE)
bool house_recipe_set(house_recipe_t *recipe, const char *key, const char *value);
bool house_recipe_load(house_recipe_t *recipe, const char *path);
bool house_recipe_save(const house_recipe_t *recipe, const char *path);
// Every setting's key, with its value in the recipe, one a line (--list)
void house_recipe_print(const house_recipe_t *recipe);

// The house's mesh (its textures named: the recipe's, or the style's
// stand-ins, made under the data directory if missing)
bool house_grow(const house_recipe_t *recipe, const char *data_directory, house_mesh_t *mesh_out);
// The same without the weathering baked (every vertex open to the sky, no
// shelter): the most of the time a big house takes, for a quick look (the
// house editor's, as the numbers change); never written as it is
bool house_grow_quick(const house_recipe_t *recipe, const char *data_directory, house_mesh_t *mesh_out);
void house_mesh_free(house_mesh_t *mesh);
bool house_write(const char *path, const house_mesh_t *mesh);
// A section for layouts.ini and one for objects.ini, for this house and
// what stands by it (its anchors), printed to the file
bool house_write_sections(FILE *file, const char *object_name, const house_mesh_t *mesh);

// The stand-in textures of one material (made if missing, or always if
// remake): their names (relative to data/) into names_out (the emissive's
// empty where the material has none)
bool house_stand_in_textures(const char *name, const char *data_directory, bool remake, char names_out[4][HOUSE_PATH_LENGTH]);

// The details atlas (tools/house_atlas.c): its cells, the texture
// coordinates of a point (s, t in 0..1) across a cell, the stand-in
// painted from nothing (data/props/textures/house_details_*.png: color
// with alpha, normal, arm, emissive; made if missing or remake) and the
// template with the cells framed for a painter, with a guide (markdown)
int house_atlas_cell_count(void);
const char *house_atlas_cell_name(int cell);
void house_atlas_uv(int cell, float s, float t, float uv[2]);
void house_atlas_cell_meters(int cell, float *wide, float *high);
bool house_atlas_stand_in(const char *data_directory, bool remake, char names_out[4][HOUSE_PATH_LENGTH]);
bool house_atlas_template(const char *png_path, const char *guide_path);
// The plaster's tints (plaster_tint picks one by the seed): "plaster_<name>" is a stand-in
int house_tint_count(void);
const char *house_tint_name(int index);

#endif
