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
//   --throw-at N        throw a ball from the camera at the building at frame N
//   --blast-at N        a blast in the building's middle at frame N
//
// In the window:
//   right mouse button held: look round; W A S D, Q E: move (shift: faster)
//   1: the wrecking ball, 2: the bomb; left click: the weapon at the
//   cursor (the ball thrown towards it, the bomb set off where it points)
//   R: the building again; F1 to F4: house, cabin, support, bend
//   space: pause; T: slow motion; Esc: quit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

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
} options_t;

static const char *const BUILDING_NAMES[4] = { "house", "cabin", "support", "bend" };

typedef enum
{
	WEAPON_BALL,
	WEAPON_BOMB,
	WEAPON_COUNT
} weapon_t;

static const char *const WEAPON_NAMES[WEAPON_COUNT] = { "wrecking ball", "bomb" };

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

// Looking at the building from a little above, at a distance that shows it whole
static void camera_frame_building(fly_camera_t *camera, const wb_world_t *world)
{
	float center[3];
	float radius = 0.0f;
	float direction[3] = { -0.55f, 0.42f, 0.72f };
	float eye[3];

	for (int k = 0; k < 3; k++)
	{
		center[k] = 0.5f * (world->building_minimum[k] + world->building_maximum[k]);
		radius = SDL_max(radius, 0.5f * (world->building_maximum[k] - world->building_minimum[k]));
	}
	if (!world->building_loaded)
	{
		center[0] = 0.0f;
		center[1] = 1.0f;
		center[2] = 0.0f;
		radius = 5.0f;
	}
	wb_vector_normalize(direction);
	for (int k = 0; k < 3; k++)
	{
		eye[k] = center[k] + direction[k] * SDL_max(radius, 2.0f) * 2.6f;
	}
	camera_look_at(camera, eye, center);
}

static void building_center(const wb_world_t *world, float center[3], float *radius)
{
	*radius = 0.0f;
	for (int k = 0; k < 3; k++)
	{
		center[k] = 0.5f * (world->building_minimum[k] + world->building_maximum[k]);
		*radius = SDL_max(*radius, 0.5f * (world->building_maximum[k] - world->building_minimum[k]));
	}
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
	if (world->model != NULL && world->building_loaded)
	{
		const wreck_model_t *model = world->model;

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
			color = scale_color(material_colors[piece->material], 0.88f + 0.24f * hash_unit((uint32_t)p * 2654435761u));
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

			wb_render_add(render, WB_MESH_SPHERE, position, rotation, scale, 0x45474a);
		}
	}
	// Where a blast would go (the cross, until there is an interface)
	if (aim_point != NULL)
	{
		const float scale[3] = { 0.06f, 0.06f, 0.06f };

		wb_render_add(render, WB_MESH_SPHERE, aim_point, no_turn, scale, 0xe03020);
	}
}

static void update_shadow_bounds(wb_render_t *render, const wb_world_t *world)
{
	float center[3];
	float radius;

	building_center(world, center, &radius);
	if (!world->building_loaded)
	{
		center[0] = center[1] = center[2] = 0.0f;
		radius = 8.0f;
	}
	wb_render_set_shadow_bounds(render, center, SDL_max(radius * 1.8f, 10.0f));
}

// ---------------------------------------------------------------------------
// Options

static void usage(void)
{
	fprintf(stderr, "wrecking_ball [BUILDING.wreck] [--settings FILE] [--shaders DIR] [--shot FILE.png --frames N --size WxH]\n"
		"              [--camera EX EY EZ TX TY TZ] [--throw-at N] [--blast-at N]\n");
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
		else if (strcmp(argument, "--throw-at") == 0 && has_value)
		{
			options->throw_frame = atoi(arguments[++i]);
		}
		else if (strcmp(argument, "--blast-at") == 0 && has_value)
		{
			options->blast_frame = atoi(arguments[++i]);
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
	if (world->frame_index == options->blast_frame)
	{
		wb_world_blast(world, center, BLAST_KILOGRAMS);
	}
}

// ---------------------------------------------------------------------------
// Without a window: a picture of a frame

static int run_shot(const options_t *options)
{
	wb_world_t *world = SDL_calloc(1, sizeof(*world));
	wb_render_t *render;
	fly_camera_t camera;
	wb_camera_t render_camera;
	bool saved;

	if (world == NULL || !wb_world_create(world, options->settings_path))
	{
		return 1;
	}
	wb_world_load_building(world, options->building_path);
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
	}
	draw_world(render, world, NULL);
	camera_to_render(&camera, &render_camera);
	saved = wb_render_end_frame(render, &render_camera, options->shot_path);
	if (saved && world->model != NULL)
	{
		printf("%s: frame %d, %d links broken\n", options->shot_path, world->frame_index, world->model->broken_link_count);
	}
	wb_render_destroy(render);
	wb_world_destroy(world);
	SDL_free(world);
	return saved ? 0 : 1;
}

// ---------------------------------------------------------------------------
// The window

static void load_numbered_building(wb_world_t *world, wb_render_t *render, fly_camera_t *camera, const char *first_path, int number)
{
	char path[512];
	const char *slash = strrchr(first_path, '/');
	int directory_length = slash != NULL ? (int)(slash - first_path) + 1 : 0;

	SDL_snprintf(path, sizeof(path), "%.*s%s.wreck", directory_length, first_path, BUILDING_NAMES[number]);
	if (wb_world_load_building(world, path))
	{
		update_shadow_bounds(render, world);
		camera_frame_building(camera, world);
	}
}

static int run_window(const options_t *options)
{
	SDL_Window *window;
	wb_render_t *render;
	wb_world_t *world = SDL_calloc(1, sizeof(*world));
	fly_camera_t camera;
	bool running = true;
	bool looking = false;
	bool paused = false;
	bool slow = false;
	weapon_t weapon = WEAPON_BALL;
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
	wb_world_load_building(world, options->building_path);
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
		if (looking)
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
				if (weapon == WEAPON_BALL)
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
					wb_world_throw_ball(world, camera.position, throw_direction, BALL_SPEED);
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
					wb_world_reset_building(world);
				}
				else if (key == SDLK_1)
				{
					weapon = WEAPON_BALL;
				}
				else if (key == SDLK_2)
				{
					weapon = WEAPON_BOMB;
				}
				else if (key >= SDLK_F1 && key <= SDLK_F4)
				{
					load_numbered_building(world, render, &camera, options->building_path, (int)(key - SDLK_F1));
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
		}
		aiming = wb_world_cast_ray(world, camera.position, aim_direction, 200.0f, aim_point);
		draw_world(render, world, aiming ? aim_point : NULL);
		camera_to_render(&camera, &render_camera);
		wb_render_end_frame(render, &render_camera, NULL);
		// The title says how it goes (until there is an interface)
		frames_counted++;
		if (now_ticks - title_ticks >= 500000000ull)
		{
			char title[256];
			const char *name = strrchr(world->building_path, '/');
			double fps = (double)frames_counted * 1e9 / (double)(now_ticks - title_ticks);

			SDL_snprintf(title, sizeof(title), "WreckingBall - %s - %s: %d pieces, %d links broken - %.0f fps%s%s", WEAPON_NAMES[weapon],
				name != NULL ? name + 1 : world->building_path,
				world->model != NULL ? world->model->piece_count : 0, world->model != NULL ? world->model->broken_link_count : 0, fps,
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
