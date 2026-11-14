// Cache of generated terrains. See terrain_cache.h.
//
// File layout (little endian, as written by the machine that made it; the
// cache is local, so it is never moved between machines):
//   char     magic[8]            "POTOPTER"
//   uint32   format_version
//   uint32   generator_version
//   uint64   key                 hash of everything that shaped the terrain
//   int32    size
//   float    max_height_meters
//   int32    heights[size * size]
//   uint8    soil_types[size * size]
//   int32    bedrock_heights[size * size]

#include "terrain_cache.h"
#include "terrain_gen.h"

#include <SDL3/SDL.h>

#define CACHE_FORMAT_VERSION 2
#define CACHE_FILE_PREFIX "terrain_"
#define CACHE_FILE_SUFFIX ".bin"

typedef struct
{
	char magic[8];
	uint32_t format_version;
	uint32_t generator_version;
	uint64_t key;
	int32_t size;
	float max_height_meters;
} cache_header_t;

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
	const uint8_t *bytes = data;

	// FNV-1a, 64 bit
	for (size_t i = 0; i < size; i++)
	{
		hash ^= bytes[i];
		hash *= 0x100000001B3ULL;
	}
	return hash;
}

static uint64_t compute_key(const terrain_settings_t *settings, const soil_settings_t *soil_settings, uint32_t seed, int32_t size)
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	uint32_t generator_version = TERRAIN_GENERATOR_VERSION;
	terrain_settings_t copy = *settings;

	// The seed and size stored in the settings may differ from the ones
	// actually used (random seed, command line size); hash the real ones
	copy.seed = seed;
	copy.size = size;
	// terrain_settings_t holds only 32 bit fields, so it has no padding and
	// its bytes are a reliable fingerprint
	hash = hash_bytes(hash, &generator_version, sizeof(generator_version));
	hash = hash_bytes(hash, &copy, sizeof(copy));
	// The soil depth shapes the bedrock
	hash = hash_bytes(hash, &soil_settings->soil_depth_minimum_meters, sizeof(float));
	hash = hash_bytes(hash, &soil_settings->soil_depth_maximum_meters, sizeof(float));
	return hash;
}

static bool build_cache_directory(char *path_out, size_t path_out_size)
{
	char *preference_path = SDL_GetPrefPath("potop", "potop");

	if (preference_path == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Terrain cache disabled: no preference directory (%s)", SDL_GetError());
		return false;
	}
	SDL_snprintf(path_out, path_out_size, "%scache/", preference_path);
	SDL_free(preference_path);
	if (!SDL_CreateDirectory(path_out))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Terrain cache disabled: cannot create %s (%s)", path_out, SDL_GetError());
		return false;
	}
	return true;
}

static bool build_cache_file_path(uint64_t key, char *path_out, size_t path_out_size)
{
	char directory[1024];

	if (!build_cache_directory(directory, sizeof(directory)))
	{
		return false;
	}
	SDL_snprintf(path_out, path_out_size, "%s" CACHE_FILE_PREFIX "%016llx" CACHE_FILE_SUFFIX, directory, (unsigned long long)key);
	return true;
}

bool terrain_cache_load(terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings, uint32_t seed)
{
	uint64_t key = compute_key(settings, soil_settings, seed, terrain->size);
	char path[1200];
	SDL_IOStream *stream;
	cache_header_t header;
	size_t cell_count = (size_t)terrain->size * (size_t)terrain->size;
	bool loaded = false;
	Uint64 start_ticks = SDL_GetTicks();

	if (!build_cache_file_path(key, path, sizeof(path)))
	{
		return false;
	}
	stream = SDL_IOFromFile(path, "rb");
	if (stream == NULL)
	{
		SDL_Log("Terrain cache miss for seed %u (%s)", seed, path);
		return false;
	}

	if (SDL_ReadIO(stream, &header, sizeof(header)) != sizeof(header))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Terrain cache %s: truncated header, ignoring it", path);
	}
	else if (SDL_memcmp(header.magic, "POTOPTER", 8) != 0 || header.format_version != CACHE_FORMAT_VERSION
		|| header.generator_version != TERRAIN_GENERATOR_VERSION || header.key != key || header.size != terrain->size)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Terrain cache %s: header does not match, ignoring it", path);
	}
	else if (SDL_ReadIO(stream, terrain->heights, cell_count * sizeof(int32_t)) != cell_count * sizeof(int32_t)
		|| SDL_ReadIO(stream, terrain->soil_types, cell_count) != cell_count
		|| SDL_ReadIO(stream, terrain->bedrock_heights, cell_count * sizeof(int32_t)) != cell_count * sizeof(int32_t))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Terrain cache %s: truncated data, ignoring it", path);
	}
	else
	{
		terrain->seed = seed;
		terrain->max_height_meters = header.max_height_meters;
		SDL_memset(terrain->silt_thickness, 0, cell_count * sizeof(int32_t));
		terrain_mark_all_changed(terrain);
		loaded = true;
		SDL_Log("Loaded %dx%d terrain, seed %u, from cache in %llu ms", terrain->size, terrain->size, seed,
			(unsigned long long)(SDL_GetTicks() - start_ticks));
	}
	SDL_CloseIO(stream);
	return loaded;
}

typedef struct
{
	char names[64][256];
	SDL_Time modified[64];
	int count;
	const char *directory;
} cache_listing_t;

static SDL_EnumerationResult SDLCALL collect_cache_file(void *user_data, const char *directory, const char *file_name)
{
	cache_listing_t *listing = user_data;
	char path[1300];
	SDL_PathInfo info;

	if (SDL_strncmp(file_name, CACHE_FILE_PREFIX, SDL_strlen(CACHE_FILE_PREFIX)) != 0 || listing->count >= 64)
	{
		return SDL_ENUM_CONTINUE;
	}
	SDL_snprintf(path, sizeof(path), "%s%s", directory, file_name);
	if (SDL_GetPathInfo(path, &info))
	{
		SDL_strlcpy(listing->names[listing->count], path, sizeof(listing->names[0]));
		listing->modified[listing->count] = info.modify_time;
		listing->count++;
	}
	return SDL_ENUM_CONTINUE;
}

// Removes the oldest cache files until at most maximum_files remain
static void prune_cache(int maximum_files)
{
	static cache_listing_t listing;
	char directory[1024];

	if (!build_cache_directory(directory, sizeof(directory)))
	{
		return;
	}
	listing.count = 0;
	listing.directory = directory;
	if (!SDL_EnumerateDirectory(directory, collect_cache_file, &listing))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot list terrain cache %s: %s", directory, SDL_GetError());
		return;
	}
	while (listing.count > SDL_max(maximum_files, 1))
	{
		int oldest = 0;

		for (int i = 1; i < listing.count; i++)
		{
			if (listing.modified[i] < listing.modified[oldest])
			{
				oldest = i;
			}
		}
		if (!SDL_RemovePath(listing.names[oldest]))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot remove old cache file %s: %s", listing.names[oldest], SDL_GetError());
		}
		else
		{
			SDL_Log("Removed old terrain cache file %s", listing.names[oldest]);
		}
		listing.count--;
		SDL_memcpy(listing.names[oldest], listing.names[listing.count], sizeof(listing.names[0]));
		listing.modified[oldest] = listing.modified[listing.count];
	}
}

void terrain_cache_save(const terrain_t *terrain, const terrain_settings_t *settings, const soil_settings_t *soil_settings,
	const cache_settings_t *cache_settings)
{
	uint64_t key = compute_key(settings, soil_settings, terrain->seed, terrain->size);
	char path[1200];
	SDL_IOStream *stream;
	cache_header_t header;
	size_t cell_count = (size_t)terrain->size * (size_t)terrain->size;
	bool written;

	if (!build_cache_file_path(key, path, sizeof(path)))
	{
		return;
	}
	SDL_zero(header);
	SDL_memcpy(header.magic, "POTOPTER", 8);
	header.format_version = CACHE_FORMAT_VERSION;
	header.generator_version = TERRAIN_GENERATOR_VERSION;
	header.key = key;
	header.size = terrain->size;
	header.max_height_meters = terrain->max_height_meters;

	stream = SDL_IOFromFile(path, "wb");
	if (stream == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot write terrain cache %s: %s", path, SDL_GetError());
		return;
	}
	written = SDL_WriteIO(stream, &header, sizeof(header)) == sizeof(header)
		&& SDL_WriteIO(stream, terrain->heights, cell_count * sizeof(int32_t)) == cell_count * sizeof(int32_t)
		&& SDL_WriteIO(stream, terrain->soil_types, cell_count) == cell_count
		&& SDL_WriteIO(stream, terrain->bedrock_heights, cell_count * sizeof(int32_t)) == cell_count * sizeof(int32_t);
	if (!SDL_CloseIO(stream) || !written)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Writing terrain cache %s failed: %s; removing it", path, SDL_GetError());
		SDL_RemovePath(path);
		return;
	}
	SDL_Log("Saved terrain to cache %s", path);
	prune_cache(cache_settings->cache_maximum_files);
}
