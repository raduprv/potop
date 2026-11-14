// A module (module_format.h) in memory, read from and written to its bytes.
// Plain C (no SDL): the tools that write modules share it with the game.

#ifndef MODULE_FILE_H
#define MODULE_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "module_format.h"

typedef struct
{
	int part_count;
	int state_count;
	int first_state;
	module_file_part_t parts[MODULE_FILE_MAX_PARTS];
	module_file_state_t states[MODULE_FILE_MAX_STATES];
	module_file_pose_t poses[MODULE_FILE_MAX_STATES][MODULE_FILE_MAX_PARTS];
} module_file_t;

// An empty module: no parts, no states
void module_file_clear(module_file_t *module);

// Adds a part (its poses in every state: in place, still, shown) or a
// state (every part's pose as in the first state, if there is one).
// Returns its index, -1 if full
int module_file_add_part(module_file_t *module, const char *name, const char *model, const float position[3], float yaw_degrees,
	uint32_t flags);
int module_file_add_state(module_file_t *module, const char *name, float seconds);

// A part's or a state's index by name, -1 if none
int module_file_find_part(const module_file_t *module, const char *name);
int module_file_find_state(const module_file_t *module, const char *name);

// From its bytes; false (with why in error, if given) if they are not a
// module this code reads
bool module_file_parse(const void *data, size_t size, module_file_t *out, char *error, size_t error_size);

// Into bytes (malloc'ed, the caller frees them); NULL if it is not a
// valid module (no parts, no states, a count too big)
void *module_file_serialize(const module_file_t *module, size_t *size_out);

// Written to / read from a file
bool module_file_write(const char *path, const module_file_t *module);
bool module_file_read(const char *path, module_file_t *out, char *error, size_t error_size);

// Everything in it, as text
void module_file_dump(const module_file_t *module, FILE *stream);

#endif
