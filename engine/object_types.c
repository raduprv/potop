// Object types from data/objects.ini. See object_types.h.

#include "object_types.h"
#include "ini_parser.h"
#include "terrain.h"
#include "materials.h"
#include "file_io.h"

#include <SDL3/SDL.h>

typedef struct
{
	object_types_t *types;
	char current_section[OBJECT_TYPE_NAME_LENGTH];
	object_type_t *current;        // the type being read, NULL in [defaults]
	bool in_defaults;
} load_state_t;

static void set_compiled_defaults(object_types_t *types)
{
	object_type_t *defaults = &types->defaults;

	SDL_zerop(defaults);
	defaults->model = -1;
	defaults->scale_minimum = 1.0f;
	defaults->scale_maximum = 1.0f;
	defaults->random_yaw = true;
	defaults->draw_distance_meters = 400.0f;
	defaults->lod_distance_scale = 1.0f;
	light_source_reset(&defaults->light);
	// Lamps: lit at dusk, put out at dawn (light_switched = false: burns on)
	defaults->light.switched = true;
	defaults->shadow = OBJECT_SHADOW_ALL;
	defaults->impostor = true;
	defaults->grass_clear_meters = OBJECT_GRASS_CLEAR_AUTO;
	defaults->cover_material = 255;
	defaults->physics = OBJECT_PHYSICS_NONE;
	defaults->density_kilograms_per_cubic_meter = 1000.0f;
	defaults->flood_speed_meters_per_second = 1.0f;
	defaults->flood_depth_meters = 0.05f;
	defaults->flood_recover_seconds = 120.0f;
	defaults->damaged_model = -1;
	defaults->destroyed_model = -1;
	defaults->age = 0.5f;
	defaults->age_random = 0.25f;
	defaults->dirt_meters = 0.5f;
	defaults->debris_count = 4;
	defaults->harvest_count_minimum = 1;
	defaults->harvest_count_maximum = 1;
	defaults->harvest_times = 1;
	defaults->harvest_change_type = -1;
	defaults->module = -1;
	defaults->module_type = -1;
	defaults->module_part = -1;
	// Negative: the brush plants as densely as the map is populated
	defaults->brush_per_hectare = -1.0f;
	defaults->spawn_soils = (uint8_t)((1u << SOIL_TYPE_COUNT) - 1);
	defaults->spawn_slope_maximum_degrees = 90.0f;
	defaults->spawn_height_minimum = 0.0f;
	defaults->spawn_height_maximum = 1.0f;
	defaults->spawn_spacing_meters = 2.0f;
	defaults->spawn_patch_meters = 50.0f;
	defaults->spawn_valley_minimum = -1.0f;
	defaults->spawn_water_minimum_meters = 0.0f;
	defaults->spawn_water_maximum_meters = 0.02f;
	defaults->spawn_near_meters = 10.0f;
	defaults->spawn_near_count = 1.0f;
	SDL_strlcpy(types->default_category, "misc", sizeof(types->default_category));
}

int object_types_find(const object_types_t *types, const char *name)
{
	for (int i = 0; i < types->type_count; i++)
	{
		if (SDL_strcasecmp(types->types[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

int object_types_find_category(const object_types_t *types, const char *name)
{
	for (int i = 0; i < types->category_count; i++)
	{
		if (SDL_strcasecmp(types->categories[i], name) == 0)
		{
			return i;
		}
	}
	return -1;
}

// Index of the category, added if new; 0 if the table is full
static int category_index(object_types_t *types, const char *name)
{
	int index = object_types_find_category(types, name);

	if (index >= 0)
	{
		return index;
	}
	if (types->category_count >= OBJECT_CATEGORIES_MAX)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: too many categories, '%s' goes into '%s'", name, types->categories[0]);
		return 0;
	}
	SDL_strlcpy(types->categories[types->category_count], name, OBJECT_CATEGORY_NAME_LENGTH);
	return types->category_count++;
}

bool object_types_category_plantable(const object_types_t *types, int category)
{
	for (int i = 0; i < types->type_count; i++)
	{
		if (types->types[i].category == category && types->types[i].brush_per_hectare > 0.0f && types->types[i].model >= 0)
		{
			return true;
		}
	}
	return false;
}

static bool parse_soils(const char *value, uint8_t *mask_out)
{
	char buffer[128];
	char *items[8];
	int count;
	uint8_t mask = 0;

	SDL_strlcpy(buffer, value, sizeof(buffer));
	count = ini_split_list(buffer, items, 8);
	for (int i = 0; i < count; i++)
	{
		if (SDL_strcasecmp(items[i], "all") == 0)
		{
			mask = (uint8_t)((1u << SOIL_TYPE_COUNT) - 1);
		}
		else if (SDL_strcasecmp(items[i], "sand") == 0)
		{
			mask |= 1u << SOIL_SAND;
		}
		else if (SDL_strcasecmp(items[i], "earth") == 0)
		{
			mask |= 1u << SOIL_EARTH;
		}
		else if (SDL_strcasecmp(items[i], "clay") == 0)
		{
			mask |= 1u << SOIL_CLAY;
		}
		else if (SDL_strcasecmp(items[i], "rock") == 0)
		{
			mask |= 1u << SOIL_ROCK;
		}
		else
		{
			return false;
		}
	}
	*mask_out = mask;
	return true;
}

static bool parse_float(const char *value, float *out)
{
	return ini_value_to_float(value, out);
}

// Applies one key to a type; false if the key or its value is not valid
// One of a list of names (case does not matter); its index into *index_out
static bool parse_choice(const char *value, const char *const *names, int name_count, int *index_out)
{
	for (int i = 0; i < name_count; i++)
	{
		if (SDL_strcasecmp(value, names[i]) == 0)
		{
			*index_out = i;
			return true;
		}
	}
	return false;
}

static bool apply_key(object_types_t *types, object_type_t *type, bool in_defaults, const char *key, const char *value)
{
	{
		bool ok;

		if (light_source_parse(&type->light, key, value, &ok))
		{
			return ok;
		}
	}
	if (SDL_strcmp(key, "model") == 0)
	{
		SDL_snprintf(type->model_path, sizeof(type->model_path), "objects/%s.e3d", value);
		return true;
	}
	if (SDL_strcmp(key, "category") == 0)
	{
		if (in_defaults)
		{
			SDL_strlcpy(types->default_category, value, sizeof(types->default_category));
		}
		else
		{
			type->category = category_index(types, value);
		}
		return true;
	}
	if (SDL_strcmp(key, "scale") == 0)
	{
		char buffer[64];
		char *items[2];
		int count;

		SDL_strlcpy(buffer, value, sizeof(buffer));
		count = ini_split_list(buffer, items, 2);
		if (count < 1 || !parse_float(items[0], &type->scale_minimum))
		{
			return false;
		}
		type->scale_maximum = type->scale_minimum;
		return count < 2 || parse_float(items[1], &type->scale_maximum);
	}
	if (SDL_strcmp(key, "random_yaw") == 0)
	{
		return ini_value_to_bool(value, &type->random_yaw);
	}
	if (SDL_strcmp(key, "yaw_spread") == 0)
	{
		return parse_float(value, &type->yaw_spread_degrees);
	}
	if (SDL_strcmp(key, "front_degrees") == 0)
	{
		return parse_float(value, &type->front_degrees);
	}
	if (SDL_strcmp(key, "lod_distance_scale") == 0)
	{
		return parse_float(value, &type->lod_distance_scale);
	}
	if (SDL_strcmp(key, "draw_distance_meters") == 0)
	{
		return parse_float(value, &type->draw_distance_meters);
	}
	if (SDL_strcmp(key, "shadow") == 0)
	{
		if (SDL_strcasecmp(value, "none") == 0)
		{
			type->shadow = OBJECT_SHADOW_NONE;
		}
		else if (SDL_strcasecmp(value, "near") == 0)
		{
			type->shadow = OBJECT_SHADOW_NEAR;
		}
		else if (SDL_strcasecmp(value, "all") == 0)
		{
			type->shadow = OBJECT_SHADOW_ALL;
		}
		else
		{
			return false;
		}
		return true;
	}
	if (SDL_strcmp(key, "footprint_meters") == 0)
	{
		return parse_float(value, &type->footprint_meters);
	}
	if (SDL_strcmp(key, "grass_clear_meters") == 0)
	{
		if (SDL_strcasecmp(value, "auto") == 0)
		{
			type->grass_clear_meters = OBJECT_GRASS_CLEAR_AUTO;
			return true;
		}
		if (SDL_strcasecmp(value, "none") == 0)
		{
			type->grass_clear_meters = OBJECT_GRASS_CLEAR_NONE;
			return true;
		}
		if (!parse_float(value, &type->grass_clear_meters))
		{
			return false;
		}
		type->grass_clear_meters = SDL_max(type->grass_clear_meters, 0.0f);
		return true;
	}
	if (SDL_strcmp(key, "age") == 0)
	{
		return parse_float(value, &type->age);
	}
	if (SDL_strcmp(key, "age_random") == 0)
	{
		return parse_float(value, &type->age_random);
	}
	if (SDL_strcmp(key, "dirt_meters") == 0)
	{
		return parse_float(value, &type->dirt_meters);
	}
	if (SDL_strcmp(key, "cover") == 0)
	{
		// (materials.h's order)
		static const char *const names[] = { "needles", "leaves", "trampled", "petals" };

		if (SDL_strcasecmp(value, "none") == 0)
		{
			type->cover_material = 255;
			return true;
		}
		for (int i = 0; i < 4; i++)
		{
			if (SDL_strcasecmp(value, names[i]) == 0)
			{
				type->cover_material = (uint8_t)(MATERIAL_NEEDLES + i);
				return true;
			}
		}
		return false;
	}
	if (SDL_strcmp(key, "months") == 0)
	{
		float values[2];

		if (SDL_strcasecmp(value, "all") == 0 || SDL_strcasecmp(value, "none") == 0)
		{
			type->months_first = 0.0f;
			type->months_last = 0.0f;
			return true;
		}
		if (SDL_sscanf(value, "%f , %f", &values[0], &values[1]) != 2 || values[0] < 1.0f || values[0] > 12.0f || values[1] < 1.0f
			|| values[1] > 12.0f)
		{
			return false;
		}
		type->months_first = SDL_roundf(values[0]);
		type->months_last = SDL_roundf(values[1]);
		return true;
	}
	if (SDL_strcmp(key, "cover_meters") == 0)
	{
		return parse_float(value, &type->cover_meters);
	}
	if (SDL_strcmp(key, "shelter") == 0)
	{
		return parse_float(value, &type->shelter);
	}
	if (SDL_strcmp(key, "shelter_meters") == 0)
	{
		return parse_float(value, &type->shelter_meters);
	}
	if (SDL_strcmp(key, "sway") == 0)
	{
		type->sway_own = !in_defaults;
		return parse_float(value, &type->sway_meters);
	}
	if (SDL_strcmp(key, "flutter") == 0)
	{
		type->flutter_own = !in_defaults;
		return parse_float(value, &type->flutter_meters);
	}
	if (SDL_strcmp(key, "stiffness") == 0)
	{
		type->stiffness_own = !in_defaults;
		return parse_float(value, &type->stiffness);
	}
	if (SDL_strcmp(key, "impostor") == 0)
	{
		return ini_value_to_bool(value, &type->impostor);
	}
	if (SDL_strcmp(key, "module") == 0)
	{
		SDL_snprintf(type->module_path, sizeof(type->module_path), "modules/%s.pmod", value);
		return true;
	}
	if (SDL_strcmp(key, "spin") == 0)
	{
		char buffer[128];
		char *items[4];
		float length;

		SDL_strlcpy(buffer, value, sizeof(buffer));
		if (ini_split_list(buffer, items, 4) != 4 || !parse_float(items[0], &type->spin_axis[0]) || !parse_float(items[1], &type->spin_axis[1])
			|| !parse_float(items[2], &type->spin_axis[2]) || !parse_float(items[3], &type->spin_rpm))
		{
			return false;
		}
		length = SDL_sqrtf(type->spin_axis[0] * type->spin_axis[0] + type->spin_axis[1] * type->spin_axis[1] + type->spin_axis[2] * type->spin_axis[2]);
		if (length < 1e-6f)
		{
			return false;
		}
		for (int k = 0; k < 3; k++)
		{
			type->spin_axis[k] /= length;
		}
		type->spins = type->spin_rpm != 0.0f;
		return true;
	}
	if (SDL_strcmp(key, "spin_pivot_meters") == 0)
	{
		char buffer[128];
		char *items[3];

		SDL_strlcpy(buffer, value, sizeof(buffer));
		return ini_split_list(buffer, items, 3) == 3 && parse_float(items[0], &type->spin_pivot[0]) && parse_float(items[1], &type->spin_pivot[1])
			&& parse_float(items[2], &type->spin_pivot[2]);
	}
	if (SDL_strcmp(key, "floats") == 0)
	{
		return ini_value_to_bool(value, &type->floats);
	}
	if (SDL_strcmp(key, "physics") == 0)
	{
		static const char *names[] = { "none", "flood", "static", "body" };

		return parse_choice(value, names, 4, (int *)&type->physics);
	}
	if (SDL_strcmp(key, "flood") == 0)
	{
		static const char *names[] = { "stay", "flatten", "destroy" };

		return parse_choice(value, names, 3, (int *)&type->flood);
	}
	if (SDL_strcmp(key, "shape") == 0)
	{
		static const char *names[] = { "auto", "sphere", "capsule", "hull", "mesh" };

		return parse_choice(value, names, 5, (int *)&type->shape);
	}
	if (SDL_strcmp(key, "navigation") == 0)
	{
		static const char *names[] = { "auto", "wall", "round", "soft" };

		return parse_choice(value, names, 4, (int *)&type->navigation);
	}
	if (SDL_strcmp(key, "blocks_water") == 0)
	{
		if (SDL_strcasecmp(value, "default") == 0)
		{
			type->blocks_water_given = false;
			return true;
		}
		type->blocks_water_given = true;
		return ini_value_to_bool(value, &type->blocks_water);
	}
	if (SDL_strcmp(key, "flood_speed_meters_per_second") == 0)
	{
		return parse_float(value, &type->flood_speed_meters_per_second);
	}
	if (SDL_strcmp(key, "flood_depth_meters") == 0)
	{
		return parse_float(value, &type->flood_depth_meters);
	}
	if (SDL_strcmp(key, "flood_recover_seconds") == 0)
	{
		return parse_float(value, &type->flood_recover_seconds);
	}
	if (SDL_strcmp(key, "damage_newtons") == 0)
	{
		return parse_float(value, &type->damage_newtons);
	}
	if (SDL_strcmp(key, "damaged_model") == 0 || SDL_strcmp(key, "destroyed_model") == 0)
	{
		char *path = key[1] == 'a' ? type->damaged_model_path : type->destroyed_model_path;

		if (value[0] == '\0')
		{
			path[0] = '\0';
		}
		else
		{
			SDL_snprintf(path, OBJECT_TYPE_PATH_LENGTH, "objects/%s.e3d", value);
		}
		return true;
	}
	if (SDL_strcmp(key, "debris") == 0)
	{
		char list[512];
		char *items[OBJECT_DEBRIS_MAX];

		SDL_strlcpy(list, value, sizeof(list));
		type->debris_type_count = ini_split_list(list, items, OBJECT_DEBRIS_MAX);
		for (int i = 0; i < type->debris_type_count; i++)
		{
			SDL_strlcpy(type->debris_names[i], items[i], sizeof(type->debris_names[i]));
		}
		return true;
	}
	if (SDL_strcmp(key, "harvest") == 0)
	{
		SDL_strlcpy(type->harvest_item, value, sizeof(type->harvest_item));
		return true;
	}
	if (SDL_strcmp(key, "harvest_count") == 0)
	{
		char buffer[64];
		char *items[2];
		int count;

		SDL_strlcpy(buffer, value, sizeof(buffer));
		count = ini_split_list(buffer, items, 2);
		if (count < 1 || !ini_value_to_int(items[0], &type->harvest_count_minimum))
		{
			return false;
		}
		type->harvest_count_maximum = type->harvest_count_minimum;
		return count < 2 || ini_value_to_int(items[1], &type->harvest_count_maximum);
	}
	if (SDL_strcmp(key, "harvest_removes") == 0)
	{
		return ini_value_to_bool(value, &type->harvest_removes);
	}
	if (SDL_strcmp(key, "harvest_times") == 0)
	{
		return ini_value_to_int(value, &type->harvest_times);
	}
	if (SDL_strcmp(key, "harvest_regrow_hours") == 0)
	{
		return parse_float(value, &type->harvest_regrow_hours);
	}
	if (SDL_strcmp(key, "harvest_change_object") == 0)
	{
		SDL_strlcpy(type->harvest_change_object, value, sizeof(type->harvest_change_object));
		return true;
	}
	if (SDL_strcmp(key, "debris_count") == 0)
	{
		return ini_value_to_int(value, &type->debris_count);
	}
	if (SDL_strcmp(key, "mass_kilograms") == 0)
	{
		return parse_float(value, &type->mass_kilograms);
	}
	if (SDL_strcmp(key, "density_kilograms_per_cubic_meter") == 0)
	{
		return parse_float(value, &type->density_kilograms_per_cubic_meter);
	}
	if (SDL_strcmp(key, "anchor_newtons") == 0)
	{
		return parse_float(value, &type->anchor_newtons);
	}
	if (SDL_strcmp(key, "falls_when_uprooted") == 0)
	{
		return ini_value_to_bool(value, &type->falls_when_uprooted);
	}
	if (SDL_strcmp(key, "spawn_per_hectare") == 0)
	{
		return parse_float(value, &type->spawn_per_hectare);
	}
	if (SDL_strcmp(key, "brush_per_hectare") == 0)
	{
		return parse_float(value, &type->brush_per_hectare);
	}
	if (SDL_strcmp(key, "spawn_soils") == 0)
	{
		return parse_soils(value, &type->spawn_soils);
	}
	if (SDL_strcmp(key, "spawn_slope_maximum_degrees") == 0)
	{
		return parse_float(value, &type->spawn_slope_maximum_degrees);
	}
	if (SDL_strcmp(key, "spawn_height_minimum") == 0)
	{
		return parse_float(value, &type->spawn_height_minimum);
	}
	if (SDL_strcmp(key, "spawn_height_maximum") == 0)
	{
		return parse_float(value, &type->spawn_height_maximum);
	}
	if (SDL_strcmp(key, "spawn_spacing_meters") == 0)
	{
		return parse_float(value, &type->spawn_spacing_meters);
	}
	if (SDL_strcmp(key, "spawn_patch") == 0)
	{
		// A leading '!' grows outside the patches
		type->spawn_patch_inverted = value[0] == '!';
		SDL_strlcpy(type->spawn_patch, value + (type->spawn_patch_inverted ? 1 : 0), sizeof(type->spawn_patch));
		return true;
	}
	if (SDL_strcmp(key, "spawn_clustering") == 0)
	{
		return parse_float(value, &type->spawn_clustering);
	}
	if (SDL_strcmp(key, "spawn_patch_meters") == 0)
	{
		return parse_float(value, &type->spawn_patch_meters);
	}
	if (SDL_strcmp(key, "spawn_valley_preference") == 0)
	{
		return parse_float(value, &type->spawn_valley_preference);
	}
	if (SDL_strcmp(key, "spawn_valley_minimum") == 0)
	{
		return parse_float(value, &type->spawn_valley_minimum);
	}
	if (SDL_strcmp(key, "spawn_water_minimum_meters") == 0)
	{
		return parse_float(value, &type->spawn_water_minimum_meters);
	}
	if (SDL_strcmp(key, "spawn_water_maximum_meters") == 0)
	{
		return parse_float(value, &type->spawn_water_maximum_meters);
	}
	if (SDL_strcmp(key, "spawn_water_near_meters") == 0)
	{
		return parse_float(value, &type->spawn_water_near_meters);
	}
	if (SDL_strcmp(key, "spawn_water_far_meters") == 0)
	{
		return parse_float(value, &type->spawn_water_far_meters);
	}
	if (SDL_strcmp(key, "spawn_near") == 0)
	{
		// A leading '!' grows where they are not
		type->spawn_near_inverted = value[0] == '!';
		SDL_strlcpy(type->spawn_near, value + (type->spawn_near_inverted ? 1 : 0), sizeof(type->spawn_near));
		return true;
	}
	if (SDL_strcmp(key, "spawn_near_meters") == 0)
	{
		return parse_float(value, &type->spawn_near_meters);
	}
	if (SDL_strcmp(key, "spawn_near_count") == 0)
	{
		return parse_float(value, &type->spawn_near_count);
	}
	return false;
}

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;
	object_types_t *types = state->types;

	if (SDL_strcmp(section, state->current_section) != 0)
	{
		SDL_strlcpy(state->current_section, section, sizeof(state->current_section));
		state->in_defaults = SDL_strcasecmp(section, "defaults") == 0;
		state->current = NULL;
		// The wind does not carry over from one group of plants to the
		// next: each [defaults] starts still; nor the season, the cover and
		// the shelter (the rocks after the flowers are there all year)
		if (state->in_defaults)
		{
			types->defaults.sway_meters = 0.0f;
			types->defaults.flutter_meters = 0.0f;
			types->defaults.stiffness = 0.0f;
			types->defaults.months_first = 0.0f;
			types->defaults.months_last = 0.0f;
			types->defaults.cover_material = 255;
			types->defaults.cover_meters = 0.0f;
			types->defaults.shelter = 0.0f;
			types->defaults.shelter_meters = 0.0f;
			types->defaults.grass_clear_meters = OBJECT_GRASS_CLEAR_AUTO;
		}
		if (!state->in_defaults)
		{
			if (object_types_find(types, section) >= 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: type '%s' defined twice, the second one is ignored", section);
			}
			else if (types->type_count >= OBJECT_TYPES_MAX)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: too many types, '%s' is ignored", section);
			}
			else
			{
				state->current = &types->types[types->type_count++];
				*state->current = types->defaults;
				SDL_strlcpy(state->current->name, section, sizeof(state->current->name));
				SDL_snprintf(state->current->model_path, sizeof(state->current->model_path), "objects/%s.e3d", section);
				state->current->category = category_index(types, types->default_category);
			}
		}
	}
	if (state->in_defaults)
	{
		return apply_key(types, &types->defaults, true, key, value);
	}
	if (state->current == NULL)
	{
		return true;
	}
	return apply_key(types, state->current, false, key, value);
}

// A type's module read (its .pmod), and a type made for each part after
// the body: the module's type as it is (its draw distance, its age, its
// category), with the part's model, flags and name
static void resolve_module(object_types_t *types, int type_index)
{
	object_type_t *type = &types->types[type_index];
	module_file_t *module;
	void *data;
	size_t size;
	char error[96];
	char model_name[MODULE_FILE_MODEL_LENGTH];

	if (types->modules == NULL)
	{
		types->modules = SDL_calloc(OBJECT_MODULES_MAX, sizeof(module_file_t));
		if (types->modules == NULL)
		{
			return;
		}
	}
	if (types->module_count >= OBJECT_MODULES_MAX)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: %s: too many modules, '%s' left out", type->name, type->module_path);
		return;
	}
	data = file_io_load_data_file(type->module_path, &size);
	if (data == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: %s: its module 'data/%s' did not load", type->name, type->module_path);
		return;
	}
	module = &types->modules[types->module_count];
	if (!module_file_parse(data, size, module, error, sizeof(error)))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: %s: data/%s: %s", type->name, type->module_path, error);
		SDL_free(data);
		return;
	}
	SDL_free(data);
	type->module = types->module_count++;
	// (the body: the type's own model, flags and all)
	{
		const module_file_part_t *body = &module->parts[0];

		SDL_strlcpy(model_name, body->model, sizeof(model_name));
		SDL_snprintf(type->model_path, sizeof(type->model_path), "objects/%s.e3d", model_name);
		type->physics = (body->flags & MODULE_PART_COLLISION) != 0 ? OBJECT_PHYSICS_STATIC : OBJECT_PHYSICS_NONE;
		type->shape = type->physics == OBJECT_PHYSICS_STATIC ? OBJECT_SHAPE_MESH : OBJECT_SHAPE_AUTO;
		type->shadow = (body->flags & MODULE_PART_SHADOW) != 0 ? OBJECT_SHADOW_ALL : OBJECT_SHADOW_NONE;
		type->impostor = (body->flags & MODULE_PART_IMPOSTOR) != 0;
		type->spins = false;
		type->random_yaw = false;
		type->blocks_water_given = false;
	}
	for (int p = 1; p < module->part_count; p++)
	{
		const module_file_part_t *part = &module->parts[p];
		object_type_t *made;
		bool moves = (part->flags & MODULE_PART_MOVES) != 0;

		if (types->type_count >= OBJECT_TYPES_MAX)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: too many types, %s's parts left out", type->name);
			return;
		}
		made = &types->types[types->type_count++];
		*made = types->types[type_index];
		SDL_snprintf(made->name, sizeof(made->name), "%s.%s", types->types[type_index].name, part->name);
		SDL_strlcpy(model_name, part->model, sizeof(model_name));
		SDL_snprintf(made->model_path, sizeof(made->model_path), "objects/%s.e3d", model_name);
		made->module = -1;
		made->module_path[0] = '\0';
		made->module_type = type_index;
		made->module_part = p;
		made->physics = (part->flags & MODULE_PART_COLLISION) != 0 && !moves ? OBJECT_PHYSICS_STATIC : OBJECT_PHYSICS_NONE;
		made->shape = made->physics == OBJECT_PHYSICS_STATIC ? OBJECT_SHAPE_MESH : OBJECT_SHAPE_AUTO;
		made->shadow = (part->flags & MODULE_PART_SHADOW) != 0 ? OBJECT_SHADOW_ALL : OBJECT_SHADOW_NONE;
		made->impostor = (part->flags & MODULE_PART_IMPOSTOR) != 0 && !moves;
		made->footprint_meters = 0.0f;
		made->spawn_per_hectare = 0.0f;
		made->brush_per_hectare = 0.0f;
		made->harvest_item[0] = '\0';
		made->damage_newtons = 0.0f;
		made->damaged_model_path[0] = '\0';
		made->destroyed_model_path[0] = '\0';
		made->debris_type_count = 0;
		made->blocks_water_given = false;
	}
	SDL_Log("Objects: '%s' is a module of %d parts and %d states (data/%s)", type->name, module->part_count, module->state_count, type->module_path);
}

void object_types_free(object_types_t *types)
{
	SDL_free(types->modules);
	types->modules = NULL;
	types->module_count = 0;
}

bool object_types_load(object_types_t *types)
{
	load_state_t state;

	SDL_zerop(types);
	set_compiled_defaults(types);
	SDL_zero(state);
	state.types = types;
	if (!ini_parse_data_file("objects.ini", load_key, &state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "data/objects.ini not found: no objects");
		return false;
	}
	// (the modules' parts after every type objects.ini names)
	{
		int named = types->type_count;

		for (int i = 0; i < named; i++)
		{
			if (types->types[i].module_path[0] != '\0')
			{
				resolve_module(types, i);
			}
		}
	}
	for (int i = 0; i < types->type_count; i++)
	{
		object_type_t *type = &types->types[i];

		type->scale_minimum = SDL_max(type->scale_minimum, 0.01f);
		type->scale_maximum = SDL_max(type->scale_maximum, type->scale_minimum);
		type->spawn_spacing_meters = SDL_max(type->spawn_spacing_meters, 0.1f);
		type->spawn_patch_meters = SDL_max(type->spawn_patch_meters, 1.0f);
		type->spawn_clustering = SDL_clamp(type->spawn_clustering, 0.0f, 1.0f);
		if (!type->blocks_water_given)
		{
			type->blocks_water = type->physics == OBJECT_PHYSICS_STATIC;
		}
		if (type->shape == OBJECT_SHAPE_MESH && type->physics != OBJECT_PHYSICS_STATIC)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: %s: shape = mesh works on static objects only, using a hull", type->name);
			type->shape = OBJECT_SHAPE_HULL;
		}
		type->debris_count = SDL_clamp(type->debris_count, 0, 32);
		for (int i = 0; i < type->debris_type_count; i++)
		{
			type->debris_types[i] = -1;
		}
		if (type->brush_per_hectare < 0.0f)
		{
			// As dense as inside its patches: a planted forest looks like a
			// forest, not like the average over forests and meadows
			type->brush_per_hectare = type->spawn_per_hectare * (1.0f + type->spawn_clustering);
		}
	}
	// Debris names refer to types that may come later in the file
	for (int i = 0; i < types->type_count; i++)
	{
		object_type_t *type = &types->types[i];

		for (int d = 0; d < type->debris_type_count; d++)
		{
			type->debris_types[d] = object_types_find(types, type->debris_names[d]);
			if (type->debris_types[d] < 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: %s: unknown debris type '%s'", type->name, type->debris_names[d]);
			}
		}
		type->harvest_change_type = type->harvest_change_object[0] != '\0' ? object_types_find(types, type->harvest_change_object) : -1;
		if (type->harvest_change_object[0] != '\0' && type->harvest_change_type < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: %s: unknown harvest_change_object '%s'", type->name, type->harvest_change_object);
		}
		type->near_category = type->spawn_near[0] != '\0' ? object_types_find_category(types, type->spawn_near) : -1;
		if (type->spawn_near[0] != '\0' && type->near_category < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "objects.ini: %s: unknown spawn_near category '%s'", type->name, type->spawn_near);
		}
		type->harvest_times = SDL_max(type->harvest_times, 1);
		type->harvest_count_maximum = SDL_max(type->harvest_count_maximum, type->harvest_count_minimum);
	}
	return true;
}
