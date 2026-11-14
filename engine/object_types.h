// Object types: what can stand on the map, read from data/objects.ini.
//
// A type is a model plus everything the game needs to know about it: how
// big it may be, how far away it is still drawn, whether it casts shadows,
// how it takes part in the physics (see physics.h), and where it grows
// when the map is populated or a brush plants it. Several types may use
// the same model (a small and a big variant of a rock, say).
//
// objects.ini has one section per type, named after the type; a [defaults]
// section sets values for the types that follow it. The keys are explained
// at the top of the file.

#ifndef OBJECT_TYPES_H
#define OBJECT_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "light_source.h"
#include "module_file.h"

#define OBJECT_TYPE_NAME_LENGTH 48
#define OBJECT_TYPE_PATH_LENGTH 128
#define OBJECT_TYPES_MAX 512
#define OBJECT_CATEGORY_NAME_LENGTH 32
#define OBJECT_CATEGORIES_MAX 32
#define OBJECT_MODULES_MAX 32

typedef enum
{
	OBJECT_SHADOW_NONE = 0,        // casts no shadow
	OBJECT_SHADOW_NEAR,            // only in the fine shadow map around the camera's target
	OBJECT_SHADOW_ALL              // in both shadow maps
} object_shadow_t;

#define OBJECT_DEBRIS_MAX 8

// How an object takes part in the physics (see objects.ini)
typedef enum
{
	OBJECT_PHYSICS_NONE = 0,       // only drawn
	OBJECT_PHYSICS_FLOOD,          // no body; the water flattens or destroys it
	OBJECT_PHYSICS_STATIC,         // a body that never moves
	OBJECT_PHYSICS_BODY            // a body that moves (once loose, if anchored)
} object_physics_t;

typedef enum
{
	OBJECT_FLOOD_STAY = 0,
	OBJECT_FLOOD_FLATTEN,
	OBJECT_FLOOD_DESTROY
} object_flood_t;

typedef enum
{
	OBJECT_SHAPE_AUTO = 0,         // capsule for long things, hull otherwise
	OBJECT_SHAPE_SPHERE,
	OBJECT_SHAPE_CAPSULE,
	OBJECT_SHAPE_HULL,
	OBJECT_SHAPE_MESH              // the model's triangles (static bodies only)
} object_shape_t;

typedef struct
{
	char name[OBJECT_TYPE_NAME_LENGTH];
	char model_path[OBJECT_TYPE_PATH_LENGTH]; // relative to data/
	int model;                     // index into objects_t.models, -1 if it did not load
	int category;                  // index into object_types_t.categories

	// Looks
	float scale_minimum;           // each placed object gets a random size in this range
	float scale_maximum;
	bool random_yaw;               // turned at random (else facing +Z)
	float yaw_spread_degrees;      // turned at random only this far either way from +Z (0: all the way round)
	float front_degrees;           // where the model's front (door, opening) points: 0 = +Z, 90 = +X
	float draw_distance_meters;    // fades out (dithered) over the last part of this
	float lod_distance_scale;      // its levels of detail that much farther off (times the settings' objects.lod_distance_scale; 1: as they are)
	object_shadow_t shadow;
	float footprint_meters;        // radius nothing else solid may grow in (0 = none)
	// A building's (its OBJECT_MATERIAL_AGED materials, building.frag): how
	// weathered (0 new .. 1 very old), how much that differs from one to the
	// next (by where it stands), how high the dirt splashes up its walls
	float age;
	float age_random;
	float dirt_meters;
	bool floats;                   // rides the water surface (lily pads)
	// Turning all the time (a windmill's sails): round spin_axis (model
	// space, unit) through its model's origin, spin_rpm turns a minute; its
	// origin spin_pivot (meters, as it stands) from the spot it is placed
	// on (spin = x, y, z, rpm; spin_pivot_meters = x, y, z)
	bool spins;
	float spin_axis[3];
	float spin_rpm;
	float spin_pivot[3];
	bool impostor;                 // far away, drawn as a picture of itself (see impostors.h), if its model is worth one
	// A module (module_format.h: module = NAME, data/modules/NAME.pmod):
	// its index in object_types_t.modules (-1: a plain object). Its model is
	// its first part's (the body), its physics, shadow and impostor that
	// part's flags; the other parts are types of their own, made when it is
	// read (named TYPE.PART, after it in the list), placed with it
	char module_path[OBJECT_TYPE_PATH_LENGTH];
	int module;
	// A module's part (not the body): its module's type (-1: none) and which
	// part it is. Never placed on its own: the editor's lists leave it out
	int module_type;
	int module_part;
	// What it leaves on the ground round it (material_map.h): a cover
	// (needles, leaves, trampled ground) or the petals in its bloom, so far
	// round (meters at size 1; 0: none). (shelter, shelter_meters: read
	// and not used any more: how much falls under it comes from the sky's
	// occlusion)
	// Its season (season.h): there from the start of first_month to the end
	// of last_month (each plant its own day in the first and last half
	// month), none the rest of the year; 0: all year
	float months_first;
	float months_last;
	uint8_t cover_material;        // material_type_t, MATERIAL_NONE (255): none
	float cover_meters;
	float shelter;
	float shelter_meters;
	// In the wind (object.vert; render.wind_strength): how far its top
	// leans (meters at size 1, in a gale), and how much its leaves (the cut
	// out materials) shake (meters)
	float sway_meters;
	float flutter_meters;
	float stiffness;               // how hard the wind must blow before it bends (0..1: plant.vert)
	// Given in its own section (not [defaults]): they win over its model's
	// own (a plant's pobj carries its wind from tools/plant_editor)
	bool sway_own;
	bool flutter_own;
	bool stiffness_own;
	light_source_t light;          // the light it gives (torches, candles): see lights.h

	// Physics
	object_physics_t physics;
	float mass_kilograms;          // at size 1; 0: shape volume times density
	float density_kilograms_per_cubic_meter; // below 1000 it floats
	float anchor_newtons;          // force that tears it loose (0 = loose), at size 1
	bool falls_when_uprooted;      // trees tip over before they drift
	object_shape_t shape;
	bool blocks_water;             // a wall for the water simulation
	bool blocks_water_given;       // set in the ini (else it follows physics)
	// Flood (physics = flood)
	object_flood_t flood;
	float flood_speed_meters_per_second;
	float flood_depth_meters;
	float flood_recover_seconds;   // 0: flattened for good
	// Damage: intact -> damaged -> destroyed
	float damage_newtons;          // 0: unbreakable
	char damaged_model_path[OBJECT_TYPE_PATH_LENGTH];   // empty: none
	char destroyed_model_path[OBJECT_TYPE_PATH_LENGTH];
	int damaged_model;             // index into objects_t.models, -1 if none
	int destroyed_model;
	char debris_names[OBJECT_DEBRIS_MAX][OBJECT_TYPE_NAME_LENGTH];
	int debris_types[OBJECT_DEBRIS_MAX];  // resolved after loading, -1 unknown
	int debris_type_count;
	int debris_count;              // how many pieces it throws out
	// Picked by hand (E on foot, harvest.h): what it gives (items.ini's;
	// empty: nothing), how many at a time, whether it goes (a weed) or how
	// many times it can be picked (a willow's twigs), after how many game
	// hours it can be again (0: never), what it turns into while picked
	char harvest_item[OBJECT_TYPE_NAME_LENGTH];
	int harvest_count_minimum;
	int harvest_count_maximum;
	bool harvest_removes;
	int harvest_times;
	float harvest_regrow_hours;
	char harvest_change_object[OBJECT_TYPE_NAME_LENGTH];
	int harvest_change_type;       // resolved after loading, -1 none

	// Where it grows: number per hectare where everything below allows it
	float spawn_per_hectare;       // when the map is populated (0 = never)
	float brush_per_hectare;       // when planted with the brush (0 = never; unset: as dense as inside its patches)
	uint8_t spawn_soils;           // bit mask of soil_type_t
	float spawn_slope_maximum_degrees;
	float spawn_height_minimum;    // fraction of the map's highest point
	float spawn_height_maximum;
	float spawn_spacing_meters;    // grid the candidates are placed on
	// Patches: a noise field shared by every type naming the same patch
	// (forest, flowers); clustering 0 ignores it, 1 grows only inside the
	// patches; inverted grows outside them (meadows between the forests)
	char spawn_patch[OBJECT_CATEGORY_NAME_LENGTH];
	bool spawn_patch_inverted;
	float spawn_clustering;
	float spawn_patch_meters;      // typical patch size
	// Valleys: -1 avoids them, 1 seeks them; valley_minimum (-1..1) only
	// allows places at least this deep in a valley
	float spawn_valley_preference;
	float spawn_valley_minimum;
	// Water standing on the ground at that moment (only matters when the
	// map is repopulated or planted after it has water)
	float spawn_water_minimum_meters;
	float spawn_water_maximum_meters;
} object_type_t;

typedef struct
{
	object_type_t types[OBJECT_TYPES_MAX];
	int type_count;
	char categories[OBJECT_CATEGORIES_MAX][OBJECT_CATEGORY_NAME_LENGTH];
	int category_count;
	// Values that sections after [defaults] start from
	object_type_t defaults;
	char default_category[OBJECT_CATEGORY_NAME_LENGTH];
	// The modules the types name (module = ), read with them
	module_file_t *modules;
	int module_count;
} object_types_t;

// Reads data/objects.ini. Returns false if the file is missing; types with
// errors are logged and kept with what could be read.
bool object_types_load(object_types_t *types);
void object_types_free(object_types_t *types);

// Index of the type or category with that name, -1 if there is none
int object_types_find(const object_types_t *types, const char *name);
int object_types_find_category(const object_types_t *types, const char *name);

// True if the brush can plant something of the category
bool object_types_category_plantable(const object_types_t *types, int category);

#endif
