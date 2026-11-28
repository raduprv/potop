// WreckingBall: build things out of pieces and knock them down. A side
// project of Potop's, on the engine's destruction model (engine/wreck.h)
// with a renderer of its own on SDL_GPU (wb_render.h).
//
// For now: one building of a .wreck file on flat ground, the ball and
// blasts. Later: the piece catalog, building by drag and drop, modules,
// water (see the project's notes in Potop's chat history, and TODO).
//
//   wrecking_ball [BUILDING.wreck] [options]
//
//   --settings FILE     the model's numbers (default ../../data/wreck.ini)
//   --shaders DIR       the compiled shaders (default build/shaders)
//   --shot FILE.png     no window: step --frames frames, save the last one
//   --frames N          (default 1)
//   --size WxH          the picture's size without a window (default 1280x720)
//   --camera EX EY EZ TX TY TZ    where the camera is and looks at
//   --add X Z YAW       one more of the building at (x, z), turned yaw degrees
//   --throw-at N        throw a ball from the camera at the building at frame N
//   --blast-at N        a blast in the building's middle at frame N
//   --flood-at N        water 60 cm deep everywhere at frame N
//   --water-blast-at N  a blast 50 cm up, 8 m off the building's side, at frame N
//   --bomb-at N         throw a bomb ball from the camera at the building at frame N
//   --trace             with --shot: the fastest loose piece every half second
//   --pour-at N         water poured as with the mouse, for 2 seconds from frame N, 6 m off the building's side
//
// In the window:
//   right mouse button held: look round; W A S D, Q E: move (shift: faster)
//   1: the wrecking ball, 2: the bomb, 3: water, 4: the bomb ball; left
//   click: the weapon at the cursor (the balls thrown towards it, the bomb
//   ball going off where it first knocks into anything; the bomb set off
//   where it points; water poured there while held, 1000 cubic meters a
//   second)
//   F: a flood (the water 30 cm higher everywhere); G: the water gone
//   middle button held: turn round what is under the cursor; wheel: closer
//   or further, towards the cursor
//   F1 to F4: house, cabin, support, bend, alone in the middle; H: one
//   more of it where the cursor points; R: all again as placed; C or
//   Delete: all taken away
//   space: pause; T: slow motion; Esc: quit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "wb_effects.h"
#include "wb_math.h"
#include "wb_render.h"
#include "wb_world.h"

#define BALL_SPEED 30.0f
#define BLAST_KILOGRAMS 3.0f

typedef struct
{
	float position[3];
	float yaw;                         // radians round y, 0 looking along -z
	float pitch;                       // radians up
} fly_camera_t;

typedef struct
{
	const char *building_path;
	const char *settings_path;
	const char *shader_directory;
	const char *shot_path;
	int frames;
	int width;
	int height;
	bool camera_given;
	float camera[6];
	int throw_frame;
	int blast_frame;
	int flood_frame;
	int pour_frame;
	int bomb_frame;
	int water_blast_frame;
	bool trace;
	// More of the same building: x, z, yaw in degrees each
	float extra_buildings[WB_BUILDINGS_MAX][3];
	int extra_building_count;
} options_t;

static const char *const BUILDING_NAMES[4] = { "house", "cabin", "support", "bend" };

typedef enum
{
	WEAPON_BALL,
	WEAPON_BOMB,
	WEAPON_WATER,
	WEAPON_BOMB_BALL,
	WEAPON_COUNT
} weapon_t;

static const char *const WEAPON_NAMES[WEAPON_COUNT] = { "wrecking ball", "bomb", "water", "bomb ball" };

#define WATER_POUR_CUBIC_METERS_A_SECOND 1000.0f
#define WATER_POUR_RADIUS 3.0f
#define FLOOD_STEP_METERS 0.3f

// ---------------------------------------------------------------------------
// The camera

static void camera_forward(const fly_camera_t *camera, float forward[3])
{
	forward[0] = -SDL_sinf(camera->yaw) * SDL_cosf(camera->pitch);
	forward[1] = SDL_sinf(camera->pitch);
	forward[2] = -SDL_cosf(camera->yaw) * SDL_cosf(camera->pitch);
}

static void camera_look_at(fly_camera_t *camera, const float eye[3], const float target[3])
{
	float direction[3];

	SDL_memcpy(camera->position, eye, sizeof(camera->position));
	wb_vector_subtract(target, eye, direction);
	wb_vector_normalize(direction);
	camera->pitch = SDL_asinf(SDL_clamp(direction[1], -1.0f, 1.0f));
	camera->yaw = SDL_atan2f(-direction[0], -direction[2]);
}

static void camera_to_render(const fly_camera_t *camera, wb_camera_t *render_camera)
{
	float forward[3];

	camera_forward(camera, forward);
	SDL_memcpy(render_camera->eye, camera->position, sizeof(render_camera->eye));
	for (int k = 0; k < 3; k++)
	{
		render_camera->target[k] = camera->position[k] + forward[k];
	}
	render_camera->vertical_field_of_view_degrees = 60.0f;
}

// The direction from the eye through a point of the window (in its own
// coordinates, as the mouse's are)
static void camera_ray_through(const fly_camera_t *camera, SDL_Window *window, float window_x, float window_y, float direction[3])
{
	float forward[3];
	float right[3];
	float up[3];
	const float world_up[3] = { 0.0f, 1.0f, 0.0f };
	int width;
	int height;
	float half_height = SDL_tanf(0.5f * 60.0f * SDL_PI_F / 180.0f);
	float across;
	float upwards;

	SDL_GetWindowSize(window, &width, &height);
	camera_forward(camera, forward);
	wb_vector_cross(forward, world_up, right);
	wb_vector_normalize(right);
	wb_vector_cross(right, forward, up);
	across = (2.0f * window_x / (float)SDL_max(width, 1) - 1.0f) * half_height * (float)width / (float)SDL_max(height, 1);
	upwards = (1.0f - 2.0f * window_y / (float)SDL_max(height, 1)) * half_height;
	for (int k = 0; k < 3; k++)
	{
		direction[k] = forward[k] + right[k] * across + up[k] * upwards;
	}
	wb_vector_normalize(direction);
}

// The middle of the buildings and half their size (a place in the
// middle of the ground when there are none)
static void building_center(const wb_world_t *world, float center[3], float *radius)
{
	float minimum[3];
	float maximum[3];

	*radius = 0.0f;
	if (!wb_world_buildings_bounds(world, minimum, maximum))
	{
		center[0] = 0.0f;
		center[1] = 1.0f;
		center[2] = 0.0f;
		*radius = 5.0f;
		return;
	}
	for (int k = 0; k < 3; k++)
	{
		center[k] = 0.5f * (minimum[k] + maximum[k]);
		*radius = SDL_max(*radius, 0.5f * (maximum[k] - minimum[k]));
	}
}

// Looking at the buildings from a little above, at a distance that shows them whole
static void camera_frame_building(fly_camera_t *camera, const wb_world_t *world)
{
	float center[3];
	float radius;
	float direction[3] = { -0.55f, 0.42f, 0.72f };
	float eye[3];

	building_center(world, center, &radius);
	wb_vector_normalize(direction);
	for (int k = 0; k < 3; k++)
	{
		eye[k] = center[k] + direction[k] * SDL_max(radius, 2.0f) * 2.6f;
	}
	camera_look_at(camera, eye, center);
}

// Turned round a point: the camera moved and turned as one, yaw round the
// vertical and pitch round its own right, so what it looks at stays put
// on the screen
static void camera_orbit(fly_camera_t *camera, const float pivot[3], float yaw_change, float pitch_change)
{
	float offset[3];
	float right[3];
	float forward[3];
	const float up[3] = { 0.0f, 1.0f, 0.0f };
	float new_pitch = SDL_clamp(camera->pitch + pitch_change, -1.5f, 1.5f);
	float cosine;
	float sine;
	float turned[3];

	pitch_change = new_pitch - camera->pitch;
	wb_vector_subtract(camera->position, pivot, offset);
	// Pitch: round the camera's right (level, so the pitch just adds)
	camera_forward(camera, forward);
	wb_vector_cross(forward, up, right);
	wb_vector_normalize(right);
	{
		const float rotation[4] = { right[0] * SDL_sinf(0.5f * pitch_change), right[1] * SDL_sinf(0.5f * pitch_change),
			right[2] * SDL_sinf(0.5f * pitch_change), SDL_cosf(0.5f * pitch_change) };

		wb_quaternion_rotate(rotation, offset, offset);
	}
	// Yaw: round the vertical
	cosine = SDL_cosf(yaw_change);
	sine = SDL_sinf(yaw_change);
	turned[0] = offset[0] * cosine + offset[2] * sine;
	turned[1] = offset[1];
	turned[2] = -offset[0] * sine + offset[2] * cosine;
	for (int k = 0; k < 3; k++)
	{
		camera->position[k] = pivot[k] + turned[k];
	}
	camera->yaw += yaw_change;
	camera->pitch = new_pitch;
}

// ---------------------------------------------------------------------------
// Drawing

// A number made from another, spread over 0..1
static float hash_unit(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return (float)(value & 0xffffff) / (float)0xffffff;
}

static uint32_t scale_color(uint32_t color, float factor)
{
	uint32_t red = (uint32_t)SDL_clamp((float)((color >> 16) & 0xff) * factor, 0.0f, 255.0f);
	uint32_t green = (uint32_t)SDL_clamp((float)((color >> 8) & 0xff) * factor, 0.0f, 255.0f);
	uint32_t blue = (uint32_t)SDL_clamp((float)(color & 0xff) * factor, 0.0f, 255.0f);

	return (red << 16) | (green << 8) | blue;
}

static void draw_world(wb_render_t *render, const wb_world_t *world, const float *aim_point)
{
	static const uint32_t material_colors[WRECK_MATERIAL_COUNT] = { 0x8a5a36, 0xa04a35, 0xa8c8d8 };
	const float no_turn[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float no_place[3] = { 0.0f, 0.0f, 0.0f };
	const float unit_scale[3] = { 1.0f, 1.0f, 1.0f };

	wb_render_begin_frame(render);
	wb_render_add(render, WB_MESH_TERRAIN, no_place, no_turn, unit_scale, 0x7d8768);
	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		const wreck_model_t *model = world->buildings[b].model;

		if (!world->buildings[b].used)
		{
			continue;
		}
		for (int p = 0; p < model->piece_count; p++)
		{
			const wreck_piece_t *piece = &model->pieces[p];
			b3Transform transform;
			float position[3];
			float rotation[4];
			float scale[3];
			uint32_t color;

			if (piece->group == WRECK_GONE || piece->crushed)
			{
				continue;
			}
			transform = wreck_model_piece_transform(model, p);
			position[0] = transform.p.x;
			position[1] = transform.p.y;
			position[2] = transform.p.z;
			rotation[0] = transform.q.v.x;
			rotation[1] = transform.q.v.y;
			rotation[2] = transform.q.v.z;
			rotation[3] = transform.q.s;
			color = scale_color(material_colors[piece->material], 0.88f + 0.24f * hash_unit((uint32_t)(b * WRECK_PIECES_MAX + p) * 2654435761u));
			if (piece->shape == WRECK_PIECE_LOG)
			{
				scale[0] = piece->half_length;
				scale[1] = piece->radius;
				scale[2] = piece->radius;
				wb_render_add(render, WB_MESH_PRISM, position, rotation, scale, color);
			}
			else
			{
				scale[0] = piece->half_extents.x;
				scale[1] = piece->half_extents.y;
				scale[2] = piece->half_extents.z;
				wb_render_add(render, WB_MESH_BOX, position, rotation, scale, color);
			}
		}
	}
	for (int i = 0; i < WB_BALLS_MAX; i++)
	{
		const wb_ball_t *ball = &world->balls[i];

		if (ball->used)
		{
			b3Vec3 at = b3Body_GetPosition(ball->body);
			b3Quat turn = b3Body_GetRotation(ball->body);
			const float position[3] = { at.x, at.y, at.z };
			const float rotation[4] = { turn.v.x, turn.v.y, turn.v.z, turn.s };
			const float scale[3] = { ball->radius, ball->radius, ball->radius };

			wb_render_add(render, WB_MESH_SPHERE, position, rotation, scale, ball->explosive ? 0x9a2418 : 0x45474a);
		}
	}
	// Where a blast would go (the cross, until there is an interface)
	if (aim_point != NULL)
	{
		const float scale[3] = { 0.06f, 0.06f, 0.06f };

		wb_render_add(render, WB_MESH_SPHERE, aim_point, no_turn, scale, 0xe03020);
	}
}

// The world's new blasts to their looks
static void start_blast_effects(wb_world_t *world, wb_effects_t *effects)
{
	float blasts[WB_BLASTS_KEPT][4];
	int count = wb_world_take_blasts(world, blasts);

	for (int i = 0; i < count; i++)
	{
		wb_effects_start(effects, blasts[i], blasts[i][3]);
	}
}

static void update_water(wb_render_t *render, const wb_world_t *world)
{
	const shallow_water_t *water = &world->water;

	wb_render_set_water(render, water->depth, water->floor, world->terrain_heights, water->cells, water->cell_meters, water->origin_x, water->origin_z);
}

static void update_shadow_bounds(wb_render_t *render, const wb_world_t *world)
{
	float center[3];
	float radius;

	building_center(world, center, &radius);
	wb_render_set_shadow_bounds(render, center, SDL_clamp(radius * 1.8f, 10.0f, wb_world_terrain_half_size() * 1.5f));
}

// ---------------------------------------------------------------------------
// Options

static void usage(void)
{
	fprintf(stderr, "wrecking_ball [BUILDING.wreck] [--settings FILE] [--shaders DIR] [--shot FILE.png --frames N --size WxH]\n"
		"              [--camera EX EY EZ TX TY TZ] [--add X Z YAW]... [--throw-at N] [--blast-at N] [--flood-at N] [--pour-at N] [--bomb-at N] [--water-blast-at N] [--trace]\n");
}

static bool read_options(int argument_count, char **arguments, options_t *options)
{
	SDL_zerop(options);
	options->building_path = "../../data/wrecks/house.wreck";
	options->settings_path = "../../data/wreck.ini";
	options->shader_directory = "build/shaders";
	options->frames = 1;
	options->width = 1280;
	options->height = 720;
	options->throw_frame = -1;
	options->blast_frame = -1;
	options->flood_frame = -1;
	options->pour_frame = -1;
	options->bomb_frame = -1;
	options->water_blast_frame = -1;
	for (int i = 1; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		bool has_value = i + 1 < argument_count;

		if (strcmp(argument, "--settings") == 0 && has_value)
		{
			options->settings_path = arguments[++i];
		}
		else if (strcmp(argument, "--shaders") == 0 && has_value)
		{
			options->shader_directory = arguments[++i];
		}
		else if (strcmp(argument, "--shot") == 0 && has_value)
		{
			options->shot_path = arguments[++i];
		}
		else if (strcmp(argument, "--frames") == 0 && has_value)
		{
			int frames = atoi(arguments[++i]);

			options->frames = frames > 1 ? frames : 1;
		}
		else if (strcmp(argument, "--size") == 0 && has_value)
		{
			if (sscanf(arguments[++i], "%dx%d", &options->width, &options->height) != 2 || options->width < 16 || options->height < 16)
			{
				usage();
				return false;
			}
		}
		else if (strcmp(argument, "--camera") == 0 && i + 6 < argument_count)
		{
			for (int k = 0; k < 6; k++)
			{
				options->camera[k] = (float)atof(arguments[++i]);
			}
			options->camera_given = true;
		}
		else if (strcmp(argument, "--add") == 0 && i + 3 < argument_count)
		{
			if (options->extra_building_count < WB_BUILDINGS_MAX)
			{
				for (int k = 0; k < 3; k++)
				{
					options->extra_buildings[options->extra_building_count][k] = (float)atof(arguments[i + 1 + k]);
				}
				options->extra_building_count++;
			}
			i += 3;
		}
		else if (strcmp(argument, "--throw-at") == 0 && has_value)
		{
			options->throw_frame = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--blast-at") == 0 && has_value)
		{
			options->blast_frame = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--water-blast-at") == 0 && has_value)
		{
			options->water_blast_frame = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--bomb-at") == 0 && has_value)
		{
			options->bomb_frame = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--trace") == 0)
		{
			options->trace = true;
		}
		else if (strcmp(argument, "--pour-at") == 0 && has_value)
		{
			options->pour_frame = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--flood-at") == 0 && has_value)
		{
			options->flood_frame = atoi(arguments[++i]);
		}
		else if (argument[0] != '-')
		{
			options->building_path = argument;
		}
		else
		{
			usage();
			return false;
		}
	}
	return true;
}

static void add_extra_buildings(wb_world_t *world, const options_t *options, const char *path)
{
	for (int i = 0; i < options->extra_building_count; i++)
	{
		wb_world_add_building(world, path, options->extra_buildings[i][0], options->extra_buildings[i][1],
			options->extra_buildings[i][2] * SDL_PI_F / 180.0f);
	}
}

// The fastest moving and fastest turning of the loose pieces, and how high
// the highest is: for tuning the water from the command line
static void print_fastest_piece(const wb_world_t *world)
{
	float fastest = 0.0f;
	float fastest_turning = 0.0f;
	float highest = 0.0f;
	int loose = 0;

	for (int b = 0; b < WB_BUILDINGS_MAX; b++)
	{
		const wreck_model_t *model = world->buildings[b].model;

		if (!world->buildings[b].used)
		{
			continue;
		}
		for (int p = 0; p < model->piece_count; p++)
		{
			const wreck_piece_t *piece = &model->pieces[p];
			const wreck_group_t *group;

			if (piece->group == WRECK_GONE || piece->crushed)
			{
				continue;
			}
			group = &model->groups[piece->group];
			if (group->anchored || !b3Body_IsValid(group->body))
			{
				continue;
			}
			loose++;
			fastest = SDL_max(fastest, b3Length(b3Body_GetLinearVelocity(group->body)));
			fastest_turning = SDL_max(fastest_turning, b3Length(b3Body_GetAngularVelocity(group->body)));
			highest = SDL_max(highest, wreck_model_piece_transform(model, p).p.y);
		}
	}
	printf("frame %4d: %3d loose, fastest %5.1f m/s, turning %5.1f rad/s, highest %5.1f m\n", world->frame_index, loose, (double)fastest,
		(double)fastest_turning, (double)highest);
}

// The building's middle, for the scripted ball and blast
static void scripted_events(wb_world_t *world, const options_t *options, const fly_camera_t *camera)
{
	float center[3];
	float radius;

	building_center(world, center, &radius);
	if (world->frame_index == options->throw_frame)
	{
		float direction[3];

		wb_vector_subtract(center, camera->position, direction);
		wb_vector_normalize(direction);
		wb_world_throw_ball(world, camera->position, direction, BALL_SPEED);
	}
	if (world->frame_index == options->bomb_frame)
	{
		float direction[3];

		wb_vector_subtract(center, camera->position, direction);
		wb_vector_normalize(direction);
		wb_world_throw_bomb(world, camera->position, direction, BALL_SPEED, BLAST_KILOGRAMS);
	}
	if (world->frame_index == options->blast_frame)
	{
		wb_world_blast(world, center, BLAST_KILOGRAMS);
	}
	if (options->pour_frame >= 0 && world->frame_index >= options->pour_frame && world->frame_index < options->pour_frame + 2 * WB_WORLD_STEPS_PER_SECOND)
	{
		shallow_water_pour(&world->water, center[0] + radius + 6.0f, center[2], WATER_POUR_RADIUS,
			WATER_POUR_CUBIC_METERS_A_SECOND / (float)WB_WORLD_STEPS_PER_SECOND);
	}
	if (world->frame_index == options->water_blast_frame)
	{
		const float position[3] = { center[0] + radius + 8.0f, 0.5f, center[2] };

		wb_world_blast(world, position, BLAST_KILOGRAMS);
	}
	if (world->frame_index == options->flood_frame)
	{
		shallow_water_fill_to(&world->water, 0.6f);
	}
}

// ---------------------------------------------------------------------------
// Without a window: a picture of a frame

static int run_shot(const options_t *options)
{
	wb_world_t *world = SDL_calloc(1, sizeof(*world));
	static wb_effects_t effects_storage;
	wb_effects_t *effects = &effects_storage;
	wb_render_t *render;
	fly_camera_t camera;
	wb_camera_t render_camera;
	bool saved;

	if (world == NULL || !wb_world_create(world, options->settings_path))
	{
		return 1;
	}
	wb_world_add_building(world, options->building_path, 0.0f, 0.0f, 0.0f);
	add_extra_buildings(world, options, options->building_path);
	render = wb_render_create(NULL, options->width, options->height, options->shader_directory);
	if (render == NULL)
	{
		wb_world_destroy(world);
		SDL_free(world);
		return 1;
	}
	wb_render_set_terrain(render, world->terrain_heights, WB_TERRAIN_CELLS, WB_TERRAIN_CELL_METERS, -wb_world_terrain_half_size(),
		-wb_world_terrain_half_size());
	update_shadow_bounds(render, world);
	SDL_zero(camera);
	if (options->camera_given)
	{
		camera_look_at(&camera, &options->camera[0], &options->camera[3]);
	}
	else
	{
		camera_frame_building(&camera, world);
	}
	for (int frame = 1; frame < options->frames; frame++)
	{
		scripted_events(world, options, &camera);
		wb_world_step(world);
		start_blast_effects(world, effects);
		wb_effects_update(effects, 1.0f / (float)WB_WORLD_STEPS_PER_SECOND);
		if (options->trace && world->frame_index % (WB_WORLD_STEPS_PER_SECOND / 2) == 0)
		{
			print_fastest_piece(world);
		}
	}
	draw_world(render, world, NULL);
	wb_effects_draw(effects, render);
	update_water(render, world);
	camera_to_render(&camera, &render_camera);
	saved = wb_render_end_frame(render, &render_camera, options->shot_path);
	if (saved)
	{
		printf("%s: frame %d, %d links broken\n", options->shot_path, world->frame_index, wb_world_broken_link_count(world));
	}
	wb_render_destroy(render);
	wb_world_destroy(world);
	SDL_free(world);
	return saved ? 0 : 1;
}

// ---------------------------------------------------------------------------
// The window

// The path of one of the buildings next to the first one given
static void numbered_building_path(const char *first_path, int number, char *path, size_t path_size)
{
	const char *slash = strrchr(first_path, '/');
	int directory_length = slash != NULL ? (int)(slash - first_path) + 1 : 0;

	SDL_snprintf(path, path_size, "%.*s%s.wreck", directory_length, first_path, BUILDING_NAMES[number]);
}

static int run_window(const options_t *options)
{
	SDL_Window *window;
	wb_render_t *render;
	wb_world_t *world = SDL_calloc(1, sizeof(*world));
	static wb_effects_t effects_storage;
	wb_effects_t *effects = &effects_storage;
	fly_camera_t camera;
	bool running = true;
	bool looking = false;
	bool paused = false;
	bool slow = false;
	weapon_t weapon = WEAPON_BALL;
	bool orbiting = false;
	bool pouring = false;
	float flood_level = 0.0f;
	float orbit_pivot[3] = { 0.0f, 0.0f, 0.0f };
	char building_path[512];
	uint64_t last_ticks;
	double step_time_left = 0.0;
	uint64_t title_ticks = 0;
	int frames_counted = 0;

	window = SDL_CreateWindow("WreckingBall", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (window == NULL || world == NULL)
	{
		SDL_Log("No window: %s", SDL_GetError());
		return 1;
	}
	render = wb_render_create(window, options->width, options->height, options->shader_directory);
	if (render == NULL || !wb_world_create(world, options->settings_path))
	{
		SDL_DestroyWindow(window);
		return 1;
	}
	SDL_strlcpy(building_path, options->building_path, sizeof(building_path));
	wb_world_add_building(world, building_path, 0.0f, 0.0f, 0.0f);
	add_extra_buildings(world, options, building_path);
	wb_render_set_terrain(render, world->terrain_heights, WB_TERRAIN_CELLS, WB_TERRAIN_CELL_METERS, -wb_world_terrain_half_size(),
		-wb_world_terrain_half_size());
	update_shadow_bounds(render, world);
	SDL_zero(camera);
	if (options->camera_given)
	{
		camera_look_at(&camera, &options->camera[0], &options->camera[3]);
	}
	else
	{
		camera_frame_building(&camera, world);
	}
	last_ticks = SDL_GetTicksNS();
	while (running)
	{
		SDL_Event event;
		uint64_t now_ticks = SDL_GetTicksNS();
		double seconds = (double)(now_ticks - last_ticks) / 1e9;
		const bool *keys = SDL_GetKeyboardState(NULL);
		float forward[3];
		float right[3];
		const float up[3] = { 0.0f, 1.0f, 0.0f };
		float speed;
		float aim_point[3];
		float aim_direction[3];
		float mouse_x;
		float mouse_y;
		bool aiming;
		wb_camera_t render_camera;

		last_ticks = now_ticks;
		seconds = SDL_min(seconds, 0.1);
		camera_forward(&camera, forward);
		// Aimed at the cursor (at the middle while looking round: the
		// cursor is hidden then)
		SDL_GetMouseState(&mouse_x, &mouse_y);
		if (looking || orbiting)
		{
			SDL_memcpy(aim_direction, forward, sizeof(aim_direction));
		}
		else
		{
			camera_ray_through(&camera, window, mouse_x, mouse_y, aim_direction);
		}
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT)
			{
				running = false;
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT)
			{
				looking = true;
				SDL_SetWindowRelativeMouseMode(window, true);
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT)
			{
				looking = false;
				SDL_SetWindowRelativeMouseMode(window, false);
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
			{
				if (!looking)
				{
					camera_ray_through(&camera, window, event.button.x, event.button.y, aim_direction);
				}
				if (weapon == WEAPON_WATER)
				{
					pouring = true;
				}
				else if (weapon == WEAPON_BALL || weapon == WEAPON_BOMB_BALL)
				{
					float hit[3];
					float throw_direction[3];

					// Lifted by what gravity takes off it on the way, so it
					// lands where the cursor points
					SDL_memcpy(throw_direction, aim_direction, sizeof(throw_direction));
					if (wb_world_cast_ray(world, camera.position, aim_direction, 200.0f, hit))
					{
						float offset[3];
						float flight_seconds;

						wb_vector_subtract(hit, camera.position, offset);
						flight_seconds = wb_vector_length(offset) / BALL_SPEED;
						throw_direction[1] += 0.5f * 9.81f * flight_seconds / BALL_SPEED;
					}
					wb_vector_normalize(throw_direction);
					if (weapon == WEAPON_BOMB_BALL)
					{
						wb_world_throw_bomb(world, camera.position, throw_direction, BALL_SPEED, BLAST_KILOGRAMS);
					}
					else
					{
						wb_world_throw_ball(world, camera.position, throw_direction, BALL_SPEED);
					}
				}
				else
				{
					float hit[3];

					if (wb_world_cast_ray(world, camera.position, aim_direction, 200.0f, hit))
					{
						wb_world_blast(world, hit, BLAST_KILOGRAMS);
					}
				}
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT)
			{
				pouring = false;
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_MIDDLE)
			{
				// Turning round what is under the cursor (or a point 10 m
				// along it, over nothing)
				float direction[3];

				camera_ray_through(&camera, window, event.button.x, event.button.y, direction);
				if (!wb_world_cast_ray(world, camera.position, direction, 200.0f, orbit_pivot))
				{
					for (int k = 0; k < 3; k++)
					{
						orbit_pivot[k] = camera.position[k] + direction[k] * 10.0f;
					}
				}
				orbiting = true;
				SDL_SetWindowRelativeMouseMode(window, true);
			}
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_MIDDLE)
			{
				orbiting = false;
				SDL_SetWindowRelativeMouseMode(window, false);
			}
			else if (event.type == SDL_EVENT_MOUSE_MOTION && orbiting)
			{
				camera_orbit(&camera, orbit_pivot, -event.motion.xrel * 0.005f, -event.motion.yrel * 0.005f);
			}
			else if (event.type == SDL_EVENT_MOUSE_WHEEL && !orbiting)
			{
				// Towards the cursor, a part of the way to what it points
				// at (closer, slower), or a meter
				float hit[3];
				float distance = 5.0f;

				if (wb_world_cast_ray(world, camera.position, aim_direction, 200.0f, hit))
				{
					float offset[3];

					wb_vector_subtract(hit, camera.position, offset);
					distance = wb_vector_length(offset);
				}
				for (int k = 0; k < 3; k++)
				{
					camera.position[k] += aim_direction[k] * event.wheel.y * SDL_max(distance * 0.15f, 0.2f);
				}
			}
			else if (event.type == SDL_EVENT_MOUSE_MOTION && looking)
			{
				camera.yaw -= event.motion.xrel * 0.003f;
				camera.pitch -= event.motion.yrel * 0.003f;
				camera.pitch = SDL_clamp(camera.pitch, -1.5f, 1.5f);
			}
			else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
			{
				SDL_Keycode key = event.key.key;

				if (key == SDLK_ESCAPE)
				{
					running = false;
				}
				else if (key == SDLK_SPACE)
				{
					paused = !paused;
				}
				else if (key == SDLK_T)
				{
					slow = !slow;
				}
				else if (key == SDLK_R)
				{
					wb_world_reset_buildings(world);
					wb_effects_clear(effects);
				}
				else if (key == SDLK_H)
				{
					// Another building where the cursor points, turned any way
					float hit[3];

					if (wb_world_cast_ray(world, camera.position, aim_direction, 200.0f, hit))
					{
						float yaw = (float)SDL_rand(3600) * SDL_PI_F / 1800.0f;

						if (wb_world_add_building(world, building_path, hit[0], hit[2], yaw) >= 0)
						{
							update_shadow_bounds(render, world);
						}
					}
				}
				else if (key == SDLK_DELETE || key == SDLK_C)
				{
					wb_world_clear(world);
				}
				else if (key == SDLK_1)
				{
					weapon = WEAPON_BALL;
				}
				else if (key == SDLK_2)
				{
					weapon = WEAPON_BOMB;
				}
				else if (key == SDLK_3)
				{
					weapon = WEAPON_WATER;
				}
				else if (key == SDLK_4)
				{
					weapon = WEAPON_BOMB_BALL;
				}
				else if (key == SDLK_F)
				{
					flood_level += FLOOD_STEP_METERS;
					shallow_water_fill_to(&world->water, flood_level);
				}
				else if (key == SDLK_G)
				{
					flood_level = 0.0f;
					shallow_water_clear(&world->water);
				}
				else if (key >= SDLK_F1 && key <= SDLK_F4)
				{
					// That kind of building, alone in the middle (H then adds more of it)
					numbered_building_path(options->building_path, (int)(key - SDLK_F1), building_path, sizeof(building_path));
					wb_world_clear(world);
					if (wb_world_add_building(world, building_path, 0.0f, 0.0f, 0.0f) >= 0)
					{
						update_shadow_bounds(render, world);
						camera_frame_building(&camera, world);
					}
				}
			}
		}
		// Moving
		camera_forward(&camera, forward);
		wb_vector_cross(forward, up, right);
		wb_vector_normalize(right);
		speed = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 18.0f : 6.0f) * (float)seconds;
		for (int k = 0; k < 3; k++)
		{
			float move = 0.0f;

			move += keys[SDL_SCANCODE_W] ? forward[k] : 0.0f;
			move -= keys[SDL_SCANCODE_S] ? forward[k] : 0.0f;
			move += keys[SDL_SCANCODE_D] ? right[k] : 0.0f;
			move -= keys[SDL_SCANCODE_A] ? right[k] : 0.0f;
			move += keys[SDL_SCANCODE_E] ? up[k] : 0.0f;
			move -= keys[SDL_SCANCODE_Q] ? up[k] : 0.0f;
			camera.position[k] += move * speed;
		}
		// Stepping, at a fixed rate
		if (pouring && weapon == WEAPON_WATER && !paused)
		{
			float hit[3];

			if (wb_world_cast_ray(world, camera.position, aim_direction, 200.0f, hit))
			{
				shallow_water_pour(&world->water, hit[0], hit[2], WATER_POUR_RADIUS,
					WATER_POUR_CUBIC_METERS_A_SECOND * (float)seconds * (slow ? 0.25f : 1.0f));
			}
		}
		if (!paused)
		{
			int steps = 0;

			step_time_left += seconds * (slow ? 0.25 : 1.0);
			while (step_time_left >= 1.0 / WB_WORLD_STEPS_PER_SECOND && steps < 4)
			{
				wb_world_step(world);
				step_time_left -= 1.0 / WB_WORLD_STEPS_PER_SECOND;
				steps++;
			}
			if (steps == 4)
			{
				step_time_left = 0.0;
			}
			start_blast_effects(world, effects);
			wb_effects_update(effects, (float)seconds * (slow ? 0.25f : 1.0f));
		}
		aiming = wb_world_cast_ray(world, camera.position, aim_direction, 200.0f, aim_point);
		draw_world(render, world, aiming ? aim_point : NULL);
		wb_effects_draw(effects, render);
		if (!paused || pouring)
		{
			update_water(render, world);
		}
		camera_to_render(&camera, &render_camera);
		wb_render_end_frame(render, &render_camera, NULL);
		// The title says how it goes (until there is an interface)
		frames_counted++;
		if (now_ticks - title_ticks >= 500000000ull)
		{
			char title[256];
			const char *name = strrchr(building_path, '/');
			double fps = (double)frames_counted * 1e9 / (double)(now_ticks - title_ticks);

			SDL_snprintf(title, sizeof(title), "WreckingBall - %s - %s x%d: %d pieces, %d links broken - water %.0f m3 - %.0f fps%s%s", WEAPON_NAMES[weapon],
				name != NULL ? name + 1 : building_path, wb_world_building_count(world), wb_world_piece_count(world), wb_world_broken_link_count(world), world->water.total_volume, fps,
				paused ? " - paused" : "", slow ? " - slow" : "");
			SDL_SetWindowTitle(window, title);
			title_ticks = now_ticks;
			frames_counted = 0;
		}
	}
	wb_render_destroy(render);
	wb_world_destroy(world);
	SDL_free(world);
	SDL_DestroyWindow(window);
	return 0;
}

int main(int argument_count, char **arguments)
{
	options_t options;
	int result;

	if (!read_options(argument_count, arguments, &options))
	{
		return 2;
	}
	// (no window for a picture: the GPU still wants the video subsystem)
	if (options.shot_path != NULL)
	{
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
	}
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "wrecking_ball: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	result = options.shot_path != NULL ? run_shot(&options) : run_window(&options);
	SDL_Quit();
	return result;
}
