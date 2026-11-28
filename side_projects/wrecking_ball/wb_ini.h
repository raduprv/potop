// A small ini reader: [section] lines, key = value lines, ';' and '#'
// start comments. Each key goes to the callback with its section (empty
// before the first one); the callback says whether it knew the key.

#ifndef WB_INI_H
#define WB_INI_H

#include <stdbool.h>

typedef bool (*wb_ini_callback_t)(void *user, const char *section, const char *key, const char *value);

// False when the file cannot be read; keys the callback does not know
// are logged, not an error
bool wb_ini_parse_file(const char *path, wb_ini_callback_t callback, void *user);

#endif
