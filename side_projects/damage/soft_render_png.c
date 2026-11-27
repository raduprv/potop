// Soft render pictures to PNG files: see soft_render_png.h

#include "soft_render_png.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define SHEET_GAP_PIXELS 2
#define SHEET_GAP_SHADE 24

bool soft_render_save_png(const soft_render_t *renderer, const char *path)
{
	return stbi_write_png(path, renderer->width, renderer->height, 4, renderer->color, renderer->width * 4) != 0;
}

bool soft_render_sheet_create(soft_render_sheet_t *sheet, int column_count, int row_count, int cell_width, int cell_height)
{
	size_t byte_count;
	size_t i;

	memset(sheet, 0, sizeof(*sheet));
	sheet->column_count = column_count;
	sheet->row_count = row_count;
	sheet->cell_width = cell_width;
	sheet->cell_height = cell_height;
	sheet->width = column_count * cell_width + (column_count - 1) * SHEET_GAP_PIXELS;
	sheet->height = row_count * cell_height + (row_count - 1) * SHEET_GAP_PIXELS;
	byte_count = (size_t)sheet->width * (size_t)sheet->height * 4;
	sheet->color = malloc(byte_count);
	if (sheet->color == NULL)
	{
		memset(sheet, 0, sizeof(*sheet));
		return false;
	}
	for (i = 0; i < byte_count; i++)
	{
		sheet->color[i] = (i % 4 == 3) ? 255 : SHEET_GAP_SHADE;
	}
	return true;
}

void soft_render_sheet_destroy(soft_render_sheet_t *sheet)
{
	free(sheet->color);
	memset(sheet, 0, sizeof(*sheet));
}

void soft_render_sheet_put(soft_render_sheet_t *sheet, int cell, const soft_render_t *renderer)
{
	int column = cell % sheet->column_count;
	int row = cell / sheet->column_count;
	int left = column * (sheet->cell_width + SHEET_GAP_PIXELS);
	int top = row * (sheet->cell_height + SHEET_GAP_PIXELS);
	int copied_width = renderer->width < sheet->cell_width ? renderer->width : sheet->cell_width;
	int copied_height = renderer->height < sheet->cell_height ? renderer->height : sheet->cell_height;
	int y;

	if (cell < 0 || row >= sheet->row_count)
	{
		return;
	}
	for (y = 0; y < copied_height; y++)
	{
		memcpy(&sheet->color[((size_t)(top + y) * (size_t)sheet->width + (size_t)left) * 4],
			&renderer->color[(size_t)y * (size_t)renderer->width * 4], (size_t)copied_width * 4);
	}
}

bool soft_render_sheet_save_png(const soft_render_sheet_t *sheet, const char *path)
{
	return stbi_write_png(path, sheet->width, sheet->height, 4, sheet->color, sheet->width * 4) != 0;
}
