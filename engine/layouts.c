// Layouts of objects. See layouts.h.

#include "layouts.h"
#include "ini_parser.h"

#include <SDL3/SDL.h>

// Nested layouts go at most this deep (a hamlet of homesteads with gardens)
#define MAXIMUM_DEPTH 4
// Water this deep counts as water for shores and orientation
#define SHORE_DEPTH_METERS 0.15f
// Ground with water deeper than this is no place for a new layout
#define DRY_DEPTH_METERS 0.05f

// --- Loading ----------------------------------------------------------------

typedef struct
{
	layouts_t *layouts;
	const object_types_t *types;
	char current_section[LAYOUT_NAME_LENGTH];
	layout_t *current;
	int part_numbers[LAYOUT_MAX_PARTS]; // the number in the file of each part of the current layout
} load_state_t;

static void set_layout_defaults(layout_t *layout, const char *name)
{
	SDL_zerop(layout);
	SDL_strlcpy(layout->name, name, sizeof(layout->name));
	layout->footprint_meters = 10.0f;
	layout->orient = LAYOUT_ORIENT_RANDOM;
	layout->spawn_soils = (uint8_t)((1u << SOIL_TYPE_COUNT) - 1);
	layout->flatness_meters = 3.0f;
	layout->spawn_height_maximum = 1.0f;
	layout->spacing_meters = 20.0f;
	layout->water_search_meters = 40.0f;
}

static void set_part_defaults(layout_part_t *part, layout_part_kind_t kind)
{
	SDL_zerop(part);
	part->kind = kind;
	part->layout = -1;
	part->chance = 1.0f;
	part->item_chance = 1.0f;
	part->count_minimum = 1;
	part->support_type = -1;
	part->count_maximum = 1;
	part->row_spacing = 1.0f;
	part->plant_spacing = 0.6f;
	part->gate_side = -1;
	part->gate_width = 2.5f;
	part->facing = kind == LAYOUT_PART_RING ? LAYOUT_FACING_CENTER : kind == LAYOUT_PART_SCATTER || kind == LAYOUT_PART_ROWS
		? LAYOUT_FACING_RANDOM : LAYOUT_FACING_FRONT;
}

int layouts_find(const layouts_t *layouts, const char *name)
{
	for (int i = 0; i < layouts->count; i++)
	{
		if (SDL_strcasecmp(layouts->layouts[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

static bool parse_float_pair(const char *value, float *first, float *second)
{
	char buffer[64];
	char *items[2];
	int count;

	SDL_strlcpy(buffer, value, sizeof(buffer));
	count = ini_split_list(buffer, items, 2);
	if (count < 1 || !ini_value_to_float(items[0], first))
	{
		return false;
	}
	*second = *first;
	return count < 2 || ini_value_to_float(items[1], second);
}

static bool parse_soils(const char *value, uint8_t *mask_out)
{
	static const char *names[SOIL_TYPE_COUNT] = { "sand", "earth", "clay", "rock" };
	char buffer[128];
	char *items[8];
	int count;
	uint8_t mask = 0;

	SDL_strlcpy(buffer, value, sizeof(buffer));
	count = ini_split_list(buffer, items, 8);
	for (int i = 0; i < count; i++)
	{
		bool known = false;

		if (SDL_strcasecmp(items[i], "all") == 0)
		{
			mask = (uint8_t)((1u << SOIL_TYPE_COUNT) - 1);
			continue;
		}
		for (int soil = 0; soil < SOIL_TYPE_COUNT; soil++)
		{
			if (SDL_strcasecmp(items[i], names[soil]) == 0)
			{
				mask |= (uint8_t)(1u << soil);
				known = true;
			}
		}
		if (!known)
		{
			return false;
		}
	}
	*mask_out = mask;
	return true;
}

// "name:weight, name, ..." (weight 1 if left out)
static bool parse_choices(const object_types_t *types, const char *value, layout_part_t *part)
{
	char buffer[512];
	char *items[LAYOUT_MAX_CHOICES];
	int count;
	bool ok = true;

	SDL_strlcpy(buffer, value, sizeof(buffer));
	count = ini_split_list(buffer, items, LAYOUT_MAX_CHOICES);
	part->choice_count = 0;
	for (int i = 0; i < count; i++)
	{
		char *colon = SDL_strchr(items[i], ':');
		float weight = 1.0f;
		int type;

		if (colon != NULL)
		{
			*colon = '\0';
			if (!ini_value_to_float(colon + 1, &weight))
			{
				ok = false;
			}
		}
		type = object_types_find(types, items[i]);
		if (type < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "layouts.ini: unknown object type '%s'", items[i]);
			ok = false;
			continue;
		}
		part->choice_types[part->choice_count] = type;
		part->choice_weights[part->choice_count] = SDL_max(weight, 0.0f);
		part->choice_count++;
	}
	return ok;
}

static bool parse_part_kind(const char *value, layout_part_kind_t *kind_out)
{
	static const char *names[] = { "single", "ring", "scatter", "rows", "fence", "stack" };

	for (int i = 0; i < (int)SDL_arraysize(names); i++)
	{
		if (SDL_strcasecmp(value, names[i]) == 0)
		{
			*kind_out = (layout_part_kind_t)i;
			return true;
		}
	}
	return false;
}

static bool apply_part_key(load_state_t *state, layout_part_t *part, const char *field, const char *value)
{
	if (SDL_strcmp(field, "models") == 0)
	{
		return parse_choices(state->types, value, part);
	}
	if (SDL_strcmp(field, "supports") == 0)
	{
		part->support_type = object_types_find(state->types, value);
		if (part->support_type < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "layouts.ini: unknown type '%s'", value);
		}
		return part->support_type >= 0;
	}
	if (SDL_strcmp(field, "layout") == 0)
	{
		SDL_strlcpy(part->layout_name, value, sizeof(part->layout_name));
		return true;
	}
	if (SDL_strcmp(field, "offset_meters") == 0)
	{
		return parse_float_pair(value, &part->offset_x, &part->offset_z);
	}
	if (SDL_strcmp(field, "yaw_degrees") == 0)
	{
		return ini_value_to_float(value, &part->yaw_degrees);
	}
	if (SDL_strcmp(field, "facing") == 0)
	{
		static const char *names[] = { "front", "center", "outward", "along", "random" };

		for (int i = 0; i < (int)SDL_arraysize(names); i++)
		{
			if (SDL_strcasecmp(value, names[i]) == 0)
			{
				part->facing = (layout_facing_t)i;
				return true;
			}
		}
		return false;
	}
	if (SDL_strcmp(field, "chance") == 0)
	{
		return ini_value_to_float(value, &part->chance);
	}
	if (SDL_strcmp(field, "item_chance") == 0)
	{
		return ini_value_to_float(value, &part->item_chance);
	}
	if (SDL_strcmp(field, "count") == 0)
	{
		float minimum;
		float maximum;

		if (!parse_float_pair(value, &minimum, &maximum))
		{
			return false;
		}
		part->count_minimum = (int)minimum;
		part->count_maximum = SDL_max((int)maximum, part->count_minimum);
		return true;
	}
	if (SDL_strcmp(field, "radius_meters") == 0)
	{
		bool ok = parse_float_pair(value, &part->radius_minimum, &part->radius_maximum);

		// One value: exactly on that radius (ring), or a disk (scatter)
		if (ok && SDL_strchr(value, ',') == NULL && part->kind == LAYOUT_PART_SCATTER)
		{
			part->radius_minimum = 0.0f;
		}
		return ok;
	}
	if (SDL_strcmp(field, "size_meters") == 0)
	{
		return parse_float_pair(value, &part->size_x, &part->size_z);
	}
	if (SDL_strcmp(field, "row_spacing_meters") == 0)
	{
		return ini_value_to_float(value, &part->row_spacing);
	}
	if (SDL_strcmp(field, "plant_spacing_meters") == 0)
	{
		return ini_value_to_float(value, &part->plant_spacing);
	}
	if (SDL_strcmp(field, "jitter_meters") == 0)
	{
		return ini_value_to_float(value, &part->jitter_meters);
	}
	if (SDL_strcmp(field, "same_model_per_row") == 0)
	{
		return ini_value_to_bool(value, &part->same_model_per_row);
	}
	if (SDL_strcmp(field, "gate_side") == 0)
	{
		static const char *names[] = { "front", "back", "left", "right" };

		part->gate_side = -1;
		for (int i = 0; i < (int)SDL_arraysize(names); i++)
		{
			if (SDL_strcasecmp(value, names[i]) == 0)
			{
				part->gate_side = i;
			}
		}
		return part->gate_side >= 0 || SDL_strcasecmp(value, "none") == 0;
	}
	if (SDL_strcmp(field, "gate_width_meters") == 0)
	{
		return ini_value_to_float(value, &part->gate_width);
	}
	if (SDL_strcmp(field, "lift_meters") == 0)
	{
		return parse_float_pair(value, &part->lift_minimum, &part->lift_maximum);
	}
	if (SDL_strcmp(field, "scale") == 0)
	{
		return parse_float_pair(value, &part->scale_minimum, &part->scale_maximum);
	}
	if (SDL_strcmp(field, "place") == 0)
	{
		if (SDL_strcasecmp(value, "ground") == 0)
		{
			part->place = LAYOUT_PLACE_GROUND;
		}
		else if (SDL_strcasecmp(value, "shore") == 0)
		{
			part->place = LAYOUT_PLACE_SHORE;
		}
		else if (SDL_strcasecmp(value, "water") == 0)
		{
			part->place = LAYOUT_PLACE_WATER;
		}
		else
		{
			return false;
		}
		return true;
	}
	return false;
}

static bool apply_layout_key(layout_t *layout, const char *key, const char *value)
{
	if (SDL_strcmp(key, "footprint_meters") == 0)
	{
		return ini_value_to_float(value, &layout->footprint_meters);
	}
	if (SDL_strcmp(key, "flatten") == 0)
	{
		return ini_value_to_bool(value, &layout->flatten);
	}
	if (SDL_strcmp(key, "orient") == 0)
	{
		if (SDL_strcasecmp(value, "random") == 0)
		{
			layout->orient = LAYOUT_ORIENT_RANDOM;
		}
		else if (SDL_strcasecmp(value, "downhill") == 0)
		{
			layout->orient = LAYOUT_ORIENT_DOWNHILL;
		}
		else if (SDL_strcasecmp(value, "water") == 0)
		{
			layout->orient = LAYOUT_ORIENT_WATER;
		}
		else
		{
			return false;
		}
		return true;
	}
	if (SDL_strcmp(key, "spawn_per_square_kilometer") == 0)
	{
		return ini_value_to_float(value, &layout->spawn_per_square_kilometer);
	}
	if (SDL_strcmp(key, "spawn_soils") == 0)
	{
		return parse_soils(value, &layout->spawn_soils);
	}
	if (SDL_strcmp(key, "flatness_meters") == 0)
	{
		return ini_value_to_float(value, &layout->flatness_meters);
	}
	if (SDL_strcmp(key, "spawn_height_minimum") == 0)
	{
		return ini_value_to_float(value, &layout->spawn_height_minimum);
	}
	if (SDL_strcmp(key, "spawn_height_maximum") == 0)
	{
		return ini_value_to_float(value, &layout->spawn_height_maximum);
	}
	if (SDL_strcmp(key, "spacing_meters") == 0)
	{
		return ini_value_to_float(value, &layout->spacing_meters);
	}
	if (SDL_strcmp(key, "water_search_meters") == 0)
	{
		return ini_value_to_float(value, &layout->water_search_meters);
	}
	if (SDL_strcmp(key, "needs_water") == 0)
	{
		return ini_value_to_bool(value, &layout->needs_water);
	}
	return false;
}

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;
	layouts_t *layouts = state->layouts;

	if (SDL_strcmp(section, state->current_section) != 0)
	{
		SDL_strlcpy(state->current_section, section, sizeof(state->current_section));
		state->current = NULL;
		if (layouts_find(layouts, section) >= 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "layouts.ini: layout '%s' defined twice, the second one is ignored", section);
		}
		else if (layouts->count >= LAYOUTS_MAX)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "layouts.ini: too many layouts, '%s' is ignored", section);
		}
		else
		{
			state->current = &layouts->layouts[layouts->count++];
			set_layout_defaults(state->current, section);
		}
	}
	if (state->current == NULL)
	{
		return true;
	}
	if (SDL_strncmp(key, "part.", 5) == 0)
	{
		layout_t *layout = state->current;
		char *field = NULL;
		int number = (int)SDL_strtol(key + 5, &field, 10);
		int index = -1;

		for (int p = 0; p < layout->part_count; p++)
		{
			if (state->part_numbers[p] == number)
			{
				index = p;
			}
		}
		if (index < 0)
		{
			if (layout->part_count >= LAYOUT_MAX_PARTS)
			{
				return false;
			}
			index = layout->part_count++;
			state->part_numbers[index] = number;
			set_part_defaults(&layout->parts[index], LAYOUT_PART_SINGLE);
		}
		if (field == NULL || *field == '\0')
		{
			layout_part_kind_t kind;

			if (!parse_part_kind(value, &kind))
			{
				return false;
			}
			set_part_defaults(&layout->parts[index], kind);
			return true;
		}
		if (*field != '.')
		{
			return false;
		}
		return apply_part_key(state, &layout->parts[index], field + 1, value);
	}
	return apply_layout_key(state->current, key, value);
}

bool layouts_load(layouts_t *layouts, const object_types_t *types)
{
	load_state_t state;

	SDL_zerop(layouts);
	SDL_zero(state);
	state.layouts = layouts;
	state.types = types;
	if (!ini_parse_data_file("layouts.ini", load_key, &state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "data/layouts.ini not found: no layouts");
		return false;
	}
	// Nested layouts by name, now that all are known
	for (int l = 0; l < layouts->count; l++)
	{
		for (int p = 0; p < layouts->layouts[l].part_count; p++)
		{
			layout_part_t *part = &layouts->layouts[l].parts[p];

			if (part->layout_name[0] != '\0')
			{
				part->layout = layouts_find(layouts, part->layout_name);
				if (part->layout < 0)
				{
					SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "layouts.ini: layout '%s' uses unknown layout '%s'", layouts->layouts[l].name,
						part->layout_name);
				}
			}
		}
	}
	SDL_Log("Layouts: %d", layouts->count);
	return true;
}

// --- Placing ----------------------------------------------------------------

typedef struct
{
	const layouts_t *layouts;
	objects_t *objects;
	terrain_t *terrain;
	water_t *water;
	random_t *random;
	int group;
	int placed;
	objects_clear_circle_t *clear;
	int clear_count;
	int clear_capacity;
} place_context_t;

// A layout's frame on the map: its center, and where its front points
// (radians, 0 = +Z, as object yaw)
typedef struct
{
	float x;
	float z;
	float facing;
	bool shore_searched;
	bool has_shore;
	float shore_distance;
	float water_search_meters;
} frame_t;

static float water_depth_at(const water_t *water, float x, float z)
{
	int32_t cell_x;
	int32_t cell_y;

	if (water == NULL || water->depth == NULL)
	{
		return 0.0f;
	}
	cell_x = clamp_int((int32_t)(x / water->cell_size_meters + 0.5f), 0, water->size - 1);
	cell_y = clamp_int((int32_t)(z / water->cell_size_meters + 0.5f), 0, water->size - 1);
	return (float)water->depth[cell_y * water->size + cell_x] * TERRAIN_METERS_PER_HEIGHT_UNIT;
}

static void frame_to_world(const frame_t *frame, float local_x, float local_z, float *x_out, float *z_out)
{
	float sine = SDL_sinf(frame->facing);
	float cosine = SDL_cosf(frame->facing);

	// The same turn as an object's yaw
	*x_out = frame->x + local_x * cosine + local_z * sine;
	*z_out = frame->z - local_x * sine + local_z * cosine;
}

// How far in front of the frame the water starts
static bool find_shore(frame_t *frame, const water_t *water)
{
	if (!frame->shore_searched)
	{
		frame->shore_searched = true;
		frame->has_shore = false;
		for (float distance = 0.0f; distance <= frame->water_search_meters; distance += 0.5f)
		{
			float x;
			float z;

			frame_to_world(frame, 0.0f, distance, &x, &z);
			if (water_depth_at(water, x, z) > SHORE_DEPTH_METERS)
			{
				frame->has_shore = true;
				frame->shore_distance = distance;
				break;
			}
		}
	}
	return frame->has_shore;
}

static void add_clear_circle(place_context_t *context, float x, float z, float radius)
{
	if (context->clear_count == context->clear_capacity)
	{
		int capacity = SDL_max(64, context->clear_capacity * 2);
		objects_clear_circle_t *grown = SDL_realloc(context->clear, sizeof(objects_clear_circle_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		context->clear = grown;
		context->clear_capacity = capacity;
	}
	context->clear[context->clear_count].x = x;
	context->clear[context->clear_count].z = z;
	context->clear[context->clear_count].radius = radius;
	context->clear_count++;
}

static int pick_choice(const layout_part_t *part, random_t *random)
{
	float total = 0.0f;
	float pick;

	for (int k = 0; k < part->choice_count; k++)
	{
		total += part->choice_weights[k];
	}
	if (part->choice_count == 0 || total <= 0.0f)
	{
		return -1;
	}
	pick = random_float(random) * total;
	for (int k = 0; k < part->choice_count; k++)
	{
		if (pick < part->choice_weights[k])
		{
			return part->choice_types[k];
		}
		pick -= part->choice_weights[k];
	}
	return part->choice_types[part->choice_count - 1];
}

// Levels the ground in a circle to the average height of its middle, with
// a soft edge
static void flatten_ground(terrain_t *terrain, water_t *water, float center_x, float center_z, float radius)
{
	float cell = terrain->cell_size_meters;
	int32_t bounds[4] = {
		clamp_int((int32_t)((center_x - radius) / cell), 0, terrain->size - 1),
		clamp_int((int32_t)((center_z - radius) / cell), 0, terrain->size - 1),
		clamp_int((int32_t)((center_x + radius) / cell) + 1, 0, terrain->size - 1),
		clamp_int((int32_t)((center_z + radius) / cell) + 1, 0, terrain->size - 1) };
	double sum = 0.0;
	int count = 0;
	int32_t level;

	for (float offset_z = -0.5f * radius; offset_z <= 0.5f * radius; offset_z += 1.0f)
	{
		for (float offset_x = -0.5f * radius; offset_x <= 0.5f * radius; offset_x += 1.0f)
		{
			if (offset_x * offset_x + offset_z * offset_z <= 0.25f * radius * radius)
			{
				sum += terrain_sample_height_meters(terrain, center_x + offset_x, center_z + offset_z);
				count++;
			}
		}
	}
	if (count == 0)
	{
		return;
	}
	level = (int32_t)(sum / (double)count * (double)TERRAIN_HEIGHT_UNITS_PER_METER);
	for (int32_t y = bounds[1]; y <= bounds[3]; y++)
	{
		for (int32_t x = bounds[0]; x <= bounds[2]; x++)
		{
			float offset_x = (float)x * cell - center_x;
			float offset_z = (float)y * cell - center_z;
			float distance = SDL_sqrtf(offset_x * offset_x + offset_z * offset_z);
			float keep;
			int32_t index;

			if (distance >= radius)
			{
				continue;
			}
			// 0 inside two thirds of the radius, rising smoothly to 1 at its edge
			keep = SDL_clamp((distance - 0.65f * radius) / (0.35f * radius), 0.0f, 1.0f);
			keep = keep * keep * (3.0f - 2.0f * keep);
			index = terrain_index(terrain, x, y);
			terrain->heights[index] = (int32_t)((float)level + (float)(terrain->heights[index] - level) * keep);
		}
	}
	terrain_fix_layers(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
	terrain_mark_cells_changed(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
	terrain->shadow_dirty = true;
	if (water != NULL && water->depth != NULL)
	{
		water_wake_cells(water, bounds[0], bounds[1], bounds[2], bounds[3]);
	}
}

static void place_layout(place_context_t *context, int layout_index, float x, float z, float facing, int depth);

// One object of a part, at a spot in the frame, turned by local_yaw
// (radians, in the frame) unless the type's front says otherwise
static void place_object(place_context_t *context, const frame_t *frame, const layout_part_t *part, int type, float local_x, float local_z,
	float local_yaw, bool along_axis, float scale_override)
{
	const object_type_t *definition;
	const object_model_t *model;
	float x;
	float z;
	float yaw;
	float scale;
	float unused_yaw;
	float lift = part->lift_minimum + (part->lift_maximum - part->lift_minimum) * random_float(context->random);

	if (type < 0)
	{
		return;
	}
	definition = &context->objects->types.types[type];
	if (definition->model < 0)
	{
		return;
	}
	model = &context->objects->models[definition->model];
	frame_to_world(frame, local_x, local_z, &x, &z);
	objects_random_placement(context->objects, type, random_float(context->random), random_float(context->random), &unused_yaw, &scale);
	if (part->scale_minimum > 0.0f)
	{
		scale = part->scale_minimum + (part->scale_maximum - part->scale_minimum) * random_float(context->random);
	}
	if (scale_override > 0.0f)
	{
		scale = scale_override;
	}
	// Fence pieces are laid along their long axis, whatever their front
	yaw = frame->facing + local_yaw - (along_axis ? 0.0f : definition->front_degrees * (MATH3D_PI / 180.0f));
	if (objects_add_lifted(context->objects, context->terrain, type, x, z, yaw, scale, context->group, lift))
	{
		context->placed++;
		add_clear_circle(context, x, z, 0.8f * model->horizontal_radius * scale + 0.3f);
	}
}

// One item of a part: an object or a nested layout
static void place_item(place_context_t *context, frame_t *frame, const layout_part_t *part, float local_x, float local_z, float local_yaw,
	int depth)
{
	if (part->layout >= 0)
	{
		float x;
		float z;

		frame_to_world(frame, local_x, local_z, &x, &z);
		place_layout(context, part->layout, x, z, frame->facing + local_yaw, depth + 1);
		return;
	}
	place_object(context, frame, part, pick_choice(part, context->random), local_x, local_z, local_yaw, false, 0.0f);
}

static int random_count(const layout_part_t *part, random_t *random)
{
	int spread = part->count_maximum - part->count_minimum + 1;

	return part->count_minimum + SDL_min((int)(random_float(random) * (float)spread), spread - 1);
}

// Turn of an item at angle 'angle' around its part's center
static float item_yaw(const layout_part_t *part, float angle, random_t *random)
{
	float yaw = part->yaw_degrees * (MATH3D_PI / 180.0f);

	switch (part->facing)
	{
		case LAYOUT_FACING_CENTER:
			return yaw + angle + MATH3D_PI;
		case LAYOUT_FACING_OUTWARD:
			return yaw + angle;
		case LAYOUT_FACING_ALONG:
			return yaw + angle + 0.5f * MATH3D_PI;
		case LAYOUT_FACING_RANDOM:
			return random_float(random) * 2.0f * MATH3D_PI;
		case LAYOUT_FACING_FRONT:
		default:
			return yaw;
	}
}

static void place_fence(place_context_t *context, frame_t *frame, const layout_part_t *part)
{
	int type = pick_choice(part, context->random);
	const object_model_t *model;
	float extent_x;
	float extent_z;
	float piece_length;
	bool long_along_z;
	// Sides: front, back, left, right; each from a start point along a direction
	const float half_x = 0.5f * part->size_x;
	const float half_z = 0.5f * part->size_z;

	if (type < 0 || context->objects->types.types[type].model < 0)
	{
		return;
	}
	model = &context->objects->models[context->objects->types.types[type].model];
	extent_x = model->bounds_maximum.x - model->bounds_minimum.x;
	extent_z = model->bounds_maximum.z - model->bounds_minimum.z;
	long_along_z = extent_z > extent_x;
	piece_length = SDL_max(SDL_max(extent_x, extent_z), 0.3f);

	for (int side = 0; side < 4; side++)
	{
		bool along_x = side < 2;
		float length = along_x ? part->size_x : part->size_z;
		int pieces = SDL_max(1, (int)SDL_roundf(length / piece_length));
		float scale = SDL_clamp(length / ((float)pieces * piece_length), 0.7f, 1.3f);

		for (int i = 0; i < pieces; i++)
		{
			float along = -0.5f * length + ((float)i + 0.5f) * length / (float)pieces;
			float local_x = along_x ? part->offset_x + along : part->offset_x + (side == 2 ? -half_x : half_x);
			float local_z = along_x ? part->offset_z + (side == 0 ? half_z : -half_z) : part->offset_z + along;
			float yaw = (along_x ? 0.0f : 0.5f * MATH3D_PI) + (long_along_z ? 0.5f * MATH3D_PI : 0.0f);

			if (side == part->gate_side && SDL_fabsf(along) < 0.5f * part->gate_width + 0.25f * piece_length * scale)
			{
				continue;
			}
			if (random_float(context->random) >= part->item_chance)
			{
				continue;
			}
			place_object(context, frame, part, type, local_x, local_z, yaw, true, scale);
		}
	}
}

static void place_rows(place_context_t *context, frame_t *frame, const layout_part_t *part)
{
	int rows = SDL_max(1, (int)(part->size_z / SDL_max(part->row_spacing, 0.1f)) + 1);
	int plants = SDL_max(1, (int)(part->size_x / SDL_max(part->plant_spacing, 0.1f)) + 1);

	for (int row = 0; row < rows; row++)
	{
		float local_z = part->offset_z - 0.5f * part->size_z + (float)row * part->row_spacing;
		int row_type = pick_choice(part, context->random);

		for (int k = 0; k < plants; k++)
		{
			float local_x = part->offset_x - 0.5f * part->size_x + (float)k * part->plant_spacing;
			int type = part->same_model_per_row ? row_type : pick_choice(part, context->random);
			float jitter_x = (random_float(context->random) * 2.0f - 1.0f) * part->jitter_meters;
			float jitter_z = (random_float(context->random) * 2.0f - 1.0f) * part->jitter_meters;
			float x;
			float z;

			if (random_float(context->random) >= part->item_chance)
			{
				continue;
			}
			place_object(context, frame, part, type, local_x + jitter_x, local_z + jitter_z, item_yaw(part, 0.0f, context->random), false,
				0.0f);
			// The soil between the plants is bare too
			frame_to_world(frame, local_x, local_z, &x, &z);
			add_clear_circle(context, x, z, 0.6f * SDL_max(part->row_spacing, part->plant_spacing));
		}
	}
}

// One object at an exact height over the ground (stacks)
static void place_lifted(place_context_t *context, const frame_t *frame, int type, float local_x, float local_z, float local_yaw, float scale,
	float lift)
{
	const object_type_t *definition = &context->objects->types.types[type];
	const object_model_t *model;
	float x;
	float z;

	if (definition->model < 0)
	{
		return;
	}
	model = &context->objects->models[definition->model];
	frame_to_world(frame, local_x, local_z, &x, &z);
	if (objects_add_lifted(context->objects, context->terrain, type, x, z, frame->facing + local_yaw, scale, context->group, lift))
	{
		context->placed++;
		add_clear_circle(context, x, z, 0.8f * model->horizontal_radius * scale + 0.3f);
	}
}

// A pile: count copies of one model, one on top of the other, all turned
// alike; with supports, two of those lie across the pile's ends under it
static void place_stack(place_context_t *context, frame_t *frame, const layout_part_t *part)
{
	int type = pick_choice(part, context->random);
	int count = random_count(part, context->random);
	const object_type_t *definition;
	const object_model_t *model;
	float scale = part->scale_minimum > 0.0f ? part->scale_minimum : 1.0f;
	float yaw = part->yaw_degrees * (MATH3D_PI / 180.0f);
	float height;
	float lift = part->lift_minimum;

	if (type < 0 || context->objects->types.types[type].model < 0)
	{
		return;
	}
	definition = &context->objects->types.types[type];
	model = &context->objects->models[definition->model];
	height = (model->bounds_maximum.y - model->bounds_minimum.y) * scale;
	if (part->support_type >= 0 && context->objects->types.types[part->support_type].model >= 0)
	{
		const object_model_t *support = &context->objects->models[context->objects->types.types[part->support_type].model];
		float extent_x = model->bounds_maximum.x - model->bounds_minimum.x;
		float extent_z = model->bounds_maximum.z - model->bounds_minimum.z;
		// The pile's long axis, in the frame: model X turned by the yaw is
		// (cos, -sin), model Z is (sin, cos)
		bool along_x = extent_x >= extent_z;
		float reach = 0.35f * SDL_max(extent_x, extent_z) * scale;
		float direction_x = along_x ? SDL_cosf(yaw) : SDL_sinf(yaw);
		float direction_z = along_x ? -SDL_sinf(yaw) : SDL_cosf(yaw);
		float support_height = support->bounds_maximum.y - support->bounds_minimum.y;

		for (int side = -1; side <= 1; side += 2)
		{
			place_lifted(context, frame, part->support_type, part->offset_x + direction_x * reach * (float)side,
				part->offset_z + direction_z * reach * (float)side, yaw + 0.5f * MATH3D_PI, 1.0f, lift);
		}
		// The pile rests on them (round things sink in a little)
		lift += 0.9f * support_height;
	}
	for (int i = 0; i < count; i++)
	{
		place_lifted(context, frame, type, part->offset_x, part->offset_z, yaw, scale, lift);
		lift += 0.98f * height;
	}
}

static void place_part(place_context_t *context, frame_t *frame, const layout_part_t *part, int depth)
{
	if (random_float(context->random) >= part->chance)
	{
		return;
	}
	switch (part->kind)
	{
		case LAYOUT_PART_STACK:
			place_stack(context, frame, part);
			break;
		case LAYOUT_PART_SINGLE:
		{
			float local_z = part->offset_z;

			if (part->place != LAYOUT_PLACE_GROUND)
			{
				if (!find_shore(frame, context->water))
				{
					return;
				}
				local_z += frame->shore_distance;
			}
			place_item(context, frame, part, part->offset_x, local_z, item_yaw(part, 0.0f, context->random), depth);
			break;
		}
		case LAYOUT_PART_RING:
		{
			int count = random_count(part, context->random);
			float start = random_float(context->random) * 2.0f * MATH3D_PI;

			for (int i = 0; i < count; i++)
			{
				float angle = start + 2.0f * MATH3D_PI * (float)i / (float)SDL_max(count, 1)
					+ (random_float(context->random) - 0.5f) * (MATH3D_PI / (float)SDL_max(count, 1)) * 0.5f;
				float radius = part->radius_minimum + (part->radius_maximum - part->radius_minimum) * random_float(context->random);

				if (random_float(context->random) >= part->item_chance)
				{
					continue;
				}
				place_item(context, frame, part, part->offset_x + SDL_sinf(angle) * radius, part->offset_z + SDL_cosf(angle) * radius,
					item_yaw(part, angle, context->random), depth);
			}
			break;
		}
		case LAYOUT_PART_SCATTER:
		{
			int count = random_count(part, context->random);
			float placed_x[64];
			float placed_z[64];
			int placed = 0;
			float spacing = 0.8f;

			for (int i = 0; i < count && placed < 64; i++)
			{
				for (int attempt = 0; attempt < 12; attempt++)
				{
					float angle = random_float(context->random) * 2.0f * MATH3D_PI;
					float inner = part->radius_minimum * part->radius_minimum;
					float outer = part->radius_maximum * part->radius_maximum;
					float radius = SDL_sqrtf(inner + (outer - inner) * random_float(context->random));
					float local_x = part->offset_x + SDL_sinf(angle) * radius;
					float local_z = part->offset_z + SDL_cosf(angle) * radius;
					bool crowded = false;

					for (int k = 0; k < placed; k++)
					{
						float dx = placed_x[k] - local_x;
						float dz = placed_z[k] - local_z;

						crowded = crowded || dx * dx + dz * dz < spacing * spacing;
					}
					if (crowded)
					{
						continue;
					}
					placed_x[placed] = local_x;
					placed_z[placed] = local_z;
					placed++;
					place_item(context, frame, part, local_x, local_z, item_yaw(part, angle, context->random), depth);
					break;
				}
			}
			break;
		}
		case LAYOUT_PART_ROWS:
			place_rows(context, frame, part);
			break;
		case LAYOUT_PART_FENCE:
			place_fence(context, frame, part);
			break;
	}
}

static void place_layout(place_context_t *context, int layout_index, float x, float z, float facing, int depth)
{
	const layout_t *layout;
	frame_t frame;

	if (layout_index < 0 || layout_index >= context->layouts->count || depth > MAXIMUM_DEPTH)
	{
		return;
	}
	layout = &context->layouts->layouts[layout_index];
	SDL_zero(frame);
	frame.x = x;
	frame.z = z;
	frame.facing = facing;
	frame.water_search_meters = layout->water_search_meters;
	if (layout->flatten)
	{
		flatten_ground(context->terrain, context->water, x, z, layout->footprint_meters);
	}
	for (int p = 0; p < layout->part_count; p++)
	{
		place_part(context, &frame, &layout->parts[p], depth);
	}
}

// Where a layout at (x, z) should face
static float choose_facing(const layout_t *layout, const terrain_t *terrain, const water_t *water, float x, float z, random_t *random)
{
	if (layout->orient == LAYOUT_ORIENT_WATER)
	{
		float best = 1e30f;
		float facing = 0.0f;

		for (int k = 0; k < 32; k++)
		{
			float angle = 2.0f * MATH3D_PI * (float)k / 32.0f;

			for (float distance = 1.0f; distance <= layout->water_search_meters && distance < best; distance += 1.0f)
			{
				if (water_depth_at(water, x + SDL_sinf(angle) * distance, z + SDL_cosf(angle) * distance) > SHORE_DEPTH_METERS)
				{
					best = distance;
					facing = angle;
					break;
				}
			}
		}
		if (best < 1e29f)
		{
			return facing;
		}
	}
	if (layout->orient != LAYOUT_ORIENT_RANDOM)
	{
		// Down the slope: against the height gradient around the center
		float radius = SDL_max(0.7f * layout->footprint_meters, 2.0f);
		float gradient_x = 0.0f;
		float gradient_z = 0.0f;

		for (int k = 0; k < 16; k++)
		{
			float angle = 2.0f * MATH3D_PI * (float)k / 16.0f;
			float height = terrain_sample_height_meters(terrain, x + SDL_sinf(angle) * radius, z + SDL_cosf(angle) * radius);

			gradient_x += height * SDL_sinf(angle);
			gradient_z += height * SDL_cosf(angle);
		}
		if (gradient_x * gradient_x + gradient_z * gradient_z > 1e-4f)
		{
			return SDL_atan2f(-gradient_x, -gradient_z);
		}
	}
	return random_float(random) * 2.0f * MATH3D_PI;
}

// A layout placed; the circles its objects want clear of plants added to
// the context's (cleared by the caller: at once, or all of a new map's at
// the end). Returns its group (0: nothing placed)
static int place_collecting(place_context_t *context, int layout, float x, float z)
{
	const layouts_t *layouts = context->layouts;

	if (layout < 0 || layout >= layouts->count)
	{
		return 0;
	}
	context->placed = 0;
	context->group = objects_new_group(context->objects);
	place_layout(context, layout, x, z, choose_facing(&layouts->layouts[layout], context->terrain, context->water, x, z, context->random), 0);
	return context->placed > 0 ? context->group : 0;
}

int layouts_place(const layouts_t *layouts, objects_t *objects, terrain_t *terrain, water_t *water, int layout, float x, float z,
	random_t *random)
{
	place_context_t context;
	int group;

	SDL_zero(context);
	context.layouts = layouts;
	context.objects = objects;
	context.terrain = terrain;
	context.water = water;
	context.random = random;
	group = place_collecting(&context, layout, x, z);
	objects_clear_vegetation(objects, context.clear, context.clear_count);
	SDL_free(context.clear);
	return group;
}

// --- New maps -----------------------------------------------------------------

typedef struct
{
	float x;
	float z;
	float radius;
} placed_site_t;

// Whether the ground at (x, z) suits the layout
static bool site_suits(const layout_t *layout, const terrain_t *terrain, const water_t *water, float x, float z)
{
	float radius = layout->footprint_meters;
	float lowest = 1e30f;
	float highest = -1e30f;
	int32_t cell_x;
	int32_t cell_y;
	float height_fraction;

	if (!terrain_world_to_cell(terrain, x, z, &cell_x, &cell_y))
	{
		return false;
	}
	if ((layout->spawn_soils & (1u << terrain->soil_types[terrain_index(terrain, cell_x, cell_y)])) == 0)
	{
		return false;
	}
	height_fraction = terrain_sample_height_meters(terrain, x, z) / SDL_max(terrain->max_height_meters, 1.0f);
	if (height_fraction < layout->spawn_height_minimum || height_fraction > layout->spawn_height_maximum)
	{
		return false;
	}
	for (int ring = 0; ring <= 2; ring++)
	{
		float ring_radius = radius * (float)ring * 0.5f;
		int count = ring == 0 ? 1 : 12;

		for (int k = 0; k < count; k++)
		{
			float angle = 2.0f * MATH3D_PI * (float)k / (float)count;
			float sample_x = x + SDL_sinf(angle) * ring_radius;
			float sample_z = z + SDL_cosf(angle) * ring_radius;
			float height = terrain_sample_height_meters(terrain, sample_x, sample_z);

			if (water_depth_at(water, sample_x, sample_z) > DRY_DEPTH_METERS)
			{
				return false;
			}
			lowest = SDL_min(lowest, height);
			highest = SDL_max(highest, height);
		}
	}
	return highest - lowest <= layout->flatness_meters;
}

static uint64_t hash_name(const char *name)
{
	uint64_t hash = 1469598103934665603ull;

	for (const char *character = name; *character != '\0'; character++)
	{
		hash = (hash ^ (uint64_t)(unsigned char)SDL_tolower((unsigned char)*character)) * 1099511628211ull;
	}
	return hash;
}

void layouts_populate_map(const layouts_t *layouts, objects_t *objects, terrain_t *terrain, water_t *water,
	const objects_settings_t *settings)
{
	float map_meters = (float)terrain->size * terrain->cell_size_meters;
	float square_kilometers = map_meters * map_meters * 1e-6f;
	placed_site_t *sites = NULL;
	int site_count = 0;
	int site_capacity = 0;
	uint64_t start = SDL_GetPerformanceCounter();
	// Every layout's circles to clear of plants, cleared once at the end:
	// cleared after each, each time the whole list was sorted and
	// compacted for a few trees (a million objects, 390 layouts: 25 s).
	// Nothing in the placing looks at the plants (the ground and the water
	// only), and the clearing leaves the layouts' own objects (a group)
	place_context_t context;

	SDL_zero(context);
	context.layouts = layouts;
	context.objects = objects;
	context.terrain = terrain;
	context.water = water;

	for (int l = 0; l < layouts->count; l++)
	{
		const layout_t *layout = &layouts->layouts[l];
		random_t random;
		float wanted_real = layout->spawn_per_square_kilometer * square_kilometers * SDL_max(settings->density_scale, 0.0f);
		int wanted;
		int placed = 0;
		float margin = layout->footprint_meters + 2.0f;

		if (wanted_real <= 0.0f || map_meters <= 2.0f * margin)
		{
			continue;
		}
		random_seed(&random, ((uint64_t)terrain->seed * 0x9E3779B97F4A7C15ull) ^ hash_name(layout->name));
		// A fraction of a layout is one with that probability
		wanted = (int)wanted_real + (random_float(&random) < wanted_real - SDL_floorf(wanted_real) ? 1 : 0);
		for (int attempt = 0; attempt < wanted * 80 && placed < wanted; attempt++)
		{
			float x = margin + random_float(&random) * (map_meters - 2.0f * margin);
			float z = margin + random_float(&random) * (map_meters - 2.0f * margin);
			bool crowded = false;

			for (int s = 0; s < site_count && !crowded; s++)
			{
				float limit = sites[s].radius + layout->footprint_meters + layout->spacing_meters;
				float dx = sites[s].x - x;
				float dz = sites[s].z - z;

				crowded = dx * dx + dz * dz < limit * limit;
			}
			if (crowded || !site_suits(layout, terrain, water, x, z))
			{
				continue;
			}
			if (layout->needs_water)
			{
				frame_t frame;

				SDL_zero(frame);
				frame.x = x;
				frame.z = z;
				frame.facing = choose_facing(layout, terrain, water, x, z, &random);
				frame.water_search_meters = layout->water_search_meters;
				if (!find_shore(&frame, water))
				{
					continue;
				}
			}
			context.random = &random;
			if (place_collecting(&context, l, x, z) == 0)
			{
				continue;
			}
			if (site_count == site_capacity)
			{
				int capacity = SDL_max(32, site_capacity * 2);
				placed_site_t *grown = SDL_realloc(sites, sizeof(placed_site_t) * (size_t)capacity);

				if (grown == NULL)
				{
					break;
				}
				sites = grown;
				site_capacity = capacity;
			}
			sites[site_count].x = x;
			sites[site_count].z = z;
			sites[site_count].radius = layout->footprint_meters;
			site_count++;
			placed++;
		}
		if (placed > 0)
		{
			SDL_Log("Layouts: %d %s%s", placed, layout->name, placed < wanted ? " (no more room that suits it)" : "");
		}
	}
	SDL_free(sites);
	objects_clear_vegetation(objects, context.clear, context.clear_count);
	SDL_free(context.clear);
	objects_sort(objects);
	SDL_Log("Layouts: %d placed in %.0f ms", site_count,
		1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency());
}
