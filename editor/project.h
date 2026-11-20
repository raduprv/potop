// Map projects: everything the editor keeps for one map, in a folder of its
// own under the preference directory (projects/NAME/):
//
//   project.ini    what it is: its name, seed, size, when it was made
//   map.potop      the map being made (active; baked if it was brought in
//                  from a baked map: that can only be saved baked)
//   baked.potop    the last bake (for play); a copy goes into the saves
//                  (NAME_baked) for the game to find
//   sketch/        the 2D sketch the terrain was made from (terrain_sketch.h:
//                  height.png, relief.png, water.png), if one was painted
//
// The editor's own: the engine knows maps by path only
// (potop_map_save_file, potop_map_load_file).
#ifndef PROJECT_H
#define PROJECT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define PROJECT_NAME_LENGTH 64
#define PROJECT_LIST_MAX 128
#define PROJECT_FOLDER "projects/"
#define PROJECT_INFO_FILE "project.ini"
#define PROJECT_MAP_FILE "map.potop"
#define PROJECT_BAKE_FILE "baked.potop"
#define PROJECT_SKETCH_FOLDER "sketch/"

typedef struct
{
	char name[PROJECT_NAME_LENGTH];
	uint32_t seed;                 // the terrain's (as made, or as last saved)
	int size_cells;                // the map's side in cells
	float cell_size_meters;
	int64_t created_seconds;       // Unix time
	bool baked;                    // its map is a baked one (brought in so)
	// What its land was made with besides (0: not known, the settings')
	float max_height_meters;
	float erosion_droplets;
	// Found by project_list (not in project.ini)
	int64_t modified_nanoseconds;  // its map's last save
	uint64_t map_bytes;
	bool has_bake;
} project_info_t;

// A file of a project (relative: PROJECT_MAP_FILE, ...; "" the folder
// itself), its folder made if missing; false if there is no preference
// directory
bool project_path(const char *name, const char *relative, char *path_out, size_t path_out_size);
// Whether a project of that name has a map
bool project_exists(const char *name);
// Writes / reads project.ini
bool project_write_info(const project_info_t *info);
bool project_read_info(const char *name, project_info_t *info_out);
// The projects with a map, the last saved first; their count
int project_list(project_info_t *infos_out, int max_infos);
// Deletes a project's folder and what is in it (its files and the sketch's)
bool project_delete(const char *name);

// The sketches kept on their own (the sketch window's Save sketch), each a
// folder under the preference directory (sketches/NAME/: the sketch's own
// files, terrain_sketch_save's, and sketch.ini, the window's map fields):
// its folder (made if missing; false if there is no preference directory),
// and their names, alphabetically, and their count
#define SKETCH_LIBRARY_FOLDER "sketches/"
#define SKETCH_LIBRARY_INFO_FILE "sketch.ini"
bool sketch_library_path(const char *name, char *path_out, size_t path_out_size);
int sketch_library_list(char (*names_out)[PROJECT_NAME_LENGTH], int max_names);

#endif
