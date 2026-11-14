// Tiny INI parser.
//
// Supported syntax:
//   [section]
//   key = value
//   ; comment    or    # comment   (whole line only)
// Whitespace around sections, keys and values is trimmed. Values are passed
// as raw strings; converting them is the caller's job (see the helpers).

#ifndef INI_PARSER_H
#define INI_PARSER_H

#include <stdbool.h>

// Called once per key. Return false to report an unknown or invalid key; the
// parser logs it with the line number and keeps going.
typedef bool (*ini_key_callback_t)(void *user_data, const char *section, const char *key, const char *value);

// Parses a zero terminated buffer in place (the buffer is modified).
// file_name is only used in log messages.
void ini_parse_buffer(char *buffer, const char *file_name, ini_key_callback_t callback, void *user_data);

// Loads a file from the data directory and parses it. Returns false if the
// file could not be read.
bool ini_parse_data_file(const char *relative_path, ini_key_callback_t callback, void *user_data);

// Value helpers. They return false (leaving *value_out untouched) when the
// text is not a valid number.
bool ini_value_to_float(const char *text, float *value_out);
bool ini_value_to_int(const char *text, int *value_out);
bool ini_value_to_bool(const char *text, bool *value_out);

// Parses "r, g, b" with components in 0..255 into 0..1 floats.
bool ini_value_to_color(const char *text, float color_out[3]);

// Splits a comma separated list in place. Returns the number of items
// written to items_out (at most max_items); items are trimmed.
int ini_split_list(char *text, char **items_out, int max_items);

#endif
