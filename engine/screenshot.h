// Saves a frame as a PNG.

#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>

#include <stdint.h>

// Writes RGBA8 pixels (rows from the top, as gpu_end_frame downloads them)
// to path. Returns false on failure.
bool screenshot_save(const char *path, const uint8_t *rgba, int width, int height);

// Builds a timestamped file name like "screenshot_20261010_172501.png".
void screenshot_make_name(char *name_out, int name_out_size);

#endif
