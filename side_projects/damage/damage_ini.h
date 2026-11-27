// A small reader of ini files: sections in square brackets, key = value
// lines, comments from a ; or a # to the end of the line.

#ifndef DAMAGE_INI_H
#define DAMAGE_INI_H

#include <stdbool.h>

// Called for every key; returns false for a key or a value it does not
// know (the reader then says which line that was, and goes on)
typedef bool damage_ini_key_function_t(void *user, const char *section, const char *key, const char *value);

// False when the file cannot be opened
bool damage_ini_read(const char *path, damage_ini_key_function_t *key_function, void *user);

#endif
