// The world WreckingBall simulates: see wb_world.h

#include "wb_world.h"

#include <SDL3/SDL.h>

#include "wb_ini.h"

static const char *const MATERIAL_NAMES[WRECK_MATERIAL_COUNT] = { "wood", "brick", "glass" };
static const char *const LINK_NAMES[WRECK_LINK_TYPE_COUNT] = { "nails", "mortar", "foundation", "glazing", "notch" };

float wb_world_terrain_half_size(void)
{
	return 0.5f * (float)WB_TERRAIN_CELLS * WB_TERRAIN_CELL_METERS;
}

float wb_world_terrain_height(const wb_world_t *world, float x, float z)
{
	float half = wb_world_terrain_half_size();
	float cell_x = (x + half) / WB_TERRAIN_CELL_METERS;
	float cell_z = (z + half) / WB_TERRAIN_CELL_METERS;
	int column;
	int row;
	float u;
	float v;
	const float *heights = world->terrain_heights;
	int stride = WB_TERRAIN_CELLS + 1;

	cell_x = SDL_clamp(cell_x, 0.0f, (float)WB_TERRAIN_CELLS - 0.001f);
	cell_z = SDL_clamp(cell_z, 0.0f, (float)WB_TERRAIN_CELLS - 0.001f);
	column = (int)cell_x;
	row = (int)cell_z;
	u = cell_x - (float)column;
	v = cell_z - (float)row;
	return (heights[row * stride + column] * (1.0f - u) + heights[row * stride + column + 1] * u) * (1.0f - v)
		+ (heights[(row + 1) * stride + column] * (1.0f - u) + heights[(row + 1) * stride + column + 1] * u) * v;
}

static void make_terrain(wb_world_t *world)
{
	int count = WB_TERRAIN_CELLS + 1;
	float half = wb_world_terrain_half_size();
	float lowest = 0.0f;
	float highest = 0.0f;
	b3HeightFieldDef definition;
	b3BodyDef body_definition = b3DefaultBodyDef();
	b3ShapeDef shape_definition = b3DefaultShapeDef();

	// Flat for now (the heights are there for water and hills later)
	for (int i = 0; i < count * count; i++)
	{
		world->terrain_heights[i] = 0.0f;
	}
	for (int i = 0; i < count * count; i++)
	{
		lowest = SDL_min(lowest, world->terrain_heights[i]);
		highest = SDL_max(highest, world->terrain_heights[i]);
	}
	SDL_zero(definition);
	definition.heights = world->terrain_heights;
	definition.scale = (b3Vec3){ WB_TERRAIN_CELL_METERS, 1.0f, WB_TERRAIN_CELL_METERS };
	definition.countX = count;
	definition.countZ = count;
	definition.globalMinimumHeight = lowest - 1.0f;
	definition.globalMaximumHeight = highest + 1.0f;
	world->terrain_field = b3CreateHeightField(&definition);
	if (world->terrain_field == NULL)
	{
		SDL_Log("World: the ground's height field could not be made");
		return;
	}
	body_definition.type = b3_staticBody;
	body_definition.position = (b3Vec3){ -half, 0.0f, -half };
	world->terrain_body = b3CreateBody(world->world, &body_definition);
	shape_definition.baseMaterial.friction = 0.8f;
	b3CreateHeightFieldShape(world->terrain_body, &shape_definition, world->terrain_field);
}

// The model's numbers; the engine's own sections of the same file (how
// Potop draws the pieces) are not ours
static bool read_settings_key(void *user, const char *section, const char *key, const char *value)
{
	if (SDL_strcmp(section, "looks") == 0 || SDL_strcmp(section, "cuts") == 0)
	{
		return true;
	}
	return wreck_settings_set(user, section, key, value);
}

bool wb_world_create(wb_world_t *world, const char *settings_path)
{
	b3WorldDef definition = b3DefaultWorldDef();

	SDL_zerop(world);
	wreck_settings_default(&world->settings);
	if (settings_path != NULL && !wb_ini_parse_file(settings_path, read_settings_key, &world->settings))
	{
		SDL_Log("World: no %s, the model's own numbers", settings_path);
	}
	definition.gravity = (b3Vec3){ 0.0f, -9.81f, 0.0f };
	definition.hitEventThreshold = world->settings.hit_speed;
	world->world = b3CreateWorld(&definition);
	make_terrain(world);
	return true;
}

static void clear_balls(wb_world_t *world)
{
	for (int i = 0; i < WB_BALLS_MAX; i++)
	{
		wb_ball_t *ball = &world->balls[i];

		if (!ball->used)
		{
			continue;
		}
		if (world->model != NULL && world->building_loaded)
		{
			wreck_model_remove_projectile(world->model, ball->body);
		}
		b3DestroyBody(ball->body);
		ball->used = false;
	}
}

static void free_building(wb_world_t *world)
{
	clear_balls(world);
	if (world->model != NULL)
	{
		if (world->building_loaded)
		{
			wreck_model_destroy(world->model);
		}
		SDL_free(world->model);
		world->model = NULL;
	}
	world->building_loaded = false;
}

void wb_world_destroy(wb_world_t *world)
{
	free_building(world);
	if (B3_IS_NON_NULL(world->world))
	{
		b3DestroyWorld(world->world);
	}
	if (world->terrain_field != NULL)
	{
		b3DestroyHeightField(world->terrain_field);
	}
	SDL_zerop(world);
}

// ---------------------------------------------------------------------------
// Reading a building

typedef struct
{
	wreck_model_t *model;
	const char *path;
	float foot_height;                 // the ground under the building's middle
	bool failed;
} reading_t;

// "a, b, c" into the numbers; how many were read
static int read_numbers(const char *text, float *numbers, int most)
{
	int count = 0;
	const char *at = text;

	while (count < most && *at != '\0')
	{
		char *end;
		float value = (float)SDL_strtod(at, &end);

		if (end == at)
		{
			break;
		}
		numbers[count++] = value;
		at = end;
		while (*at == ' ' || *at == '\t' || *at == ',')
		{
			at++;
		}
	}
	return count;
}

// The first word of a value, up to its comma or space, and where the rest begins
static const char *first_word(const char *value, char *word, size_t word_size)
{
	size_t length = 0;

	while (*value == ' ' || *value == '\t')
	{
		value++;
	}
	while (value[length] != '\0' && value[length] != ',' && value[length] != ' ' && length + 1 < word_size)
	{
		word[length] = value[length];
		length++;
	}
	word[length] = '\0';
	value += length;
	while (*value == ' ' || *value == '\t' || *value == ',')
	{
		value++;
	}
	return value;
}

static bool read_wreck_key(void *user, const char *section, const char *key, const char *value)
{
	reading_t *reading = user;
	char word[32];
	const char *rest = first_word(value, word, sizeof(word));

	if (SDL_strcmp(section, "pieces") == 0 && (SDL_strcmp(key, "box") == 0 || SDL_strcmp(key, "log") == 0))
	{
		int material = -1;
		bool box = SDL_strcmp(key, "box") == 0;
		int wanted = box ? 10 : 9;
		float numbers[10];
		b3Vec3 position;
		b3Quat rotation;

		for (int m = 0; m < WRECK_MATERIAL_COUNT; m++)
		{
			if (SDL_strcasecmp(word, MATERIAL_NAMES[m]) == 0)
			{
				material = m;
			}
		}
		if (material < 0 || read_numbers(rest, numbers, wanted) != wanted)
		{
			SDL_Log("World: %s: a %s I cannot read: '%s'", reading->path, key, value);
			reading->failed = true;
			return true;
		}
		position = (b3Vec3){ numbers[0], numbers[1] + reading->foot_height, numbers[2] };
		rotation = b3NormalizeQuat((b3Quat){ { numbers[3], numbers[4], numbers[5] }, numbers[6] });
		if (box)
		{
			wreck_model_add_box(reading->model, position, rotation, (b3Vec3){ numbers[7], numbers[8], numbers[9] }, (wreck_material_t)material,
				0xffffffffu);
		}
		else
		{
			wreck_model_add_log(reading->model, position, rotation, numbers[7], numbers[8], (wreck_material_t)material, 0xffffffffu);
		}
		return true;
	}
	if (SDL_strcmp(section, "links") == 0 && SDL_strcmp(key, "link") == 0)
	{
		int piece_a = SDL_atoi(word);
		int piece_b;
		int type = -1;
		char type_name[32];
		float numbers[3];

		rest = first_word(rest, word, sizeof(word));
		piece_b = SDL_atoi(word);
		rest = first_word(rest, type_name, sizeof(type_name));
		for (int t = 0; t < WRECK_LINK_TYPE_COUNT; t++)
		{
			if (SDL_strcasecmp(type_name, LINK_NAMES[t]) == 0)
			{
				type = t;
			}
		}
		if (type < 0 || read_numbers(rest, numbers, 3) != 3 || piece_a < 0 || piece_a >= reading->model->piece_count
			|| piece_b < WRECK_GROUND || piece_b >= reading->model->piece_count)
		{
			SDL_Log("World: %s: a link I cannot read: '%s'", reading->path, value);
			reading->failed = true;
			return true;
		}
		wreck_model_add_link(reading->model, piece_a, piece_b, (b3Vec3){ numbers[0], numbers[1] + reading->foot_height, numbers[2] },
			(wreck_link_type_t)type);
		return true;
	}
	return false;
}

static void measure_building(wb_world_t *world)
{
	const wreck_model_t *model = world->model;

	for (int k = 0; k < 3; k++)
	{
		world->building_minimum[k] = 1e9f;
		world->building_maximum[k] = -1e9f;
	}
	for (int p = 0; p < model->piece_count; p++)
	{
		const wreck_piece_t *piece = &model->pieces[p];
		float reach = piece->shape == WRECK_PIECE_LOG ? SDL_max(piece->radius, piece->half_length) : b3Length(piece->half_extents);
		const float center[3] = { piece->local.p.x, piece->local.p.y, piece->local.p.z };

		for (int k = 0; k < 3; k++)
		{
			world->building_minimum[k] = SDL_min(world->building_minimum[k], center[k] - reach);
			world->building_maximum[k] = SDL_max(world->building_maximum[k], center[k] + reach);
		}
	}
}

bool wb_world_load_building(wb_world_t *world, const char *path)
{
	reading_t reading;

	free_building(world);
	world->model = SDL_calloc(1, sizeof(*world->model));
	if (world->model == NULL)
	{
		return false;
	}
	wreck_model_create(world->model, world->world, &world->settings);
	world->building_loaded = true;
	SDL_zero(reading);
	reading.model = world->model;
	reading.path = path;
	reading.foot_height = wb_world_terrain_height(world, 0.0f, 0.0f);
	if (!wb_ini_parse_file(path, read_wreck_key, &reading) || world->model->piece_count == 0)
	{
		SDL_Log("World: no %s, or nothing in it", path);
		free_building(world);
		return false;
	}
	wreck_model_finish(world->model);
	if (path != world->building_path)
	{
		SDL_strlcpy(world->building_path, path, sizeof(world->building_path));
	}
	measure_building(world);
	world->broken_links_at_start = world->model->broken_link_count;
	SDL_Log("World: %s: %d pieces, %d links%s", path, world->model->piece_count, world->model->link_count,
		reading.failed ? " (some lines could not be read, see above)" : "");
	return true;
}

bool wb_world_reset_building(wb_world_t *world)
{
	if (world->building_path[0] == '\0')
	{
		return false;
	}
	return wb_world_load_building(world, world->building_path);
}

void wb_world_step(wb_world_t *world)
{
	const float step_seconds = 1.0f / (float)WB_WORLD_STEPS_PER_SECOND;
	b3ContactEvents events;
	bool has_model = world->model != NULL && world->building_loaded;

	if (has_model)
	{
		wreck_model_before_step(world->model);
	}
	b3World_Step(world->world, step_seconds, 4);
	if (has_model)
	{
		events = b3World_GetContactEvents(world->world);
		for (int e = 0; e < events.hitCount; e++)
		{
			const b3ContactHitEvent *event = &events.hitEvents[e];

			if (wreck_shape_model(event->shapeIdA) == world->model || wreck_shape_model(event->shapeIdB) == world->model)
			{
				wreck_model_take_hit(world->model, event, world->frame_index);
			}
		}
		if (wreck_model_is_busy(world->model))
		{
			wreck_model_after_step(world->model, world->frame_index, step_seconds);
		}
	}
	// Balls fallen off the world go
	for (int i = 0; i < WB_BALLS_MAX; i++)
	{
		wb_ball_t *ball = &world->balls[i];

		if (ball->used && b3Body_GetPosition(ball->body).y < -50.0f)
		{
			if (has_model)
			{
				wreck_model_remove_projectile(world->model, ball->body);
			}
			b3DestroyBody(ball->body);
			ball->used = false;
		}
	}
	world->frame_index++;
}

void wb_world_throw_ball(wb_world_t *world, const float origin[3], const float direction[3], float speed)
{
	wb_ball_t *ball = &world->balls[world->next_ball];
	b3BodyDef body_definition = b3DefaultBodyDef();
	b3ShapeDef shape_definition = b3DefaultShapeDef();
	b3Sphere sphere;
	bool has_model = world->model != NULL && world->building_loaded;

	world->next_ball = (world->next_ball + 1) % WB_BALLS_MAX;
	if (ball->used)
	{
		if (has_model)
		{
			wreck_model_remove_projectile(world->model, ball->body);
		}
		b3DestroyBody(ball->body);
		ball->used = false;
	}
	body_definition.type = b3_dynamicBody;
	body_definition.position = (b3Vec3){ origin[0], origin[1], origin[2] };
	body_definition.linearVelocity = (b3Vec3){ direction[0] * speed, direction[1] * speed, direction[2] * speed };
	body_definition.isBullet = true;
	ball->body = b3CreateBody(world->world, &body_definition);
	ball->radius = world->settings.ball_radius;
	sphere.center = (b3Vec3){ 0.0f, 0.0f, 0.0f };
	sphere.radius = ball->radius;
	shape_definition.density = world->settings.ball_density;
	shape_definition.enableHitEvents = true;
	b3CreateSphereShape(ball->body, &shape_definition, &sphere);
	ball->used = true;
	if (has_model)
	{
		wreck_model_add_projectile(world->model, ball->body, ball->radius);
	}
}

void wb_world_blast(wb_world_t *world, const float position[3], float kilograms)
{
	if (world->model == NULL || !world->building_loaded)
	{
		return;
	}
	wreck_model_blast_kilograms(world->model, (b3Vec3){ position[0], position[1], position[2] }, kilograms, world->frame_index);
}

bool wb_world_cast_ray(const wb_world_t *world, const float origin[3], const float direction[3], float distance, float hit_point[3])
{
	b3RayResult result = b3World_CastRayClosest(world->world, (b3Vec3){ origin[0], origin[1], origin[2] },
		(b3Vec3){ direction[0] * distance, direction[1] * distance, direction[2] * distance }, b3DefaultQueryFilter());

	if (!result.hit)
	{
		return false;
	}
	hit_point[0] = result.point.x;
	hit_point[1] = result.point.y;
	hit_point[2] = result.point.z;
	return true;
}
