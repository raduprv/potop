// Growing plants (trees, bushes, the small things on the ground) into the
// game's objects (.pobj): what tools/potop_plant (the command line) and
// tools/plant_editor (the window) share. How they grow is told in
// plant_grow.c.
//
// A plant is made from a recipe (plant_recipe_t): its kind, its seed, the
// pictures it is dressed in and the numbers that shape it. Recipes are kept
// as text (plant_recipe_load, plant_recipe_save; plant_plans/ holds them):
// one line per setting, "key = value", a picture's line repeated for each
// picture.
//
// Making one takes two steps. The atlas (plant_atlas_prepare): the shared
// texture the plant's leaves, twigs, heads and fruit live in, each
// picture's months (name_m1 .. name_m12 beside it) in cells of their own,
// with the recipe's pictures put in it (in memory; plant_atlas_write writes
// it, BC7, only if it changed). Slow-ish: it reads every picture of the atlas, so a
// window keeps it while only the numbers change. Then the plant itself
// (plant_grow): fast, its mesh into memory; plant_write writes it as a
// .pobj naming the atlas and the bark.
//
// Paths in a recipe are as seen from where the tool runs (the project's
// directory); the bark's belongs under the data directory (the objects
// name their bark, the game loads it from there): one from elsewhere is
// copied to its textures/ when the object is written (unless another
// picture of that name is there).

#ifndef PLANT_GROW_H
#define PLANT_GROW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "object_format.h"

#define PLANT_MAX_PICTURES 16            // of each sort (leaves, twigs, heads, fruit)
#define PLANT_PATH_LENGTH 512
#define PLANT_MAX_LEVELS 3
#define PLANT_MAX_MONTH_CELLS 32         // a plant's pictures with months of their own

// The materials of a plant's object, in this order
typedef enum
{
	PLANT_MATERIAL_TRUNK,              // the trunk (a bush's stems): solid, what one bumps into
	PLANT_MATERIAL_BARK,               // the branches and stalks: the same bark, passable
	PLANT_MATERIAL_FOLIAGE,            // cards from the atlas
	PLANT_MATERIAL_FRUIT,              // fruit: small solids (or cards) from the atlas, lit by their own normals
	PLANT_MATERIAL_SNOW,               // the winter's snow baked in, lying on the boughs (object_format.h, OBJECT_FILE_SNOW)
	PLANT_MATERIAL_COUNT
} plant_material_t;

typedef enum
{
	PLANT_FRUIT_SPHERE,                // cherries, nuts (oval with a width of their own)
	PLANT_FRUIT_SPINDLE,               // cones
	PLANT_FRUIT_POD,                   // flat, bent a little
	PLANT_FRUIT_CARD                   // two crossed cards
} plant_fruit_shape_t;

// How fruit grows (PLANT_FRUIT_KINDS: as the kind's own)
#define PLANT_FRUIT_KINDS -1
typedef enum
{
	PLANT_FRUIT_DOWN,                  // hanging on its stalk
	PLANT_FRUIT_UP,                    // standing on the branch
	PLANT_FRUIT_OUT                    // out of it
} plant_fruit_hang_t;

typedef enum
{
	PLANT_FRUIT_OUTER,                 // the outside of the crown, where the sun is
	PLANT_FRUIT_TOP,                   // the top of the crown
	PLANT_FRUIT_ALL
} plant_fruit_zone_t;

// A picture, and what it looks like through the year: a month's own
// picture is found beside it by name (twig.png or twig_m3.png: twig_m1.png
// .. twig_m12.png), or chosen by hand (months, empty: by name). A month
// without one of its own shows the picture itself if it is a plain one
// (twig.png), else (twig_m3.png) the latest one before it, round the year
// Which cells of the atlas a picture goes in, if it is not a long one (those
// go in the tall ones): as the kind has it (the small plants' little ones,
// mushrooms and flowers' heads: the small cells), or as chosen (| small,
// | big after it in a recipe)
#define PLANT_CELL_AS_KIND 0
#define PLANT_CELL_SMALL 1
#define PLANT_CELL_BIG 2

typedef struct
{
	char color[PLANT_PATH_LENGTH];
	char months[12][PLANT_PATH_LENGTH];  // January first
	int cell_size;                       // PLANT_CELL_...
} plant_picture_t;

typedef struct
{
	plant_picture_t pictures[PLANT_MAX_PICTURES];
	int count;
} plant_pictures_t;

// A mushroom (the kind mushroom): a stem and a cap, its sizes of the
// mushroom's height. Its texture: the recipe's first leaf picture, laid out
// as data/mushrooms/mushroom_template.png (potop_plant --mushroom-template),
// else one made from the colors here (sRGB 0..1)
#define PLANT_MUSHROOM_GILLS 0          // under the cap: thin gills
#define PLANT_MUSHROOM_PORES 1          // a sponge of pores (boletes)
#define PLANT_MUSHROOM_RIDGES 2         // low forked ridges running down the stem (chanterelles)

typedef struct
{
	float size;                        // all of it this many times as big (bigger than life if you like; 0: 1)
	float stem_length;                 // the stem's length (0: what the cap leaves of the height)
	float stem_thickness;              // its radius at its middle
	float stem_taper;                  // its radius at its top, of its foot's
	float stem_bulb;                   // its foot swollen by this much of its radius (0: none)
	float stem_volva;                  // a cup round its foot (0..1; 0: none)
	float stem_bend;                   // it bends this far towards the cap (0: straight; 1: about 45 degrees)
	float ring_height;                 // a skirt round it this far up it (0..1; 0: none)
	float ring_width;                  // the skirt out from the stem, of its radius
	float ring_droop;                  // how far the skirt hangs down (0: level .. 1)
	float cap_width;                   // the cap's radius
	float cap_height;                  // how high it is
	float cap_dome;                    // 0: a cone .. 1: a dome
	float cap_funnel;                  // its middle down and its rim up, a funnel (0..1)
	float cap_umbo;                    // a bump in its middle (0..1)
	float cap_curl;                    // its rim bent down (+) or up (-)
	float cap_wave;                    // its rim wavy and uneven (0..1)
	int underside;                     // PLANT_MUSHROOM_GILLS, _PORES, _RIDGES
	// Its colors and their patterns (for the texture made from them)
	float cap_color[3];
	float cap_rim_color[3];            // towards its rim
	float cap_center_color[3];         // in its middle
	float cap_center;                  // how far out the middle's color goes (0..1)
	float spots;                       // warts on it, how many (0..1)
	float spot_size;                   // how big (0..1; 0: none set, as 0.5)
	float scales;                      // scales in rings round it, how many (0..1)
	float spot_color[3];               // the warts' and the scales'
	float streaks;                     // fine streaks out from the middle (0..1)
	float zones;                       // rings of the middle's color round it (0..1)
	float gills_color[3];
	float stem_color[3];
	float stem_pattern_color[3];
	float stem_net;                    // a net over the stem's upper part (0..1)
	float stem_bands;                  // zigzag bands down it (0..1)
} plant_mushroom_t;

// The mushrooms the generator knows to start from (fly_agaric, porcini ...)
int plant_mushroom_preset_count(void);
const char *plant_mushroom_preset_name(int preset);
bool plant_mushroom_preset(plant_mushroom_t *mushroom, const char *name);   // false if there is none of that name

void plant_mushroom_defaults(plant_mushroom_t *mushroom);
// One of its values by its recipe key (stem_thickness ... stem_color: a
// color as "r, g, b"; preset = NAME: all of them as that preset's); false
// if the key is none of them
bool plant_mushroom_set(plant_mushroom_t *mushroom, const char *key, const char *value);

// Writes the template a mushroom's texture is laid out as (512 x 512: each
// part flat colored, outlined): the cap from above, the gills from below,
// the skirt, the stem round
bool plant_mushroom_template(const char *path);

// A small plant's shape (the kinds tuft, rosette, flower): its leaves round
// its foot, its flower stalks out of its middle. Not given: the kind's own
// (as it always grew). Burdock, plantain, foxglove...: plant_herb_preset
typedef struct
{
	bool given;                        // false: the kind's own; true: these
	int leaves;                        // round its foot (give or take one)
	float leaf_spread;                 // their sizes: 0 all alike .. 1 from half to one and a half of the leaf size
	float leaf_rise;                   // degrees up from level (0: flat on the ground .. 90: upright)
	float leaf_rise_spread;            // give or take (degrees)
	float leaf_arch;                   // how far they bend over along their length (0: straight; 1: much)
	float leaf_layers;                 // the inner ones smaller and steeper (0..1)
	float petiole;                     // a stalk under each leaf (meters at the leaf size; 0: none)
	float petiole_rise;                // its degrees up from level
	float petiole_thickness;           // its radius at its foot (meters at the leaf size; 0: 0.008 of the leaf's size), thinner to the leaf
	// Runners: stems along the ground out of its foot, forking, leaves (or
	// sprigs: a picture of a leafy twig) up along them (creeping plants:
	// lingonberry, creeping juniper, ivy, thyme)
	int runners;                       // how many (0: none)
	float runner_length;               // meters, give or take a fifth
	int runner_forks;                  // each forks this many times (0..4)
	float runner_wander;               // how much they wind (0: straight .. 1)
	float runner_thickness;            // their radius (meters)
	int runner_leaves;                 // leaves (or sprigs) along each, its forks as many for their length
	float runner_leaf_size;            // of the leaf size, smaller to the tips (0: 1)
	float runner_leaf_rise;            // degrees up from level (0: 35)
	float runner_leaf_turn;            // degrees out from the runner's way, give or take (0: 50)
	int stalks_minimum;               // flower stalks out of its middle
	int stalks_maximum;
	float stalk_height;                // of its height
	float stalk_lean;                  // degrees, at random
	float stalk_thickness;             // meters (their radius)
	int stalk_leaves;                  // leaves up each stalk
	float stalk_leaves_from;           // from this much up it (0..1; 0 and 0: 0.1 to 0.6)
	float stalk_leaves_to;             // to this much
	float stalk_leaf_size;             // their size, of the leaf size (0: 0.55)
	float stalk_leaf_taper;            // the topmost this much smaller (0: all alike .. 1: none left at the top)
	float stalk_leaf_rise;             // degrees up from level (0: 30)
	float stalk_leaf_arch;             // how far they bend over
	float stalk_petiole;               // a stalk under each (meters at the leaf size; 0: none)
	float stalk_petiole_rise;          // its degrees up from level (0: 37)
	float stalk_petiole_thickness;     // its radius at its foot (meters at the leaf size; 0: 0.006 of the leaf's size), thinner to the leaf
	float stalk_leaf_droop;            // the lowest hang this many degrees lower than the topmost (their stalks half as much)
	int stalk_leaf_spiral;             // 1: round the stalk one after the other (the golden angle); 0: at random
	float stalk_leaf_scatter;          // round it, give or take this many degrees from the spiral
	float head_size;                   // a head's, of its height
	int heads;                         // on each stalk: 1 on its top; more: up its top part
	float head_spike;                  // how much of the stalk's top they are along (0..1)
	float head_lean;                   // the ones under the top one stand out from the stalk this many degrees (0: upright round it)
	// The head on top: PLANT_HEAD_CROSSED (two cards crossed), _CUP (the
	// flower seen from above on a disc, a cup of a sphere under it with the
	// flower seen from below: name_under.png beside its picture), _FLAT
	// (the disc alone)
	int head_shape;
	float head_tilt;                   // a disc's, at random up to (degrees)
	float head_tilt_minimum;           // and at least (a sunflower's faces the side)
	float cup_size;                    // the cup's radius, of the disc's (0..1)
	float cup_depth;                   // how deep, of its radius (0.7: a third of a sphere)
	float disc_curve;                  // the disc's top: 0 its middle up a little, below 0 hollow (a sunflower's: -0.8), above 0 domed (-1..1)
	float head_neck;                   // the stalk's top this much of it bends over into the head's tilt (0: straight up to it .. 0.5)
	// Their faces to the light (the object's +Z, up 45 degrees: the game
	// turns such a plant at random only as far as its type's yaw_spread)
	float leaf_light;                  // how far the leaves turn their faces to it (0: not .. 1: all the way)
	float light_spread;                // each its own way, give or take this many degrees round from it
	float head_light;                  // 1: the heads (discs) tilt towards it too (give or take as much); 0: any way
	float leaf_twist;                  // each leaf rolled round its middle at random, give or take this many degrees
	float leaf_fold;                   // the leaves folded along their middle, a V (0: flat .. 1: much)
} plant_herb_t;

#define PLANT_HEAD_CROSSED 0
#define PLANT_HEAD_CUP 1
#define PLANT_HEAD_FLAT 2

// Small plants' shapes: the kind's own (the numbers it grows by when the
// recipe gives none), the presets (burdock, plantain...: the kind, the
// leaf size, the height and the shape), one value by its recipe key
// (leaves, leaf_rise ...: as a recipe names them without "herb_")
void plant_herb_kind_defaults(int kind, plant_herb_t *herb_out);
int plant_herb_preset_count(void);
const char *plant_herb_preset_name(int preset);
const char *plant_herb_preset_kind(int preset);
bool plant_herb_set(plant_herb_t *herb, const char *key, const char *value);

// Everything a plant is made from. 0 (or -1 where 0 means something) is
// "the kind's own"
typedef struct
{
	char kind[32];                     // see plant_kind_name
	unsigned seed;                     // which one of the kind
	float height;                      // meters, about (0: the kind's, give or take 15%)
	char atlas[64];                    // another atlas than the kind's (empty: the kind's)
	char output[PLANT_PATH_LENGTH];    // where the object goes (data/objects/name.pobj)
	char bark[3][PLANT_PATH_LENGTH];   // color, normal, arm (under the data directory; empty: none)
	float bark_meters;                 // how much bark one tile covers (1.5)
	// The wind in it (written into the object; objects.ini's sway, flutter,
	// stiffness in its type's own section win; below 0: the kind's, by its
	// height and leaf size): its top's lean in a gale (meters), its leaves'
	// shaking (meters, at their tips), how hard it must blow before it bends
	// (0: at once .. 1: never; a bush stands still but in a storm)
	float sway;
	float flutter;
	float stiffness;
	float stalk_color[3];              // the stalks' and branches' plain color without a bark picture (sRGB 0..1; the first below 0: the kind's)
	plant_pictures_t leaves;          // leaves on alpha, the stem at the bottom middle (any one at random)
	plant_pictures_t twigs;            // trees: sprigs that stand for the finest branches
	plant_pictures_t heads;            // flower heads (rosettes, flowers)
	plant_pictures_t fruit;            // trees: fruit, seen from the side, filling the picture
	float leaf_size;                   // a leaf's length (meters)
	int leaves_per_branch;             // trees: on each finest branch (-1: the kind's)
	int branches[PLANT_MAX_LEVELS];    // trees: branches out of the trunk, out of each of those...
	int branch_levels;                 // how many of branches are given (0: the kind's)
	float branch_length;               // trees: the trunk's branches this many times as long (0: 1)
	float branch_lengths[PLANT_MAX_LEVELS]; // trees: those out of them ([1]), out of those ([2])... this many times as long, of their parent (0: 1; [0] unused)
	float twig_scale;                  // trees: a twig's length, of the branch it stands for
	float twig_meters;                 // trees: a finest branch longer than this has twigs one after the other along it (0: one; -1: the kind's)
	float twig_share;                  // trees with twigs and leaves: the share of the finest branches that are twigs (0.7)
	float twig_gap;                    // trees with twigs: twigs added so no branch is bare longer than this (meters; 0: as grown)
	bool curved_twigs;                 // trees (for now the fir only): each twig one card, arched across and drooping, instead of two crossed
	bool brushes;                      // trees (pines): each twig a spray of brushes in the round, its pictures one shoot side on, the head picture one end on
	int brush_strips;                  // brushes: the narrow strips round each needle cone (10)
	float brush_spacing;               // brushes: the gap between cones along a shoot, in needle lengths (0.5)
	bool snow_bake;                    // trees of whole boughs (fir_squares): the winter's snow baked in (object_format.h, OBJECT_FILE_SNOW)
	float loose_leaves;                // trees: leaves along the other branches too, per meter (0)
	plant_fruit_shape_t fruit_shape;
	float fruit_length;                // meters along its axis (0.03)
	float fruit_width;                 // 0: from the picture's shape
	int fruit_hang;                    // plant_fruit_hang_t, or PLANT_FRUIT_KINDS
	int fruit_zone;                    // plant_fruit_zone_t, or PLANT_FRUIT_KINDS
	float fruit_stalk;                 // meters (0: none)
	int cluster_minimum;               // fruit together, their stalks from one point (1)
	int cluster_maximum;
	float fruit_per_branch;            // clusters on each finest branch or twig, on average (1)
	float fruit_bunch;                 // 0: a cluster's stalks from one point, fanned out; up to 1: packed close on a short stalk hanging from the branch (more down)
	float fruit_tip;                   // 0: fruit anywhere on a finest branch's outer part; 1: at its tip
	// The crown's shape (trees; below 0: the kind's)
	float crown_start;                 // the trunk's first branches, of its height (0..1)
	float leaves_from;                 // foliage only above this much of the height (0: all the way down)
	float dead_branches;               // dead bare branches on a stem under the crown (below 0: the kind's)
	float trunk_upright;               // the trunk kept straight the way it set out (0 wanders .. 1 straight; below 0: the kind's)
	float light_from_below;            // the foliage's normals out from this many heights under the crown's foot (a mat lit from above; below 0: the kind's)
	float droop;                       // how far the branches bend down under their weight
	float tip_lift;                    // how far their tips turn up again
	float collar;                      // how much a branch's foot swells where it joins (of its radius)
	float root_flare;                  // the trunk's foot, towards the roots
	// The thickness (trees; below 0: the kind's)
	float trunk_thickness;             // the trunk's radius at its foot, of the height
	float trunk_taper;                 // its radius at its top, of its foot's
	float branch_thickness;            // a branch's radius at its foot, of its parent's there
	float branch_taper;                // its radius at its tip, of its foot's
	float thinnest_branch;             // no branch thinner than this (radius, meters)
	float frond_angle;                 // ferns: their fronds start this steep (degrees up from level; below 0: 58)
	float frond_droop;                 // and arch over this much (below 0: 0.7; 1: much)
	plant_mushroom_t mushroom;         // a mushroom's shape and colors
	plant_herb_t herb;                 // a small plant's shape (tuft, rosette, flower)
	// The trunk (trees; below 0: the kind's)
	int stems;                         // trunks (-1: the kind's)
	float fork_height;                 // they grow as one up to this much of the height, then part (0: apart from the ground)
	float extra_stem_chance;           // the chance (by the seed) of one more stem than that
	float gnarl;                       // the branches and the trunk wander and swell and narrow (0: smooth)
	float kinks;                       // a branch turns away where another leaves it (0: none)
	float lean_degrees;                // the trunk leaves the ground this far from upright (the wind's)
	float lean_length;                 // and goes so for this much of its length
	float straighten;                  // then turns up again: 1 upright at its top, 0 not at all
	float wind_side;                   // the branches on the side the wind comes from (against the lean) shorter and fewer
	// Through the seasons (below 0: the kind's)
	float moss;                        // the moss on the bark's north side (0: none)
	float tips;                        // how much lighter the foliage's tips are (the new growth)
	float autumn_colors[2][3];         // what the foliage turns to in the autumn (sRGB 0..1; a first one below 0: the kind's)
	float autumn;                      // how much it turns (0: evergreen)
	int falls;                         // 1: it falls once turned, 0: it stays (-1: the kind's)
	// The autumn's months (1..12; 0: the kind's): it turns from the start of
	// the first, falls (where it does) by the end of the last, and is bare
	// until the leaves' month starts
	int autumn_first_month;
	int autumn_last_month;
	int leaves_month;
	float snow_sag;                    // how far the branches sag under the winter's snow (of the height, at the crown's edge; below 0: the kind's)
} plant_recipe_t;

// A small plant's preset: its kind, its leaf size, its height and its
// shape (plant_herb_t) into the recipe; false if there is none of that name
bool plant_herb_preset(plant_recipe_t *recipe, const char *name);

// The plant as grown: its vertices, and the triangles of each material
typedef struct
{
	object_file_vertex_t *vertices;
	size_t vertex_count;
	uint32_t *indices[PLANT_MATERIAL_COUNT];
	size_t index_count[PLANT_MATERIAL_COUNT];
	float minimum[3];
	float maximum[3];
	float height;                      // as grown (meters)
	int fruit_count;
	bool tree;                         // a tree or a bush (else a small plant)
	bool still;                        // the wind leaves it be (a mushroom: object_format.h, OBJECT_FILE_STILL)
	float wind[3];                     // its sway, flutter, stiffness (plant_recipe_wind), written into it
	// What each vertex is on the plant, and the materials through the
	// seasons (object_format.h, OBJECT_FILE_PLANT)
	object_file_plant_vertex_t *plant_vertices;
	object_file_plant_material_t plant_materials[PLANT_MATERIAL_COUNT];
	// The snow baked in: where each vertex goes under all of it (NULL: none)
	object_file_snow_vertex_t *snow_vertices;
	// Levels of detail (object_format.h: OBJECT_MATERIAL_LODS, _SHUFFLED)
	bool lods[PLANT_MATERIAL_COUNT];
	bool shuffled[PLANT_MATERIAL_COUNT];
	uint32_t lod_index_count[PLANT_MATERIAL_COUNT][3];
	// Its pictures that show others in some months (object_format.h)
	object_file_month_cell_t month_cells[PLANT_MAX_MONTH_CELLS];
	int month_cell_count;
} plant_mesh_t;

typedef struct plant_atlas plant_atlas_t;

// The last thing that went wrong (also written to stderr when it does)
const char *plant_last_error(void);

// The kinds
int plant_kind_count(void);
const char *plant_kind_name(int kind);
int plant_kind_find(const char *name);       // its name or alias; -1 if none
bool plant_kind_is_tree(int kind);
bool plant_kind_is_mushroom(int kind);   // made of the recipe's mushroom shape (plant_mushroom_t); its leaf: its texture
// What a kind does when the recipe leaves it to the kind (for a window to show)
float plant_kind_height(int kind);
float plant_kind_leaf_size(int kind);
int plant_kind_leaves_per_branch(int kind);
int plant_kind_levels(int kind);
int plant_kind_branches(int kind, int level);
float plant_kind_twig_scale(int kind);
float plant_kind_twig_meters(int kind);
plant_fruit_hang_t plant_kind_fruit_hang(int kind);
plant_fruit_zone_t plant_kind_fruit_zone(int kind);
float plant_kind_crown_start(int kind);
float plant_kind_leaves_from(int kind);
float plant_kind_dead_branches(int kind);
float plant_kind_trunk_upright(int kind);
float plant_kind_light_from_below(int kind);
float plant_kind_droop(int kind);
float plant_kind_trunk_thickness(int kind);
float plant_kind_trunk_taper(int kind);
float plant_kind_branch_thickness(int kind);
float plant_kind_branch_taper(int kind);
float plant_kind_thinnest_branch(int kind);
float plant_kind_tip_lift(int kind);
float plant_kind_collar(int kind);
float plant_kind_root_flare(int kind);
int plant_kind_stems(int kind);
float plant_kind_fork_height(int kind);
float plant_kind_extra_stem_chance(int kind);
float plant_kind_gnarl(int kind);
float plant_kind_kinks(int kind);
float plant_kind_lean_degrees(int kind);
float plant_kind_lean_length(int kind);
float plant_kind_straighten(int kind);
float plant_kind_wind_side(int kind);
float plant_kind_moss(int kind);
float plant_kind_tips(int kind);
float plant_kind_autumn(int kind);
bool plant_kind_falls(int kind);
void plant_kind_autumn_colors(int kind, float colors_out[2][3]);
// The stalks' plain color (the recipe's, else the kind's: a tree's brown, a
// small plant's green)
void plant_stalk_color(const plant_recipe_t *recipe, int kind, float color_out[3]);
// The wind a kind grows with at a height and a leaf size (sway, flutter,
// stiffness), and the recipe's (its own where given)
void plant_kind_wind(int kind, float height, float leaf_size, float wind_out[3]);
void plant_recipe_wind(const plant_recipe_t *recipe, int kind, float height, float leaf_size, float wind_out[3]);
int plant_kind_autumn_first_month(int kind);
int plant_kind_autumn_last_month(int kind);
int plant_kind_leaves_month(int kind);
float plant_kind_snow_sag(int kind);

void plant_recipe_defaults(plant_recipe_t *recipe);
bool plant_recipe_load(plant_recipe_t *recipe, const char *path);
bool plant_recipe_save(const plant_recipe_t *recipe, const char *path);
bool plant_pictures_add(plant_pictures_t *pictures, const char *color);
void plant_pictures_remove(plant_pictures_t *pictures, int index);
// What a picture shows each month (January first): sources_out the file,
// own_out whether the month has one of its own (chosen or found by name)
void plant_picture_months(const plant_picture_t *picture, char sources_out[12][PLANT_PATH_LENGTH], bool own_out[12]);

// The recipe's atlas with its pictures in it (data_directory: the game's
// data, "data"). NULL if it cannot be made (plant_last_error)
plant_atlas_t *plant_atlas_prepare(const plant_recipe_t *recipe, const char *data_directory);
// Its pixels (RGBA8, size x size)
int plant_atlas_size(const plant_atlas_t *atlas);
const uint8_t *plant_atlas_pixels(const plant_atlas_t *atlas);
const char *plant_atlas_name(const plant_atlas_t *atlas);
// Writes its .ini and pictures (those that changed)
bool plant_atlas_write(plant_atlas_t *atlas);
void plant_atlas_free(plant_atlas_t *atlas);

// Grows the plant (the atlas: plant_atlas_prepare's, for the same recipe's
// pictures). False if nothing grew (plant_last_error)
bool plant_grow(const plant_recipe_t *recipe, const plant_atlas_t *atlas, plant_mesh_t *mesh_out);
void plant_mesh_free(plant_mesh_t *mesh);

// Writes the object (a .pobj naming the atlas and the bark)
bool plant_write(const char *path, const plant_mesh_t *mesh, const plant_recipe_t *recipe, const plant_atlas_t *atlas,
	const char *data_directory);

#endif
