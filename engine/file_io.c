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

// Moves what an older version kept in its preference directory,
// potop/potop/ (SDL_GetPrefPath with "potop" as the organization too),
// up into potop/: a name already there stays where it was
static SDL_EnumerationResult move_up_one(void *user, const char *directory, const char *file_name)
{
	const char *new_directory = user;
	char old_path[1200];
	char new_path[1200];

	SDL_snprintf(old_path, sizeof(old_path), "%s%s", directory, file_name);
	SDL_snprintf(new_path, sizeof(new_path), "%s%s", new_directory, file_name);
	if (SDL_GetPathInfo(new_path, NULL))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Preferences: %s left where it was (there is a %s)", old_path, new_path);
	}
	else if (!SDL_RenamePath(old_path, new_path))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Preferences: cannot move %s to %s: %s", old_path, new_path, SDL_GetError());
	}
	return SDL_ENUM_CONTINUE;
}

static void move_old_preferences(const char *preference_path)
{
	static bool done = false;
	char old_directory[1100];
	SDL_PathInfo info;

	if (done)
	{
		return;
	}
	done = true;
	SDL_snprintf(old_directory, sizeof(old_directory), "%spotop/", preference_path);
	if (SDL_GetPathInfo(old_directory, &info) && info.type == SDL_PATHTYPE_DIRECTORY)
	{
		SDL_Log("Preferences: moving %s up into %s", old_directory, preference_path);
		SDL_EnumerateDirectory(old_directory, move_up_one, (void *)preference_path);
		// (gone if it is empty now; left, with what stayed in it, if not)
		SDL_RemovePath(old_directory);
	}
}

char *file_io_preference_directory(void)
{
	// (no organization: ~/.local/share/potop/ on Linux, not potop/potop/)
	char *preference_path = SDL_GetPrefPath("", "potop");

	if (preference_path != NULL)
	{
		move_old_preferences(preference_path);
	}
	return preference_path;
}

bool file_io_preference_path(const char *relative_path, char *path_out, size_t path_out_size)
{
	char *preference_path = file_io_preference_directory();
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
