// Map projects. See project.h.

#include "project.h"

#include "file_io.h"
#include "ini_parser.h"

#include <SDL3/SDL.h>

bool project_path(const char *name, const char *relative, char *path_out, size_t path_out_size)
{
	char inside[PROJECT_NAME_LENGTH + 96];

	// (a trailing slash: file_io_preference_path makes the folder)
	SDL_snprintf(inside, sizeof(inside), PROJECT_FOLDER "%s/%s", name, relative);
	return file_io_preference_path(inside, path_out, path_out_size);
}

bool project_exists(const char *name)
{
	char path[1024];

	return name[0] != '\0' && project_path(name, PROJECT_MAP_FILE, path, sizeof(path)) && SDL_GetPathInfo(path, NULL);
}

bool project_write_info(const project_info_t *info)
{
	char path[1024];
	char text[512];

	if (!project_path(info->name, PROJECT_INFO_FILE, path, sizeof(path)))
	{
		return false;
	}
	SDL_snprintf(text, sizeof(text),
		"; A map project (editor/project.h)\n"
		"name = %s\nseed = %u\nsize_cells = %d\ncell_size_meters = %.3f\ncreated_seconds = %lld\nbaked = %s\n"
		"max_height_meters = %.1f\nerosion_droplets = %.2f\n",
		info->name, info->seed, info->size_cells, (double)info->cell_size_meters, (long long)info->created_seconds,
		info->baked ? "true" : "false", (double)info->max_height_meters, (double)info->erosion_droplets);
	return file_io_save_file(path, text, SDL_strlen(text));
}

static bool read_info_line(void *user, const char *section, const char *key, const char *value)
{
	project_info_t *info = user;

	(void)section;
	if (SDL_strcmp(key, "name") == 0)
	{
		return true;
	}
	if (SDL_strcmp(key, "seed") == 0)
	{
		info->seed = (uint32_t)SDL_strtoul(value, NULL, 10);
	}
	else if (SDL_strcmp(key, "size_cells") == 0)
	{
		info->size_cells = SDL_atoi(value);
	}
	else if (SDL_strcmp(key, "cell_size_meters") == 0)
	{
		info->cell_size_meters = (float)SDL_atof(value);
	}
	else if (SDL_strcmp(key, "created_seconds") == 0)
	{
		info->created_seconds = (int64_t)SDL_strtoll(value, NULL, 10);
	}
	else if (SDL_strcmp(key, "baked") == 0)
	{
		ini_value_to_bool(value, &info->baked);
	}
	else if (SDL_strcmp(key, "max_height_meters") == 0)
	{
		info->max_height_meters = (float)SDL_atof(value);
	}
	else if (SDL_strcmp(key, "erosion_droplets") == 0)
	{
		info->erosion_droplets = (float)SDL_atof(value);
	}
	else
	{
		return false;
	}
	return true;
}

bool project_read_info(const char *name, project_info_t *info_out)
{
	char path[1024];
	SDL_PathInfo path_info;

	SDL_zerop(info_out);
	SDL_strlcpy(info_out->name, name, sizeof(info_out->name));
	if (!project_path(name, PROJECT_INFO_FILE, path, sizeof(path)))
	{
		return false;
	}
	// (a project without its ini still opens: its map is what counts)
	{
		size_t size = 0;
		char *text = SDL_LoadFile(path, &size);

		if (text != NULL)
		{
			ini_parse_buffer(text, PROJECT_INFO_FILE, read_info_line, info_out);
			SDL_free(text);
		}
	}
	if (project_path(name, PROJECT_MAP_FILE, path, sizeof(path)) && SDL_GetPathInfo(path, &path_info))
	{
		info_out->modified_nanoseconds = path_info.modify_time;
		info_out->map_bytes = path_info.size;
	}
	info_out->has_bake = project_path(name, PROJECT_BAKE_FILE, path, sizeof(path)) && SDL_GetPathInfo(path, NULL);
	return true;
}

typedef struct
{
	project_info_t *infos;
	int max_infos;
	int count;
} project_listing_t;

static SDL_EnumerationResult list_one(void *user, const char *directory, const char *file_name)
{
	project_listing_t *listing = user;
	char path[1200];
	SDL_PathInfo path_info;

	SDL_snprintf(path, sizeof(path), "%s%s", directory, file_name);
	if (listing->count < listing->max_infos && file_name[0] != '.' && SDL_strlen(file_name) < PROJECT_NAME_LENGTH
		&& SDL_GetPathInfo(path, &path_info) && path_info.type == SDL_PATHTYPE_DIRECTORY && project_exists(file_name))
	{
		project_read_info(file_name, &listing->infos[listing->count++]);
	}
	return SDL_ENUM_CONTINUE;
}

static int newest_first(const void *a, const void *b)
{
	const project_info_t *first = a;
	const project_info_t *second = b;

	return first->modified_nanoseconds < second->modified_nanoseconds ? 1 : (first->modified_nanoseconds > second->modified_nanoseconds ? -1 : 0);
}

int project_list(project_info_t *infos_out, int max_infos)
{
	char folder[1024];
	project_listing_t listing;

	listing.infos = infos_out;
	listing.max_infos = max_infos;
	listing.count = 0;
	if (!file_io_preference_path(PROJECT_FOLDER, folder, sizeof(folder)))
	{
		return 0;
	}
	SDL_EnumerateDirectory(folder, list_one, &listing);
	SDL_qsort(infos_out, (size_t)listing.count, sizeof(*infos_out), newest_first);
	return listing.count;
}

// Removes a folder's files and folders under it, then the folder
static SDL_EnumerationResult remove_one(void *user, const char *directory, const char *file_name)
{
	bool *all_removed = user;
	char path[1200];
	SDL_PathInfo path_info;

	SDL_snprintf(path, sizeof(path), "%s%s", directory, file_name);
	if (SDL_GetPathInfo(path, &path_info) && path_info.type == SDL_PATHTYPE_DIRECTORY)
	{
		SDL_strlcat(path, "/", sizeof(path));
		SDL_EnumerateDirectory(path, remove_one, all_removed);
	}
	if (!SDL_RemovePath(path))
	{
		*all_removed = false;
	}
	return SDL_ENUM_CONTINUE;
}

bool project_delete(const char *name)
{
	char folder[1024];
	bool all_removed = true;

	// (a name that is not a project's: nothing, not a folder above)
	if (name[0] == '\0' || SDL_strchr(name, '/') != NULL || SDL_strcmp(name, "..") == 0 || !project_exists(name)
		|| !project_path(name, "", folder, sizeof(folder)))
	{
		return false;
	}
	SDL_EnumerateDirectory(folder, remove_one, &all_removed);
	return SDL_RemovePath(folder) && all_removed;
}

bool sketch_library_path(const char *name, char *path_out, size_t path_out_size)
{
	char inside[PROJECT_NAME_LENGTH + 32];

	if (name[0] == '\0' || SDL_strchr(name, '/') != NULL || SDL_strcmp(name, "..") == 0)
	{
		return false;
	}
	SDL_snprintf(inside, sizeof(inside), SKETCH_LIBRARY_FOLDER "%s/", name);
	return file_io_preference_path(inside, path_out, path_out_size);
}

typedef struct
{
	char (*names)[PROJECT_NAME_LENGTH];
	int max_names;
	int count;
} sketch_listing_t;

static SDL_EnumerationResult list_one_sketch(void *user, const char *directory, const char *file_name)
{
	sketch_listing_t *listing = user;
	char path[1200];

	// (a folder with a sketch's regions in it)
	SDL_snprintf(path, sizeof(path), "%s%s/regions.png", directory, file_name);
	if (listing->count < listing->max_names && file_name[0] != '.' && SDL_strlen(file_name) < PROJECT_NAME_LENGTH && SDL_GetPathInfo(path, NULL))
	{
		SDL_strlcpy(listing->names[listing->count++], file_name, PROJECT_NAME_LENGTH);
	}
	return SDL_ENUM_CONTINUE;
}

static int alphabetically(const void *a, const void *b)
{
	return SDL_strcasecmp((const char *)a, (const char *)b);
}

int sketch_library_list(char (*names_out)[PROJECT_NAME_LENGTH], int max_names)
{
	char folder[1024];
	sketch_listing_t listing;

	listing.names = names_out;
	listing.max_names = max_names;
	listing.count = 0;
	if (!file_io_preference_path(SKETCH_LIBRARY_FOLDER, folder, sizeof(folder)))
	{
		return 0;
	}
	SDL_EnumerateDirectory(folder, list_one_sketch, &listing);
	SDL_qsort(names_out, (size_t)listing.count, PROJECT_NAME_LENGTH, alphabetically);
	return listing.count;
}
