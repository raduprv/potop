// Soft render pictures to PNG files, alone or many on a sheet.
//
// A sheet is a grid of pictures in one file (a contact sheet): a
// collapse as twelve frames, or one frame from four sides, looked at in
// one go instead of a file at a time.

#ifndef SOFT_RENDER_PNG_H
#define SOFT_RENDER_PNG_H

#include <stdbool.h>
#include <stdint.h>

#include "soft_render.h"

typedef struct
{
	int column_count;
	int row_count;
	int cell_width;
	int cell_height;
	int width;                         // of the whole sheet, the gaps between the cells too
	int height;
	uint8_t *color;                    // width * height * 4 bytes, as soft_render_t's
} soft_render_sheet_t;

bool soft_render_save_png(const soft_render_t *renderer, const char *path);

// False when out of memory (nothing to destroy then)
bool soft_render_sheet_create(soft_render_sheet_t *sheet, int column_count, int row_count, int cell_width, int cell_height);
void soft_render_sheet_destroy(soft_render_sheet_t *sheet);
// Copies a picture (of the cells' size) into a cell; they count along the
// rows, from the top left
void soft_render_sheet_put(soft_render_sheet_t *sheet, int cell, const soft_render_t *renderer);
bool soft_render_sheet_save_png(const soft_render_sheet_t *sheet, const char *path);

#endif
