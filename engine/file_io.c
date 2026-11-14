// File access through SDL's IO layer. See file_io.h.

#include "file_io.h"

#include <SDL3/SDL.h>

static char data_root[1024] = "data/";

void file_io_initialize(void)
{
	// Probe a file that always ships with the game (the ini files may be
	// missing or replaced by the player's own)
	SDL_IOStream *probe = SDL_IOFromFile("data/shaders/terrain.vert", "rb");
	const char *base_path;

	if (probe != NULL)
	{
		SDL_CloseIO(probe);
		SDL_strlcpy(data_root, "data/", sizeof(data_root));
		return;
	}

	// Not started from the project directory: look next to the executable.
	// On Android SDL_GetBasePath returns NULL and assets are addressed by
	// relative paths, so "data/" stays as the default there.
	base_path = SDL_GetBasePath();
	if (base_path != NULL)
	{
		SDL_snprintf(data_root, sizeof(data_root), "%sdata/", base_path);
	}
	SDL_Log("Data directory: %s", data_root);
}

void file_io_data_path(const char *relative_path, char *path_out, size_t path_out_size)
{
	SDL_snprintf(path_out, path_out_size, "%s%s", data_root, relative_path);
}

void *file_io_load_data_file(const char *relative_path, size_t *size_out)
{
	char path[1024];
	void *data;
	size_t size = 0;

	file_io_data_path(relative_path, path, sizeof(path));
	// SDL_LoadFile goes through SDL_IOFromFile and always appends a zero byte
	data = SDL_LoadFile(path, &size);
	if (size_out != NULL)
	{
		*size_out = size;
	}
	return data;
}

bool file_io_data_file_exists(const char *relative_path)
{
	char path[1024];
	SDL_IOStream *stream;

	file_io_data_path(relative_path, path, sizeof(path));
	stream = SDL_IOFromFile(path, "rb");
	if (stream == NULL)
	{
		return false;
	}
	SDL_CloseIO(stream);
	return true;
}

bool file_io_save_file(const char *path, const void *data, size_t size)
{
	if (!SDL_SaveFile(path, data, size))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write %s: %s", path, SDL_GetError());
		return false;
	}
	return true;
}

bool file_io_preference_path(const char *relative_path, char *path_out, size_t path_out_size)
{
	char *preference_path = SDL_GetPrefPath("potop", "potop");
	char *last_slash;

	if (preference_path == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No preference directory: %s", SDL_GetError());
		return false;
	}
	SDL_snprintf(path_out, path_out_size, "%s%s", preference_path, relative_path);
	SDL_free(preference_path);
	last_slash = SDL_strrchr(path_out, '/');
	if (last_slash != NULL && SDL_strchr(relative_path, '/') != NULL)
	{
		char directory[1024];

		SDL_strlcpy(directory, path_out, SDL_min(sizeof(directory), (size_t)(last_slash - path_out) + 1));
		if (!SDL_CreateDirectory(directory))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot create %s: %s", directory, SDL_GetError());
			return false;
		}
	}
	return true;
}

void *file_io_load_file(const char *path, size_t *size_out)
{
	return SDL_LoadFile(path, size_out);
}
