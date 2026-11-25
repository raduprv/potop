// Saved maps: everything the player made of a map, in one zip archive
// (a .potop file in the saves folder of the preference directory).
//
// The archive holds a small text header (map.ini: size, seed, the water's
// switches, springs, the camera) and one entry per layer: heights,
// bedrock, silt, soils, water depth, suspended mud, ground moisture, and
// the objects (their type names, so a changed objects.ini still matches
// what it can), and the painted sky (sky.bin, cloud_paint.h; a map
// without one gets a neutral sky). Height layers are stored as the difference of each cell
// to what its neighbors predict, which deflate packs more than twice as
// well as the raw values.
// The flow of the water is not kept: a loaded map starts with still water,
// which gets going again within a step.
//
// Loading goes in two steps, because a saved map may be of another size
// than the current one: open reads the header, the caller makes the world
// the right size, then read fills it in.
//
// Two kinds of map, told apart by their header (kind) and their file
// names (NAME.active.potop, NAME.baked.potop; NAME.potop from before is an
// active one):
//   active  everything above: the water and the ground go on (erosion,
//           the tools), to make maps with
//   baked   for play: the ground and the water as they were when it was
//           baked, and stay (no simulation, no terrain tools). Per cell
//           only what is drawn and stood on: the height in 16 bits (in
//           steps of height_step_units over height_base_units, predicted
//           as above: heights16_low.bin, heights16_high.bin), the
//           material map in two bytes (materials16_low.bin: base and
//           cover a nibble each, an index into the header's palette, 15
//           none; materials16_high.bin: the overlay a bit, the header's
//           overlay, the shelter in 7 bits); and the tiles with water, or
//           with what it left on the ground, alone, in two groups
//           (water_tiles.bin, ground_tiles.bin: the count, the cells per
//           tile, the tiles' numbers; then an entry per plane over all of
//           the group's: for the water its depth in the same height
//           steps, low and high byte, so a lake's level stays flat, and
//           its flow as the textures show it, WATER_BAKED_FLOW_BYTES; for
//           the ground the silt, wetness and bare bedrock shown,
//           TERRAIN_BAKED_SURFACE_BYTES; without tiles_split, the first
//           baked maps: one group, every plane). Every layer its own entry, the
//           big ones cut into parts of part_bytes (NAME.0, NAME.1, ...),
//           the heights predicted in bands of height_band_rows rows, so
//           they unpack and decode side by side. Objects, sky and camera as
//           an active map's. The soils go by what the ground shows.

#ifndef MAP_FILE_H
#define MAP_FILE_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"
#include "settings.h"
#include "cloud_paint.h"
#include "lights.h"
#include "objects.h"
#include "soils.h"
#include "terrain.h"
#include "water.h"
#include "waterfalls.h"

#define MAP_FILE_EXTENSION ".potop"
// Put before the extension, to tell the kinds apart by name
#define MAP_FILE_ACTIVE_SUFFIX ".active"
#define MAP_FILE_BAKED_SUFFIX ".baked"
#define MAP_FILE_NAME_LENGTH 64
#define MAP_FILE_LIST_MAX 256

typedef struct
{
	int version;
	bool baked;
	int32_t size;
	float cell_size_meters;
	uint32_t seed;
	float max_height_meters;
	bool raining;
	bool drain_edges;
	bool depth_scaled_pipes;
	bool has_camera;
	float camera_target_x;
	float camera_target_z;
	float camera_target_height;
	float camera_distance_meters;
	float camera_yaw_degrees;
	float camera_pitch_degrees;
} map_file_info_t;

typedef struct map_file_reader_s map_file_reader_t;

// What was put on the map besides the objects, for both kinds (each its
// entry: lights.bin, effects.bin, waterfalls.bin; a header of version,
// record size and count, then the records; a newer record size reads as
// far as this version knows):
//   the lights placed (the Light tool)
//   the effects placed (the Effect tool: continuous ones; on an object or
//   an actor: where it was, it does not follow it after a load)
//   the waterfalls (waterfalls.h; a baked map's alone: an active one finds
//   them in its water)
//   the items: the player's, and what lies on the ground
//   what was picked off the objects (harvest.h)
//   the actors (their type, place, name, the game's number, dead or not)
#define MAP_EFFECT_NAME_LENGTH 32

typedef struct
{
	char effect[MAP_EFFECT_NAME_LENGTH];
	float position[3];
	float rotation[4];
	float extents[3];
} map_effect_t;

// What is carried and what lies about (inventory.h, ground_items.h), by
// the items' names (items.ini may change between saves)
typedef struct
{
	int32_t group;                     // the object it lies as (its group); -1: the player's
	char item[32];
	int32_t count;
	int32_t slot;                      // worn or held there (the player's), -1: in the pack
} map_item_t;

// What was picked off the objects (harvest.h): each by its type's name and
// where it stands (the objects' order is not kept)
typedef struct
{
	char type[48];                     // as it is now
	float x;
	float z;
	int32_t harvested;
	float regrow_hours;
	char original[48];                 // what it was before it turned (empty: it did not)
} map_harvest_t;

// The actors (actors.h): their type by name, where they stand, what the
// game calls them, its number for them, dead or not
typedef struct
{
	char type[48];
	float x;
	float z;
	float yaw_radians;
	char name[32];
	uint32_t game_id;
	int32_t hostile;
	int32_t dead;
} map_actor_t;

// An area (areas.h), as kept
typedef struct
{
	uint32_t id;
	char name[32];
	float x;
	float z;
	float half_width_meters;
	float half_depth_meters;
	float yaw_radians;
	float bottom_meters;
	float top_meters;
	int32_t triggers;
	int32_t blocks;
} map_area_t;

typedef struct
{
	placed_light_t *lights;
	int light_count;
	map_effect_t *effects;
	int effect_count;
	waterfall_segment_t *waterfalls;
	int waterfall_count;
	// The sky's occlusion (sky_occlusion.h): a byte a cell, rows of
	// sky_occlusion_size (the map's size; NULL: none). In the file
	// (sky_occlusion.bin) each byte less the one before it in its row
	uint8_t *sky_occlusion;
	int sky_occlusion_size;
	// Its layers in the air (sky_occlusion.h): sky_occlusion_layer_size x
	// 4 * sky_occlusion_layer_size bytes (map_file_sky_layer_size; NULL:
	// none), sky_occlusion_layers.bin the same way
	uint8_t *sky_occlusion_layers;
	int sky_occlusion_layer_size;
	// And the light the land round sends back (sky_occlusion.h):
	// sky_occlusion_bounce_size x sky_occlusion_bounce_size x 4 bytes
	// (map_file_sky_bounce_size), sky_occlusion_bounce.bin the same way
	uint8_t *sky_occlusion_bounce;
	int sky_occlusion_bounce_size;
	// The items (items.bin)
	map_item_t *items;
	int item_count;
	// What was picked (harvest.bin)
	map_harvest_t *harvest;
	int harvest_count;
	// The actors (actors.bin); actors_saved: the file has the entry (an
	// older map's actors are left as they are)
	map_actor_t *actors;
	int actor_count;
	bool actors_saved;
	// The areas (areas.bin); areas_saved: the file has the entry
	map_area_t *areas;
	int area_count;
	bool areas_saved;
	// The map's own settings over the player's (map.ini's [settings])
	map_settings_t settings;
} map_features_t;

// The sky occlusion's layers of a map: texels per side (about a meter each,
// at most 4096)
int map_file_sky_layer_size(int map_size, float cell_size_meters);
// Its bounce texture's texels per side (the layers', at most 3072)
int map_file_sky_bounce_size(int map_size, float cell_size_meters);

// Frees what map_file_read put in it (and zeroes it)
void map_features_free(map_features_t *features);

typedef struct
{
	char name[MAP_FILE_NAME_LENGTH];   // without folder and extension (with the kind's suffix: what map_file_path takes)
	char shown_name[MAP_FILE_NAME_LENGTH]; // ...and without the kind's suffix
	bool baked;                        // by its name
	int64_t modified_nanoseconds;      // SDL_Time
	uint64_t size_bytes;
} map_file_entry_t;

// The full path of a saved map by its name (no extension). Makes the
// saves folder if needed. False if there is no preference directory.
bool map_file_path(const char *name, char *path_out, size_t path_out_size);

// A map's file name (no folder, no extension) from its name and kind:
// NAME.baked or NAME.active
void map_file_kind_name(const char *name, bool baked, char *name_out, size_t name_out_size);

// Turns what the player typed into a name that is safe as a file name
// (letters, digits, spaces, - _ and non-ASCII characters are kept).
void map_file_clean_name(const char *typed, char *name_out, size_t name_out_size);

// The saved maps, newest first. Returns how many (at most max_entries).
int map_file_list(map_file_entry_t *entries_out, int max_entries);

bool map_file_save(const char *path, const terrain_t *terrain, const water_t *water, const objects_t *objects, const camera_t *camera,
	const cloud_paint_t *sky, const map_features_t *features);

// Bakes the map (an active one, or a baked one again: as it is now) into
// a baked map file. The soil settings decide what the silt and the bare
// bedrock show, as the terrain renderer does.
bool map_file_save_baked(const char *path, const terrain_t *terrain, const water_t *water, const objects_t *objects, const camera_t *camera,
	const cloud_paint_t *sky, const soil_settings_t *soil_settings, const map_features_t *features);

// Opens a saved map and reads its header; NULL (after logging) if it is not
// a map this version can read.
map_file_reader_t *map_file_open(const char *path, map_file_info_t *info_out);

// Fills in a world that has the size and the kind from the header (a
// baked map: terrain_create_baked, water_create_baked). Objects of types
// that no longer exist are left out. Returns false if an entry is damaged;
// the world is then partly loaded and should be replaced.
bool map_file_read(map_file_reader_t *reader, terrain_t *terrain, water_t *water, objects_t *objects, cloud_paint_t *sky,
	map_features_t *features_out);

void map_file_close(map_file_reader_t *reader);

bool map_file_delete(const char *name);

#endif
