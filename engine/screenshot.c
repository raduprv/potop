// Screenshots. See screenshot.h.

#include "screenshot.h"
#include "file_io.h"

#include <SDL3/SDL.h>
#include "stb_image_write.h"

typedef struct
{
	uint8_t *data;
	size_t size;
	size_t capacity;
	bool failed;
} memory_writer_t;

// stb_image_write hands us the encoded PNG in pieces; collect them in memory
// so the file itself can be written through SDL's IO layer
static void append_to_memory(void *context, void *data, int size)
{
	memory_writer_t *writer = context;

	if (writer->failed)
	{
		return;
	}
	if (writer->size + (size_t)size > writer->capacity)
	{
		size_t new_capacity = SDL_max(writer->capacity * 2, writer->size + (size_t)size);
		uint8_t *grown = SDL_realloc(writer->data, new_capacity);

		if (grown == NULL)
		{
			writer->failed = true;
			return;
		}
		writer->data = grown;
		writer->capacity = new_capacity;
	}
	SDL_memcpy(writer->data + writer->size, data, (size_t)size);
	writer->size += (size_t)size;
}

bool screenshot_save(const char *path, const uint8_t *rgba, int width, int height)
{
	size_t row_bytes = (size_t)width * 3;
	uint8_t *pixels = SDL_malloc(row_bytes * (size_t)height);
	memory_writer_t writer = { 0 };
	bool saved = false;

	if (pixels == NULL || rgba == NULL)
	{
		SDL_free(pixels);
		return false;
	}
	// RGB only: the alpha channel says nothing
	for (size_t i = 0; i < (size_t)width * (size_t)height; i++)
	{
		pixels[i * 3] = rgba[i * 4];
		pixels[i * 3 + 1] = rgba[i * 4 + 1];
		pixels[i * 3 + 2] = rgba[i * 4 + 2];
	}
	if (stbi_write_png_to_func(append_to_memory, &writer, width, height, 3, pixels, (int)row_bytes) && !writer.failed)
	{
		saved = file_io_save_file(path, writer.data, writer.size);
	}
	if (saved)
	{
		SDL_Log("Saved screenshot %s", path);
	}

	SDL_free(writer.data);
	SDL_free(pixels);
	return saved;
}

void screenshot_make_name(char *name_out, int name_out_size)
{
	SDL_Time now;
	SDL_DateTime date_time;

	if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &date_time, true))
	{
		SDL_snprintf(name_out, (size_t)name_out_size, "screenshot_%04d%02d%02d_%02d%02d%02d.png",
			date_time.year, date_time.month, date_time.day, date_time.hour, date_time.minute, date_time.second);
	}
	else
	{
		SDL_snprintf(name_out, (size_t)name_out_size, "screenshot_%llu.png", (unsigned long long)SDL_GetTicks());
	}
}
