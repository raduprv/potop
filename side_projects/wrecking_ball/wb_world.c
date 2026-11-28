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
	if (!shallow_water_create(&world->water, world->terrain_heights, WB_TERRAIN_CELLS, WB_TERRAIN_CELL_METERS, -wb_world_terrain_half_size(),
		-wb_world_terrain_half_size()))
	{
		return false;
	}
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
		for (int b = 0; b < WB_BUILDINGS_MAX; b++)
		{
			if (world->buildings[b].used)
			{
				wreck_model_remove_projectile(world->buildings[b].model, ball->body);
			}
		}
		b3DestroyBody(ball->body);
		ball->used = false;
	}
}

static void free_building(wb_building_t *building)
{
	if (building->model != NULL)
	{
		wreck_model_destroy(building->model);
		SDL_free(building->model);
		building->model = NULL;
	}
	building->used = false;
}

void wb_world_clear(wb_world_t *world)
{
	clear_balls(world);
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		free_building(&world->buildings[b]);
	}
}

void wb_world_destroy(wb_world_t *world)
{
	wb_world_clear(world);
	if (B3_IS_NON_NULL(world->world))
	{
		b3DestroyWorld(world->world);
	}
	if (world->terrain_field != NULL)
	{
		b3DestroyHeightField(world->terrain_field);
	}
	shallow_water_destroy(&world->water);
	SDL_zerop(world);
}

// ---------------------------------------------------------------------------
// Reading a building

typedef struct
{
	wreck_model_t *model;
	const char *path;
	float origin[3];                   // the building's foot: its place on the ground
	float turn[4];                     // its turn round the vertical, a quaternion
	bool failed;
} reading_t;

static void cross_3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

// A point of the building's frame in the world
static b3Vec3 placed_point(const reading_t *reading, float x, float y, float z)
{
	const float local[3] = { x, y, z };
	float twice_cross[3];
	float second[3];
	float turned[3];

	cross_3(reading->turn, local, twice_cross);
	for (int k = 0; k < 3; k++)
	{
		twice_cross[k] *= 2.0f;
	}
	cross_3(reading->turn, twice_cross, second);
	for (int k = 0; k < 3; k++)
	{
		turned[k] = local[k] + reading->turn[3] * twice_cross[k] + second[k];
	}
	return (b3Vec3){ reading->origin[0] + turned[0], reading->origin[1] + turned[1], reading->origin[2] + turned[2] };
}

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
		position = placed_point(reading, numbers[0], numbers[1], numbers[2]);
		// (its own turn, then the building's)
		rotation = b3MulQuat((b3Quat){ { reading->turn[0], reading->turn[1], reading->turn[2] }, reading->turn[3] },
			b3NormalizeQuat((b3Quat){ { numbers[3], numbers[4], numbers[5] }, numbers[6] }));
		rotation = b3NormalizeQuat(rotation);
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
		wreck_model_add_link(reading->model, piece_a, piece_b, placed_point(reading, numbers[0], numbers[1], numbers[2]), (wreck_link_type_t)type);
		return true;
	}
	return false;
}

static void measure_building(wb_building_t *building)
{
	const wreck_model_t *model = building->model;

	for (int k = 0; k < 3; k++)
	{
		building->minimum[k] = 1e9f;
		building->maximum[k] = -1e9f;
	}
	for (int p = 0; p < model->piece_count; p++)
	{
		const wreck_piece_t *piece = &model->pieces[p];
		float reach = piece->shape == WRECK_PIECE_LOG ? SDL_max(piece->radius, piece->half_length) : b3Length(piece->half_extents);
		const float center[3] = { piece->local.p.x, piece->local.p.y, piece->local.p.z };

		for (int k = 0; k < 3; k++)
		{
			building->minimum[k] = SDL_min(building->minimum[k], center[k] - reach);
			building->maximum[k] = SDL_max(building->maximum[k], center[k] + reach);
		}
	}
}

int wb_world_add_building(wb_world_t *world, const char *path, float x, float z, float yaw_radians)
{
	wb_building_t *building = NULL;
	int number = -1;
	reading_t reading;

	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		if (!world->buildings[b].used)
		{
			building = &world->buildings[b];
			number = b;
			break;
		}
	}
	if (building == NULL)
	{
		SDL_Log("World: there are %d buildings already", WB_BUILDINGS_MAX);
		return -1;
	}
	SDL_zerop(building);
	building->model = SDL_calloc(1, sizeof(*building->model));
	if (building->model == NULL)
	{
		return -1;
	}
	wreck_model_create(building->model, world->world, &world->settings);
	building->used = true;
	SDL_strlcpy(building->path, path, sizeof(building->path));
	building->x = x;
	building->z = z;
	building->yaw_radians = yaw_radians;
	SDL_zero(reading);
	reading.model = building->model;
	reading.path = path;
	reading.origin[0] = x;
	reading.origin[1] = wb_world_terrain_height(world, x, z);
	reading.origin[2] = z;
	reading.turn[1] = SDL_sinf(0.5f * yaw_radians);
	reading.turn[3] = SDL_cosf(0.5f * yaw_radians);
	if (!wb_ini_parse_file(path, read_wreck_key, &reading) || building->model->piece_count == 0)
	{
		SDL_Log("World: no %s, or nothing in it", path);
		free_building(building);
		return -1;
	}
	wreck_model_finish(building->model);
	measure_building(building);
	// (the balls in the air know the new building too)
	for (int i = 0; i < WB_BALLS_MAX; i++)
	{
		if (world->balls[i].used)
		{
			wreck_model_add_projectile(building->model, world->balls[i].body, world->balls[i].radius);
		}
	}
	SDL_Log("World: %s at (%.1f, %.1f): %d pieces, %d links%s", path, (double)x, (double)z, building->model->piece_count,
		building->model->link_count, reading.failed ? " (some lines could not be read, see above)" : "");
	return number;
}

void wb_world_reset_buildings(wb_world_t *world)
{
	wb_building_t placed[WB_BUILDINGS_MAX];

	SDL_memcpy(placed, world->buildings, sizeof(placed));
	wb_world_clear(world);
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		if (placed[b].used)
		{
			wb_world_add_building(world, placed[b].path, placed[b].x, placed[b].z, placed[b].yaw_radians);
		}
	}
}

int wb_world_building_count(const wb_world_t *world)
{
	int count = 0;

	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		count += world->buildings[b].used ? 1 : 0;
	}
	return count;
}

bool wb_world_buildings_bounds(const wb_world_t *world, float minimum[3], float maximum[3])
{
	bool any = false;

	for (int k = 0; k < 3; k++)
	{
		minimum[k] = 1e9f;
		maximum[k] = -1e9f;
	}
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		const wb_building_t *building = &world->buildings[b];

		if (!building->used)
		{
			continue;
		}
		any = true;
		for (int k = 0; k < 3; k++)
		{
			minimum[k] = SDL_min(minimum[k], building->minimum[k]);
			maximum[k] = SDL_max(maximum[k], building->maximum[k]);
		}
	}
	return any;
}

int wb_world_piece_count(const wb_world_t *world)
{
	int count = 0;

	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		count += world->buildings[b].used ? world->buildings[b].model->piece_count : 0;
	}
	return count;
}

int wb_world_broken_link_count(const wb_world_t *world)
{
	int count = 0;

	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		count += world->buildings[b].used ? world->buildings[b].model->broken_link_count : 0;
	}
	return count;
}

static void remove_ball(wb_world_t *world, wb_ball_t *ball)
{
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		if (world->buildings[b].used)
		{
			wreck_model_remove_projectile(world->buildings[b].model, ball->body);
		}
	}
	b3DestroyBody(ball->body);
	ball->used = false;
}

// The water at a point, for the model (wreck_water_function_t)
static bool water_at(void *user, b3Vec3 point, float *surface_height, b3Vec3 *flow)
{
	const shallow_water_t *water = user;
	float depth;
	float velocity[2];

	if (!shallow_water_sample(water, point.x, point.z, &depth, surface_height, velocity))
	{
		return false;
	}
	*flow = (b3Vec3){ velocity[0], 0.0f, velocity[1] };
	return true;
}

// The models told of the water only while there is some: with it set, a
// model counts as busy (stepped every step) for as long as anything of
// it is loose
static void tell_models_of_water(wb_world_t *world)
{
	bool has_water = world->water.total_volume > 0.0;

	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		wb_building_t *building = &world->buildings[b];

		if (building->used && building->knows_water != has_water)
		{
			wreck_model_set_water(building->model, has_water ? water_at : NULL, &world->water);
			building->knows_water = has_water;
		}
	}
}

void wb_world_step(wb_world_t *world)
{
	const float step_seconds = 1.0f / (float)WB_WORLD_STEPS_PER_SECOND;
	b3ContactEvents events;

	shallow_water_step(&world->water, step_seconds);
	tell_models_of_water(world);

	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		if (world->buildings[b].used)
		{
			wreck_model_before_step(world->buildings[b].model);
		}
	}
	b3World_Step(world->world, step_seconds, 4);
	// Each hit to the buildings whose pieces it touches (both, for a hit
	// between pieces of two)
	events = b3World_GetContactEvents(world->world);
	for (int e = 0; e < events.hitCount; e++)
	{
		const b3ContactHitEvent *event = &events.hitEvents[e];
		wreck_model_t *model_a = wreck_shape_model(event->shapeIdA);
		wreck_model_t *model_b = wreck_shape_model(event->shapeIdB);

		if (model_a != NULL)
		{
			wreck_model_take_hit(model_a, event, world->frame_index);
		}
		if (model_b != NULL && model_b != model_a)
		{
			wreck_model_take_hit(model_b, event, world->frame_index);
		}
	}
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		if (world->buildings[b].used && wreck_model_is_busy(world->buildings[b].model))
		{
			wreck_model_after_step(world->buildings[b].model, world->frame_index, step_seconds);
		}
	}
	// The air and the water on the balls (the models do their own pieces)
	for (int i = 0; i < WB_BALLS_MAX; i++)
	{
		const wb_ball_t *ball = &world->balls[i];

		if (ball->used)
		{
			wreck_push_ball_by_fluids(&world->settings, world->water.total_volume > 0.0 ? water_at : NULL, &world->water, ball->body, ball->radius,
				step_seconds);
		}
	}
	// Bombs that knocked into something go off
	for (int i = 0; i < WB_BALLS_MAX; i++)
	{
		wb_ball_t *ball = &world->balls[i];
		b3Vec3 velocity;
		b3Vec3 change;

		if (!ball->used || !ball->explosive)
		{
			continue;
		}
		velocity = b3Body_GetLinearVelocity(ball->body);
		change = b3Sub(velocity, ball->last_velocity);
		ball->last_velocity = velocity;
		ball->age_seconds += step_seconds;
		if (ball->age_seconds >= WB_BOMB_ARM_SECONDS && b3Length(change) > WB_BOMB_KNOCK_SPEED)
		{
			b3Vec3 at = b3Body_GetPosition(ball->body);
			const float position[3] = { at.x, at.y, at.z };
			float kilograms = ball->kilograms;

			remove_ball(world, ball);
			wb_world_blast(world, position, kilograms);
		}
	}
	// Balls fallen off the world go
	for (int i = 0; i < WB_BALLS_MAX; i++)
	{
		if (world->balls[i].used && b3Body_GetPosition(world->balls[i].body).y < -50.0f)
		{
			remove_ball(world, &world->balls[i]);
		}
	}
	world->frame_index++;
}

static wb_ball_t *throw_round_thing(wb_world_t *world, const float origin[3], const float direction[3], float speed, float radius, float density);

void wb_world_throw_ball(wb_world_t *world, const float origin[3], const float direction[3], float speed)
{
	throw_round_thing(world, origin, direction, speed, world->settings.ball_radius, world->settings.ball_density);
}

void wb_world_throw_bomb(wb_world_t *world, const float origin[3], const float direction[3], float speed, float kilograms)
{
	wb_ball_t *ball = throw_round_thing(world, origin, direction, speed, WB_BOMB_RADIUS, WB_BOMB_DENSITY);

	ball->explosive = true;
	ball->kilograms = kilograms;
	ball->last_velocity = b3Body_GetLinearVelocity(ball->body);
}

static wb_ball_t *throw_round_thing(wb_world_t *world, const float origin[3], const float direction[3], float speed, float radius, float density)
{
	wb_ball_t *ball = &world->balls[world->next_ball];
	b3BodyDef body_definition = b3DefaultBodyDef();
	b3ShapeDef shape_definition = b3DefaultShapeDef();
	b3Sphere sphere;

	world->next_ball = (world->next_ball + 1) % WB_BALLS_MAX;
	if (ball->used)
	{
		remove_ball(world, ball);
	}
	body_definition.type = b3_dynamicBody;
	body_definition.position = (b3Vec3){ origin[0], origin[1], origin[2] };
	body_definition.linearVelocity = (b3Vec3){ direction[0] * speed, direction[1] * speed, direction[2] * speed };
	body_definition.isBullet = true;
	ball->body = b3CreateBody(world->world, &body_definition);
	ball->radius = radius;
	ball->explosive = false;
	ball->kilograms = 0.0f;
	ball->age_seconds = 0.0f;
	sphere.center = (b3Vec3){ 0.0f, 0.0f, 0.0f };
	sphere.radius = ball->radius;
	shape_definition.density = density;
	shape_definition.enableHitEvents = true;
	b3CreateSphereShape(ball->body, &shape_definition, &sphere);
	ball->used = true;
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		if (world->buildings[b].used)
		{
			wreck_model_add_projectile(world->buildings[b].model, ball->body, ball->radius);
		}
	}
	return ball;
}

void wb_world_blast(wb_world_t *world, const float position[3], float kilograms)
{
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		if (world->buildings[b].used)
		{
			wreck_model_blast_kilograms(world->buildings[b].model, (b3Vec3){ position[0], position[1], position[2] }, kilograms,
				world->frame_index);
		}
	}
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
