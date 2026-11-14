// File access. Everything goes through SDL's IO layer (SDL_IOFromFile and
// friends) so that the same code can later read assets packed inside an
// Android APK. Do not use fopen() anywhere else in the game.

#ifndef FILE_IO_H
#define FILE_IO_H

#include <stddef.h>
#include <stdbool.h>

// Picks the data directory: "data/" relative to the working directory if it
// exists there, otherwise "data/" next to the executable. Call once at
// startup, after SDL_Init.
void file_io_initialize(void);

// Builds "<data root><relative_path>" into the caller's buffer.
void file_io_data_path(const char *relative_path, char *path_out, size_t path_out_size);

// Reads a whole file from the data directory. The returned buffer is
// zero terminated (handy for text) and must be released with SDL_free.
// Returns NULL if the file cannot be read; size_out may be NULL.
void *file_io_load_data_file(const char *relative_path, size_t *size_out);

// True if the data file exists and can be opened.
bool file_io_data_file_exists(const char *relative_path);

// Writes a buffer to a path that is NOT relative to the data directory
// (used for screenshots). Returns false on failure.
bool file_io_save_file(const char *path, const void *data, size_t size);

// Builds "<preference directory><relative_path>": the per user directory
// for saved maps, settings changed in the game and the like (on Linux
// ~/.local/share/potop/potop/). Creates the directory part of the path
// (up to the last '/') if needed. Returns false if there is none.
bool file_io_preference_path(const char *relative_path, char *path_out, size_t path_out_size);

// Reads a whole file by its full path, zero terminated, released with
// SDL_free. NULL if it cannot be read; size_out may be NULL.
void *file_io_load_file(const char *path, size_t *size_out);

#endif
