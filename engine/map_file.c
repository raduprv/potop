// Saved maps. See map_file.h.

#include "map_file.h"

#include <SDL3/SDL.h>

#include "file_io.h"
#include "ini_parser.h"
#include "material_map.h"
#include "materials.h"
#include "terrain_renderer.h"

// File access goes through file_io (SDL), never miniz' own stdio
#define MINIZ_NO_STDIO
#include "miniz.h"

// 1: objects without their physics state; 2: with it; 3: a baked map
// (kind = baked; an active map is still written as 2)
#define MAP_FILE_VERSION 2
#define MAP_FILE_BAKED_VERSION 3
// A baked map's material palette: base and cover take a nibble each (an
// index into the palette, PALETTE_NONE for none)
#define PALETTE_SIZE 15
#define PALETTE_NONE 15
#define SAVES_FOLDER "saves/"
// Deflate effort: 1 is the fastest; the predicted heights pack well even
// so (see encode_layer)
#define MAP_FILE_COMPRESSION_LEVEL 2

typedef struct
{
	int32_t type;
	int32_t group;
	float x;
	float z;
	float lift_meters;
	float yaw_radians;
	float scale;
} saved_object_v1_t;

typedef struct
{
	int32_t type;
	int32_t group;
	float x;
	float z;
	float lift_meters;
	float yaw_radians;
	float scale;
	float rotation[4];             // as drawn (loose objects: as their body lies)
	float y;                       // loose objects: height of the origin
	float flatten;
	float flatten_yaw;
	uint8_t state;
	uint8_t damage;
	uint8_t padding[2];
} saved_object_t;

struct map_file_reader_s
{
	void *archive;
	size_t archive_size;
	mz_zip_archive zip;
	map_file_info_t info;
	water_spring_t springs[WATER_MAX_SPRINGS];
	int spring_count;
	int next_group;
	// A baked map's
	int32_t height_base_units;
	int32_t height_step_units;
	uint8_t palette[PALETTE_SIZE];
	int palette_count;
	uint8_t overlay;
	int32_t height_band_rows;      // 0: one band (the first baked maps)
	bool tiles_split;              // the tiles with water and those with ground apart (else: one list, every plane)
	int32_t part_bytes;            // 0: every layer one entry
};

bool map_file_path(const char *name, char *path_out, size_t path_out_size)
{
	char relative[MAP_FILE_NAME_LENGTH + 32];

	SDL_snprintf(relative, sizeof(relative), SAVES_FOLDER "%s" MAP_FILE_EXTENSION, name);
	return file_io_preference_path(relative, path_out, path_out_size);
}

void map_file_kind_name(const char *name, bool baked, char *name_out, size_t name_out_size)
{
	SDL_snprintf(name_out, name_out_size, "%s%s", name, baked ? MAP_FILE_BAKED_SUFFIX : MAP_FILE_ACTIVE_SUFFIX);
}

static bool ends_with(const char *text, const char *end)
{
	size_t length = SDL_strlen(text);
	size_t end_length = SDL_strlen(end);

	return length >= end_length && SDL_strcmp(text + length - end_length, end) == 0;
}

void map_file_clean_name(const char *typed, char *name_out, size_t name_out_size)
{
	size_t length = 0;

	for (const char *character = typed; *character != '\0' && length + 1 < name_out_size; character++)
	{
		unsigned char byte = (unsigned char)*character;
		bool kept = byte >= 0x80 || SDL_isalnum(byte) || byte == ' ' || byte == '-' || byte == '_';

		// No leading spaces or dots (hidden files)
		if (length == 0 && byte == ' ')
		{
			continue;
		}
		name_out[length++] = kept ? (char)byte : '_';
	}
	while (length > 0 && name_out[length - 1] == ' ')
	{
		length--;
	}
	name_out[length] = '\0';
}

static int compare_entries_newest_first(const void *a, const void *b)
{
	const map_file_entry_t *first = a;
	const map_file_entry_t *second = b;

	if (first->modified_nanoseconds != second->modified_nanoseconds)
	{
		return first->modified_nanoseconds > second->modified_nanoseconds ? -1 : 1;
	}
	return SDL_strcmp(first->name, second->name);
}

int map_file_list(map_file_entry_t *entries_out, int max_entries)
{
	char folder[1024];
	char **matches;
	int match_count = 0;
	int count = 0;

	if (!file_io_preference_path(SAVES_FOLDER, folder, sizeof(folder)))
	{
		return 0;
	}
	matches = SDL_GlobDirectory(folder, "*" MAP_FILE_EXTENSION, 0, &match_count);
	if (matches == NULL)
	{
		return 0;
	}
	for (int i = 0; i < match_count && count < max_entries; i++)
	{
		char path[1200];
		SDL_PathInfo info;
		map_file_entry_t *entry = &entries_out[count];
		size_t length = SDL_strlen(matches[i]);
		size_t extension_length = SDL_strlen(MAP_FILE_EXTENSION);

		if (length <= extension_length || length - extension_length >= sizeof(entry->name))
		{
			continue;
		}
		SDL_snprintf(path, sizeof(path), "%s%s", folder, matches[i]);
		if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
		{
			continue;
		}
		SDL_memcpy(entry->name, matches[i], length - extension_length);
		entry->name[length - extension_length] = '\0';
		// (the kind by the name; the name shown without it)
		SDL_strlcpy(entry->shown_name, entry->name, sizeof(entry->shown_name));
		entry->baked = ends_with(entry->name, MAP_FILE_BAKED_SUFFIX);
		if (entry->baked || ends_with(entry->name, MAP_FILE_ACTIVE_SUFFIX))
		{
			entry->shown_name[SDL_strlen(entry->name) - SDL_strlen(entry->baked ? MAP_FILE_BAKED_SUFFIX : MAP_FILE_ACTIVE_SUFFIX)] = '\0';
		}
		entry->modified_nanoseconds = info.modify_time;
		entry->size_bytes = info.size;
		count++;
	}
	SDL_free(matches);
	SDL_qsort(entries_out, (size_t)count, sizeof(map_file_entry_t), compare_entries_newest_first);
	return count;
}

bool map_file_delete(const char *name)
{
	char path[1024];

	if (!map_file_path(name, path, sizeof(path)))
	{
		return false;
	}
	if (!SDL_RemovePath(path))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot delete %s: %s", path, SDL_GetError());
		return false;
	}
	SDL_Log("Deleted %s", path);
	return true;
}

// Height layers are stored predicted: each cell as its difference to
// left + above - above left (a plane through its neighbors), which is
// small and close to zero on smooth ground; the differences are folded to
// unsigned (zigzag: 0, -1, 1, -2... -> 0, 1, 2, 3...) and split into four
// planes of bytes, lowest bytes first. Deflate packs that more than twice
// as well as the raw values. All arithmetic wraps (unsigned), so any value
// comes back exactly. 'minus', if not NULL, is subtracted first (the
// bedrock is stored as the soil's thickness over it).
static uint8_t *encode_layer(const int32_t *values, const int32_t *minus, int32_t size)
{
	size_t count = (size_t)size * (size_t)size;
	uint8_t *planes = SDL_malloc(count * 4);

	if (planes == NULL)
	{
		return NULL;
	}
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			size_t index = (size_t)y * (size_t)size + (size_t)x;
#define LAYER_VALUE(i) ((uint32_t)values[i] - (minus != NULL ? (uint32_t)minus[i] : 0u))
			uint32_t value = LAYER_VALUE(index);
			uint32_t left = x > 0 ? LAYER_VALUE(index - 1) : 0u;
			uint32_t above = y > 0 ? LAYER_VALUE(index - (size_t)size) : 0u;
			uint32_t above_left = x > 0 && y > 0 ? LAYER_VALUE(index - (size_t)size - 1) : 0u;
#undef LAYER_VALUE
			uint32_t predicted = x > 0 && y > 0 ? left + above - above_left : (x > 0 ? left : above);
			int32_t difference = (int32_t)(value - predicted);
			uint32_t folded = ((uint32_t)difference << 1) ^ (uint32_t)(difference >> 31);

			planes[index] = (uint8_t)folded;
			planes[count + index] = (uint8_t)(folded >> 8);
			planes[2 * count + index] = (uint8_t)(folded >> 16);
			planes[3 * count + index] = (uint8_t)(folded >> 24);
		}
	}
	return planes;
}

// The reverse, from planes into values (minus added back by the caller)
static void decode_layer(const uint8_t *planes, int32_t *values, int32_t size)
{
	size_t count = (size_t)size * (size_t)size;

	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			size_t index = (size_t)y * (size_t)size + (size_t)x;
			uint32_t folded = (uint32_t)planes[index] | (uint32_t)planes[count + index] << 8 | (uint32_t)planes[2 * count + index] << 16
				| (uint32_t)planes[3 * count + index] << 24;
			uint32_t difference = (folded >> 1) ^ (0u - (folded & 1u));
			uint32_t left = x > 0 ? (uint32_t)values[index - 1] : 0u;
			uint32_t above = y > 0 ? (uint32_t)values[index - (size_t)size] : 0u;
			uint32_t above_left = x > 0 && y > 0 ? (uint32_t)values[index - (size_t)size - 1] : 0u;
			uint32_t predicted = x > 0 && y > 0 ? left + above - above_left : (x > 0 ? left : above);

			values[index] = (int32_t)(predicted + difference);
		}
	}
}

typedef struct
{
	const char *name;
	const void *data;
	size_t size;
	void *owned;                   // freed after writing (SDL_free)
	void *compressed;              // from miniz (mz_free)
	size_t compressed_size;
	mz_ulong crc;
} archive_entry_t;

static size_t append_text(char *text, size_t capacity, size_t length, const char *format, ...)
{
	va_list arguments;
	int written;

	if (length >= capacity)
	{
		return length;
	}
	va_start(arguments, format);
	written = SDL_vsnprintf(text + length, capacity - length, format, arguments);
	va_end(arguments);
	return written < 0 ? length : SDL_min(capacity - 1, length + (size_t)written);
}

// The time since mark (milliseconds), and mark moved on to now: the stages'
// times in the log
static double stage_milliseconds(uint64_t *mark)
{
	uint64_t now = SDL_GetPerformanceCounter();
	double milliseconds = 1000.0 * (double)(now - *mark) / (double)SDL_GetPerformanceFrequency();

	*mark = now;
	return milliseconds;
}

// The entries compressed side by side, put into one archive, written to
// path. The stages' times out (milliseconds)
static bool write_archive(const char *path, archive_entry_t *entries, int entry_count, size_t *archive_size_out, double *compress_milliseconds,
	double *archive_milliseconds, double *write_milliseconds)
{
	mz_uint compression_flags = tdefl_create_comp_flags_from_zip_params(MAP_FILE_COMPRESSION_LEVEL, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
	uint64_t stage_mark = SDL_GetPerformanceCounter();
	mz_zip_archive zip;
	void *archive = NULL;
	size_t archive_size = 0;
	bool saved = false;

#pragma omp parallel for schedule(dynamic, 1)
	for (int i = 0; i < entry_count; i++)
	{
		entries[i].crc = mz_crc32(MZ_CRC32_INIT, entries[i].data, entries[i].size);
		entries[i].compressed = tdefl_compress_mem_to_heap(entries[i].data, entries[i].size, &entries[i].compressed_size, (int)compression_flags);
	}
	*compress_milliseconds = stage_milliseconds(&stage_mark);

	SDL_zero(zip);
	if (!mz_zip_writer_init_heap(&zip, 0, 16u << 20))
	{
		return false;
	}
	for (int i = 0; i < entry_count; i++)
	{
		if (entries[i].compressed == NULL && entries[i].size > 0)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Compressing %s failed", entries[i].name);
			mz_zip_writer_end(&zip);
			return false;
		}
		if (!mz_zip_writer_add_mem_ex(&zip, entries[i].name, entries[i].compressed, entries[i].compressed_size, NULL, 0,
			MAP_FILE_COMPRESSION_LEVEL | MZ_ZIP_FLAG_COMPRESSED_DATA, entries[i].size, (mz_uint32)entries[i].crc))
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Adding %s to the map archive failed: %s", entries[i].name,
				mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
			mz_zip_writer_end(&zip);
			return false;
		}
	}
	if (!mz_zip_writer_finalize_heap_archive(&zip, &archive, &archive_size))
	{
		mz_zip_writer_end(&zip);
		return false;
	}
	mz_zip_writer_end(&zip);
	*archive_milliseconds = stage_milliseconds(&stage_mark);
	saved = file_io_save_file(path, archive, archive_size);
	*write_milliseconds = stage_milliseconds(&stage_mark);
	mz_free(archive);
	*archive_size_out = archive_size;
	return saved;
}

// The objects' ids, in the order of their records (object_ids.bin: a
// uint32_t each)
static uint32_t *object_ids(const objects_t *objects)
{
	uint32_t *ids = SDL_malloc(sizeof(uint32_t) * (size_t)SDL_max(objects->instance_count, 1));

	for (int i = 0; ids != NULL && i < objects->instance_count; i++)
	{
		ids[i] = objects->instances[i].id;
	}
	return ids;
}

// Whose the objects are, in the order of their records (object_owners.bin:
// a uint32_t each, an actor's game number); NULL if no one owns any
static uint32_t *object_owners(const objects_t *objects)
{
	bool any = false;
	uint32_t *owners;

	for (int i = 0; i < objects->instance_count && !any; i++)
	{
		any = objects->instances[i].owner != 0;
	}
	if (!any)
	{
		return NULL;
	}
	owners = SDL_malloc(sizeof(uint32_t) * (size_t)objects->instance_count);
	for (int i = 0; owners != NULL && i < objects->instance_count; i++)
	{
		owners[i] = objects->instances[i].owner;
	}
	return owners;
}

// The objects' records, and their types' names (one a line)
static void fill_objects(const objects_t *objects, char *type_names, size_t type_names_capacity, size_t *type_names_length,
	saved_object_t *saved_objects)
{
	// Objects: their types by name, then one record each
	type_names[0] = '\0';
	for (int t = 0; t < objects->types.type_count; t++)
	{
		*type_names_length += (size_t)SDL_snprintf(type_names + *type_names_length, type_names_capacity - *type_names_length, "%s\n",
			objects->types.types[t].name);
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];

		saved_objects[i].type = instance->type;
		saved_objects[i].group = instance->group;
		saved_objects[i].x = instance->x;
		saved_objects[i].z = instance->z;
		saved_objects[i].lift_meters = instance->lift_meters;
		saved_objects[i].yaw_radians = instance->yaw_radians;
		saved_objects[i].scale = instance->scale;
		SDL_memcpy(saved_objects[i].rotation, instance->rotation, sizeof(instance->rotation));
		saved_objects[i].y = instance->y;
		saved_objects[i].flatten = instance->flatten;
		saved_objects[i].flatten_yaw = instance->flatten_yaw;
		saved_objects[i].state = (uint8_t)(instance->state & ~OBJECT_STATE_AWAKE);
		saved_objects[i].damage = instance->damage;
		saved_objects[i].padding[0] = 0;
		saved_objects[i].padding[1] = 0;
	}
}

// ---------------------------------------------------------------------------
// What was put on the map (map_file.h): an entry each, a header and fixed
// records

typedef struct
{
	uint32_t version;
	uint32_t record_bytes;
	uint32_t count;
	uint32_t padding;
} feature_header_t;

typedef struct
{
	float position[3];
	float color[3];
	float intensity;
	float radius_meters;
	float flicker;
	float height_meters;
	float core_meters;
	uint8_t switched;
	uint8_t padding[3];
} saved_light_t;

typedef struct
{
	char effect[MAP_EFFECT_NAME_LENGTH];
	float position[3];
	float rotation[4];
	float extents[3];
} saved_effect_t;

void map_features_free(map_features_t *features)
{
	SDL_free(features->lights);
	SDL_free(features->effects);
	SDL_free(features->waterfalls);
	SDL_free(features->sky_occlusion);
	SDL_free(features->sky_occlusion_layers);
	SDL_free(features->sky_occlusion_bounce);
	SDL_free(features->items);
	SDL_free(features->harvest);
	SDL_free(features->actors);
	SDL_free(features->areas);
	SDL_zerop(features);
}

int map_file_sky_bounce_size(int map_size, float cell_size_meters)
{
	return SDL_min(map_file_sky_layer_size(map_size, cell_size_meters), 3072);
}

int map_file_sky_layer_size(int map_size, float cell_size_meters)
{
	float extent = (float)map_size * cell_size_meters;

	return clamp_int((int)SDL_ceilf(extent), 16, 4096);
}

// rows of width bytes as each less the one stride before it in its row
// (the same channel of the texel before; or back: undo), in place
static void row_differences(uint8_t *bytes, int width, int rows, bool undo, int stride)
{
	for (int y = 0; y < rows; y++)
	{
		uint8_t *row = bytes + (size_t)y * (size_t)width;

		if (undo)
		{
			for (int x = stride; x < width; x++)
			{
				row[x] = (uint8_t)(row[x] + row[x - stride]);
			}
		}
		else
		{
			for (int x = width - 1; x >= stride; x--)
			{
				row[x] = (uint8_t)(row[x] - row[x - stride]);
			}
		}
	}
}

// An entry's bytes: the header, then count records (NULL if out of memory)
static uint8_t *feature_entry(const void *records, size_t record_bytes, int count, size_t *size_out)
{
	size_t size = sizeof(feature_header_t) + record_bytes * (size_t)SDL_max(count, 0);
	uint8_t *data = SDL_malloc(size);
	feature_header_t header = { 1u, (uint32_t)record_bytes, (uint32_t)SDL_max(count, 0), 0u };

	if (data == NULL)
	{
		return NULL;
	}
	SDL_memcpy(data, &header, sizeof(header));
	if (count > 0)
	{
		SDL_memcpy(data + sizeof(header), records, record_bytes * (size_t)count);
	}
	*size_out = size;
	return data;
}

// The entries of the lights and effects (and the waterfalls if with_waterfalls)
// added to entries; false if out of memory
static bool add_feature_entries(archive_entry_t *entries, int *entry_count, const map_features_t *features, bool with_waterfalls)
{
	saved_light_t *lights = SDL_calloc((size_t)SDL_max(features != NULL ? features->light_count : 0, 1), sizeof(saved_light_t));
	saved_effect_t *effects = SDL_calloc((size_t)SDL_max(features != NULL ? features->effect_count : 0, 1), sizeof(saved_effect_t));
	int light_count = features != NULL ? features->light_count : 0;
	int effect_count = features != NULL ? features->effect_count : 0;
	uint8_t *data;
	size_t size;
	bool ok = lights != NULL && effects != NULL;

	for (int i = 0; i < light_count && ok; i++)
	{
		const placed_light_t *light = &features->lights[i];

		SDL_memcpy(lights[i].position, light->position, sizeof(lights[i].position));
		SDL_memcpy(lights[i].color, light->source.color, sizeof(lights[i].color));
		lights[i].intensity = light->source.intensity;
		lights[i].radius_meters = light->source.radius_meters;
		lights[i].flicker = light->source.flicker;
		lights[i].height_meters = light->source.height_meters;
		lights[i].core_meters = light->source.core_meters;
		lights[i].switched = light->source.switched ? 1 : 0;
	}
	for (int i = 0; i < effect_count && ok; i++)
	{
		const map_effect_t *effect = &features->effects[i];

		SDL_strlcpy(effects[i].effect, effect->effect, sizeof(effects[i].effect));
		SDL_memcpy(effects[i].position, effect->position, sizeof(effects[i].position));
		SDL_memcpy(effects[i].rotation, effect->rotation, sizeof(effects[i].rotation));
		SDL_memcpy(effects[i].extents, effect->extents, sizeof(effects[i].extents));
	}
	if (ok && (data = feature_entry(lights, sizeof(saved_light_t), light_count, &size)) != NULL)
	{
		entries[(*entry_count)++] = (archive_entry_t){ "lights.bin", data, size, data, NULL, 0, 0 };
	}
	else
	{
		ok = false;
	}
	if (ok && (data = feature_entry(effects, sizeof(saved_effect_t), effect_count, &size)) != NULL)
	{
		entries[(*entry_count)++] = (archive_entry_t){ "effects.bin", data, size, data, NULL, 0, 0 };
	}
	else
	{
		ok = false;
	}
	if (ok && with_waterfalls)
	{
		data = feature_entry(features != NULL ? features->waterfalls : NULL, sizeof(waterfall_segment_t),
			features != NULL ? features->waterfall_count : 0, &size);
		if (data != NULL)
		{
			entries[(*entry_count)++] = (archive_entry_t){ "waterfalls.bin", data, size, data, NULL, 0, 0 };
		}
		else
		{
			ok = false;
		}
	}
	if (ok && features != NULL && features->actors_saved)
	{
		data = feature_entry(features->actors, sizeof(map_actor_t), features->actor_count, &size);
		if (data != NULL)
		{
			entries[(*entry_count)++] = (archive_entry_t){ "actors.bin", data, size, data, NULL, 0, 0 };
		}
		else
		{
			ok = false;
		}
	}
	if (ok && features != NULL && features->areas_saved)
	{
		data = feature_entry(features->areas, sizeof(map_area_t), features->area_count, &size);
		if (data != NULL)
		{
			entries[(*entry_count)++] = (archive_entry_t){ "areas.bin", data, size, data, NULL, 0, 0 };
		}
		else
		{
			ok = false;
		}
	}
	if (ok && features != NULL && features->harvest_count > 0)
	{
		data = feature_entry(features->harvest, sizeof(map_harvest_t), features->harvest_count, &size);
		if (data != NULL)
		{
			entries[(*entry_count)++] = (archive_entry_t){ "harvest.bin", data, size, data, NULL, 0, 0 };
		}
		else
		{
			ok = false;
		}
	}
	if (ok && features != NULL && features->item_count > 0)
	{
		data = feature_entry(features->items, sizeof(map_item_t), features->item_count, &size);
		if (data != NULL)
		{
			entries[(*entry_count)++] = (archive_entry_t){ "items.bin", data, size, data, NULL, 0, 0 };
		}
		else
		{
			ok = false;
		}
	}
	if (ok && features != NULL && features->sky_occlusion != NULL && features->sky_occlusion_size > 0
		&& features->sky_occlusion_layers != NULL && features->sky_occlusion_layer_size > 0
		&& features->sky_occlusion_bounce != NULL && features->sky_occlusion_bounce_size > 0)
	{
		// (each less the one before: the smooth shade packs much smaller;
		// the bounce texture's bytes each less the same channel's before)
		const char *names[3] = { "sky_occlusion.bin", "sky_occlusion_layers.bin", "sky_occlusion_bounce.bin" };
		const uint8_t *sources[3] = { features->sky_occlusion, features->sky_occlusion_layers, features->sky_occlusion_bounce };
		int widths[3] = { features->sky_occlusion_size, features->sky_occlusion_layer_size, features->sky_occlusion_bounce_size * 4 };
		int rows[3] = { features->sky_occlusion_size, features->sky_occlusion_layer_size * 4, features->sky_occlusion_bounce_size };

		for (int k = 0; k < 3 && ok; k++)
		{
			uint8_t *differences = SDL_malloc((size_t)widths[k] * (size_t)rows[k]);

			data = NULL;
			if (differences != NULL)
			{
				SDL_memcpy(differences, sources[k], (size_t)widths[k] * (size_t)rows[k]);
				row_differences(differences, widths[k], rows[k], false, k == 2 ? 4 : 1);
				data = feature_entry(differences, (size_t)widths[k], rows[k], &size);
			}
			SDL_free(differences);
			if (data != NULL)
			{
				entries[(*entry_count)++] = (archive_entry_t){ names[k], data, size, data, NULL, 0, 0 };
			}
			else
			{
				ok = false;
			}
		}
	}
	SDL_free(lights);
	SDL_free(effects);
	return ok;
}

// An entry's records, each record_bytes (what a newer, longer record has
// more is left out; a shorter one's rest is zero). NULL (count 0) if there
// is none
static void *read_feature_entry(map_file_reader_t *reader, const char *name, size_t record_bytes, int *count_out)
{
	size_t size = 0;
	uint8_t *data = mz_zip_reader_extract_file_to_heap(&reader->zip, name, &size, 0);
	feature_header_t header;
	uint8_t *records = NULL;

	*count_out = 0;
	if (data == NULL)
	{
		return NULL;
	}
	if (size >= sizeof(header))
	{
		SDL_memcpy(&header, data, sizeof(header));
		if (header.record_bytes > 0 && header.count > 0 && header.count <= 1000000u
			&& sizeof(header) + (size_t)header.record_bytes * header.count <= size)
		{
			records = SDL_calloc(header.count, record_bytes);
			if (records != NULL)
			{
				for (uint32_t i = 0; i < header.count; i++)
				{
					SDL_memcpy(records + (size_t)i * record_bytes, data + sizeof(header) + (size_t)i * header.record_bytes,
						SDL_min(record_bytes, (size_t)header.record_bytes));
				}
				*count_out = (int)header.count;
			}
		}
		else if (header.count > 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: %s is damaged", name);
		}
	}
	mz_free(data);
	return records;
}

static void read_features(map_file_reader_t *reader, map_features_t *features)
{
	saved_light_t *lights;
	saved_effect_t *effects;
	int light_count;
	int effect_count;

	SDL_zerop(features);
	lights = read_feature_entry(reader, "lights.bin", sizeof(saved_light_t), &light_count);
	effects = read_feature_entry(reader, "effects.bin", sizeof(saved_effect_t), &effect_count);
	features->waterfalls = read_feature_entry(reader, "waterfalls.bin", sizeof(waterfall_segment_t), &features->waterfall_count);
	features->items = read_feature_entry(reader, "items.bin", sizeof(map_item_t), &features->item_count);
	features->harvest = read_feature_entry(reader, "harvest.bin", sizeof(map_harvest_t), &features->harvest_count);
	features->actors = read_feature_entry(reader, "actors.bin", sizeof(map_actor_t), &features->actor_count);
	features->actors_saved = mz_zip_reader_locate_file(&reader->zip, "actors.bin", NULL, 0) >= 0;
	features->areas = read_feature_entry(reader, "areas.bin", sizeof(map_area_t), &features->area_count);
	features->areas_saved = mz_zip_reader_locate_file(&reader->zip, "areas.bin", NULL, 0) >= 0;
	if (!features->areas_saved)
	{
		// (saved while they were called zones)
		features->areas = read_feature_entry(reader, "zones.bin", sizeof(map_area_t), &features->area_count);
		features->areas_saved = mz_zip_reader_locate_file(&reader->zip, "zones.bin", NULL, 0) >= 0;
	}
	for (int i = 0; i < features->area_count; i++)
	{
		features->areas[i].name[sizeof(features->areas[i].name) - 1] = '\0';
	}
	for (int i = 0; i < features->actor_count; i++)
	{
		features->actors[i].type[sizeof(features->actors[i].type) - 1] = '\0';
		features->actors[i].name[sizeof(features->actors[i].name) - 1] = '\0';
	}
	for (int i = 0; i < features->harvest_count; i++)
	{
		features->harvest[i].type[sizeof(features->harvest[i].type) - 1] = '\0';
		features->harvest[i].original[sizeof(features->harvest[i].original) - 1] = '\0';
	}
	for (int i = 0; i < features->item_count; i++)
	{
		features->items[i].item[sizeof(features->items[i].item) - 1] = '\0';
	}
	{
		int layer_size = map_file_sky_layer_size(reader->info.size, reader->info.cell_size_meters);
		int bounce_size = map_file_sky_bounce_size(reader->info.size, reader->info.cell_size_meters);
		int rows = 0;
		int layer_rows = 0;
		int bounce_rows = 0;
		uint8_t *cells = read_feature_entry(reader, "sky_occlusion.bin", (size_t)reader->info.size, &rows);
		uint8_t *layers = cells != NULL ? read_feature_entry(reader, "sky_occlusion_layers.bin", (size_t)layer_size, &layer_rows) : NULL;
		uint8_t *bounce = layers != NULL ? read_feature_entry(reader, "sky_occlusion_bounce.bin", (size_t)bounce_size * 4, &bounce_rows) : NULL;

		// (all three, or none: a map from before them works it out anew)
		if (cells != NULL && rows == reader->info.size && layers != NULL && layer_rows == layer_size * 4 && bounce != NULL
			&& bounce_rows == bounce_size)
		{
			row_differences(cells, rows, rows, true, 1);
			row_differences(layers, layer_size, layer_rows, true, 1);
			row_differences(bounce, bounce_size * 4, bounce_rows, true, 4);
			features->sky_occlusion = cells;
			features->sky_occlusion_size = rows;
			features->sky_occlusion_layers = layers;
			features->sky_occlusion_layer_size = layer_size;
			features->sky_occlusion_bounce = bounce;
			features->sky_occlusion_bounce_size = bounce_size;
		}
		else
		{
			SDL_free(cells);
			SDL_free(layers);
			SDL_free(bounce);
		}
	}
	if (lights != NULL && (features->lights = SDL_calloc((size_t)light_count, sizeof(placed_light_t))) != NULL)
	{
		for (int i = 0; i < light_count; i++)
		{
			placed_light_t *light = &features->lights[i];

			SDL_memcpy(light->position, lights[i].position, sizeof(light->position));
			SDL_memcpy(light->source.color, lights[i].color, sizeof(light->source.color));
			light->source.intensity = lights[i].intensity;
			light->source.radius_meters = lights[i].radius_meters;
			light->source.flicker = lights[i].flicker;
			light->source.height_meters = lights[i].height_meters;
			light->source.core_meters = lights[i].core_meters;
			light->source.switched = lights[i].switched != 0;
		}
		features->light_count = light_count;
	}
	if (effects != NULL && (features->effects = SDL_calloc((size_t)effect_count, sizeof(map_effect_t))) != NULL)
	{
		for (int i = 0; i < effect_count; i++)
		{
			map_effect_t *effect = &features->effects[i];

			SDL_memcpy(effect->effect, effects[i].effect, sizeof(effect->effect));
			effect->effect[MAP_EFFECT_NAME_LENGTH - 1] = '\0';
			SDL_memcpy(effect->position, effects[i].position, sizeof(effect->position));
			SDL_memcpy(effect->rotation, effects[i].rotation, sizeof(effect->rotation));
			SDL_memcpy(effect->extents, effects[i].extents, sizeof(effect->extents));
		}
		features->effect_count = effect_count;
	}
	SDL_free(lights);
	SDL_free(effects);
	if (features->light_count > 0 || features->effect_count > 0 || features->waterfall_count > 0)
	{
		SDL_Log("Map: %d lights, %d effects, %d waterfall segments", features->light_count, features->effect_count, features->waterfall_count);
	}
}

bool map_file_save(const char *path, const terrain_t *terrain, const water_t *water, const objects_t *objects, const camera_t *camera,
	const cloud_paint_t *sky, const map_features_t *features)
{
	uint64_t start_counter = SDL_GetPerformanceCounter();
	size_t cell_count = (size_t)terrain->size * (size_t)terrain->size;
	archive_entry_t entries[40];
	int entry_count = 0;
	char *header;
	size_t header_length = 0;
	const size_t header_capacity = 16384;
	char *type_names;
	size_t type_names_length = 0;
	size_t type_names_capacity = (size_t)SDL_max(objects->types.type_count, 1) * (OBJECT_TYPE_NAME_LENGTH + 1) + 1;
	saved_object_t *saved_objects;
	size_t archive_size = 0;
	bool saved = false;
	uint64_t stage_mark = start_counter;
	double prepare_milliseconds;
	double compress_milliseconds;
	double archive_milliseconds;
	double write_milliseconds;

	SDL_zero(entries);
	header = SDL_malloc(header_capacity);
	type_names = SDL_malloc(type_names_capacity);
	saved_objects = SDL_malloc(sizeof(saved_object_t) * (size_t)SDL_max(objects->instance_count, 1));
	if (header == NULL || type_names == NULL || saved_objects == NULL)
	{
		SDL_free(header);
		SDL_free(type_names);
		SDL_free(saved_objects);
		return false;
	}

	header_length = append_text(header, header_capacity, header_length,
		"; Potop map\n[map]\nversion = %d\nsize = %d\ncell_size_meters = %.9g\nseed = %u\nmax_height_meters = %.9g\n"
		"raining = %s\ndrain_edges = %s\ndepth_scaled_pipes = %s\nnext_group = %d\n",
		MAP_FILE_VERSION, terrain->size, (double)terrain->cell_size_meters, terrain->seed, (double)terrain->max_height_meters,
		water->raining ? "true" : "false", water->drain_edges ? "true" : "false", water->depth_scaled_pipes ? "true" : "false",
		objects->next_group);
	for (int i = 0; i < water->spring_count; i++)
	{
		header_length = append_text(header, header_capacity, header_length, "spring = %d, %d\n", water->springs[i].cell_x,
			water->springs[i].cell_y);
	}
	if (camera != NULL)
	{
		header_length = append_text(header, header_capacity, header_length,
			"\n[camera]\ntarget_x = %.9g\ntarget_z = %.9g\ntarget_height = %.9g\ndistance_meters = %.9g\nyaw_degrees = %.9g\npitch_degrees = %.9g\n",
			(double)camera->target_x, (double)camera->target_z, (double)camera->target_height, (double)camera->distance_meters,
			(double)camera->yaw_degrees, (double)camera->pitch_degrees);
	}

	fill_objects(objects, type_names, type_names_capacity, &type_names_length, saved_objects);

	entries[entry_count++] = (archive_entry_t){ "map.ini", header, header_length, header, NULL, 0, 0 };
	entries[entry_count++] = (archive_entry_t){ "object_types.txt", type_names, type_names_length, type_names, NULL, 0, 0 };
	entries[entry_count++] = (archive_entry_t){ "objects.bin", saved_objects, sizeof(saved_object_t) * (size_t)objects->instance_count,
		saved_objects, NULL, 0, 0 };
	{
		uint32_t *ids = object_ids(objects);

		if (ids != NULL)
		{
			entries[entry_count++] = (archive_entry_t){ "object_ids.bin", ids, sizeof(uint32_t) * (size_t)objects->instance_count, ids, NULL, 0, 0 };
		}
	}
	{
		uint32_t *owners = object_owners(objects);

		if (owners != NULL)
		{
			entries[entry_count++] = (archive_entry_t){ "object_owners.bin", owners, sizeof(uint32_t) * (size_t)objects->instance_count, owners,
				NULL, 0, 0 };
		}
	}
	{
		uint8_t *heights = encode_layer(terrain->heights, NULL, terrain->size);
		uint8_t *bedrock = encode_layer(terrain->heights, terrain->bedrock_heights, terrain->size);
		uint8_t *silt = encode_layer(terrain->silt_thickness, NULL, terrain->size);

		entries[entry_count++] = (archive_entry_t){ "heights.bin", heights, cell_count * sizeof(int32_t), heights, NULL, 0, 0 };
		entries[entry_count++] = (archive_entry_t){ "soil_thickness.bin", bedrock, cell_count * sizeof(int32_t), bedrock, NULL, 0, 0 };
		entries[entry_count++] = (archive_entry_t){ "silt.bin", silt, cell_count * sizeof(int32_t), silt, NULL, 0, 0 };
		if (heights == NULL || bedrock == NULL || silt == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory saving the map");
			goto cleanup;
		}
	}
	entries[entry_count++] = (archive_entry_t){ "soils.bin", terrain->soil_types, cell_count, NULL, NULL, 0, 0 };
	// (base, cover, overlay, shelter per cell: material_map.h)
	entries[entry_count++] = (archive_entry_t){ "materials.bin", terrain->materials, cell_count * TERRAIN_MATERIAL_BYTES, NULL, NULL, 0, 0 };
	entries[entry_count++] = (archive_entry_t){ "water_depth.bin", water->depth, cell_count * sizeof(int32_t), NULL, NULL, 0, 0 };
	entries[entry_count++] = (archive_entry_t){ "sediment.bin", water->sediment, cell_count * sizeof(int32_t), NULL, NULL, 0, 0 };
	entries[entry_count++] = (archive_entry_t){ "moisture.bin", water->moisture, cell_count * sizeof(uint16_t), NULL, NULL, 0, 0 };
	if (sky != NULL)
	{
		entries[entry_count++] = (archive_entry_t){ "sky.bin", sky->values, sizeof(sky->values), NULL, NULL, 0, 0 };
	}
	if (!add_feature_entries(entries, &entry_count, features, false))
	{
		goto cleanup;
	}

	prepare_milliseconds = stage_milliseconds(&stage_mark);
	saved = write_archive(path, entries, entry_count, &archive_size, &compress_milliseconds, &archive_milliseconds, &write_milliseconds);
	if (saved)
	{
		SDL_Log("Saved the map to %s: %.1f MB, %d objects, %llu ms (prepared %.0f, compressed %.0f, archived %.0f, written %.0f)", path,
			(double)archive_size / (1024.0 * 1024.0), objects->instance_count,
			(unsigned long long)((SDL_GetPerformanceCounter() - start_counter) * 1000 / SDL_GetPerformanceFrequency()), prepare_milliseconds,
			compress_milliseconds, archive_milliseconds, write_milliseconds);
	}

cleanup:
	for (int i = 0; i < entry_count; i++)
	{
		mz_free(entries[i].compressed);
		SDL_free(entries[i].owned);
	}
	if (entry_count < 3)
	{
		// Not yet handed to the entries
		SDL_free(header);
		SDL_free(type_names);
		SDL_free(saved_objects);
	}
	return saved;
}

static bool header_key(void *user_data, const char *section, const char *key, const char *value)
{
	map_file_reader_t *reader = user_data;
	map_file_info_t *info = &reader->info;

	if (SDL_strcmp(section, "map") == 0)
	{
		if (SDL_strcmp(key, "version") == 0)
		{
			return ini_value_to_int(value, &info->version);
		}
		if (SDL_strcmp(key, "size") == 0)
		{
			return ini_value_to_int(value, &info->size);
		}
		if (SDL_strcmp(key, "cell_size_meters") == 0)
		{
			return ini_value_to_float(value, &info->cell_size_meters);
		}
		if (SDL_strcmp(key, "seed") == 0)
		{
			info->seed = (uint32_t)SDL_strtoul(value, NULL, 10);
			return true;
		}
		if (SDL_strcmp(key, "max_height_meters") == 0)
		{
			return ini_value_to_float(value, &info->max_height_meters);
		}
		if (SDL_strcmp(key, "raining") == 0)
		{
			return ini_value_to_bool(value, &info->raining);
		}
		if (SDL_strcmp(key, "drain_edges") == 0)
		{
			return ini_value_to_bool(value, &info->drain_edges);
		}
		if (SDL_strcmp(key, "depth_scaled_pipes") == 0)
		{
			return ini_value_to_bool(value, &info->depth_scaled_pipes);
		}
		if (SDL_strcmp(key, "next_group") == 0)
		{
			return ini_value_to_int(value, &reader->next_group);
		}
		if (SDL_strcmp(key, "kind") == 0)
		{
			info->baked = SDL_strcmp(value, "baked") == 0;
			return info->baked || SDL_strcmp(value, "active") == 0;
		}
		if (SDL_strcmp(key, "height_base_units") == 0)
		{
			return ini_value_to_int(value, &reader->height_base_units);
		}
		if (SDL_strcmp(key, "height_step_units") == 0)
		{
			return ini_value_to_int(value, &reader->height_step_units);
		}
		if (SDL_strcmp(key, "height_band_rows") == 0)
		{
			return ini_value_to_int(value, &reader->height_band_rows);
		}
		if (SDL_strcmp(key, "part_bytes") == 0)
		{
			return ini_value_to_int(value, &reader->part_bytes);
		}
		if (SDL_strcmp(key, "tiles_split") == 0)
		{
			return ini_value_to_bool(value, &reader->tiles_split);
		}
		if (SDL_strcmp(key, "palette") == 0 || SDL_strcmp(key, "overlay") == 0)
		{
			int material = -1;

			for (int m = 0; m < MATERIAL_COUNT; m++)
			{
				if (SDL_strcasecmp(value, materials_name((material_type_t)m)) == 0)
				{
					material = m;
				}
			}
			if (key[0] == 'o')
			{
				reader->overlay = material >= 0 ? (uint8_t)material : MATERIAL_NONE;
				return material >= 0 || SDL_strcmp(value, "none") == 0;
			}
			if (reader->palette_count >= PALETTE_SIZE)
			{
				return false;
			}
			// (one it does not know any more: the first, so the cells
			// still show something)
			reader->palette[reader->palette_count++] = (uint8_t)SDL_max(material, 0);
			return material >= 0;
		}
		if (SDL_strcmp(key, "spring") == 0)
		{
			int cell_x;
			int cell_y;

			if (reader->spring_count >= WATER_MAX_SPRINGS || SDL_sscanf(value, "%d , %d", &cell_x, &cell_y) != 2)
			{
				return false;
			}
			reader->springs[reader->spring_count].cell_x = cell_x;
			reader->springs[reader->spring_count].cell_y = cell_y;
			reader->springs[reader->spring_count].remainder_units = 0.0;
			reader->spring_count++;
			return true;
		}
		return false;
	}
	if (SDL_strcmp(section, "camera") == 0)
	{
		float *target = NULL;

		info->has_camera = true;
		if (SDL_strcmp(key, "target_x") == 0)
		{
			target = &info->camera_target_x;
		}
		else if (SDL_strcmp(key, "target_z") == 0)
		{
			target = &info->camera_target_z;
		}
		else if (SDL_strcmp(key, "target_height") == 0)
		{
			target = &info->camera_target_height;
		}
		else if (SDL_strcmp(key, "distance_meters") == 0)
		{
			target = &info->camera_distance_meters;
		}
		else if (SDL_strcmp(key, "yaw_degrees") == 0)
		{
			target = &info->camera_yaw_degrees;
		}
		else if (SDL_strcmp(key, "pitch_degrees") == 0)
		{
			target = &info->camera_pitch_degrees;
		}
		return target != NULL && ini_value_to_float(value, target);
	}
	return false;
}

map_file_reader_t *map_file_open(const char *path, map_file_info_t *info_out)
{
	map_file_reader_t *reader = SDL_calloc(1, sizeof(map_file_reader_t));
	char *header;
	size_t header_size = 0;

	uint64_t read_mark = SDL_GetPerformanceCounter();

	if (reader == NULL)
	{
		return NULL;
	}
	reader->overlay = MATERIAL_NONE;
	reader->archive = file_io_load_file(path, &reader->archive_size);
	if (reader->archive != NULL)
	{
		SDL_Log("Map: read %s, %.1f MB, %.0f ms", path, (double)reader->archive_size / (1024.0 * 1024.0), stage_milliseconds(&read_mark));
	}
	if (reader->archive == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot read %s: %s", path, SDL_GetError());
		SDL_free(reader);
		return NULL;
	}
	if (!mz_zip_reader_init_mem(&reader->zip, reader->archive, reader->archive_size, 0))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s is not a map archive", path);
		map_file_close(reader);
		return NULL;
	}
	header = mz_zip_reader_extract_file_to_heap(&reader->zip, "map.ini", &header_size, 0);
	if (header == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s has no map.ini", path);
		map_file_close(reader);
		return NULL;
	}
	{
		// Zero terminated for the parser
		char *text = SDL_malloc(header_size + 1);

		if (text != NULL)
		{
			SDL_memcpy(text, header, header_size);
			text[header_size] = '\0';
			ini_parse_buffer(text, "map.ini", header_key, reader);
			SDL_free(text);
		}
		mz_free(header);
	}
	if (reader->info.version < 1 || reader->info.version > MAP_FILE_BAKED_VERSION
		|| (reader->info.baked && (reader->height_step_units <= 0 || reader->palette_count == 0)) || reader->info.size < TERRAIN_CHUNK_SIZE
		|| reader->info.size % TERRAIN_CHUNK_SIZE != 0 || reader->info.size > 16384 || reader->info.cell_size_meters <= 0.0f)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: version %d, size %d: not a map this version can read", path, reader->info.version,
			reader->info.size);
		map_file_close(reader);
		return NULL;
	}
	*info_out = reader->info;
	return reader;
}

typedef struct
{
	const char *name;
	void *destination;
	size_t size;
	bool predicted;                // stored by encode_layer
	bool done;
	bool optional;                 // older maps have none: made again
} extract_job_t;

// The painted sky and the objects (shared by both kinds). False if the
// objects are missing
static bool read_sky_and_objects(map_file_reader_t *reader, terrain_t *terrain, objects_t *objects, cloud_paint_t *sky, int *loaded_out,
	int *dropped_out)
{
	bool complete = true;
	int loaded_objects = 0;
	int dropped_objects = 0;

	// The painted sky (older maps have none: a neutral one)
	if (sky != NULL)
	{
		int index = mz_zip_reader_locate_file(&reader->zip, "sky.bin", NULL, 0);
		mz_zip_archive_file_stat stat;

		if (index >= 0 && mz_zip_reader_file_stat(&reader->zip, (mz_uint)index, &stat) && stat.m_uncomp_size == sizeof(sky->values)
			&& mz_zip_reader_extract_to_mem(&reader->zip, (mz_uint)index, sky->values, sizeof(sky->values), 0))
		{
			sky->revision++;
			sky->stamp_revision++;
			sky->changed_ticks = SDL_GetTicks();
		}
		else
		{
			cloud_paint_reset(sky);
		}
	}

	// Objects, their types matched by name
	objects_clear(objects, false);
	{
		size_t names_size = 0;
		size_t records_size = 0;
		char *names = mz_zip_reader_extract_file_to_heap(&reader->zip, "object_types.txt", &names_size, 0);
		void *records = mz_zip_reader_extract_file_to_heap(&reader->zip, "objects.bin", &records_size, 0);
		size_t ids_size = 0;
		uint32_t *ids = mz_zip_reader_extract_file_to_heap(&reader->zip, "object_ids.bin", &ids_size, 0);
		size_t owners_size = 0;
		uint32_t *owners = mz_zip_reader_extract_file_to_heap(&reader->zip, "object_owners.bin", &owners_size, 0);
		size_t record_size = reader->info.version >= 2 ? sizeof(saved_object_t) : sizeof(saved_object_v1_t);
		int *type_map = NULL;
		int saved_type_count = 0;

		if (names != NULL && records != NULL)
		{
			char *text = SDL_malloc(names_size + 1);

			type_map = SDL_malloc(sizeof(int) * (names_size + 1));
			if (text != NULL && type_map != NULL)
			{
				char *line = text;

				SDL_memcpy(text, names, names_size);
				text[names_size] = '\0';
				while (*line != '\0')
				{
					char *end = SDL_strchr(line, '\n');

					if (end != NULL)
					{
						*end = '\0';
					}
					type_map[saved_type_count++] = object_types_find(&objects->types, line);
					if (end == NULL)
					{
						break;
					}
					line = end + 1;
				}
				for (size_t i = 0; i < records_size / record_size; i++)
				{
					saved_object_t record;
					int type;

					// Version 1 records are the start of version 2 ones
					SDL_zero(record);
					SDL_memcpy(&record, (const uint8_t *)records + i * record_size, record_size);
					type = record.type >= 0 && record.type < saved_type_count ? type_map[record.type] : -1;
					// (a module's part: its module puts it back)
					if (type >= 0 && objects->types.types[type].module_type >= 0)
					{
						type = -1;
					}
					if (type >= 0 && objects_add_lifted(objects, terrain, type, record.x, record.z, record.yaw_radians, record.scale,
						record.group, record.lift_meters))
					{
						object_instance_t *instance = &objects->instances[objects->instance_count - 1];

						// (its id as saved; a map from before ids, or one taken: the new one)
						if (ids != NULL && (i + 1) * sizeof(uint32_t) <= ids_size)
						{
							objects_set_id(objects, objects->instance_count - 1, ids[i]);
						}
						if (owners != NULL && (i + 1) * sizeof(uint32_t) <= owners_size)
						{
							instance->owner = owners[i];
						}
						if (record_size == sizeof(saved_object_t))
						{
							SDL_memcpy(instance->rotation, record.rotation, sizeof(instance->rotation));
							instance->y = record.y;
							instance->flatten = record.flatten;
							instance->flatten_yaw = record.flatten_yaw;
							instance->state = (uint8_t)(record.state & ~OBJECT_STATE_AWAKE);
							instance->damage = record.damage;
						}
						loaded_objects++;
					}
					else
					{
						dropped_objects++;
					}
				}
			}
			SDL_free(text);
		}
		else
		{
			complete = false;
		}
		SDL_free(type_map);
		mz_free(names);
		mz_free(records);
		mz_free(ids);
		mz_free(owners);
		objects->next_group = SDL_max(objects->next_group, reader->next_group);
	}
	*loaded_out = loaded_objects;
	*dropped_out = dropped_objects;
	return complete;
}

// ---------------------------------------------------------------------------
// Baked maps (map_file.h): the ground as 16 bits a cell, the material map
// as two bytes, and the water as the simulation left it, only in the tiles
// that have any (or show what it left: silt, wet ground, rock laid bare)

// The planes of a baked tile in water_tiles.bin, each TILE_CELLS bytes:
// the water's depth (in height steps, low and high byte), its flow
// (WATER_BAKED_FLOW_BYTES), the surface (TERRAIN_BAKED_SURFACE_BYTES)
#define BAKED_TILE_CELLS (TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE)
#define BAKED_PLANE_DEPTH 0
#define BAKED_PLANE_FLOW 2
#define BAKED_PLANE_SURFACE (BAKED_PLANE_FLOW + WATER_BAKED_FLOW_BYTES)
#define BAKED_PLANES (BAKED_PLANE_SURFACE + TERRAIN_BAKED_SURFACE_BYTES)
#define BAKED_TILES_HEADER_BYTES 8

static const char *baked_plane_names[BAKED_PLANES] = { "water_depth_low.bin", "water_depth_high.bin", "water_velocity_x.bin",
	"water_velocity_z.bin", "water_mud.bin", "water_rush.bin", "ground_silt.bin", "ground_wetness.bin", "ground_bare.bin" };

// 16 bit values, predicted like encode_layer, in two planes of bytes. In
// bands of band_rows rows (0: one band), each predicted on its own (its
// first row from the left alone), so that they decode side by side
static uint8_t *encode_layer16(const uint16_t *values, int32_t size, int32_t band_rows)
{
	size_t count = (size_t)size * (size_t)size;
	uint8_t *planes = SDL_malloc(count * 2);

	if (planes == NULL)
	{
		return NULL;
	}
#pragma omp parallel for schedule(static)
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			size_t index = (size_t)y * (size_t)size + (size_t)x;
			bool first_row = band_rows > 0 ? y % band_rows == 0 : y == 0;
			uint32_t left = x > 0 ? values[index - 1] : 0u;
			uint32_t above = !first_row ? values[index - (size_t)size] : 0u;
			uint32_t above_left = x > 0 && !first_row ? values[index - (size_t)size - 1] : 0u;
			uint32_t predicted = x > 0 && !first_row ? left + above - above_left : (x > 0 ? left : above);
			int16_t difference = (int16_t)(uint16_t)((uint32_t)values[index] - predicted);
			uint16_t folded = (uint16_t)(((uint16_t)difference << 1) ^ (uint16_t)(difference >> 15));

			planes[index] = (uint8_t)folded;
			planes[count + index] = (uint8_t)(folded >> 8);
		}
	}
	return planes;
}

static void decode_layer16(const uint8_t *planes, uint16_t *values, int32_t size, int32_t band_rows)
{
	size_t count = (size_t)size * (size_t)size;
	int32_t rows = band_rows > 0 ? band_rows : size;
	int32_t band_count = (size + rows - 1) / rows;

#pragma omp parallel for schedule(dynamic, 1)
	for (int32_t band = 0; band < band_count; band++)
	{
		int32_t last_y = SDL_min((band + 1) * rows, size);

		for (int32_t y = band * rows; y < last_y; y++)
		{
			bool first_row = y == band * rows;

			for (int32_t x = 0; x < size; x++)
			{
				size_t index = (size_t)y * (size_t)size + (size_t)x;
				uint16_t folded = (uint16_t)(planes[index] | planes[count + index] << 8);
				uint16_t difference = (uint16_t)((folded >> 1) ^ (uint16_t)(0u - (folded & 1u)));
				uint32_t left = x > 0 ? values[index - 1] : 0u;
				uint32_t above = !first_row ? values[index - (size_t)size] : 0u;
				uint32_t above_left = x > 0 && !first_row ? values[index - (size_t)size - 1] : 0u;
				uint32_t predicted = x > 0 && !first_row ? left + above - above_left : (x > 0 ? left : above);

				values[index] = (uint16_t)(predicted + difference);
			}
		}
	}
}

// Big layers go into the archive in parts of this many bytes (NAME.0,
// NAME.1, ...: they unpack side by side), the heights predicted in bands
// of this many rows
#define BAKED_PART_BYTES (2u << 20)
#define BAKED_HEIGHT_BAND_ROWS 256
#define BAKED_MOST_PARTS 1024

// The layers' entries cut into parts (those over BAKED_PART_BYTES whose
// names say they are layers); names into part_names. Returns the count
static int split_layer_entries(const archive_entry_t *entries, int entry_count, archive_entry_t *parts_out, int parts_capacity,
	char (*part_names)[64])
{
	int count = 0;

	for (int i = 0; i < entry_count; i++)
	{
		const char *name = entries[i].name;
		bool layer = SDL_strncmp(name, "heights16", 9) == 0 || SDL_strncmp(name, "materials16", 11) == 0
			|| (SDL_strncmp(name, "water_", 6) == 0 && SDL_strcmp(name, "water_tiles.bin") != 0) || SDL_strncmp(name, "ground_", 7) == 0;

		if (!layer || entries[i].size <= BAKED_PART_BYTES)
		{
			if (count < parts_capacity)
			{
				parts_out[count++] = entries[i];
			}
			continue;
		}
		for (size_t offset = 0; offset < entries[i].size && count < parts_capacity; offset += BAKED_PART_BYTES)
		{
			archive_entry_t *part = &parts_out[count];

			SDL_zerop(part);
			SDL_snprintf(part_names[count], 64, "%s.%u", name, (unsigned)(offset / BAKED_PART_BYTES));
			part->name = part_names[count];
			part->data = (const uint8_t *)entries[i].data + offset;
			part->size = SDL_min((size_t)BAKED_PART_BYTES, entries[i].size - offset);
			// (the whole of it freed with the first part)
			part->owned = offset == 0 ? entries[i].owned : NULL;
			count++;
		}
	}
	return count;
}

// The water's depth as drawn (units): over a wall (a roof) it lies on the
// wall's top, and a film there is none (as water_render.comp draws it)
static int32_t drawn_depth_units(const water_t *water, size_t index)
{
	int32_t depth = water->depth[index];

	if (water->obstacle_centimeters != NULL && water->obstacle_centimeters[index] > 0)
	{
		return (float)depth * TERRAIN_METERS_PER_HEIGHT_UNIT > 0.05f
			? depth + (int32_t)water->obstacle_centimeters[index] * WATER_UNITS_PER_OBSTACLE_CENTIMETER : 0;
	}
	return depth;
}

// A cell's flow as the water textures show it (water_render.comp), in
// bytes (WATER_BAKED_FLOW_BYTES)
static void flow_bytes(const water_t *water, int32_t x, int32_t y, uint8_t bytes_out[WATER_BAKED_FLOW_BYTES])
{
	size_t index = (size_t)y * (size_t)water->size + (size_t)x;
	float depth = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
	float own[WATER_DIRECTION_COUNT];
	float in_from_left;
	float in_from_right;
	float in_from_back;
	float in_from_front;
	float cross_section;
	float largest = 0.0f;

	SDL_memset(bytes_out, 0, WATER_BAKED_FLOW_BYTES);
	if (depth <= 0.0f)
	{
		return;
	}
	if (water->baked)
	{
		const uint8_t *flow = water->baked_flow[water_tile_index_of_cell(water, x, y)];

		if (flow != NULL)
		{
			SDL_memcpy(bytes_out, flow + ((y % WATER_TILE_SIZE) * WATER_TILE_SIZE + x % WATER_TILE_SIZE) * WATER_BAKED_FLOW_BYTES,
				WATER_BAKED_FLOW_BYTES);
		}
		return;
	}
	for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
	{
		own[k] = (float)water->flux[k][index];
		largest = SDL_max(largest, SDL_fabsf(own[k]));
	}
	in_from_left = x > 0 ? (float)water->flux[WATER_DIRECTION_RIGHT][index - 1] : 0.0f;
	in_from_right = x < water->size - 1 ? (float)water->flux[WATER_DIRECTION_LEFT][index + 1] : 0.0f;
	in_from_back = y > 0 ? (float)water->flux[WATER_DIRECTION_FRONT][index - (size_t)water->size] : 0.0f;
	in_from_front = y < water->size - 1 ? (float)water->flux[WATER_DIRECTION_BACK][index + (size_t)water->size] : 0.0f;
	cross_section = SDL_max(depth, 0.1f) * water->cell_size_meters;
	bytes_out[0] = water_baked_encode_velocity(0.5f * (in_from_left - own[WATER_DIRECTION_LEFT] + own[WATER_DIRECTION_RIGHT] - in_from_right)
		/ cross_section);
	bytes_out[1] = water_baked_encode_velocity(0.5f * (in_from_back - own[WATER_DIRECTION_BACK] + own[WATER_DIRECTION_FRONT] - in_from_front)
		/ cross_section);
	bytes_out[2] = water_baked_encode_amount((float)water->sediment[index] / (float)water->depth[index], WATER_BAKED_MUDDIEST);
	bytes_out[3] = water_baked_encode_amount(largest / (SDL_max(depth, water->speed_depth_meters) * water->cell_size_meters),
		WATER_BAKED_FASTEST);
}

static uint16_t height_step_of(int64_t units, int32_t base, int32_t step)
{
	int64_t steps = (units - base + step / 2) / step;

	return (uint16_t)SDL_clamp(steps, 0, 65535);
}

// The soil a baked map's cell is made of, by what it shows
static uint8_t soil_of_material(uint8_t material)
{
	switch (material)
	{
	case MATERIAL_ROCK: return SOIL_ROCK;
	case MATERIAL_SAND: return SOIL_SAND;
	case MATERIAL_CLAY: return SOIL_CLAY;
	default: return SOIL_EARTH;
	}
}

bool map_file_save_baked(const char *path, const terrain_t *terrain, const water_t *water, const objects_t *objects, const camera_t *camera,
	const cloud_paint_t *sky, const soil_settings_t *soil_settings, const map_features_t *features)
{
	uint64_t start_counter = SDL_GetPerformanceCounter();
	uint64_t stage_mark = start_counter;
	int32_t size = terrain->size;
	size_t cell_count = (size_t)size * (size_t)size;
	int32_t tiles_per_side = terrain->chunks_per_side;
	int32_t tile_count = tiles_per_side * tiles_per_side;
	archive_entry_t entries[32];
	int entry_count = 0;
	char *header = NULL;
	size_t header_length = 0;
	const size_t header_capacity = 16384;
	char *type_names = NULL;
	size_t type_names_length = 0;
	size_t type_names_capacity = (size_t)SDL_max(objects->types.type_count, 1) * (OBJECT_TYPE_NAME_LENGTH + 1) + 1;
	saved_object_t *saved_objects = NULL;
	uint16_t *heights16 = NULL;
	uint8_t *materials16 = NULL;
	uint8_t *tile_kept = NULL;
	int32_t *tile_slot = NULL;
	int32_t kept_count = 0;
	int32_t wet_count = 0;
	int32_t base;
	int32_t step;
	int64_t material_counts[MATERIAL_COUNT];
	int64_t overlay_counts[MATERIAL_COUNT];
	uint8_t palette[PALETTE_SIZE];
	int palette_count = 0;
	uint8_t palette_index[256];
	uint8_t overlay = MATERIAL_NONE;
	size_t archive_size = 0;
	bool saved = false;
	double prepare_milliseconds;
	double compress_milliseconds;
	double archive_milliseconds;
	double write_milliseconds;

	SDL_zero(entries);
	SDL_zero(material_counts);
	SDL_zero(overlay_counts);
	if (terrain->size != water->size || terrain->chunks_per_side != water->tiles_per_side)
	{
		return false;
	}
	header = SDL_malloc(header_capacity);
	type_names = SDL_malloc(type_names_capacity);
	saved_objects = SDL_malloc(sizeof(saved_object_t) * (size_t)SDL_max(objects->instance_count, 1));
	heights16 = SDL_malloc(cell_count * sizeof(uint16_t));
	materials16 = SDL_malloc(cell_count * 2);
	tile_kept = SDL_calloc((size_t)tile_count, 1);
	tile_slot = SDL_calloc((size_t)tile_count, sizeof(int32_t));
	if (header == NULL || type_names == NULL || saved_objects == NULL || heights16 == NULL || materials16 == NULL || tile_kept == NULL
		|| tile_slot == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory baking the map");
		goto cleanup;
	}

	// The heights' range, the water's too (its level is stored in the same
	// steps, so a lake stays flat); a baked map keeps its own
	if (terrain->baked)
	{
		base = terrain->baked_height_base_units;
		step = terrain->baked_height_step_units;
	}
	else
	{
		int64_t lowest = INT64_MAX;
		int64_t highest = INT64_MIN;

		for (size_t i = 0; i < cell_count; i++)
		{
			lowest = SDL_min(lowest, (int64_t)terrain->heights[i]);
			highest = SDL_max(highest, (int64_t)terrain->heights[i] + drawn_depth_units(water, i));
		}
		base = (int32_t)lowest;
		step = (int32_t)SDL_max(1, (highest - lowest + 65534) / 65535);
	}
#pragma omp parallel for schedule(static)
	for (int64_t i = 0; i < (int64_t)cell_count; i++)
	{
		heights16[i] = height_step_of(terrain->heights[i], base, step);
	}

	// The material map: the palette of what it uses (the most used, if
	// more than it holds), then two bytes a cell
	for (size_t i = 0; i < cell_count; i++)
	{
		const uint8_t *cell = &terrain->materials[i * TERRAIN_MATERIAL_BYTES];

		for (int k = 0; k < 2; k++)
		{
			if (cell[k] < MATERIAL_COUNT)
			{
				material_counts[cell[k]]++;
			}
		}
		if (cell[MATERIAL_MAP_OVERLAY] < MATERIAL_COUNT)
		{
			overlay_counts[cell[MATERIAL_MAP_OVERLAY]]++;
		}
	}
	SDL_memset(palette_index, PALETTE_NONE, sizeof(palette_index));
	for (;;)
	{
		int most = -1;

		for (int m = 0; m < MATERIAL_COUNT; m++)
		{
			if (material_counts[m] > 0 && palette_index[m] == PALETTE_NONE && (most < 0 || material_counts[m] > material_counts[most]))
			{
				most = m;
			}
		}
		if (most < 0)
		{
			break;
		}
		if (palette_count == PALETTE_SIZE)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Bake: more than %d materials; %s shows as %s", PALETTE_SIZE,
				materials_name((material_type_t)most), materials_name((material_type_t)palette[0]));
			palette_index[most] = 0;
			continue;
		}
		palette_index[most] = (uint8_t)palette_count;
		palette[palette_count++] = (uint8_t)most;
	}
	if (palette_count == 0)
	{
		palette[palette_count++] = MATERIAL_GRASS;
	}
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		if (overlay_counts[m] > 0 && (overlay == MATERIAL_NONE || overlay_counts[m] > overlay_counts[overlay]))
		{
			if (overlay != MATERIAL_NONE)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Bake: only one overlay is kept: %s, not %s", materials_name((material_type_t)m),
					materials_name((material_type_t)overlay));
			}
			overlay = (uint8_t)m;
		}
	}
#pragma omp parallel for schedule(static)
	for (int64_t i = 0; i < (int64_t)cell_count; i++)
	{
		const uint8_t *cell = &terrain->materials[(size_t)i * TERRAIN_MATERIAL_BYTES];
		uint8_t base_index = palette_index[cell[MATERIAL_MAP_BASE]];
		uint8_t cover_index = palette_index[cell[MATERIAL_MAP_COVER]];

		// (a base it has no room for: the first)
		materials16[i] = (uint8_t)((base_index == PALETTE_NONE ? 0 : base_index) | cover_index << 4);
		materials16[cell_count + (size_t)i] = (uint8_t)((cell[MATERIAL_MAP_OVERLAY] != MATERIAL_NONE && cell[MATERIAL_MAP_OVERLAY] == overlay ? 0x80 : 0)
);
	}

	// The tiles worth keeping, in two groups: those with water in them
	// (its depth and flow), and those with something left on the ground
	// (the surface); then their planes, each group's own
#pragma omp parallel for schedule(dynamic, 4)
	for (int32_t tile = 0; tile < tile_count; tile++)
	{
		int32_t start_x = (tile % tiles_per_side) * TERRAIN_CHUNK_SIZE;
		int32_t start_y = (tile / tiles_per_side) * TERRAIN_CHUNK_SIZE;
		bool wet = false;
		bool shows = false;

		for (int32_t y = start_y; y < start_y + TERRAIN_CHUNK_SIZE && !(wet && shows); y++)
		{
			for (int32_t x = start_x; x < start_x + TERRAIN_CHUNK_SIZE && !(wet && shows); x++)
			{
				size_t index = (size_t)y * (size_t)size + (size_t)x;
				uint8_t surface[TERRAIN_BAKED_SURFACE_BYTES];

				terrain_surface_bytes(terrain, water, soil_settings, (int32_t)index, surface);
				wet = wet || drawn_depth_units(water, index) > 0;
				shows = shows || surface[0] != 0 || surface[1] != 0 || surface[2] != 0;
			}
		}
		tile_kept[tile] = (uint8_t)((wet ? 1 : 0) | (shows ? 2 : 0));
	}
	{
		const char *list_names[2] = { "water_tiles.bin", "ground_tiles.bin" };
		int first_planes[2] = { BAKED_PLANE_DEPTH, BAKED_PLANE_SURFACE };
		int plane_counts[2] = { BAKED_PLANE_SURFACE, TERRAIN_BAKED_SURFACE_BYTES };

		for (int g = 0; g < 2; g++)
		{
			int32_t count = 0;
			size_t list_bytes;
			size_t plane_bytes;
			uint8_t *group_data;
			uint32_t *words;

			for (int32_t tile = 0; tile < tile_count; tile++)
			{
				tile_slot[tile] = (tile_kept[tile] & (1 << g)) ? count++ : -1;
			}
			list_bytes = BAKED_TILES_HEADER_BYTES + 4 * (size_t)count;
			plane_bytes = (size_t)count * BAKED_TILE_CELLS;
			group_data = SDL_calloc(list_bytes + plane_bytes * (size_t)plane_counts[g], 1);
			if (group_data == NULL)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory baking the map's water");
				goto cleanup;
			}
			words = (uint32_t *)group_data;
			words[0] = (uint32_t)count;
			words[1] = BAKED_TILE_CELLS;
			for (int32_t tile = 0; tile < tile_count; tile++)
			{
				if (tile_slot[tile] >= 0)
				{
					words[2 + tile_slot[tile]] = (uint32_t)tile;
				}
			}
#pragma omp parallel for schedule(dynamic, 4)
			for (int32_t tile = 0; tile < tile_count; tile++)
			{
				int32_t start_x = (tile % tiles_per_side) * TERRAIN_CHUNK_SIZE;
				int32_t start_y = (tile / tiles_per_side) * TERRAIN_CHUNK_SIZE;
				uint8_t *planes;

				if (tile_slot[tile] < 0)
				{
					continue;
				}
				planes = group_data + list_bytes + (size_t)tile_slot[tile] * BAKED_TILE_CELLS;
				for (int32_t y = 0; y < TERRAIN_CHUNK_SIZE; y++)
				{
					for (int32_t x = 0; x < TERRAIN_CHUNK_SIZE; x++)
					{
						size_t index = (size_t)(start_y + y) * (size_t)size + (size_t)(start_x + x);
						size_t cell = (size_t)y * TERRAIN_CHUNK_SIZE + (size_t)x;

						if (g == 0)
						{
							int32_t drawn = drawn_depth_units(water, index);
							uint8_t flow[WATER_BAKED_FLOW_BYTES];
							uint16_t depth_steps = 0;

							if (drawn > 0)
							{
								uint16_t level = height_step_of((int64_t)terrain->heights[index] + drawn, base, step);

								depth_steps = (uint16_t)SDL_max((int32_t)level - (int32_t)heights16[index], 0);
							}
							flow_bytes(water, start_x + x, start_y + y, flow);
							planes[(BAKED_PLANE_DEPTH + 0) * plane_bytes + cell] = (uint8_t)depth_steps;
							planes[(BAKED_PLANE_DEPTH + 1) * plane_bytes + cell] = (uint8_t)(depth_steps >> 8);
							for (int k = 0; k < WATER_BAKED_FLOW_BYTES; k++)
							{
								planes[(BAKED_PLANE_FLOW + k) * plane_bytes + cell] = depth_steps > 0 ? flow[k] : 0;
							}
						}
						else
						{
							uint8_t surface[TERRAIN_BAKED_SURFACE_BYTES];

							terrain_surface_bytes(terrain, water, soil_settings, (int32_t)index, surface);
							for (int k = 0; k < TERRAIN_BAKED_SURFACE_BYTES; k++)
							{
								planes[(size_t)k * plane_bytes + cell] = surface[k];
							}
						}
					}
				}
			}
			// (the list, then each plane an entry of its own: they come
			// out side by side)
			entries[entry_count++] = (archive_entry_t){ list_names[g], group_data, list_bytes, group_data, NULL, 0, 0 };
			for (int k = 0; k < plane_counts[g]; k++)
			{
				entries[entry_count++] = (archive_entry_t){ baked_plane_names[first_planes[g] + k], group_data + list_bytes + (size_t)k * plane_bytes,
					plane_bytes, NULL, NULL, 0, 0 };
			}
			kept_count += count;
			if (g == 0)
			{
				wet_count = count;
			}
		}
	}

	header_length = append_text(header, header_capacity, header_length,
		"; Potop map, baked (for play: no simulation)\n[map]\nversion = %d\nkind = baked\nsize = %d\ncell_size_meters = %.9g\nseed = %u\n"
		"max_height_meters = %.9g\nnext_group = %d\nheight_base_units = %d\nheight_step_units = %d\nheight_band_rows = %d\npart_bytes = %u\ntiles_split = true\n",
		MAP_FILE_BAKED_VERSION, terrain->size, (double)terrain->cell_size_meters, terrain->seed, (double)terrain->max_height_meters,
		objects->next_group, base, step, BAKED_HEIGHT_BAND_ROWS, BAKED_PART_BYTES);
	for (int i = 0; i < palette_count; i++)
	{
		header_length = append_text(header, header_capacity, header_length, "palette = %s\n", materials_name((material_type_t)palette[i]));
	}
	header_length = append_text(header, header_capacity, header_length, "overlay = %s\n",
		overlay == MATERIAL_NONE ? "none" : materials_name((material_type_t)overlay));
	if (camera != NULL)
	{
		header_length = append_text(header, header_capacity, header_length,
			"\n[camera]\ntarget_x = %.9g\ntarget_z = %.9g\ntarget_height = %.9g\ndistance_meters = %.9g\nyaw_degrees = %.9g\npitch_degrees = %.9g\n",
			(double)camera->target_x, (double)camera->target_z, (double)camera->target_height, (double)camera->distance_meters,
			(double)camera->yaw_degrees, (double)camera->pitch_degrees);
	}
	fill_objects(objects, type_names, type_names_capacity, &type_names_length, saved_objects);

	entries[entry_count++] = (archive_entry_t){ "map.ini", header, header_length, header, NULL, 0, 0 };
	header = NULL;
	entries[entry_count++] = (archive_entry_t){ "object_types.txt", type_names, type_names_length, type_names, NULL, 0, 0 };
	type_names = NULL;
	entries[entry_count++] = (archive_entry_t){ "objects.bin", saved_objects, sizeof(saved_object_t) * (size_t)objects->instance_count,
		saved_objects, NULL, 0, 0 };
	{
		uint32_t *ids = object_ids(objects);

		if (ids != NULL)
		{
			entries[entry_count++] = (archive_entry_t){ "object_ids.bin", ids, sizeof(uint32_t) * (size_t)objects->instance_count, ids, NULL, 0, 0 };
		}
	}
	{
		uint32_t *owners = object_owners(objects);

		if (owners != NULL)
		{
			entries[entry_count++] = (archive_entry_t){ "object_owners.bin", owners, sizeof(uint32_t) * (size_t)objects->instance_count, owners,
				NULL, 0, 0 };
		}
	}
	saved_objects = NULL;
	{
		uint8_t *heights = encode_layer16(heights16, size, BAKED_HEIGHT_BAND_ROWS);

		if (heights == NULL)
		{
			goto cleanup;
		}
		entries[entry_count++] = (archive_entry_t){ "heights16_low.bin", heights, cell_count, heights, NULL, 0, 0 };
		entries[entry_count++] = (archive_entry_t){ "heights16_high.bin", heights + cell_count, cell_count, NULL, NULL, 0, 0 };
	}
	entries[entry_count++] = (archive_entry_t){ "materials16_low.bin", materials16, cell_count, materials16, NULL, 0, 0 };
	entries[entry_count++] = (archive_entry_t){ "materials16_high.bin", materials16 + cell_count, cell_count, NULL, NULL, 0, 0 };
	materials16 = NULL;
	if (sky != NULL)
	{
		entries[entry_count++] = (archive_entry_t){ "sky.bin", sky->values, sizeof(sky->values), NULL, NULL, 0, 0 };
	}
	if (!add_feature_entries(entries, &entry_count, features, true))
	{
		goto cleanup;
	}

	prepare_milliseconds = stage_milliseconds(&stage_mark);
	{
		archive_entry_t *parts = SDL_calloc(BAKED_MOST_PARTS, sizeof(archive_entry_t));
		char (*part_names)[64] = SDL_calloc(BAKED_MOST_PARTS, 64);
		int part_count;

		if (parts == NULL || part_names == NULL)
		{
			SDL_free(parts);
			SDL_free(part_names);
			goto cleanup;
		}
		part_count = split_layer_entries(entries, entry_count, parts, BAKED_MOST_PARTS, part_names);
		saved = write_archive(path, parts, part_count, &archive_size, &compress_milliseconds, &archive_milliseconds, &write_milliseconds);
		// (what was compressed is the parts'; the entries keep what they own)
		for (int i = 0; i < part_count; i++)
		{
			mz_free(parts[i].compressed);
		}
		SDL_free(parts);
		SDL_free(part_names);
	}
	if (saved)
	{
		SDL_Log("Baked the map to %s: %.1f MB (of %d tiles %d with water, %d with what it left on the ground, %d materials, heights in %.2f mm "
			"steps), %d objects, %llu ms (prepared %.0f, compressed %.0f, archived %.0f, written %.0f)", path, (double)archive_size / (1024.0 * 1024.0),
			tile_count, wet_count, kept_count - wet_count, palette_count, (double)step * 1000.0 / (double)TERRAIN_HEIGHT_UNITS_PER_METER, objects->instance_count,
			(unsigned long long)((SDL_GetPerformanceCounter() - start_counter) * 1000 / SDL_GetPerformanceFrequency()), prepare_milliseconds,
			compress_milliseconds, archive_milliseconds, write_milliseconds);
	}

cleanup:
	for (int i = 0; i < entry_count; i++)
	{
		mz_free(entries[i].compressed);
		SDL_free(entries[i].owned);
	}
	SDL_free(header);
	SDL_free(type_names);
	SDL_free(saved_objects);
	SDL_free(heights16);
	SDL_free(materials16);
	SDL_free(tile_kept);
	SDL_free(tile_slot);
	return saved;
}

// An entry of the archive whole, or NULL (missing, or not size bytes when
// size is not 0); mz_free it
static void *extract_entry(const map_file_reader_t *reader, const char *name, size_t size, size_t *size_out)
{
	mz_zip_archive zip;
	void *data = NULL;
	size_t data_size = 0;

	SDL_zero(zip);
	if (!mz_zip_reader_init_mem(&zip, reader->archive, reader->archive_size, 0))
	{
		return NULL;
	}
	data = mz_zip_reader_extract_file_to_heap(&zip, name, &data_size, 0);
	mz_zip_reader_end(&zip);
	if (data != NULL && size != 0 && data_size != size)
	{
		mz_free(data);
		return NULL;
	}
	if (size_out != NULL)
	{
		*size_out = data_size;
	}
	return data;
}

// An entry of the archive into destination, which holds exactly size
// bytes (its own reader: for threads)
static bool extract_entry_to(const map_file_reader_t *reader, const char *name, void *destination, size_t size)
{
	mz_zip_archive zip;
	mz_zip_archive_file_stat stat;
	int index;
	bool done = false;

	SDL_zero(zip);
	if (!mz_zip_reader_init_mem(&zip, reader->archive, reader->archive_size, 0))
	{
		return false;
	}
	index = mz_zip_reader_locate_file(&zip, name, NULL, 0);
	if (index >= 0 && mz_zip_reader_file_stat(&zip, (mz_uint)index, &stat) && stat.m_uncomp_size == size)
	{
		done = size == 0 || mz_zip_reader_extract_to_mem(&zip, (mz_uint)index, destination, size, 0);
	}
	mz_zip_reader_end(&zip);
	return done;
}

static bool read_baked(map_file_reader_t *reader, terrain_t *terrain, water_t *water, objects_t *objects, cloud_paint_t *sky)
{
	uint64_t start_counter = SDL_GetPerformanceCounter();
	uint64_t stage_mark = start_counter;
	int32_t size = terrain->size;
	size_t cell_count = (size_t)size * (size_t)size;
	int32_t tile_count = terrain->chunks_per_side * terrain->chunks_per_side;
	void *data[2] = { NULL, NULL };
	typedef struct
	{
		const char *list_name;
		int first_plane;
		int plane_count;
		uint32_t *words;
		int32_t count;
		uint8_t *planes;
	} tile_group_t;
	tile_group_t groups[2] = { 0 };
	int group_count = 1;
	size_t list_size = 0;
	const char *names[4 + BAKED_PLANES] = { "heights16_low.bin", "heights16_high.bin", "materials16_low.bin", "materials16_high.bin" };
	uint8_t *destinations[4 + BAKED_PLANES];
	size_t sizes[4 + BAKED_PLANES];
	bool extracted[4 + BAKED_PLANES];
	int job_count = 4;
	uint16_t *heights16 = NULL;
	int32_t base = reader->height_base_units;
	int32_t step = reader->height_step_units;
	int32_t kept_count = 0;
	int loaded_objects = 0;
	int dropped_objects = 0;
	bool complete = true;
	double extract_milliseconds;
	double unpack_milliseconds;

	// The lists of the tiles kept first (small), then every plane side by
	// side: data[0] the heights', [1] the material map's, each group of
	// tiles its own (the first baked maps: one group, all planes; then the
	// tiles with water, and those with something on the ground, apart)
	if (reader->tiles_split)
	{
		groups[0] = (tile_group_t){ "water_tiles.bin", BAKED_PLANE_DEPTH, BAKED_PLANE_SURFACE, NULL, 0, NULL };
		groups[1] = (tile_group_t){ "ground_tiles.bin", BAKED_PLANE_SURFACE, TERRAIN_BAKED_SURFACE_BYTES, NULL, 0, NULL };
		group_count = 2;
	}
	else
	{
		groups[0] = (tile_group_t){ "water_tiles.bin", 0, BAKED_PLANES, NULL, 0, NULL };
		group_count = 1;
	}
	for (int g = 0; g < group_count; g++)
	{
		tile_group_t *group = &groups[g];

		group->words = extract_entry(reader, group->list_name, 0, &list_size);
		if (group->words != NULL && list_size >= BAKED_TILES_HEADER_BYTES)
		{
			group->count = (int32_t)group->words[0];
			if (group->words[1] != BAKED_TILE_CELLS || group->count < 0 || group->count > tile_count
				|| list_size != BAKED_TILES_HEADER_BYTES + 4 * (size_t)group->count)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: %s is damaged", group->list_name);
				group->count = 0;
				complete = false;
			}
		}
		else
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: %s is missing", group->list_name);
			complete = false;
		}
		group->planes = SDL_malloc(SDL_max((size_t)group->count * BAKED_TILE_CELLS * (size_t)group->plane_count, 1));
		kept_count += group->count;
	}
	data[0] = SDL_malloc(cell_count * 2);
	data[1] = SDL_malloc(cell_count * 2);
	heights16 = SDL_malloc(cell_count * sizeof(uint16_t));
	if (data[0] == NULL || data[1] == NULL || heights16 == NULL || groups[0].planes == NULL || (group_count > 1 && groups[1].planes == NULL))
	{
		complete = false;
		goto done;
	}
	for (int i = 0; i < 4; i++)
	{
		destinations[i] = (uint8_t *)data[i / 2] + (size_t)(i % 2) * cell_count;
		sizes[i] = cell_count;
	}
	for (int g = 0; g < group_count; g++)
	{
		for (int k = 0; k < groups[g].plane_count && groups[g].count > 0; k++)
		{
			names[job_count] = baked_plane_names[groups[g].first_plane + k];
			destinations[job_count] = groups[g].planes + (size_t)k * (size_t)groups[g].count * BAKED_TILE_CELLS;
			sizes[job_count] = (size_t)groups[g].count * BAKED_TILE_CELLS;
			job_count++;
		}
	}
	// (each layer in its parts, if it was cut: every part a job of its own)
	{
		typedef struct
		{
			char name[64];
			uint8_t *destination;
			size_t size;
			int layer;
			bool done;
		} part_job_t;
		size_t part_bytes = reader->part_bytes > 0 ? (size_t)reader->part_bytes : 0;
		int part_count = 0;
		part_job_t *parts;

		for (int i = 0; i < job_count; i++)
		{
			part_count += part_bytes > 0 && sizes[i] > part_bytes ? (int)((sizes[i] + part_bytes - 1) / part_bytes) : 1;
		}
		parts = SDL_calloc((size_t)SDL_max(part_count, 1), sizeof(part_job_t));
		if (parts == NULL)
		{
			complete = false;
			goto done;
		}
		part_count = 0;
		for (int i = 0; i < job_count; i++)
		{
			if (part_bytes > 0 && sizes[i] > part_bytes)
			{
				for (size_t offset = 0; offset < sizes[i]; offset += part_bytes)
				{
					SDL_snprintf(parts[part_count].name, sizeof(parts[part_count].name), "%s.%u", names[i], (unsigned)(offset / part_bytes));
					parts[part_count].destination = destinations[i] + offset;
					parts[part_count].size = SDL_min(part_bytes, sizes[i] - offset);
					parts[part_count].layer = i;
					part_count++;
				}
			}
			else
			{
				SDL_strlcpy(parts[part_count].name, names[i], sizeof(parts[part_count].name));
				parts[part_count].destination = destinations[i];
				parts[part_count].size = sizes[i];
				parts[part_count].layer = i;
				part_count++;
			}
		}
#pragma omp parallel for schedule(dynamic, 1)
		for (int i = 0; i < part_count; i++)
		{
			parts[i].done = extract_entry_to(reader, parts[i].name, parts[i].destination, parts[i].size);
		}
		for (int i = 0; i < job_count; i++)
		{
			extracted[i] = true;
		}
		for (int i = 0; i < part_count; i++)
		{
			extracted[parts[i].layer] = extracted[parts[i].layer] && parts[i].done;
		}
		SDL_free(parts);
	}
	extract_milliseconds = stage_milliseconds(&stage_mark);
	for (int i = 4; i < job_count; i++)
	{
		if (!extracted[i])
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: %s is missing or damaged", names[i]);
			kept_count = 0;
			complete = false;
		}
	}
	if (!extracted[0] || !extracted[1] || !extracted[2] || !extracted[3])
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: its heights or its material map are missing or damaged");
		complete = false;
		goto done;
	}

	// The ground
	decode_layer16(data[0], heights16, size, reader->height_band_rows);
#pragma omp parallel for schedule(static)
	for (int64_t i = 0; i < (int64_t)cell_count; i++)
	{
		terrain->heights[i] = base + (int32_t)heights16[i] * step;
	}
	terrain->baked_height_base_units = base;
	terrain->baked_height_step_units = step;

	// The material map, and the soils by what it shows
	{
		const uint8_t *planes = data[1];

#pragma omp parallel for schedule(static)
		for (int64_t i = 0; i < (int64_t)cell_count; i++)
		{
			uint8_t *cell = &terrain->materials[(size_t)i * TERRAIN_MATERIAL_BYTES];
			uint8_t first = planes[i];
			uint8_t second = planes[cell_count + (size_t)i];
			int base_index = first & 15;
			int cover_index = first >> 4;

			cell[MATERIAL_MAP_BASE] = reader->palette[base_index < reader->palette_count ? base_index : 0];
			cell[MATERIAL_MAP_COVER] = cover_index < reader->palette_count ? reader->palette[cover_index] : MATERIAL_NONE;
			cell[MATERIAL_MAP_OVERLAY] = (second & 0x80) ? reader->overlay : MATERIAL_NONE;
			// (the low 7 bits held the shelter once: the sky's occlusion has it now)
			cell[MATERIAL_MAP_GROUND] = 0;
			terrain->soil_types[i] = soil_of_material(cell[MATERIAL_MAP_BASE]);
		}
	}
	terrain->ground_footprints_stale = true;

	// The water, and what it left on the ground: each group's tiles, the
	// planes it has
	for (int g = 0; g < group_count; g++)
	{
		const tile_group_t *group = &groups[g];
		size_t plane_bytes = (size_t)group->count * BAKED_TILE_CELLS;
		bool has_water = group->first_plane == BAKED_PLANE_DEPTH;
		bool has_surface = group->first_plane + group->plane_count > BAKED_PLANE_SURFACE;
		// (a plane's start in the group's data, by its number)
		int water_plane = BAKED_PLANE_DEPTH - group->first_plane;
		int flow_plane = BAKED_PLANE_FLOW - group->first_plane;
		int surface_plane = BAKED_PLANE_SURFACE - group->first_plane;

#pragma omp parallel for schedule(dynamic, 4)
		for (int32_t slot = 0; slot < group->count; slot++)
		{
			int32_t tile = (int32_t)group->words[2 + slot];
			int32_t start_x;
			int32_t start_y;
			const uint8_t *planes = group->planes + (size_t)slot * BAKED_TILE_CELLS;
			bool wet = false;
			bool shows = false;

			if (tile < 0 || tile >= tile_count)
			{
				continue;
			}
			start_x = (tile % terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE;
			start_y = (tile / terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE;
			for (int32_t cell = 0; cell < BAKED_TILE_CELLS; cell++)
			{
				size_t index = (size_t)(start_y + cell / TERRAIN_CHUNK_SIZE) * (size_t)size + (size_t)(start_x + cell % TERRAIN_CHUNK_SIZE);

				if (has_water)
				{
					int32_t depth_steps = planes[(size_t)water_plane * plane_bytes + (size_t)cell]
						| planes[(size_t)(water_plane + 1) * plane_bytes + (size_t)cell] << 8;

					water->depth[index] = depth_steps * step;
					wet = wet || depth_steps > 0;
				}
				for (int k = 0; k < TERRAIN_BAKED_SURFACE_BYTES && has_surface; k++)
				{
					shows = shows || planes[(size_t)(surface_plane + k) * plane_bytes + (size_t)cell] != 0;
				}
			}
			if (wet)
			{
				uint8_t *flow = SDL_malloc(BAKED_TILE_CELLS * WATER_BAKED_FLOW_BYTES);

				if (flow != NULL)
				{
					for (int32_t cell = 0; cell < BAKED_TILE_CELLS; cell++)
					{
						for (int k = 0; k < WATER_BAKED_FLOW_BYTES; k++)
						{
							flow[cell * WATER_BAKED_FLOW_BYTES + k] = planes[(size_t)(flow_plane + k) * plane_bytes + (size_t)cell];
						}
					}
				}
				water->baked_flow[tile] = flow;
			}
			if (shows)
			{
				uint8_t *surface = SDL_malloc(BAKED_TILE_CELLS * TERRAIN_BAKED_SURFACE_BYTES);

				if (surface != NULL)
				{
					for (int32_t cell = 0; cell < BAKED_TILE_CELLS; cell++)
					{
						for (int k = 0; k < TERRAIN_BAKED_SURFACE_BYTES; k++)
						{
							surface[cell * TERRAIN_BAKED_SURFACE_BYTES + k] = planes[(size_t)(surface_plane + k) * plane_bytes + (size_t)cell];
						}
					}
				}
				terrain->baked_surface[tile] = surface;
			}
		}
	}
	unpack_milliseconds = stage_milliseconds(&stage_mark);

	terrain->seed = reader->info.seed;
	terrain->max_height_meters = reader->info.max_height_meters;
	terrain_mark_all_changed(terrain);
	terrain->shadow_dirty = true;
	terrain->materials_dirty = false;
	terrain_mark_materials_changed(terrain, 0, 0, size - 1, size - 1);
	water->raining = false;
	water->spring_count = 0;
	water_restore(water);

	complete = read_sky_and_objects(reader, terrain, objects, sky, &loaded_objects, &dropped_objects) && complete;
	if (dropped_objects > 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: %d objects of types objects.ini no longer has were left out", dropped_objects);
	}
	SDL_Log("Loaded a baked %dx%d map (seed %u): of %d tiles %d with water, %d with what it left on the ground, %d objects, %llu ms "
		"(unpacked %.0f, decoded %.0f, the rest %.0f)", size, size, terrain->seed, tile_count, groups[0].count,
		group_count > 1 ? groups[1].count : groups[0].count, loaded_objects,
		(unsigned long long)((SDL_GetPerformanceCounter() - start_counter) * 1000 / SDL_GetPerformanceFrequency()), extract_milliseconds,
		unpack_milliseconds, stage_milliseconds(&stage_mark));

done:
	for (int i = 0; i < 2; i++)
	{
		SDL_free(data[i]);
	}
	for (int g = 0; g < group_count; g++)
	{
		mz_free(groups[g].words);
		SDL_free(groups[g].planes);
	}
	SDL_free(heights16);
	return complete;
}

bool map_file_read(map_file_reader_t *reader, terrain_t *terrain, water_t *water, objects_t *objects, cloud_paint_t *sky,
	map_features_t *features_out)
{
	uint64_t start_counter = SDL_GetPerformanceCounter();
	size_t cell_count = (size_t)terrain->size * (size_t)terrain->size;
	extract_job_t jobs[] = {
		{ "heights.bin", terrain->heights, cell_count * sizeof(int32_t), true, false, false },
		{ "soil_thickness.bin", terrain->bedrock_heights, cell_count * sizeof(int32_t), true, false, false },
		{ "silt.bin", terrain->silt_thickness, cell_count * sizeof(int32_t), true, false, false },
		{ "soils.bin", terrain->soil_types, cell_count, false, false, false },
		{ "water_depth.bin", water->depth, cell_count * sizeof(int32_t), false, false, false },
		{ "sediment.bin", water->sediment, cell_count * sizeof(int32_t), false, false, false },
		{ "moisture.bin", water->moisture, cell_count * sizeof(uint16_t), false, false, false },
		{ "materials.bin", terrain->materials, cell_count * TERRAIN_MATERIAL_BYTES, false, false, true },
	};
	int job_count = (int)(sizeof(jobs) / sizeof(jobs[0]));
	bool complete = true;
	int dropped_objects = 0;
	int loaded_objects = 0;

	uint64_t stage_mark = start_counter;
	double extract_milliseconds;
	double water_milliseconds;

	if (terrain->size != reader->info.size || water->size != reader->info.size || terrain->baked != reader->info.baked
		|| water->baked != reader->info.baked)
	{
		return false;
	}
	if (features_out != NULL)
	{
		read_features(reader, features_out);
	}
	if (reader->info.baked)
	{
		return read_baked(reader, terrain, water, objects, sky);
	}

	// Each thread reads the archive through its own reader
#pragma omp parallel for schedule(dynamic, 1)
	for (int i = 0; i < job_count; i++)
	{
		mz_zip_archive zip;
		int index;
		mz_zip_archive_file_stat stat;

		SDL_zero(zip);
		if (!mz_zip_reader_init_mem(&zip, reader->archive, reader->archive_size, 0))
		{
			continue;
		}
		index = mz_zip_reader_locate_file(&zip, jobs[i].name, NULL, 0);
		if (index >= 0 && mz_zip_reader_file_stat(&zip, (mz_uint)index, &stat) && stat.m_uncomp_size == jobs[i].size)
		{
			if (jobs[i].predicted)
			{
				uint8_t *planes = SDL_malloc(jobs[i].size);

				if (planes != NULL && mz_zip_reader_extract_to_mem(&zip, (mz_uint)index, planes, jobs[i].size, 0))
				{
					decode_layer(planes, jobs[i].destination, terrain->size);
					jobs[i].done = true;
				}
				SDL_free(planes);
			}
			else if (mz_zip_reader_extract_to_mem(&zip, (mz_uint)index, jobs[i].destination, jobs[i].size, 0))
			{
				jobs[i].done = true;
			}
		}
		mz_zip_reader_end(&zip);
	}
	extract_milliseconds = stage_milliseconds(&stage_mark);
	for (int i = 0; i < job_count; i++)
	{
		if (!jobs[i].done && !jobs[i].optional)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: %s is missing or damaged", jobs[i].name);
			complete = false;
		}
	}

	// The bedrock was stored as the soil's thickness over it
	for (size_t i = 0; i < cell_count; i++)
	{
		terrain->bedrock_heights[i] = (int32_t)((uint32_t)terrain->heights[i] - (uint32_t)terrain->bedrock_heights[i]);
	}
	terrain->seed = reader->info.seed;
	terrain->max_height_meters = reader->info.max_height_meters;
	for (size_t i = 0; i < cell_count; i++)
	{
		terrain->soil_types[i] = (uint8_t)SDL_min(terrain->soil_types[i], SOIL_TYPE_COUNT - 1);
	}
	terrain_mark_all_changed(terrain);
	terrain->shadow_dirty = true;
	// The material map as it was saved (its cover was put there, it would
	// not come back), else baked again (and nothing over it)
	if (jobs[job_count - 1].done)
	{
		terrain->materials_dirty = false;
		terrain_mark_materials_changed(terrain, 0, 0, terrain->size - 1, terrain->size - 1);
	}
	else
	{
		for (size_t i = 0; i < cell_count; i++)
		{
			terrain->materials[i * TERRAIN_MATERIAL_BYTES + 1] = 255;
			terrain->materials[i * TERRAIN_MATERIAL_BYTES + 2] = 255;
			terrain->materials[i * TERRAIN_MATERIAL_BYTES + 3] = 0;
		}
	}

	water->raining = reader->info.raining;
	water->drain_edges = reader->info.drain_edges;
	water->depth_scaled_pipes = reader->info.depth_scaled_pipes;
	water->spring_count = 0;
	for (int i = 0; i < reader->spring_count; i++)
	{
		if (reader->springs[i].cell_x >= 0 && reader->springs[i].cell_y >= 0 && reader->springs[i].cell_x < water->size
			&& reader->springs[i].cell_y < water->size)
		{
			water->springs[water->spring_count++] = reader->springs[i];
		}
	}
	stage_milliseconds(&stage_mark);
	water_restore(water);
	water_milliseconds = stage_milliseconds(&stage_mark);

	complete = read_sky_and_objects(reader, terrain, objects, sky, &loaded_objects, &dropped_objects) && complete;
	if (dropped_objects > 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: %d objects of types objects.ini no longer has were left out", dropped_objects);
	}
	SDL_Log("Loaded a %dx%d map (seed %u): %d objects, %d springs, %llu ms (unpacked %.0f, the water %.0f, the rest %.0f)", terrain->size,
		terrain->size, terrain->seed, loaded_objects, water->spring_count,
		(unsigned long long)((SDL_GetPerformanceCounter() - start_counter) * 1000 / SDL_GetPerformanceFrequency()), extract_milliseconds,
		water_milliseconds, stage_milliseconds(&stage_mark));
	return complete;
}

void map_file_close(map_file_reader_t *reader)
{
	if (reader == NULL)
	{
		return;
	}
	mz_zip_reader_end(&reader->zip);
	SDL_free(reader->archive);
	SDL_free(reader);
}
