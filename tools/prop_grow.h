// Making props (rocks, stone walls, wells, wooden fences) into the game's
// objects (.pobj): what tools/potop_prop (the command line) and
// tools/prop_editor (the window) share. How they are made is told in
// prop_grow.c.
//
// A prop is made from a recipe (prop_recipe_t): its kind, its seed, its
// textures and the numbers that shape it. Recipes are kept as text
// (prop_recipe_load, prop_recipe_save; prop_plans/ holds them): one line
// per setting, "key = value". A preset (prop_preset) sets the numbers of a
// sort of prop (a boulder, a slab, a dry stone wall, a picket fence...),
// to start from.
//
// Three kinds:
//   rock    one stone (or a few: pebbles), carved out of a lump: its own
//           shape, its lumps, flat faces where it split, layers, holes
//   wall    a dry stone wall of stones laid in courses, straight, or round
//           (a well's ring)
//   fence   a wooden fence: posts, rails, pickets
// Rocks and walls are textured triplanar (OBJECT_MATERIAL_TRIPLANAR: by the
// model's own axes, one repeat every texture_meters, no texture
// coordinates, no seams), and say by their vertices where their crevices,
// edges and open sky are, for the moss, the lichen and the snow the game
// grows on them (prop.frag). A fence has texture coordinates of its own,
// each piece of wood along the grain, laid out as
// data/props/wood_template.png (prop_wood_template): the side grain in the
// top three quarters (along U, a repeat every texture_meters), four ends'
// rings in the bottom quarter.
//
// Textures: three files under the data directory (color sRGB; normal,
// OpenGL's +Y; arm: R occlusion, G roughness, B metal), named in the recipe
// as the game names them (relative to data/). None named: the style's own,
// made here once (data/props/textures/<style>_color.png...) as stand-ins
// for real ones.

#ifndef PROP_GROW_H
#define PROP_GROW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "object_format.h"

#define PROP_PATH_LENGTH 512
#define PROP_NAME_LENGTH 32

typedef enum
{
	PROP_KIND_ROCK,
	PROP_KIND_WALL,
	PROP_KIND_FENCE,
	PROP_KIND_BARREL,
	PROP_KIND_COUNT
} prop_kind_t;

typedef struct
{
	prop_kind_t kind;
	char preset[PROP_NAME_LENGTH];         // what it started from (only shown)
	uint32_t seed;
	char output[PROP_PATH_LENGTH];         // the .pobj (under the data directory)
	// The textures, as the game names them (relative to data/): color,
	// normal, arm; empty: the style's stand-ins
	char textures[3][PROP_PATH_LENGTH];
	char style[PROP_NAME_LENGTH];          // the stand-ins': granite, sandstone, slate, limestone, wood, old_wood
	// The metal's (a barrel's hoops, a bucket's handle): its textures, its
	// stand-ins' style (rusty_iron, iron)
	char metal_textures[3][PROP_PATH_LENGTH];
	char metal_style[PROP_NAME_LENGTH];
	float texture_meters;                  // one repeat of the textures (meters)
	float moss;                            // how much moss grows on it (0..1)
	float lichen;                          // how much lichen (0..1)
	int resolution;                        // cells along a rock's longest side; a wall's: along a stone (the more, the more triangles)
	float sink;                            // how deep its foot is in the ground (meters)
	int triangles;                         // rock, wall: at most about this many (fewer where it is flat; 0: as carved)

	// rock
	float length;                          // its size (meters): along X, up, along Z
	float height;
	float width;
	float boxiness;                        // 0 round .. 1 a block
	float lumps;                           // how lumpy (of its smallest half size)
	float lump_size;                       // how big the lumps (meters)
	float grain;                           // the small roughness (0..1)
	int facets;                            // flat faces where it split
	float facet_depth;                     // how deep they cut (0..1)
	float flat_bottom;                     // its foot cut flat (0: not .. 1: half of it)
	float layers;                          // layered stone: grooves between the layers (0..1)
	float layer_spacing;                   // meters
	float layer_tilt;                      // degrees
	int holes;                             // holes through it (and pits)
	float hole_size;                       // their radius, of its smallest half size
	int stones;                            // 1; more: a heap of pebbles that many, each of the size above or smaller
	float spread;                          // the heap's radius (meters)

	// wall
	float wall_length;                     // meters (a ring: ignored)
	float wall_height;
	float wall_thickness;                  // at its foot
	float stone_size;                      // a stone's length (meters)
	float ring_radius;                     // a ring (a well) this wide, its middle's; 0: a straight wall
	float ruin;                            // 0: whole .. 1: stones gone from its top
	float batter;                          // how much narrower at its top (0..0.6)

	// fence
	float fence_length;
	float fence_height;
	float post_spacing;
	float post_thickness;
	int rails;
	float rail_height;                     // a rail's board: its height, its thickness
	float rail_depth;
	float picket_spacing;                  // 0: no pickets
	float picket_width;
	float picket_point;                    // how far the pickets' tops are pointed (meters)
	float wear;                            // 0: new .. 1: leaning, sagging, gaps (a barrel's: open, staves gone)

	// barrel (a tub, a bucket)
	float barrel_height;
	float barrel_radius;                   // at its middle
	float barrel_end_radius;               // at its ends (smaller: it bulges)
	int staves;
	float stave_thickness;
	int hoops;
	float hoop_width;
	float hoop_thickness;
	int lid;                               // 1: a lid on its top; 0: open (a tub)
	int lying;                             // 1: lying on its side
	int handle;                            // 1: an iron handle over its top (a bucket)

	int variants;                          // made with the next seeds too, as name_1.pobj .. name_N.pobj (potop_prop --all)
} prop_recipe_t;

#define PROP_MAX_MATERIALS 4

// One of the mesh's materials: its indices are a range of the mesh's, its
// own first, then its levels of detail's (OBJECT_MATERIAL_LOD_MESHES)
typedef struct
{
	char name[PROP_NAME_LENGTH];
	bool triplanar;                        // its vertices' tangents are the weathering (object_format.h)
	bool weathered;                        // moss, lichen, snow on it (prop.frag)
	float texture_meters;                  // triplanar: one repeat (meters)
	char textures[3][PROP_PATH_LENGTH];    // the files it names (relative to data/); none: its factors alone
	float base_color[3];                   // linear, times its texture (1 with one)
	float metallic;                        // times its arm's (1 with one)
	float roughness;
	size_t first_index;
	size_t own_index_count;
	size_t lod_index_count[3];             // its levels' (0: no levels, all its own)
} prop_material_t;

// The prop's mesh, in memory
typedef struct
{
	object_file_vertex_t *vertices;
	size_t vertex_count;
	size_t vertex_capacity;
	uint32_t *indices;                     // counter clockwise; the materials' one after the other
	size_t index_count;
	size_t index_capacity;
	prop_material_t materials[PROP_MAX_MATERIALS];
	int material_count;
	float lod_errors[3];                   // how far off each level is (meters; the game picks them by it)
	float minimum[3];
	float maximum[3];
	float moss;
	float lichen;
} prop_mesh_t;

// Its triangles at a level of detail (0: its own), all its materials'
size_t prop_mesh_triangles(const prop_mesh_t *mesh, int level);
// Whether it has levels of detail
bool prop_mesh_has_levels(const prop_mesh_t *mesh);

const char *prop_last_error(void);

const char *prop_kind_name(prop_kind_t kind);
int prop_kind_find(const char *name);       // -1: none

void prop_recipe_defaults(prop_recipe_t *recipe);
int prop_preset_count(void);
const char *prop_preset_name(int index);
// A preset's numbers (its kind, its style); the seed, the output and the
// textures stay. False: no such preset
bool prop_preset(prop_recipe_t *recipe, const char *name);
// One setting, by its recipe key (the command line's --set KEY=VALUE)
bool prop_recipe_set(prop_recipe_t *recipe, const char *key, const char *value);
bool prop_recipe_load(prop_recipe_t *recipe, const char *path);
bool prop_recipe_save(const prop_recipe_t *recipe, const char *path);

// The prop's mesh (its textures named: the recipe's, or the style's
// stand-ins, made under the data directory if missing)
bool prop_grow(const prop_recipe_t *recipe, const char *data_directory, prop_mesh_t *mesh_out);
void prop_mesh_free(prop_mesh_t *mesh);
bool prop_write(const char *path, const prop_mesh_t *mesh);

// The style's stand-in textures (made if missing, or always if remake):
// their names (relative to data/) into names_out
bool prop_style_textures(const char *style, const char *data_directory, bool remake, char names_out[3][PROP_PATH_LENGTH]);
// The wood's layout, as a picture to paint over (or to give the one making
// the textures): the side grain's part, the four ends
bool prop_wood_template(const char *path);

#endif
